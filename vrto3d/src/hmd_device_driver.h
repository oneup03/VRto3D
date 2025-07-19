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
#include "openvr_driver.h"
#include <atomic>
#include <thread>
#include <shared_mutex>
#include <string>

#include "json_manager.h"

 // Forward declare XINPUT_STATE
struct _XINPUT_STATE;
typedef _XINPUT_STATE XINPUT_STATE;


class StereoDisplayComponent : public vr::IVRDisplayComponent
{
public:
    explicit StereoDisplayComponent( const StereoDisplayDriverConfiguration &config );

    // ----- Functions to override vr::IVRDisplayComponent -----
    bool IsDisplayOnDesktop() override;
    bool IsDisplayRealDisplay() override;
    void GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight ) override;
    void GetEyeOutputViewport( vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight ) override;
    void GetProjectionRaw( vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom ) override;
    vr::DistortionCoordinates_t ComputeDistortion( vr::EVREye eEye, float fU, float fV ) override;
    bool ComputeInverseDistortion(vr::HmdVector2_t* pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV) override;
    void GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight ) override;
    StereoDisplayDriverConfiguration GetConfig();
    void AdjustDepth(float new_depth, bool is_delta, uint32_t device_index);
    void AdjustConvergence(float new_conv, bool is_delta, uint32_t device_index);
    float GetDepth();
    float GetConvergence();
    std::string CheckUserSettings(uint32_t device_index);
    void AdjustSensitivity(float delta);
    void AdjustRadius(float delta);
    void SetHeight();
    void SetReset();
    void LoadSettings(StereoDisplayDriverConfiguration& config, uint32_t device_index);
    void ResetProjection(uint32_t device_index);

private:
    StereoDisplayDriverConfiguration config_;
    std::atomic< float > depth_;
    std::atomic< float > convergence_;

    std::shared_mutex  cfg_mutex_;
};

//-----------------------------------------------------------------------------
// Purpose: Represents a Mock HMD in the system
//-----------------------------------------------------------------------------
class MockControllerDeviceDriver : public vr::ITrackedDeviceServerDriver
{
public:
    MockControllerDeviceDriver();
    vr::EVRInitError Activate( uint32_t unObjectId ) override;
    void EnterStandby() override;
    void *GetComponent( const char *pchComponentNameAndVersion ) override;
    void DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize ) override;
    vr::DriverPose_t GetPose() override;
    void Deactivate() override;

    void OpenTrackThread();
    void PoseUpdateThread();
    void PollHotkeysThread();
    void FocusUpdateThread();
    void AutoDepthThread();

    void LoadSettings(const std::string& app_name, uint32_t app_pid, vr::EVREventType status);

private:
    std::unique_ptr< StereoDisplayComponent > stereo_display_component_;

    std::string stereo_model_number_;
    std::string stereo_serial_number_;

    std::string app_name_;
    std::string prev_name_;
    std::atomic< uint32_t > app_pid_;
    std::atomic< bool > app_updated_;
    std::atomic< bool > no_profile_;

    std::atomic< bool > is_active_;
    std::atomic< uint32_t > device_index_;
    std::atomic< bool > is_on_top_;
    std::atomic< bool > take_screenshot_;
    std::atomic< bool > use_auto_depth_;

    std::mutex pose_mutex_;
    vr::DriverPose_t curr_pose_;

    std::thread pose_thread_;
    std::thread hotkey_thread_;
    std::thread focus_thread_;
    std::thread depth_thread_;
    std::thread track_thread_;

    vr::HmdQuaternion_t open_track_att_;
    std::shared_mutex  trk_mutex_;
};
