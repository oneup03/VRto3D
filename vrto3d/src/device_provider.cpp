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

#define WIN32_LEAN_AND_MEAN
#include <algorithm>
#include <thread>
#ifdef _WIN32
#include <Windows.h>
#include <psapi.h>
#include <tchar.h>

#include "vrto3dlib/win32_helper.hpp"
#include "device_provider.h"
#include "dx11_renderer.h"
#include "direct_mode_component.h"
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vrto3dlib/linux_helper.hpp"
#include "device_provider.h"
#include "vk/vk_renderer.h"
#include "vk/direct_mode_component_vk.h"
#endif
#include "vr_recenter.h"

namespace {

// Sample the PID a few seconds after disconnect — most games take a moment
// to fully exit after their SteamVR connection drops. If the PID is gone,
// shut SteamVR down; otherwise the user still has the app running and we
// leave SteamVR alone.
void ScheduleAutoExitCheck(uint32_t pid)
{
    std::thread([pid]() {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (IsProcessRunning(pid)) {
            LOG() << "auto_exit: pid " << pid
                  << " still running after 5s, leaving SteamVR alone";
            return;
        }
        LOG() << "auto_exit: pid " << pid
              << " is gone, shutting down SteamVR";
        RequestSteamVRShutdown();
    }).detach();
}

}  // namespace

//-----------------------------------------------------------------------------
// Purpose: This is called by vrserver after it receives a pointer back from HmdDriverFactory.
// You should do your resources allocations here (**not** in the constructor).
//-----------------------------------------------------------------------------
vr::EVRInitError MyDeviceProvider::Init( vr::IVRDriverContext *pDriverContext )
{
#ifdef _WIN32
    global_mtx_ = CreateMutex(NULL, TRUE, kVRto3DMutexName);

    // Best-effort process-level DPI elevation. Usually fails because vrserver
    // already locked its awareness, in which case the per-thread elevation in
    // LeiaSrPresenter::WindowThreadLoop and the WM_DPICHANGED handler in
    // PresentWndProc cover us.
    using SetProcessDpiAwarenessContextFn = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (auto set_proc_ctx = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            set_proc_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }
#else
    // Single-instance guard: flock'd file in XDG_RUNTIME_DIR (mirrors the
    // Global\VRto3DDriver named mutex on Windows).
    {
        const char* run = getenv("XDG_RUNTIME_DIR");
        std::string lock_path = std::string(run ? run : "/tmp") + "/" + kVRto3DLockName;
        int fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd >= 0)
            flock(fd, LOCK_EX | LOCK_NB);  // advisory; keep fd for process lifetime
        global_mtx_ = (void*)(intptr_t)fd;
    }
#endif

    // We need to initialise our driver context to make calls to the server.
    // OpenVR provides a macro to do this for us.
    VR_INIT_SERVER_DRIVER_CONTEXT( pDriverContext );

    // First, initialize our hmd, which we'll later pass OpenVR a pointer to.
    my_hmd_device_ = std::make_unique< MockControllerDeviceDriver >();

    static const char kSerialNumber[] = "VRto3D-1234";

    // TrackedDeviceAdded returning true means we have had our device added to SteamVR.
    if (!vr::VRServerDriverHost()->TrackedDeviceAdded(kSerialNumber, vr::TrackedDeviceClass_HMD, my_hmd_device_.get()))
    {
        LOG() << "Failed to create hmd device!";
        return vr::VRInitError_Driver_Unknown;
    }

    // No DisplayRedirect sibling needed in direct mode — game eye textures
    // arrive via IVRDriverDirectModeComponent::SubmitLayer / Present rather
    // than IVRVirtualDisplay::Present.

    return vr::VRInitError_None;
}

//-----------------------------------------------------------------------------
// Purpose: Tells the runtime which version of the API we are targeting.
// Helper variables in the header you're using contain this information, which can be returned here.
//-----------------------------------------------------------------------------
const char *const *MyDeviceProvider::GetInterfaceVersions()
{
    return vr::k_InterfaceVersions;
}

//-----------------------------------------------------------------------------
// Purpose: This function is deprecated and never called. But, it must still be defined, or we can't compile.
//-----------------------------------------------------------------------------
bool MyDeviceProvider::ShouldBlockStandbyMode()
{
    return false;
}

//-----------------------------------------------------------------------------
// Purpose: This is called in the main loop of vrserver.
// Drivers *can* do work here, but should ensure this work is relatively inexpensive.
// A good thing to do here is poll for events from the runtime or applications
//-----------------------------------------------------------------------------
void MyDeviceProvider::RunFrame()
{
    if (wait_count_ > 0)
    {
        wait_count_--;
    }

    vr::VREvent_t vrEvent;
    while (vr::VRServerDriverHost()->PollNextEvent(&vrEvent, sizeof(vrEvent)))
    {
        // Connect-side events are NOT gated by wait_count_. When a mod (e.g.
        // RealVR) re-inits VR after an FoV/resolution change, the reconnect
        // event lands inside the post-disconnect cool-off window; dropping it
        // would leave the renderer stuck in paused_for_disconnect_ with no
        // further chance to resume.
        if (vrEvent.eventType == vr::VREvent_ProcessConnected ||
            vrEvent.eventType == vr::VREvent_ActionBindingReloaded ||
            vrEvent.eventType == vr::VREvent_SceneApplicationChanged ||
            vrEvent.eventType == vr::VREvent_Input_BindingLoadFailed ||
            vrEvent.eventType == vr::VREvent_Input_BindingLoadSuccessful ||
            vrEvent.eventType == vr::VREvent_Input_ActionManifestReloaded)
        {
            auto appName = GetProcessName(vrEvent.data.process.pid);
            auto lowerAppName = appName;
            std::transform(lowerAppName.begin(), lowerAppName.end(), lowerAppName.begin(), ::tolower);
            
#ifdef _WIN32
            const bool name_is_game = lowerAppName.find("exe") != std::string::npos;
#else
            // Native Linux games have no .exe suffix; Proton games keep it.
            const bool name_is_game = !lowerAppName.empty();
#endif
            if (Skip_Processes.find(lowerAppName) == Skip_Processes.end() && name_is_game)
            {
                app_name_ = appName;
                app_pid_ = vrEvent.data.process.pid;
                g_current_app_pid.store(app_pid_);
                LOG() << "AppName = " << app_name_.c_str();
                my_hmd_device_->LoadSettings(app_name_, app_pid_, vr::VREvent_ProcessConnected);
                // Resume the renderer (cleared by the prior ProcessDisconnected).
                if (auto* r = my_hmd_device_->GetRenderer()) r->OnAppConnect();
                wait_count_ = 500;
            }
        }
        else if ((vrEvent.eventType == vr::VREvent_ProcessDisconnected ||
                  vrEvent.eventType == vr::VREvent_Compositor_ApplicationNotResponding ||
                  vrEvent.eventType == vr::VREvent_SceneAppPipeDisconnected) &&
                 !app_name_.empty() && vrEvent.data.process.pid == app_pid_ && wait_count_ == 0)
        {
            LOG() << "Unload = " << app_name_.c_str();
            // Sample auto_exit + pid before LoadSettings / app_pid_ reset.
            // The actual liveness check + shutdown happens on a delayed
            // thread (ScheduleAutoExitCheck) so the game has time to fully
            // terminate after the SteamVR disconnect fires.
            const bool want_auto_exit =
                my_hmd_device_->GetStereoComponent() &&
                my_hmd_device_->GetStereoComponent()->GetConfig().auto_exit;
            const uint32_t pid_snapshot = app_pid_;
            my_hmd_device_->LoadSettings(app_name_, app_pid_, vr::VREvent_ProcessDisconnected);

            // Drop the departed game's swap-texture handles BEFORE we
            // continue submitting frames. The compositor doesn't always call
            // DestroySwapTextureSet on app exit, leaving stale entries in
            // handle_map_ that the renderer would try to import — and that
            // import was the trigger for the DEVICE_REMOVED cascade observed
            // in crash logs. Pause the renderer so it stops trying until the
            // next ProcessConnected.
            if (auto* dmc = my_hmd_device_->GetDirectModeComponent()) {
                dmc->DestroyAllSwapTextureSets(pid_snapshot);
            }
            if (auto* r = my_hmd_device_->GetRenderer()) r->OnAppDisconnect();

            app_name_ = "";
            app_pid_ = 0;
            g_current_app_pid.store(0);
            wait_count_ = 500;
            if (want_auto_exit) {
                LOG() << "auto_exit: scheduling check for pid " << pid_snapshot;
                ScheduleAutoExitCheck(pid_snapshot);
            }
        }
    }

    // VREvent_Quit isn't delivered via the driver-host event stream — it
    // goes to *client* apps over their IPC pipe. Our TriggerOpenVRRecenter
    // VR_Init registers us as a Background client, so we have to drain
    // that client-side queue and ack quit, or vrserver waits 5s for us
    // and force-kills the process.
    vrto3d::PumpOpenVRClientEvents();
}

//-----------------------------------------------------------------------------
// Purpose: This function is called when the system enters a period of inactivity.
// The devices might want to turn off their displays or go into a low power mode to preserve them.
//-----------------------------------------------------------------------------
void MyDeviceProvider::EnterStandby()
{
}

//-----------------------------------------------------------------------------
// Purpose: This function is called after the system has been in a period of inactivity, and is waking up again.
// Turn back on the displays or devices here.
//-----------------------------------------------------------------------------
void MyDeviceProvider::LeaveStandby()
{
}

//-----------------------------------------------------------------------------
// Purpose: This function is called just before the driver is unloaded from vrserver.
// Drivers should free whatever resources they have acquired over the session here.
// Any calls to the server is guaranteed to be valid before this, but not after it has been called.
//-----------------------------------------------------------------------------
void MyDeviceProvider::Cleanup()
{
#ifdef _WIN32
    if (global_mtx_)
    {
        CloseHandle((HANDLE)global_mtx_);
    }
#else
    if ((intptr_t)global_mtx_ >= 0)
    {
        close((int)(intptr_t)global_mtx_);
    }
#endif

    // Our controller devices will have already deactivated. Let's now destroy them.
    my_hmd_device_ = nullptr;

    // Tear down the background OpenVR client session we used for chaperone
    // recenters. vrserver flags an unclean exit if a VR_Init'd client never
    // VR_Shutdown's. This is the last hook called before our DLL unloads.
    vrto3d::ShutdownOpenVRClient();
}

