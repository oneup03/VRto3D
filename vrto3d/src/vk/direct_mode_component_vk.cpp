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
#include "vk/direct_mode_component_vk.h"

#include <cinttypes>
#include <unistd.h>

#include "vk/vk_renderer.h"
#include "vrto3dlib/debug_log.hpp"

namespace {

// The IPC resource manager initializes asynchronously inside vrserver — it is
// typically NULL during IServerTrackedDeviceProvider::Init and ready by the
// first CreateSwapTextureSet (M0 finding). Always fetch it fresh.
vr::IVRIPCResourceManagerClient* ResourceManager()
{
    return vr::VRIPCResourceManager();
}

// The whole present chain is byte-preserving in gamma space, matching the
// Windows D3D11 path (raw copies, raw sampling, anaglyph matrices designed
// for gamma-encoded values). Importing the compositor's sRGB images with the
// UNORM twin format keeps Vulkan from silently linearizing at the blit.
VkFormat RawTwin(uint32_t vk_format)
{
    switch ((VkFormat)vk_format) {
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        default:                      return (VkFormat)vk_format;
    }
}

}  // namespace

DirectModeComponentVk::DirectModeComponentVk(VkRenderer* renderer)
    : renderer_(renderer)
{
}

DirectModeComponentVk::~DirectModeComponentVk()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [handle, tex] : textures_)
        ReleaseTexture(tex);
    textures_.clear();
}

void DirectModeComponentVk::CreateSwapTextureSet(uint32_t unPid,
                                                 const SwapTextureSetDesc_t* desc,
                                                 SwapTextureSet_t* out)
{
    auto* rm = ResourceManager();
    if (!rm) {
        LOG() << "direct_mode_vk: VRIPCResourceManager unavailable in CreateSwapTextureSet";
        return;
    }
    if (renderer_ && renderer_->IsDeviceDead())
        return;

    LOG() << "direct_mode_vk: CreateSwapTextureSet pid=" << unPid << " " << desc->nWidth
          << "x" << desc->nHeight << " fmt=" << desc->nFormat
          << " samples=" << desc->nSampleCount;

    vr::SharedTextureHandle_t handles[3] = {};
    for (int i = 0; i < 3; ++i) {
        // nFormat arrives as a VkFormat on Linux (M0: 43 = R8G8B8A8_SRGB).
        if (!rm->NewSharedVulkanImage(desc->nFormat, desc->nWidth, desc->nHeight,
                                      /*bRenderable=*/true, /*bMappable=*/false,
                                      /*bComputeAccess=*/true, /*unMipLevels=*/1,
                                      /*unArrayLayerCount=*/1, 0, 0, &handles[i])) {
            LOG() << "direct_mode_vk: NewSharedVulkanImage failed (i=" << i << ")";
            for (int j = 0; j < i; ++j)
                rm->UnrefResource(handles[j]);
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < 3; ++i) {
            if (!ImportHandle(handles[i], unPid, desc->nWidth, desc->nHeight, desc->nFormat,
                              handles[0])) {
                LOG() << "direct_mode_vk: dmabuf import failed for handle 0x" << std::hex
                      << handles[i] << std::dec << " — set unusable";
            }
        }
    }

    for (int i = 0; i < 3; ++i)
        out->rSharedTextureHandles[i] = handles[i];
    out->unTextureFlags = 0;
}

DirectModeComponentVk::ImportedTexture* DirectModeComponentVk::ImportHandle(
    vr::SharedTextureHandle_t handle, uint32_t pid, uint32_t width, uint32_t height,
    uint32_t vk_format, vr::SharedTextureHandle_t set_key)
{
    auto* rm = ResourceManager();
    auto& ctx = renderer_->Ctx();
    if (!rm || ctx.device == VK_NULL_HANDLE)
        return nullptr;

    uint64_t ipc_handle = 0;
    if (!rm->RefResource(handle, &ipc_handle)) {
        LOG() << "direct_mode_vk: RefResource failed";
        return nullptr;
    }
    int fd = -1;
    if (!rm->ReceiveSharedFd(ipc_handle, &fd) || fd < 0) {
        LOG() << "direct_mode_vk: ReceiveSharedFd failed";
        rm->UnrefResource(handle);
        return nullptr;
    }

    // Same-driver dmabuf import: the image was allocated by vrserver's Vulkan
    // device on the same GPU, so OPTIMAL tiling round-trips correctly on RADV
    // (validated in M0 bring-up). If a driver ever mismatches layouts here the
    // symptom is garbled sampling — the fallback would be
    // VK_EXT_image_drm_format_modifier with explicit modifiers.
    VkExternalMemoryImageCreateInfo ext_img{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &ext_img;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = RawTwin(vk_format);
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (vrto3d::vk::LogIfFailed(vkCreateImage(ctx.device, &ici, nullptr, &image),
                                "import vkCreateImage") != VK_SUCCESS) {
        close(fd);
        rm->UnrefResource(handle);
        return nullptr;
    }

    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(ctx.device, image, &reqs);

    PFN_vkGetMemoryFdPropertiesKHR get_fd_props =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(ctx.device,
                                                            "vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR fd_props{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (get_fd_props)
        get_fd_props(ctx.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_props);

    uint32_t type_bits = reqs.memoryTypeBits & (fd_props.memoryTypeBits ? fd_props.memoryTypeBits
                                                                        : reqs.memoryTypeBits);
    uint32_t mem_type = ctx.FindMemoryType(type_bits, 0);
    if (mem_type == UINT32_MAX) {
        LOG() << "direct_mode_vk: no compatible memory type for dmabuf import";
        vkDestroyImage(ctx.device, image, nullptr);
        close(fd);
        rm->UnrefResource(handle);
        return nullptr;
    }

    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = image;
    VkImportMemoryFdInfoKHR import_info{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    import_info.pNext = &dedicated;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_info.fd = fd;  // ownership transfers to Vulkan on success

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.pNext = &import_info;
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = mem_type;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vrto3d::vk::LogIfFailed(vkAllocateMemory(ctx.device, &alloc, nullptr, &memory),
                                "import vkAllocateMemory") != VK_SUCCESS) {
        vkDestroyImage(ctx.device, image, nullptr);
        close(fd);
        rm->UnrefResource(handle);
        return nullptr;
    }
    if (vrto3d::vk::LogIfFailed(vkBindImageMemory(ctx.device, image, memory, 0),
                                "import vkBindImageMemory") != VK_SUCCESS) {
        vkFreeMemory(ctx.device, memory, nullptr);
        vkDestroyImage(ctx.device, image, nullptr);
        rm->UnrefResource(handle);
        return nullptr;
    }

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = RawTwin(vk_format);
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vrto3d::vk::LogIfFailed(vkCreateImageView(ctx.device, &vci, nullptr, &view),
                                "import vkCreateImageView") != VK_SUCCESS) {
        vkFreeMemory(ctx.device, memory, nullptr);
        vkDestroyImage(ctx.device, image, nullptr);
        rm->UnrefResource(handle);
        return nullptr;
    }

    ImportedTexture tex;
    tex.handle = handle;
    tex.image = image;
    tex.memory = memory;
    tex.view = view;
    tex.width = width;
    tex.height = height;
    tex.format = RawTwin(vk_format);
    tex.pid = pid;
    tex.set_key = set_key;
    auto [it, inserted] = textures_.insert_or_assign(handle, tex);
    return &it->second;
}

void DirectModeComponentVk::ReleaseTexture(ImportedTexture& tex)
{
    auto& ctx = renderer_->Ctx();
    if (ctx.device != VK_NULL_HANDLE) {
        if (tex.view)
            vkDestroyImageView(ctx.device, tex.view, nullptr);
        if (tex.image)
            vkDestroyImage(ctx.device, tex.image, nullptr);
        if (tex.memory)
            vkFreeMemory(ctx.device, tex.memory, nullptr);
    }
    if (auto* rm = ResourceManager())
        rm->UnrefResource(tex.handle);
    tex.view = VK_NULL_HANDLE;
    tex.image = VK_NULL_HANDLE;
    tex.memory = VK_NULL_HANDLE;
}

void DirectModeComponentVk::DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle)
{
    // The renderer may still have a submit in flight sampling these images.
    renderer_->WaitIdleForTextureRelease();

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(sharedTextureHandle);
    if (it == textures_.end())
        return;
    const vr::SharedTextureHandle_t set_key = it->second.set_key;
    for (auto iter = textures_.begin(); iter != textures_.end();) {
        if (iter->second.set_key == set_key) {
            ReleaseTexture(iter->second);
            iter = textures_.erase(iter);
        } else {
            ++iter;
        }
    }
}

void DirectModeComponentVk::DestroyAllSwapTextureSets(uint32_t unPid)
{
    LOG() << "direct_mode_vk: DestroyAllSwapTextureSets pid=" << unPid;
    renderer_->WaitIdleForTextureRelease();

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto iter = textures_.begin(); iter != textures_.end();) {
        if (iter->second.pid == unPid) {
            ReleaseTexture(iter->second);
            iter = textures_.erase(iter);
        } else {
            ++iter;
        }
    }
}

void DirectModeComponentVk::GetNextSwapTextureSetIndex(
    vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t (*pIndices)[2])
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (int eye = 0; eye < 2; ++eye) {
        uint32_t index = 0;
        auto it = textures_.find(sharedTextureHandles[eye]);
        if (it != textures_.end()) {
            auto set_it = textures_.find(it->second.set_key);
            if (set_it != textures_.end()) {
                set_it->second.set_next_index = (set_it->second.set_next_index + 1) % 3;
                index = set_it->second.set_next_index;
            }
        }
        (*pIndices)[eye] = index;
    }
}

void DirectModeComponentVk::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2])
{
    // Single-layer model on Linux: keep only the most recent pair. (If the
    // compositor ever submits multiple layers, the last one wins — log once.)
    ++submit_count_;
    if (have_layer_ && submit_count_ <= 3)
        LOG() << "direct_mode_vk: multiple SubmitLayer calls per Present — unexpected on Linux";
    last_layer_[0] = perEye[0];
    last_layer_[1] = perEye[1];
    have_layer_ = true;
}

void DirectModeComponentVk::Present(vr::SharedTextureHandle_t syncTexture)
{
    (void)syncTexture;  // 0 on Linux (M0); sync is implicit via dmabuf
    ++present_count_;
    if (!have_layer_ || !renderer_)
        return;
    have_layer_ = false;

    VkRenderer::EyeLayer eyes[2];
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < 2; ++i) {
            auto it = textures_.find(last_layer_[i].hTexture);
            if (it == textures_.end() || it->second.image == VK_NULL_HANDLE) {
                if (present_count_ <= 5 || present_count_ % 600 == 0)
                    LOG() << "direct_mode_vk: Present with unknown texture handle 0x" << std::hex
                          << last_layer_[i].hTexture << std::dec;
                return;
            }
            eyes[i].image = it->second.image;
            eyes[i].view = it->second.view;
            eyes[i].width = it->second.width;
            eyes[i].height = it->second.height;
            eyes[i].bounds = last_layer_[i].bounds;
        }
    }
    renderer_->OnDirectModeFrame(eyes[0], eyes[1]);
}

void DirectModeComponentVk::PostPresent(const Throttling_t* pThrottling)
{
    (void)pThrottling;
}
