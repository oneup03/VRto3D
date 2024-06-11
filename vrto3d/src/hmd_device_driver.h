//============ Copyright (c) Valve Corporation, All rights reserved. ============
#pragma once

#include <array>
#include <string>

#include "openvr_driver.h"
#include <atomic>
#include <thread>

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
	bool ss_enable;
	bool hdr_enable;

	float ss_scale;
	float display_latency;
	float display_frequency;
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

private:
	StereoDisplayDriverConfiguration config_;
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

	void MyRunFrame();
	void PoseUpdateThread();

private:
	std::unique_ptr< StereoDisplayComponent > stereo_display_component_;

	std::string stereo_model_number_;
	std::string stereo_serial_number_;

	std::array< vr::VRInputComponentHandle_t, MyComponent_MAX > my_input_handles_{};
	std::atomic< bool > is_active_;
	std::atomic< uint32_t > device_index_;
	std::atomic< int > frame_number_;

	std::thread pose_update_thread_;
};
