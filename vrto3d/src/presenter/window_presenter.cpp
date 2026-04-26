/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "window_presenter.h"

#include <chrono>
#include <cstring>

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

#include "dx11_renderer.h"
#include "vrto3dlib/debug_log.hpp"

using Microsoft::WRL::ComPtr;

namespace vrto3d {

namespace {

// Mode constants matching OutputMode values the shader handles (LeiaSR / NvidiaDX9
// are handled by their own presenters — not this one).
//
// DualDisplay / DualDisplayFlip share the SbS shader path; their distinction is
// the window spans two contiguous monitors (handled at window-creation time).
// DualDisplayFlip flips the left half vertically.
constexpr uint32_t kModeSbS                        = 0;
constexpr uint32_t kModeTaB                        = 1;
constexpr uint32_t kModeRowInterlaced              = 2;
constexpr uint32_t kModeColInterlaced              = 3;
constexpr uint32_t kModeCheckerboard               = 4;
constexpr uint32_t kModeAnaglyphRedCyan            = 5;
constexpr uint32_t kModeAnaglyphRedCyanDubois      = 6;
constexpr uint32_t kModeAnaglyphRedCyanDeghosted   = 7;
constexpr uint32_t kModeAnaglyphGreenMagenta       = 8;
constexpr uint32_t kModeAnaglyphGreenMagentaDubois = 9;
constexpr uint32_t kModeAnaglyphGreenMagentaDeghosted = 10;
constexpr uint32_t kModeAnaglyphBlueAmber          = 11;
constexpr uint32_t kModeDualDisplayFlip            = 12;

uint32_t ModeToShaderEnum(OutputMode m)
{
    switch (m) {
        case OutputMode::SbS:                            return kModeSbS;
        case OutputMode::DualDisplay:                    return kModeSbS;
        case OutputMode::DualDisplayFlip:                return kModeDualDisplayFlip;
        case OutputMode::TaB:                            return kModeTaB;
        case OutputMode::RowInterlaced:                  return kModeRowInterlaced;
        case OutputMode::ColInterlaced:                  return kModeColInterlaced;
        case OutputMode::Checkerboard:                   return kModeCheckerboard;
        case OutputMode::AnaglyphRedCyan:                return kModeAnaglyphRedCyan;
        case OutputMode::AnaglyphRedCyanDubois:          return kModeAnaglyphRedCyanDubois;
        case OutputMode::AnaglyphRedCyanDeghosted:       return kModeAnaglyphRedCyanDeghosted;
        case OutputMode::AnaglyphGreenMagenta:           return kModeAnaglyphGreenMagenta;
        case OutputMode::AnaglyphGreenMagentaDubois:     return kModeAnaglyphGreenMagentaDubois;
        case OutputMode::AnaglyphGreenMagentaDeghosted:  return kModeAnaglyphGreenMagentaDeghosted;
        case OutputMode::AnaglyphBlueAmber:              return kModeAnaglyphBlueAmber;
        default:                                         return kModeSbS;
    }
}

// Vertex shader: generates a full-screen triangle from SV_VertexID (no vertex buffer).
const char* kVsSource = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    float2 p = float2((vid << 1) & 2, vid & 2);
    o.uv  = p;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

// Pixel shader: samples the 2W x H SbS input texture and repacks according to `mode`.
// cbuffer layout mirrors CBParams in the header.
const char* kPsSource = R"(
Texture2D    g_sbs : register(t0);
SamplerState g_smp : register(s0);

cbuffer Params : register(b0) {
    uint  mode;
    uint  framepack_offset;
    uint  eye_swap;
    uint  vd_fsbs_hack;
    float out_width;
    float out_height;
    float aspect_ratio;
    float _pad;
};

// Given a half (0=left, 1=right), sample the input SbS at the given (u_half, v).
// u_half in [0,1] = position within the eye's half of the input.
float4 SampleEye(uint half_idx, float u_half, float v) {
    // eye_swap flips which half-source we read.
    uint eye = (eye_swap != 0) ? (1u - half_idx) : half_idx;
    float u_src = 0.5 * u_half + 0.5 * float(eye);
    return g_sbs.Sample(g_smp, float2(u_src, v));
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // SbS
    if (mode == 0) {
        float2 in_uv = uv;
        // vd_fsbs_hack: only draw rows [0.25, 0.75) (center band of a 2W x 2H window).
        if (vd_fsbs_hack != 0) {
            if (uv.y < 0.25 || uv.y >= 0.75) return float4(0,0,0,1);
            in_uv.y = (uv.y - 0.25) * 2.0;
        }
        // Split output: left half samples left eye fully; right half samples right eye fully.
        uint half_idx = (uv.x < 0.5) ? 0u : 1u;
        float u_half  = (uv.x < 0.5) ? (uv.x * 2.0) : ((uv.x - 0.5) * 2.0);
        return SampleEye(half_idx, u_half, in_uv.y);
    }

    // TaB
    if (mode == 1) {
        float gap = (framepack_offset > 0) ? (float(framepack_offset) / out_height) : 0.0;
        float half_height = (1.0 - gap) * 0.5;
        if (uv.y < half_height) {
            // Top half -> left eye
            return SampleEye(0u, uv.x, uv.y / half_height);
        } else if (uv.y >= half_height + gap) {
            // Bottom half -> right eye
            float v_rel = (uv.y - (half_height + gap)) / half_height;
            return SampleEye(1u, uv.x, v_rel);
        } else {
            return float4(0,0,0,1);  // framepack gap
        }
    }

    // Row-interlaced: alternating rows; odd rows = right eye.
    if (mode == 2) {
        uint row = (uint)floor(uv.y * out_height);
        uint half_idx = (row & 1u);
        return SampleEye(half_idx, uv.x, uv.y);
    }

    // Col-interlaced
    if (mode == 3) {
        uint col = (uint)floor(uv.x * out_width);
        uint half_idx = (col & 1u);
        return SampleEye(half_idx, uv.x, uv.y);
    }

    // Checkerboard
    if (mode == 4) {
        uint row = (uint)floor(uv.y * out_height);
        uint col = (uint)floor(uv.x * out_width);
        uint half_idx = ((row + col) & 1u);
        return SampleEye(half_idx, uv.x, uv.y);
    }

    // ----- Anaglyph variants (3DToElse / iaian7+vectorform formulas) -----
    // Sample full image of each eye; cA = left, cB = right.
    float4 cA = SampleEye(0u, uv.x, uv.y);
    float4 cB = SampleEye(1u, uv.x, uv.y);

    // Anaglyph red-cyan (simple)
    if (mode == 5) {
        return float4(cA.r, cB.g, cB.b, 1.0);
    }

    // Anaglyph red-cyan Dubois
    if (mode == 6) {
        float r = saturate( 0.437 * cA.r + 0.449 * cA.g + 0.164 * cA.b
                           - 0.011 * cB.r - 0.032 * cB.g - 0.007 * cB.b );
        float g = saturate(-0.062 * cA.r - 0.062 * cA.g - 0.024 * cA.b
                           + 0.377 * cB.r + 0.761 * cB.g + 0.009 * cB.b );
        float b = saturate(-0.048 * cA.r - 0.050 * cA.g - 0.017 * cA.b
                           - 0.026 * cB.r - 0.093 * cB.g + 1.234 * cB.b );
        return float4(r, g, b, 1.0);
    }

    // Anaglyph red-cyan deghosted (iaian7 / vectorform)
    if (mode == 7) {
        float Contrast = 1.0;
        float DeGhost  = 0.06 * 0.1;
        float contrast = (Contrast * 0.5) + 0.5;
        float LOne = contrast * 0.45;
        float ROne = contrast;

        float4 image = float4(0,0,0,1);
        float4 accum;
        accum = saturate(cA * float4(LOne, (1.0-LOne)*0.5, (1.0-LOne)*0.5, 1.0));
        image.r = pow(accum.r + accum.g + accum.b, 1.00);
        accum = saturate(cB * float4(1.0-ROne, ROne, 0.0, 1.0));
        image.g = pow(accum.r + accum.g + accum.b, 1.15);
        accum = saturate(cB * float4(1.0-ROne, 0.0, ROne, 1.0));
        image.b = pow(accum.r + accum.g + accum.b, 1.15);

        accum = image;
        image.r = accum.r + (accum.r * DeGhost) + (accum.g * (DeGhost * -0.5)) + (accum.b * (DeGhost * -0.5));
        image.g = accum.g + (accum.r * (DeGhost * -0.25)) + (accum.g * (DeGhost * 0.5)) + (accum.b * (DeGhost * -0.25));
        image.b = accum.b + (accum.r * (DeGhost * -0.25)) + (accum.g * (DeGhost * -0.25)) + (accum.b * (DeGhost * 0.5));
        image.a = 1.0;
        return image;
    }

    // Anaglyph green-magenta (simple)
    if (mode == 8) {
        return float4(cB.r, cA.g, cB.b, 1.0);
    }

    // Anaglyph green-magenta Dubois
    if (mode == 9) {
        float r = saturate(-0.062 * cA.r - 0.158 * cA.g - 0.039 * cA.b
                           + 0.529 * cB.r + 0.705 * cB.g + 0.024 * cB.b );
        float g = saturate( 0.284 * cA.r + 0.668 * cA.g + 0.143 * cA.b
                           - 0.016 * cB.r - 0.015 * cB.g + 0.065 * cB.b );
        float b = saturate(-0.015 * cA.r - 0.027 * cA.g + 0.021 * cA.b
                           + 0.009 * cB.r + 0.075 * cB.g + 0.937 * cB.b );
        return float4(r, g, b, 1.0);
    }

    // Anaglyph green-magenta deghosted
    if (mode == 10) {
        float Contrast = 1.0;
        float DeGhost  = 0.06 * 0.275;
        float contrast = (Contrast * 0.5) + 0.5;
        float LOne = contrast * 0.45;
        float ROne = contrast * 0.8;

        float4 image = float4(0,0,0,1);
        float4 accum;
        accum = saturate(cB * float4(ROne, 1.0-ROne, 0.0, 1.0));
        image.r = pow(accum.r + accum.g + accum.b, 1.15);
        accum = saturate(cA * float4((1.0-LOne)*0.5, LOne, (1.0-LOne)*0.5, 1.0));
        image.g = pow(accum.r + accum.g + accum.b, 1.05);
        accum = saturate(cB * float4(0.0, 1.0-ROne, ROne, 1.0));
        image.b = pow(accum.r + accum.g + accum.b, 1.15);

        accum = image;
        image.r = accum.r + (accum.r * (DeGhost * 0.5))  + (accum.g * (DeGhost * -0.25)) + (accum.b * (DeGhost * -0.25));
        image.g = accum.g + (accum.r * (DeGhost * -0.5)) + (accum.g * (DeGhost * 0.25))  + (accum.b * (DeGhost * -0.5));
        image.b = accum.b + (accum.r * (DeGhost * -0.25))+ (accum.g * (DeGhost * -0.25)) + (accum.b * (DeGhost * 0.5));
        image.a = 1.0;
        return image;
    }

    // Anaglyph blue-amber (ColorCode style)
    if (mode == 11) {
        float Contrast = 1.0;
        float DeGhost  = 0.06 * 0.275;
        float contrast = (Contrast * 0.5) + 0.5;
        float LOne = contrast * 0.45;
        float ROne = contrast;

        float4 image = float4(0,0,0,1);
        float4 accum;
        accum = saturate(cA * float4(ROne, 0.0, 1.0-ROne, 1.0));
        image.r = pow(accum.r + accum.g + accum.b, 1.05);
        accum = saturate(cA * float4(0.0, ROne, 1.0-ROne, 1.0));
        image.g = pow(accum.r + accum.g + accum.b, 1.10);
        accum = saturate(cB * float4((1.0-LOne)*0.5, (1.0-LOne)*0.5, LOne, 1.0));
        image.b = pow(accum.r + accum.g + accum.b, 1.0);
        image.b = lerp(pow(image.b, (DeGhost * 0.15) + 1.0),
                       1.0 - pow(abs(1.0 - image.b), (DeGhost * 0.15) + 1.0),
                       image.b);

        accum = image;
        image.r = accum.r + (accum.r * (DeGhost * 1.5))  + (accum.g * (DeGhost * -0.75)) + (accum.b * (DeGhost * -0.75));
        image.g = accum.g + (accum.r * (DeGhost * -0.75)) + (accum.g * (DeGhost * 1.5))   + (accum.b * (DeGhost * -0.75));
        image.b = accum.b + (accum.r * (DeGhost * -1.5)) + (accum.g * (DeGhost * -1.5))  + (accum.b * (DeGhost * 3.0));
        return saturate(image);
    }

    // DualDisplayFlip: same horizontal split as SbS/DualDisplay, but the LEFT
    // half is sampled with v inverted (vertical flip).
    if (mode == 12) {
        uint half_idx = (uv.x < 0.5) ? 0u : 1u;
        float u_half  = (uv.x < 0.5) ? (uv.x * 2.0) : ((uv.x - 0.5) * 2.0);
        float v_src   = (half_idx == 0u) ? (1.0 - uv.y) : uv.y;
        return SampleEye(half_idx, u_half, v_src);
    }

    // Fallback
    return float4(uv, 0, 1);
}
)";


bool CompileShader(const char* src, const char* entry, const char* profile,
                   ComPtr<ID3DBlob>& out_blob)
{
    ComPtr<ID3DBlob> errors;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
                            entry, profile, flags, 0, &out_blob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            LOG() << "D3DCompile(" << entry << ") error: "
                  << static_cast<const char*>(errors->GetBufferPointer());
        }
        return false;
    }
    return true;
}

}  // namespace


bool WindowPresenter::CreateShaders()
{
    ID3D11Device* dev = renderer_->Device();

    ComPtr<ID3DBlob> vs_blob, ps_blob;
    if (!CompileShader(kVsSource, "main", "vs_5_0", vs_blob)) return false;
    if (!CompileShader(kPsSource, "main", "ps_5_0", ps_blob)) return false;

    if (FAILED(dev->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs_))) return false;
    if (FAILED(dev->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &ps_)))  return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD         = 0;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &sampler_))) return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(CBParams);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb_))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, &rasterizer_))) return false;

    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].BlendEnable = FALSE;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bl, &blend_))) return false;

    return true;
}


bool WindowPresenter::CreateSwapChain(Dx11Renderer& renderer)
{
#ifdef _WIN32
    ID3D11Device* dev = renderer.Device();

    ComPtr<IDXGIDevice> dxgi_dev;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi_dev)))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_dev->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

    // Use legacy DISCARD swap effect — FLIP_DISCARD requires the window to be
    // visible / on the desktop session and has been observed to silently fail
    // to composite when the owner process (vrserver.exe) is a console app.
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width       = window_->Width();
    scd.Height      = window_->Height();
    scd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc  = { 1, 0 };
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 1;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_DISCARD;
    scd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

    HWND hwnd = static_cast<HWND>(window_->NativeHandle());
    HRESULT hr = factory->CreateSwapChainForHwnd(dev, hwnd, &scd, nullptr, nullptr, &swapchain_);
    if (FAILED(hr)) {
        LOG() << "CreateSwapChainForHwnd failed hr=" << std::hex << hr;
        return false;
    }
    LOG() << "WindowPresenter: swapchain created " << scd.Width << "x" << scd.Height
          << " (DISCARD, " << scd.BufferCount << " buffer)";

    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
    if (FAILED(dev->CreateRenderTargetView(bb.Get(), nullptr, &swapchain_rtv_))) return false;

    swap_width_  = window_->Width();
    swap_height_ = window_->Height();
    return true;
#else
    (void)renderer;
    // Linux swapchain: DXVK provides IDXGIFactory2::CreateSwapChainForHwnd with SDL's window
    // via an HWND shim. Full bring-up is a v1.5 task.
    LOG() << "WindowPresenter::CreateSwapChain is a v1.5 TODO on Linux";
    return false;
#endif
}


bool WindowPresenter::Init(Dx11Renderer& renderer,
                           const StereoDisplayDriverConfiguration& cfg,
                           const FocusContext& focus)
{
    renderer_         = &renderer;
    mode_             = cfg.output_mode;
    eye_swap_         = cfg.eye_swap;
    vd_fsbs_hack_     = cfg.vd_fsbs_hack;
    framepack_offset_ = static_cast<uint32_t>(cfg.framepack_offset);
    aspect_ratio_     = cfg.aspect_ratio;
    spans_two_monitors_ = (mode_ == OutputMode::DualDisplay
                           || mode_ == OutputMode::DualDisplayFlip);
    auto_focus_       = cfg.auto_focus;
    focus_            = focus;

    // Resolve target monitor(s) — reuses cfg.display_index. The dual-display
    // modes ask for a contiguous-right secondary so the window spans both.
    platform::MonitorInfo primary{}, secondary{};
    if (!platform::ResolveTargetMonitors(cfg.display_index, spans_two_monitors_, primary, secondary)) {
        LOG() << "WindowPresenter::Init: ResolveTargetMonitors failed";
        return false;
    }

    // vd_fsbs_hack needs a 2H window; shader centers content vertically.
    uint32_t override_h = (mode_ == OutputMode::SbS && vd_fsbs_hack_) ? primary.height * 2 : 0;

    // Win32 windows are tied to their creating thread — only that thread can
    // pump them, and Windows tears them down if their owner thread doesn't
    // service messages. vrserver.exe's activation thread doesn't pump, so we
    // spin a dedicated thread here that creates the window, builds the
    // swapchain, runs the message loop, and stays alive for the window's life.
    window_stop_.store(false);
    window_ready_.store(false);
    window_failed_.store(false);
    window_thread_ = std::thread(&WindowPresenter::WindowThreadLoop, this, &renderer,
                                  primary, secondary, override_h);

    // Wait up to 5s for the window thread to finish setup.
    for (int i = 0; i < 500; ++i) {
        if (window_ready_.load() || window_failed_.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (window_failed_.load() || !window_ready_.load()) {
        LOG() << "WindowPresenter::Init: window thread failed to come up";
        Shutdown();
        return false;
    }

    LOG() << "WindowPresenter: window " << (window_ ? window_->Width() : 0) << "x"
          << (window_ ? window_->Height() : 0)
          << " on display_index=" << cfg.display_index
          << " spans_two=" << (spans_two_monitors_ ? "yes" : "no")
          << " mode=" << OutputModeToString(mode_);

    // Kick focus thread (z-order asserts; window thread already pumps messages).
    focus_stop_.store(false);
    focus_thread_ = std::thread(&WindowPresenter::FocusThreadLoop, this);

    LOG() << "WindowPresenter: window thread up; focus thread running";
    return true;
}


void WindowPresenter::WindowThreadLoop(Dx11Renderer* renderer,
                                        platform::MonitorInfo primary,
                                        platform::MonitorInfo secondary,
                                        uint32_t override_h)
{
    window_ = platform::CreatePresentWindow(
        primary,
        (secondary.width > 0 ? &secondary : nullptr),
        override_h,
        "VRto3D");
    if (!window_) {
        LOG() << "WindowThreadLoop: CreatePresentWindow failed";
        window_failed_.store(true);
        return;
    }

    if (!CreateShaders() || !CreateSwapChain(*renderer)) {
        LOG() << "WindowThreadLoop: shader/swapchain init failed";
        window_failed_.store(true);
        window_.reset();
        return;
    }

    // Initial black frame so DWM composites something for our HWND.
    {
        ID3D11DeviceContext* ctx = renderer->Context();
        const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ctx->ClearRenderTargetView(swapchain_rtv_.Get(), clear);
        swapchain_->Present(0, 0);
    }
    // Window starts non-topmost; FocusThreadLoop asserts topmost when
    // is_on_top_ / man_on_top_ / ue3d_on_top_ / auto_focus path triggers.
    window_ready_.store(true);

    // This thread owns the window message pump AND drives rendering. Win32
    // requires the same thread that created the window to service its message
    // queue, and DXGI DISCARD swap effect requires Present to be issued by
    // that same thread for DWM to update the window reliably.
    while (!window_stop_.load(std::memory_order_relaxed)) {
        // Block up to 33ms for a frame from the compositor; render + present
        // when one arrives. PresentFrame internally calls swapchain->Present
        // which blocks for vsync, naturally pacing this loop.
        renderer->WaitAndDrawPending(33);

        // Pump window messages so the window stays responsive even if the
        // compositor stops submitting frames.
        if (window_) window_->PollEvents();
    }

    // Tear down on this thread (DestroyWindow must be called on the creating thread).
    swapchain_rtv_.Reset();
    swapchain_.Reset();
    rasterizer_.Reset();
    blend_.Reset();
    cb_.Reset();
    sampler_.Reset();
    ps_.Reset();
    vs_.Reset();
    window_.reset();
    LOG() << "WindowThreadLoop: exited";
}


void WindowPresenter::PresentFrame(ID3D11Texture2D* sbs_input)
{
    if (!swapchain_ || !sbs_input) {
        static std::atomic<bool> logged_null{false};
        bool expected = false;
        if (logged_null.compare_exchange_strong(expected, true)) {
            LOG() << "PresentFrame: early return swapchain=" << (void*)swapchain_.Get()
                  << " sbs_input=" << (void*)sbs_input;
        }
        return;
    }

    ID3D11Device* dev = renderer_->Device();
    ID3D11DeviceContext* ctx = renderer_->Context();

    ComPtr<ID3D11ShaderResourceView> srv;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    D3D11_TEXTURE2D_DESC td{};
    sbs_input->GetDesc(&td);
    sv.Format                    = td.Format;
    sv.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels       = static_cast<UINT>(-1);   // all mips
    sv.Texture2D.MostDetailedMip = 0;
    HRESULT srv_hr = dev->CreateShaderResourceView(sbs_input, &sv, &srv);
    if (FAILED(srv_hr)) {
        static std::atomic<bool> logged_srv{false};
        bool expected = false;
        if (logged_srv.compare_exchange_strong(expected, true)) {
            LOG() << "PresentFrame: CreateShaderResourceView failed hr=0x"
                  << std::hex << srv_hr
                  << " fmt=" << std::dec << td.Format
                  << " bind=0x" << std::hex << td.BindFlags;
        }
        return;
    }

    static std::atomic<bool> logged_first{false};
    bool expected = false;
    if (logged_first.compare_exchange_strong(expected, true)) {
        LOG() << "PresentFrame: first call OK srv=" << (void*)srv.Get()
              << " td=" << td.Width << "x" << td.Height
              << " fmt=" << td.Format << " bind=0x" << std::hex << td.BindFlags;
    }

    // (magenta-clear short-circuit removed — confirmed Present-to-screen works)

    // Update constant buffer.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        CBParams p{};
        p.mode             = ModeToShaderEnum(mode_);
        p.framepack_offset = framepack_offset_;
        p.eye_swap         = eye_swap_ ? 1u : 0u;
        p.vd_fsbs_hack     = vd_fsbs_hack_ ? 1u : 0u;
        p.out_width        = static_cast<float>(swap_width_);
        p.out_height       = static_cast<float>(swap_height_);
        p.aspect_ratio     = aspect_ratio_;
        std::memcpy(mapped.pData, &p, sizeof(p));
        ctx->Unmap(cb_.Get(), 0);
    }

    D3D11_VIEWPORT vp{};
    vp.Width    = static_cast<float>(swap_width_);
    vp.Height   = static_cast<float>(swap_height_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    ID3D11RenderTargetView* rtv = swapchain_rtv_.Get();
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ctx->ClearRenderTargetView(rtv, clear);

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rasterizer_.Get());
    float blend_factor[4] = { 1,1,1,1 };
    ctx->OMSetBlendState(blend_.Get(), blend_factor, 0xFFFFFFFF);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = { cb_.Get() };
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* srvs[] = { srv.Get() };
    ctx->PSSetShaderResources(0, 1, srvs);
    ID3D11SamplerState* samps[] = { sampler_.Get() };
    ctx->PSSetSamplers(0, 1, samps);

    ctx->Draw(3, 0);

    // Unbind SRV so we don't warn on next frame when input changes.
    ID3D11ShaderResourceView* null_srv[] = { nullptr };
    ctx->PSSetShaderResources(0, 1, null_srv);

    HRESULT hr = swapchain_->Present(1, 0);
    if (FAILED(hr)) {
        static std::atomic<bool> logged_present_fail{false};
        bool expected = false;
        if (logged_present_fail.compare_exchange_strong(expected, true)) {
            LOG() << "WindowPresenter: swapchain->Present failed hr=" << std::hex << hr;
        }
    } else if (hr == DXGI_STATUS_OCCLUDED) {
        static std::atomic<bool> logged_occluded{false};
        bool expected = false;
        if (logged_occluded.compare_exchange_strong(expected, true)) {
            LOG() << "WindowPresenter: swapchain occluded (window hidden / minimized)";
        }
    }
    // Message pump runs on WindowThreadLoop — don't poll here.
}


void WindowPresenter::Shutdown()
{
    if (!window_thread_.joinable() && !window_) return;   // idempotent
    LOG() << "WindowPresenter: Shutdown called";

    focus_stop_.store(true);
    if (focus_thread_.joinable()) focus_thread_.join();

    // Window thread owns the window + swapchain; signal it to exit and join.
    window_stop_.store(true);
    if (window_thread_.joinable()) window_thread_.join();

    renderer_ = nullptr;
}


void WindowPresenter::FocusThreadLoop()
{
    using namespace std::chrono_literals;

    bool was_on_top = false;
    bool nudged     = false;
    int  reassert_counter = 0;
    uint32_t last_auto_focused_pid = 0;   // tracks which pid we already auto-raised for (auto_focus path)
    uint32_t last_ue3d_focused_pid = 0;   // tracks which pid we already raised for via UE3D IPC

    while (!focus_stop_.load(std::memory_order_relaxed)) {
        if (!window_) break;

        // Note: do NOT pump messages here — the window's message queue must
        // be serviced from its creating thread (WindowThreadLoop).

        const bool is_on_top   = focus_.is_on_top   && focus_.is_on_top->load();
        const bool man_on_top  = focus_.man_on_top  && focus_.man_on_top->load();
        const bool ue3d_on_top = focus_.ue3d_on_top && focus_.ue3d_on_top->load();
        const uint32_t pid     = focus_.app_pid ? focus_.app_pid->load() : 0;

        // Multi-display placement nudge, once after first show.
        if (spans_two_monitors_ && !nudged) {
            window_->MultiDisplayNudge();
            nudged = true;
        }

        const bool app_running = platform::IsProcessRunning(pid);

        // Reset auto-focus + UE3D latches when the tracked app is gone so a
        // future launch of a (possibly same-pid-recycled) app can re-trigger.
        if (pid == 0 || !app_running) {
            last_auto_focused_pid = 0;
            last_ue3d_focused_pid = 0;
        }

        bool want_on_top = false;
        if (man_on_top) {
            want_on_top = true;
        } else if (is_on_top && app_running) {
            want_on_top = true;
        } else if (auto_focus_ && !is_on_top && !ue3d_on_top
                   && app_running && pid != 0
                   && pid != last_auto_focused_pid) {
            // Auto-raise once per new tracked app PID. Without the pid latch
            // the user could never disable topmost via Ctrl+F8 while an app
            // is running — this branch would re-enable it on the next tick.
            if (focus_.is_on_top)  focus_.is_on_top->store(true);
            if (focus_.man_on_top) focus_.man_on_top->store(true);
            last_auto_focused_pid = pid;
            want_on_top = true;
        } else if (ue3d_on_top && pid != 0 && pid != last_ue3d_focused_pid) {
            // UE3D IPC set ue3d_on_top_; raise once per new pid. Same latch
            // pattern as auto-focus so Ctrl+F8 can subsequently disable it.
            last_ue3d_focused_pid = pid;
            want_on_top = true;
        }

        if (want_on_top != was_on_top) {
            if (want_on_top) window_->BringToTop();
            else             window_->ReleaseTopmost();
            was_on_top = want_on_top;
            reassert_counter = 0;
        }
        // While we want to be on top, re-assert ~once per second so other
        // apps that grab HWND_TOPMOST (dialogs, dashboards) don't push us
        // behind for long.
        else if (want_on_top && ++reassert_counter >= 20) {
            reassert_counter = 0;
            window_->BringToTop();
        }

        std::this_thread::sleep_for(50ms);
    }
}

}  // namespace vrto3d
