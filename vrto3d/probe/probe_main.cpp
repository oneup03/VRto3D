/*
 * VRto3D Linux port — Milestone 0 probe driver.
 *
 * Minimal SteamVR HMD driver whose only job is to discover, on Linux, whether
 * vrcompositor drives a driver-side IVRDriverDirectModeComponent, and to log
 * everything about the attempt. No rendering, no window, no config.
 */
#include "probe_direct_mode.h"
#include "probe_log.h"

#include <openvr_driver.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kEyeWidth = 1280;
constexpr uint32_t kEyeHeight = 720;
constexpr float kDisplayHz = 60.0f;

// ---------------------------------------------------------------------------
class ProbeDisplay : public vr::IVRDisplayComponent
{
public:
    void GetWindowBounds(int32_t *pnX, int32_t *pnY, uint32_t *pnWidth,
                         uint32_t *pnHeight) override
    {
        PLOG("IVRDisplayComponent::GetWindowBounds called");
        *pnX = 0;
        *pnY = 0;
        *pnWidth = kEyeWidth * 2;
        *pnHeight = kEyeHeight;
    }
    bool IsDisplayOnDesktop() override
    {
        PLOG("IVRDisplayComponent::IsDisplayOnDesktop called -> false");
        return false;
    }
    bool IsDisplayRealDisplay() override
    {
        PLOG("IVRDisplayComponent::IsDisplayRealDisplay called -> false");
        return false;
    }
    void GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight) override
    {
        *pnWidth = kEyeWidth;
        *pnHeight = kEyeHeight;
    }
    void GetEyeOutputViewport(vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth,
                              uint32_t *pnHeight) override
    {
        *pnY = 0;
        *pnWidth = kEyeWidth;
        *pnHeight = kEyeHeight;
        *pnX = (eEye == vr::Eye_Left) ? 0 : kEyeWidth;
    }
    void GetProjectionRaw(vr::EVREye, float *pfLeft, float *pfRight, float *pfTop,
                          float *pfBottom) override
    {
        *pfLeft = -1.0f;
        *pfRight = 1.0f;
        *pfTop = -1.0f;
        *pfBottom = 1.0f;
    }
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye, float fU, float fV) override
    {
        vr::DistortionCoordinates_t c{};
        c.rfRed[0] = c.rfGreen[0] = c.rfBlue[0] = fU;
        c.rfRed[1] = c.rfGreen[1] = c.rfBlue[1] = fV;
        return c;
    }
    bool ComputeInverseDistortion(vr::HmdVector2_t *pResult, vr::EVREye, uint32_t, float fU,
                                  float fV) override
    {
        if (pResult) {
            pResult->v[0] = fU;
            pResult->v[1] = fV;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
class ProbeHmd : public vr::ITrackedDeviceServerDriver
{
public:
    vr::EVRInitError Activate(uint32_t unObjectId) override
    {
        device_index_ = unObjectId;
        PLOG("HMD Activate: device index %u", unObjectId);

        auto *vrp = vr::VRProperties();
        vr::PropertyContainerHandle_t c = vrp->TrackedDeviceToPropertyContainer(unObjectId);
        vrp->SetStringProperty(c, vr::Prop_ModelNumber_String, "VRto3D_Probe");
        vrp->SetStringProperty(c, vr::Prop_ManufacturerName_String, "VRto3D");
        vrp->SetFloatProperty(c, vr::Prop_UserIpdMeters_Float, 0.063f);
        vrp->SetFloatProperty(c, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
        vrp->SetFloatProperty(c, vr::Prop_DisplayFrequency_Float, kDisplayHz);
        vrp->SetFloatProperty(c, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.008f);
        vrp->SetFloatProperty(c, vr::Prop_SecondsFromPhotonsToVblank_Float, 0.0f);
        vrp->SetBoolProperty(c, vr::Prop_ReportsTimeSinceVSync_Bool, false);
        vrp->SetBoolProperty(c, vr::Prop_IsOnDesktop_Bool, false);
        vrp->SetBoolProperty(c, vr::Prop_DisplayDebugMode_Bool, true);
        vrp->SetBoolProperty(c, vr::Prop_HasDriverDirectModeComponent_Bool, true);
        // Deliberately NOT setting Prop_GraphicsAdapterLuid_Uint64 — it's a
        // Windows/DXGI concept. Whether its absence matters on Linux is one of
        // the things this probe exists to find out.
        vrp->SetBoolProperty(c, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool, true);
        vrp->SetFloatProperty(c, vr::Prop_DashboardScale_Float, 1.0f);

        running_.store(true);
        pose_thread_ = std::thread(&ProbeHmd::PoseThread, this);
        vsync_thread_ = std::thread(&ProbeHmd::VsyncThread, this);
        return vr::VRInitError_None;
    }

    void Deactivate() override
    {
        PLOG("HMD Deactivate");
        running_.store(false);
        if (pose_thread_.joinable())
            pose_thread_.join();
        if (vsync_thread_.joinable())
            vsync_thread_.join();
        device_index_ = vr::k_unTrackedDeviceIndexInvalid;
    }

    void EnterStandby() override { PLOG("HMD EnterStandby"); }

    void *GetComponent(const char *pchComponentNameAndVersion) override
    {
        PLOG("GetComponent(\"%s\")", pchComponentNameAndVersion);
        if (strcmp(pchComponentNameAndVersion, vr::IVRDisplayComponent_Version) == 0)
            return &display_;
        if (strcmp(pchComponentNameAndVersion, vr::IVRDriverDirectModeComponent_Version) == 0) {
            PLOG(">>> compositor requested IVRDriverDirectModeComponent — direct mode is being wired up <<<");
            return &direct_mode_;
        }
        return nullptr;
    }

    void DebugRequest(const char *, char *pchResponseBuffer, uint32_t unResponseBufferSize) override
    {
        if (unResponseBufferSize > 0)
            pchResponseBuffer[0] = 0;
    }

    vr::DriverPose_t GetPose() override
    {
        vr::DriverPose_t pose{};
        pose.poseIsValid = true;
        pose.deviceIsConnected = true;
        pose.result = vr::TrackingResult_Running_OK;
        pose.qWorldFromDriverRotation.w = 1.0;
        pose.qDriverFromHeadRotation.w = 1.0;
        pose.qRotation.w = 1.0;
        pose.vecPosition[1] = 1.6; // standing eye height
        return pose;
    }

private:
    void PoseThread()
    {
        while (running_.load()) {
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(device_index_, GetPose(),
                                                               sizeof(vr::DriverPose_t));
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    void VsyncThread()
    {
        const auto interval =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / kDisplayHz));
        auto next = std::chrono::steady_clock::now();
        while (running_.load()) {
            vr::VRServerDriverHost()->VsyncEvent(0.0);
            next += interval;
            std::this_thread::sleep_until(next);
        }
    }

    ProbeDisplay display_;
    ProbeDirectMode direct_mode_;
    std::atomic<bool> running_{false};
    std::thread pose_thread_;
    std::thread vsync_thread_;
    uint32_t device_index_ = vr::k_unTrackedDeviceIndexInvalid;
};

// ---------------------------------------------------------------------------
class ProbeProvider : public vr::IServerTrackedDeviceProvider
{
public:
    vr::EVRInitError Init(vr::IVRDriverContext *pDriverContext) override
    {
        VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
        ProbeLog::Get().SetDriverLogReady(true);

        PLOG("=================================================================");
        PLOG("vrto3d_probe Init (pid %d)", (int)getpid());
        PLOG("env: DISPLAY=%s WAYLAND_DISPLAY=%s XDG_RUNTIME_DIR=%s XDG_SESSION_TYPE=%s",
             getenv("DISPLAY") ? getenv("DISPLAY") : "(null)",
             getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(null)",
             getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(null)",
             getenv("XDG_SESSION_TYPE") ? getenv("XDG_SESSION_TYPE") : "(null)");
        PLOG("env: PRESSURE_VESSEL_RUNTIME=%s STEAM_RUNTIME=%s",
             getenv("PRESSURE_VESSEL_RUNTIME") ? getenv("PRESSURE_VESSEL_RUNTIME") : "(null)",
             getenv("STEAM_RUNTIME") ? getenv("STEAM_RUNTIME") : "(null)");

        LogIpcResourceManagerState();

        vr::VRServerDriverHost()->TrackedDeviceAdded("VRto3D_Probe_0001",
                                                     vr::TrackedDeviceClass_HMD, &hmd_);
        return vr::VRInitError_None;
    }

    void Cleanup() override
    {
        PLOG("Provider Cleanup");
        ProbeLog::Get().SetDriverLogReady(false);
    }

    const char *const *GetInterfaceVersions() override { return vr::k_InterfaceVersions; }

    void RunFrame() override
    {
        vr::VREvent_t event;
        while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
            // Just drain; specific event handling is not the probe's job.
        }
    }

    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

private:
    void LogIpcResourceManagerState()
    {
        auto *rm = vr::VRIPCResourceManager();
        if (!rm) {
            PLOG("VRIPCResourceManager: NULL at Init (may initialize async — will retry in "
                 "CreateSwapTextureSet)");
            return;
        }
        PLOG("VRIPCResourceManager: available");

        uint32_t format_count = 0;
        if (rm->GetDmabufFormats(&format_count, nullptr) && format_count > 0) {
            std::vector<uint32_t> formats(format_count);
            if (rm->GetDmabufFormats(&format_count, formats.data())) {
                for (uint32_t i = 0; i < format_count && i < 32; ++i) {
                    char fourcc[5] = {(char)(formats[i] & 0xff), (char)((formats[i] >> 8) & 0xff),
                                      (char)((formats[i] >> 16) & 0xff),
                                      (char)((formats[i] >> 24) & 0xff), 0};
                    uint32_t mod_count = 0;
                    rm->GetDmabufModifiers(vr::VRApplication_Other, formats[i], &mod_count,
                                           nullptr);
                    PLOG("dmabuf format[%u]: 0x%08x '%s' (%u modifiers)", i, formats[i], fourcc,
                         mod_count);
                }
            }
        } else {
            PLOG("GetDmabufFormats: failed or zero formats");
        }
    }

    ProbeHmd hmd_;
};

ProbeProvider g_provider;

} // namespace

// ---------------------------------------------------------------------------
extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(
    const char *pInterfaceName, int *pReturnCode)
{
    PLOG("HmdDriverFactory(\"%s\")", pInterfaceName);
    if (strcmp(pInterfaceName, vr::IServerTrackedDeviceProvider_Version) == 0)
        return &g_provider;
    if (pReturnCode)
        *pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
