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

#define WIN32_LEAN_AND_MEAN
#include "hmd_device_driver.h"
#include "dx11_renderer.h"
#include "platform/platform.h"
#include "vrto3dlib/key_mappings.h"
#include "vrto3dlib/json_manager.h"
#include "vrto3dlib/app_id_mgr.h"
#include "vrto3dlib/overlay_mgr.h"
#include "vrto3dlib/win32_helper.hpp"
#include "vrmath.h"

#include <string>
#include <sstream>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment (lib, "WSock32.Lib")
#include <windows.h>
#include <xinput.h>


// Load settings from default.vrsettings
static const char *stereo_main_settings_section = "driver_vrto3d";

constexpr float kMinDeltaTimeSeconds = 1e-5f;
constexpr float kSmallAngleEpsilon = 1e-5f;

float ApplyDeadzone(float value, float deadzone)
{
    if (std::abs(value) < deadzone)
    {
        return 0.0f;
    }

    if (value > 0.0f)
    {
        return (value - deadzone) / (1.0f - deadzone);
    }

    return (value + deadzone) / (1.0f - deadzone);
}

MockControllerDeviceDriver::MockControllerDeviceDriver()
{
    // Keep track of whether Activate() has been called
    is_active_ = false;
    curr_pose_ = { 0 };
    app_name_ = "";
    prev_name_ = "";
    app_pid_ = 0;
    launch_script_executed_ = false;

    auto* vrs = vr::VRSettings();
    JsonManager json_manager;
    json_manager.EnsureDefaultConfigExists();

    char model_number[ 1024 ];
    vrs->GetString( stereo_main_settings_section, "model_number", model_number, sizeof( model_number ) );
    stereo_model_number_ = model_number;
    char serial_number[ 1024 ];
    vrs->GetString( stereo_main_settings_section, "serial_number", serial_number, sizeof( serial_number ) );
    stereo_serial_number_ = serial_number;
    char version_number[ 1024 ];
    vrs->GetString( stereo_main_settings_section, "version_number", version_number, sizeof( version_number ) );
    stereo_version_number_ = version_number;

    LOG() << "VRto3D Model Number: " << stereo_model_number_.c_str();
    LOG() << "VRto3D Serial Number: " << stereo_serial_number_.c_str();

    SwitchToXinpuGetStateEx();

    // Display settings
    StereoDisplayDriverConfiguration display_configuration{};
    display_configuration.display_index = 0;
    display_configuration.window_x = 0;
    display_configuration.window_y = 0;
    display_configuration.window_width = 1920;
    display_configuration.window_height = 1080;
    json_manager.LoadParamsFromJson(display_configuration);

    // Profile settings
    json_manager.LoadProfileFromJson(DEF_CFG, display_configuration);

    // Resolve display-index-driven window bounds from the active desktop layout
    const bool monitor_bounds_applied = ApplyDisplaySelectionToWindowConfig(display_configuration);
    LOG()
        << "Pre-init window bounds before StereoDisplayComponent: resolved="
        << (monitor_bounds_applied ? "true" : "false")
        << " display_index=" << display_configuration.display_index
        << " bounds=(" << display_configuration.window_x << "," << display_configuration.window_y
        << " " << display_configuration.window_width << "x" << display_configuration.window_height << ")";

    // Instantiate our display component
    stereo_display_component_ = std::make_unique< StereoDisplayComponent >( display_configuration );

    LOG() << "Default Config Loaded";
}


//-----------------------------------------------------------------------------
// Purpose: Initialize all settings and notify SteamVR
//-----------------------------------------------------------------------------
vr::EVRInitError MockControllerDeviceDriver::Activate( uint32_t unObjectId )
{
    // SteamVR calls Activate twice — once for TrackedDeviceClass_HMD and once
    // for TrackedDeviceClass_DisplayRedirect (same object, same serial). On the
    // second call we only need to stash the index and set the minimal property
    // the compositor reads on the DR device.
    if (is_active_.exchange(true)) {
        display_redirect_index_ = unObjectId;
        auto* vrp = vr::VRProperties();
        vr::PropertyContainerHandle_t dr_container = vrp->TrackedDeviceToPropertyContainer(unObjectId);
        LUID luid = platform::PrimaryAdapterLuid();
        uint64_t luid_u64 = (static_cast<uint64_t>(luid.HighPart) << 32) | luid.LowPart;
        vrp->SetUint64Property(dr_container, vr::Prop_GraphicsAdapterLuid_Uint64, luid_u64);
        LOG() << "MockControllerDeviceDriver::Activate (DisplayRedirect) object_id=" << unObjectId;
        return vr::VRInitError_None;
    }

    device_index_ = unObjectId;
    is_on_top_ = false;
    man_on_top_ = false;
    ue3d_on_top_ = false;
    take_screenshot_ = false;
    app_updated_ = false;
    no_profile_ = false;

    stereo_display_component_->Init(device_index_);

    if (!SetOpenXRRuntimeToSteamVR()) {
        LOG() << "OpenXR ActiveRuntime switch to SteamVR failed. Continuing activation.";
    }

    // A list of properties available is contained in vr::ETrackedDeviceProperty.
    auto* vrp = vr::VRProperties();
    auto* vrs = vr::VRSettings();
    vr::PropertyContainerHandle_t container = vrp->TrackedDeviceToPropertyContainer( device_index_ );
    vrp->SetStringProperty( container, vr::Prop_ModelNumber_String, stereo_model_number_.c_str() );
    vrp->SetStringProperty( container, vr::Prop_ManufacturerName_String, "VRto3D");
    vrp->SetStringProperty( container, vr::Prop_TrackingFirmwareVersion_String, "1.0");
    vrp->SetStringProperty( container, vr::Prop_HardwareRevision_String, "1.0");

    // Query the target output monitor (resolved by display_index) for
    // display frequency + approximate vsync-to-photons latency, then write
    // the computed values back into the config so downstream consumers
    // (pose thread cadence, hotkey sleep counts) pick them up.
    {
        auto cfg = stereo_display_component_->GetConfig();
        platform::MonitorInfo primary{}, secondary{};
        if (platform::ResolveTargetMonitors(cfg.display_index, false, primary, secondary)) {
            cfg.display_frequency = platform::QueryRefreshHz(primary, 60.0f);
        } else {
            cfg.display_frequency = 60.0f;
        }
        cfg.display_latency   = (cfg.display_frequency > 1.0f)
            ? (0.5f / cfg.display_frequency) : 0.011f;
        cfg.sleep_count_max   = (int)(floor(1600.0 / (1000.0 / cfg.display_frequency)));
        stereo_display_component_->LoadSettings(cfg);
        LOG() << "Display: target=" << primary.device_name
              << " freq=" << cfg.display_frequency << "Hz"
              << " latency=" << cfg.display_latency << "s";
    }

    // Display settings
    vrp->SetFloatProperty( container, vr::Prop_UserIpdMeters_Float, stereo_display_component_->GetConfig().depth);
    vrp->SetFloatProperty( container, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f);
    vrp->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, stereo_display_component_->GetConfig().display_frequency);
    vrp->SetFloatProperty( container, vr::Prop_SecondsFromVsyncToPhotons_Float, stereo_display_component_->GetConfig().display_latency);
    vrp->SetFloatProperty( container, vr::Prop_SecondsFromPhotonsToVblank_Float, 0.0);
    vrp->SetBoolProperty( container, vr::Prop_ReportsTimeSinceVSync_Bool, false);
    vrp->SetBoolProperty( container, vr::Prop_IsOnDesktop_Bool, false);
    vrp->SetBoolProperty( container, vr::Prop_DisplayDebugMode_Bool, true);
    vrp->SetBoolProperty( container, vr::Prop_HasDriverDirectModeComponent_Bool, false);

    // Direct-mode virtual-display integration: advertise our adapter LUID so
    // the compositor composites on the same GPU the renderer will use, and
    // signal that we fire VsyncEvent ourselves from the renderer thread.
    {
        LUID luid = platform::PrimaryAdapterLuid();
        uint64_t luid_u64 = (static_cast<uint64_t>(luid.HighPart) << 32) | luid.LowPart;
        vrp->SetUint64Property(container, vr::Prop_GraphicsAdapterLuid_Uint64, luid_u64);
        vrp->SetBoolProperty  (container, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool, true);
    }
    if (stereo_display_component_->GetConfig().dash_enable)
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
    vrs->SetBool(vr::k_pch_Dashboard_Section, vr::k_pch_Dashboard_EnableDashboard_Bool, stereo_display_component_->GetConfig().dash_enable);
    vrs->SetBool(vr::k_pch_Dashboard_Section, vr::k_pch_Dashboard_ArcadeMode_Bool, !stereo_display_component_->GetConfig().dash_enable);
    vrs->SetBool(vr::k_pch_Dashboard_Section, "allowAppQuitting", stereo_display_component_->GetConfig().dash_enable);
    vrs->SetBool(vr::k_pch_Dashboard_Section, "autoShowGameTheater", false);
    vrs->SetBool(vr::k_pch_Dashboard_Section, "showDesktop", false);
    vrs->SetBool(vr::k_pch_Dashboard_Section, "showPowerOptions", false);
    vrs->SetBool(vr::k_pch_Dashboard_Section, "inputCaptureEnabled", false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_EnableHomeApp, stereo_display_component_->GetConfig().dash_enable);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_MirrorViewVisibility_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_EnableSafeMode, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_DisplayDebug_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_MotionSmoothing_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_DisableAsyncReprojection_Bool, !stereo_display_component_->GetConfig().async_enable);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_AllowSupersampleFiltering_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_SupersampleManualOverride_Bool, true);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_ForceFadeOnBadTracking_Bool, false);
    vrs->SetBool(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_ActivateMultipleDrivers_Bool, true);
    vrs->SetString(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_ForcedDriverKey_String, "vrto3d");

    const auto launch_script = stereo_display_component_->GetConfig().launch_script;
    bool can_execute_launch_script = false;
    if (!launch_script.empty() && launch_script_executed_.compare_exchange_strong(can_execute_launch_script, true))
    {
        std::thread([launch_script]() {
            LOG() << "Executing launch_script: " << launch_script.c_str();
            const std::string command = "cmd.exe /C " + launch_script;
            const int result = std::system(command.c_str());
            if (result == 0) {
                LOG() << "launch_script completed successfully";
            }
            else {
                LOG() << "launch_script failed with exit code: " << result;
            }
        }).detach();
    }
    
    // Thread setup. FocusUpdateThread is gone — its responsibilities (window
    // placement + always-on-top) now live inside WindowPresenter, which is
    // owned by VirtualDisplayDevice and observes the focus atomics below via
    // GetFocusContext().
    xinput_thread_ = std::thread(&MockControllerDeviceDriver::XInputUpdateThread, this);
    pose_thread_ = std::thread(&MockControllerDeviceDriver::PoseUpdateThread, this);
    hotkey_thread_ = std::thread(&MockControllerDeviceDriver::PollHotkeysThread, this);
    depth_thread_ = std::thread(&MockControllerDeviceDriver::AutoDepthThread, this);
    if (stereo_display_component_->GetConfig().use_open_track) {
        open_track_att_ = HmdQuaternion_Identity;
		open_track_pos_ = { 0.0, 0.0, 0.0 };
        track_thread_ = std::thread(&MockControllerDeviceDriver::OpenTrackThread, this);
    }

    HANDLE thread_handle = pose_thread_.native_handle();

    // Set the thread priority
    if (!SetThreadPriority(thread_handle, THREAD_PRIORITY_HIGHEST)) {
        // Handle error if setting priority fails
        LOG() << "Failed to set thread priority: " << GetLastError();
    }

    // Direct-mode virtual-display: build the DX11 renderer + selected presenter
    // on the same adapter LUID we advertised on the HMD's property container.
    // The compositor will hand composited frames to our Present() override once
    // it has the matching DisplayRedirect registration (same serial, class 5).
    {
        LUID luid = platform::PrimaryAdapterLuid();
        renderer_ = std::make_unique<Dx11Renderer>();
        if (!renderer_->Init(luid, stereo_display_component_->GetConfig(), GetFocusContext())) {
            LOG() << "Dx11Renderer::Init failed; IVRVirtualDisplay path will be inactive";
            renderer_.reset();
        }
    }

    LOG() << "Activation Complete";

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
    if ( strcmp( pchComponentNameAndVersion, vr::IVRVirtualDisplay_Version ) == 0 )
    {
        return static_cast<vr::IVRVirtualDisplay*>(this);
    }

    return nullptr;
}

//-----------------------------------------------------------------------------
// IVRVirtualDisplay — compositor hands us the composited SBS backbuffer each
// frame. We forward to Dx11Renderer which copies to our internal texture and
// invokes the selected presenter.
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::Present( const vr::PresentInfo_t *pPresentInfo, uint32_t unPresentInfoSize )
{
    if ( !pPresentInfo || unPresentInfoSize < sizeof(vr::PresentInfo_t) || !renderer_ ) return;

    static std::atomic<bool> first_present{ true };
    bool expected = true;
    if ( first_present.compare_exchange_strong(expected, false) ) {
        LOG() << "IVRVirtualDisplay::Present first call, handle=0x"
              << std::hex << pPresentInfo->backbufferTextureHandle
              << " frameId=" << std::dec << pPresentInfo->nFrameId;
    }
    renderer_->OnPresent(*pPresentInfo);
}

void MockControllerDeviceDriver::WaitForPresent()
{
    // v1: return immediately. Pacing comes from VsyncEvent fired by Dx11Renderer
    // after the presenter returns.
}

bool MockControllerDeviceDriver::GetTimeSinceLastVsync( float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter )
{
    if ( !renderer_ ) return false;

    LARGE_INTEGER f{}, q{};
    if ( !QueryPerformanceFrequency(&f) || !QueryPerformanceCounter(&q) ) return false;
    const double now  = static_cast<double>(q.QuadPart) / static_cast<double>(f.QuadPart);
    const double last = renderer_->LastVsyncQpcSec();
    if ( pfSecondsSinceLastVsync ) *pfSecondsSinceLastVsync = static_cast<float>(now - last);
    if ( pulFrameCounter ) *pulFrameCounter = renderer_->FrameCounter();
    return true;
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
// Purpose: Receive OpenTrack 3DoF updates
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::OpenTrackThread()
{
    SOCKET socket_s;
    struct sockaddr_in from = {};
    int from_len = sizeof(from);
    struct TOpenTrack {
        double X;
        double Y;
        double Z;
        double Yaw;
        double Pitch;
        double Roll;
    };
    TOpenTrack open_track;
    auto ot_port = stereo_display_component_->GetConfig().open_track_port;

    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        LOG() << "WSAStartup failed: " << iResult;
    }
    else {
        struct sockaddr_in local = {};
        local.sin_family = AF_INET;
        local.sin_port = htons(ot_port);
        local.sin_addr.s_addr = INADDR_ANY;

        socket_s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_s == INVALID_SOCKET) {
            LOG() << "Socket creation failed: " << WSAGetLastError();
            WSACleanup();  // Cleanup only if socket creation fails
        }
        else {
            // Set non-blocking mode
            u_long nonblocking_enabled = 1;
            if (ioctlsocket(socket_s, FIONBIO, &nonblocking_enabled) == SOCKET_ERROR) {
                LOG() << "Failed to set non-blocking mode: " << WSAGetLastError();
                closesocket(socket_s);
                WSACleanup();
            }
            else if (bind(socket_s, (struct sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
                LOG() << "Bind failed: " << WSAGetLastError();
                closesocket(socket_s);
                WSACleanup();
            }
        }
    }

    while (is_active_) {
        //Read UDP socket with OpenTrack data
        memset(&open_track, 0, sizeof(open_track));
        auto bytes_read = recvfrom(socket_s, (char*)(&open_track), sizeof(open_track), 0, (sockaddr*)&from, &from_len);

        if (bytes_read > 0) {
            const auto sample_time = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(trk_mutex_);
                open_track_att_ = HmdQuaternion_FromEulerAngles(DEG_TO_RAD(open_track.Roll), DEG_TO_RAD(open_track.Pitch), DEG_TO_RAD(-open_track.Yaw));
                // Map Opentrack pose data to steam_vr coordinate system
                open_track_pos_ = { -(open_track.X / 100.0f), -(open_track.Y / 100.0f), open_track.Z / 100.0f };
            }

            open_track_pose_sample_time_seconds_.store(
                std::chrono::duration<double>(sample_time.time_since_epoch()).count(),
                std::memory_order_relaxed);
        }
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    closesocket(socket_s);
    WSACleanup();
}


//-----------------------------------------------------------------------------
// Purpose: Poll XInput and publish controller-derived rotation and offset
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::XInputUpdateThread()
{
    float current_pitch = 0.0f;
    vr::HmdQuaternion_t current_yaw_quat = HmdQuaternion_Identity;

    while (is_active_)
    {
        const auto loop_start = std::chrono::steady_clock::now();
        const auto config = stereo_display_component_->GetConfig();

        XINPUT_STATE state;
        ZeroMemory(&state, sizeof(XINPUT_STATE));
        const bool got_xinput = (_XInputGetState(0, &state) == ERROR_SUCCESS);

        if (config.pitch_enable && got_xinput)
        {
            float normalized_y = state.Gamepad.sThumbRY / 32767.0f;
            normalized_y = ApplyDeadzone(normalized_y, config.ctrl_deadzone);

            current_pitch += (normalized_y * config.ctrl_sensitivity);
            current_pitch = std::clamp(current_pitch, -90.0f, 90.0f);
        }

        if (config.yaw_enable && got_xinput)
        {
            float normalized_x = state.Gamepad.sThumbRX / 32767.0f;
            normalized_x = ApplyDeadzone(normalized_x, config.ctrl_deadzone);

            const float yaw_adjustment = -normalized_x * config.ctrl_sensitivity;
            const vr::HmdQuaternion_t yaw_quat_adjust =
                QuaternionFromAxisAngle(0.0f, 1.0f, 0.0f, DEG_TO_RAD(yaw_adjustment));

            current_yaw_quat = HmdQuaternion_Normalize(yaw_quat_adjust * current_yaw_quat);
        }

        if (config.pose_reset)
        {
            current_pitch = 0.0f;
            current_yaw_quat = HmdQuaternion_Identity;
            stereo_display_component_->SetReset();
        }

        const float pitch_radians = DEG_TO_RAD(current_pitch);
        const vr::HmdQuaternion_t pitch_quaternion =
            QuaternionFromAxisAngle(1.0f, 0.0f, 0.0f, pitch_radians);

        const vr::HmdQuaternion_t controller_rotation =
            HmdQuaternion_Normalize(current_yaw_quat * pitch_quaternion);

        // Build pitch-radius offset in local (pre-yaw) space, then rotate by yaw quaternion.
        const vr::HmdVector3_t local_pitch_radius_offset = {
            0.0f,
            static_cast<float>(-config.pitch_radius * std::sin(pitch_radians)),
            static_cast<float>(config.pitch_radius * (std::cos(pitch_radians) - 1.0f))
        };
        const vr::HmdVector3_t rotated_pitch_radius_offset = local_pitch_radius_offset * current_yaw_quat;

        std::array<double, 3> controller_pos_offset = {
            rotated_pitch_radius_offset.v[0],
            rotated_pitch_radius_offset.v[1],
            rotated_pitch_radius_offset.v[2]
        };

        {
            std::lock_guard<std::mutex> lock(controller_pose_mutex_);
            controller_rotation_ = controller_rotation;
            controller_pos_offset_ = controller_pos_offset;
        }

        const auto sample_time = std::chrono::steady_clock::now();
        xinput_pose_sample_time_seconds_.store(
            std::chrono::duration<double>(sample_time.time_since_epoch()).count(),
            std::memory_order_relaxed);

        const auto elapsed = std::chrono::steady_clock::now() - loop_start;
        const auto xinput_period = std::chrono::milliseconds(8); // 125Hz
        if (elapsed < xinput_period)
        {
            std::this_thread::sleep_for(xinput_period - elapsed);
        }
    }
}


//-----------------------------------------------------------------------------
// Purpose: Compose final pose and calculate velocity/acceleration
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::PoseUpdateThread()
{
    while (is_active_)
    {
        const auto loop_start = std::chrono::steady_clock::now();
        double pose_sample_time = 0.0;
        const auto config = stereo_display_component_->GetConfig();
        vr::DriverPose_t pose = { 0 };

        // Monitor mode: static pose, skip VR tracking logic.
        if (stereo_display_component_->IsMonitorMode())
        {
            if (track_filter_was_enabled_)
            {
                track_filter_.Reset();
                track_filter_was_enabled_ = false;
            }

            pose.qWorldFromDriverRotation = HmdQuaternion_Identity;
            pose.qDriverFromHeadRotation = HmdQuaternion_Identity;
            pose.qRotation = HmdQuaternion_Identity;
            pose.vecPosition[1] = config.hmd_height;
            pose.poseIsValid = true;
            pose.deviceIsConnected = true;
            pose.result = vr::TrackingResult_Running_OK;
            pose.shouldApplyHeadModel = false;
            pose.willDriftInYaw = false;
            pose.poseTimeOffset = 0;
        }
        // Default mode: Static + HMD Emulation
        else {
            vr::HmdQuaternion_t controller_rotation = HmdQuaternion_Identity;
            std::array<double, 3> controller_pos_offset = { 0.0, 0.0, 0.0 };
            {
                std::lock_guard<std::mutex> lock(controller_pose_mutex_);
                controller_rotation = controller_rotation_;
                controller_pos_offset = controller_pos_offset_;
            }

            const vr::HmdQuaternion_t hmd_yaw_quat =
                QuaternionFromAxisAngle(0.0f, 1.0f, 0.0f, DEG_TO_RAD(config.hmd_yaw));
            const vr::HmdQuaternion_t final_controller_rotation =
                HmdQuaternion_Normalize(hmd_yaw_quat * controller_rotation);

            pose.qWorldFromDriverRotation = HmdQuaternion_Identity;
            pose.qDriverFromHeadRotation = HmdQuaternion_Identity;
            pose.qRotation = final_controller_rotation;

            // Calculate Position
            pose.vecPosition[0] = config.hmd_x;
            pose.vecPosition[1] = config.hmd_height;
            pose.vecPosition[2] = config.hmd_y;
            if (config.use_open_track)
            {
                std::lock_guard<std::mutex> lock(trk_mutex_);
                pose.qRotation = HmdQuaternion_Normalize(final_controller_rotation * open_track_att_);
                pose.vecPosition[0] += open_track_pos_[0];
                pose.vecPosition[1] += open_track_pos_[1];
                pose.vecPosition[2] += open_track_pos_[2];
                pose_sample_time = open_track_pose_sample_time_seconds_.load(std::memory_order_relaxed);
            }
            else
            {
                pose.qRotation = final_controller_rotation;
                pose.vecPosition[0] += controller_pos_offset[0];
                pose.vecPosition[1] += controller_pos_offset[1];
                pose.vecPosition[2] += controller_pos_offset[2];
                if (pose.vecPosition[1] < config.hmd_height - 1.0)
                {
                    pose.vecPosition[1] = config.hmd_height - 1.0;
                }
                pose_sample_time = xinput_pose_sample_time_seconds_.load(std::memory_order_relaxed);
            }

            pose.poseIsValid = true;
            pose.deviceIsConnected = true;
            pose.result = vr::TrackingResult_Running_OK;
            pose.shouldApplyHeadModel = false;
            pose.willDriftInYaw = false;
        }

        if (config.use_track_filter)
        {
            double filtered_position[3] = {
                pose.vecPosition[0],
                pose.vecPosition[1],
                pose.vecPosition[2]
            };
            vr::HmdQuaternion_t filtered_rotation = pose.qRotation;
            track_filter_.FilterPose(filtered_rotation, filtered_position, config);
            pose.qRotation = filtered_rotation;
            pose.vecPosition[0] = filtered_position[0];
            pose.vecPosition[1] = filtered_position[1];
            pose.vecPosition[2] = filtered_position[2];
            track_filter_was_enabled_ = true;
        }
        else if (track_filter_was_enabled_)
        {
            track_filter_.Reset();
            track_filter_was_enabled_ = false;
        }

        if (pose_sample_time > 0.0)
        {
            const double pose_publish_time_seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            pose.poseTimeOffset = pose_sample_time - pose_publish_time_seconds;
        }
        else
        {
            pose.poseTimeOffset = 0.0;
        }

        // Update the pose
        {
            std::lock_guard<std::mutex> lock(pose_mutex_);
            curr_pose_ = pose;
        }

        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(device_index_, pose, sizeof(vr::DriverPose_t));

        // Pose update cadence: 2x display frequency.
        const float target_frequency = (config.display_frequency * 2.0f > 1.0f)
            ? (config.display_frequency * 2.0f)
            : 1.0f;
        const auto target_period = std::chrono::duration<double>(1.0 / target_frequency);
        const auto elapsed = std::chrono::steady_clock::now() - loop_start;
        if (elapsed < target_period)
        {
            std::this_thread::sleep_for(target_period - elapsed);
        }
    }
}


//-----------------------------------------------------------------------------
// Purpose: Return current pose
//-----------------------------------------------------------------------------
vr::DriverPose_t MockControllerDeviceDriver::GetPose()
{
    std::lock_guard<std::mutex> lock(pose_mutex_);
    return curr_pose_;
}


//-----------------------------------------------------------------------------
// Purpose: Update HMD position, Depth, Convergence, and user binds
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::PollHotkeysThread() {
    struct {
        int shot = 0;
        int hmd = 0;
        int top = 0;
        int save = 0;
        int overlay = 0;
    } sleep;

    const int sleep_time = static_cast<int>(floor(1000.0 / stereo_display_component_->GetConfig().display_frequency));
    std::string overlay_msg;
    HWND overlay_hwnd = nullptr;

    InitGDIPlus();

    auto setOverlay = [&](const std::string& msg) {
        overlay_msg = msg;
        sleep.overlay = stereo_display_component_->GetConfig().sleep_count_max * 3;
    };
    auto fmt = [&](const std::string& label, float value, int precision = 3) {
        std::ostringstream ss;
        ss << label << std::fixed << std::setprecision(precision) << value;
        return ss.str();
    };
    auto fmtDepthConv = [&]() {
        std::ostringstream ss;
        ss << "Depth: " << std::fixed << std::setprecision(3) << stereo_display_component_->GetDepth()
            << " Conv: " << stereo_display_component_->GetConvergence();
        return ss.str();
    };

    setOverlay("VRto3D: " + stereo_version_number_);

    while (is_active_) {
        auto cfg = stereo_display_component_->GetConfig();

        if (!cfg.disable_hotkeys) {
            // Ctrl+F3 Decrease Depth
            if (isCtrlDown() && isDown(VK_F3)) {
                stereo_display_component_->AdjustDepth(-0.001f, true);
                if (isDown(VK_SHIFT)) stereo_display_component_->ResetProjection();
                setOverlay(fmtDepthConv());
            }
            // Ctrl+F4 Increase Depth
            else if (isCtrlDown() && isDown(VK_F4)) {
                stereo_display_component_->AdjustDepth(0.001f, true);
                if (isDown(VK_SHIFT)) stereo_display_component_->ResetProjection();
                setOverlay(fmtDepthConv());
            }
            // Ctrl+F5 Decrease Convergence
            else if (isCtrlDown() && isDown(VK_F5)) {
                stereo_display_component_->AdjustConvergence(0.005f, true);
                setOverlay(fmtDepthConv());
            }
            // Ctrl+F6 Increase Convergence
            else if (isCtrlDown() && isDown(VK_F6)) {
                stereo_display_component_->AdjustConvergence(-0.005f, true);
                setOverlay(fmtDepthConv());
            }
            // Ctrl+F7 Store settings into game profile
            if (isCtrlDown() && isDown(VK_F7) && sleep.save == 0) {
                if (!prev_name_.empty()) {
                    cfg.depth = stereo_display_component_->GetDepth();
                    cfg.convergence = stereo_display_component_->GetConvergence();
                    cfg.fov = stereo_display_component_->GetFoV();
                    JsonManager().SaveProfileToJson(prev_name_ + "_config.json", cfg);
                    BeepSuccess();
                    setOverlay("Saved " + prev_name_ + "_config.json profile");
                }
                else {
                    BeepFailure();
                    setOverlay("Failed to save profile");
                }
                sleep.save = cfg.sleep_count_max;
            }
            // Ctrl+F9 Save HMD Position & Yaw
            if (isCtrlDown() && isDown(VK_F9) && sleep.hmd == 0) {
                JsonManager().SaveHmdOffsets(cfg);
                BeepSuccess();
                setOverlay("Saved HMD Offsets");
                sleep.hmd = cfg.sleep_count_max;
            }
            else if (sleep.hmd > 0) {
                --sleep.hmd;
            }
            // Ctrl+F10 Reload settings from Game Profile or (+Shift) Default Profile
            else if (isCtrlDown() && isDown(VK_F10) && sleep.save == 0) {
                std::string path = "";
                if (isDown(VK_SHIFT)) {
                    path = DEF_CFG;
                    app_name_ = "";
                }
                else if (!prev_name_.empty()) {
                    path = prev_name_ + "_config.json";
                    app_name_ = prev_name_;
                }
                if (JsonManager().LoadProfileFromJson(path, cfg)) {
                    stereo_display_component_->LoadSettings(cfg);
                    SetAsync(cfg.async_enable);
                    LOG() << "Loaded " << path.c_str() << " profile";
                    BeepSuccess();
                    setOverlay("Loaded " + path + " profile");
                }
                else {
                    BeepFailure();
                    setOverlay("Failed to load profile");
                }
                sleep.save = cfg.sleep_count_max;
            }
            else if (sleep.save > 0) {
                --sleep.save;
            }
        }
        // Ctrl+F8 Toggle Always On Top
        if (isCtrlDown() && isDown(VK_F8) && sleep.top == 0) {
            is_on_top_ = !is_on_top_;
            man_on_top_ = is_on_top_.load();
            sleep.top = cfg.sleep_count_max;
        }
        else if (sleep.top > 0) {
            --sleep.top;
        }
        // Ctrl+F12 Take Screenshot
        if (isCtrlDown() && isDown(VK_F12) && sleep.shot == 0) {
            take_screenshot_ = true;
            sleep.shot = cfg.sleep_count_max;
        }
        else if (sleep.shot > 0) {
            --sleep.shot;
        }
        // Ctrl+- Decrease Sensitivity / Shift+Ctrl+- Decrease Filter Deadzone
        if (isCtrlDown() && isDown(VK_OEM_MINUS)) {
            const bool shift = isDown(VK_SHIFT);
            if (cfg.use_track_filter) {
                if (shift) {
                    stereo_display_component_->AdjustTrackFilterRotationDeadzone(-0.001f);
                    cfg = stereo_display_component_->GetConfig();
                    setOverlay(fmt("Track Rot DZ: ", cfg.trk_flt_rot_dz, 3));
                }
                else {
                    stereo_display_component_->AdjustTrackFilterRotation(-0.01f);
                    cfg = stereo_display_component_->GetConfig();
                    setOverlay(fmt("Track Rot Sens: ", cfg.trk_flt_rot_sens, 2));
                }
            }
            else {
                stereo_display_component_->AdjustSensitivity(-0.01f);
                cfg = stereo_display_component_->GetConfig();
                setOverlay(fmt("Ctrl Sensitivity: ", cfg.ctrl_sensitivity, 2));
            }
        }
        // Ctrl++ Increase Sensitivity / Shift+Ctrl++ Increase Filter Deadzone
        else if (isCtrlDown() && isDown(VK_OEM_PLUS)) {
            const bool shift = isDown(VK_SHIFT);
            if (cfg.use_track_filter) {
                if (shift) {
                    stereo_display_component_->AdjustTrackFilterRotationDeadzone(0.001f);
                    cfg = stereo_display_component_->GetConfig();
                    setOverlay(fmt("Track Rot DZ: ", cfg.trk_flt_rot_dz, 3));
                }
                else {
                    stereo_display_component_->AdjustTrackFilterRotation(0.01f);
                    cfg = stereo_display_component_->GetConfig();
                    setOverlay(fmt("Track Rot Sens: ", cfg.trk_flt_rot_sens, 2));
                }
            }
            else {
                stereo_display_component_->AdjustSensitivity(0.01f);
                cfg = stereo_display_component_->GetConfig();
                setOverlay(fmt("Ctrl Sensitivity: ", cfg.ctrl_sensitivity, 2));
            }
        }
        // Ctrl+[ Decrease Pitch Radius / Shift+Ctrl+[ Decrease Filter Position Deadzone
        if (isCtrlDown() && isDown(VK_OEM_4)) {
            const bool shift = isDown(VK_SHIFT);
            if (cfg.use_track_filter) {
                if (shift) {
                    stereo_display_component_->AdjustTrackFilterTranslationDeadzone(-0.001f);
                    cfg = stereo_display_component_->GetConfig();
                    setOverlay(fmt("Track Pos DZ: ", cfg.trk_flt_pos_dz, 3));
                }
                else {
                    stereo_display_component_->AdjustTrackFilterTranslation(-0.01f);
                    cfg = stereo_display_component_->GetConfig();
                    setOverlay(fmt("Track Pos Sens: ", cfg.trk_flt_pos_sens, 2));
                }
            }
            else {
                stereo_display_component_->AdjustRadius(-0.01f);
                cfg = stereo_display_component_->GetConfig();
                setOverlay(fmt("Pitch Radius: ", cfg.pitch_radius, 2));
            }
        }
        // Ctrl+] Increase Pitch Radius / Shift+Ctrl+] Increase Filter Position Deadzone
        else if (isCtrlDown() && isDown(VK_OEM_6)) {
            const bool shift = isDown(VK_SHIFT);
            if (cfg.use_track_filter) {
                if (shift) {
                    stereo_display_component_->AdjustTrackFilterTranslationDeadzone(0.001f);
                    cfg = stereo_display_component_->GetConfig();
                    setOverlay(fmt("Track Pos DZ: ", cfg.trk_flt_pos_dz, 3));
                }
                else {
                    stereo_display_component_->AdjustTrackFilterTranslation(0.01f);
                    cfg = stereo_display_component_->GetConfig();
                    setOverlay(fmt("Track Pos Sens: ", cfg.trk_flt_pos_sens, 2));
                }
            }
            else {
                stereo_display_component_->AdjustRadius(0.01f);
                cfg = stereo_display_component_->GetConfig();
                setOverlay(fmt("Pitch Radius: ", cfg.pitch_radius, 2));
            }
        }

        // Ctrl+; Decrease Filter Zoom Smoothing / Shift+Ctrl+; Decrease Filter Max Zoom
        if (isCtrlDown() && isDown(VK_OEM_1)) {
            const bool shift = isDown(VK_SHIFT);
            if (cfg.use_track_filter) {
                if (shift) {
                    stereo_display_component_->AdjustTrackFilterMaxZoom(-0.1f);
                    cfg = stereo_display_component_->GetConfig();
                    setOverlay(fmt("Track Max Zoom: ", cfg.trk_flt_max_zoom, 2));
                }
                else {
                    stereo_display_component_->AdjustTrackFilterZoomSmoothing(-0.05f);
                    cfg = stereo_display_component_->GetConfig();
                    setOverlay(fmt("Track Zoom Smooth: ", cfg.trk_flt_zoom_smooth, 2));
                }
            }
        }
        // Ctrl+' Increase Filter Zoom Smoothing / Shift+Ctrl+' Increase Filter Max Zoom
        else if (isCtrlDown() && isDown(VK_OEM_7)) {
            const bool shift = isDown(VK_SHIFT);
            if (cfg.use_track_filter) {
                if (shift) {
                    stereo_display_component_->AdjustTrackFilterMaxZoom(0.1f);
                    cfg = stereo_display_component_->GetConfig();
                    setOverlay(fmt("Track Max Zoom: ", cfg.trk_flt_max_zoom, 2));
                }
                else {
                    stereo_display_component_->AdjustTrackFilterZoomSmoothing(0.05f);
                    cfg = stereo_display_component_->GetConfig();
                    setOverlay(fmt("Track Zoom Smooth: ", cfg.trk_flt_zoom_smooth, 2));
                }
            }
        }

        // Check User binds
        auto hotkey_str = stereo_display_component_->CheckUserSettings();

        // Check for Position Adjustment
        auto pos_str = stereo_display_component_->CheckPositionInput();

        if (!hotkey_str.empty()) {
            setOverlay(hotkey_str);
        }
        else if (!pos_str.empty()) {
            setOverlay(pos_str);
        }

        // Check for new profile load
        if (app_updated_)
        {
            setOverlay("Loaded " + app_name_ + "_config.json profile");
            app_updated_ = false;
        }
        // Check for no profile
        else if (no_profile_)
        {
            setOverlay("No profile found for " + app_name_);
            no_profile_ = false;
        }

        // Draw Overlay if applicable
        if (!overlay_hwnd) {
            overlay_hwnd = FindWindow(NULL, L"Headset Window");
        }
        else if (sleep.overlay > 0) {
            DrawOverlayText(overlay_hwnd, overlay_msg, cfg.window_height);
            --sleep.overlay;
        }

        // Sleep for ~ 1 frame
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
    }
}


//-----------------------------------------------------------------------------
// Purpose: Expose focus/top atomics to the virtual-display presenter.
//
// The presenter owns the 3D output window and runs its own focus thread
// that observes these flags. The hotkey handler + UE3D auto-focus path
// continue to toggle them here.
//-----------------------------------------------------------------------------
vrto3d::FocusContext MockControllerDeviceDriver::GetFocusContext()
{
    vrto3d::FocusContext fc;
    fc.is_on_top   = &is_on_top_;
    fc.man_on_top  = &man_on_top_;
    fc.ue3d_on_top = &ue3d_on_top_;
    fc.app_pid     = &app_pid_;
    return fc;
}


//-----------------------------------------------------------------------------
// Purpose: Process UE3D/UEVR shared-memory monitor/depth requests
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::AutoDepthThread() {
    auto& rx = uevr::receiver();
    static float last_hint_ipd = -1.0f;

    while (is_active_) {
        const auto config = stereo_display_component_->GetConfig();

        if (!rx.is_connected()) rx.init();

        rx.update(
            stereo_display_component_->GetDepth(),
            stereo_display_component_->GetConvergence(),
            stereo_display_component_->GetFoV(),
            0.0f,   // fov_adj (unused in monitor mode)
            (config.output_mode == OutputMode::TaB) ? 0 : 1,
            !no_profile_.load()
        );

        const bool mon = rx.is_connected() && rx.get_monitor_mode();
        stereo_display_component_->SetMonitorMode(mon);

        if (mon)
        {
            if (config.auto_focus && !is_on_top_ && !ue3d_on_top_) {
                BeepSuccess();
                std::this_thread::sleep_for(std::chrono::seconds(2));
                is_on_top_ = true;
                man_on_top_ = true;
                ue3d_on_top_ = true;
            }

            // Depth commands from UEVR (Calibrate, VRto3D++/+/-/--)
            uint8_t depth_cmd = rx.get_depth_request();
            if (depth_cmd >= 2 && depth_cmd <= 6) {
                if (depth_cmd == 2) {  // Calibrate from world_scale
                    float ad = 0.0f, ac = 0.0f;
                    if (rx.calculate_auto_stereo(ad, ac)) {
                        auto cfg_cal = stereo_display_component_->GetConfig();
                        cfg_cal.depth = ad;
                        cfg_cal.convergence = ac;
                        stereo_display_component_->LoadSettings(cfg_cal);
                        LOG() << "UE3D Calibrate: d=" << ad << " c=" << ac;
                        app_updated_ = true;
                    }
                }
                else if (depth_cmd == 3) {  // VRto3D-: Decrease depth 20%
                    float new_d = stereo_display_component_->GetDepth() * 0.8f;
                    new_d = (std::max)(0.005f, new_d);
                    stereo_display_component_->AdjustDepth(new_d, false);
                    app_updated_ = true;
                }
                else if (depth_cmd == 4) {  // VRto3D+: Increase depth 20%
                    float new_d = stereo_display_component_->GetDepth() * 1.2f;
                    new_d = (std::min)(1.0f, new_d);
                    stereo_display_component_->AdjustDepth(new_d, false);
                    app_updated_ = true;
                }
                else if (depth_cmd == 5) {  // VRto3D--: Big decrease 40%
                    float new_d = stereo_display_component_->GetDepth() * 0.6f;
                    new_d = (std::max)(0.005f, new_d);
                    stereo_display_component_->AdjustDepth(new_d, false);
                    app_updated_ = true;
                }
                else if (depth_cmd == 6) {  // VRto3D++: Big increase 40%
                    float new_d = stereo_display_component_->GetDepth() * 1.4f;
                    new_d = (std::min)(1.0f, new_d);
                    stereo_display_component_->AdjustDepth(new_d, false);
                    app_updated_ = true;
                }
                rx.clear_depth_request();
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}


//-----------------------------------------------------------------------------
// Purpose: Load Game Specific Settings from Steam\config\vrto3d\app_name_config.json
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::LoadSettings(const std::string& app_name, uint32_t app_pid, vr::EVREventType status)
{
    if ((app_name != app_name_ || app_pid != app_pid_) && status == vr::VREvent_ProcessConnected)
    {
        app_name_ = app_name;
        prev_name_ = app_name;
        app_pid_ = app_pid;
        auto config = stereo_display_component_->GetConfig();

        // Attempt to read the JSON settings file
        if (JsonManager().LoadProfileFromJson(app_name + "_config.json", config))
        {
            stereo_display_component_->LoadSettings(config);
            LOG() << "Loaded " << app_name.c_str() << " profile";
            BeepSuccess();
            app_updated_ = true;
            if (config.auto_focus) {
                std::this_thread::sleep_for(std::chrono::seconds(8));
                is_on_top_ = true;
                man_on_top_ = true;
            }
        }
        else {
            BeepFailure();
            no_profile_ = true;
        }

        SetAsync(config.async_enable);
    }
    else if (status == vr::VREvent_ProcessDisconnected)
    {
        is_on_top_ = false;
        man_on_top_ = false;
        ue3d_on_top_ = false;
    }
}


//-----------------------------------------------------------------------------
// Purpose: Scan for App IDs and set Async Reprojection On/Off
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::SetAsync(bool enable)
{
    auto app_ids = AppIdMgr().GetSteamAppIDs();
    for (const std::string& app_id : app_ids) {
        vr::VRSettings()->SetBool(app_id.c_str(), vr::k_pch_SteamVR_DisableAsyncReprojection_Bool, !enable);
        LOG() << (enable ? "Enabled" : "Disabled") << " Async Reprojection for appkey: " << app_id.c_str();
    }
}


//-----------------------------------------------------------------------------
// Purpose: Stub for Standby mode
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::EnterStandby()
{
    LOG() << "HMD has been put into standby.";
}

//-----------------------------------------------------------------------------
// Purpose: Shutdown process
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::Deactivate()
{
    LOG() << "MockControllerDeviceDriver::Deactivate";
    if ( is_active_.exchange( false ) )
    {
        if (xinput_thread_.joinable()) {
            xinput_thread_.join();
        }
        if (pose_thread_.joinable()) {
            pose_thread_.join();
        }
        if (hotkey_thread_.joinable()) {
            hotkey_thread_.join();
        }
        if (depth_thread_.joinable()) {
            depth_thread_.join();
        }
        if (track_thread_.joinable()) {
            track_thread_.join();
        }
        if (renderer_) {
            renderer_->Shutdown();
            renderer_.reset();
        }
    }

    // unassign our controller index (we don't want to be calling vrserver anymore after Deactivate() has been called
    device_index_ = vr::k_unTrackedDeviceIndexInvalid;
    display_redirect_index_ = vr::k_unTrackedDeviceIndexInvalid;
}

MockControllerDeviceDriver::~MockControllerDeviceDriver() = default;


//-----------------------------------------------------------------------------
// DISPLAY DRIVER METHOD DEFINITIONS
//-----------------------------------------------------------------------------

StereoDisplayComponent::StereoDisplayComponent( const StereoDisplayDriverConfiguration &config )
    : config_( config ), depth_(config.depth), convergence_(config.convergence), fov_(config.fov)
{}


//-----------------------------------------------------------------------------
// Purpose: Initialize the Stereo Display Component
//-----------------------------------------------------------------------------
void StereoDisplayComponent::Init(uint32_t device_index) 
{
    device_index_ = device_index;
}


//-----------------------------------------------------------------------------
// Purpose: To inform vrcompositor if this display is considered an on-desktop display.
//-----------------------------------------------------------------------------
bool StereoDisplayComponent::IsDisplayOnDesktop() { return false; }

//-----------------------------------------------------------------------------
// Purpose: To as vrcompositor to search for this display.
//-----------------------------------------------------------------------------
bool StereoDisplayComponent::IsDisplayRealDisplay() { return false; }

//-----------------------------------------------------------------------------
// Purpose: To inform the rest of the vr system what the recommended target size should be
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight )
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);
    *pnWidth = config_.render_width;
    *pnHeight = config_.render_height;
}

//-----------------------------------------------------------------------------
// Purpose: Canonical SbS output for the compositor. Each eye occupies half
// the horizontal extent of the 2W x H backbuffer; the presenter repacks into
// TaB / interlaced / anaglyph / etc. downstream based on cfg.output_mode.
// eye_swap is applied in the presenter, not here.
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetEyeOutputViewport( vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);
    const uint32_t eye_w = static_cast<uint32_t>(config_.render_width);
    const uint32_t eye_h = static_cast<uint32_t>(config_.render_height);
    *pnWidth  = eye_w;
    *pnHeight = eye_h;
    *pnY      = 0;
    *pnX      = (eEye == vr::Eye_Left) ? 0u : eye_w;
}

//-----------------------------------------------------------------------------
// Purpose: Utilize the desired FoV, Aspect Ratio, and Convergence settings
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetProjectionRaw( vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom )
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);

    // Convert horizontal FOV from degrees to radians
    float horFovRadians = tan((GetFoV() * (M_PI / 180.0f)) / 2);

    // Calculate vertical FOV in radians
    float verFovRadians = horFovRadians / config_.aspect_ratio;

    // UE3D Monitor Mode: symmetric frustum (UEVR owns convergence via [2][0])
    if (monitor_mode_.load())
    {
        *pfTop = -verFovRadians;
        *pfBottom = verFovRadians;
        *pfLeft = -horFovRadians;
        *pfRight = horFovRadians;
        return;
    }

    // IPD-based horizontal offset
    float sep = GetDepth();
    float conv = GetConvergence();
    float eyeOffset = (eEye == vr::Eye_Left) ? sep * 0.5f / conv : -sep * 0.5f / conv;

    // Set frustum bounds
    *pfTop = -verFovRadians;
    *pfBottom = verFovRadians;
    *pfLeft = -horFovRadians + eyeOffset;
    *pfRight = horFovRadians + eyeOffset;
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
{ return false; }

//-----------------------------------------------------------------------------
// Purpose: To inform vrcompositor what the window bounds for this virtual HMD are.
//-----------------------------------------------------------------------------
void StereoDisplayComponent::GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);
    // Backbuffer is canonical 2W x H SbS. Presenter repacks downstream.
    *pnX = 0;
    *pnY = 0;
    *pnWidth  = static_cast<uint32_t>(config_.render_width)  * 2u;
    *pnHeight = static_cast<uint32_t>(config_.render_height);
}

//-----------------------------------------------------------------------------
// Purpose: To provide access to settings
//-----------------------------------------------------------------------------
StereoDisplayDriverConfiguration StereoDisplayComponent::GetConfig()
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);
    return config_;
}


//-----------------------------------------------------------------------------
// Purpose: To update the Depth value
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustDepth(float new_depth, bool is_delta)
{
    float cur_depth = GetDepth();
    if (is_delta) {
        new_depth += cur_depth;
        new_depth = (new_depth < 0) ? 0 : new_depth;
    }
    while (!depth_.compare_exchange_weak(cur_depth, new_depth, std::memory_order_relaxed));
    vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(device_index_);
    vr::VRProperties()->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, new_depth);
}


//-----------------------------------------------------------------------------
// Purpose: To update the Convergence value
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustConvergence(float new_conv, bool is_delta)
{
    float cur_conv = GetConvergence();
    if (is_delta) {
        new_conv += cur_conv;
        new_conv = (new_conv < 0.001) ? 0.001 : new_conv;
    }
    if (NearlyEqual(cur_conv, new_conv))
        return;
    while (!convergence_.compare_exchange_weak(cur_conv, new_conv, std::memory_order_relaxed));
    ResetProjection();
}


//-----------------------------------------------------------------------------
// Purpose: To update the FoV value
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustFoV(float new_fov)
{
    float cur_fov = GetFoV();
    if (NearlyEqual(cur_fov, new_fov))
        return;
    while (!fov_.compare_exchange_weak(cur_fov, new_fov, std::memory_order_relaxed));
    ResetProjection();
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
// Purpose: Get FoV value
//-----------------------------------------------------------------------------
float StereoDisplayComponent::GetFoV()
{
    return fov_.load(std::memory_order_relaxed);
}


//-----------------------------------------------------------------------------
// Purpose: Check User Settings and act on them
//-----------------------------------------------------------------------------
std::string StereoDisplayComponent::CheckUserSettings()
{
    static int sleep_ctrl = 0;
    static int sleep_rest = 0;
    std::string overlay_msg = "";
    
    DWORD xstate;
    bool got_xinput = GetXInputButtonState(xstate);

    auto config = GetConfig();

    // Toggle Pitch and Yaw control
    if ((config.ctrl_xinput && got_xinput &&
        ((xstate & config.ctrl_toggle_key) == config.ctrl_toggle_key))
        || (!config.ctrl_xinput && isDown(config.ctrl_toggle_key)))
    {
        if (config.ctrl_type == HOLD && !config.ctrl_held)
        {
            config.ctrl_held = true;
            config.pitch_enable = false;
            config.yaw_enable = false;
        }
        else if ((config.ctrl_type == TOGGLE || config.ctrl_type == SWITCH) && sleep_ctrl < 1)
        {
            sleep_ctrl = config.sleep_count_max;
            if (config.pitch_set) {
                config.pitch_enable = !config.pitch_enable;
            }
            if (config.yaw_set) {
                config.yaw_enable = !config.yaw_enable;
            }
        }
    }
    else if (config.ctrl_type == HOLD && config.ctrl_held)
    {
        config.ctrl_held = false;
        config.pitch_enable = config.pitch_set;
        config.yaw_enable = config.yaw_set;
    }
    if (sleep_ctrl > 0) {
        sleep_ctrl--;
    }

    // Reset HMD position
    if (((config.reset_xinput && got_xinput &&
        ((xstate & config.pose_reset_key) == config.pose_reset_key))
        || (!config.reset_xinput && isDown(config.pose_reset_key)))
        && sleep_rest == 0)
    {
        sleep_rest = config.sleep_count_max;
        if (!config.pose_reset) {
            config.pose_reset = true;
        }
    }
    else if (sleep_rest > 0) {
        sleep_rest--;
    }

    DepthConvBackend b{
      +[](void* ctx)->float { return static_cast<StereoDisplayComponent*>(ctx)->GetDepth(); },
      +[](void* ctx)->float { return static_cast<StereoDisplayComponent*>(ctx)->GetConvergence(); },
      +[](void* ctx, float v) { static_cast<StereoDisplayComponent*>(ctx)->AdjustDepth(v, false); },
      +[](void* ctx, float v) { static_cast<StereoDisplayComponent*>(ctx)->AdjustConvergence(v, false); },
      +[](void* ctx)->float { return static_cast<StereoDisplayComponent*>(ctx)->GetFoV(); },
      +[](void* ctx, float v) { static_cast<StereoDisplayComponent*>(ctx)->AdjustFoV(v); },
      nullptr,
      this
    };

    auto msg = ApplyUserSettingsHotkeys(config, got_xinput, xstate, b);
    if (!msg.empty()) overlay_msg = std::move(msg);

    // Update the config
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_ = config;

    return overlay_msg;
}


//-----------------------------------------------------------------------------
// Purpose: Move HMD origin position with keyboard input
//-----------------------------------------------------------------------------
std::string StereoDisplayComponent::CheckPositionInput() {
    if (!isCtrlDown())
        return "";

    const float step = 0.01f;
    auto config = GetConfig();

    if (isDown(VK_HOME)) {
            config.hmd_y -= step;       // Forward
    }
    else if (isDown(VK_END)) {
            config.hmd_y += step;       // Backward
    }
    else if (isDown(VK_DELETE)) {
        config.hmd_x -= step;       // Left
    }
    else if (isDown(VK_NEXT)) {
        if (isDown(VK_SHIFT)) {
            config.hmd_height -= step;  // Down
        }
        else {
            config.hmd_x += step;       // Right
        }
    }
    else if (isDown(VK_PRIOR)) {
        if (isDown(VK_SHIFT)) {
            config.hmd_height += step;  // Up
        }
        else {
            config.hmd_yaw -= step * 10;     // Yaw CW
        }
    }
    else if (isDown(VK_INSERT)) {
        config.hmd_yaw += step * 10;     // Yaw CCW
    }
    else {
        return "";
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "Pos X:" << config.hmd_x
        << " Y:" << config.hmd_y
        << " Z:" << config.hmd_height
        << " Yaw:" << config.hmd_yaw;

    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_ = config;

    return oss.str();
}


//-----------------------------------------------------------------------------
// Purpose: Adjust XInput Right Stick sensitivity
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustSensitivity(float delta)
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    if (config_.pitch_enable || config_.yaw_enable)
    {
        config_.ctrl_sensitivity += delta;
        if (config_.ctrl_sensitivity < 0.0f)
            config_.ctrl_sensitivity = 0.0f;
    }
}


//-----------------------------------------------------------------------------
// Purpose: Adjust XInput Pitch Radius
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustRadius(float delta)
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    if (config_.pitch_enable)
    {
        config_.pitch_radius += delta;
        if (config_.pitch_radius < 0.0f)
            config_.pitch_radius = 0.0f;
    }
}


//-----------------------------------------------------------------------------
// Purpose: Adjust Track Filter rotation sensitivity
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustTrackFilterRotation(float delta)
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_.trk_flt_rot_sens += delta;
    config_.trk_flt_rot_sens = std::clamp(
        config_.trk_flt_rot_sens,
        0.01f,
        5.0f);
}


//-----------------------------------------------------------------------------
// Purpose: Adjust Track Filter translation sensitivity
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustTrackFilterTranslation(float delta)
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_.trk_flt_pos_sens += delta;
    config_.trk_flt_pos_sens = std::clamp(
        config_.trk_flt_pos_sens,
        0.01f,
        3.0f);
}


//-----------------------------------------------------------------------------
// Purpose: Adjust Track Filter rotation deadzone
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustTrackFilterRotationDeadzone(float delta)
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_.trk_flt_rot_dz += delta;
    config_.trk_flt_rot_dz = std::clamp(
        config_.trk_flt_rot_dz,
        0.0f,
        0.4f);
}


//-----------------------------------------------------------------------------
// Purpose: Adjust Track Filter translation deadzone
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustTrackFilterTranslationDeadzone(float delta)
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_.trk_flt_pos_dz += delta;
    config_.trk_flt_pos_dz = std::clamp(
        config_.trk_flt_pos_dz,
        0.0f,
        2.0f);
}


//-----------------------------------------------------------------------------
// Purpose: Adjust Track Filter zoom smoothing
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustTrackFilterZoomSmoothing(float delta)
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_.trk_flt_zoom_smooth += delta;
    config_.trk_flt_zoom_smooth = std::clamp(
        config_.trk_flt_zoom_smooth,
        0.0f,
        20.0f);
}


//-----------------------------------------------------------------------------
// Purpose: Adjust Track Filter max zoom range
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustTrackFilterMaxZoom(float delta)
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_.trk_flt_max_zoom += delta;
    config_.trk_flt_max_zoom = std::clamp(
        config_.trk_flt_max_zoom,
        0.1f,
        60.0f);
}


//-----------------------------------------------------------------------------
// Purpose: Toggle Reset off
//-----------------------------------------------------------------------------
void StereoDisplayComponent::SetReset()
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_.pose_reset = false;
}


//-----------------------------------------------------------------------------
// Purpose: Load Game Specific Settings from Steam\config\vrto3d\app_name_config.json
//-----------------------------------------------------------------------------
void StereoDisplayComponent::LoadSettings(StereoDisplayDriverConfiguration& config)
{
    // Apply loaded settings
    AdjustDepth(config.depth, false);
    AdjustConvergence(config.convergence, false);
    AdjustFoV(config.fov);

    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_ = config;
    lock.unlock();
    ResetProjection();
}


//-----------------------------------------------------------------------------
// Purpose: Reset per-eye FoV calculation
//-----------------------------------------------------------------------------
void StereoDisplayComponent::ResetProjection()
{
    // Regenerate the Projection
    vr::HmdRect2_t eyeLeft, eyeRight;
    GetProjectionRaw(vr::Eye_Left, &eyeLeft.vTopLeft.v[0], &eyeLeft.vBottomRight.v[0], &eyeLeft.vTopLeft.v[1], &eyeLeft.vBottomRight.v[1]);
    GetProjectionRaw(vr::Eye_Right, &eyeRight.vTopLeft.v[0], &eyeRight.vBottomRight.v[0], &eyeRight.vTopLeft.v[1], &eyeRight.vBottomRight.v[1]);
    vr::VREvent_Data_t temp;
    vr::VRServerDriverHost()->SetDisplayProjectionRaw(device_index_, eyeLeft, eyeRight);
    vr::VRServerDriverHost()->VendorSpecificEvent(device_index_, vr::VREvent_LensDistortionChanged, temp, 0.0f);
}


//-----------------------------------------------------------------------------
// Purpose: Enable/Disable Monitor Mode
//-----------------------------------------------------------------------------
void StereoDisplayComponent::SetMonitorMode(bool enabled) {
    bool was_monitor = monitor_mode_.load();
    monitor_mode_.store(enabled);
    if (enabled && !was_monitor) {
        ResetProjection();
    }
}


//-----------------------------------------------------------------------------
// Purpose: Get Monitor Mode Status
//-----------------------------------------------------------------------------
bool StereoDisplayComponent::IsMonitorMode()
{
    return monitor_mode_.load();
}
