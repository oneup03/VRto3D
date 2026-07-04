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
#include "vk/vk_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "hmd_device_driver.h"
#include "osd/osd_menu.h"
#include "osd/osd_renderer.h"
#include "platform.h"
#include "presenter/vk_presenter.h"
#include "vrto3dlib/debug_log.hpp"
#include "vrto3dlib/linux_helper.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_DISABLE
#include "stb_image_write.h"

#include "../shaders/generated/fullscreen_vert_spv.h"
#include "../shaders/generated/repack_frag_spv.h"

namespace {

// C++ mirror of the push-constant block in shaders/repack.frag.
struct RepackPush {
    int32_t out_w, out_h;
    int32_t eye_w, eye_h;
    int32_t mode;
    int32_t eye_swap;
    int32_t correction_enabled;
    float   curve;
    float   lift[4];
    float   gamma[4];
    float   gain[4];
    float   curve_offsets[4];
};
static_assert(sizeof(RepackPush) <= 128, "push constant limit");

VkImageMemoryBarrier ImageBarrier(VkImage image, VkAccessFlags src_access,
                                  VkAccessFlags dst_access, VkImageLayout old_layout,
                                  VkImageLayout new_layout,
                                  uint32_t src_family = VK_QUEUE_FAMILY_IGNORED,
                                  uint32_t dst_family = VK_QUEUE_FAMILY_IGNORED)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = src_family;
    b.dstQueueFamilyIndex = dst_family;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

}  // namespace

struct FrameTargetBits {
    VkFramebuffer framebuffer;
    VkExtent2D    extent;
};

VkRenderer::VkRenderer() = default;

VkRenderer::~VkRenderer()
{
    Shutdown();
}

bool VkRenderer::Init(const StereoDisplayDriverConfiguration& cfg,
                      const vrto3d::FocusContext& focus)
{
    cfg_ = cfg;
    focus_ = focus;
    display_frequency_ = cfg.display_frequency > 1.0f ? cfg.display_frequency : 60.0f;

    if (!ctx_.Init()) {
        LOG() << "vk_renderer: device init failed";
        device_dead_.store(true);
        return false;
    }

    running_.store(true);
    present_thread_ = std::thread(&VkRenderer::PresentThread, this);
    vsync_thread_ = std::thread(&VkRenderer::VsyncTickThread, this);
    return true;
}

void VkRenderer::Shutdown()
{
    if (!running_.exchange(false))
        return;
    frame_cv_.notify_all();
    if (present_thread_.joinable())
        present_thread_.join();
    if (vsync_thread_.joinable())
        vsync_thread_.join();
    // Present thread destroyed its own resources (pipeline, presenter, OSD)
    // before exiting; the device goes last.
    ctx_.Destroy();
}

void VkRenderer::OnDirectModeFrame(const EyeLayer& left, const EyeLayer& right)
{
    if (paused_for_disconnect_.load(std::memory_order_acquire) ||
        device_dead_.load(std::memory_order_acquire))
        return;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        pending_left_ = left;
        pending_right_ = right;
        ++pending_seq_;
    }
    frame_cv_.notify_one();
}

void VkRenderer::ConfigureOsd(StereoDisplayComponent* component,
                              vrto3d::osd::MenuCallbacks callbacks, void* /*native_window*/)
{
    osd_component_ = component;
    osd_callbacks_ = std::make_unique<vrto3d::osd::MenuCallbacks>(std::move(callbacks));
    osd_config_pending_ = true;
}

vrto3d::osd::OsdRenderer* VkRenderer::Osd()
{
    return osd_renderer_.get();
}

void VkRenderer::RequestScreenshot(std::string app_name)
{
    std::lock_guard<std::mutex> lock(shot_mutex_);
    shot_app_name_ = std::move(app_name);
    shot_requested_ = true;
}

void VkRenderer::OnAppConnect()
{
    paused_for_disconnect_.store(false, std::memory_order_release);
}

void VkRenderer::OnAppDisconnect()
{
    paused_for_disconnect_.store(true, std::memory_order_release);
}

void VkRenderer::WaitIdleForTextureRelease()
{
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        consumed_seq_ = pending_seq_;   // drop any not-yet-consumed snapshot
        pending_left_ = {};
        pending_right_ = {};
    }
    if (ctx_.device != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> qlock(ctx_.queue_mutex);
        vkQueueWaitIdle(ctx_.queue);
    }
}

// ---------------------------------------------------------------------------

bool VkRenderer::EnsureOutputImage(uint32_t eye_w, uint32_t eye_h)
{
    const uint32_t want_w = eye_w * 2;
    const uint32_t want_h = eye_h;
    if (out_sbs_ != VK_NULL_HANDLE && sbs_width_ == want_w && sbs_height_ == want_h)
        return true;

    {
        std::lock_guard<std::mutex> qlock(ctx_.queue_mutex);
        vkQueueWaitIdle(ctx_.queue);
    }
    if (out_sbs_view_) vkDestroyImageView(ctx_.device, out_sbs_view_, nullptr);
    if (out_sbs_) vkDestroyImage(ctx_.device, out_sbs_, nullptr);
    if (out_sbs_mem_) vkFreeMemory(ctx_.device, out_sbs_mem_, nullptr);
    out_sbs_ = VK_NULL_HANDLE; out_sbs_view_ = VK_NULL_HANDLE; out_sbs_mem_ = VK_NULL_HANDLE;

    out_sbs_format_ = VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = out_sbs_format_;
    ici.extent = {want_w, want_h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vrto3d::vk::LogIfFailed(vkCreateImage(ctx_.device, &ici, nullptr, &out_sbs_),
                                "out_sbs vkCreateImage") != VK_SUCCESS)
        return false;

    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(ctx_.device, out_sbs_, &reqs);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = ctx_.FindMemoryType(reqs.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vrto3d::vk::LogIfFailed(vkAllocateMemory(ctx_.device, &alloc, nullptr, &out_sbs_mem_),
                                "out_sbs vkAllocateMemory") != VK_SUCCESS)
        return false;
    vkBindImageMemory(ctx_.device, out_sbs_, out_sbs_mem_, 0);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = out_sbs_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = out_sbs_format_;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vrto3d::vk::LogIfFailed(vkCreateImageView(ctx_.device, &vci, nullptr, &out_sbs_view_),
                                "out_sbs vkCreateImageView") != VK_SUCCESS)
        return false;

    sbs_width_ = want_w;
    sbs_height_ = want_h;
    out_sbs_initialized_ = false;

    // Point the repack descriptor at the new view.
    if (repack_set_ != VK_NULL_HANDLE) {
        VkDescriptorImageInfo img{repack_sampler_, out_sbs_view_,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = repack_set_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &img;
        vkUpdateDescriptorSets(ctx_.device, 1, &write, 0, nullptr);
    }

    // OSD render target size follows the per-eye dims.
    if (osd_renderer_)
        osd_renderer_->OnResize(eye_w, eye_h);

    LOG() << "vk_renderer: out_sbs " << want_w << "x" << want_h;
    return true;
}

bool VkRenderer::EnsureRepackPipeline()
{
    if (repack_pipeline_ != VK_NULL_HANDLE)
        return true;
    if (!presenter_)
        return false;

    repack_render_pass_ = presenter_->RenderPass();

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(ctx_.device, &sci, nullptr, &repack_sampler_);

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;
    vkCreateDescriptorSetLayout(ctx_.device, &dslci, nullptr, &repack_dsl_);

    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(RepackPush)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &repack_dsl_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pc;
    vkCreatePipelineLayout(ctx_.device, &plci, nullptr, &repack_layout_);

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &pool_size;
    vkCreateDescriptorPool(ctx_.device, &dpci, nullptr, &repack_pool_);

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = repack_pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &repack_dsl_;
    vkAllocateDescriptorSets(ctx_.device, &dsai, &repack_set_);

    if (out_sbs_view_ != VK_NULL_HANDLE) {
        VkDescriptorImageInfo img{repack_sampler_, out_sbs_view_,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = repack_set_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &img;
        vkUpdateDescriptorSets(ctx_.device, 1, &write, 0, nullptr);
    }

    VkShaderModule vs = vrto3d::vk::CreateShaderModule(ctx_.device, fullscreen_vert_spv,
                                                       sizeof(fullscreen_vert_spv));
    VkShaderModule fs = vrto3d::vk::CreateShaderModule(ctx_.device, repack_frag_spv,
                                                       sizeof(repack_frag_spv));
    if (!vs || !fs)
        return false;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_att;
    VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vin;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pColorBlendState = &blend;
    gpci.pDynamicState = &dyn;
    gpci.layout = repack_layout_;
    gpci.renderPass = repack_render_pass_;
    gpci.subpass = 0;
    VkResult r = vkCreateGraphicsPipelines(ctx_.device, VK_NULL_HANDLE, 1, &gpci, nullptr,
                                           &repack_pipeline_);
    vkDestroyShaderModule(ctx_.device, vs, nullptr);
    vkDestroyShaderModule(ctx_.device, fs, nullptr);
    if (vrto3d::vk::LogIfFailed(r, "repack vkCreateGraphicsPipelines") != VK_SUCCESS)
        return false;

    LOG() << "vk_renderer: repack pipeline ready";
    return true;
}

void VkRenderer::RecordFrame(VkCommandBuffer cmd, const EyeLayer& left, const EyeLayer& right,
                             const FrameTargetBits& target)
{
    // Acquire the compositor's images (written on vrserver's own device;
    // implicit dmabuf sync orders those writes against this submission).
    VkImageMemoryBarrier acquire[2] = {
        ImageBarrier(left.image, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_QUEUE_FAMILY_EXTERNAL, ctx_.queue_family),
        ImageBarrier(right.image, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_QUEUE_FAMILY_EXTERNAL, ctx_.queue_family),
    };
    VkImageMemoryBarrier to_dst = ImageBarrier(
        out_sbs_, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        out_sbs_initialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    out_sbs_initialized_ = true;
    VkImageMemoryBarrier pre[3] = {acquire[0], acquire[1], to_dst};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 3, pre);

    // Blit each eye into its half, honoring the submitted UV bounds (swapped
    // offsets mirror automatically, which covers vMin>vMax flipped submissions).
    const uint32_t eye_w = sbs_width_ / 2;
    const uint32_t eye_h = sbs_height_;
    for (int i = 0; i < 2; ++i) {
        const EyeLayer& src = (i == 0) ? left : right;
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {(int32_t)std::lround(src.bounds.uMin * src.width),
                              (int32_t)std::lround(src.bounds.vMin * src.height), 0};
        blit.srcOffsets[1] = {(int32_t)std::lround(src.bounds.uMax * src.width),
                              (int32_t)std::lround(src.bounds.vMax * src.height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {(int32_t)(i * eye_w), 0, 0};
        blit.dstOffsets[1] = {(int32_t)((i + 1) * eye_w), (int32_t)eye_h, 1};
        vkCmdBlitImage(cmd, src.image, VK_IMAGE_LAYOUT_GENERAL, out_sbs_,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }

    // Screenshot readback happens pre-OSD, matching the Windows behavior.
    MaybeSaveScreenshot(cmd);

    // OSD pass (no-op inside when nothing to draw).
    VkImageMemoryBarrier to_color = ImageBarrier(
        out_sbs_, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &to_color);
    if (osd_renderer_)
        osd_renderer_->RenderFrame(cmd, out_sbs_, out_sbs_view_, sbs_width_, sbs_height_);

    VkImageMemoryBarrier to_read = ImageBarrier(
        out_sbs_, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_read);

    // Repack into the swapchain image.
    VkClearValue clear{};
    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = repack_render_pass_;
    rpbi.framebuffer = target.framebuffer;
    rpbi.renderArea = {{0, 0}, target.extent};
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{0.f, 0.f, (float)target.extent.width, (float)target.extent.height,
                        0.f, 1.f};
    VkRect2D scissor{{0, 0}, target.extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, repack_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, repack_layout_, 0, 1,
                            &repack_set_, 0, nullptr);

    RepackPush push{};
    push.out_w = (int32_t)target.extent.width;
    push.out_h = (int32_t)target.extent.height;
    push.eye_w = (int32_t)eye_w;
    push.eye_h = (int32_t)eye_h;
    StereoDisplayDriverConfiguration cfg = osd_component_ ? osd_component_->GetConfig() : cfg_;
    push.mode = (int32_t)cfg.output_mode;
    push.eye_swap = cfg.eye_swap ? 1 : 0;
    push.correction_enabled = cfg.shader_enabled ? 1 : 0;
    push.curve = cfg.shader_curve;
    for (int i = 0; i < 3; ++i) {
        push.lift[i] = cfg.shader_lift[i];
        push.gamma[i] = cfg.shader_gamma[i];
        push.gain[i] = cfg.shader_gain[i];
    }
    push.curve_offsets[0] = cfg.shader_curve_off_low;
    push.curve_offsets[1] = cfg.shader_curve_off_high;
    push.curve_offsets[2] = cfg.shader_curve_off_both;
    vkCmdPushConstants(cmd, repack_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void VkRenderer::MaybeSaveScreenshot(VkCommandBuffer cmd)
{
    bool want = false;
    {
        std::lock_guard<std::mutex> lock(shot_mutex_);
        want = shot_requested_ && !shot_inflight_;
        if (want)
            shot_requested_ = false;
    }
    if (!want)
        return;

    const VkDeviceSize size = (VkDeviceSize)sbs_width_ * sbs_height_ * 4;
    if (shot_buffer_ == VK_NULL_HANDLE || shot_w_ != sbs_width_ || shot_h_ != sbs_height_) {
        if (shot_buffer_) vkDestroyBuffer(ctx_.device, shot_buffer_, nullptr);
        if (shot_memory_) vkFreeMemory(ctx_.device, shot_memory_, nullptr);
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(ctx_.device, &bci, nullptr, &shot_buffer_) != VK_SUCCESS)
            return;
        VkMemoryRequirements reqs{};
        vkGetBufferMemoryRequirements(ctx_.device, shot_buffer_, &reqs);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = reqs.size;
        alloc.memoryTypeIndex = ctx_.FindMemoryType(
            reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (alloc.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(ctx_.device, &alloc, nullptr, &shot_memory_) != VK_SUCCESS)
            return;
        vkBindBufferMemory(ctx_.device, shot_buffer_, shot_memory_, 0);
        shot_w_ = sbs_width_;
        shot_h_ = sbs_height_;
    }

    VkImageMemoryBarrier to_src = ImageBarrier(
        out_sbs_, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_src);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {sbs_width_, sbs_height_, 1};
    vkCmdCopyImageToBuffer(cmd, out_sbs_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot_buffer_, 1,
                           &copy);
    VkImageMemoryBarrier back = ImageBarrier(
        out_sbs_, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &back);
    shot_inflight_ = true;
}

void VkRenderer::FinishScreenshot()
{
    if (!shot_inflight_)
        return;
    shot_inflight_ = false;

    void* mapped = nullptr;
    if (vkMapMemory(ctx_.device, shot_memory_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        return;

    namespace fs = std::filesystem;
    const std::string steam = GetSteamInstallPath();
    if (steam.empty()) {
        vkUnmapMemory(ctx_.device, shot_memory_);
        return;
    }
    fs::path dir = fs::path(steam) / "steamapps/common/SteamVR/screenshots";
    std::error_code ec;
    fs::create_directories(dir, ec);

    std::string app;
    {
        std::lock_guard<std::mutex> lock(shot_mutex_);
        app = shot_app_name_.empty() ? "vrto3d" : shot_app_name_;
    }
    for (char& c : app)
        if (c == '/' || c == '\\' || c == ':')
            c = '_';
    const auto stamp = (long long)(platform::MonotonicSeconds() * 1000.0);

    const uint32_t w = shot_w_, h = shot_h_, eye_w = w / 2;
    const uint8_t* src = (const uint8_t*)mapped;

    // Parallel-view (as composited) + cross-view (eyes swapped).
    std::vector<uint8_t> cross((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = src + (size_t)y * w * 4;
        uint8_t* out = cross.data() + (size_t)y * w * 4;
        memcpy(out, row + (size_t)eye_w * 4, (size_t)eye_w * 4);
        memcpy(out + (size_t)eye_w * 4, row, (size_t)eye_w * 4);
    }
    const std::string para = (dir / (app + "_" + std::to_string(stamp) + "_parallel.png")).string();
    const std::string crss = (dir / (app + "_" + std::to_string(stamp) + "_cross.png")).string();
    stbi_write_png(para.c_str(), (int)w, (int)h, 4, src, (int)(w * 4));
    stbi_write_png(crss.c_str(), (int)w, (int)h, 4, cross.data(), (int)(w * 4));
    vkUnmapMemory(ctx_.device, shot_memory_);
    LOG() << "vk_renderer: screenshot saved " << para;
}

// ---------------------------------------------------------------------------

void VkRenderer::PresentThread()
{
    presenter_ = vrto3d::MakeVkPresenter(cfg_);
    if (!presenter_ || !presenter_->Init(&ctx_, cfg_)) {
        LOG() << "vk_renderer: presenter init failed — no output window. Frames will be dropped.";
        presenter_.reset();
        // Stay alive: vsync events keep the compositor happy so SteamVR
        // doesn't watchdog; there's just nothing on screen.
        while (running_.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        return;
    }
    presenter_->BringToTop();

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = ctx_.queue_family;
    vkCreateCommandPool(ctx_.device, &cpci, nullptr, &cmd_pool_);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmd_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = kFramesInFlight;
    vkAllocateCommandBuffers(ctx_.device, &cbai, cmd_bufs_);
    for (int i = 0; i < kFramesInFlight; ++i) {
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(ctx_.device, &fci, nullptr, &frame_fences_[i]);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(ctx_.device, &sci, nullptr, &acquire_sems_[i]);
        vkCreateSemaphore(ctx_.device, &sci, nullptr, &render_sems_[i]);
    }

    while (running_.load()) {
        EyeLayer left, right;
        bool have_frame = false;
        {
            std::unique_lock<std::mutex> lock(frame_mutex_);
            frame_cv_.wait_for(lock, std::chrono::milliseconds(50), [&] {
                return pending_seq_ != consumed_seq_ || !running_.load();
            });
            if (!running_.load())
                break;
            if (pending_seq_ != consumed_seq_) {
                consumed_seq_ = pending_seq_;
                left = pending_left_;
                right = pending_right_;
                have_frame = left.image != VK_NULL_HANDLE && right.image != VK_NULL_HANDLE;
            }
        }

        if (!presenter_->PumpEvents()) {
            LOG() << "vk_renderer: presenter closed — requesting SteamVR shutdown";
            std::thread([] { RequestSteamVRShutdownWithApp(g_current_app_pid.load()); }).detach();
            break;
        }
        if (!have_frame)
            continue;

        if (!EnsureOutputImage(left.width, left.height) || !EnsureRepackPipeline())
            continue;

        // Lazy OSD init once dimensions are known.
        if (osd_config_pending_ && osd_component_ && osd_callbacks_) {
            osd_config_pending_ = false;
            auto osd = std::make_unique<vrto3d::osd::OsdRenderer>();
            if (osd->Init(&ctx_, out_sbs_format_, sbs_width_ / 2, sbs_height_, nullptr,
                          osd_component_, *osd_callbacks_)) {
                osd_renderer_ = std::move(osd);
            } else {
                LOG() << "vk_renderer: OSD init failed — continuing without OSD";
            }
        }

        const int slot = frame_slot_;
        frame_slot_ = (frame_slot_ + 1) % kFramesInFlight;
        vkWaitForFences(ctx_.device, 1, &frame_fences_[slot], VK_TRUE, UINT64_MAX);
        if (shot_inflight_)
            FinishScreenshot();
        vkResetFences(ctx_.device, 1, &frame_fences_[slot]);

        vrto3d::IVkPresenter::FrameTarget target{};
        if (!presenter_->AcquireNext(&target, acquire_sems_[slot])) {
            // Swapchain went stale (resize etc.) — pipeline must follow the
            // new render pass on the next round if the presenter recreated it.
            continue;
        }

        VkCommandBuffer cmd = cmd_bufs_[slot];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        FrameTargetBits bits{target.framebuffer, presenter_->Extent()};
        RecordFrame(cmd, left, right, bits);
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquire_sems_[slot];
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &render_sems_[slot];
        {
            std::lock_guard<std::mutex> qlock(ctx_.queue_mutex);
            if (vrto3d::vk::LogIfFailed(vkQueueSubmit(ctx_.queue, 1, &submit, frame_fences_[slot]),
                                        "frame vkQueueSubmit") != VK_SUCCESS) {
                device_dead_.store(true, std::memory_order_release);
                break;
            }
        }
        presenter_->Present(target.index, render_sems_[slot]);

        frame_counter_.fetch_add(1, std::memory_order_relaxed);
        last_vsync_sec_.store(platform::MonotonicSeconds(), std::memory_order_relaxed);
    }

    // Teardown on this thread (presenter + OSD are thread-affine).
    {
        std::lock_guard<std::mutex> qlock(ctx_.queue_mutex);
        vkQueueWaitIdle(ctx_.queue);
    }
    if (osd_renderer_) {
        osd_renderer_->Shutdown();
        osd_renderer_.reset();
    }
    for (int i = 0; i < kFramesInFlight; ++i) {
        if (frame_fences_[i]) vkDestroyFence(ctx_.device, frame_fences_[i], nullptr);
        if (acquire_sems_[i]) vkDestroySemaphore(ctx_.device, acquire_sems_[i], nullptr);
        if (render_sems_[i]) vkDestroySemaphore(ctx_.device, render_sems_[i], nullptr);
    }
    if (cmd_pool_) vkDestroyCommandPool(ctx_.device, cmd_pool_, nullptr);
    if (repack_pipeline_) vkDestroyPipeline(ctx_.device, repack_pipeline_, nullptr);
    if (repack_layout_) vkDestroyPipelineLayout(ctx_.device, repack_layout_, nullptr);
    if (repack_pool_) vkDestroyDescriptorPool(ctx_.device, repack_pool_, nullptr);
    if (repack_dsl_) vkDestroyDescriptorSetLayout(ctx_.device, repack_dsl_, nullptr);
    if (repack_sampler_) vkDestroySampler(ctx_.device, repack_sampler_, nullptr);
    if (out_sbs_view_) vkDestroyImageView(ctx_.device, out_sbs_view_, nullptr);
    if (out_sbs_) vkDestroyImage(ctx_.device, out_sbs_, nullptr);
    if (out_sbs_mem_) vkFreeMemory(ctx_.device, out_sbs_mem_, nullptr);
    if (shot_buffer_) vkDestroyBuffer(ctx_.device, shot_buffer_, nullptr);
    if (shot_memory_) vkFreeMemory(ctx_.device, shot_memory_, nullptr);
    if (presenter_) {
        presenter_->Shutdown();
        presenter_.reset();
    }
}

void VkRenderer::VsyncTickThread()
{
    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / display_frequency_));
    auto next = std::chrono::steady_clock::now();
    while (running_.load()) {
        vr::VRServerDriverHost()->VsyncEvent(0.0);
        next += interval;
        std::this_thread::sleep_until(next);
    }
}
