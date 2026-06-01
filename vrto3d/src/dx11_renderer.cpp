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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <chrono>

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

#include "vrto3dlib/debug_log.hpp"

#include "auto_depth.h"
#include "hmd_device_driver.h"           // StereoDisplayComponent::GetConfig()
#include "osd/osd_renderer.h"
#include "osd/osd_menu.h"
#include "screenshot.h"


Dx11Renderer::Dx11Renderer()
{
    LARGE_INTEGER f{};
    if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) {
        qpc_freq_sec_ = 1.0 / static_cast<double>(f.QuadPart);
    }
}

Dx11Renderer::~Dx11Renderer() { Shutdown(); }


namespace {

// Fullscreen-triangle vertex shader. The pixel shader samples its SRV at the
// computed UV and outputs straight-alpha BGRA. Blend state is configured for
// SRC_ALPHA / INV_SRC_ALPHA on the renderer side, so transparent pixels in
// the overlay leave the underlying SbS pixels intact.
const char* kCompositeVsHlsl = R"hlsl(
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

const char* kCompositePsHlsl = R"hlsl(
Texture2D    layer_tex : register(t0);
SamplerState layer_smp : register(s0);
struct VsOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
// Sampler auto-decodes the source's sRGB texels to linear; out_sbs_ is
// UNORM-typed (no encode on write), so we re-apply the sRGB curve here to
// keep the byte convention consistent with layer 0's CopySubresourceRegion.
// Piecewise sRGB encode rather than pow(1/2.2) so dark-near-zero pixels stay
// on the linear portion of the curve.
float3 linear_to_srgb(float3 c) {
    float3 lo = 12.92f * c;
    float3 hi = 1.055f * pow(max(c, 0.0f), 1.0f / 2.4f) - 0.055f;
    // step(edge, x) = (x >= edge) ? 1 : 0 — picks hi when c >= threshold.
    float3 mask = step(0.0031308f, c);
    return lerp(lo, hi, mask);
}
float4 main(VsOut i) : SV_Target {
    float4 c = layer_tex.Sample(layer_smp, i.uv);
    c.rgb = linear_to_srgb(c.rgb);
    return c;
}
)hlsl";

bool CompileShaderBlob(const char* src, const char* entry, const char* profile,
                       Microsoft::WRL::ComPtr<ID3DBlob>& out_blob) {
    Microsoft::WRL::ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
                            entry, profile, 0, 0, &out_blob, &err);
    if (FAILED(hr)) {
        LOG() << "Dx11Renderer: composite shader compile (" << entry << "/" << profile
              << ") failed hr=0x" << std::hex << hr;
        if (err) LOG() << " err: " << static_cast<const char*>(err->GetBufferPointer());
        return false;
    }
    return true;
}

}  // namespace


bool Dx11Renderer::Init(LUID adapter_luid,
                        const StereoDisplayDriverConfiguration& cfg,
                        const vrto3d::FocusContext& focus)
{
    if (!platform::CreateD3D11Device(adapter_luid, device_, context_, adapter_)) {
        LOG() << "Dx11Renderer: CreateD3D11Device failed";
        return false;
    }

    // Cap GPU frame latency to 1 so Present blocks the CPU as soon as the
    // GPU falls a single frame behind. The default is 3, which lets us
    // happily queue 3 frames of work ahead — fine when frame cost is
    // uniform, but in direct-mode multi-layer composite the cost varies
    // with content and the queue piles up over a session. Without this
    // clamp the frame-interval histogram drifts from a tight 8ms cluster
    // to a peak at 20-26ms, which the eye reads as "vibrating back."
    {
        Microsoft::WRL::ComPtr<IDXGIDevice1> dxgi_dev1;
        if (SUCCEEDED(device_.As(&dxgi_dev1)) && dxgi_dev1) {
            HRESULT hr = dxgi_dev1->SetMaximumFrameLatency(1);
            if (FAILED(hr)) {
                LOG() << "Dx11Renderer: SetMaximumFrameLatency(1) hr=0x"
                      << std::hex << hr;
            }
        }
    }

    DXGI_ADAPTER_DESC1 desc{};
    if (adapter_) adapter_->GetDesc1(&desc);
    char narrow[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, narrow, sizeof(narrow), nullptr, nullptr);
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

    // Build the multi-layer composite pipeline. If this fails we still run —
    // overlay layers just won't blend (only layer 0's CopySubresourceRegion
    // will produce visible output).
    do {
        Microsoft::WRL::ComPtr<ID3DBlob> vsb, psb;
        if (!CompileShaderBlob(kCompositeVsHlsl, "main", "vs_5_0", vsb)) break;
        if (!CompileShaderBlob(kCompositePsHlsl, "main", "ps_5_0", psb)) break;
        if (FAILED(device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(),
                                               nullptr, &composite_vs_))) break;
        if (FAILED(device_->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(),
                                               nullptr, &composite_ps_))) break;

        D3D11_SAMPLER_DESC sd{};
        sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD         = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateSamplerState(&sd, &composite_sampler_))) break;

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable    = TRUE;
        bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device_->CreateBlendState(&bd, &composite_blend_))) break;

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        if (FAILED(device_->CreateRasterizerState(&rd, &composite_raster_))) break;

        composite_pipeline_ready_ = true;
    } while (false);

    if (!composite_pipeline_ready_) {
        LOG() << "Dx11Renderer: composite pipeline init failed — overlay layers will be dropped";
    }

    // Derive the per-eye frame interval from cfg.display_frequency. That
    // value has already been halved for frame-sequential output modes
    // (see hmd_device_driver.cpp Activate path), so this number is what
    // the OpenVR compositor expects as the per-eye refresh rate.
    if (cfg.display_frequency > 1.0f) {
        const double ns_per_frame = 1.0e9 / static_cast<double>(cfg.display_frequency);
        frame_interval_ns_ = std::chrono::nanoseconds(static_cast<int64_t>(ns_per_frame));
        LOG() << "Dx11Renderer: per-eye frame interval = "
              << (ns_per_frame / 1.0e6) << "ms ("
              << cfg.display_frequency << "Hz)";
    }

    // Dedicated VsyncEvent ticker. The OpenVR compositor uses VsyncEvent to
    // pace itself in direct mode; firing it from the window thread inherits
    // all our render jitter and feeds it back into the compositor's submit
    // cadence (visible as bimodal frame-interval histograms under load).
    // Running it from a high-precision timer thread gives the compositor a
    // stable per-eye tick regardless of what the GPU is doing.
    initialized_ = true;
    vsync_stop_.store(false, std::memory_order_relaxed);
    vsync_thread_ = std::thread(&Dx11Renderer::VsyncTickThread, this);
    return true;
}


void Dx11Renderer::VsyncTickThread()
{
    using clock = std::chrono::steady_clock;
    const auto interval = frame_interval_ns_;
    // High-resolution timing: bump system timer to 1ms for sleep_until accuracy.
    timeBeginPeriod(1);
    auto next = clock::now() + interval;
    while (!vsync_stop_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_until(next);
        vr::VRServerDriverHost()->VsyncEvent(0.0);
        next += interval;
        // If we've fallen behind (e.g. process throttled), don't try to
        // catch up by firing back-to-back — re-anchor to "now + interval"
        // so the compositor's clock stays smooth.
        const auto now = clock::now();
        if (next < now) next = now + interval;
    }
    timeEndPeriod(1);
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
    out_sbs_rtv_.Reset();
    composite_srv_cache_.clear();  // texture lifetimes change at this boundary
    HRESULT hr = device_->CreateTexture2D(&d, nullptr, &out_sbs_);
    if (FAILED(hr)) {
        LOG() << "Dx11Renderer: CreateTexture2D(out_sbs) failed hr=" << std::hex << hr;
        return;
    }
    sbs_width_  = incoming.Width;
    sbs_height_ = incoming.Height;
    sbs_format_ = incoming.Format;

    // Build the RTV used by the multi-layer composite path. Failure here just
    // disables overlay blending; layer 0 still flows through CopySubresourceRegion.
    if (composite_pipeline_ready_) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format        = d.Format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        HRESULT rhr = device_->CreateRenderTargetView(out_sbs_.Get(), &rd, &out_sbs_rtv_);
        if (FAILED(rhr)) {
            LOG() << "Dx11Renderer: CreateRenderTargetView(out_sbs) failed hr=0x"
                  << std::hex << rhr;
        }
    }
    LOG() << "Dx11Renderer: out_sbs_ (re)created " << d.Width << "x" << d.Height
          << " fmt=" << d.Format;
}


void Dx11Renderer::OnDirectModeFrame(const DirectModeLayerPair* layers,
                                     int layer_count,
                                     vr::SharedTextureHandle_t sync_handle)
{
    if (!initialized_ || !device_ || !context_ || !layers || layer_count <= 0) return;
    if (layer_count > kMaxLayers) layer_count = kMaxLayers;
    if (!layers[0].left || !layers[0].right) return;

    // Compose synchronously on the compositor thread (matches ALVR's
    // pattern). The AcquireSync gate forces UEVR's GPU work for both eyes
    // to be committed before we read, eliminating the read-mid-render race
    // that caused the "frame jumping back" stutter.

    // Open + cache the syncTexture (compositor reuses the handle).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sync_tex;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> sync_mutex;
    if (sync_handle) {
        auto it = shared_texture_cache_.find(sync_handle);
        if (it != shared_texture_cache_.end()) {
            sync_tex = it->second;
        } else if (platform::ImportSharedTexture(device_.Get(), sync_handle, sync_tex) && sync_tex) {
            shared_texture_cache_.emplace(sync_handle, sync_tex);
            LOG() << "Dx11Renderer: cached sync texture handle=0x" << std::hex << sync_handle;
        }
        if (sync_tex) sync_tex.As(&sync_mutex);
    }

    ID3D11Texture2D* const left  = layers[0].left.Get();
    ID3D11Texture2D* const right = layers[0].right.Get();

    D3D11_TEXTURE2D_DESC eye_desc{};
    left->GetDesc(&eye_desc);
    D3D11_TEXTURE2D_DESC right_desc{};
    right->GetDesc(&right_desc);

    // One-line-per-change diagnostic.
    if (eye_desc.Width  != last_left_w_  || eye_desc.Height  != last_left_h_  ||
        right_desc.Width != last_right_w_ || right_desc.Height != last_right_h_ ||
        eye_desc.Format != last_eye_format_) {
        LOG() << "Dx11Renderer: per-eye dims L=" << eye_desc.Width << "x" << eye_desc.Height
              << " R=" << right_desc.Width << "x" << right_desc.Height
              << " fmt=" << eye_desc.Format;
        last_left_w_     = eye_desc.Width;
        last_left_h_     = eye_desc.Height;
        last_right_w_    = right_desc.Width;
        last_right_h_    = right_desc.Height;
        last_eye_format_ = eye_desc.Format;
    }

    auto strip_srgb = [](DXGI_FORMAT f) -> DXGI_FORMAT {
        switch (f) {
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:   return DXGI_FORMAT_B8G8R8X8_UNORM;
            case DXGI_FORMAT_BC1_UNORM_SRGB:        return DXGI_FORMAT_BC1_UNORM;
            case DXGI_FORMAT_BC2_UNORM_SRGB:        return DXGI_FORMAT_BC2_UNORM;
            case DXGI_FORMAT_BC3_UNORM_SRGB:        return DXGI_FORMAT_BC3_UNORM;
            case DXGI_FORMAT_BC7_UNORM_SRGB:        return DXGI_FORMAT_BC7_UNORM;
            default: return f;
        }
    };

    // Lock the immediate context for the duration of our GPU submits so
    // the window thread doesn't issue Present-side commands concurrently.
    std::lock_guard<std::mutex> ctx_lk(context_mutex_);

    D3D11_TEXTURE2D_DESC sbs_desc = eye_desc;
    sbs_desc.Width = eye_desc.Width * 2;
    sbs_desc.Format = strip_srgb(eye_desc.Format);
    EnsureOutputTexture(sbs_desc);
    if (!out_sbs_) return;

    // Hold AcquireSync only across the read of the per-eye textures.
    // Failure (timeout) means the compositor's work isn't yet committed —
    // skip this frame rather than read stale/torn content.
    bool acquired = false;
    if (sync_mutex) {
        HRESULT ahr = sync_mutex->AcquireSync(0, 10);
        if (ahr == S_OK) {
            acquired = true;
        } else {
            static std::atomic<bool> logged{false};
            bool expected = false;
            if (logged.compare_exchange_strong(expected, true)) {
                LOG() << "Dx11Renderer: AcquireSync(0,10) returned 0x"
                      << std::hex << ahr << " — skipping frame";
            }
            return;
        }
    }

    // Layer 0 — base scene, byte-pass-through via CopySubresourceRegion.
    D3D11_BOX src_full{};
    src_full.left   = 0;
    src_full.top    = 0;
    src_full.front  = 0;
    src_full.right  = eye_desc.Width;
    src_full.bottom = eye_desc.Height;
    src_full.back   = 1;
    context_->CopySubresourceRegion(out_sbs_.Get(), 0,
                                    0, 0, 0,
                                    left,           0,
                                    &src_full);
    context_->CopySubresourceRegion(out_sbs_.Get(), 0,
                                    eye_desc.Width, 0, 0,
                                    right,          0,
                                    &src_full);

    // Layers 1+ — alpha-blended overlay via shader composite.
    if (layer_count > 1 && composite_pipeline_ready_ && out_sbs_rtv_) {
        ID3D11RenderTargetView* rtv = out_sbs_rtv_.Get();
        const float blend_factor[4] = { 1, 1, 1, 1 };

        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->OMSetBlendState(composite_blend_.Get(), blend_factor, 0xFFFFFFFF);
        context_->RSSetState(composite_raster_.Get());
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->VSSetShader(composite_vs_.Get(), nullptr, 0);
        context_->PSSetShader(composite_ps_.Get(), nullptr, 0);
        ID3D11SamplerState* samps[] = { composite_sampler_.Get() };
        context_->PSSetSamplers(0, 1, samps);

        const UINT half_w = eye_desc.Width;
        const UINT full_h = eye_desc.Height;

        auto blit_half = [&](ID3D11Texture2D* tex, UINT viewport_x) {
            if (!tex) return;
            ID3D11ShaderResourceView* srv_raw = nullptr;
            auto it = composite_srv_cache_.find(tex);
            if (it != composite_srv_cache_.end()) {
                srv_raw = it->second.Get();
            } else {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.Format                    = td.Format;
                sv.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
                sv.Texture2D.MipLevels       = 1;
                sv.Texture2D.MostDetailedMip = 0;
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                if (FAILED(device_->CreateShaderResourceView(tex, &sv, &srv)) || !srv) return;
                srv_raw = srv.Get();
                composite_srv_cache_.emplace(tex, std::move(srv));
            }
            ID3D11ShaderResourceView* srvs[] = { srv_raw };
            context_->PSSetShaderResources(0, 1, srvs);

            D3D11_VIEWPORT vp{};
            vp.TopLeftX = static_cast<float>(viewport_x);
            vp.TopLeftY = 0.0f;
            vp.Width    = static_cast<float>(half_w);
            vp.Height   = static_cast<float>(full_h);
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            context_->RSSetViewports(1, &vp);
            context_->Draw(3, 0);

            ID3D11ShaderResourceView* null_srv[] = { nullptr };
            context_->PSSetShaderResources(0, 1, null_srv);
        };

        for (int li = 1; li < layer_count; ++li) {
            blit_half(layers[li].left.Get(),  0);
            blit_half(layers[li].right.Get(), half_w);
        }

        ID3D11RenderTargetView* null_rtv = nullptr;
        context_->OMSetRenderTargets(1, &null_rtv, nullptr);
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

    // Drain a pending screenshot request before OSD compositing so the saved
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

    // Force compositor's writes to commit before we release the sync mutex
    // and before the window thread observes a fresh out_sbs_.
    context_->Flush();

    if (acquired) sync_mutex->ReleaseSync(0);

    // Signal the window thread that out_sbs_ holds a fresh composite so it
    // can wake from cv.wait_for and present immediately — drives the
    // window-thread pace off the compositor's actual submit rate rather
    // than an independent timer that would introduce phase-offset latency.
    {
        std::lock_guard<std::mutex> lk(composite_ready_mutex_);
        composite_ready_ = true;
    }
    composite_cv_.notify_one();
}


bool Dx11Renderer::WaitAndDrawPending(int timeout_ms)
{
    if (!initialized_ || !device_) return false;

    // Wait for the compositor thread to signal "fresh composite ready"
    // (matches the old IVRVirtualDisplay path: window thread paced by
    // compositor's submit rate, so each present happens immediately after
    // a new out_sbs_ is ready — no phase-offset latency from a separate
    // internal timer). If nothing arrives within `timeout_ms` we still
    // fall through and re-present whatever out_sbs_ holds, so the display
    // can't stall on an idle compositor.
    {
        std::unique_lock<std::mutex> lk(composite_ready_mutex_);
        composite_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                               [&]{ return composite_ready_; });
        composite_ready_ = false;
    }

    if (!out_sbs_) return false;

    // Serialize against OnDirectModeFrame's GPU work on the immediate
    // context. Lock held only across DrainPendingShot + OSD + presenter
    // — the compositor side waits at most this long, typically <1ms.
    std::lock_guard<std::mutex> ctx_lk(context_mutex_);

    // Drain any previously-queued staging readback (screenshot encode).
    DrainPendingShot();

    // If a background screenshot encode completed since our last tick,
    // surface its result toast now — done on the window thread so the
    // OSD lifetime is well-defined.
    {
        std::string toast;
        {
            std::lock_guard<std::mutex> lk(screenshot_result_mutex_);
            if (!pending_screenshot_toast_.empty()) {
                toast = std::move(pending_screenshot_toast_);
                pending_screenshot_toast_.clear();
            }
        }
        if (!toast.empty() && osd_renderer_) {
            osd_renderer_->SetText(toast);
        }
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

    // VsyncEvent is driven from a dedicated 120Hz timer thread (see
    // VsyncTickThread), not here. Firing it from the window thread inherits
    // all of our jitter, which the compositor then feeds back into its
    // submit cadence — bimodal frame intervals being the visible result.

    frame_counter_.fetch_add(1, std::memory_order_relaxed);
    LARGE_INTEGER q{};
    if (QueryPerformanceCounter(&q)) {
        last_vsync_qpc_sec_.store(static_cast<double>(q.QuadPart) * qpc_freq_sec_,
                                  std::memory_order_relaxed);
    }

    return true;
}


void Dx11Renderer::Shutdown()
{
    if (!initialized_) return;
    LOG() << "Dx11Renderer: Shutdown (frames=" << frame_counter_.load() << ")";

    // Stop the VsyncEvent ticker first so the compositor doesn't keep
    // submitting frames into a half-torn-down pipeline.
    vsync_stop_.store(true, std::memory_order_relaxed);
    if (vsync_thread_.joinable()) vsync_thread_.join();

    // Wake the window thread so it can drop out of WaitAndDrawPending.
    {
        std::lock_guard<std::mutex> lk(composite_ready_mutex_);
        composite_ready_ = true;
    }
    composite_cv_.notify_all();
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
    composite_srv_cache_.clear();
    for (auto& pl : pending_layers_) { pl.left.Reset(); pl.right.Reset(); }
    pending_layer_count_ = 0;
    pending_shot_.staging.Reset();
    pending_shot_.app_name.clear();
    has_pending_shot_ = false;
    out_sbs_rtv_.Reset();
    out_sbs_.Reset();
    composite_vs_.Reset();
    composite_ps_.Reset();
    composite_sampler_.Reset();
    composite_blend_.Reset();
    composite_raster_.Reset();
    composite_pipeline_ready_ = false;
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

    // If a previous shot is still pending, drop this request. The latest
    // pending one will complete on a later frame and the user can re-trigger
    // once it finishes. Bounded to one in-flight to keep memory simple.
    if (has_pending_shot_) {
        LOG() << "Screenshot: previous shot still draining; dropping new request";
        return;
    }

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
    // Queue the GPU->staging copy. We deliberately DO NOT Map here — that
    // would block the immediate context until the GPU caught up, and on a
    // 120Hz panel that's enough delay for SteamVR's compositor to partially
    // desync us (visible as the right eye going black and the HUD overlay
    // layer dropping for a moment when the live display resumes).
    context_->CopyResource(staging.Get(), out_sbs_.Get());

    float target_eye_aspect = 0.0f;
    if (osd_component_) {
        target_eye_aspect = osd_component_->GetConfig().aspect_ratio;
    }

    pending_shot_.staging           = std::move(staging);
    pending_shot_.app_name          = app_name;
    pending_shot_.width             = desc.Width;
    pending_shot_.height            = desc.Height;
    pending_shot_.format            = desc.Format;
    pending_shot_.target_eye_aspect = target_eye_aspect;
    pending_shot_.age_frames        = 0;
    has_pending_shot_               = true;

    // Toast is set once by the background encode thread when it completes —
    // see DrainPendingShot, which hands the "Screenshot: <name>_NNNN.png"
    // text to the window thread for OsdRenderer::SetText.
}


void Dx11Renderer::DrainPendingShot()
{
    if (!has_pending_shot_ || !pending_shot_.staging || !context_) return;
    ++pending_shot_.age_frames;

    // Give the GPU at least one frame to finish the queued CopyResource
    // before we even try to Map — there's no point spending the Map syscall
    // on a guaranteed-still-drawing texture.
    if (pending_shot_.age_frames < 2) return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context_->Map(pending_shot_.staging.Get(), 0, D3D11_MAP_READ,
                               D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        // GPU hasn't finished the CopyResource yet. Try again next frame.
        // Bail out after ~120 frames (~1 second at 120Hz) so a wedged staging
        // doesn't lock the slot indefinitely.
        if (pending_shot_.age_frames > 120) {
            LOG() << "Screenshot: staging never became readable after "
                  << pending_shot_.age_frames << " frames — abandoning";
            pending_shot_.staging.Reset();
            pending_shot_.app_name.clear();
            has_pending_shot_ = false;
        }
        return;
    }
    if (FAILED(hr)) {
        LOG() << "Screenshot: Map(staging,DO_NOT_WAIT) hr=0x" << std::hex << hr;
        pending_shot_.staging.Reset();
        pending_shot_.app_name.clear();
        has_pending_shot_ = false;
        return;
    }

    const UINT  row_pitch = mapped.RowPitch;
    const UINT  width     = pending_shot_.width;
    const UINT  height    = pending_shot_.height;
    const DXGI_FORMAT fmt = pending_shot_.format;
    const float target_eye_aspect = pending_shot_.target_eye_aspect;
    const std::string app_name = std::move(pending_shot_.app_name);

    // Copy the staging bytes into a writable buffer with alpha forced to
    // 0xFF. Per-eye textures in direct mode arrive with whatever alpha the
    // game wrote (often 0 — real HMDs don't sample alpha), and WIC's
    // cubic-interpolation scaler would zero the RGB contribution of alpha-0
    // pixels during aspect-ratio scaling. Forcing alpha=0xFF preserves RGB.
    std::vector<BYTE> opaque_buf(static_cast<size_t>(row_pitch) * height);
    {
        const BYTE* src_row = static_cast<const BYTE*>(mapped.pData);
        BYTE*       dst_row = opaque_buf.data();
        for (UINT y = 0; y < height; ++y) {
            memcpy(dst_row, src_row, row_pitch);
            for (UINT x = 0; x < width; ++x) {
                dst_row[x * 4 + 3] = 0xFF;
            }
            src_row += row_pitch;
            dst_row += row_pitch;
        }
    }
    context_->Unmap(pending_shot_.staging.Get(), 0);

    pending_shot_.staging.Reset();
    has_pending_shot_ = false;

    std::thread([this, app_name, width, height, fmt, row_pitch, target_eye_aspect,
                 buf = std::move(opaque_buf)]() mutable {
        // SaveStereoPair logs its own "Screenshot: saved index..." line.
        auto r = vrto3d::screenshot::SaveStereoPair(app_name,
                                                    width, height,
                                                    fmt,
                                                    buf.data(), row_pitch,
                                                    target_eye_aspect);
        // Hand the result toast text back to the window thread to display
        // via SetText. Doing it here on the background thread would race
        // OSD lifetime at driver shutdown; the window thread picks this up
        // on its next WaitAndDrawPending tick under the mutex.
        char msg[256];
        if (r.ok) {
            _snprintf_s(msg, _TRUNCATE,
                        "Screenshot: %s_%04d.png",
                        app_name.empty() ? "vrto3d" : app_name.c_str(), r.index);
        } else {
            _snprintf_s(msg, _TRUNCATE, "Screenshot failed (see log)");
        }
        std::lock_guard<std::mutex> lk(screenshot_result_mutex_);
        pending_screenshot_toast_ = msg;
    }).detach();
}

