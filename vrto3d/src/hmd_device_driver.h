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
#include <vector>

enum MyComponent
{
	MyComponent_system_touch,
	MyComponent_system_click,

	MyComponent_MAX
};

struct StereoDisplayDriverConfiguration
{
	int32_t window_x;
	int32_t window_y;

	int32_t window_width;
	int32_t window_height;

	int32_t render_width;
	int32_t render_height;

	float aspect_ratio;
	float fov;
	float depth;
	float convergence;

	bool tab_enable;
	bool half_enable;
	bool reverse_enable;
	bool ss_enable;
	bool hdr_enable;
	bool depth_gauge;

	float ss_scale;
	float display_latency;
	float display_frequency;

	bool ctrl_enable;
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
	std::vector<bool> store_xinput;
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
	void AdjustPitch(vr::HmdQuaternion_t& qRotation, float& currentPitch);

private:
	StereoDisplayDriverConfiguration config_;
	std::atomic< float > depth_;
	std::atomic< float > convergence_;
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
	void SaveDepthConv();

private:
	std::unique_ptr< StereoDisplayComponent > stereo_display_component_;

	std::string stereo_model_number_;
	std::string stereo_serial_number_;

	std::array< vr::VRInputComponentHandle_t, MyComponent_MAX > my_input_handles_{};
	std::atomic< bool > is_active_;
	std::atomic< uint32_t > device_index_;

	std::thread pose_update_thread_;
};
