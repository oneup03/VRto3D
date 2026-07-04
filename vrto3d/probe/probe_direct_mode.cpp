#include "probe_direct_mode.h"
#include "probe_log.h"

#include <vulkan/vulkan.h>

#include <chrono>
#include <cinttypes>
#include <unistd.h>
#include <vector>

namespace {

double NowSec()
{
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

// Minimal Vulkan bring-up used only to prove a received fd is a dmabuf the
// GPU driver will accept. Full image import (modifiers, bind) is M3 work.
struct VkProbe
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetMemoryFdPropertiesKHR pGetMemoryFdPropertiesKHR = nullptr;
    bool ready = false;

    bool Init()
    {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "vrto3d_probe";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
            PLOG("vk: vkCreateInstance FAILED");
            return false;
        }
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (count == 0) {
            PLOG("vk: no physical devices");
            return false;
        }
        std::vector<VkPhysicalDevice> devs(count);
        vkEnumeratePhysicalDevices(instance, &count, devs.data());
        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(devs[i], &props);
            PLOG("vk: physical device %u: %s (vendor 0x%x device 0x%x)", i,
                 props.deviceName, props.vendorID, props.deviceID);
        }
        phys = devs[0];

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = 0;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        const char *exts[] = {
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        };
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 2;
        dci.ppEnabledExtensionNames = exts;
        VkResult r = vkCreateDevice(phys, &dci, nullptr, &device);
        if (r != VK_SUCCESS) {
            PLOG("vk: vkCreateDevice FAILED (%d) — dmabuf exts unsupported?", (int)r);
            return false;
        }
        pGetMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
            device, "vkGetMemoryFdPropertiesKHR");
        ready = pGetMemoryFdPropertiesKHR != nullptr;
        PLOG("vk: device ready=%d", (int)ready);
        return ready;
    }
};

VkProbe &GetVkProbe()
{
    static VkProbe probe;
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        probe.Init();
    }
    return probe;
}

} // namespace

void ProbeDirectMode::CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t *desc,
                                           SwapTextureSet_t *out)
{
    PLOG("CreateSwapTextureSet: pid=%u %ux%u format=%u samples=%u", unPid, desc->nWidth,
         desc->nHeight, desc->nFormat, desc->nSampleCount);

    auto *rm = vr::VRIPCResourceManager();
    if (!rm) {
        PLOG("CreateSwapTextureSet: VRIPCResourceManager() is NULL — cannot allocate");
        return;
    }

    SetInfo info{};
    info.pid = unPid;
    info.width = desc->nWidth;
    info.height = desc->nHeight;
    info.format = desc->nFormat;
    info.next_index = 0;

    for (int i = 0; i < 3; ++i) {
        vr::SharedTextureHandle_t handle = 0;
        bool ok = rm->NewSharedVulkanImage(desc->nFormat, desc->nWidth, desc->nHeight,
                                           /*bRenderable=*/true, /*bMappable=*/false,
                                           /*bComputeAccess=*/true, /*unMipLevels=*/1,
                                           /*unArrayLayerCount=*/1,
                                           /*unAdditionalVkCreateFlags=*/0,
                                           /*unAdditionalVkUsageFlags=*/0, &handle);
        PLOG("  NewSharedVulkanImage[%d]: ok=%d handle=0x%" PRIx64, i, (int)ok,
             (uint64_t)handle);
        if (!ok) {
            // Format rejected? Try VK_FORMAT_R8G8B8A8_UNORM (=37) as a probe of
            // whether the failure is format-specific or systemic.
            ok = rm->NewSharedVulkanImage(37, desc->nWidth, desc->nHeight, true, false, true,
                                          1, 1, 0, 0, &handle);
            PLOG("  NewSharedVulkanImage[%d] retry fmt=VK_FORMAT_R8G8B8A8_UNORM: ok=%d "
                 "handle=0x%" PRIx64, i, (int)ok, (uint64_t)handle);
        }
        if (!ok)
            return; // leave out zeroed; compositor reaction is itself a data point
        info.handles[i] = handle;
        out->rSharedTextureHandles[i] = handle;
    }
    out->unTextureFlags = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        sets_[info.handles[0]] = info;
        for (int i = 0; i < 3; ++i)
            handle_to_set_[info.handles[i]] = info.handles[0];
    }

    if (!dmabuf_validated_) {
        dmabuf_validated_ = true;
        ValidateDmabufImport(info.handles[0]);
    }
}

void ProbeDirectMode::ValidateDmabufImport(vr::SharedTextureHandle_t handle)
{
    auto *rm = vr::VRIPCResourceManager();
    if (!rm)
        return;

    uint64_t ipc_handle = 0;
    bool ok = rm->RefResource(handle, &ipc_handle);
    PLOG("dmabuf-validate: RefResource(0x%" PRIx64 ") ok=%d ipc=0x%" PRIx64, (uint64_t)handle,
         (int)ok, ipc_handle);
    if (!ok)
        return;

    int fd = -1;
    ok = rm->ReceiveSharedFd(ipc_handle, &fd);
    PLOG("dmabuf-validate: ReceiveSharedFd ok=%d fd=%d", (int)ok, fd);
    if (!ok || fd < 0) {
        rm->UnrefResource(handle);
        return;
    }

    off_t size = lseek(fd, 0, SEEK_END);
    PLOG("dmabuf-validate: fd size=%lld bytes", (long long)size);

    auto &vk = GetVkProbe();
    if (vk.ready) {
        VkMemoryFdPropertiesKHR props{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        VkResult r = vk.pGetMemoryFdPropertiesKHR(
            vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &props);
        PLOG("dmabuf-validate: vkGetMemoryFdPropertiesKHR result=%d memoryTypeBits=0x%x  %s",
             (int)r, props.memoryTypeBits,
             r == VK_SUCCESS ? "<<< DMABUF IMPORT VIABLE >>>" : "<<< IMPORT FAILED >>>");
    } else {
        PLOG("dmabuf-validate: vulkan device unavailable, skipped fd property query");
    }

    close(fd);
    rm->UnrefResource(handle);
}

void ProbeDirectMode::DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle)
{
    PLOG("DestroySwapTextureSet: 0x%" PRIx64, (uint64_t)sharedTextureHandle);
    auto *rm = vr::VRIPCResourceManager();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = handle_to_set_.find(sharedTextureHandle);
    if (it == handle_to_set_.end())
        return;
    auto set_it = sets_.find(it->second);
    if (set_it != sets_.end()) {
        for (int i = 0; i < 3; ++i) {
            if (rm)
                rm->UnrefResource(set_it->second.handles[i]);
            handle_to_set_.erase(set_it->second.handles[i]);
        }
        sets_.erase(set_it);
    }
}

void ProbeDirectMode::DestroyAllSwapTextureSets(uint32_t unPid)
{
    PLOG("DestroyAllSwapTextureSets: pid=%u", unPid);
    auto *rm = vr::VRIPCResourceManager();
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sets_.begin(); it != sets_.end();) {
        if (it->second.pid == unPid) {
            for (int i = 0; i < 3; ++i) {
                if (rm)
                    rm->UnrefResource(it->second.handles[i]);
                handle_to_set_.erase(it->second.handles[i]);
            }
            it = sets_.erase(it);
        } else {
            ++it;
        }
    }
}

void ProbeDirectMode::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2],
                                                 uint32_t (*pIndices)[2])
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (int eye = 0; eye < 2; ++eye) {
        uint32_t index = 0;
        auto it = handle_to_set_.find(sharedTextureHandles[eye]);
        if (it != handle_to_set_.end()) {
            auto &set = sets_[it->second];
            set.next_index = (set.next_index + 1) % 3;
            index = set.next_index;
        }
        (*pIndices)[eye] = index;
    }
    static int logged = 0;
    if (logged++ < 5)
        PLOG("GetNextSwapTextureSetIndex: L=0x%" PRIx64 " R=0x%" PRIx64 " -> %u,%u",
             (uint64_t)sharedTextureHandles[0], (uint64_t)sharedTextureHandles[1],
             (*pIndices)[0], (*pIndices)[1]);
}

void ProbeDirectMode::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2])
{
    ++submit_count_;
    if (submit_count_ <= 10 || submit_count_ % 300 == 0) {
        PLOG("SubmitLayer #%" PRIu64 ": L=0x%" PRIx64 " R=0x%" PRIx64
             " boundsL=(%.2f,%.2f,%.2f,%.2f)",
             submit_count_, (uint64_t)perEye[0].hTexture, (uint64_t)perEye[1].hTexture,
             perEye[0].bounds.uMin, perEye[0].bounds.vMin, perEye[0].bounds.uMax,
             perEye[0].bounds.vMax);
    }
}

void ProbeDirectMode::Present(vr::SharedTextureHandle_t syncTexture)
{
    ++present_count_;
    double now = NowSec();
    double dt = now - last_present_sec_;
    last_present_sec_ = now;
    if (present_count_ <= 10 || present_count_ % 300 == 0) {
        PLOG("Present #%" PRIu64 ": sync=0x%" PRIx64 " dt=%.4fs (%.1f fps)", present_count_,
             (uint64_t)syncTexture, dt, dt > 0 ? 1.0 / dt : 0.0);
    }
}

void ProbeDirectMode::PostPresent(const Throttling_t *pThrottling)
{
    static int logged = 0;
    if (logged++ < 5)
        PLOG("PostPresent: throttle=%u predict=%u", pThrottling ? pThrottling->nFramesToThrottle : 0,
             pThrottling ? pThrottling->nAdditionalFramesToPredict : 0);
}
