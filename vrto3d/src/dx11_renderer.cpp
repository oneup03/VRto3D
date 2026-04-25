/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "dx11_renderer.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include "vrto3dlib/debug_log.hpp"


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
    d.MiscFlags          = 0;

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

    const uint64_t c = frame_counter_.load(std::memory_order_relaxed);
    if (c <= 5 || (c % 600 == 0)) {
        LOG() << "Dx11Renderer: frame=" << c
              << " incoming=" << incoming_desc.Width << "x" << incoming_desc.Height
              << " fmt=" << incoming_desc.Format
              << " mutex=" << (acquired ? "acquired" : (mutex ? "skip" : "none"));
    }
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

    if (presenter_) {
        presenter_->Shutdown();
        presenter_.reset();
    }
    shared_texture_cache_.clear();
    out_sbs_.Reset();
    context_.Reset();
    device_.Reset();
    adapter_.Reset();
    initialized_ = false;
}
