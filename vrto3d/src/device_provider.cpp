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

#include "device_provider.h"
#include "driverlog.h"

//-----------------------------------------------------------------------------
// Purpose: This is called by vrserver after it receives a pointer back from HmdDriverFactory.
// You should do your resources allocations here (**not** in the constructor).
//-----------------------------------------------------------------------------
vr::EVRInitError MyDeviceProvider::Init( vr::IVRDriverContext *pDriverContext )
{
    global_mtx_ = CreateMutex(NULL, TRUE, L"Global\\VRto3DDriver");

    // We need to initialise our driver context to make calls to the server.
    // OpenVR provides a macro to do this for us.
    VR_INIT_SERVER_DRIVER_CONTEXT( pDriverContext );

    // First, initialize our hmd, which we'll later pass OpenVR a pointer to.
    my_hmd_device_ = std::make_unique< MockControllerDeviceDriver >();

    // TrackedDeviceAdded returning true means we have had our device added to SteamVR.
    if (!vr::VRServerDriverHost()->TrackedDeviceAdded("VRto3D-1234", vr::TrackedDeviceClass_HMD, my_hmd_device_.get()))
    {
        DriverLog( "Failed to create hmd device!" );
        return vr::VRInitError_Driver_Unknown;
    }

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
    vr::VREvent_t vrEvent;
    while (vr::VRServerDriverHost()->PollNextEvent(&vrEvent, sizeof(vrEvent)))
    {
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
            
            if (skip_processes_.find(appName) == skip_processes_.end() &&
                lowerAppName.find("exe") != std::string::npos)
            {
                DriverLog("AppName = %s\n", appName.c_str());
                my_hmd_device_->LoadSettings(appName);
            }
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

//-----------------------------------------------------------------------------
// Purpose: To get the executable name given a process ID
//-----------------------------------------------------------------------------
std::string MyDeviceProvider::GetProcessName(uint32_t processID)
{
    std::string result = "<unknown>";

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, processID);
    if (hProcess)
    {
        TCHAR processName[MAX_PATH] = TEXT("<unknown>");
        DWORD size = MAX_PATH;

        // Try QueryFullProcessImageName for better accuracy and access support
        if (QueryFullProcessImageName(hProcess, 0, processName, &size))
        {
#ifdef UNICODE
            std::wstring ws(processName);
            result.assign(ws.begin(), ws.end());
#else
            result.assign(processName);
#endif
            std::filesystem::path fullPath = result;
            result = fullPath.filename().string();
        }

        CloseHandle(hProcess);
    }

    return result;
}