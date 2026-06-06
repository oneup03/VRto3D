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
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include <wrl/client.h>
#include <d3d11.h>

#include "openvr_driver.h"

class Dx11Renderer;


// IVRDriverDirectModeComponent implementation. In direct mode SteamVR's
// compositor never runs its lens-distortion / panel-mask render pass — game
// eye textures flow into us through SubmitLayer as shared DXGI handles to
// textures we ourselves allocated in CreateSwapTextureSet. Present hands the
// pair off to the Dx11Renderer's window thread, which composes them into the
// existing SbS scratch buffer and feeds the rest of the pipeline (OSD,
// auto-depth, presenter, screenshots) unchanged.
class DirectModeComponent : public vr::IVRDriverDirectModeComponent {
public:
    explicit DirectModeComponent(Dx11Renderer* renderer);
    ~DirectModeComponent();

    // ----- vr::IVRDriverDirectModeComponent overrides -----

    void CreateSwapTextureSet(uint32_t unPid,
                              const SwapTextureSetDesc_t* pSwapTextureSetDesc,
                              SwapTextureSet_t* pOutSwapTextureSet) override;

    void DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) override;

    void DestroyAllSwapTextureSets(uint32_t unPid) override;

    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2],
                                    uint32_t (*pIndices)[2]) override;

    void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override;

    void Present(vr::SharedTextureHandle_t syncTexture) override;

private:
    Dx11Renderer* renderer_;

    // Each application gets three textures per eye allocated on our device
    // and shared via legacy DXGI shared handles (matches ALVR's pattern; the
    // NT-handle path is not exercised here). We map every handle back to its
    // owning TextureSet so SubmitLayer / Present can resolve the texture
    // pointer and so DestroySwapTextureSet can wipe all three handles when
    // any one of them is destroyed.
    struct TextureSet {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[3];
        HANDLE   handles[3]{};
        uint32_t pid = 0;
    };
    std::mutex sets_mutex_;
    std::map<HANDLE, std::pair<TextureSet*, int>> handle_map_;

    // Per-frame layer accumulation. Apps (especially UEVR-driven games)
    // submit multiple layers per frame — typically layer 0 is the base scene
    // and layers 1+ are HUD / overlay quads that alpha-blend on top. We keep
    // up to kMaxLayers of them and the renderer composites in order.
    static constexpr int kMaxLayers = 8;
    std::mutex          submit_mutex_;
    SubmitLayerPerEye_t submit_layers_[kMaxLayers][2]{};
    int                 submit_layer_count_       = 0;

    // SubmitLayer-call counter — logged once per change. Tracked separately
    // from submit_layer_count_ because the count we observe may exceed
    // kMaxLayers; we want to know when that happens.
    int                 last_logged_submit_count_ = -1;

    // Running count of handle-map misses (submitted hTexture not in
    // handle_map_). Logged on first miss + every 60th to detect persistence.
    uint64_t handle_miss_count_ = 0;

    // Track which submit-count we last logged per-layer dimensions for, so
    // dimension lines aren't spammed every frame.
    int last_logged_layer_dim_count_ = -1;

    // pid → executable name cache for the per-layer diagnostic log. Avoids
    // an OpenProcess call every time the layer-count changes.
    std::unordered_map<uint32_t, std::string> pid_name_cache_;
};
