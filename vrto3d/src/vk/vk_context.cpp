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
#include "vk/vk_context.h"

#include <cstring>
#include <vector>

#include "vrto3dlib/debug_log.hpp"

namespace vrto3d::vk {

VkResult LogIfFailed(VkResult r, const char* what)
{
    if (r != VK_SUCCESS)
        LOG() << "vk: " << what << " failed (" << (int)r << ")";
    return r;
}

bool DeviceCtx::Init()
{
    // ---- instance ----
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vrto3d";
    app.apiVersion = VK_API_VERSION_1_1;

    uint32_t avail_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &avail_count, nullptr);
    std::vector<VkExtensionProperties> avail(avail_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &avail_count, avail.data());
    auto instance_ext_supported = [&](const char* name) {
        for (const auto& e : avail)
            if (strcmp(e.extensionName, name) == 0) return true;
        return false;
    };

    std::vector<const char*> inst_exts = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
    for (const char* wsi : {"VK_KHR_surface", "VK_KHR_wayland_surface",
                            "VK_KHR_xcb_surface", "VK_KHR_xlib_surface"}) {
        if (instance_ext_supported(wsi))
            inst_exts.push_back(wsi);
    }

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)inst_exts.size();
    ici.ppEnabledExtensionNames = inst_exts.data();
    if (LogIfFailed(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance") != VK_SUCCESS)
        return false;

    // ---- physical device ----
    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(instance, &dev_count, nullptr);
    if (dev_count == 0) {
        LOG() << "vk: no physical devices";
        return false;
    }
    std::vector<VkPhysicalDevice> devs(dev_count);
    vkEnumeratePhysicalDevices(instance, &dev_count, devs.data());

    static const char* kRequiredDevExts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    };

    for (VkPhysicalDevice candidate : devs) {
        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &ext_count, exts.data());
        bool all = true;
        for (const char* need : kRequiredDevExts) {
            bool found = false;
            for (const auto& e : exts)
                if (strcmp(e.extensionName, need) == 0) { found = true; break; }
            if (!found) { all = false; break; }
        }
        if (!all)
            continue;

        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qf_count, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qf_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qf_count, qfs.data());
        for (uint32_t i = 0; i < qf_count; ++i) {
            if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                phys = candidate;
                queue_family = i;
                break;
            }
        }
        if (phys != VK_NULL_HANDLE)
            break;
    }
    if (phys == VK_NULL_HANDLE) {
        LOG() << "vk: no physical device with required extensions";
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    LOG() << "vk: using " << props.deviceName;
    vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);

    // ---- device ----
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    timeline.timelineSemaphore = VK_TRUE;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &timeline;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)(sizeof(kRequiredDevExts) / sizeof(kRequiredDevExts[0]));
    dci.ppEnabledExtensionNames = kRequiredDevExts;
    if (LogIfFailed(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice") != VK_SUCCESS)
        return false;

    vkGetDeviceQueue(device, queue_family, 0, &queue);
    return true;
}

void DeviceCtx::Destroy()
{
    if (device) {
        vkDeviceWaitIdle(device);
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (instance) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
    phys = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;
}

uint32_t DeviceCtx::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags want) const
{
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}

VkShaderModule CreateShaderModule(VkDevice device, const uint32_t* spirv_words, size_t byte_size)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = byte_size;
    ci.pCode = spirv_words;
    VkShaderModule mod = VK_NULL_HANDLE;
    LogIfFailed(vkCreateShaderModule(device, &ci, nullptr, &mod), "vkCreateShaderModule");
    return mod;
}

}  // namespace vrto3d::vk
