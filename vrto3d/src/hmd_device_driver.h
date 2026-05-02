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
#include <array>
#include <atomic>
#include <thread>
#include <shared_mutex>
#include <string>

#include "accela_hamilton_runtime.h"
#include "focus_context.h"
#include "vrto3dlib/json_manager.h"
#include "vrto3dlib/uevr_receiver.hpp"

 // Forward declare XINPUT_STATE
struct _XINPUT_STATE;
typedef _XINPUT_STATE XINPUT_STATE;

class Dx11Renderer;


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
    void AdjustDepth(float new_depth, bool is_delta);
    void AdjustConvergence(float new_conv, bool is_delta);
    void AdjustFoV(float new_fov);
    float GetDepth();
    float GetConvergence();
    float GetFoV();

    // UE3D Monitor Mode
    void SetMonitorMode(bool enabled);
    bool IsMonitorMode();

    std::string CheckUserSettings();
    void AdjustSensitivity(float delta);
    void AdjustRadius(float delta);
    void AdjustTrackFilterRotation(float delta);
    void AdjustTrackFilterTranslation(float delta);
    void AdjustTrackFilterRotationDeadzone(float delta);
    void AdjustTrackFilterTranslationDeadzone(float delta);
    void AdjustTrackFilterZoomSmoothing(float delta);
    void AdjustTrackFilterMaxZoom(float delta);
    void SetReset();
    void LoadSettings(StereoDisplayDriverConfiguration& config);
    void ResetProjection();
    void Init(uint32_t device_index);

private:
    StereoDisplayDriverConfiguration config_;
    std::atomic< float > depth_;
    std::atomic< float > convergence_;
    std::atomic< float > fov_;
    std::atomic< uint32_t > device_index_;

    std::shared_mutex  cfg_mutex_;

    // UE3D Monitor Mode
    std::atomic< bool > monitor_mode_{ false };
};



//-----------------------------------------------------------------------------
// Purpose: Represents a Mock HMD in the system. Also implements
// IVRVirtualDisplay so the same object can be registered twice with the
// same serial — once as TrackedDeviceClass_HMD and once as
// TrackedDeviceClass_DisplayRedirect. This matches the working WibbleWobbleVR
// pattern; registering two separate objects does not route composited frames.
//-----------------------------------------------------------------------------
class MockControllerDeviceDriver : public vr::ITrackedDeviceServerDriver,
                                    public vr::IVRVirtualDisplay
{
public:
    MockControllerDeviceDriver();
    ~MockControllerDeviceDriver();

    // ITrackedDeviceServerDriver
    vr::EVRInitError Activate( uint32_t unObjectId ) override;
    void EnterStandby() override;
    void *GetComponent( const char *pchComponentNameAndVersion ) override;
    void DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize ) override;
    vr::DriverPose_t GetPose() override;
    void Deactivate() override;

    // IVRVirtualDisplay
    void Present( const vr::PresentInfo_t *pPresentInfo, uint32_t unPresentInfoSize ) override;
    void WaitForPresent() override;
    bool GetTimeSinceLastVsync( float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter ) override;

    void OpenTrackThread();
    void XInputUpdateThread();
    void PoseUpdateThread();
    void PollHotkeysThread();
    void AutoDepthThread();

    void LoadSettings(const std::string& app_name, uint32_t app_pid, vr::EVREventType status);
    void SetAsync(bool enable);

    // Accessors used by VirtualDisplayDevice to reach shared config / focus
    // state without duplicating it.
    const StereoDisplayComponent* GetStereoComponent() const { return stereo_display_component_.get(); }
    StereoDisplayComponent*       GetStereoComponent()       { return stereo_display_component_.get(); }
    vrto3d::FocusContext          GetFocusContext();

private:
    std::unique_ptr< StereoDisplayComponent > stereo_display_component_;

    std::string stereo_model_number_;
    std::string stereo_serial_number_;
    std::string stereo_version_number_;

    std::string app_name_;
    std::string prev_name_;
    std::atomic< uint32_t > app_pid_;
    std::atomic< bool > app_updated_;
    std::atomic< bool > no_profile_;

    std::atomic< bool > is_active_;
    std::atomic< uint32_t > device_index_;         // HMD class object id (first Activate)
    std::atomic< uint32_t > display_redirect_index_{ vr::k_unTrackedDeviceIndexInvalid };
    std::unique_ptr< Dx11Renderer > renderer_;
    std::atomic< bool > is_on_top_;
    std::atomic< bool > man_on_top_;
    std::atomic< bool > ue3d_on_top_;
    std::atomic< bool > take_screenshot_;
    std::atomic< bool > launch_script_executed_;

    std::mutex pose_mutex_;
    vr::DriverPose_t curr_pose_;

    std::mutex controller_pose_mutex_;
    vr::HmdQuaternion_t controller_rotation_ = { 1.0, 0.0, 0.0, 0.0 };
    std::array<double, 3> controller_pos_offset_ = { 0.0, 0.0, 0.0 };
    std::atomic< double > xinput_pose_sample_time_seconds_ = 0.0;

    std::thread xinput_thread_;
    std::thread pose_thread_;
    std::thread hotkey_thread_;
    std::thread depth_thread_;
    std::thread track_thread_;

    vr::HmdQuaternion_t open_track_att_;
    std::array<double, 3> open_track_pos_;
    std::atomic< double > open_track_pose_sample_time_seconds_ = 0.0;
    std::mutex trk_mutex_;

    AccelaHamiltonRuntimeFilter track_filter_;
    bool track_filter_was_enabled_ = false;
};
