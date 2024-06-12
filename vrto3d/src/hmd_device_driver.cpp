//============ Copyright (c) Valve Corporation, All rights reserved. ============
#include "hmd_device_driver.h"

#include "driverlog.h"
#include "vrmath.h"
#include <string>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <windows.h>
#include <Xinput.h>

// Link the XInput library
#pragma comment(lib, "XInput.lib")

// Load settings from default.vrsettings
static const char *stereo_main_settings_section = "driver_vrto3d";
static const char *stereo_display_settings_section = "vrto3d_display";

MockControllerDeviceDriver::MockControllerDeviceDriver()
{
	// Keep track of whether Activate() has been called
	is_active_ = false;

	char model_number[ 1024 ];
	vr::VRSettings()->GetString( stereo_main_settings_section, "model_number", model_number, sizeof( model_number ) );
	stereo_model_number_ = model_number;
	char serial_number[ 1024 ];
	vr::VRSettings()->GetString( stereo_main_settings_section, "serial_number", serial_number, sizeof( serial_number ) );
	stereo_serial_number_ = serial_number;

	DriverLog( "VRto3D Model Number: %s", stereo_model_number_.c_str() );
	DriverLog( "VRto3D Serial Number: %s", stereo_serial_number_.c_str() );

	// Display settings
	StereoDisplayDriverConfiguration display_configuration{};
	display_configuration.window_x = vr::VRSettings()->GetInt32( stereo_display_settings_section, "window_x" );
	display_configuration.window_y = vr::VRSettings()->GetInt32( stereo_display_settings_section, "window_y" );

	display_configuration.window_width = vr::VRSettings()->GetInt32( stereo_display_settings_section, "window_width" );
	display_configuration.window_height = vr::VRSettings()->GetInt32( stereo_display_settings_section, "window_height" );

	display_configuration.aspect_ratio = vr::VRSettings()->GetFloat(stereo_display_settings_section, "aspect_ratio");
	display_configuration.fov = vr::VRSettings()->GetFloat(stereo_display_settings_section, "fov");
	display_configuration.depth = vr::VRSettings()->GetFloat(stereo_display_settings_section, "depth");
	display_configuration.convergence = vr::VRSettings()->GetFloat(stereo_display_settings_section, "convergence");

	// Read user binds
	display_configuration.num_user_settings = vr::VRSettings()->GetInt32(stereo_display_settings_section, "num_user_settings");
	display_configuration.user_load_key.resize(display_configuration.num_user_settings);
	display_configuration.user_store_key.resize(display_configuration.num_user_settings);
	display_configuration.user_depth.resize(display_configuration.num_user_settings);
	display_configuration.user_convergence.resize(display_configuration.num_user_settings);
	for (int i = 0; i < display_configuration.num_user_settings; i++)
	{
		char user_key[1024];
		std::string temp = "user_load_key" + std::to_string(i + 1);
		vr::VRSettings()->GetString(stereo_display_settings_section, temp.c_str(), user_key, sizeof(user_key));
		display_configuration.user_load_key[i] = std::stoi(std::string(user_key), nullptr, 16);
		temp = "user_store_key" + std::to_string(i + 1);
		vr::VRSettings()->GetString(stereo_display_settings_section, temp.c_str(), user_key, sizeof(user_key));
		display_configuration.user_store_key[i] = std::stoi(std::string(user_key), nullptr, 16);
		temp = "user_depth" + std::to_string(i + 1);
		display_configuration.user_depth[i] = vr::VRSettings()->GetFloat(stereo_display_settings_section, temp.c_str());
		temp = "user_convergence" + std::to_string(i + 1);
		display_configuration.user_convergence[i] = vr::VRSettings()->GetFloat(stereo_display_settings_section, temp.c_str());
	}

	display_configuration.tab_enable = vr::VRSettings()->GetBool(stereo_display_settings_section, "tab_enable");
	display_configuration.half_enable = vr::VRSettings()->GetBool(stereo_display_settings_section, "half_enable");
	display_configuration.reverse_enable = vr::VRSettings()->GetBool(stereo_display_settings_section, "reverse_enable");
	display_configuration.ss_enable = vr::VRSettings()->GetBool(stereo_display_settings_section, "ss_enable");
	display_configuration.hdr_enable = vr::VRSettings()->GetBool(stereo_display_settings_section, "hdr_enable");

	display_configuration.ss_scale = vr::VRSettings()->GetFloat(stereo_display_settings_section, "ss_scale");
	display_configuration.display_latency = vr::VRSettings()->GetFloat(stereo_display_settings_section, "display_latency");
	display_configuration.display_frequency = vr::VRSettings()->GetFloat(stereo_display_settings_section, "display_frequency");

	int32_t half_width = display_configuration.half_enable ? 1 : 2;
	if (display_configuration.tab_enable)
	{
		display_configuration.render_width = display_configuration.window_width;
		display_configuration.render_height = display_configuration.window_height / half_width;
	}
	else
	{
		display_configuration.render_width = display_configuration.window_width / half_width;
		display_configuration.render_height = display_configuration.window_height;
	}

	// Instantiate our display component
	stereo_display_component_ = std::make_unique< StereoDisplayComponent >( display_configuration );
}

//-----------------------------------------------------------------------------
// Purpose: Initialize all settings and notify SteamVR
//-----------------------------------------------------------------------------
vr::EVRInitError MockControllerDeviceDriver::Activate( uint32_t unObjectId )
{
	device_index_ = unObjectId;
	is_active_ = true;

	// A list of properties available is contained in vr::ETrackedDeviceProperty.
	vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer( device_index_ );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_ModelNumber_String, stereo_model_number_.c_str() );
	vr::VRProperties()->SetStringProperty( container, vr::Prop_ManufacturerName_String, "VRto3D");
	vr::VRProperties()->SetStringProperty( container, vr::Prop_TrackingFirmwareVersion_String, "1.0");
	vr::VRProperties()->SetStringProperty( container, vr::Prop_HardwareRevision_String, "1.0");

	// Display settings
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_UserIpdMeters_Float, stereo_display_component_->GetConfig().depth);
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f);
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_DisplayFrequency_Float, stereo_display_component_->GetConfig().display_frequency);
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_SecondsFromVsyncToPhotons_Float, stereo_display_component_->GetConfig().display_latency);
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_IsOnDesktop_Bool, false);
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_DisplayDebugMode_Bool, true);
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_HasDriverDirectModeComponent_Bool, false);
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_Hmd_SupportsHDR10_Bool, stereo_display_component_->GetConfig().hdr_enable);
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_Hmd_AllowSupersampleFiltering_Bool, stereo_display_component_->GetConfig().ss_enable);

	// Set the chaperone JSON property
	// Get the current time
	std::time_t t = std::time(nullptr);
	std::tm tm;
	localtime_s(&tm, &t);
	// Construct the JSON string with variables
	std::stringstream ss;
	ss << R"(
        {
           "jsonid" : "chaperone_info",
           "universes" : [
              {
                 "collision_bounds" : [
                    [
                       [ -1.0, 0.0, -1.0 ],
                       [ -1.0, 3.0, -1.0 ],
                       [ -1.0, 3.0, 1.0 ],
                       [ -1.0, 0.0, 1.0 ]
                    ],
                    [
                       [ -1.0, 0.0, 1.0 ],
                       [ -1.0, 3.0, 1.0 ],
                       [ 1.0, 3.0, 1.0 ],
                       [ 1.0, 0.0, 1.0 ]
                    ],
                    [
                       [ 1.0, 0.0, 1.0 ],
                       [ 1.0, 3.0, 1.0 ],
                       [ 1.0, 3.0, -1.0 ],
                       [ 1.0, 0.0, -1.0 ]
                    ],
                    [
                       [ 1.0, 0.0, -1.0 ],
                       [ 1.0, 3.0, -1.0 ],
                       [ -1.0, 3.0, -1.0 ],
                       [ -1.0, 0.0, -1.0 ]
                    ]
                 ],
                 "play_area" : [ 2.0, 2.0 ],
                 "seated" : {
                    "translation" : [ 0.0, 0.0, 0.0 ],
                    "yaw" : 0.0
                 },
                 "standing" : {
                    "translation" : [ 0.0, 0.0, 0.0 ],
                    "yaw" : 0.0
                 },
                 "time" : ")" << std::put_time(&tm, "%a %b %d %H:%M:%S %Y") << R"(",
                 "universeID" : "64"
              }
           ],
           "version" : 5
        }
        )";
	// Convert the stringstream to a string
	std::string chaperoneJson = ss.str();
	// Set the chaperone JSON property
	vr::VRProperties()->SetStringProperty(container, vr::Prop_DriverProvidedChaperoneJson_String, chaperoneJson.c_str());
	vr::VRProperties()->SetUint64Property(container, vr::Prop_CurrentUniverseId_Uint64, 64);

	// Miscellaneous settings
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_WillDriftInYaw_Bool, false);
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_DeviceIsWireless_Bool, false);
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_DeviceIsCharging_Bool, false);
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_ContainsProximitySensor_Bool, false);
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_DeviceCanPowerOff_Bool, false);

	// Now let's set up our inputs
	// This tells the UI what to show the user for bindings for this controller,
	// As well as what default bindings should be for legacy apps.
	// Note, we can use the wildcard {<driver_name>} to match the root folder location
	// of our driver.
	vr::VRProperties()->SetStringProperty( container, vr::Prop_InputProfilePath_String, "{vrto3d}/input/vrto3d_profile.json" );

	// Let's set up handles for all of our components.
	// Even though these are also defined in our input profile,
	// We need to get handles to them to update the inputs.
	vr::VRDriverInput()->CreateBooleanComponent( container, "/input/system/touch", &my_input_handles_[ MyComponent_system_touch ] );
	vr::VRDriverInput()->CreateBooleanComponent( container, "/input/system/click", &my_input_handles_[ MyComponent_system_click ] );

	// Set supersample scale
	vr::VRSettings()->SetFloat(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_SupersampleScale_Float, stereo_display_component_->GetConfig().ss_scale);
	
	// Miscellaneous settings
	vr::VRSettings()->SetBool(vr::k_pch_DirectMode_Section, vr::k_pch_DirectMode_Enable_Bool, false);
	vr::VRSettings()->SetFloat(vr::k_pch_Power_Section, vr::k_pch_Power_TurnOffScreensTimeout_Float, 86400.0f);
	vr::VRSettings()->SetBool(vr::k_pch_Power_Section, vr::k_pch_Power_PauseCompositorOnStandby_Bool, false);
	vr::VRSettings()->SetBool(vr::k_pch_Dashboard_Section, vr::k_pch_Dashboard_EnableDashboard_Bool, false);
	vr::VRSettings()->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_EnableHomeApp, false);
	vr::VRSettings()->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_MirrorViewVisibility_Bool, false);
	vr::VRSettings()->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_EnableSafeMode, false);
	
	pose_update_thread_ = std::thread( &MockControllerDeviceDriver::PoseUpdateThread, this );

	return vr::VRInitError_None;
}

//-----------------------------------------------------------------------------
// Purpose: If you're an HMD, this is where you would return an implementation
// of vr::IVRDisplayComponent, vr::IVRVirtualDisplay or vr::IVRDirectModeComponent.
//-----------------------------------------------------------------------------
void *MockControllerDeviceDriver::GetComponent( const char *pchComponentNameAndVersion )
{
	if ( strcmp( pchComponentNameAndVersion, vr::IVRDisplayComponent_Version ) == 0 )
	{
		return stereo_display_component_.get();
	}

	return nullptr;
}

//-----------------------------------------------------------------------------
// Purpose: This is called by vrserver when a debug request has been made from an application to the driver.
// What is in the response and request is up to the application and driver to figure out themselves.
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize )
{
	if ( unResponseBufferSize >= 1 )
		pchResponseBuffer[ 0 ] = 0;
}


//-----------------------------------------------------------------------------
// Purpose: Static Pose, Depth and Convergence Hotkeys
//-----------------------------------------------------------------------------
vr::DriverPose_t MockControllerDeviceDriver::GetPose()
{
	vr::DriverPose_t pose = { 0 };

	pose.qWorldFromDriverRotation.w = 1.f;
	pose.qDriverFromHeadRotation.w = 1.f;

	pose.qRotation.w = 1.f;

	pose.vecPosition[ 0 ] = 0.0f;
	pose.vecPosition[ 1 ] = 1.0f;
	pose.vecPosition[ 2 ] = 0.0f;

	pose.poseIsValid = true;
	pose.deviceIsConnected = true;
	pose.result = vr::TrackingResult_Running_OK;

	// For HMDs we want to apply rotation/motion prediction
	pose.shouldApplyHeadModel = false;

	return pose;
}
void MockControllerDeviceDriver::PoseUpdateThread()
{
	XINPUT_STATE state;
	ZeroMemory(&state, sizeof(XINPUT_STATE));

	while ( is_active_ )
	{
		// Get the state of the first controller (index 0)
		DWORD dwResult = XInputGetState(0, &state);
		if (dwResult == ERROR_SUCCESS) {
			// Controller is connected
			// Check the state of the buttons
			if (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) {
				stereo_display_component_->AdjustDepth(0.001f, true, device_index_);
			}
		}

		// Inform the vrserver that our tracked device's pose has updated, giving it the pose returned by our GetPose().
		vr::VRServerDriverHost()->TrackedDevicePoseUpdated( device_index_, GetPose(), sizeof( vr::DriverPose_t ) );
		
		// Ctrl+F3 Decrease Depth
		if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F3) & 0x8000)) {
			stereo_display_component_->AdjustDepth(-0.001f, true, device_index_);
		}
		// Ctrl+F4 Increase Depth
		if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F4) & 0x8000)) {
			stereo_display_component_->AdjustDepth(0.001f, true, device_index_);
		}
		// Ctrl+F5 Decrease Convergence
		if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F5) & 0x8000)) {
			stereo_display_component_->AdjustConvergence(-0.001f, true, device_index_);
		}
		// Ctrl+F6 Increase Convergence
		if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F6) & 0x8000)) {
			stereo_display_component_->AdjustConvergence(0.001f, true, device_index_);
		}

		// Check User binds
		for (int i = 0; i < stereo_display_component_->GetConfig().num_user_settings; i++)
		{
			if (GetAsyncKeyState(stereo_display_component_->GetConfig().user_load_key[i]) & 0x8000)
			{
				stereo_display_component_->AdjustDepth(stereo_display_component_->GetConfig().user_depth[i], false, device_index_);
				stereo_display_component_->AdjustConvergence(stereo_display_component_->GetConfig().user_convergence[i], false, device_index_);
			}

			if (GetAsyncKeyState(stereo_display_component_->GetConfig().user_store_key[i]) & 0x8000)
			{
				stereo_display_component_->SaveUserSetting(i);
			}
		}

		// Update our pose every 30 milliseconds.
		std::this_thread::sleep_for( std::chrono::milliseconds( 30 ) );
	}
}

//-----------------------------------------------------------------------------
// Purpose: Stub for Standby mode
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::EnterStandby()
{
	DriverLog( "HMD has been put into standby." );
}

//-----------------------------------------------------------------------------
// Purpose: Shutdown process
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::Deactivate()
{
	if ( is_active_.exchange( false ) )
	{
		pose_update_thread_.join();
	}

	vr::VRSettings()->SetFloat(stereo_display_settings_section, "depth", stereo_display_component_->GetDepth());
	vr::VRSettings()->SetFloat(stereo_display_settings_section, "convergence", stereo_display_component_->GetConvergence());

	// unassign our controller index (we don't want to be calling vrserver anymore after Deactivate() has been called
	device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}


//-----------------------------------------------------------------------------
// DISPLAY DRIVER METHOD DEFINITIONS
//-----------------------------------------------------------------------------

StereoDisplayComponent::StereoDisplayComponent( const StereoDisplayDriverConfiguration &config )
	: config_( config ), depth_(config.depth), convergence_(config.convergence)
{
}

//-----------------------------------------------------------------------------
// Purpose: To inform vrcompositor if this display is considered an on-desktop display.
//-----------------------------------------------------------------------------
bool StereoDisplayComponent::IsDisplayOnDesktop()
{
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: To as vrcompositor to search for this display.
//-----------------------------------------------------------------------------
bool StereoDisplayComponent::IsDisplayRealDisplay()
{
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: To inform the rest of the vr system what the recommended target size should be
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight )
{
	*pnWidth = config_.render_width;
	*pnHeight = config_.render_height;
}

//-----------------------------------------------------------------------------
// Purpose: Render in SbS or TaB Stereo3D
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetEyeOutputViewport( vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
	if (config_.reverse_enable)
	{
		eEye = static_cast<vr::EVREye>(!static_cast<bool> (eEye));
	}
	// Use Top and Bottom Rendering
	if (config_.tab_enable)
	{
		*pnX = 0;
		// Each eye will have full width
		*pnWidth = config_.window_width;
		// Each eye will have half height
		*pnHeight = config_.window_height / 2;
		if (eEye == vr::Eye_Left)
		{
			// Left eye viewport on the top half of the window
			*pnY = 0;
		}
		else
		{
			// Right eye viewport on the bottom half of the window
			*pnY = config_.window_height / 2;
		}
	}

	// Use Side by Side Rendering
	else
	{
		*pnY = 0;
		// Each eye will have half width
		*pnWidth = config_.window_width / 2;
		// Each eye will have full height
		*pnHeight = config_.window_height;
		if (eEye == vr::Eye_Left)
		{
			// Left eye viewport on the left half of the window
			*pnX = 0;
		}
		else
		{
			// Right eye viewport on the right half of the window
			*pnX = config_.window_width / 2;
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Utilize the desired FoV, Aspect Ratio, and Convergence settings
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetProjectionRaw( vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom )
{
	// Convert horizontal FOV from degrees to radians
	float horFovRadians = tan((config_.fov * (M_PI / 180.0f)) / 2);

	// Calculate the vertical FOV in radians
	float verFovRadians = tan(atan(horFovRadians / config_.aspect_ratio));

	// Get convergence value
	float convergence = GetConvergence();

	// Calculate the raw projection values
	*pfTop = -verFovRadians;
	*pfBottom = verFovRadians;

	// Adjust the frustum based on the eye
	if (eEye == vr::Eye_Left) {
		*pfLeft = -horFovRadians + convergence;
		*pfRight = horFovRadians + convergence;
	}
	else {
		*pfLeft = -horFovRadians - convergence;
		*pfRight = horFovRadians - convergence;
	}
}

//-----------------------------------------------------------------------------
// Purpose: Don't distort any coordinates for Stereo3D
//-----------------------------------------------------------------------------
vr::DistortionCoordinates_t StereoDisplayComponent::ComputeDistortion( vr::EVREye eEye, float fU, float fV )
{
	vr::DistortionCoordinates_t coordinates{};
	coordinates.rfBlue[ 0 ] = fU;
	coordinates.rfBlue[ 1 ] = fV;
	coordinates.rfGreen[ 0 ] = fU;
	coordinates.rfGreen[ 1 ] = fV;
	coordinates.rfRed[ 0 ] = fU;
	coordinates.rfRed[ 1 ] = fV;
	return coordinates;
}
bool StereoDisplayComponent::ComputeInverseDistortion(vr::HmdVector2_t* pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV)
{
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: To inform vrcompositor what the window bounds for this virtual HMD are.
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
	*pnX = config_.window_x;
	*pnY = config_.window_y;
	*pnWidth = config_.window_width;
	*pnHeight = config_.window_height;
}

//-----------------------------------------------------------------------------
// Purpose: To provide access to settings
//-----------------------------------------------------------------------------
StereoDisplayDriverConfiguration StereoDisplayComponent::GetConfig()
{
	return config_;
}


//-----------------------------------------------------------------------------
// Purpose: To update the Depth value
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustDepth(float new_depth, bool is_delta, uint32_t device_index)
{
	float cur_depth = GetDepth();
	if (is_delta)
		new_depth += cur_depth;
	while (!depth_.compare_exchange_weak(cur_depth, new_depth, std::memory_order_relaxed));
	vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(device_index);
	vr::VRProperties()->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, new_depth);
}


//-----------------------------------------------------------------------------
// Purpose: To update the Convergence value
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustConvergence(float new_conv, bool is_delta, uint32_t device_index)
{
	float cur_conv = GetConvergence();
	if (is_delta)
		new_conv += cur_conv;
	while (!convergence_.compare_exchange_weak(cur_conv, new_conv, std::memory_order_relaxed));
	// Regenerate the Projection
	vr::HmdRect2_t eyeLeft, eyeRight;
	GetProjectionRaw(vr::Eye_Left, &eyeLeft.vTopLeft.v[0], &eyeLeft.vBottomRight.v[0], &eyeLeft.vTopLeft.v[1], &eyeLeft.vBottomRight.v[1]);
	GetProjectionRaw(vr::Eye_Right, &eyeRight.vTopLeft.v[0], &eyeRight.vBottomRight.v[0], &eyeRight.vTopLeft.v[1], &eyeRight.vBottomRight.v[1]);
	vr::VREvent_Data_t temp;
	vr::VRServerDriverHost()->SetDisplayProjectionRaw(device_index, eyeLeft, eyeRight);
	vr::VRServerDriverHost()->VendorSpecificEvent(device_index, vr::VREvent_LensDistortionChanged, temp, 0.0f);
}


//-----------------------------------------------------------------------------
// Purpose: Get Depth value
//-----------------------------------------------------------------------------
float StereoDisplayComponent::GetDepth()
{
	return depth_.load(std::memory_order_relaxed);
}


//-----------------------------------------------------------------------------
// Purpose: Get Convergence value
//-----------------------------------------------------------------------------
float StereoDisplayComponent::GetConvergence()
{
	return convergence_.load(std::memory_order_relaxed);
}


//-----------------------------------------------------------------------------
// Purpose: Store current Depth and Convergence into user setting i
//-----------------------------------------------------------------------------
void StereoDisplayComponent::SaveUserSetting(int i)
{
	config_.user_depth[i] = GetDepth();
	config_.user_convergence[i] = GetConvergence();
}
