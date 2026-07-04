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
#include "presenter/vk_swapchain_util.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace vrto3d {

void PresenterLog(const char* fmt, ...)
{
    char body[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    std::fprintf(stderr, "[vrto3d] %s\n", body);
    std::fflush(stderr);
}

namespace {

VkSurfaceFormatKHR PickSurfaceFormat(VkPhysicalDevice phys, VkSurfaceKHR surface)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (count) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, formats.data());
    }
    if (formats.empty()) {
        return { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    }
    // Byte-preserving chain (matches the Windows D3D11 path): the repack
    // shader reads gamma-encoded values from a UNORM view and must write them
    // out unmodified, so prefer a UNORM swapchain over sRGB.
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        for (const auto& f : formats) {
            if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return f;
            }
        }
    }
    return formats.front();
}

}  // namespace


bool SwapchainBundle::Create(vk::DeviceCtx* device_ctx, VkSurfaceKHR surf,
                             uint32_t want_w, uint32_t want_h)
{
    ctx = device_ctx;
    surface = surf;
    desired_w = want_w;
    desired_h = want_h;

    VkBool32 supported = VK_FALSE;
    VkResult r = vkGetPhysicalDeviceSurfaceSupportKHR(ctx->phys, ctx->queue_family,
                                                      surface, &supported);
    if (r != VK_SUCCESS || supported != VK_TRUE) {
        PresenterLog("SwapchainBundle: surface not presentable on queue family %u (r=%d)",
                     ctx->queue_family, static_cast<int>(r));
        return false;
    }

    VkSurfaceFormatKHR sf = PickSurfaceFormat(ctx->phys, surface);
    format = sf.format;
    color_space = sf.colorSpace;

    // Render pass: single color attachment, loadOp DONT_CARE (the repack
    // shader overwrites every pixel), storeOp STORE, final PRESENT_SRC.
    VkAttachmentDescription att{};
    att.format         = format;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color_ref;

    // Order the layout transition after the acquire semaphore wait.
    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;

    r = vkCreateRenderPass(ctx->device, &rp, nullptr, &render_pass);
    if (r != VK_SUCCESS) {
        PresenterLog("SwapchainBundle: vkCreateRenderPass failed (r=%d)", static_cast<int>(r));
        return false;
    }

    if (!CreateSwapchainObjects(VK_NULL_HANDLE)) {
        Destroy();
        return false;
    }
    return true;
}


bool SwapchainBundle::CreateSwapchainObjects(VkSwapchainKHR old_swapchain)
{
    VkSurfaceCapabilitiesKHR caps{};
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->phys, surface, &caps);
    if (r != VK_SUCCESS) {
        PresenterLog("SwapchainBundle: surface caps query failed (r=%d)", static_cast<int>(r));
        return false;
    }

    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        extent = caps.currentExtent;
    } else {
        extent.width  = std::clamp(desired_w, caps.minImageExtent.width,  caps.maxImageExtent.width);
        extent.height = std::clamp(desired_h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        // Surface is (temporarily) zero-sized — nothing to present to.
        PresenterLog("SwapchainBundle: zero-sized surface extent, deferring swapchain");
        return false;
    }

    uint32_t min_images = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) {
        min_images = std::min(min_images, caps.maxImageCount);
    }

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        const VkCompositeAlphaFlagBitsKHR candidates[] = {
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        };
        for (auto c : candidates) {
            if (caps.supportedCompositeAlpha & c) { alpha = c; break; }
        }
    }

    VkSwapchainCreateInfoKHR sci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface          = surface;
    sci.minImageCount    = min_images;
    sci.imageFormat      = format;
    sci.imageColorSpace  = color_space;
    sci.imageExtent      = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform     = caps.currentTransform;
    sci.compositeAlpha   = alpha;
    sci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;   // vsync frame pacing
    sci.clipped          = VK_TRUE;
    sci.oldSwapchain     = old_swapchain;

    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(ctx->device, &sci, nullptr, &new_swapchain);
    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(ctx->device, old_swapchain, nullptr);
    }
    if (r != VK_SUCCESS) {
        PresenterLog("SwapchainBundle: vkCreateSwapchainKHR failed (r=%d)", static_cast<int>(r));
        return false;
    }
    swapchain = new_swapchain;

    uint32_t image_count = 0;
    vkGetSwapchainImagesKHR(ctx->device, swapchain, &image_count, nullptr);
    images.resize(image_count);
    vkGetSwapchainImagesKHR(ctx->device, swapchain, &image_count, images.data());

    views.resize(image_count, VK_NULL_HANDLE);
    framebuffers.resize(image_count, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < image_count; ++i) {
        VkImageViewCreateInfo iv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        iv.image    = images[i];
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format   = format;
        iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv.subresourceRange.levelCount = 1;
        iv.subresourceRange.layerCount = 1;
        r = vkCreateImageView(ctx->device, &iv, nullptr, &views[i]);
        if (r != VK_SUCCESS) {
            PresenterLog("SwapchainBundle: vkCreateImageView failed (r=%d)", static_cast<int>(r));
            return false;
        }

        VkFramebufferCreateInfo fb{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fb.renderPass      = render_pass;
        fb.attachmentCount = 1;
        fb.pAttachments    = &views[i];
        fb.width           = extent.width;
        fb.height          = extent.height;
        fb.layers          = 1;
        r = vkCreateFramebuffer(ctx->device, &fb, nullptr, &framebuffers[i]);
        if (r != VK_SUCCESS) {
            PresenterLog("SwapchainBundle: vkCreateFramebuffer failed (r=%d)", static_cast<int>(r));
            return false;
        }
    }

    needs_recreate = false;
    PresenterLog("SwapchainBundle: swapchain %ux%u, %u images, format %d (FIFO)",
                 extent.width, extent.height, image_count, static_cast<int>(format));
    return true;
}


void SwapchainBundle::DestroySwapchainObjects(bool keep_render_pass)
{
    if (!ctx || ctx->device == VK_NULL_HANDLE) return;

    for (VkFramebuffer fb : framebuffers) {
        if (fb) vkDestroyFramebuffer(ctx->device, fb, nullptr);
    }
    framebuffers.clear();
    for (VkImageView v : views) {
        if (v) vkDestroyImageView(ctx->device, v, nullptr);
    }
    views.clear();
    images.clear();

    if (swapchain) {
        vkDestroySwapchainKHR(ctx->device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
    if (!keep_render_pass && render_pass) {
        vkDestroyRenderPass(ctx->device, render_pass, nullptr);
        render_pass = VK_NULL_HANDLE;
    }
}


bool SwapchainBundle::Recreate()
{
    if (!ctx) return false;

    {
        // Drain in-flight work touching the old swapchain images. queue_mutex
        // keeps the compositor thread's submits out while we idle the device.
        std::lock_guard<std::mutex> lock(ctx->queue_mutex);
        vkDeviceWaitIdle(ctx->device);
    }

    for (VkFramebuffer fb : framebuffers) {
        if (fb) vkDestroyFramebuffer(ctx->device, fb, nullptr);
    }
    framebuffers.clear();
    for (VkImageView v : views) {
        if (v) vkDestroyImageView(ctx->device, v, nullptr);
    }
    views.clear();
    images.clear();

    VkSwapchainKHR old = swapchain;
    swapchain = VK_NULL_HANDLE;
    return CreateSwapchainObjects(old);
}


void SwapchainBundle::Destroy()
{
    if (!ctx) return;
    {
        std::lock_guard<std::mutex> lock(ctx->queue_mutex);
        vkDeviceWaitIdle(ctx->device);
    }
    DestroySwapchainObjects(false);
    surface = VK_NULL_HANDLE;
    ctx = nullptr;
}


void SwapchainBundle::SetDesiredExtent(uint32_t w, uint32_t h)
{
    if (w != 0 && h != 0 && (w != desired_w || h != desired_h)) {
        desired_w = w;
        desired_h = h;
        needs_recreate = true;
    }
}


bool SwapchainBundle::AcquireNext(IVkPresenter::FrameTarget* out, VkSemaphore signal_sem)
{
    if (!ctx) return false;

    if (needs_recreate || swapchain == VK_NULL_HANDLE) {
        if (!Recreate()) return false;
        // Skip this frame — caller retries next frame with a clean semaphore.
        return false;
    }

    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(ctx->device, swapchain, UINT64_MAX,
                                       signal_sem, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        // Semaphore was NOT signaled — safe to recreate and skip the frame.
        needs_recreate = true;
        return false;
    }
    if (r == VK_SUBOPTIMAL_KHR) {
        // Image acquired and semaphore pending — must proceed with the frame;
        // recreate before the next acquire.
        needs_recreate = true;
    } else if (r != VK_SUCCESS) {
        PresenterLog("SwapchainBundle: vkAcquireNextImageKHR failed (r=%d)", static_cast<int>(r));
        return false;
    }

    out->image       = images[index];
    out->view        = views[index];
    out->framebuffer = framebuffers[index];
    out->index       = index;
    return true;
}


bool SwapchainBundle::Present(uint32_t image_index, VkSemaphore wait_sem)
{
    if (!ctx || swapchain == VK_NULL_HANDLE) return false;

    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &wait_sem;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &swapchain;
    pi.pImageIndices      = &image_index;

    VkResult r;
    {
        std::lock_guard<std::mutex> lock(ctx->queue_mutex);
        r = vkQueuePresentKHR(ctx->queue, &pi);
    }

    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        // Wait semaphore is still consumed by a rejected present (spec
        // guarantees the wait executes), so just flag the recreate.
        needs_recreate = true;
        return true;
    }
    if (r != VK_SUCCESS) {
        PresenterLog("SwapchainBundle: vkQueuePresentKHR failed (r=%d)", static_cast<int>(r));
        return false;
    }
    return true;
}

}  // namespace vrto3d
