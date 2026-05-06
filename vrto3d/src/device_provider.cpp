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
#include <Windows.h>
#include <psapi.h>
#include <tchar.h>

#include "vrto3dlib/win32_helper.hpp"
#include "device_provider.h"

//-----------------------------------------------------------------------------
// Purpose: This is called by vrserver after it receives a pointer back from HmdDriverFactory.
// You should do your resources allocations here (**not** in the constructor).
//-----------------------------------------------------------------------------
vr::EVRInitError MyDeviceProvider::Init( vr::IVRDriverContext *pDriverContext )
{
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

    // Register the SAME object with the SAME serial number as a DisplayRedirect
    // device too. This is how SteamVR wires IVRVirtualDisplay::Present — the
    // compositor only routes composited frames through the DR class when it
    // finds a DR sibling that shares the active HMD's serial. Two separate
    // objects with different serials do not work (see WibbleWobbleVR for the
    // working reference pattern).
    // SteamVR returns false here because the serial is already registered,
    // but the DisplayRedirect class binding still takes effect — frames will
    // route through IVRVirtualDisplay::Present. Don't treat the false return
    // as an error.
    vr::VRServerDriverHost()->TrackedDeviceAdded(kSerialNumber,
                                                 vr::TrackedDeviceClass_DisplayRedirect,
                                                 my_hmd_device_.get());

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
        if ((vrEvent.eventType == vr::VREvent_ProcessConnected ||
            vrEvent.eventType == vr::VREvent_ActionBindingReloaded ||
            vrEvent.eventType == vr::VREvent_SceneApplicationChanged ||
            vrEvent.eventType == vr::VREvent_Input_BindingLoadFailed || 
            vrEvent.eventType == vr::VREvent_Input_BindingLoadSuccessful ||
            vrEvent.eventType == vr::VREvent_Input_ActionManifestReloaded) &&
            wait_count_ == 0)
        {
            auto appName = GetProcessName(vrEvent.data.process.pid);
            auto lowerAppName = appName;
            std::transform(lowerAppName.begin(), lowerAppName.end(), lowerAppName.begin(), ::tolower);
            
            if (Skip_Processes.find(appName) == Skip_Processes.end() &&
                lowerAppName.find("exe") != std::string::npos)
            {
                app_name_ = appName;
                app_pid_ = vrEvent.data.process.pid;
                LOG() << "AppName = " << app_name_.c_str();
                my_hmd_device_->LoadSettings(app_name_, app_pid_, vr::VREvent_ProcessConnected);
                wait_count_ = 500;
            }
        }
        else if ((vrEvent.eventType == vr::VREvent_ProcessDisconnected ||
                  vrEvent.eventType == vr::VREvent_Compositor_ApplicationNotResponding ||
                  vrEvent.eventType == vr::VREvent_SceneAppPipeDisconnected) &&
                 !app_name_.empty() && vrEvent.data.process.pid == app_pid_ && wait_count_ == 0)
        {
            LOG() << "Unload = " << app_name_.c_str();
            my_hmd_device_->LoadSettings(app_name_, app_pid_, vr::VREvent_ProcessDisconnected);
            app_name_ = "";
            app_pid_ = 0;
            wait_count_ = 500;
        }
    }
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
    if (global_mtx_)
    {
        CloseHandle((HANDLE)global_mtx_);
    }

    // Our controller devices will have already deactivated. Let's now destroy them.
    my_hmd_device_ = nullptr;
}

