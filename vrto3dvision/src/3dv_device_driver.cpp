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

#include "3dv_device_driver.h"
#include "../../vrto3d/src/key_mappings.h"

#include "driverlog.h"

#include <algorithm>
#include <chrono>

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
static const char* stereo_main_settings_section = "driver_vrto3dvision";
static const char* stereo_display_settings_section = "vrto3dvision_display";

OVR_3DV_Driver::OVR_3DV_Driver()
{
    DriverLog( "OVR_3DV_Driver::OVR_3DV_Driver()\n" );

    InitializeCriticalSectionAndSpinCount( &m_texSetCS, 0x442 );


    // Keep track of whether Activate() has been called
    is_active_ = false;

    char model_number[1024];
    vr::VRSettings()->GetString(stereo_main_settings_section, "model_number", model_number, sizeof(model_number));
    stereo_model_number_ = model_number;
    char serial_number[1024];
    vr::VRSettings()->GetString(stereo_main_settings_section, "serial_number", serial_number, sizeof(serial_number));
    stereo_serial_number_ = serial_number;

    DriverLog("VRto3DVision Model Number: %s", stereo_model_number_.c_str());
    DriverLog("VRto3DVision Serial Number: %s", stereo_serial_number_.c_str());

    // Display settings
    config_.window_x = vr::VRSettings()->GetInt32(stereo_display_settings_section, "window_x");
    config_.window_y = vr::VRSettings()->GetInt32(stereo_display_settings_section, "window_y");

    config_.window_width = vr::VRSettings()->GetInt32(stereo_display_settings_section, "window_width");
    config_.window_height = vr::VRSettings()->GetInt32(stereo_display_settings_section, "window_height");
    config_.render_width = config_.window_width;
    config_.render_height = config_.window_height;

    config_.aspect_ratio = vr::VRSettings()->GetFloat(stereo_display_settings_section, "aspect_ratio");
    config_.fov = vr::VRSettings()->GetFloat(stereo_display_settings_section, "fov");
    config_.depth = vr::VRSettings()->GetFloat(stereo_display_settings_section, "depth");
    config_.convergence = vr::VRSettings()->GetFloat(stereo_display_settings_section, "convergence");
    depth_ = config_.depth;
    convergence_ = config_.convergence;

    config_.ss_enable = vr::VRSettings()->GetBool(stereo_display_settings_section, "ss_enable");
    config_.depth_gauge = vr::VRSettings()->GetBool(stereo_display_settings_section, "depth_gauge");

    config_.ss_scale = vr::VRSettings()->GetFloat(stereo_display_settings_section, "ss_scale");
    config_.display_latency = vr::VRSettings()->GetFloat(stereo_display_settings_section, "display_latency");
    config_.display_frequency = vr::VRSettings()->GetFloat(stereo_display_settings_section, "display_frequency");

    // Controller settings
    config_.ctrl_enable = vr::VRSettings()->GetBool(stereo_display_settings_section, "ctrl_enable");
    config_.ctrl_deadzone = vr::VRSettings()->GetFloat(stereo_display_settings_section, "ctrl_deadzone");
    config_.ctrl_sensitivity = vr::VRSettings()->GetFloat(stereo_display_settings_section, "ctrl_sensitivity");

    // Read user binds
    config_.num_user_settings = vr::VRSettings()->GetInt32(stereo_display_settings_section, "num_user_settings");
    config_.user_load_key.resize(config_.num_user_settings);
    config_.user_store_key.resize(config_.num_user_settings);
    config_.user_key_type.resize(config_.num_user_settings);
    config_.user_depth.resize(config_.num_user_settings);
    config_.user_convergence.resize(config_.num_user_settings);
    config_.prev_depth.resize(config_.num_user_settings);
    config_.prev_convergence.resize(config_.num_user_settings);
    config_.was_held.resize(config_.num_user_settings);
    config_.load_xinput.resize(config_.num_user_settings);
    config_.store_xinput.resize(config_.num_user_settings);
    config_.sleep_count.resize(config_.num_user_settings);
    for (int i = 0; i < config_.num_user_settings; i++)
    {
        char user_key[1024];
        std::string temp = "user_load_key" + std::to_string(i + 1);
        vr::VRSettings()->GetString(stereo_display_settings_section, temp.c_str(), user_key, sizeof(user_key));
        if (VirtualKeyMappings.find(user_key) != VirtualKeyMappings.end()) {
            config_.user_load_key[i] = VirtualKeyMappings[user_key];
            config_.load_xinput[i] = false;
        }
        else if (XInputMappings.find(user_key) != XInputMappings.end()) {
            config_.user_load_key[i] = XInputMappings[user_key];
            config_.load_xinput[i] = true;
        }
        temp = "user_store_key" + std::to_string(i + 1);
        vr::VRSettings()->GetString(stereo_display_settings_section, temp.c_str(), user_key, sizeof(user_key));
        if (VirtualKeyMappings.find(user_key) != VirtualKeyMappings.end()) {
            config_.user_store_key[i] = VirtualKeyMappings[user_key];
            config_.store_xinput[i] = false;
        }
        else if (XInputMappings.find(user_key) != XInputMappings.end()) {
            config_.user_store_key[i] = XInputMappings[user_key];
            config_.store_xinput[i] = true;
        }
        temp = "user_key_type" + std::to_string(i + 1);
        vr::VRSettings()->GetString(stereo_display_settings_section, temp.c_str(), user_key, sizeof(user_key));
        if (KeyBindTypes.find(user_key) != KeyBindTypes.end()) {
            config_.user_key_type[i] = KeyBindTypes[user_key];
        }
        temp = "user_depth" + std::to_string(i + 1);
        config_.user_depth[i] = vr::VRSettings()->GetFloat(stereo_display_settings_section, temp.c_str());
        temp = "user_convergence" + std::to_string(i + 1);
        config_.user_convergence[i] = vr::VRSettings()->GetFloat(stereo_display_settings_section, temp.c_str());
    }
}

OVR_3DV_Driver::~OVR_3DV_Driver()
{
  DriverLog( "OVR_3DV_Driver::~OVR_3DV_Driver()\n" );

  if ( m_runVSyncThread )
  {
    m_runVSyncThread = false;
    m_vSyncThread.join();
  }

  DeleteCriticalSection( &m_texSetCS );
}


//-----------------------------------------------------------------------------
// Purpose: Update HMD position, Depth, Convergence, and user binds
//-----------------------------------------------------------------------------
void OVR_3DV_Driver::PoseUpdateThread()
{
    static int sleep_time = (int)(floor(1000.0 / config_.display_frequency));
    while (is_active_)
    {
        // Inform the vrserver that our tracked device's pose has updated, giving it the pose returned by our GetPose().
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(device_index_, GetPose(), sizeof(vr::DriverPose_t));

        // Ctrl+F3 Decrease Depth
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F3) & 0x8000)) {
            AdjustDepth(-0.001f, true, device_index_);
        }
        // Ctrl+F4 Increase Depth
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F4) & 0x8000)) {
            AdjustDepth(0.001f, true, device_index_);
        }
        // Ctrl+F5 Decrease Convergence
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F5) & 0x8000)) {
            AdjustConvergence(-0.001f, true, device_index_);
        }
        // Ctrl+F6 Increase Convergence
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F6) & 0x8000)) {
            AdjustConvergence(0.001f, true, device_index_);
        }
        // Ctrl+F7 Store Depth & Convergence values
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F7) & 0x8000)) {
            SaveDepthConv();
        }

        // Check User binds
        CheckUserSettings(device_index_);

        // Update our pose ~ every frame
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
    }
}


//-----------------------------------------------------------------------------
// Purpose: To update the Depth value
//-----------------------------------------------------------------------------
void OVR_3DV_Driver::AdjustDepth(float new_depth, bool is_delta, uint32_t device_index)
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
void OVR_3DV_Driver::AdjustConvergence(float new_conv, bool is_delta, uint32_t device_index)
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
float OVR_3DV_Driver::GetDepth()
{
    return depth_.load(std::memory_order_relaxed);
}


//-----------------------------------------------------------------------------
// Purpose: Get Convergence value
//-----------------------------------------------------------------------------
float OVR_3DV_Driver::GetConvergence()
{
    return convergence_.load(std::memory_order_relaxed);
}


//-----------------------------------------------------------------------------
// Purpose: Check User Settings and act on them
//-----------------------------------------------------------------------------
void OVR_3DV_Driver::CheckUserSettings(uint32_t device_index)
{
    XINPUT_STATE state;
    ZeroMemory(&state, sizeof(XINPUT_STATE));
    // Get the state of the first controller (index 0)
    DWORD dwResult = XInputGetState(0, &state);

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
                config_.sleep_count[i] = 100;
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
// Purpose: Save Depth and Convergence to Steam\config\steamvr.vrsettings
//-----------------------------------------------------------------------------
void OVR_3DV_Driver::SaveDepthConv()
{
    vr::VRSettings()->SetFloat(stereo_display_settings_section, "depth", GetDepth());
    vr::VRSettings()->SetFloat(stereo_display_settings_section, "convergence", GetConvergence());

    for (int i = 0; i < config_.num_user_settings; i++)
    {
        std::string temp = "user_depth" + std::to_string(i + 1);
        vr::VRSettings()->SetFloat(stereo_display_settings_section, temp.c_str(), config_.user_depth[i]);
        temp = "user_convergence" + std::to_string(i + 1);
        vr::VRSettings()->SetFloat(stereo_display_settings_section, temp.c_str(), config_.user_convergence[i]);
    }
}


//-----------------------------------------------------------------------------
// Purpose: Adjust HMD Pitch using XInput Right Stick YAxis
//-----------------------------------------------------------------------------
void OVR_3DV_Driver::AdjustPitch(vr::HmdQuaternion_t& qRotation, float& currentPitch)
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

        // Adjust quaternion
        vr::HmdQuaternion_t pitchQuaternion = HmdQuaternion_FromEulerAngles(0, DEG_TO_RAD(currentPitch), 0);
        qRotation = HmdQuaternion_Normalize(qRotation * pitchQuaternion);
    }
}


//-----------------------------------------------------------------------------
// Purpose: Initialize all settings and notify SteamVR
//-----------------------------------------------------------------------------
vr::EVRInitError OVR_3DV_Driver::Activate( vr::TrackedDeviceIndex_t unObjectId )
{
    DriverLog( "OVR_3DV_Driver::Activate(%i)\n", unObjectId );

    if ( !m_renderHelper.Init() )
    {
        DriverLog( "OVR_3DV_Driver: ERROR: Initialization failed, D3D renderer failed.\n" );
        return vr::VRInitError_Driver_Failed;
    }

    device_index_ = unObjectId;
    is_active_ = true;
    auto* vrp = vr::VRProperties();
    auto* vrs = vr::VRSettings();
    vr::PropertyContainerHandle_t container = vrp->TrackedDeviceToPropertyContainer(device_index_);
    DriverLog( "HMD Activate %i, %i\n", device_index_, container);
    DriverLog( "HMD Serial Number: %s\n", stereo_serial_number_.c_str() );
    DriverLog( "HMD Model Number: %s\n", stereo_model_number_.c_str() );
    DriverLog( "HMD Render Target: %d %d\n", config_.render_width, config_.render_height );
    DriverLog( "HMD Seconds from Vsync to Photons: %f\n", config_.display_latency );
    DriverLog( "HMD Display Frequency: %f\n", config_.display_frequency );
    DriverLog( "HMD Depth: %f\n", config_.depth );
    DriverLog( "HMD Convergence: %f\n", config_.convergence);

    vrp->SetStringProperty(container, vr::Prop_ModelNumber_String, stereo_model_number_.c_str());
    vrp->SetStringProperty(container, vr::Prop_ManufacturerName_String, "VRto3D");
    vrp->SetStringProperty(container, vr::Prop_TrackingFirmwareVersion_String, "1.0");
    vrp->SetStringProperty(container, vr::Prop_HardwareRevision_String, "1.0");

    // Display settings
    vrp->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, config_.depth);
    vrp->SetFloatProperty(container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f);
    vrp->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, config_.display_frequency);
    vrp->SetFloatProperty(container, vr::Prop_SecondsFromVsyncToPhotons_Float, config_.display_latency);
    vrp->SetBoolProperty(container, vr::Prop_IsOnDesktop_Bool, false);
    vrp->SetBoolProperty(container, vr::Prop_DisplayDebugMode_Bool, true);
    vrp->SetBoolProperty(container, vr::Prop_HasDriverDirectModeComponent_Bool, true);
    vrp->SetBoolProperty(container, vr::Prop_HasDisplayComponent_Bool, true);
    vrp->SetBoolProperty(container, vr::Prop_Hmd_AllowSupersampleFiltering_Bool, config_.ss_enable);
    if (config_.depth_gauge)
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
    vrp->SetStringProperty(container, vr::Prop_DriverProvidedChaperoneJson_String, chaperoneJson.c_str());
    vrp->SetUint64Property(container, vr::Prop_CurrentUniverseId_Uint64, 64);
    vrs->SetInt32(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_Style_Int32, vr::COLLISION_BOUNDS_STYLE_NONE);
    vrs->SetBool(vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_GroundPerimeterOn_Bool, false);
  
    // Miscellaneous settings
    vrp->SetBoolProperty(container, vr::Prop_WillDriftInYaw_Bool, false);
    vrp->SetBoolProperty(container, vr::Prop_DeviceIsWireless_Bool, false);
    vrp->SetBoolProperty(container, vr::Prop_DeviceIsCharging_Bool, false);
    vrp->SetBoolProperty(container, vr::Prop_ContainsProximitySensor_Bool, false);
    vrp->SetBoolProperty(container, vr::Prop_DeviceCanPowerOff_Bool, false);

    // Set supersample scale
    vrs->SetFloat(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_SupersampleScale_Float, config_.ss_scale);

    // Miscellaneous settings
    vrs->SetBool(vr::k_pch_DirectMode_Section, vr::k_pch_DirectMode_Enable_Bool, true);
    vrs->SetFloat(vr::k_pch_Power_Section, vr::k_pch_Power_TurnOffScreensTimeout_Float, 86400.0f);
    vrs->SetBool(vr::k_pch_Power_Section, vr::k_pch_Power_PauseCompositorOnStandby_Bool, false);
    vrs->SetBool(vr::k_pch_Dashboard_Section, vr::k_pch_Dashboard_EnableDashboard_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_EnableHomeApp, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_MirrorViewVisibility_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_EnableSafeMode, false);

    pose_update_thread_ = std::thread(&OVR_3DV_Driver::PoseUpdateThread, this);


  if ( m_generateVSync )
  {
    vrp->SetBoolProperty( m_ulPropertyContainer, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool, true );
    m_runVSyncThread = true;
    m_vSyncThread    = std::thread( &OVR_3DV_Driver::VSyncThread, this );
  }
  else
  {
    vrp->SetBoolProperty( m_ulPropertyContainer, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool, false );
    m_runVSyncThread = false;
  }

  // set proximity senser to always on, always head present
  auto                          input     = vr::VRDriverInput();
  vr::VRInputComponentHandle_t  prox;
  input->CreateBooleanComponent( container, "/proximity", &prox );
  input->UpdateBooleanComponent( prox, true, 0.0 );

  return vr::VRInitError_None;
}


void OVR_3DV_Driver::Deactivate()
{
    
  if ( m_runVSyncThread )
  {
    m_runVSyncThread = false;
    m_vSyncThread.join();
  }

    device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}


//-----------------------------------------------------------------------------
// Purpose: Stub for Standby mode
//-----------------------------------------------------------------------------
void OVR_3DV_Driver::EnterStandby() {}


//-----------------------------------------------------------------------------
// Purpose: Return self as an vr::IVRDisplayComponent or vr::IVRDirectModeComponent.
//-----------------------------------------------------------------------------
void * OVR_3DV_Driver::GetComponent( const char * pchComponentNameAndVersion )
{
    DriverLog( "GetComponent(%s)\n", pchComponentNameAndVersion );

    if ( !_stricmp( pchComponentNameAndVersion, vr::IVRDriverDirectModeComponent_Version ) )
    {
        DriverLog( "  returning IVRDriverDirectModeComponent interface\n" );
        return (IVRDriverDirectModeComponent *)this;
    }
    else if ( !_stricmp( pchComponentNameAndVersion, vr::IVRDisplayComponent_Version ) )
    {
        DriverLog( "  returning IVRDisplayComponent interface\n" );
        return (vr::IVRDisplayComponent *)this;
    }

    return NULL;
}


//-----------------------------------------------------------------------------
// Purpose: This is called by vrserver when a debug request has been made from an application to the driver.
// What is in the response and request is up to the application and driver to figure out themselves.
//-----------------------------------------------------------------------------
void OVR_3DV_Driver::DebugRequest( const char * pchRequest, char * pchResponseBuffer, uint32_t unResponseBufferSize )
{
    if ( unResponseBufferSize >= 1 )
        pchResponseBuffer[0] = 0;
}


//-----------------------------------------------------------------------------
// Purpose: Static Pose with pitch adjustment
//-----------------------------------------------------------------------------
vr::DriverPose_t OVR_3DV_Driver::GetPose()
{
    static float currentPitch = 0.0f; // Keep track of the current pitch

    vr::DriverPose_t pose = { 0 };

    pose.qWorldFromDriverRotation.w = 1.f;
    pose.qDriverFromHeadRotation.w = 1.f;

    pose.qRotation.w = 1.f;

    pose.vecPosition[0] = 0.0f;
    pose.vecPosition[1] = 1.0f;
    pose.vecPosition[2] = 0.0f;

    pose.poseIsValid = true;
    pose.deviceIsConnected = true;
    pose.result = vr::TrackingResult_Running_OK;

    // For HMDs we want to apply rotation/motion prediction
    pose.shouldApplyHeadModel = false;

    // Adjust pitch based on controller input
    if (config_.ctrl_enable)
    {
        AdjustPitch(pose.qRotation, currentPitch);
    }

    return pose;
}




void OVR_3DV_Driver::CreateSwapTextureSet( uint32_t unPid, const SwapTextureSetDesc_t * pSwapTextureSetDesc, SwapTextureSet_t * pOutSwapTextureSet )
{
  TextureSet * texSet = new TextureSet;
  int          height = pSwapTextureSetDesc->nHeight;
  int          width  = pSwapTextureSetDesc->nWidth;

  DriverLog( "CreateSwapTextureSet(pid=%u, res=%d x %d)\n", unPid, width, height );

  if ( height & 1 )
  {
    height++;
  }

  if ( width & 1 )
  {
    width++;
  }

  for ( int i = 0; i < TextureSet::NUM_TEX; i++ )
  {
    ID3D11Texture2D * t = m_renderHelper.CreateTexture( width, height, D3D11_RESOURCE_MISC_SHARED, (DXGI_FORMAT)pSwapTextureSetDesc->nFormat );

    IDXGIResource * pDXGIResource = NULL;
    t->QueryInterface( __uuidof( IDXGIResource ), (LPVOID *)&pDXGIResource );

    HANDLE h;
    pDXGIResource->GetSharedHandle( &h );
    pDXGIResource->Release();

    if ( !h )
    {
      DriverLog( "Failed to retrieve the shared texture handle.\n" );
      return;
    }

    pOutSwapTextureSet->rSharedTextureHandles[i] = (vr::SharedTextureHandle_t)h;

    texSet->m_tex[i]           = t;
    texSet->m_sharedHandles[i] = h;
  }
  texSet->m_textureFormat = (DXGI_FORMAT)pSwapTextureSetDesc->nFormat;
  texSet->m_srcIdx        = 0;
  texSet->m_nextIdx       = 0;
  texSet->m_width         = width;
  texSet->m_height        = height;
  texSet->m_pid           = unPid;

  EnterCriticalSection( &m_texSetCS );
  m_texSets.push_back( texSet );
  LeaveCriticalSection( &m_texSetCS );

  DriverLog( "CreateSwapTextureSet done\n" );
}

void OVR_3DV_Driver::DestroySwapTextureSet( vr::SharedTextureHandle_t sharedTextureHandle )
{
  DriverLog( "DestroySwapTextureSet\n" );

  EnterCriticalSection( &m_texSetCS );
  for ( size_t i = 0; i < m_texSets.size(); i++ )
  {
    TextureSet * set = m_texSets[i];

    if ( sharedTextureHandle == (vr::SharedTextureHandle_t)set->m_sharedHandles[0] ||
         sharedTextureHandle == (vr::SharedTextureHandle_t)set->m_sharedHandles[1] ||
         sharedTextureHandle == (vr::SharedTextureHandle_t)set->m_sharedHandles[2] )
    {
      set->Release();
      delete set;
      m_texSets.erase( m_texSets.begin() + i );

      break;
    }
  }
  LeaveCriticalSection( &m_texSetCS );
}

void OVR_3DV_Driver::DestroyAllSwapTextureSets( uint32_t unPid )
{
  DriverLog( "DestroyAllSwapTextureSets(pid=%u)\n", unPid );

  std::vector<TextureSet *> newSet;

  EnterCriticalSection( &m_texSetCS );

  for ( auto set : m_texSets )
  {
    if ( unPid != ( ~0 ) && set->m_pid != unPid )
    {
      newSet.push_back( set );
    }
    else
    {
      set->Release();
      delete set;
    }
  }
  m_texSets.swap( newSet );
  LeaveCriticalSection( &m_texSetCS );
}

void OVR_3DV_Driver::GetNextSwapTextureSetIndex( vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t ( *pIndices )[2] )
{
  EnterCriticalSection( &m_texSetCS );

  for ( int i = 0; i < 2; i++ )
  {
    ( *pIndices )[i] = 0;

    int nextSwapChainTexIndex = -1;  // enum the tex set to the right index instead of reusing set->m_index
                                     // since the app can request out of order tex handles i.e. instead of
                                     // passing shared handles indexed @ 0,1,2 the app can come in with 0,2,1.
    for ( auto & set : m_texSets )
    {
      for ( int sharedhandleIndex = 0; sharedhandleIndex < TextureSet::NUM_TEX; sharedhandleIndex++ )
      {
        if ( sharedTextureHandles[i] == (vr::SharedTextureHandle_t)set->m_sharedHandles[sharedhandleIndex] )
        {
          nextSwapChainTexIndex = sharedhandleIndex;
          break;
        }
      }

      if ( nextSwapChainTexIndex != -1 )
      {
        set->m_nextIdx   = ( set->m_nextIdx + 1 ) % TextureSet::NUM_TEX;
        ( *pIndices )[i] = set->m_nextIdx;

        break;
      }
    }
  }

  LeaveCriticalSection( &m_texSetCS );
}

void OVR_3DV_Driver::SubmitLayer( const SubmitLayerPerEye_t ( &perEye )[2] ) {}

void OVR_3DV_Driver::Present( vr::SharedTextureHandle_t syncTexture ) {}



//-----------------------------------------------------------------------------
// Purpose: To inform vrcompositor what the window bounds for this virtual HMD are.
//-----------------------------------------------------------------------------
void OVR_3DV_Driver::GetWindowBounds(int32_t* pnX, int32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight)
{
    *pnX = config_.window_x;
    *pnY = config_.window_y;
    *pnWidth = config_.window_width;
    *pnHeight = config_.window_height;
}


//-----------------------------------------------------------------------------
// Purpose: To inform vrcompositor if this display is considered an on-desktop display.
//-----------------------------------------------------------------------------
bool OVR_3DV_Driver::IsDisplayOnDesktop()
{
    return false;
}


//-----------------------------------------------------------------------------
// Purpose: To as vrcompositor to search for this display.
//-----------------------------------------------------------------------------
bool OVR_3DV_Driver::IsDisplayRealDisplay()
{
    return false;
}


//-----------------------------------------------------------------------------
// Purpose: To inform the rest of the vr system what the recommended target size should be
//-----------------------------------------------------------------------------
void OVR_3DV_Driver::GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight)
{
    *pnWidth = config_.render_width;
    *pnHeight = config_.render_height;
}


//-----------------------------------------------------------------------------
// Purpose: Render Full-Res 3DVision
//-----------------------------------------------------------------------------
void OVR_3DV_Driver::GetEyeOutputViewport( vr::EVREye eEye, uint32_t * pnX, uint32_t * pnY, uint32_t * pnWidth, uint32_t * pnHeight )
{
  *pnY      = 0;
  *pnWidth  = config_.render_width;
  *pnHeight = config_.render_height;

  if ( eEye == vr::Eye_Left )
  {
    *pnX = 0;
  }
  else
  {
    *pnX = config_.render_width / 2;
  }
}


//-----------------------------------------------------------------------------
// Purpose: Utilize the desired FoV, Aspect Ratio, and Convergence settings
//-----------------------------------------------------------------------------
void OVR_3DV_Driver::GetProjectionRaw(vr::EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom)
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
vr::DistortionCoordinates_t OVR_3DV_Driver::ComputeDistortion(vr::EVREye eEye, float fU, float fV)
{
    vr::DistortionCoordinates_t coordinates{};
    coordinates.rfBlue[0] = fU;
    coordinates.rfBlue[1] = fV;
    coordinates.rfGreen[0] = fU;
    coordinates.rfGreen[1] = fV;
    coordinates.rfRed[0] = fU;
    coordinates.rfRed[1] = fV;
    return coordinates;
}
bool OVR_3DV_Driver::ComputeInverseDistortion(vr::HmdVector2_t* pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV)
{
    return false;
}





#define CALC_JITTER 1

void OVR_3DV_Driver::VSyncThread()
{
  DriverLog( "OVR_3DV_Driver::VSyncThread thread started, frequency %fHz, id %i, genstyle %i\n", config_.display_frequency, std::this_thread::get_id(), m_vsyncStyle );

  using Clock     = std::chrono::steady_clock;
  using Duration  = std::chrono::duration<double, std::nano>;
  using Timepoint = std::chrono::time_point<Clock, Duration>;

  Duration period = std::chrono::seconds( 1 ) / config_.display_frequency;

  Timepoint last = Clock::now();

#if CALC_JITTER
  size_t    n{ 0 };                      // number of samples
  double    sd{ 0 };                     // sum of differences from period (for averaging), in milliseconds
  double    ssd{ 0 };                    // sum of squares of differences from period (for approximating standard deviation}, in milliseconds
  Timepoint lastVS{ Duration::zero() };  // time of last VSync event
#endif

  while ( m_runVSyncThread )
  {
    Timepoint now  = Clock::now();
    Timepoint next = last + period;
    Duration  wait = next - now;
    if ( wait.count() > 0 )
    {
      switch ( m_vsyncStyle )
      {
        case VSyncStyle::SLEEP:
          // simple sleep - the granularity of sleep_for varies by platform
          // and can be above 1ms - be cautious with higher frequencies
          // 'wait' often is also just a minimum
          std::this_thread::sleep_for( wait );
          break;

        case VSyncStyle::SLEEP_BUSY:
          // sleep one millisecond less, then busy wait
          if ( wait > std::chrono::milliseconds( 2 ) )
          {
            // minimum is one millisecond, so check for two in if
            std::this_thread::sleep_for( wait - std::chrono::milliseconds( 1 ) );
          }
          while ( Clock::now() < next )
          {
          }
          break;

        case VSyncStyle::BUSY:
          // just busy wait
          while ( Clock::now() < next )
          {
          }
          break;
      }

      last = next;
    }
    else
    {
      last = now;
    }

    vr::VRServerDriverHost()->VsyncEvent( 0 );

#if CALC_JITTER
    if ( lastVS != Timepoint( Duration::zero() ) )
    {
      now = Clock::now();
      double diff{ std::chrono::duration<double, std::micro>( now - lastVS ).count() };
      sd += diff;
      double diffsq{ diff * diff };
      ssd += diff;
      ++n;
    }
    lastVS = now;
#endif
  }

  DriverLog( "VSyncThread thread ending\n" );

#if CALC_JITTER
  std::stringstream ss;

  double avg    = sd / n;
  double stddev = sqrt( ssd / n );

  ss << "Jitter calculation, vsync style " << m_vsyncStyle << "\n";
  ss << "\t# Samples: " << n << "\n";
  ss << std::fixed << std::setprecision( 3 );
  ss << "\tAverage deviation:  " << avg << " us\n";
  ss << "\tStandard deviation: " << stddev << " us\n";
  DriverLog( ss.str().c_str() );
#endif
}