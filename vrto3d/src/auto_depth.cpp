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
#include "auto_depth.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstring>

#include <d3dcompiler.h>

#include "vrto3dlib/debug_log.hpp"

#include "hmd_device_driver.h"  // StereoDisplayComponent::FeedAutoDepthSample


// Disparity histogram — 128 buckets covering up to 512px (stride=4).
// Layout: [0..127] = bucket counts, [128] = total committed matches.
#define NUM_BUCKETS 128

namespace {

// Quarter-res block-matched left/right disparity search. Each thread picks a
// left-eye sample, sweeps `search_radius` candidate offsets in the right eye,
// records the best (lowest SAD) as that thread's disparity, and increments a
// bucket in the global disparity histogram. The CPU then walks the histogram
// for a percentile/peak-shape result.
constexpr const char* kAutoDepthCS = R"HLSL(
Texture2D<float4>      g_sbs    : register(t0);
RWByteAddressBuffer    g_result : register(u0);

cbuffer Params : register(b0)
{
    uint  sbs_w;
    uint  sbs_h;
    uint  search_radius;  // in source pixels
    uint  stride;         // 4 = quarter-res
};

#define NUM_BUCKETS 128

// Block size for SAD matching. A 5x1 horizontal block is enough to make random
// single-pixel color collisions vanishingly rare, while staying cheap inside
// the inner search loop.
#define BLOCK_HALF 2

float3 LoadL(uint x, uint y) { return g_sbs.Load(int3(x, y, 0)).rgb; }
float3 LoadR(uint rx, uint y) { return g_sbs.Load(int3(rx, y, 0)).rgb; }

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID, uint GI : SV_GroupIndex)
{
    uint eye_w = sbs_w / 2;
    uint x = DTid.x * stride;
    uint y = DTid.y * stride;

    // Inner ROI: skip the periphery where SteamVR's hidden-area mask blacks
    // out the lens corners. Black-vs-black matches at the eye edges
    // otherwise dominate the disparity histogram. Keep the central ~84% wide
    // by ~90% tall window (per eye).
    uint x_lo = eye_w * 8u / 100u;
    uint x_hi = eye_w * 92u / 100u;
    uint y_lo = sbs_h * 5u / 100u;
    uint y_hi = sbs_h * 95u / 100u;

    if (x >= x_lo && x < x_hi && y >= y_lo && y < y_hi)
    {
        // Local gradient — skip textureless regions.
        float3 L  = LoadL(x, y);
        float3 Lr = LoadL(min(x + stride, eye_w - 1), y);
        float grad = abs(L.r - Lr.r) + abs(L.g - Lr.g) + abs(L.b - Lr.b);
        // Brightness floor — black hidden-area-mask pixels that survive the
        // ROI gate (anti-aliased mask edges) are still ambiguous matches.
        float Llum = L.r + L.g + L.b;
        if (grad > 0.04 && Llum > 0.05)
        {
            // Cache the left block once.
            float3 Lb[2 * BLOCK_HALF + 1];
            [unroll] for (int i = -BLOCK_HALF; i <= BLOCK_HALF; ++i)
                Lb[i + BLOCK_HALF] = LoadL(x + i, y);

            // Search positive disparity: right-eye match is to the LEFT of
            // the left-eye column. d=0 == infinity, growing d == closer.
            uint max_d = min(search_radius, x - BLOCK_HALF);
            float bestCost   = 1e9;
            float secondBest = 1e9;
            uint  bestD      = 0;
            for (uint d = 0; d < max_d; d += stride)
            {
                uint rx = eye_w + x - d;
                float cost = 0.0;
                [unroll] for (int j = -BLOCK_HALF; j <= BLOCK_HALF; ++j)
                {
                    float3 R = LoadR(rx + j, y);
                    float3 dC = abs(Lb[j + BLOCK_HALF] - R);
                    cost += dC.r + dC.g + dC.b;
                }
                if (cost < bestCost)       { secondBest = bestCost; bestCost = cost; bestD = d; }
                else if (cost < secondBest){ secondBest = cost; }
            }

            // Lowe-style uniqueness test: only trust the match if it's
            // meaningfully better than the runner-up (rejects ambiguous /
            // repeating-pattern regions where a "best" disparity is random).
            // Also require an absolute quality floor.
            const float maxAbsCost = 0.30 * float(2 * BLOCK_HALF + 1); // ~0.3 per pixel * block size
            if (bestCost < maxAbsCost && bestCost * 1.4 < secondBest)
            {
                // Bucket index: each bucket spans `stride` source pixels.
                uint bucket = bestD / stride;
                if (bucket >= NUM_BUCKETS) bucket = NUM_BUCKETS - 1;
                uint orig0;
                g_result.InterlockedAdd(bucket * 4, 1, orig0);
                g_result.InterlockedAdd(NUM_BUCKETS * 4, 1, orig0);
            }
        }
    }
}
)HLSL";

struct AutoDepthParams {
    uint32_t sbs_w;
    uint32_t sbs_h;
    uint32_t search_radius;
    uint32_t stride;
};

} // namespace


namespace vrto3d {

void AutoDepthAnalyzer::Init(ID3D11Device* device)
{
    device_ = device;
}


bool AutoDepthAnalyzer::EnsureResources(ID3D11Texture2D* sbs)
{
    if (init_failed_) return false;
    if (!device_ || !sbs) return false;

    // Rebuild the SRV whenever the source texture changes (e.g. window
    // resize re-creates `out_sbs_` upstream).
    if (sbs != srv_source_) {
        sbs_srv_.Reset();
        srv_source_ = sbs;
    }

    if (!sbs_srv_) {
        D3D11_TEXTURE2D_DESC td{};
        sbs->GetDesc(&td);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                    = td.Format;
        sd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels       = 1;
        sd.Texture2D.MostDetailedMip = 0;
        HRESULT hr = device_->CreateShaderResourceView(sbs, &sd, &sbs_srv_);
        if (FAILED(hr)) {
            LOG() << "AutoDepth: CreateShaderResourceView hr=0x" << std::hex << hr;
            init_failed_ = true;
            return false;
        }
    }

    if (cs_ && result_buf_ && result_uav_ && staging_[0] && staging_[1] && params_cb_) {
        return sbs_srv_ != nullptr;
    }

    // Compile the compute shader.
    if (!cs_) {
        Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
        HRESULT hr = D3DCompile(kAutoDepthCS, std::strlen(kAutoDepthCS),
                                nullptr, nullptr, nullptr,
                                "CSMain", "cs_5_0", 0, 0, &blob, &err);
        if (FAILED(hr)) {
            if (err) LOG() << "AutoDepth: D3DCompile error: " << reinterpret_cast<const char*>(err->GetBufferPointer());
            else      LOG() << "AutoDepth: D3DCompile failed hr=0x" << std::hex << hr;
            init_failed_ = true;
            return false;
        }
        hr = device_->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                          nullptr, &cs_);
        if (FAILED(hr)) {
            LOG() << "AutoDepth: CreateComputeShader hr=0x" << std::hex << hr;
            init_failed_ = true;
            return false;
        }
    }

    constexpr UINT kResultUints = NUM_BUCKETS + 1;
    constexpr UINT kResultBytes = kResultUints * 4;
    if (!result_buf_) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = kResultBytes;
        bd.Usage          = D3D11_USAGE_DEFAULT;
        bd.BindFlags      = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags      = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        bd.StructureByteStride = 0;
        HRESULT hr = device_->CreateBuffer(&bd, nullptr, &result_buf_);
        if (FAILED(hr)) {
            LOG() << "AutoDepth: CreateBuffer(result) hr=0x" << std::hex << hr;
            init_failed_ = true;
            return false;
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavd{};
        uavd.Format              = DXGI_FORMAT_R32_TYPELESS;
        uavd.ViewDimension       = D3D11_UAV_DIMENSION_BUFFER;
        uavd.Buffer.FirstElement = 0;
        uavd.Buffer.NumElements  = kResultUints;
        uavd.Buffer.Flags        = D3D11_BUFFER_UAV_FLAG_RAW;
        hr = device_->CreateUnorderedAccessView(result_buf_.Get(), &uavd, &result_uav_);
        if (FAILED(hr)) {
            LOG() << "AutoDepth: CreateUAV hr=0x" << std::hex << hr;
            init_failed_ = true;
            return false;
        }
    }

    for (int i = 0; i < 2; ++i) {
        if (staging_[i]) continue;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = kResultBytes;
        bd.Usage          = D3D11_USAGE_STAGING;
        bd.BindFlags      = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bd.MiscFlags      = 0;
        HRESULT hr = device_->CreateBuffer(&bd, nullptr, &staging_[i]);
        if (FAILED(hr)) {
            LOG() << "AutoDepth: CreateBuffer(staging[" << i << "]) hr=0x" << std::hex << hr;
            init_failed_ = true;
            return false;
        }
    }

    if (!params_cb_) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = sizeof(AutoDepthParams);
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = device_->CreateBuffer(&bd, nullptr, &params_cb_);
        if (FAILED(hr)) {
            LOG() << "AutoDepth: CreateBuffer(params) hr=0x" << std::hex << hr;
            init_failed_ = true;
            return false;
        }
    }

    return sbs_srv_ != nullptr;
}


void AutoDepthAnalyzer::Run(ID3D11DeviceContext*    ctx,
                            ID3D11Texture2D*        sbs,
                            uint32_t                sbs_w,
                            uint32_t                sbs_h,
                            uint64_t                frame_counter,
                            StereoDisplayComponent* component)
{
    if (!ctx || !sbs || !component) return;
    if (sbs_w == 0 || sbs_h == 0) return;
    if (!EnsureResources(sbs)) return;

    constexpr uint32_t kStride = 4;                   // quarter-res
    const uint32_t eye_w = sbs_w / 2;
    const uint32_t search_radius = (std::max)(eye_w / 4u, 16u);

    // Update params CB.
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(params_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        AutoDepthParams* p = reinterpret_cast<AutoDepthParams*>(m.pData);
        p->sbs_w         = sbs_w;
        p->sbs_h         = sbs_h;
        p->search_radius = search_radius;
        p->stride        = kStride;
        ctx->Unmap(params_cb_.Get(), 0);
    }

    // Bind + clear + dispatch.
    const UINT clear_val[4] = {0, 0, 0, 0};
    ctx->ClearUnorderedAccessViewUint(result_uav_.Get(), clear_val);

    ID3D11ShaderResourceView*  srvs[1] = { sbs_srv_.Get() };
    ID3D11UnorderedAccessView* uavs[1] = { result_uav_.Get() };
    ID3D11Buffer*              cbs[1]  = { params_cb_.Get() };

    ctx->CSSetShader(cs_.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx->CSSetConstantBuffers(0, 1, cbs);

    const uint32_t tx = (eye_w / kStride + 7) / 8;
    const uint32_t ty = (sbs_h / kStride + 7) / 8;
    ctx->Dispatch((std::max)(tx, 1u), (std::max)(ty, 1u), 1);

    // Unbind to avoid hazards with the OSD composite / presenter that follow.
    ID3D11ShaderResourceView*  null_srv[1] = { nullptr };
    ID3D11UnorderedAccessView* null_uav[1] = { nullptr };
    ctx->CSSetShaderResources(0, 1, null_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);

    // Copy result to this frame's staging slot.
    const uint32_t cur   = ring_idx_;
    const uint32_t other = ring_idx_ ^ 1u;
    ctx->CopyResource(staging_[cur].Get(), result_buf_.Get());
    inflight_frame_[cur] = frame_counter;

    // Try to read the previous frame's result without stalling.
    if (inflight_frame_[other] != UINT64_MAX) {
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = ctx->Map(staging_[other].Get(), 0,
                              D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (SUCCEEDED(hr)) {
            uint32_t hist[NUM_BUCKETS + 1] = {};
            if (m.pData) {
                std::memcpy(hist, m.pData, sizeof(hist));
            }
            ctx->Unmap(staging_[other].Get(), 0);
            inflight_frame_[other] = UINT64_MAX;

            const uint32_t match_count = hist[NUM_BUCKETS];
            uint32_t max_disp   = 0;
            uint32_t top_bucket = 0;
            // Compute the structural-residue "tail floor": average count
            // across the top 12 buckets. Loop-boundary residue piles ~30-50
            // hits/bucket here every frame regardless of scene; a real near-
            // object peak will tower well above this floor.
            uint32_t tail_sum = 0;
            for (int b = NUM_BUCKETS - 12; b < NUM_BUCKETS; ++b) tail_sum += hist[b];
            const uint32_t tail_floor = tail_sum / 12u;

            // 3-bucket window must clear several thresholds simultaneously:
            //   (a) absolute floor (150 hits)
            //   (b) ~3.3% of total
            //   (c) 5x mean per bucket
            //   (d) 4x the tail-floor window (rejects flat boundary residue
            //       while still allowing genuine high-disparity clusters
            //       that produce a clear peak above the residue baseline)
            const uint32_t mean_per_bucket = match_count / NUM_BUCKETS;
            const uint32_t min_window = (std::max<uint32_t>)({
                150u,
                match_count / 30u,
                3u * 5u * mean_per_bucket,
                4u * 3u * tail_floor});
            for (int b = NUM_BUCKETS - 2; b >= 1; --b) {
                const uint32_t window = hist[b - 1] + hist[b] + hist[b + 1];
                if (window >= min_window) {
                    top_bucket = static_cast<uint32_t>(b);
                    max_disp   = top_bucket * kStride;
                    break;
                }
            }

            component->FeedAutoDepthSample(max_disp, eye_w);
        }
        // DXGI_ERROR_WAS_STILL_DRAWING -> just try again next frame.
    }

    ring_idx_ ^= 1u;
}


void AutoDepthAnalyzer::Shutdown()
{
    cs_.Reset();
    sbs_srv_.Reset();
    result_uav_.Reset();
    result_buf_.Reset();
    staging_[0].Reset();
    staging_[1].Reset();
    params_cb_.Reset();
    device_.Reset();
    srv_source_       = nullptr;
    inflight_frame_[0] = inflight_frame_[1] = UINT64_MAX;
    ring_idx_         = 0;
    init_failed_      = false;
}

} // namespace vrto3d
