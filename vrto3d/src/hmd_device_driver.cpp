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
#include "hmd_device_driver.h"
#include "key_mappings.h"

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

	auto* vrs = vr::VRSettings();

	char model_number[ 1024 ];
	vrs->GetString( stereo_main_settings_section, "model_number", model_number, sizeof( model_number ) );
	stereo_model_number_ = model_number;
	char serial_number[ 1024 ];
	vrs->GetString( stereo_main_settings_section, "serial_number", serial_number, sizeof( serial_number ) );
	stereo_serial_number_ = serial_number;

	DriverLog( "VRto3D Model Number: %s", stereo_model_number_.c_str() );
	DriverLog( "VRto3D Serial Number: %s", stereo_serial_number_.c_str() );

	// Display settings
	StereoDisplayDriverConfiguration display_configuration{};
	display_configuration.window_x = 0;
	display_configuration.window_y = 0;

	display_configuration.window_width = vrs->GetInt32( stereo_display_settings_section, "window_width" );
	display_configuration.window_height = vrs->GetInt32( stereo_display_settings_section, "window_height" );

	display_configuration.hmd_height = vrs->GetFloat(stereo_display_settings_section, "hmd_height");

	display_configuration.aspect_ratio = vrs->GetFloat(stereo_display_settings_section, "aspect_ratio");
	display_configuration.fov = vrs->GetFloat(stereo_display_settings_section, "fov");
	display_configuration.depth = vrs->GetFloat(stereo_display_settings_section, "depth");
	display_configuration.convergence = vrs->GetFloat(stereo_display_settings_section, "convergence");

	display_configuration.tab_enable = vrs->GetBool(stereo_display_settings_section, "tab_enable");
	display_configuration.half_enable = vrs->GetBool(stereo_display_settings_section, "half_enable");
	display_configuration.reverse_enable = vrs->GetBool(stereo_display_settings_section, "reverse_enable");
	display_configuration.hdr_enable = vrs->GetBool(stereo_display_settings_section, "hdr_enable");
	display_configuration.depth_gauge = vrs->GetBool(stereo_display_settings_section, "depth_gauge");

	display_configuration.display_latency = vrs->GetFloat(stereo_display_settings_section, "display_latency");
	display_configuration.display_frequency = vrs->GetFloat(stereo_display_settings_section, "display_frequency");
	display_configuration.sleep_count_max = (int)(floor(1600.0 / (1000.0 / display_configuration.display_frequency)));

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

	// Controller settings
	display_configuration.pitch_enable = vrs->GetBool(stereo_display_settings_section, "pitch_enable");
	display_configuration.yaw_enable = vrs->GetBool(stereo_display_settings_section, "yaw_enable");
	char ctrl_toggle_key[1024];
	vrs->GetString(stereo_display_settings_section, "ctrl_toggle_key", ctrl_toggle_key, sizeof(ctrl_toggle_key));
	if (VirtualKeyMappings.find(ctrl_toggle_key) != VirtualKeyMappings.end()) {
		display_configuration.ctrl_toggle_key = VirtualKeyMappings[ctrl_toggle_key];
		display_configuration.ctrl_xinput = false;
	}
	else if (XInputMappings.find(ctrl_toggle_key) != XInputMappings.end()) {
		display_configuration.ctrl_toggle_key = XInputMappings[ctrl_toggle_key];
		display_configuration.ctrl_xinput = true;
	}
	display_configuration.pitch_radius = vrs->GetFloat(stereo_display_settings_section, "pitch_radius");
	display_configuration.ctrl_deadzone = vrs->GetFloat(stereo_display_settings_section, "ctrl_deadzone");
	display_configuration.ctrl_sensitivity = vrs->GetFloat(stereo_display_settings_section, "ctrl_sensitivity");

	// Read user binds
	display_configuration.num_user_settings = vrs->GetInt32(stereo_display_settings_section, "num_user_settings");
	display_configuration.user_load_key.resize(display_configuration.num_user_settings);
	display_configuration.user_store_key.resize(display_configuration.num_user_settings);
	display_configuration.user_key_type.resize(display_configuration.num_user_settings);
	display_configuration.user_depth.resize(display_configuration.num_user_settings);
	display_configuration.user_convergence.resize(display_configuration.num_user_settings);
	display_configuration.prev_depth.resize(display_configuration.num_user_settings);
	display_configuration.prev_convergence.resize(display_configuration.num_user_settings);
	display_configuration.was_held.resize(display_configuration.num_user_settings);
	display_configuration.load_xinput.resize(display_configuration.num_user_settings);
	display_configuration.store_xinput.resize(display_configuration.num_user_settings);
	display_configuration.sleep_count.resize(display_configuration.num_user_settings);
	for (int i = 0; i < display_configuration.num_user_settings; i++)
	{
		char user_key[1024];
		std::string temp = "user_load_key" + std::to_string(i + 1);
		vrs->GetString(stereo_display_settings_section, temp.c_str(), user_key, sizeof(user_key));
		if (VirtualKeyMappings.find(user_key) != VirtualKeyMappings.end()) {
			display_configuration.user_load_key[i] = VirtualKeyMappings[user_key];
			display_configuration.load_xinput[i] = false;
		}
		else if (XInputMappings.find(user_key) != XInputMappings.end()) {
			display_configuration.user_load_key[i] = XInputMappings[user_key];
			display_configuration.load_xinput[i] = true;
		}
		temp = "user_store_key" + std::to_string(i + 1);
		vrs->GetString(stereo_display_settings_section, temp.c_str(), user_key, sizeof(user_key));
		if (VirtualKeyMappings.find(user_key) != VirtualKeyMappings.end()) {
			display_configuration.user_store_key[i] = VirtualKeyMappings[user_key];
			display_configuration.store_xinput[i] = false;
		}
		else if (XInputMappings.find(user_key) != XInputMappings.end()) {
			display_configuration.user_store_key[i] = XInputMappings[user_key];
			display_configuration.store_xinput[i] = true;
		}
		temp = "user_key_type" + std::to_string(i + 1);
		vrs->GetString(stereo_display_settings_section, temp.c_str(), user_key, sizeof(user_key));
		if (KeyBindTypes.find(user_key) != KeyBindTypes.end()) {
			display_configuration.user_key_type[i] = KeyBindTypes[user_key];
		}
		temp = "user_depth" + std::to_string(i + 1);
		display_configuration.user_depth[i] = vr::VRSettings()->GetFloat(stereo_display_settings_section, temp.c_str());
		temp = "user_convergence" + std::to_string(i + 1);
		display_configuration.user_convergence[i] = vr::VRSettings()->GetFloat(stereo_display_settings_section, temp.c_str());
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
	auto* vrp = vr::VRProperties();
	auto* vrs = vr::VRSettings();
	vr::PropertyContainerHandle_t container = vrp->TrackedDeviceToPropertyContainer( device_index_ );
	vrp->SetStringProperty( container, vr::Prop_ModelNumber_String, stereo_model_number_.c_str() );
	vrp->SetStringProperty( container, vr::Prop_ManufacturerName_String, "VRto3D");
	vrp->SetStringProperty( container, vr::Prop_TrackingFirmwareVersion_String, "1.0");
	vrp->SetStringProperty( container, vr::Prop_HardwareRevision_String, "1.0");

	// Display settings
	vrp->SetFloatProperty( container, vr::Prop_UserIpdMeters_Float, stereo_display_component_->GetConfig().depth);
	vrp->SetFloatProperty( container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f);
	vrp->SetFloatProperty( container, vr::Prop_DisplayFrequency_Float, stereo_display_component_->GetConfig().display_frequency);
	vrp->SetFloatProperty( container, vr::Prop_SecondsFromVsyncToPhotons_Float, stereo_display_component_->GetConfig().display_latency);
	vrp->SetBoolProperty( container, vr::Prop_IsOnDesktop_Bool, false);
	vrp->SetBoolProperty( container, vr::Prop_DisplayDebugMode_Bool, true);
	vrp->SetBoolProperty( container, vr::Prop_HasDriverDirectModeComponent_Bool, false);
	vrp->SetBoolProperty( container, vr::Prop_Hmd_SupportsHDR10_Bool, stereo_display_component_->GetConfig().hdr_enable);
	if (stereo_display_component_->GetConfig().depth_gauge)
	{
		vrp->SetFloatProperty(container, vr::Prop_DashboardScale_Float, 1.0f);
	}
	else
	{
		vrp->SetFloatProperty(container, vr::Prop_DashboardScale_Float, 0.0f);
	}

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
                    "translation" : [ 0.0, 0.5, 0.0 ],
                    "yaw" : 0.0
                 },
                 "standing" : {
                    "translation" : [ 0.0, 1.0, 0.0 ],
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
	vrp->SetStringProperty(container, vr::Prop_DriverProvidedChaperoneJson_String, chaperoneJson.c_str());
	vrp->SetUint64Property(container, vr::Prop_CurrentUniverseId_Uint64, 64);
	vrs->SetInt32(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_Style_Int32, vr::COLLISION_BOUNDS_STYLE_NONE);
	vrs->SetBool(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_GroundPerimeterOn_Bool, false);

	// Miscellaneous settings
	vrp->SetBoolProperty( container, vr::Prop_WillDriftInYaw_Bool, false);
	vrp->SetBoolProperty( container, vr::Prop_DeviceIsWireless_Bool, false);
	vrp->SetBoolProperty( container, vr::Prop_DeviceIsCharging_Bool, false);
	vrp->SetBoolProperty( container, vr::Prop_ContainsProximitySensor_Bool, false);
	vrp->SetBoolProperty( container, vr::Prop_DeviceCanPowerOff_Bool, false);

	// set proximity senser to always on, always head present
	vr::VRInputComponentHandle_t  prox;
	vr::VRDriverInput()->CreateBooleanComponent(container, "/proximity", &prox);
	vr::VRDriverInput()->UpdateBooleanComponent(prox, true, 0.0);
	
	// Miscellaneous settings
	vrs->SetBool(vr::k_pch_DirectMode_Section, vr::k_pch_DirectMode_Enable_Bool, false);
	vrs->SetFloat(vr::k_pch_Power_Section, vr::k_pch_Power_TurnOffScreensTimeout_Float, 86400.0f);
	vrs->SetBool(vr::k_pch_Power_Section, vr::k_pch_Power_PauseCompositorOnStandby_Bool, false);
	vrs->SetBool(vr::k_pch_Dashboard_Section, vr::k_pch_Dashboard_EnableDashboard_Bool, false);
	vrs->SetBool(vr::k_pch_Dashboard_Section, vr::k_pch_Dashboard_ArcadeMode_Bool, true);
	vrs->SetBool(vr::k_pch_Dashboard_Section, "allowAppQuitting", false);
	vrs->SetBool(vr::k_pch_Dashboard_Section, "autoShowGameTheater", false);
	vrs->SetBool(vr::k_pch_Dashboard_Section, "showDesktop", false);
	vrs->SetBool(vr::k_pch_Dashboard_Section, "showPowerOptions", false);
	vrs->SetBool(vr::k_pch_Dashboard_Section, "inputCaptureEnabled", false);
	vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_EnableHomeApp, false);
	vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_MirrorViewVisibility_Bool, false);
	vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_EnableSafeMode, false);
	
	pose_update_thread_ = std::thread( &MockControllerDeviceDriver::PoseUpdateThread, this );

	return vr::VRInitError_None;
}

//-----------------------------------------------------------------------------
// Purpose: Return StereoDisplayComponent as vr::IVRDisplayComponent
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
// Purpose: Static Pose with pitch adjustment
//-----------------------------------------------------------------------------
vr::DriverPose_t MockControllerDeviceDriver::GetPose()
{
	static const float updateInterval = 1.0f / stereo_display_component_->GetConfig().display_frequency; // Update interval in seconds
	static float currentPitch = 0.0f; // Keep track of the current pitch
	static float currentYaw = 0.0f; // Keep track of the current yaw
	static float lastPitch = 0.0f;
	static float lastYaw = 0.0f;
	static float lastPos[3] = { 0.0f, 0.0f, 0.0f }; // Last position vector
	static float lastVel[3] = { 0.0f, 0.0f, 0.0f }; // Last velocity vector
	static float lastAngVel[3] = { 0.0f, 0.0f, 0.0f }; // Last angular velocity vector

	vr::DriverPose_t pose = { 0 };

	pose.qWorldFromDriverRotation = HmdQuaternion_Identity;
	pose.qDriverFromHeadRotation = HmdQuaternion_Identity;
	pose.qRotation = HmdQuaternion_Identity;

	// Adjust pitch based on controller input
	if (stereo_display_component_->GetConfig().pitch_enable)
	{
		stereo_display_component_->AdjustPitch(currentPitch);
	}

	// Adjust yaw based on controller input
	if (stereo_display_component_->GetConfig().yaw_enable)
	{
		stereo_display_component_->AdjustYaw(currentYaw);
	}

	float pitchRadians = DEG_TO_RAD(currentPitch);
	float yawRadians = DEG_TO_RAD(currentYaw);
	float radius = stereo_display_component_->GetConfig().pitch_radius; // Configurable radius for pitch

	// Recompose the rotation quaternion from pitch and yaw
	vr::HmdQuaternion_t pitchQuaternion = HmdQuaternion_FromEulerAngles(0, pitchRadians, 0);
	vr::HmdQuaternion_t yawQuaternion = HmdQuaternion_FromEulerAngles(0, 0, yawRadians);
	pose.qRotation = HmdQuaternion_Normalize(yawQuaternion * pitchQuaternion);
	
	// Calculate the new position relative to the current pitch & yaw
	pose.vecPosition[0] = radius * sin(pitchRadians) * sin(yawRadians);
	pose.vecPosition[1] = stereo_display_component_->GetConfig().hmd_height - radius * (1 - cos(pitchRadians));
	pose.vecPosition[2] = radius * sin(pitchRadians) * cos(yawRadians);

	// Calculate velocity components based on change in position
	pose.vecVelocity[0] = (pose.vecPosition[0] - lastPos[0]) / updateInterval;
	pose.vecVelocity[1] = (pose.vecPosition[1] - lastPos[1]) / updateInterval;
	pose.vecVelocity[2] = (pose.vecPosition[2] - lastPos[2]) / updateInterval;

	// Calculate velocity using known update interval
	pose.vecAngularVelocity[0] = (pitchRadians - lastPitch) / updateInterval; // Pitch angular velocity
	pose.vecAngularVelocity[1] = (yawRadians - lastYaw) / updateInterval; // Yaw angular velocity
	pose.vecAngularVelocity[2] = 0.0f;

	// Calculate acceleration based on change in velocity
	pose.vecAcceleration[0] = (pose.vecVelocity[0] - lastVel[0]) / updateInterval;
	pose.vecAcceleration[1] = (pose.vecVelocity[1] - lastVel[1]) / updateInterval;
	pose.vecAcceleration[2] = (pose.vecVelocity[2] - lastVel[2]) / updateInterval;
	pose.vecAngularAcceleration[0] = (pose.vecAngularVelocity[0] - lastAngVel[0]) / updateInterval;
	pose.vecAngularAcceleration[1] = (pose.vecAngularVelocity[1] - lastAngVel[1]) / updateInterval;
	pose.vecAngularAcceleration[2] = 0.0f;

	// Update for next iteration
	lastPitch = pitchRadians;
	lastYaw = yawRadians;
	lastPos[0] = pose.vecPosition[0];
	lastPos[1] = pose.vecPosition[1];
	lastPos[2] = pose.vecPosition[2];
	lastVel[0] = pose.vecVelocity[0];
	lastVel[1] = pose.vecVelocity[1];
	lastVel[2] = pose.vecVelocity[2];
	lastAngVel[0] = pose.vecAngularVelocity[0];
	lastAngVel[1] = pose.vecAngularVelocity[1];

	pose.poseIsValid = true;
	pose.deviceIsConnected = true;
	pose.result = vr::TrackingResult_Running_OK;
	pose.shouldApplyHeadModel = false;
	pose.willDriftInYaw = false;

	return pose;
}


//-----------------------------------------------------------------------------
// Purpose: Update HMD position, Depth, Convergence, and user binds
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::PoseUpdateThread()
{
	static int sleep_time = (int)(floor(1000.0 / stereo_display_component_->GetConfig().display_frequency));
	static int sleep_counter = 0;
	while ( is_active_ )
	{
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
		// Ctrl+F7 Store Depth & Convergence values
		if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F7) & 0x8000)) {
			SaveDepthConv();
		}
		// Ctrl+F9 Toggle HMD height
		if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F9) & 0x8000) && sleep_counter == 0) {
			sleep_counter = stereo_display_component_->GetConfig().sleep_count_max;
			stereo_display_component_->SetHeight();
		}
		else if (sleep_counter > 0) {
			sleep_counter--;
		}

		// Check User binds
		stereo_display_component_->CheckUserSettings(device_index_);

		// Update our pose ~ every frame
		std::this_thread::sleep_for( std::chrono::milliseconds(sleep_time));
	}
}

//-----------------------------------------------------------------------------
// Purpose: Save Depth and Convergence to Steam\config\steamvr.vrsettings
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::SaveDepthConv()
{
	vr::VRSettings()->SetFloat(stereo_display_settings_section, "depth", stereo_display_component_->GetDepth());
	vr::VRSettings()->SetFloat(stereo_display_settings_section, "convergence", stereo_display_component_->GetConvergence());

	for (int i = 0; i < stereo_display_component_->GetConfig().num_user_settings; i++)
	{
		std::string temp = "user_depth" + std::to_string(i + 1);
		vr::VRSettings()->SetFloat(stereo_display_settings_section, temp.c_str(), stereo_display_component_->GetConfig().user_depth[i]);
		temp = "user_convergence" + std::to_string(i + 1);
		vr::VRSettings()->SetFloat(stereo_display_settings_section, temp.c_str(), stereo_display_component_->GetConfig().user_convergence[i]);
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
// Purpose: Check User Settings and act on them
//-----------------------------------------------------------------------------
void StereoDisplayComponent::CheckUserSettings(uint32_t device_index)
{
	static bool pitch_set = config_.pitch_enable;
	static bool yaw_set = config_.yaw_enable;
	static int sleep_ctrl = 0;

    XINPUT_STATE state;
    ZeroMemory(&state, sizeof(XINPUT_STATE));
    // Get the state of the first controller (index 0)
    DWORD dwResult = XInputGetState(0, &state);

	if (((config_.ctrl_xinput && dwResult == ERROR_SUCCESS &&
		((config_.ctrl_toggle_key == XINPUT_GAMEPAD_LEFT_TRIGGER && state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
			|| (config_.ctrl_toggle_key == XINPUT_GAMEPAD_RIGHT_TRIGGER && state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
			|| (state.Gamepad.wButtons & config_.ctrl_toggle_key)))
		|| (!config_.ctrl_xinput && (GetAsyncKeyState(config_.ctrl_toggle_key) & 0x8000)))
		&& sleep_ctrl == 0)
	{
		sleep_ctrl = config_.sleep_count_max;
		if (pitch_set) {
			config_.pitch_enable = !config_.pitch_enable;
		}
		if (yaw_set) {
			config_.yaw_enable = !config_.yaw_enable;
		}
	}
	else if (sleep_ctrl > 0) {
		sleep_ctrl--;
	}

    for (int i = 0; i < config_.num_user_settings; i++)
    {
        // Decrement the sleep count if it's greater than zero
        if (config_.sleep_count[i] > 0)
            config_.sleep_count[i]--;

        bool keyPressed = (config_.load_xinput[i] && dwResult == ERROR_SUCCESS &&
            ((config_.user_load_key[i] == XINPUT_GAMEPAD_LEFT_TRIGGER && state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
            || (config_.user_load_key[i] == XINPUT_GAMEPAD_RIGHT_TRIGGER && state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
            || (state.Gamepad.wButtons & config_.user_load_key[i])))
            || (!config_.load_xinput[i] && (GetAsyncKeyState(config_.user_load_key[i]) & 0x8000));

        // Load stored convergence
        if (keyPressed)
        {
            if (config_.user_key_type[i] == HOLD && !config_.was_held[i])
            {
                config_.prev_depth[i] = GetDepth();
                config_.prev_convergence[i] = GetConvergence();
                config_.was_held[i] = true;
                AdjustDepth(config_.user_depth[i], false, device_index);
                AdjustConvergence(config_.user_convergence[i], false, device_index);
            }
			else if (config_.user_key_type[i] == TOGGLE && config_.sleep_count[i] < 1)
			{
				config_.sleep_count[i] = config_.sleep_count_max;
				if (GetDepth() == config_.user_depth[i] && GetConvergence() == config_.user_convergence[i])
				{
					// If the current state matches the user settings, revert to the previous state
					AdjustDepth(config_.prev_depth[i], false, device_index);
					AdjustConvergence(config_.prev_convergence[i], false, device_index);
				}
				else
				{
					// Save the current state and apply the user settings
					config_.prev_depth[i] = GetDepth();
					config_.prev_convergence[i] = GetConvergence();
					AdjustDepth(config_.user_depth[i], false, device_index);
					AdjustConvergence(config_.user_convergence[i], false, device_index);
				}
			}
            else if (config_.user_key_type[i] == SWITCH)
            {
                AdjustDepth(config_.user_depth[i], false, device_index);
                AdjustConvergence(config_.user_convergence[i], false, device_index);
            }
        }
        // Release depth & convergence back to normal for HOLD key type
        else if (config_.user_key_type[i] == HOLD && config_.was_held[i])
        {
            config_.was_held[i] = false;
            AdjustDepth(config_.prev_depth[i], false, device_index);
            AdjustConvergence(config_.prev_convergence[i], false, device_index);
        }

        // Store current depth & convergence to user setting
        bool storeKeyPressed = (config_.store_xinput[i] && dwResult == ERROR_SUCCESS &&
            ((config_.user_store_key[i] == XINPUT_GAMEPAD_LEFT_TRIGGER && state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
            || (config_.user_store_key[i] == XINPUT_GAMEPAD_RIGHT_TRIGGER && state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
            || (state.Gamepad.wButtons & config_.user_store_key[i])))
            || (!config_.store_xinput[i] && (GetAsyncKeyState(config_.user_store_key[i]) & 0x8000));

        if (storeKeyPressed)
        {
            config_.user_depth[i] = GetDepth();
            config_.user_convergence[i] = GetConvergence();
        }
    }
}


//-----------------------------------------------------------------------------
// Purpose: Adjust HMD Pitch using XInput Right Stick YAxis
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustPitch(float& currentPitch)
{
	XINPUT_STATE state;
	ZeroMemory(&state, sizeof(XINPUT_STATE));
	DWORD dwResult = XInputGetState(0, &state);

	if (dwResult == ERROR_SUCCESS)
	{
		SHORT sThumbRY = state.Gamepad.sThumbRY;
		float normalizedY = sThumbRY / 32767.0f;

		// Apply deadzone
		if (std::abs(normalizedY) < config_.ctrl_deadzone)
		{
			normalizedY = 0.0f;
		}
		else
		{
			if (normalizedY > 0)
				normalizedY = (normalizedY - config_.ctrl_deadzone) / (1.0f - config_.ctrl_deadzone);
			else
				normalizedY = (normalizedY + config_.ctrl_deadzone) / (1.0f - config_.ctrl_deadzone);
		}

		// Scale Pitch
		currentPitch += (normalizedY * config_.ctrl_sensitivity);
		if (currentPitch > 90.0f) currentPitch = 90.0f;
		if (currentPitch < -90.0f) currentPitch = -90.0f;
	}
}


//-----------------------------------------------------------------------------
// Purpose: Adjust HMD Yaw using XInput Right Stick XAxis
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustYaw(float& currentYaw)
{
	XINPUT_STATE state;
	ZeroMemory(&state, sizeof(XINPUT_STATE));
	DWORD dwResult = XInputGetState(0, &state);

	if (dwResult == ERROR_SUCCESS)
	{
		SHORT sThumbRX = state.Gamepad.sThumbRX;
		float normalizedX = sThumbRX / 32767.0f;

		// Apply deadzone
		if (std::abs(normalizedX) < config_.ctrl_deadzone)
		{
			normalizedX = 0.0f;
		}
		else
		{
			if (normalizedX > 0)
				normalizedX = (normalizedX - config_.ctrl_deadzone) / (1.0f - config_.ctrl_deadzone);
			else
				normalizedX = (normalizedX + config_.ctrl_deadzone) / (1.0f - config_.ctrl_deadzone);
		}

		// Scale Yaw
		float yawAdjustment = -normalizedX * config_.ctrl_sensitivity;

		// Apply the incremental yaw adjustment to the current yaw
		currentYaw += yawAdjustment;
		if (currentYaw > 180.0f) currentYaw -= 360.0f;
		if (currentYaw < -180.0f) currentYaw += 360.0f;
	}
}


//-----------------------------------------------------------------------------
// Purpose: Toggle HMD Height for games that have incorrect HMD position
//-----------------------------------------------------------------------------
void StereoDisplayComponent::SetHeight()
{
	static float user_height = config_.hmd_height;
	if (config_.hmd_height == user_height)
		config_.hmd_height = 0.1;
	else
		config_.hmd_height = user_height;
}
