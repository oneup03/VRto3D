/*
 * VRto3D Linux port — Milestone 0 probe.
 *
 * Logging IVRDriverDirectModeComponent: answers whether the Linux
 * vrcompositor drives driver-side direct mode at all, and exercises the
 * IVRIPCResourceManagerClient dmabuf plumbing that the real port would use.
 */
#pragma once

#include <openvr_driver.h>

#include <cstdint>
#include <map>
#include <mutex>

class ProbeDirectMode : public vr::IVRDriverDirectModeComponent
{
public:
    void CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t *pSwapTextureSetDesc,
                              SwapTextureSet_t *pOutSwapTextureSet) override;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) override;
    void DestroyAllSwapTextureSets(uint32_t unPid) override;
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2],
                                    uint32_t (*pIndices)[2]) override;
    void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override;
    void Present(vr::SharedTextureHandle_t syncTexture) override;
    void PostPresent(const Throttling_t *pThrottling) override;

private:
    struct SetInfo
    {
        vr::SharedTextureHandle_t handles[3];
        uint32_t pid;
        uint32_t width;
        uint32_t height;
        uint32_t format;
        uint32_t next_index;
    };

    // Validates that a shared handle can be turned into a usable dmabuf fd on
    // this system: RefResource -> ReceiveSharedFd -> vkGetMemoryFdPropertiesKHR.
    void ValidateDmabufImport(vr::SharedTextureHandle_t handle);

    std::mutex mutex_;
    // Keyed by first handle of the set; every handle also maps to its set key.
    std::map<vr::SharedTextureHandle_t, SetInfo> sets_;
    std::map<vr::SharedTextureHandle_t, vr::SharedTextureHandle_t> handle_to_set_;

    uint64_t submit_count_ = 0;
    uint64_t present_count_ = 0;
    double last_present_sec_ = 0.0;
    bool dmabuf_validated_ = false;
};
