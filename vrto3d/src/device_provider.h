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

#include <chrono>
#include <string>

#include "hmd_device_driver.h"
#include "openvr_driver.h"

// make sure your class is publicly inheriting vr::IServerTrackedDeviceProvider!
class MyDeviceProvider : public vr::IServerTrackedDeviceProvider
{
public:
    vr::EVRInitError Init( vr::IVRDriverContext *pDriverContext ) override;
    const char *const *GetInterfaceVersions() override;

    void RunFrame() override;

    bool ShouldBlockStandbyMode() override;
    void EnterStandby() override;
    void LeaveStandby() override;

    void Cleanup() override;

private:
    std::unique_ptr<MockControllerDeviceDriver> my_hmd_device_;

    void* global_mtx_;

    std::string app_name_;
    uint32_t app_pid_ = 0;
    uint32_t wait_count_ = 0;

    // Faulted-session exit watch. Set to the departed game's pid when a
    // session that never produced a frame disconnects; RunFrame then polls
    // until that process is really gone and shuts SteamVR down. Unlike
    // ScheduleAutoExitCheck's single delayed sample, this keeps checking —
    // a game whose VR plugin faulted routinely drops its SteamVR connection
    // while the game itself runs on flat for hours, and one sample would
    // strand vrserver for the rest of that session. Polled from RunFrame
    // rather than a detached thread so it dies with the driver.
    uint32_t faulted_exit_pid_ = 0;
    std::chrono::steady_clock::time_point faulted_next_check_{};
};