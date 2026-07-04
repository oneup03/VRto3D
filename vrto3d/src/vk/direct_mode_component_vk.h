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

// Linux IVRDriverDirectModeComponent. Swap textures are allocated centrally
// by vrserver via IVRIPCResourceManagerClient::NewSharedVulkanImage; the
// compositor renders into them and we import each one once as a dmabuf
// VkImage for sampling. On Linux the compositor pre-composites all layers
// and submits exactly one layer pair per frame from its own pid (see
// vrto3d/probe/RESULTS.md), so SubmitLayer/Present are much simpler than the
// Windows path — no multi-layer accumulation or pid sorting.

#include <cstdint>
#include <map>
#include <mutex>

#include <vulkan/vulkan.h>

#include "openvr_driver.h"

class VkRenderer;

class DirectModeComponentVk : public vr::IVRDriverDirectModeComponent
{
public:
    explicit DirectModeComponentVk(VkRenderer* renderer);
    ~DirectModeComponentVk();

    void CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t* pSwapTextureSetDesc,
                              SwapTextureSet_t* pOutSwapTextureSet) override;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) override;
    void DestroyAllSwapTextureSets(uint32_t unPid) override;
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2],
                                    uint32_t (*pIndices)[2]) override;
    void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override;
    void Present(vr::SharedTextureHandle_t syncTexture) override;
    void PostPresent(const Throttling_t* pThrottling) override;

private:
    struct ImportedTexture {
        vr::SharedTextureHandle_t handle = 0;
        VkImage        image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView    view = VK_NULL_HANDLE;
        uint32_t       width = 0;
        uint32_t       height = 0;
        VkFormat       format = VK_FORMAT_UNDEFINED;
        uint32_t       pid = 0;
        vr::SharedTextureHandle_t set_key = 0;  // first handle of the set
        uint32_t       set_next_index = 0;      // valid on the set_key entry
    };

    // Import `handle` as a dmabuf-backed VkImage (RefResource ->
    // ReceiveSharedFd -> vkCreateImage+import+bind+view). Returns nullptr on
    // failure (entry not added).
    ImportedTexture* ImportHandle(vr::SharedTextureHandle_t handle, uint32_t pid,
                                  uint32_t width, uint32_t height, uint32_t vk_format,
                                  vr::SharedTextureHandle_t set_key);
    void ReleaseTexture(ImportedTexture& tex);

    VkRenderer* renderer_ = nullptr;

    std::mutex mutex_;
    std::map<vr::SharedTextureHandle_t, ImportedTexture> textures_;

    // Present-time diagnostics (rate-limited logging).
    uint64_t submit_count_ = 0;
    uint64_t present_count_ = 0;
    SubmitLayerPerEye_t last_layer_[2] = {};
    bool have_layer_ = false;
};
