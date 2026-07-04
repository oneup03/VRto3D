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

// Linux twin of Dx11Renderer. The Linux compositor pre-composites all apps and
// overlays and submits a single layer pair per frame (M0 finding — see
// vrto3d/probe/RESULTS.md), so unlike the Windows path there is no multi-layer
// composite: import the compositor's shared images once, per frame blit L/R
// into out_sbs_, run the OSD, then repack into the presenter's swapchain.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <vulkan/vulkan.h>

#include "openvr_driver.h"

#include "focus_context.h"
#include "vk/vk_context.h"
#include "vrto3dlib/stereo_config.h"

class StereoDisplayComponent;
namespace vrto3d {
class IVkPresenter;
namespace osd {
class OsdRenderer;
struct MenuCallbacks;
} // namespace osd
} // namespace vrto3d

class VkRenderer {
public:
    VkRenderer();
    ~VkRenderer();

    bool Init(const StereoDisplayDriverConfiguration& cfg, const vrto3d::FocusContext& focus);
    void Shutdown();

    // Per-eye view of an imported compositor swap texture. `image`/`view` are
    // owned by DirectModeComponentVk's handle map and stay valid until
    // DestroySwapTextureSet / DestroyAllSwapTextureSets.
    struct EyeLayer {
        VkImage             image = VK_NULL_HANDLE;
        VkImageView         view = VK_NULL_HANDLE;
        uint32_t            width = 0;
        uint32_t            height = 0;
        vr::VRTextureBounds_t bounds{0.f, 0.f, 1.f, 1.f};
    };

    // Called from IVRDriverDirectModeComponent::Present (compositor thread).
    // Snapshots the pair and wakes the present thread. Cheap — no Vulkan work
    // happens on the compositor thread (implicit dmabuf sync orders the
    // compositor's rendering against our sampling at submit time).
    void OnDirectModeFrame(const EyeLayer& left, const EyeLayer& right);

    // OSD plumbing (mirrors Dx11Renderer). `native_window` is unused on Linux
    // (mouse mapping is virtual-cursor based) but kept for signature parity.
    void ConfigureOsd(StereoDisplayComponent* component,
                      vrto3d::osd::MenuCallbacks callbacks,
                      void* native_window);
    void SetOsdHeadsetHwnd(void* native_window) {}

    void RequestScreenshot(std::string app_name);

    vrto3d::osd::OsdRenderer* Osd();
    StereoDisplayComponent*   Component() { return osd_component_; }
    vrto3d::IVkPresenter*     Presenter() { return presenter_.get(); }

    void OnAppConnect();
    void OnAppDisconnect();

    // Called by DirectModeComponentVk before it destroys imported images:
    // drops the pending frame snapshot and waits for in-flight GPU work so no
    // submitted command buffer still samples the doomed VkImages.
    void WaitIdleForTextureRelease();

    uint64_t FrameCounter() const { return frame_counter_.load(std::memory_order_relaxed); }
    double   LastVsyncQpcSec() const { return last_vsync_sec_.load(std::memory_order_relaxed); }
    bool     IsDeviceDead() const { return device_dead_.load(std::memory_order_acquire); }

    vrto3d::vk::DeviceCtx& Ctx() { return ctx_; }

private:
    void PresentThread();
    void VsyncTickThread();
    bool EnsureOutputImage(uint32_t eye_w, uint32_t eye_h);
    bool EnsureRepackPipeline();
    void RecordFrame(VkCommandBuffer cmd, const EyeLayer& left, const EyeLayer& right,
                     const struct FrameTargetBits& target);
    void MaybeSaveScreenshot(VkCommandBuffer cmd);
    void FinishScreenshot();

    vrto3d::vk::DeviceCtx ctx_;
    StereoDisplayDriverConfiguration cfg_;
    vrto3d::FocusContext focus_;

    std::unique_ptr<vrto3d::IVkPresenter> presenter_;

    // out_sbs_: 2W x H canonical side-by-side target.
    VkImage        out_sbs_ = VK_NULL_HANDLE;
    VkDeviceMemory out_sbs_mem_ = VK_NULL_HANDLE;
    VkImageView    out_sbs_view_ = VK_NULL_HANDLE;
    VkFormat       out_sbs_format_ = VK_FORMAT_UNDEFINED;
    uint32_t       sbs_width_ = 0;
    uint32_t       sbs_height_ = 0;
    bool           out_sbs_initialized_ = false;  // layout tracking

    // Repack pipeline (fullscreen triangle sampling out_sbs_ into swapchain).
    VkDescriptorSetLayout repack_dsl_ = VK_NULL_HANDLE;
    VkPipelineLayout      repack_layout_ = VK_NULL_HANDLE;
    VkPipeline            repack_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool      repack_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet       repack_set_ = VK_NULL_HANDLE;
    VkSampler             repack_sampler_ = VK_NULL_HANDLE;
    VkRenderPass          repack_render_pass_ = VK_NULL_HANDLE;  // presenter's

    // Present-thread command machinery (double-buffered).
    static constexpr int kFramesInFlight = 2;
    VkCommandPool   cmd_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_bufs_[kFramesInFlight] = {};
    VkFence         frame_fences_[kFramesInFlight] = {};
    VkSemaphore     acquire_sems_[kFramesInFlight] = {};
    VkSemaphore     render_sems_[kFramesInFlight] = {};
    int             frame_slot_ = 0;

    // Latest frame snapshot from the compositor thread.
    std::mutex              frame_mutex_;
    std::condition_variable frame_cv_;
    EyeLayer                pending_left_{};
    EyeLayer                pending_right_{};
    uint64_t                pending_seq_ = 0;
    uint64_t                consumed_seq_ = 0;

    // OSD
    StereoDisplayComponent*                  osd_component_ = nullptr;
    std::unique_ptr<vrto3d::osd::OsdRenderer> osd_renderer_;
    std::unique_ptr<vrto3d::osd::MenuCallbacks> osd_callbacks_;
    bool osd_config_pending_ = false;

    // Screenshot request (drained on present thread).
    std::mutex  shot_mutex_;
    std::string shot_app_name_;
    bool        shot_requested_ = false;
    VkBuffer       shot_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory shot_memory_ = VK_NULL_HANDLE;
    uint32_t       shot_w_ = 0, shot_h_ = 0;
    bool           shot_inflight_ = false;

    std::thread present_thread_;
    std::thread vsync_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_for_disconnect_{false};
    std::atomic<bool> device_dead_{false};

    std::atomic<uint64_t> frame_counter_{0};
    std::atomic<double>   last_vsync_sec_{0.0};
    float display_frequency_ = 60.0f;
};
