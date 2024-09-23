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

#include <array>
#include <string>

#include "openvr_driver.h"
#include <atomic>
#include <thread>
#include <shared_mutex>
#include <vector>

 // Forward declare XINPUT_STATE
struct _XINPUT_STATE;
typedef _XINPUT_STATE XINPUT_STATE;

struct StereoDisplayDriverConfiguration
{
    int32_t window_x;
    int32_t window_y;

    int32_t window_width;
    int32_t window_height;

    int32_t render_width;
    int32_t render_height;

    float hmd_height;

    float aspect_ratio;
    float fov;
    float depth;
    float convergence;
    bool disable_hotkeys;

    bool tab_enable;
    bool reverse_enable;
    bool depth_gauge;
    bool debug_enable;

    float display_latency;
    float display_frequency;
    int sleep_count_max;

    bool pitch_enable;
    bool yaw_enable;
    int32_t pose_reset_key;
    bool reset_xinput;
    bool pose_reset;
    int32_t ctrl_toggle_key;
    bool ctrl_xinput;
    float pitch_radius;
    float ctrl_deadzone;
    float ctrl_sensitivity;

    int32_t num_user_settings;
    std::vector<int32_t> user_load_key;
    std::vector<int32_t> user_store_key;
    std::vector<int32_t> user_key_type;
    std::vector<float> user_depth;
    std::vector<float> user_convergence;
    std::vector<float> prev_depth;
    std::vector<float> prev_convergence;
    std::vector<bool> was_held;
    std::vector<bool> load_xinput;
    std::vector<int32_t> sleep_count;
};

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
    void CheckUserSettings(uint32_t device_index);
    void AdjustSensitivity(float delta);
    void AdjustRadius(float delta);
    void SetHeight();
    void SetReset();
    void LoadSettings(const std::string& app_name, uint32_t device_index);
    void LoadDefaults(uint32_t device_index);

private:
    StereoDisplayDriverConfiguration config_;
    StereoDisplayDriverConfiguration def_config_;
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

    void PoseUpdateThread();
    void PollHotkeysThread();
    void FocusUpdateThread();

    void LoadSettings(const std::string& app_name);
    void SaveSettings();

private:
    std::unique_ptr< StereoDisplayComponent > stereo_display_component_;

    std::string stereo_model_number_;
    std::string stereo_serial_number_;

    std::string app_name_;

    std::atomic< bool > is_active_;
    std::atomic< uint32_t > device_index_;
    std::atomic< bool > is_on_top_;

    std::mutex pose_mutex_;
    vr::DriverPose_t curr_pose_;

    std::thread pose_thread_;
    std::thread hotkey_thread_;
    std::thread focus_thread_;
};
