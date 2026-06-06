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
class DirectModeComponent;


class StereoDisplayComponent : public vr::IVRDisplayComponent
{
public:
    explicit StereoDisplayComponent( const StereoDisplayDriverConfiguration &config );

    // ----- Functions to override vr::IVRDisplayComponent -----
    // IVRDisplayComponent's distortion / viewport / bounds members are pure
    // virtuals and must be overridden for the class to be instantiable, but
    // in direct mode the compositor never calls them — the overrides below
    // are stubs.
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

    // Auto-depth: an external GPU pass measures the SbS frame's max disparity
    // each frame and feeds it back via FeedAutoDepthSample. While enabled, the
    // component lerps depth_ toward a target derived from `manual_depth_ *
    // (target / observed)`, capped by `manual_depth_` (the user's intended
    // ceiling). Toggling off snaps depth_ back to manual_depth_.
    bool  IsAutoDepthEnabled() const;
    void  SetAutoDepthEnabled(bool enabled);
    float GetManualDepth() const;
    float GetAutoDepthTargetDisparity() const;
    void  SetAutoDepthTargetDisparity(float frac);
    float GetAutoDepthSmoothing() const;
    void  SetAutoDepthSmoothing(float v);
    void  FeedAutoDepthSample(uint32_t max_disp_px, uint32_t eye_w_px);

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
    // Raise the reset flag so the next XInputUpdateThread tick consumes it
    // (zeros pitch/yaw, OpenTrack state, and clears the flag via
    // MockControllerDeviceDriver::ConsumePoseReset).
    void RequestPoseReset();
    void LoadSettings(StereoDisplayDriverConfiguration& config);
    void ResetProjection();
    void Init(uint32_t device_index);

private:
    // Push new depth into depth_ (atomic CAS) and the OpenVR
    // Prop_UserIpdMeters_Float property. Used by both manual and auto paths.
    void ApplyDepth(float new_depth);

    StereoDisplayDriverConfiguration config_;
    std::atomic< float > depth_;
    std::atomic< float > convergence_;
    std::atomic< float > fov_;
    std::atomic< uint32_t > device_index_;

    std::shared_mutex  cfg_mutex_;

    // UE3D Monitor Mode
    std::atomic< bool > monitor_mode_{ false };

    // Auto-depth state
    std::atomic< bool >  auto_depth_enabled_{ false };
    std::atomic< float > manual_depth_{ 0.1f };
    std::atomic< float > auto_depth_target_disparity_{ 0.005f };
    std::atomic< float > auto_depth_smoothing_{ 0.08f };
    // EMA of incoming disparity-fraction samples (input-side jitter filter).
    // -1 = uninitialized. Reset whenever auto-depth is toggled off.
    std::atomic< float > auto_depth_disp_ema_{ -1.0f };
};



//-----------------------------------------------------------------------------
// Purpose: Virtual HMD operating in SteamVR direct mode. The
// IVRDriverDirectModeComponent (held by direct_mode_component_) takes ownership
// of the per-eye texture exchange — game eye textures flow in via SubmitLayer
// rather than the compositor's IVRVirtualDisplay::Present path. Direct mode is
// what lets us bypass the compositor's lens-distortion / panel-mask pass that
// would otherwise carve black corners out of every frame.
//-----------------------------------------------------------------------------
class MockControllerDeviceDriver : public vr::ITrackedDeviceServerDriver
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

    void OpenTrackThread();
    void XInputUpdateThread();
    void PoseUpdateThread();
    void PollHotkeysThread();
    void MonitorModeThread();

    void LoadSettings(const std::string& app_name, uint32_t app_pid, vr::EVREventType status);
    void SetAsync(bool enable);

    // Pose-reset consumption point. Called by XInputUpdateThread (and the
    // OSD Recenter button path) once the XInput-derived pitch/yaw have been
    // zeroed: also zeros the cached OpenTrack attitude/position so disabling
    // OpenTrack — or pressing the pose-reset hotkey while it's on — doesn't
    // leave stale UDP-derived bias to be re-applied later. Finally clears the
    // pose_reset flag via StereoDisplayComponent::SetReset().
    void ConsumePoseReset();

    // Accessors used by VirtualDisplayDevice to reach shared config / focus
    // state without duplicating it.
    const StereoDisplayComponent* GetStereoComponent() const { return stereo_display_component_.get(); }
    StereoDisplayComponent*       GetStereoComponent()       { return stereo_display_component_.get(); }
    vrto3d::FocusContext          GetFocusContext();

    // Reach the renderer + direct-mode component so the device provider can
    // drain stale shared-texture handles on VREvent_ProcessDisconnected and
    // toggle the renderer's pause-on-disconnect circuit-breaker.
    Dx11Renderer*         GetRenderer()            { return renderer_.get(); }
    DirectModeComponent*  GetDirectModeComponent() { return direct_mode_component_.get(); }

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
    std::atomic< uint32_t > device_index_;         // HMD object id (from Activate)
    std::unique_ptr< Dx11Renderer > renderer_;
    std::unique_ptr< DirectModeComponent > direct_mode_component_;
    std::atomic< bool > is_on_top_;
    std::atomic< bool > man_on_top_;
    // Snapshot of man_on_top_ at the moment of ProcessDisconnected, so a
    // same-pid reconnect (e.g. RealVR re-init) can refocus immediately
    // only if the user actually had focus enabled before the disconnect.
    std::atomic< bool > focus_pre_disconnect_{ false };
    // Live mirror of stereo_display_component_->GetConfig().auto_focus.
    // Updated whenever a profile/config is (re)loaded so the presenter's
    // focus loop sees toggles immediately. Default true matches stereo_config.h.
    std::atomic< bool > auto_focus_{ true };
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
    std::thread monitor_thread_;
    std::thread track_thread_;

    vr::HmdQuaternion_t open_track_att_;
    std::array<double, 3> open_track_pos_;
    std::atomic< double > open_track_pose_sample_time_seconds_ = 0.0;
    std::mutex trk_mutex_;

    AccelaHamiltonRuntimeFilter track_filter_;
    bool track_filter_was_enabled_ = false;
};
