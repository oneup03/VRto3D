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
        return true;
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

    // Cheap early-out if there's nothing to draw and no toast pending.
    if (!has_content && (!s.input || !s.input->IsCapturing())) {
        // Still pump input edges so a future Ctrl+Home press is detected.
        if (s.input) s.input->Poll();
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
        switch (s.active_output_mode) {
            case OutputMode::SbS:
            case OutputMode::VirtualDesktop:
            case OutputMode::DualDisplay:
            case OutputMode::DualDisplayFlip:
                surface.layout = StereoLayout::HorizontalSbs;
                break;
            case OutputMode::TaB:
            case OutputMode::FramePacked720p60:
            case OutputMode::FramePacked1080p24:
            case OutputMode::FramePacked1080p60:
            case OutputMode::FramePacked1080p60CVT:
                surface.layout = StereoLayout::VerticalTab;
                break;
            default:
                surface.layout = StereoLayout::Mono;
                break;
        }
        s.input->FeedImGui(io, surface);
    }

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
}

} // namespace vrto3d::osd
