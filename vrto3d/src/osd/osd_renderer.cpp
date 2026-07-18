/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VRto3D is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with VRto3D. If not, see <http://www.gnu.org/licenses/>.
 */
#include "osd/osd_renderer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <system_error>

#include <d3dcompiler.h>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

#include "osd/osd_input.h"
#include "osd/osd_menu.h"

#include "hmd_device_driver.h"
#include "vrto3dlib/stereo_config.h"
#include "vrto3dlib/win32_helper.hpp"  // GetSteamInstallPath

#ifndef VRTO3D_LOG
#  include <iostream>
#  define VRTO3D_LOG() (std::cerr)
#endif

using Microsoft::WRL::ComPtr;
using namespace std::chrono;

namespace vrto3d::osd {

namespace {

// Geometric-mean font scale based on the per-eye OSD area. Treats SbS and
// TaB layouts identically at the same source resolution (both have the same
// per-eye area), so users get consistent text size regardless of output
// mode. Baseline area = 720p SbS half (640x720 = 460800 px) so 1080p SbS
// still resolves to 1.5x, matching the previous eye_h/720 formula on that
// case while bumping TaB up from its old 0.75x floor.
float ComputeFontScale(UINT eye_w, UINT eye_h) {
    constexpr float baseline_area = 640.0f * 720.0f;
    const float area = static_cast<float>(eye_w) * static_cast<float>(eye_h);
    if (area <= 0.0f) return 1.0f;
    return std::clamp(std::sqrt(area / baseline_area), 0.75f, 3.0f);
}

// Full-screen triangle VS — emits a single triangle that covers the viewport
// using vertex IDs 0/1/2. UVs are derived so that (0,0) lands at top-left.
const char* kCompositeVs = R"hlsl(
struct VsOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VsOut main(uint id : SV_VertexID) {
    VsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)hlsl";

// Composite PS — samples the offscreen OSD texture and outputs straight RGBA.
// Blend state handles SrcAlpha/InvSrcAlpha so transparent UI regions pass
// through the underlying SbS pixels unchanged.
const char* kCompositePs = R"hlsl(
Texture2D    osd_tex : register(t0);
SamplerState osd_smp : register(s0);

struct VsOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 main(VsOut i) : SV_Target {
    return osd_tex.Sample(osd_smp, i.uv);
}
)hlsl";

// Stereo-cursor PS — procedural navigation arrow (tip/hotspot at hotspot_uv,
// body extending down-right), drawn once per eye half with a per-eye
// horizontal shift baked into hotspot_uv by the caller. Ported from
// UEVR-3D's Flat3D stereo cursor: solid silhouette + dark outline identical
// in both eyes (stable fusion at the chosen depth), two-facet shading with a
// crease highlight so it reads as a folded 3D arrow. Output is
// premultiplied alpha — pair with the ONE/INV_SRC_ALPHA blend state.
const char* kCursorPs = R"hlsl(
cbuffer CursorCb : register(b0) {
    float2 hotspot_uv;   // arrow tip in this eye's UV (depth shift included)
    float2 eye_px;       // per-eye dimensions in pixels
    float  size_px;      // arrow height in pixels
    float3 _pad;
};

struct VsOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float cross2(float2 a, float2 b) { return a.x * b.y - a.y * b.x; }
bool in_tri(float2 p, float2 a, float2 b, float2 c) {
    float c1 = cross2(b - a, p - a);
    float c2 = cross2(c - b, p - b);
    float c3 = cross2(a - c, p - c);
    return (c1 <= 0.0 && c2 <= 0.0 && c3 <= 0.0) || (c1 >= 0.0 && c2 >= 0.0 && c3 >= 0.0);
}

// Arrow points UP-LEFT like a normal cursor, split by a diagonal centre
// crease (the arrow axis). kAxis = tip->back, kPerp = across the crease.
static const float2 kAxis     = float2(0.70711, 0.70711);
static const float2 kPerp     = float2(-0.70711, 0.70711);
static const float2 kNavTip   = float2(0.0, 0.0);
static const float2 kNavNotch = float2(0.495, 0.495);
static const float2 kNavWingL = float2(0.375, 0.969);
static const float2 kNavWingR = float2(0.969, 0.375);
bool nav_any(float2 p) {
    return in_tri(p, kNavTip, kNavWingL, kNavNotch) || in_tri(p, kNavTip, kNavNotch, kNavWingR);
}

float4 main(VsOut input) : SV_Target {
    float2 d = input.uv - hotspot_uv;
    float inv_size = 1.0 / max(size_px, 1.0);
    float pxu = inv_size; // one output pixel in arrow units
    float2 p = float2(d.x * eye_px.x, d.y * max(eye_px.y, 1.0)) * inv_size;

    // Solid silhouette + outline, 4x rotated-grid supersample for smooth edges.
    const float2 J[4] = { float2(0.125, 0.375), float2(0.375, -0.125),
                          float2(-0.125, -0.375), float2(-0.375, 0.125) };
    float fill = 0.0, edge = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        float2 s = p + J[i] * (pxu * 2.0);
        if (nav_any(s)) fill += 0.25;
        if (nav_any(s * (1.0 / 1.32))) edge += 0.25; // enlarged silhouette = fill+outline
    }
    float outline = saturate(edge - fill);

    // Two-facet shading + a crease highlight — a folded LOOK, drawn
    // identically in both eyes (the 3D placement comes from hotspot_uv).
    float side  = dot(p, kPerp);   // signed distance across the crease
    float axial = dot(p, kAxis);   // 0 at tip -> ~0.95 at wings
    float crease = saturate(1.0 - abs(side) / 0.42)
                 * smoothstep(0.0, 0.18, axial) * (1.0 - smoothstep(0.62, 0.95, axial));

    float t  = saturate(abs(side) * 2.4);  // 0 at crease -> 1 at wing
    float vv = saturate(axial / 0.95);     // 0 at tip -> 1 at base
    float shade = ((side > 0.0) ? 0.98 : 0.80) - 0.10 * t - 0.12 * vv;
    shade = saturate(shade + smoothstep(0.06, 0.0, abs(side)) * crease * 0.12);

    // Premultiplied over: outline (behind) then the facet fill.
    float3 col = float3(0.03, 0.03, 0.03) * outline;
    float a = outline;
    col = float3(shade, shade, shade) * fill + col * (1.0 - fill);
    a = fill + a * (1.0 - fill);
    return float4(col, a);
}
)hlsl";

bool CompileShader(const char* src, const char* entry, const char* profile,
                   ComPtr<ID3DBlob>& out_blob) {
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
                            entry, profile, 0, 0, &out_blob, &err);
    if (FAILED(hr)) {
        VRTO3D_LOG() << "OsdRenderer: shader compile (" << profile << ") failed: "
                     << (err ? static_cast<const char*>(err->GetBufferPointer()) : "(no log)")
                     << std::endl;
        return false;
    }
    return true;
}

} // namespace

struct OsdRenderer::Impl {
    // D3D11 handles (non-owning device/context — owned by Dx11Renderer).
    ID3D11Device*        device  = nullptr;
    ID3D11DeviceContext* context = nullptr;

    // Offscreen OSD render target (per-eye sized).
    UINT                              eye_w = 0;
    UINT                              eye_h = 0;
    ComPtr<ID3D11Texture2D>           osd_tex;
    ComPtr<ID3D11RenderTargetView>    osd_rtv;
    ComPtr<ID3D11ShaderResourceView>  osd_srv;

    // Composite pipeline state — used to blend osd_tex into out_sbs.
    ComPtr<ID3D11VertexShader>        vs;
    ComPtr<ID3D11PixelShader>         ps;
    ComPtr<ID3D11BlendState>          blend;
    ComPtr<ID3D11RasterizerState>     raster;
    ComPtr<ID3D11SamplerState>        sampler;

    // Stereo-cursor pipeline (procedural arrow, premultiplied-alpha blend).
    ComPtr<ID3D11PixelShader>         cursor_ps;
    ComPtr<ID3D11Buffer>              cursor_cb;
    ComPtr<ID3D11BlendState>          blend_premul;

    // Stereo-cursor state pushed by the driver's CursorControlThread (any
    // thread) and consumed by RenderFrame (window thread).
    std::atomic<bool>                 cursor_active{false};
    std::atomic<float>                cursor_depth_px{0.0f};
    std::atomic<float>                cursor_size_px{32.0f};
    std::atomic<void*>                cursor_game_hwnd{nullptr};

    // ImGui state.
    ImGuiContext*                     imgui_ctx = nullptr;
    bool                              imgui_dx11_ready = false;
    // Backing storage for io.IniFilename — ImGui only holds the const char*,
    // so the std::string must outlive the ImGui context.
    std::string                       ini_path;

    // Input pump + menu (created in Init).
    std::unique_ptr<OsdInput>         input;
    std::unique_ptr<OsdMenu>          menu;

    // Toast text (replacement for old GDI+ overlay).
    std::mutex                        toast_mu;
    std::string                       toast_msg;
    steady_clock::time_point          toast_expiry;

    // Frame timing for ImGui's IO.DeltaTime.
    steady_clock::time_point          last_frame_time = steady_clock::now();

    // For mapping mouse coords into client space.
    void*                             headset_hwnd = nullptr;
    StereoDisplayComponent*           component = nullptr;

    // OutputMode active at the time the presenter (and thus this OsdRenderer)
    // was constructed. The System-tab combo can mutate the live config's
    // output_mode mid-session, but the real presenter pipeline doesn't switch
    // until the driver restarts — so the cursor-fold layout has to stay
    // pinned to whatever mode actually owns the SBS frame we're compositing
    // into. Refreshed only on Shutdown/re-Init.
    OutputMode                        active_output_mode = OutputMode::SbS;

    // WS_EX_LAYERED|WS_EX_TRANSPARENT toggling. The headset window is normally
    // click-through (so the user can interact with apps behind it). When the
    // menu opens we strip those bits so clicks land on the OSD; on close we
    // restore whatever was there before.
    bool                              prev_menu_visible = false;
    bool                              styles_overridden = false;
    LONG_PTR                          saved_ex_style    = 0;
    HWND                              styled_hwnd       = nullptr;
    std::function<void()>             on_menu_closed;

    HWND DiscoverHwnd() {
        if (headset_hwnd && IsWindow(static_cast<HWND>(headset_hwnd)))
            return static_cast<HWND>(headset_hwnd);
        if (cached_hwnd && IsWindow(cached_hwnd))
            return cached_hwnd;
        cached_hwnd = FindWindowW(L"VRto3D_PresentWindow", nullptr);
        return cached_hwnd;
    }
    HWND cached_hwnd = nullptr;

    void ApplyMenuVisibility(bool now_visible) {
        if (now_visible == prev_menu_visible) return;
        // NvidiaDX9: NV3D-Lib owns the FSE popup and its click-through state,
        // and toggles it via InterfaceDX11::SetInteractive (driven by the
        // presenter from this same menu-visibility signal). Manipulating
        // WS_EX_TRANSPARENT here would be a cross-thread style change on the
        // FSE D3D9Ex window (DWM-wedge risk) and wouldn't even stop click
        // pass-through, since the library's WndProc HTTRANSPARENT path stays
        // active. So leave the window alone in this mode.
        if (active_output_mode == OutputMode::NvidiaDX9) {
            prev_menu_visible = now_visible;
            return;
        }
        HWND hwnd = DiscoverHwnd();
        if (!hwnd) { prev_menu_visible = now_visible; return; }
        // Only toggle WS_EX_TRANSPARENT; WS_EX_LAYERED stays set for the
        // window's lifetime to avoid DWM rebuilding the layered surface.
        // SWP_FRAMECHANGED is intentionally omitted — it can trigger
        // WM_DPICHANGED, which would rescale the window on non-100% displays.
        if (now_visible && !styles_overridden) {
            saved_ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            if (saved_ex_style & WS_EX_TRANSPARENT) {
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, saved_ex_style & ~WS_EX_TRANSPARENT);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                             SWP_NOACTIVATE);
            }
            styles_overridden = true;
            styled_hwnd = hwnd;
            // Steal foreground so gamepad / keyboard / mouse routed by the
            // OS go to our window instead of the game underneath. Skip the
            // NvidiaDX9 presenter — it holds the D3D9Ex device in FSE on
            // this same HWND, and a foreground change there drops the
            // exclusive cooperative level (and risks a TDR). ForceFocus
            // does ~100ms of sleeps + AttachThreadInput, so dispatch to a
            // detached thread so we don't block presentation.
            if (active_output_mode != OutputMode::NvidiaDX9) {
                std::thread([hwnd]() {
                    ForceFocus(hwnd,
                               GetCurrentThreadId(),
                               GetWindowThreadProcessId(hwnd, nullptr));
                }).detach();
            }
        } else if (!now_visible && styles_overridden) {
            HWND target = styled_hwnd && IsWindow(styled_hwnd) ? styled_hwnd : hwnd;
            if (target) {
                SetWindowLongPtrW(target, GWL_EXSTYLE, saved_ex_style);
                SetWindowPos(target, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                             SWP_NOACTIVATE);
            }
            styles_overridden = false;
            styled_hwnd = nullptr;
            // Menu just closed — hand input focus back to the game.
            if (on_menu_closed) on_menu_closed();
        }
        prev_menu_visible = now_visible;
    }

    bool CreateOffscreen(UINT w, UINT h) {
        if (osd_tex && eye_w == w && eye_h == h) return true;
        osd_tex.Reset();
        osd_rtv.Reset();
        osd_srv.Reset();

        D3D11_TEXTURE2D_DESC d{};
        d.Width              = w;
        d.Height             = h;
        d.MipLevels          = 1;
        d.ArraySize          = 1;
        d.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count   = 1;
        d.Usage              = D3D11_USAGE_DEFAULT;
        d.BindFlags          = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &osd_tex))) return false;
        if (FAILED(device->CreateRenderTargetView(osd_tex.Get(), nullptr, &osd_rtv))) return false;
        if (FAILED(device->CreateShaderResourceView(osd_tex.Get(), nullptr, &osd_srv))) return false;
        eye_w = w;
        eye_h = h;
        return true;
    }

    bool CreatePipelineState() {
        ComPtr<ID3DBlob> vs_blob, ps_blob;
        if (!CompileShader(kCompositeVs, "main", "vs_5_0", vs_blob)) return false;
        if (!CompileShader(kCompositePs, "main", "ps_5_0", ps_blob)) return false;
        if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                              nullptr, &vs))) return false;
        if (FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                                             nullptr, &ps))) return false;

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable    = TRUE;
        bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&bd, &blend))) return false;

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode        = D3D11_FILL_SOLID;
        rd.CullMode        = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        if (FAILED(device->CreateRasterizerState(&rd, &raster))) return false;

        D3D11_SAMPLER_DESC sd{};
        sd.Filter        = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU      = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV      = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW      = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD        = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sd, &sampler))) return false;

        // Stereo-cursor pipeline. Failure is non-fatal — the OSD works
        // without it; the cursor pass just stays disabled.
        ComPtr<ID3DBlob> cursor_blob;
        if (CompileShader(kCursorPs, "main", "ps_5_0", cursor_blob)) {
            device->CreatePixelShader(cursor_blob->GetBufferPointer(),
                                      cursor_blob->GetBufferSize(),
                                      nullptr, &cursor_ps);
        }
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth      = 32;   // float2 + float2 + float + float3 pad
        cbd.Usage          = D3D11_USAGE_DEFAULT;
        cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        device->CreateBuffer(&cbd, nullptr, &cursor_cb);

        D3D11_BLEND_DESC pbd{};
        pbd.RenderTarget[0].BlendEnable    = TRUE;
        pbd.RenderTarget[0].SrcBlend       = D3D11_BLEND_ONE;   // premultiplied
        pbd.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
        pbd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
        pbd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
        pbd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        pbd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
        pbd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&pbd, &blend_premul);
        return true;
    }

    // Layout of the presenter's window surface — governs how the physical
    // cursor position folds into per-eye coordinates. Must match the switch
    // in RenderFrame's input block (both are pinned to active_output_mode).
    StereoLayout SurfaceLayout() const {
        switch (active_output_mode) {
            case OutputMode::SbS:
            case OutputMode::VirtualDesktop:
            case OutputMode::DualDisplay:
            case OutputMode::DualDisplayFlip:
                return StereoLayout::HorizontalSbs;
            case OutputMode::TaB:
            case OutputMode::FramePacked720p60:
            case OutputMode::FramePacked1080p24:
            case OutputMode::FramePacked1080p60:
            case OutputMode::FramePacked1080p60CVT:
                return StereoLayout::VerticalTab;
            default:
                return StereoLayout::Mono;
        }
    }

    // Map the OS cursor position into per-eye UV. Preferred path: normalize
    // against the GAME window's client rect — games place their software
    // cursor / UI hit-testing from their own client coords, and their frame
    // fills the whole per-eye image, so this tracks the in-game cursor 1:1
    // regardless of where/how large the game window is. Fallback (no game
    // window): the presenter window with the same eye-fold as
    // OsdInput::FeedImGui. Returns false when the cursor is outside (no
    // arrow drawn).
    bool MapCursorToEyeUV(float& out_u, float& out_v) {
        POINT p;
        if (!GetCursorPos(&p)) return false;

        HWND game = static_cast<HWND>(cursor_game_hwnd.load(std::memory_order_relaxed));
        if (game && IsWindow(game)) {
            RECT rc{};
            POINT cp = p;
            if (GetClientRect(game, &rc) && rc.right > 0 && rc.bottom > 0
                && ScreenToClient(game, &cp)) {
                const float u = static_cast<float>(cp.x) / static_cast<float>(rc.right);
                const float v = static_cast<float>(cp.y) / static_cast<float>(rc.bottom);
                if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return false;
                out_u = u;
                out_v = v;
                return true;
            }
        }

        HWND hwnd = DiscoverHwnd();
        if (!hwnd) return false;
        RECT client{};
        if (!GetClientRect(hwnd, &client)) return false;
        ScreenToClient(hwnd, &p);
        const int cw = client.right  - client.left;
        const int ch = client.bottom - client.top;
        if (cw <= 0 || ch <= 0) return false;
        float u = static_cast<float>(p.x) / static_cast<float>(cw);
        float v = static_cast<float>(p.y) / static_cast<float>(ch);
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return false;
        switch (SurfaceLayout()) {
            case StereoLayout::HorizontalSbs:
                u = (u >= 0.5f) ? (u - 0.5f) * 2.0f : u * 2.0f;
                break;
            case StereoLayout::VerticalTab:
                v = (v >= 0.5f) ? (v - 0.5f) * 2.0f : v * 2.0f;
                break;
            default:
                break;
        }
        out_u = u;
        out_v = v;
        return true;
    }

    // Draw the per-eye arrow into both halves of out_sbs at (u, v), with the
    // depth shift applied symmetrically (+depth = into the screen). Sign was
    // set empirically against VRto3D's eye layout — left half shifts right,
    // right half shifts left for uncrossed (behind-screen) disparity.
    void DrawCursorPass(ID3D11Texture2D* out_sbs, float u, float v) {
        if (!cursor_ps || !cursor_cb || !blend_premul) return;

        ComPtr<ID3D11RenderTargetView> sbs_rtv;
        if (FAILED(device->CreateRenderTargetView(out_sbs, nullptr, &sbs_rtv))) return;

        D3D11_TEXTURE2D_DESC sbs_desc{};
        out_sbs->GetDesc(&sbs_desc);
        const UINT half_w = sbs_desc.Width / 2;
        const UINT full_h = sbs_desc.Height;
        if (half_w == 0 || full_h == 0) return;

        ID3D11RenderTargetView* rtv = sbs_rtv.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);

        const float blend_factor[4] = { 1, 1, 1, 1 };
        context->OMSetBlendState(blend_premul.Get(), blend_factor, 0xFFFFFFFF);
        context->RSSetState(raster.Get());
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(cursor_ps.Get(), nullptr, 0);
        ID3D11Buffer* cbs[] = { cursor_cb.Get() };
        context->PSSetConstantBuffers(0, 1, cbs);

        const float depth_px = cursor_depth_px.load(std::memory_order_relaxed);
        const float size_px  = cursor_size_px.load(std::memory_order_relaxed);

        D3D11_VIEWPORT vp{};
        vp.Width    = static_cast<float>(half_w);
        vp.Height   = static_cast<float>(full_h);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;

        struct CursorCb {
            float hotspot_uv[2];
            float eye_px[2];
            float size_px;
            float pad[3];
        } cb{};
        cb.eye_px[0] = static_cast<float>(half_w);
        cb.eye_px[1] = static_cast<float>(full_h);
        cb.size_px   = (size_px > 1.0f) ? size_px : 1.0f;

        for (int eye = 0; eye < 2; ++eye) {
            const float dir = (eye == 0) ? 1.0f : -1.0f;
            cb.hotspot_uv[0] = u + dir * depth_px / static_cast<float>(half_w);
            cb.hotspot_uv[1] = v;
            context->UpdateSubresource(cursor_cb.Get(), 0, nullptr, &cb, 0, 0);

            vp.TopLeftX = static_cast<float>(eye) * static_cast<float>(half_w);
            vp.TopLeftY = 0.0f;
            context->RSSetViewports(1, &vp);
            context->Draw(3, 0);
        }

        ID3D11RenderTargetView* null_rtv = nullptr;
        context->OMSetRenderTargets(1, &null_rtv, nullptr);
    }
};

OsdRenderer::OsdRenderer()  : impl_(std::make_unique<Impl>()) {}
OsdRenderer::~OsdRenderer() { Shutdown(); }

bool OsdRenderer::Init(ID3D11Device* device,
                       ID3D11DeviceContext* context,
                       UINT eye_w, UINT eye_h,
                       void* headset_hwnd,
                       StereoDisplayComponent* component,
                       MenuCallbacks callbacks) {
    auto& s = *impl_;
    s.device  = device;
    s.context = context;
    s.headset_hwnd = headset_hwnd;
    s.component = component;

    if (!s.CreateOffscreen(eye_w, eye_h)) {
        VRTO3D_LOG() << "OsdRenderer: failed to create offscreen RT" << std::endl;
        return false;
    }
    if (!s.CreatePipelineState()) {
        VRTO3D_LOG() << "OsdRenderer: failed to create composite pipeline" << std::endl;
        return false;
    }

    IMGUI_CHECKVERSION();
    s.imgui_ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Persist window pos/size, table column widths, etc. to the VRto3D
    // config folder so the user's layout survives across sessions. The ini
    // path string is held on the Impl so the pointer stays valid for the
    // lifetime of the ImGui context. If Steam isn't installed (CI / debug
    // shells) we fall through with IniFilename = nullptr — ImGui then runs
    // ephemerally instead of writing to an unknown location.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    if (const std::string steam = GetSteamInstallPath(); !steam.empty()) {
        const std::string folder = steam + "\\config\\vrto3d";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        s.ini_path = folder + "\\imgui_osd.ini";
        io.IniFilename = s.ini_path.c_str();
    }
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    // Scale fonts to the per-eye area so SbS and TaB read the same at the
    // same source resolution; the formula clamps below 1.0 for very small
    // surfaces and above 1.0 for high-res so the menu stays legible.
    io.FontGlobalScale = ComputeFontScale(eye_w, eye_h);
    io.DisplaySize = ImVec2(static_cast<float>(eye_w), static_cast<float>(eye_h));

    if (!ImGui_ImplDX11_Init(device, context)) {
        VRTO3D_LOG() << "OsdRenderer: ImGui_ImplDX11_Init failed" << std::endl;
        ImGui::DestroyContext(s.imgui_ctx);
        s.imgui_ctx = nullptr;
        return false;
    }
    s.imgui_dx11_ready = true;

    s.input = std::make_unique<OsdInput>();
    s.on_menu_closed = callbacks.request_game_focus;
    s.menu  = std::make_unique<OsdMenu>(component, std::move(callbacks));

    // Pin the active output mode to whatever the presenter was constructed
    // for. The System-tab combo can mutate the live config mid-session, but
    // the cursor-mapping fold has to keep matching the actual SBS layout
    // until the driver restarts.
    if (component) {
        s.active_output_mode = component->GetConfig().output_mode;
    }

    return true;
}

void OsdRenderer::OnResize(UINT eye_w, UINT eye_h) {
    auto& s = *impl_;
    if (!s.device) return;
    if (s.eye_w == eye_w && s.eye_h == eye_h) return;
    s.CreateOffscreen(eye_w, eye_h);
    if (s.imgui_ctx) {
        ImGui::SetCurrentContext(s.imgui_ctx);
        ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(eye_w), static_cast<float>(eye_h));
        ImGui::GetIO().FontGlobalScale = ComputeFontScale(eye_w, eye_h);
    }
}

void OsdRenderer::SetHeadsetHwnd(void* hwnd) {
    impl_->headset_hwnd = hwnd;
    impl_->cached_hwnd  = nullptr;  // force re-discovery on next access
}

void OsdRenderer::Shutdown() {
    auto& s = *impl_;
    s.ApplyMenuVisibility(false);  // restore window styles before tearing down
    s.menu.reset();
    s.input.reset();
    if (s.imgui_dx11_ready) {
        ImGui_ImplDX11_Shutdown();
        s.imgui_dx11_ready = false;
    }
    if (s.imgui_ctx) {
        // Force-flush any pending layout changes — ImGui's auto-save is
        // throttled by IniSavingRate (5s by default) so a quick edit-then-
        // exit could otherwise lose the user's window pos/size.
        if (!s.ini_path.empty()) {
            ImGui::SaveIniSettingsToDisk(s.ini_path.c_str());
        }
        ImGui::DestroyContext(s.imgui_ctx);
        s.imgui_ctx = nullptr;
    }
    s.osd_rtv.Reset();
    s.osd_srv.Reset();
    s.osd_tex.Reset();
    s.vs.Reset();
    s.ps.Reset();
    s.blend.Reset();
    s.raster.Reset();
    s.sampler.Reset();
    s.cursor_ps.Reset();
    s.cursor_cb.Reset();
    s.blend_premul.Reset();
    s.cursor_active.store(false);
    s.device = nullptr;
    s.context = nullptr;
}

bool OsdRenderer::HasContent() const {
    auto& s = *impl_;
    if (s.menu && s.menu->Visible()) return true;
    std::lock_guard<std::mutex> lk(s.toast_mu);
    return !s.toast_msg.empty() && steady_clock::now() < s.toast_expiry;
}

bool OsdRenderer::MenuVisible() const {
    auto& s = *impl_;
    return s.menu && s.menu->Visible();
}

void OsdRenderer::ToggleMenu() {
    auto& s = *impl_;
    if (s.menu) s.menu->Toggle();
}

void OsdRenderer::SetText(const std::string& msg, milliseconds ttl) {
    auto& s = *impl_;
    std::lock_guard<std::mutex> lk(s.toast_mu);
    s.toast_msg = msg;
    s.toast_expiry = steady_clock::now() + ttl;
}

void OsdRenderer::SetAppName(const std::string& app_name) {
    auto& s = *impl_;
    if (s.menu) s.menu->SetAppName(app_name);
}

void OsdRenderer::SetVersion(const std::string& version) {
    auto& s = *impl_;
    if (s.menu) s.menu->SetVersion(version);
}

void OsdRenderer::RenderFrame(ID3D11Texture2D* out_sbs) {
    auto& s = *impl_;
    if (!s.imgui_ctx || !out_sbs || !s.osd_tex) return;

    // Toggle the headset window's WS_EX_LAYERED|WS_EX_TRANSPARENT bits in
    // sync with menu visibility so clicks reach the OSD when it's open.
    s.ApplyMenuVisibility(MenuVisible());

    // Gate the global LL mouse hook on menu visibility (or active capture).
    // Keeping it always-on routes every system-wide mouse event through this
    // process, which adds latency / cursor stutter when our pump thread is
    // busy. Clicks/wheel are only consumed by the OSD when it's actually
    // visible, so the hook is unnecessary the rest of the time.
    bool has_content = HasContent();
    if (s.input) {
        s.input->SetMouseHookActive(MenuVisible() || s.input->IsCapturing());
    }

    // Stereo cursor: the driver pushes `cursor_active` while the feature is
    // on and the game (or our menu) owns the foreground — the hardware
    // cursor is hidden then, so we're the only visible cursor. With the menu
    // open the ImGui software cursor takes over (correctly folded per-eye);
    // otherwise draw the arrow pass directly into the SbS frame.
    const bool stereo_cursor = s.cursor_active.load(std::memory_order_relaxed);
    float cursor_u = 0.0f, cursor_v = 0.0f;
    const bool cursor_draw = stereo_cursor && !MenuVisible()
                              && s.MapCursorToEyeUV(cursor_u, cursor_v);

    // Cheap early-out if there's nothing to draw and no toast pending.
    if (!has_content && (!s.input || !s.input->IsCapturing())) {
        // Still pump input edges so a future Ctrl+Home press is detected.
        if (s.input) s.input->Poll();
        if (cursor_draw) s.DrawCursorPass(out_sbs, cursor_u, cursor_v);
        return;
    }

    // ----- Set up ImGui frame -----
    ImGui::SetCurrentContext(s.imgui_ctx);
    ImGuiIO& io = ImGui::GetIO();

    auto now = steady_clock::now();
    float dt = duration_cast<duration<float>>(now - s.last_frame_time).count();
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;
    io.DeltaTime = dt;
    s.last_frame_time = now;

    if (s.input) {
        s.input->Poll();
        OsdSurface surface;
        surface.eye_w = static_cast<int>(s.eye_w);
        surface.eye_h = static_cast<int>(s.eye_h);
        surface.hwnd  = s.headset_hwnd;
        // Use the snapshot taken at Init — NOT the live config — so a
        // System-tab output_mode change doesn't break cursor mapping until
        // the driver restarts and the new presenter takes over.
        surface.layout = s.SurfaceLayout();
        s.input->FeedImGui(io, surface);
    }

    // With the stereo cursor active and the menu open, the hardware cursor
    // is hidden — let ImGui draw its software cursor at the folded per-eye
    // position so the pointer lines up with the widgets in both eyes.
    io.MouseDrawCursor = stereo_cursor && MenuVisible();

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    // Menu UI.
    if (s.menu && s.menu->Visible() && s.input) {
        s.menu->BuildUI(*s.input);
    }

    // Toast widget — bottom-left, no decoration, bright green to match the
    // old GDI+ overlay's look.
    {
        std::string msg;
        bool active = false;
        {
            std::lock_guard<std::mutex> lk(s.toast_mu);
            if (!s.toast_msg.empty() && steady_clock::now() < s.toast_expiry) {
                msg = s.toast_msg;
                active = true;
            }
        }
        if (active) {
            const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                           ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoNav |
                                           ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_NoBackground;
            const float pad_x = 50.0f;
            const float pad_y = 30.0f;
            // Toast text is 2x the menu font so it's legible at headset
            // distance even when the menu chrome is hidden.
            const float toast_scale = 2.0f;
            // Bound text wrap to the eye width so long messages wrap instead
            // of running past the right edge. Pass the wrap position as an
            // explicit window-local pixel value rather than 0.0f — with
            // AlwaysAutoResize, a wrap-at-content-right behaves circularly
            // (window width depends on content, content wrap depends on
            // window width) and collapses to one glyph per line on first
            // frame, producing a vertical column of characters.
            const float wrap_local = (std::max)(64.0f,
                static_cast<float>(s.eye_w) - 2.0f * pad_x -
                2.0f * ImGui::GetStyle().WindowPadding.x);
            // Anchor by bottom-left pivot so wrapped multi-line toasts grow
            // upward into the screen rather than falling off the bottom.
            ImGui::SetNextWindowPos(
                ImVec2(pad_x, static_cast<float>(s.eye_h) - pad_y),
                ImGuiCond_Always,
                ImVec2(0.0f, 1.0f));
            if (ImGui::Begin("##vrto3d_toast", nullptr, flags)) {
                ImGui::SetWindowFontScale(toast_scale);
                ImGui::PushTextWrapPos(wrap_local);
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));
                ImGui::TextUnformatted(msg.c_str());
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
            }
            ImGui::End();
        }
    }

    ImGui::Render();

    // ----- Pass 1: render ImGui draw lists into osd_tex -----
    {
        ID3D11RenderTargetView* rtv = s.osd_rtv.Get();
        const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };  // transparent
        s.context->OMSetRenderTargets(1, &rtv, nullptr);
        s.context->ClearRenderTargetView(rtv, clear);

        D3D11_VIEWPORT vp{};
        vp.Width    = static_cast<float>(s.eye_w);
        vp.Height   = static_cast<float>(s.eye_h);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        s.context->RSSetViewports(1, &vp);

        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    // ----- Pass 2: composite osd_tex into both halves of out_sbs -----
    {
        ComPtr<ID3D11RenderTargetView> sbs_rtv;
        if (FAILED(s.device->CreateRenderTargetView(out_sbs, nullptr, &sbs_rtv))) {
            return;
        }

        D3D11_TEXTURE2D_DESC sbs_desc{};
        out_sbs->GetDesc(&sbs_desc);
        const UINT half_w = sbs_desc.Width / 2;
        const UINT full_h = sbs_desc.Height;

        ID3D11RenderTargetView* rtv = sbs_rtv.Get();
        s.context->OMSetRenderTargets(1, &rtv, nullptr);

        const float blend_factor[4] = { 1, 1, 1, 1 };
        s.context->OMSetBlendState(s.blend.Get(), blend_factor, 0xFFFFFFFF);
        s.context->RSSetState(s.raster.Get());
        s.context->IASetInputLayout(nullptr);
        s.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        s.context->VSSetShader(s.vs.Get(), nullptr, 0);
        s.context->PSSetShader(s.ps.Get(), nullptr, 0);
        ID3D11SamplerState* samps[] = { s.sampler.Get() };
        s.context->PSSetSamplers(0, 1, samps);
        ID3D11ShaderResourceView* srvs[] = { s.osd_srv.Get() };
        s.context->PSSetShaderResources(0, 1, srvs);

        D3D11_VIEWPORT vp{};
        vp.Width    = static_cast<float>(half_w);
        vp.Height   = static_cast<float>(full_h);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;

        // Left eye
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        s.context->RSSetViewports(1, &vp);
        s.context->Draw(3, 0);

        // Right eye
        vp.TopLeftX = static_cast<float>(half_w);
        s.context->RSSetViewports(1, &vp);
        s.context->Draw(3, 0);

        // Unbind SRV so the next frame's CopyResource on out_sbs (which the
        // SRV points at indirectly) doesn't trigger a debug-layer warning.
        ID3D11ShaderResourceView* null_srv[] = { nullptr };
        s.context->PSSetShaderResources(0, 1, null_srv);
        ID3D11RenderTargetView* null_rtv = nullptr;
        s.context->OMSetRenderTargets(1, &null_rtv, nullptr);
    }

    // ----- Pass 3: stereo cursor arrow (topmost, over the OSD/toast) -----
    if (cursor_draw) {
        s.DrawCursorPass(out_sbs, cursor_u, cursor_v);
    }
}

void OsdRenderer::SetStereoCursor(bool active, float depth_px, float size_px, void* game_hwnd) {
    auto& s = *impl_;
    s.cursor_active.store(active, std::memory_order_relaxed);
    s.cursor_depth_px.store(depth_px, std::memory_order_relaxed);
    s.cursor_size_px.store(size_px, std::memory_order_relaxed);
    s.cursor_game_hwnd.store(game_hwnd, std::memory_order_relaxed);
}

} // namespace vrto3d::osd
