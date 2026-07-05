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

// Linux WibbleWobble output: instead of presenting to a local display, hand the
// composited side-by-side frame to a running WibbleWobbleLinux server
// (wwserver) over libwwclient, which does the frame-sequential presentation +
// shutter-glasses sync. VRto3D's repack pass renders the canonical SbS into a
// ring of exportable dmabuf images (this presenter's FrameTargets); Present()
// blits into a linear dmabuf and announces it to wwserver as WWSF_SideBySideFull.
//
// libwwclient is dlopen'd — if WibbleWobbleLinux isn't installed, Init fails
// gracefully (the driver logs and the output mode is inert).

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "presenter/vk_presenter.h"

namespace vrto3d {

class WibbleWobblePresenter final : public IVkPresenter {
public:
    ~WibbleWobblePresenter() override;

    bool Init(vrto3d::vk::DeviceCtx* ctx, const StereoDisplayDriverConfiguration& cfg) override;
    void Shutdown() override;
    bool PumpEvents() override;
    bool AcquireNext(FrameTarget* out, VkSemaphore signal_sem) override;
    bool Present(uint32_t image_index, VkSemaphore wait_sem) override;

    VkRenderPass RenderPass() const override { return render_pass_; }
    VkExtent2D   Extent() const override { return extent_; }
    VkFormat     Format() const override { return format_; }
    const char*  Name() const override { return "wibblewobble"; }

private:
    struct Frame {
        // Render target the repack pass draws the SbS into (OPTIMAL).
        VkImage        render_img = VK_NULL_HANDLE;
        VkDeviceMemory render_mem = VK_NULL_HANDLE;
        VkImageView    render_view = VK_NULL_HANDLE;
        VkFramebuffer  framebuffer = VK_NULL_HANDLE;
        // Exportable linear dmabuf handed to wwserver.
        VkImage        dmabuf_img = VK_NULL_HANDLE;
        VkDeviceMemory dmabuf_mem = VK_NULL_HANDLE;
        int            dmabuf_fd = -1;
        uint32_t       stride = 0;
        bool           dmabuf_initialized = false;  // layout tracking for blit
        VkCommandBuffer cmd = VK_NULL_HANDLE;        // blit + acquire-signal
        VkFence        fence = VK_NULL_HANDLE;
        bool           in_flight = false;
    };

    bool LoadClient();
    bool CreateFrames();
    void DestroyFrames();

    vrto3d::vk::DeviceCtx* ctx_ = nullptr;
    VkExtent2D  extent_{};
    VkFormat    format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;

    static constexpr int kRing = 3;
    std::vector<Frame> frames_;
    int next_ = 0;
    uint64_t frame_id_ = 0;

    // libwwclient (dlopen'd)
    void* lib_ = nullptr;
    struct WWClientOpaque* ww_ = nullptr;
    // resolved symbols
    void* (*p_create_)(const char*) = nullptr;
    int   (*p_running_)(void*) = nullptr;
    void  (*p_destroy_)(void*) = nullptr;
    void  (*p_setfmt_)(void*, int, uint32_t, uint32_t, uint32_t, uint64_t) = nullptr;
    int   (*p_regbuf_)(void*, int, uint32_t, uint32_t, uint64_t) = nullptr;
    int   (*p_present_)(void*, int, uint64_t) = nullptr;
    int   (*p_busy_)(void*, int) = nullptr;
};

}  // namespace vrto3d
