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
#pragma once

#include <memory>
#include <string>
#include <unordered_set>

#include "hmd_device_driver.h"
#include "openvr_driver.h"

// make sure your class is publicly inheriting vr::IServerTrackedDeviceProvider!
class MyDeviceProvider : public vr::IServerTrackedDeviceProvider
{
public:
	vr::EVRInitError Init( vr::IVRDriverContext *pDriverContext ) override;
	const char *const *GetInterfaceVersions() override;

	void RunFrame() override;
	std::string GetProcessName(uint32_t processID);

	bool ShouldBlockStandbyMode() override;
	void EnterStandby() override;
	void LeaveStandby() override;

	void Cleanup() override;

private:
	std::unique_ptr<MockControllerDeviceDriver> my_hmd_device_;

	std::unordered_set<std::string> skip_processes_ = {
		"vrcompositor.exe",
        "vrserver.exe",
        "vrmonitor.exe",
        "vrstartup.exe",
        "removeusbhelper.exe",
		"restarthelper.exe",
        "vrcmd.exe",
        "vrdashboard.exe",
        "vrpathreg.exe",
        "vrwebhelper.exe",
		"vrprismhost.exe",
        "vrserverhelper.exe",
        "vrservice.exe",
        "vrurlhandler.exe",
		"steam.exe",
        "steamwebhelper.exe",
        "steamerrorreporter.exe",
        "steamservice.exe"
	};
};