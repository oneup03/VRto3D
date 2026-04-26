/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifdef _WIN32

#include "leiasr_presenter.h"

#include <chrono>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <delayimp.h>

#include "dx11_renderer.h"
#include "vrto3dlib/debug_log.hpp"

// LeiaSR SDK headers
#include "sr/management/srcontext.h"
#include "sr/utility/exception.h"
#include "sr/weaver/dx11weaver.h"

using Microsoft::WRL::ComPtr;

namespace {

// Helper: invokes SRContext::create inside an SEH __try block so a missing
// LeiaSR / OpenCV DLL (delay-load resolution failure) returns nullptr instead
// of propagating a structured exception that would crash vrserver. We also
// catch SR's C++ ServerNotAvailableException at the C++ layer above.
//
// Two-tier protection:
//   1. SEH __except: catches VcppException(MOD_NOT_FOUND / PROC_NOT_FOUND)
//      raised by the delay-load helper if the DLL or import isn't resolvable.
//   2. C++ try/catch (in caller): catches SR's runtime exceptions like
//      ServerNotAvailableException when DLLs are present but service isn't.
SR::SRContext* TryCreateSRContextSEH(bool* dll_failure)
{
    *dll_failure = false;
    __try {
        return SR::SRContext::create();
    } __except (
        (GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
         GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND))
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH
    ) {
        *dll_failure = true;
        return nullptr;
    }
}

}  // namespace

namespace vrto3d {

bool LeiaSrPresenter::CreateSwapChain(Dx11Renderer& renderer)
{
    ID3D11Device* dev = renderer.Device();

    ComPtr<IDXGIDevice> dxgi_dev;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi_dev)))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_dev->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

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
        LOG() << "LeiaSrPresenter: CreateSwapChainForHwnd failed hr=0x" << std::hex << hr;
        return false;
    }

    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
    if (FAILED(dev->CreateRenderTargetView(bb.Get(), nullptr, &swapchain_rtv_))) return false;

    swap_width_  = window_->Width();
    swap_height_ = window_->Height();
    LOG() << "LeiaSrPresenter: swapchain " << swap_width_ << "x" << swap_height_;
    return true;
}


bool LeiaSrPresenter::Init(Dx11Renderer& renderer,
                            const StereoDisplayDriverConfiguration& cfg,
                            const FocusContext& focus)
{
    renderer_      = &renderer;
    eye_swap_      = cfg.eye_swap;
    auto_focus_    = cfg.auto_focus;
    focus_         = focus;

    // LeiaSR weaver targets a single SR display — never spans two monitors.
    platform::MonitorInfo primary{}, secondary{};
    if (!platform::ResolveTargetMonitors(cfg.display_index, false, primary, secondary)) {
        LOG() << "LeiaSrPresenter::Init: ResolveTargetMonitors failed";
        return false;
    }

    window_stop_.store(false);
    window_ready_.store(false);
    window_failed_.store(false);
    window_thread_ = std::thread(&LeiaSrPresenter::WindowThreadLoop, this, &renderer,
                                  primary, secondary);

    for (int i = 0; i < 500; ++i) {
        if (window_ready_.load() || window_failed_.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (window_failed_.load() || !window_ready_.load()) {
        LOG() << "LeiaSrPresenter::Init: window thread failed to come up";
        Shutdown();
        return false;
    }

    LOG() << "LeiaSrPresenter: ready, target_monitor=" << primary.device_name
          << " " << (window_ ? window_->Width() : 0) << "x" << (window_ ? window_->Height() : 0);

    focus_stop_.store(false);
    focus_thread_ = std::thread(&LeiaSrPresenter::FocusThreadLoop, this);
    return true;
}


void LeiaSrPresenter::WindowThreadLoop(Dx11Renderer* renderer,
                                        platform::MonitorInfo primary,
                                        platform::MonitorInfo secondary)
{
    window_ = platform::CreatePresentWindow(
        primary,
        (secondary.width > 0 ? &secondary : nullptr),
        0,                  // no vd_fsbs override for LeiaSR
        "VRto3D-LeiaSR");
    if (!window_) {
        LOG() << "LeiaSrPresenter: CreatePresentWindow failed";
        window_failed_.store(true);
        return;
    }

    if (!CreateSwapChain(*renderer)) {
        LOG() << "LeiaSrPresenter: swapchain init failed";
        window_failed_.store(true);
        window_.reset();
        return;
    }

    // Initial black so DWM has something to composite.
    {
        ID3D11DeviceContext* ctx = renderer->Context();
        const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ctx->ClearRenderTargetView(swapchain_rtv_.Get(), clear);
        swapchain_->Present(0, 0);
    }

    // Build SR context + weaver. Block briefly waiting for SRService.
    HWND hwnd = static_cast<HWND>(window_->NativeHandle());
    {
        const ULONGLONG t0 = GetTickCount64();
        const ULONGLONG max_wait_ms = 5000;
        bool dll_missing = false;
        while (!sr_context_) {
            try {
                sr_context_ = TryCreateSRContextSEH(&dll_missing);
                if (dll_missing) break;       // delay-load failure — no point retrying
                if (sr_context_) break;       // success
                // Shouldn't reach here without exception, but bail out if we do.
                break;
            } catch (const SR::ServerNotAvailableException&) {
                if (GetTickCount64() - t0 > max_wait_ms) break;
                Sleep(100);
            } catch (...) {
                break;
            }
        }
        if (dll_missing) {
            LOG() << "LeiaSrPresenter: LeiaSR runtime DLL(s) not resolvable. "
                  << "Install the LeiaSR / SR Platform runtime, or pick a different output_mode.";
            window_failed_.store(true);
            window_.reset();
            return;
        }
        if (!sr_context_) {
            LOG() << "LeiaSrPresenter: SRContext::create failed (SRService not running?)";
            window_failed_.store(true);
            window_.reset();
            return;
        }

        WeaverErrorCode wec = SR::CreateDX11Weaver(sr_context_, renderer->Context(), hwnd, &sr_weaver_);
        if (wec != WeaverErrorCode::WeaverSuccess || !sr_weaver_) {
            LOG() << "LeiaSrPresenter: CreateDX11Weaver failed code=" << static_cast<int>(wec);
            window_failed_.store(true);
            window_.reset();
            return;
        }
        // Configure weaver. Input is sRGB R8G8B8A8 from compositor; backbuffer
        // is _UNORM (linear). Tell the weaver to convert sRGB→Linear on read
        // and Linear→sRGB on write so colors come out right.
        sr_weaver_->setShaderSRGBConversion(true, true);
        sr_weaver_->setLatencyInFrames(1);
        sr_weaver_->setContext(renderer->Context());

        // Activate sense streams.
        sr_context_->initialize();
        sr_initialized_ = true;
        LOG() << "LeiaSrPresenter: SRContext initialized; weaver ready";
    }

    window_ready_.store(true);

    while (!window_stop_.load(std::memory_order_relaxed)) {
        renderer->WaitAndDrawPending(33);
        if (window_) window_->PollEvents();
    }

    // Tear down SR resources on this thread (they were created here).
    if (sr_weaver_) {
        sr_weaver_->destroy();
        sr_weaver_ = nullptr;
    }
    if (sr_context_) {
        delete sr_context_;
        sr_context_ = nullptr;
    }
    sr_initialized_ = false;

    input_srv_.Reset();
    swapchain_rtv_.Reset();
    swapchain_.Reset();
    window_.reset();
    LOG() << "LeiaSrPresenter: window thread exited";
}


void LeiaSrPresenter::PresentFrame(ID3D11Texture2D* sbs_input)
{
    if (!swapchain_ || !sbs_input || !sr_weaver_ || !renderer_) return;

    ID3D11Device*        dev = renderer_->Device();
    ID3D11DeviceContext* ctx = renderer_->Context();

    D3D11_TEXTURE2D_DESC td{};
    sbs_input->GetDesc(&td);

    // Rebuild SRV + retell weaver only when input changes.
    if (sbs_input != cached_input_ptr_
        || td.Width  != cached_input_w_
        || td.Height != cached_input_h_
        || td.Format != cached_input_fmt_) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format                    = td.Format;
        sv.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels       = static_cast<UINT>(-1);
        sv.Texture2D.MostDetailedMip = 0;
        ComPtr<ID3D11ShaderResourceView> new_srv;
        if (FAILED(dev->CreateShaderResourceView(sbs_input, &sv, &new_srv))) return;
        input_srv_         = new_srv;
        cached_input_ptr_  = sbs_input;
        cached_input_w_    = td.Width;
        cached_input_h_    = td.Height;
        cached_input_fmt_  = td.Format;

        // Weaver expects per-eye dimensions (input is treated as side-by-side).
        sr_weaver_->setInputViewTexture(input_srv_.Get(),
                                         static_cast<int>(td.Width / 2),
                                         static_cast<int>(td.Height),
                                         td.Format);
        LOG() << "LeiaSrPresenter: input view texture (re)bound "
              << td.Width << "x" << td.Height << " fmt=" << td.Format;
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

    sr_weaver_->weave();

    HRESULT hr = swapchain_->Present(1, 0);
    if (FAILED(hr)) {
        static std::atomic<bool> logged{false};
        bool e = false;
        if (logged.compare_exchange_strong(e, true)) {
            LOG() << "LeiaSrPresenter: Present failed hr=0x" << std::hex << hr;
        }
    }
}


void LeiaSrPresenter::FocusThreadLoop()
{
    using namespace std::chrono_literals;
    bool was_on_top = false;
    int  reassert_counter = 0;
    uint32_t last_auto_focused_pid = 0;
    uint32_t last_ue3d_focused_pid = 0;

    while (!focus_stop_.load(std::memory_order_relaxed)) {
        if (!window_) break;

        const bool is_on_top   = focus_.is_on_top   && focus_.is_on_top->load();
        const bool man_on_top  = focus_.man_on_top  && focus_.man_on_top->load();
        const bool ue3d_on_top = focus_.ue3d_on_top && focus_.ue3d_on_top->load();
        const uint32_t pid     = focus_.app_pid ? focus_.app_pid->load() : 0;

        // LeiaSR runs on a single SR display — no multi-display nudge.

        const bool app_running = platform::IsProcessRunning(pid);
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
            if (focus_.is_on_top)  focus_.is_on_top->store(true);
            if (focus_.man_on_top) focus_.man_on_top->store(true);
            last_auto_focused_pid = pid;
            want_on_top = true;
        } else if (ue3d_on_top && pid != 0 && pid != last_ue3d_focused_pid) {
            last_ue3d_focused_pid = pid;
            want_on_top = true;
        }

        if (want_on_top != was_on_top) {
            if (want_on_top) window_->BringToTop();
            else             window_->ReleaseTopmost();
            was_on_top = want_on_top;
            reassert_counter = 0;
        } else if (want_on_top && ++reassert_counter >= 20) {
            reassert_counter = 0;
            window_->BringToTop();
        }

        std::this_thread::sleep_for(50ms);
    }
}


void LeiaSrPresenter::Shutdown()
{
    if (!window_thread_.joinable() && !window_) return;
    LOG() << "LeiaSrPresenter: Shutdown called";

    focus_stop_.store(true);
    if (focus_thread_.joinable()) focus_thread_.join();

    window_stop_.store(true);
    if (window_thread_.joinable()) window_thread_.join();

    renderer_ = nullptr;
}

}  // namespace vrto3d

#endif  // _WIN32
