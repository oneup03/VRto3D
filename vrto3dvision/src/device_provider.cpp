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
#include "device_provider.h"

#include "driverlog.h"

#include <sstream>
#include <Psapi.h>
#include <windows.h>


//-----------------------------------------------------------------------------
// Purpose: This is called by vrserver after it receives a pointer back from HmdDriverFactory.
// You should do your resources allocations here (**not** in the constructor).
//-----------------------------------------------------------------------------
vr::EVRInitError OVR_DeviceProvider::Init( vr::IVRDriverContext *pDriverContext )
{
	// We need to initialise our driver context to make calls to the server.
	// OpenVR provides a macro to do this for us.
	VR_INIT_SERVER_DRIVER_CONTEXT( pDriverContext );

	{
		RenderHelper renderHelper;
		if (!renderHelper.hasGPU())
		{
			DriverLog("OpenVR_HMDDriver: ERROR: Initialization failed, no GPU found.\n");
			return vr::VRInitError_Driver_Failed;
		}
	}

	// First, initialize our hmd, which we'll later pass OpenVR a pointer to.
	my_hmd_device_ = std::make_unique< OVR_3DV_Driver >();

	// TrackedDeviceAdded returning true means we have had our device added to SteamVR.
	if (!vr::VRServerDriverHost()->TrackedDeviceAdded("VRto3DV-5678", vr::TrackedDeviceClass_HMD, my_hmd_device_.get()))
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
const char *const *OVR_DeviceProvider::GetInterfaceVersions()
{
	return vr::k_InterfaceVersions;
}

//-----------------------------------------------------------------------------
// Purpose: This function is deprecated and never called. But, it must still be defined, or we can't compile.
//-----------------------------------------------------------------------------
bool OVR_DeviceProvider::ShouldBlockStandbyMode()
{
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: This is called in the main loop of vrserver.
// Drivers *can* do work here, but should ensure this work is relatively inexpensive.
// A good thing to do here is poll for events from the runtime or applications
//-----------------------------------------------------------------------------
void OVR_DeviceProvider::RunFrame()
{
    vr::VREvent_t vrEvent;
    while (vr::VRServerDriverHost()->PollNextEvent(&vrEvent, sizeof(vrEvent)))
    {
        if (vrEvent.eventType == vr::VREvent_SceneApplicationChanged)
        {
            vr::VREvent_Process_t pe = vrEvent.data.process;

            uint32_t    pid = pe.pid;
            std::string procname;
            if (pid > 0)
            {
                HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
                if (!handle)
                {
                    DriverLog(("OpenProcess for pid " + std::to_string(pid) + " failed!").c_str());
                }
                std::vector<TCHAR> buf(MAX_PATH);
                DWORD              ret = GetModuleFileNameEx(handle, 0, &buf[0], (DWORD)buf.size());
                if (!ret)
                {
                    DriverLog(("GetModuleFileNameEx failed: " + std::to_string(GetLastError())).c_str());
                }
                //procname = &buf[0];
            }

            std::stringstream ss;
            ss << "Scene App changed: pid: " << pid << " " << procname << "\n";
            ss << "oldpid: " << pe.oldPid << " forced: " << pe.bForced << " connectionlost: " << pe.bConnectionLost << "\n";
            DriverLog(ss.str().c_str());
        }
    }
}

//-----------------------------------------------------------------------------
// Purpose: This function is called when the system enters a period of inactivity.
// The devices might want to turn off their displays or go into a low power mode to preserve them.
//-----------------------------------------------------------------------------
void OVR_DeviceProvider::EnterStandby()
{
}

//-----------------------------------------------------------------------------
// Purpose: This function is called after the system has been in a period of inactivity, and is waking up again.
// Turn back on the displays or devices here.
//-----------------------------------------------------------------------------
void OVR_DeviceProvider::LeaveStandby()
{
}

//-----------------------------------------------------------------------------
// Purpose: This function is called just before the driver is unloaded from vrserver.
// Drivers should free whatever resources they have acquired over the session here.
// Any calls to the server is guaranteed to be valid before this, but not after it has been called.
//-----------------------------------------------------------------------------
void OVR_DeviceProvider::Cleanup()
{
	// Our controller devices will have already deactivated. Let's now destroy them.
	my_hmd_device_ = nullptr;
}