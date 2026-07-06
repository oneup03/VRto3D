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
#include "presenter/wibblewobble_presenter_linux.h"

#include <dlfcn.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include "vk/vk_context.h"
#include "presenter/vk_swapchain_util.h"  // PresenterLog
#include "presenter/wwclient.h"  // WWSF_* format constants

namespace vrto3d {

WibbleWobblePresenter::~WibbleWobblePresenter()
{
    Shutdown();
}

bool WibbleWobblePresenter::LoadClient()
{
    lib_ = dlopen("libwwclient.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib_)
        lib_ = dlopen("libwwclient.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib_) {
        PresenterLog("wibblewobble: libwwclient.so not found (install WibbleWobbleLinux) — %s",
                     dlerror());
        return false;
    }
    p_create_  = (decltype(p_create_))dlsym(lib_, "WWClient_Create");
    p_running_ = (decltype(p_running_))dlsym(lib_, "WWClient_IsRunning");
    p_destroy_ = (decltype(p_destroy_))dlsym(lib_, "WWClient_Destroy");
    p_setfmt_  = (decltype(p_setfmt_))dlsym(lib_, "WWClient_SetSourceFormat");
    p_regbuf_  = (decltype(p_regbuf_))dlsym(lib_, "WWClient_RegisterBuffer");
    p_present_ = (decltype(p_present_))dlsym(lib_, "WWClient_PresentFrame");
    p_busy_    = (decltype(p_busy_))dlsym(lib_, "WWClient_BufferBusy");
    if (!p_create_ || !p_running_ || !p_destroy_ || !p_setfmt_ || !p_regbuf_ || !p_present_ ||
        !p_busy_) {
        PresenterLog("wibblewobble: libwwclient missing expected symbols");
        return false;
    }
    return true;
}

bool WibbleWobblePresenter::Init(vrto3d::vk::DeviceCtx* ctx,
                                 const StereoDisplayDriverConfiguration& cfg)
{
    ctx_ = ctx;
    // Canonical SbS extent = 2 * per-eye render size (what the compositor is
    // told to render and what out_sbs_ ends up being). If the live frame
    // differs the repack scales into this — matching wwserver's expectation of
    // a full side-by-side WWSF_SideBySideFull frame.
    extent_ = {(uint32_t)cfg.render_width * 2u, (uint32_t)cfg.render_height};
    if (extent_.width == 0 || extent_.height == 0)
        extent_ = {3840, 1080};

    if (!LoadClient())
        return false;

    // Render pass matching the swapchain presenters (loadOp DONT_CARE — the
    // repack overwrites every pixel; final layout COLOR_ATTACHMENT so we can
    // blit out of it afterwards).
    VkAttachmentDescription att{};
    att.format = format_;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    if (vkCreateRenderPass(ctx_->device, &rp, nullptr, &render_pass_) != VK_SUCCESS) {
        PresenterLog("wibblewobble: render pass creation failed");
        return false;
    }

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = ctx_->queue_family;
    vkCreateCommandPool(ctx_->device, &cpci, nullptr, &cmd_pool_);

    if (!CreateFrames())
        return false;

    ww_ = reinterpret_cast<WWClientOpaque*>(p_create_("vrto3d"));
    if (!ww_) {
        PresenterLog("wibblewobble: WWClient_Create failed");
        return false;
    }
    // XRGB8888 <- B8G8R8A8_UNORM. Full side-by-side.
    p_setfmt_(ww_, WWSF_SIDE_BY_SIDE_FULL, extent_.width, extent_.height,
              DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR);
    for (int i = 0; i < kRing; ++i) {
        const int idx = p_regbuf_(ww_, frames_[i].dmabuf_fd, frames_[i].stride, 0,
                                  (uint64_t)frames_[i].stride * extent_.height);
        if (idx != i)
            PresenterLog("wibblewobble: buffer %d registered as %d", i, idx);
    }
    PresenterLog("wibblewobble: streaming %ux%u SbS to wwserver", extent_.width, extent_.height);
    return true;
}

bool WibbleWobblePresenter::CreateFrames()
{
    frames_.resize(kRing);
    auto* get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(ctx_->device, "vkGetMemoryFdKHR");
    if (!get_fd) {
        PresenterLog("wibblewobble: vkGetMemoryFdKHR unavailable");
        return false;
    }

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmd_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    for (auto& f : frames_) {
        // ---- render target (OPTIMAL, COLOR_ATTACHMENT + TRANSFER_SRC) ----
        vrto3d::vk::Image2D render;
        if (!vrto3d::vk::CreateImage2D(*ctx_, extent_.width, extent_.height, format_,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                VK_IMAGE_TILING_OPTIMAL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                /*make_view=*/true, &render))
            return false;
        f.render_img = render.image;
        f.render_mem = render.memory;
        f.render_view = render.view;
        VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbi.renderPass = render_pass_;
        fbi.attachmentCount = 1;
        fbi.pAttachments = &f.render_view;
        fbi.width = extent_.width;
        fbi.height = extent_.height;
        fbi.layers = 1;
        if (vkCreateFramebuffer(ctx_->device, &fbi, nullptr, &f.framebuffer) != VK_SUCCESS)
            return false;

        // ---- exportable LINEAR dmabuf (TRANSFER_DST) handed to wwserver ----
        VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkImageCreateInfo dici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        dici.pNext = &ext;
        dici.imageType = VK_IMAGE_TYPE_2D;
        dici.format = format_;
        dici.extent = {extent_.width, extent_.height, 1};
        dici.mipLevels = 1;
        dici.arrayLayers = 1;
        dici.samples = VK_SAMPLE_COUNT_1_BIT;
        dici.tiling = VK_IMAGE_TILING_LINEAR;  // portable cross-process import
        dici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        dici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(ctx_->device, &dici, nullptr, &f.dmabuf_img) != VK_SUCCESS)
            return false;
        VkMemoryRequirements dreqs{};
        vkGetImageMemoryRequirements(ctx_->device, f.dmabuf_img, &dreqs);
        VkExportMemoryAllocateInfo emai{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        emai.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.image = f.dmabuf_img;
        ded.pNext = &emai;
        VkMemoryAllocateInfo dmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        dmai.pNext = &ded;
        dmai.allocationSize = dreqs.size;
        dmai.memoryTypeIndex = ctx_->FindMemoryType(dreqs.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (dmai.memoryTypeIndex == UINT32_MAX)
            dmai.memoryTypeIndex = ctx_->FindMemoryType(dreqs.memoryTypeBits, 0);
        if (vkAllocateMemory(ctx_->device, &dmai, nullptr, &f.dmabuf_mem) != VK_SUCCESS)
            return false;
        vkBindImageMemory(ctx_->device, f.dmabuf_img, f.dmabuf_mem, 0);

        VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        gfi.memory = f.dmabuf_mem;
        gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        if (get_fd(ctx_->device, &gfi, &f.dmabuf_fd) != VK_SUCCESS || f.dmabuf_fd < 0) {
            PresenterLog("wibblewobble: dmabuf export failed");
            return false;
        }
        VkImageSubresource sr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout layout{};
        vkGetImageSubresourceLayout(ctx_->device, f.dmabuf_img, &sr, &layout);
        f.stride = (uint32_t)layout.rowPitch;

        vkAllocateCommandBuffers(ctx_->device, &cbai, &f.cmd);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(ctx_->device, &fci, nullptr, &f.fence);
    }
    return true;
}

bool WibbleWobblePresenter::PumpEvents()
{
    // No window; the pipeline lives as long as wwserver is reachable. Report
    // liveness so the renderer keeps feeding (a dropped server just means
    // frames are discarded until it returns).
    return true;
}

bool WibbleWobblePresenter::AcquireNext(FrameTarget* out, VkSemaphore signal_sem)
{
    // Round-robin, skipping buffers wwserver still holds.
    int chosen = -1;
    for (int t = 0; t < kRing; ++t) {
        const int i = (next_ + t) % kRing;
        if (!p_busy_(ww_, i)) { chosen = i; break; }
    }
    if (chosen < 0)
        chosen = next_;  // all busy: reuse oldest (server will drop it)
    next_ = (chosen + 1) % kRing;
    Frame& f = frames_[chosen];

    // Wait for our own prior blit on this slot to finish, then signal the
    // renderer's acquire semaphore (our images are ready as soon as the fence
    // clears — but the binary semaphore still needs a queue signal).
    if (f.in_flight) {
        vkWaitForFences(ctx_->device, 1, &f.fence, VK_TRUE, UINT64_MAX);
        f.in_flight = false;
    }
    VkSubmitInfo sig{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sig.signalSemaphoreCount = 1;
    sig.pSignalSemaphores = &signal_sem;
    {
        std::lock_guard<std::mutex> lock(ctx_->queue_mutex);
        vkQueueSubmit(ctx_->queue, 1, &sig, VK_NULL_HANDLE);
    }

    out->image = f.render_img;
    out->view = f.render_view;
    out->framebuffer = f.framebuffer;
    out->index = (uint32_t)chosen;
    return true;
}

bool WibbleWobblePresenter::Present(uint32_t image_index, VkSemaphore wait_sem)
{
    Frame& f = frames_[image_index];

    // Blit the just-rendered SbS (TRANSFER_SRC after the render pass) into the
    // linear dmabuf, waiting on the repack's render semaphore (consumes it).
    vkResetFences(ctx_->device, 1, &f.fence);
    vkResetCommandBuffer(f.cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &begin);

    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = f.dmabuf_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = f.dmabuf_img;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_dst);

    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {extent_.width, extent_.height, 1};
    vkCmdCopyImage(f.cmd, f.render_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f.dmabuf_img,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    // Hand back to GENERAL so the importing process (wwserver) samples a
    // well-defined layout; implicit dmabuf sync orders its read after this.
    VkImageMemoryBarrier to_general{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_general.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_general.dstAccessMask = 0;
    to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_general.srcQueueFamilyIndex = ctx_->queue_family;
    to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    to_general.image = f.dmabuf_img;
    to_general.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_general);
    vkEndCommandBuffer(f.cmd);
    f.dmabuf_initialized = true;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &wait_sem;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &f.cmd;
    {
        std::lock_guard<std::mutex> lock(ctx_->queue_mutex);
        if (vkQueueSubmit(ctx_->queue, 1, &submit, f.fence) != VK_SUCCESS)
            return false;
    }
    f.in_flight = true;

    if (p_running_(ww_))
        p_present_(ww_, (int)image_index, ++frame_id_);
    return true;
}

void WibbleWobblePresenter::DestroyFrames()
{
    for (auto& f : frames_) {
        if (f.fence) vkDestroyFence(ctx_->device, f.fence, nullptr);
        if (f.framebuffer) vkDestroyFramebuffer(ctx_->device, f.framebuffer, nullptr);
        if (f.render_view) vkDestroyImageView(ctx_->device, f.render_view, nullptr);
        if (f.render_img) vkDestroyImage(ctx_->device, f.render_img, nullptr);
        if (f.render_mem) vkFreeMemory(ctx_->device, f.render_mem, nullptr);
        if (f.dmabuf_img) vkDestroyImage(ctx_->device, f.dmabuf_img, nullptr);
        if (f.dmabuf_mem) vkFreeMemory(ctx_->device, f.dmabuf_mem, nullptr);
        // dmabuf_fd ownership passed to libwwclient (it dup'd); close ours.
        if (f.dmabuf_fd >= 0) close(f.dmabuf_fd);
    }
    frames_.clear();
}

void WibbleWobblePresenter::Shutdown()
{
    if (ctx_ && ctx_->device != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(ctx_->queue_mutex);
        vkQueueWaitIdle(ctx_->queue);
    }
    if (ww_ && p_destroy_) {
        p_destroy_(ww_);
        ww_ = nullptr;
    }
    if (ctx_ && ctx_->device != VK_NULL_HANDLE) {
        DestroyFrames();
        if (cmd_pool_) { vkDestroyCommandPool(ctx_->device, cmd_pool_, nullptr); cmd_pool_ = VK_NULL_HANDLE; }
        if (render_pass_) { vkDestroyRenderPass(ctx_->device, render_pass_, nullptr); render_pass_ = VK_NULL_HANDLE; }
    }
    if (lib_) { dlclose(lib_); lib_ = nullptr; }
}

}  // namespace vrto3d
