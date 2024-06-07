//============ Copyright (c) Valve Corporation, All rights reserved. ============
#include "hmd_device_driver.h"

#include "driverlog.h"
#include "vrmath.h"
#include <string.h>

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
	display_configuration.ipd = vr::VRSettings()->GetFloat(stereo_display_settings_section, "ipd");

	display_configuration.tab_enable = vr::VRSettings()->GetBool(stereo_display_settings_section, "tab_enable");
	display_configuration.half_enable = vr::VRSettings()->GetBool(stereo_display_settings_section, "half_enable");
	display_configuration.super_sample = vr::VRSettings()->GetBool(stereo_display_settings_section, "super_sample");
	display_configuration.hdr_enable = vr::VRSettings()->GetBool(stereo_display_settings_section, "hdr_enable");

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
// Purpose: This is called by vrserver after our
//  IServerTrackedDeviceProvider calls IVRServerDriverHost::TrackedDeviceAdded.
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
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_UserIpdMeters_Float, stereo_display_component_.get()->GetConfig().ipd);
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f);
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_DisplayFrequency_Float, stereo_display_component_.get()->GetConfig().display_frequency);
	vr::VRProperties()->SetFloatProperty( container, vr::Prop_SecondsFromVsyncToPhotons_Float, stereo_display_component_.get()->GetConfig().display_latency);
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_IsOnDesktop_Bool, false);
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_DisplayDebugMode_Bool, true);
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_Hmd_SupportsHDR10_Bool, stereo_display_component_.get()->GetConfig().hdr_enable);
	vr::VRProperties()->SetBoolProperty( container, vr::Prop_Hmd_AllowSupersampleFiltering_Bool, stereo_display_component_.get()->GetConfig().super_sample);

	//Prop_FieldOfViewLeftDegrees_Float
	//Prop_FieldOfViewRightDegrees_Float
	//Prop_FieldOfViewTopDegrees_Float
	//Prop_FieldOfViewBottomDegrees_Float

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
// Purpose: This is never called by vrserver in recent OpenVR versions,
// but is useful for giving data to vr::VRServerDriverHost::TrackedDevicePoseUpdated.
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
	while ( is_active_ )
	{
		// Inform the vrserver that our tracked device's pose has updated, giving it the pose returned by our GetPose().
		vr::VRServerDriverHost()->TrackedDevicePoseUpdated( device_index_, GetPose(), sizeof( vr::DriverPose_t ) );

		// Update our pose every five milliseconds.
		// In reality, you should update the pose whenever you have new data from your device.
		std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
	}
}

//-----------------------------------------------------------------------------
// Purpose: This is called by vrserver when the device should enter standby mode.
// The device should be put into whatever low power mode it has.
// We don't really have anything to do here, so let's just log something.
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::EnterStandby()
{
	DriverLog( "HMD has been put into standby." );
}

//-----------------------------------------------------------------------------
// Purpose: This is called by vrserver when the device should deactivate.
// This is typically at the end of a session
// The device should free any resources it has allocated here.
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::Deactivate()
{
	// Let's join our pose thread that's running
	// by first checking then setting is_active_ to false to break out
	// of the while loop, if it's running, then call .join() on the thread
	if ( is_active_.exchange( false ) )
	{
		pose_update_thread_.join();
	}

	// unassign our controller index (we don't want to be calling vrserver anymore after Deactivate() has been called
	device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}


//-----------------------------------------------------------------------------
// Purpose: This is called by our IServerTrackedDeviceProvider when it pops an event off the event queue.
// It's not part of the ITrackedDeviceServerDriver interface, we created it ourselves.
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::MyProcessEvent( const vr::VREvent_t &vrevent )
{
}


//-----------------------------------------------------------------------------
// Purpose: Our IServerTrackedDeviceProvider needs our serial number to add us to vrserver.
// It's not part of the ITrackedDeviceServerDriver interface, we created it ourselves.
//-----------------------------------------------------------------------------
const std::string &MockControllerDeviceDriver::MyGetSerialNumber()
{
	return stereo_serial_number_;
}

//-----------------------------------------------------------------------------
// DISPLAY DRIVER METHOD DEFINITIONS
//-----------------------------------------------------------------------------

StereoDisplayComponent::StereoDisplayComponent( const StereoDisplayDriverConfiguration &config )
	: config_( config )
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
// Purpose: To inform the compositor what the projection parameters are for this HMD.
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetProjectionRaw( vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom )
{
	// Convert horizontal FOV from degrees to radians
	float horFovRadians = config_.fov * (M_PI / 180.0f);

	// Calculate the vertical FOV in radians
	float verFovRadians = 2 * atan(tan(horFovRadians / 2) / config_.aspect_ratio);

	// Calculate the raw projection values
	*pfLeft = -tan(horFovRadians / 2);
	*pfRight = tan(horFovRadians / 2);
	*pfTop = -tan(verFovRadians / 2);
	*pfBottom = tan(verFovRadians / 2);
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
