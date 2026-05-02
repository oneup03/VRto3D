/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <cstdint>

#include <wrl/client.h>
#include <d3d11.h>

class StereoDisplayComponent;

namespace vrto3d {

// Disparity-search compute pipeline driving auto-depth.
//
// Lifetime: created once, lazy-initialized on first Run() call (so the D3D
// device is already alive). Resources keyed to a specific source SbS texture
// are rebuilt automatically whenever Run() sees a new pointer.
//
// Thread-affinity: all methods must be called on the renderer's window
// thread (the same one that owns the immediate context).
class AutoDepthAnalyzer {
public:
    AutoDepthAnalyzer()  = default;
    ~AutoDepthAnalyzer() = default;

    // Stash the device. Resources are not built until the first Run().
    void Init(ID3D11Device* device);

    // Dispatch the disparity search on `sbs` (2W x H side-by-side, full
    // SbS dimensions in `sbs_w`/`sbs_h`), copy the histogram to a staging
    // ring slot, and read back the previous frame's slot without stalling.
    // Feeds the result into component->FeedAutoDepthSample. `frame_counter`
    // is used solely for diagnostic logging cadence.
    void Run(ID3D11DeviceContext* ctx,
             ID3D11Texture2D*     sbs,
             uint32_t             sbs_w,
             uint32_t             sbs_h,
             uint64_t             frame_counter,
             StereoDisplayComponent* component);

    // Release every D3D resource. Safe to call multiple times.
    void Shutdown();

private:
    bool EnsureResources(ID3D11Texture2D* sbs);

    Microsoft::WRL::ComPtr<ID3D11Device>              device_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader>       cs_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  sbs_srv_;
    Microsoft::WRL::ComPtr<ID3D11Buffer>              result_buf_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> result_uav_;
    Microsoft::WRL::ComPtr<ID3D11Buffer>              staging_[2];
    Microsoft::WRL::ComPtr<ID3D11Buffer>              params_cb_;

    // Source texture the SRV is bound to. When this changes (e.g. resize),
    // the SRV is rebuilt.
    ID3D11Texture2D* srv_source_ = nullptr;

    uint64_t inflight_frame_[2] = { UINT64_MAX, UINT64_MAX };
    uint32_t ring_idx_     = 0;
    bool     init_failed_  = false;
};

} // namespace vrto3d
