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

#include "dx11_renderer.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <cstdio>

#include "vrto3dlib/debug_log.hpp"

#include "auto_depth.h"
#include "hmd_device_driver.h"           // StereoDisplayComponent::GetConfig()
#include "osd/osd_renderer.h"
#include "osd/osd_menu.h"
#include "screenshot.h"


Dx11Renderer::Dx11Renderer()
{
#ifdef _WIN32
    LARGE_INTEGER f{};
    if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) {
        qpc_freq_sec_ = 1.0 / static_cast<double>(f.QuadPart);
    }
#endif
}

Dx11Renderer::~Dx11Renderer() { Shutdown(); }


bool Dx11Renderer::Init(LUID adapter_luid,
                        const StereoDisplayDriverConfiguration& cfg,
                        const vrto3d::FocusContext& focus)
{
    if (!platform::CreateD3D11Device(adapter_luid, device_, context_, adapter_)) {
        LOG() << "Dx11Renderer: CreateD3D11Device failed";
        return false;
    }

    DXGI_ADAPTER_DESC1 desc{};
    if (adapter_) adapter_->GetDesc1(&desc);
    char narrow[256] = {};
#ifdef _WIN32
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, narrow, sizeof(narrow), nullptr, nullptr);
#endif
    LOG() << "Dx11Renderer: adapter='" << narrow << "' LUID=" << desc.AdapterLuid.HighPart
          << ":" << desc.AdapterLuid.LowPart;

    presenter_ = vrto3d::MakePresenter(cfg.output_mode);
    if (!presenter_) {
        LOG() << "Dx11Renderer: MakePresenter returned null for mode=" << OutputModeToString(cfg.output_mode);
        return false;
    }
    if (!presenter_->Init(*this, cfg, focus)) {
        LOG() << "Dx11Renderer: presenter Init failed";
        return false;
    }

    initialized_ = true;
    return true;
}


void Dx11Renderer::EnsureOutputTexture(const D3D11_TEXTURE2D_DESC& incoming)
{
    if (out_sbs_ &&
        sbs_width_  == incoming.Width &&
        sbs_height_ == incoming.Height &&
        sbs_format_ == incoming.Format) {
        return;
    }

    D3D11_TEXTURE2D_DESC d{};
    d.Width              = incoming.Width;
    d.Height             = incoming.Height;
    d.MipLevels          = 1;
    d.ArraySize          = 1;
    d.Format             = incoming.Format;
    d.SampleDesc.Count   = 1;
    d.Usage              = D3D11_USAGE_DEFAULT;
    d.BindFlags          = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.CPUAccessFlags     = 0;
    // SHARED so cross-device presenters (WibbleWobble's in-process client uses
    // its own internal D3D11 device) can call OpenSharedResource on this
    // texture's GetSharedHandle() without us having to copy into a separate
    // shared staging buffer. Other presenters that use out_sbs_ on the same
    // device (Window/LeiaSR/NvStereoDX9) ignore the flag.
    d.MiscFlags          = D3D11_RESOURCE_MISC_SHARED;

    out_sbs_.Reset();
    HRESULT hr = device_->CreateTexture2D(&d, nullptr, &out_sbs_);
    if (FAILED(hr)) {
        LOG() << "Dx11Renderer: CreateTexture2D(out_sbs) failed hr=" << std::hex << hr;
        return;
    }
    sbs_width_  = incoming.Width;
    sbs_height_ = incoming.Height;
    sbs_format_ = incoming.Format;
    LOG() << "Dx11Renderer: out_sbs_ (re)created " << d.Width << "x" << d.Height
          << " fmt=" << d.Format;
}


void Dx11Renderer::OnPresent(const vr::PresentInfo_t& info)
{
    if (!initialized_) return;

    // Stash the handle and wake the window thread. Do NOT touch the immediate
    // context here — the window thread owns it (and the swapchain).
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_handle_ = info.backbufferTextureHandle;
        pending_ready_  = true;
    }
    pending_cv_.notify_one();
}


bool Dx11Renderer::WaitAndDrawPending(int timeout_ms)
{
    if (!initialized_ || !device_) return false;

    vr::SharedTextureHandle_t handle = 0;
    {
        std::unique_lock<std::mutex> lk(pending_mutex_);
        pending_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                             [&]{ return pending_ready_; });
        if (!pending_ready_) return false;
        handle = pending_handle_;
        pending_ready_ = false;
    }
    if (!handle) return false;

    // Look up cached opened texture for this handle, or open + cache.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> incoming;
    auto it = shared_texture_cache_.find(handle);
    if (it != shared_texture_cache_.end()) {
        incoming = it->second;
    } else {
        if (!platform::ImportSharedTexture(device_.Get(), handle, incoming) || !incoming) {
            return false;
        }
        shared_texture_cache_.emplace(handle, incoming);
        LOG() << "Dx11Renderer: cached new shared texture handle=0x" << std::hex << handle
              << " (cache size=" << std::dec << shared_texture_cache_.size() << ")";
    }

    D3D11_TEXTURE2D_DESC incoming_desc{};
    incoming->GetDesc(&incoming_desc);
    EnsureOutputTexture(incoming_desc);
    if (!out_sbs_) return false;

    // Acquire keyed-mutex sync before reading. The compositor releases with
    // key 0 after writing each frame; we acquire with key 0, copy, and release
    // back. Without this the compositor's GPU work may not be flushed before
    // our CopyResource reads, leading to all-zero (black) reads.
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
    HRESULT mhr = incoming.As(&mutex);
    bool   acquired = false;
    if (SUCCEEDED(mhr) && mutex) {
        HRESULT ahr = mutex->AcquireSync(0, 10);
        if (ahr == S_OK) {
            acquired = true;
        } else {
            static std::atomic<bool> logged_acq{false};
            bool e = false;
            if (logged_acq.compare_exchange_strong(e, true)) {
                LOG() << "Dx11Renderer: AcquireSync(0,10) returned 0x" << std::hex << ahr
                      << " — proceeding without sync (may yield stale/black reads)";
            }
        }
    }

    context_->CopyResource(out_sbs_.Get(), incoming.Get());

    if (acquired) {
        mutex->ReleaseSync(0);
    }

    // Auto-depth: dispatch the disparity compute pass on the freshly-copied
    // out_sbs_, before OSD composite so the analysis runs on the clean frame.
    if (osd_component_ && osd_component_->IsAutoDepthEnabled()) {
        if (!auto_depth_) {
            auto_depth_ = std::make_unique<vrto3d::AutoDepthAnalyzer>();
            auto_depth_->Init(device_.Get());
        }
        auto_depth_->Run(context_.Get(), out_sbs_.Get(),
                         sbs_width_, sbs_height_,
                         frame_counter_.load(std::memory_order_relaxed),
                         osd_component_);
    }

    // Drain a pending screenshot request *before* OSD compositing so the saved
    // image is the clean stereo frame, not one with menu/toast text baked in.
    {
        bool want = false;
        std::string name;
        {
            std::lock_guard<std::mutex> lk(screenshot_mutex_);
            if (screenshot_pending_) {
                want = true;
                name = std::move(pending_screenshot_app_);
                pending_screenshot_app_.clear();
                screenshot_pending_ = false;
            }
        }
        if (want) CaptureScreenshot(name);
    }

    // Lazy-init the OSD now that we know per-eye dimensions.
    if (!osd_initialized_ && osd_pending_callbacks_ && device_ && context_) {
        osd_renderer_ = std::make_unique<vrto3d::osd::OsdRenderer>();
        const UINT eye_w = sbs_width_ / 2;
        if (osd_renderer_->Init(device_.Get(), context_.Get(),
                                eye_w, sbs_height_,
                                osd_headset_hwnd_,
                                osd_component_,
                                std::move(*osd_pending_callbacks_))) {
            osd_initialized_ = true;
            LOG() << "Dx11Renderer: OSD initialized eye=" << eye_w << "x" << sbs_height_;
        } else {
            LOG() << "Dx11Renderer: OSD init failed";
            osd_renderer_.reset();
        }
        osd_pending_callbacks_.reset();
    }

    // Composite OSD into out_sbs_ before the presenter sees it.
    if (osd_initialized_ && osd_renderer_) {
        const UINT eye_w = sbs_width_ / 2;
        osd_renderer_->OnResize(eye_w, sbs_height_);
        osd_renderer_->RenderFrame(out_sbs_.Get());
    }

    if (presenter_) presenter_->PresentFrame(out_sbs_.Get());

    // VsyncEvent pacing signal fires after the actual display present.
    vr::VRServerDriverHost()->VsyncEvent(0.0);

    frame_counter_.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    LARGE_INTEGER q{};
    if (QueryPerformanceCounter(&q)) {
        last_vsync_qpc_sec_.store(static_cast<double>(q.QuadPart) * qpc_freq_sec_,
                                  std::memory_order_relaxed);
    }
#endif

    return true;
}


void Dx11Renderer::Shutdown()
{
    if (!initialized_) return;
    LOG() << "Dx11Renderer: Shutdown (frames=" << frame_counter_.load() << ")";

    // Wake the window thread so it can drop out of WaitAndDrawPending.
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_ready_ = true;
    }
    pending_cv_.notify_all();

    if (osd_renderer_) {
        osd_renderer_->Shutdown();
        osd_renderer_.reset();
    }
    osd_initialized_ = false;
    osd_pending_callbacks_.reset();

    if (presenter_) {
        presenter_->Shutdown();
        presenter_.reset();
    }

    if (auto_depth_) {
        auto_depth_->Shutdown();
        auto_depth_.reset();
    }

    shared_texture_cache_.clear();
    out_sbs_.Reset();
    context_.Reset();
    device_.Reset();
    adapter_.Reset();
    initialized_ = false;
}

void Dx11Renderer::ConfigureOsd(StereoDisplayComponent* component,
                                vrto3d::osd::MenuCallbacks callbacks,
                                void* headset_hwnd)
{
    osd_component_    = component;
    osd_headset_hwnd_ = headset_hwnd;
    osd_pending_callbacks_ = std::make_unique<vrto3d::osd::MenuCallbacks>(std::move(callbacks));
    // Real OsdRenderer construction happens on the window thread on the next
    // WaitAndDrawPending so all D3D11 work stays on the right thread.
}

void Dx11Renderer::SetOsdHeadsetHwnd(void* hwnd)
{
    osd_headset_hwnd_ = hwnd;
    if (osd_renderer_) osd_renderer_->SetHeadsetHwnd(hwnd);
}


void Dx11Renderer::RequestScreenshot(std::string app_name)
{
    std::lock_guard<std::mutex> lk(screenshot_mutex_);
    pending_screenshot_app_ = std::move(app_name);
    screenshot_pending_     = true;
}


void Dx11Renderer::CaptureScreenshot(const std::string& app_name)
{
    if (!device_ || !context_ || !out_sbs_) return;

    D3D11_TEXTURE2D_DESC desc{};
    out_sbs_->GetDesc(&desc);

    // Stage the texture for CPU read; everything pixel-side lives in screenshot.cpp.
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags      = 0;
    sd.MipLevels      = 1;
    sd.ArraySize      = 1;
    sd.SampleDesc     = {1, 0};

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device_->CreateTexture2D(&sd, nullptr, &staging);
    if (FAILED(hr)) {
        LOG() << "Screenshot: CreateTexture2D(staging) hr=0x" << std::hex << hr;
        return;
    }
    context_->CopyResource(staging.Get(), out_sbs_.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        LOG() << "Screenshot: Map(staging) hr=0x" << std::hex << hr;
        return;
    }

    // Per-eye target aspect ratio comes from the live config (settable by the
    // user in the OSD / profile JSON). 0 disables aspect-ratio scaling.
    float target_eye_aspect = 0.0f;
    if (osd_component_) {
        target_eye_aspect = osd_component_->GetConfig().aspect_ratio;
    }

    auto r = vrto3d::screenshot::SaveStereoPair(app_name,
                                                desc.Width, desc.Height,
                                                desc.Format,
                                                mapped.pData, mapped.RowPitch,
                                                target_eye_aspect);
    context_->Unmap(staging.Get(), 0);

    if (osd_initialized_ && osd_renderer_) {
        if (r.ok) {
            char msg[256];
            _snprintf_s(msg, _TRUNCATE,
                        "Screenshot saved: %s_%04d.png (+_crossview)",
                        app_name.empty() ? "vrto3d" : app_name.c_str(), r.index);
            osd_renderer_->SetText(msg);
        } else {
            osd_renderer_->SetText("Screenshot failed (see log)");
        }
    }
}

