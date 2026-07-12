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
#ifdef _WIN32
#include "direct_mode_component.h"
#include "dx11_renderer.h"
#include "vrto3dlib/key_codes.h"
#else
#include "vk/direct_mode_component_vk.h"
#include "vk/vk_renderer.h"
#include "vrto3dlib/key_codes.h"
#endif
#include "platform.h"
#include "vrto3dlib/json_manager.h"
#include "osd/osd_renderer.h"
#include "osd/osd_menu.h"
#include "vr_recenter.h"

#ifdef _WIN32
#include <shellapi.h>
#include <shlobj.h>
#include <urlmon.h>
#pragma comment(lib, "urlmon.lib")
#endif
#include "vrto3dlib/app_id_mgr.h"
#ifdef _WIN32
#include "vrto3dlib/win32_helper.hpp"
#else
#include "vrto3dlib/linux_helper.hpp"
#endif
#include "vrmath.h"

#include <string>
#include <sstream>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment (lib, "WSock32.Lib")
#include <windows.h>
#include <xinput.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <filesystem>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>
#include "vrto3dlib/input_state.h"
// Socket-compat shims so the shared OpenTrack UDP thread body compiles
// unchanged against BSD sockets.
using SOCKET = int;
using DWORD = uint32_t;
static constexpr int INVALID_SOCKET = -1;
static constexpr int SOCKET_ERROR = -1;
static inline int closesocket(int s) { return close(s); }
// XINPUT_STATE-shaped snapshot filled from the evdev gamepad backend.
struct PortableGamepad {
    uint16_t wButtons = 0;
    uint8_t  bLeftTrigger = 0, bRightTrigger = 0;
    int16_t  sThumbLX = 0, sThumbLY = 0, sThumbRX = 0, sThumbRY = 0;
};
struct _XINPUT_STATE {
    uint32_t dwPacketNumber = 0;
    PortableGamepad Gamepad;
};
static inline uint32_t XInputGetStateShim(uint32_t /*idx*/, _XINPUT_STATE* out)
{
    const auto pad = vrto3d::input::GetGamepadState();
    out->Gamepad.wButtons = pad.wButtons;
    out->Gamepad.bLeftTrigger = pad.bLeftTrigger;
    out->Gamepad.bRightTrigger = pad.bRightTrigger;
    out->Gamepad.sThumbLX = pad.sThumbLX;
    out->Gamepad.sThumbLY = pad.sThumbLY;
    out->Gamepad.sThumbRX = pad.sThumbRX;
    out->Gamepad.sThumbRY = pad.sThumbRY;
    return pad.connected ? 0u : 1167u;  // ERROR_DEVICE_NOT_CONNECTED
}
#define _XInputGetState XInputGetStateShim
#ifndef ERROR_SUCCESS
#define ERROR_SUCCESS 0u
#endif
static inline void SwitchToXinpuGetStateEx() {}
#ifndef ZeroMemory
#define ZeroMemory(p, s) memset((p), 0, (s))
#endif
#endif


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
#ifdef _WIN32
    const bool monitor_bounds_applied = ApplyDisplaySelectionToWindowConfig(display_configuration);
#else
    // Presenters fullscreen onto the selected output themselves on Linux —
    // window_x/y bounds are not used for placement.
    const bool monitor_bounds_applied = false;
#endif
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
    // Direct mode registers the HMD once; no sibling DisplayRedirect object
    // needs activating.
    is_active_.store(true);
    device_index_ = unObjectId;
    // Start NOT on top (both platforms): the overlay lowers to reveal the
    // desktop/game until an app connects to SteamVR, at which point the
    // auto-focus path raises it (mirrored on Linux in VkRenderer's focus loop).
    is_on_top_ = false;
    man_on_top_ = false;
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
        // JSON-supplied display_frequency overrides auto-detection. 0 (the
        // default) means "ask the monitor".
        if (cfg.display_frequency <= 0.0f) {
            if (platform::ResolveTargetMonitors(cfg.display_index, false, primary, secondary)) {
                cfg.display_frequency = platform::QueryRefreshHz(primary, 60.0f);
            } else {
                cfg.display_frequency = 60.0f;
            }
        } else {
            // Still resolve the monitor so the log line reports the target.
            platform::ResolveTargetMonitors(cfg.display_index, false, primary, secondary);
        }

        // Frame-sequential modes (NVIDIA 3D Vision, WibbleWobble lightfield)
        // alternate L/R eyes at the panel's refresh rate, so the actual
        // stereo-pair rate is panel_rate / 2. Producing SBS pairs faster than
        // that just wastes GPU bandwidth (every other pair gets dropped) and
        // adds latency through the consumer's frame queue.
        const bool frame_sequential =
            cfg.output_mode == OutputMode::NvidiaDX9
         || cfg.output_mode == OutputMode::WibbleWobble;
        if (frame_sequential && cfg.display_frequency > 1.0f) {
            const float panel_hz = cfg.display_frequency;
            cfg.display_frequency = panel_hz * 0.5f;
            LOG() << "Display: frame-sequential mode — halving reported "
                     "display freq to compositor (" << panel_hz << "Hz panel -> "
                  << cfg.display_frequency << "Hz stereo-pair rate)";
        }

        cfg.display_latency   = (cfg.display_frequency > 1.0f)
            ? (0.5f / cfg.display_frequency) : 0.011f;
        cfg.sleep_count_max   = (int)(floor(1600.0 / (1000.0 / cfg.display_frequency)));
        stereo_display_component_->LoadSettings(cfg);
        auto_focus_.store(cfg.auto_focus);
        hide_cursor_.store(cfg.hide_cursor);
        lock_cursor_.store(cfg.lock_cursor);
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
    vrp->SetBoolProperty( container, vr::Prop_HasDriverDirectModeComponent_Bool, true);

    // Direct-mode virtual-display integration: advertise our adapter LUID so
    // the compositor composites on the same GPU the renderer will use, and
    // signal that we fire VsyncEvent ourselves from the renderer thread.
    {
#ifdef _WIN32
        LUID luid = platform::PrimaryAdapterLuid();
        uint64_t luid_u64 = (static_cast<uint64_t>(luid.HighPart) << 32) | luid.LowPart;
        vrp->SetUint64Property(container, vr::Prop_GraphicsAdapterLuid_Uint64, luid_u64);
#endif
        // No LUID handshake on Linux — the compositor and driver agree on the
        // GPU implicitly.
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
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
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
#ifdef _WIN32
            const std::string command = "cmd.exe /C " + launch_script;
#else
            const std::string command = launch_script;  // /bin/sh via system()
#endif
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
    monitor_thread_ = std::thread(&MockControllerDeviceDriver::MonitorModeThread, this);
    // Always start OpenTrack listener; the use_open_track flag is checked at
    // consumption time (PoseUpdateThread / Stereo component) so it can be
    // toggled live from the OSD without restarting the driver.
    open_track_att_ = HmdQuaternion_Identity;
    open_track_pos_ = { 0.0, 0.0, 0.0 };
    track_thread_ = std::thread(&MockControllerDeviceDriver::OpenTrackThread, this);
    cursor_thread_ = std::thread(&MockControllerDeviceDriver::CursorControlThread, this);

#ifdef _WIN32
    HANDLE thread_handle = pose_thread_.native_handle();

    // Set the thread priority
    if (!SetThreadPriority(thread_handle, THREAD_PRIORITY_HIGHEST)) {
        // Handle error if setting priority fails
        LOG() << "Failed to set thread priority: " << GetLastError();
    }
#else
    {
        sched_param sch{};
        sch.sched_priority = 0;
        pthread_setschedparam(pose_thread_.native_handle(), SCHED_OTHER, &sch);
        // Global input needs /dev/input access (user in the `input` group).
        if (!vrto3d::input::Start()) {
            LOG() << "evdev input unavailable — hotkeys/gamepad dead. "
                     "Add your user to the `input` group: sudo usermod -aG input $USER";
        }
    }
#endif

    // Direct mode: build the platform renderer + selected presenter, then
    // stand up the IVRDriverDirectModeComponent that hands per-eye game
    // textures through to the renderer.
    {
        renderer_ = std::make_unique<StereoRenderer>();
#ifdef _WIN32
        LUID luid = platform::PrimaryAdapterLuid();
        const bool renderer_ok =
            renderer_->Init(luid, stereo_display_component_->GetConfig(), GetFocusContext());
#else
        const bool renderer_ok =
            renderer_->Init(stereo_display_component_->GetConfig(), GetFocusContext());
#endif
        if (!renderer_ok) {
            LOG() << "Renderer Init failed; direct-mode path will be inactive";
            renderer_.reset();
        } else {
            direct_mode_component_ = std::make_unique<StereoDirectMode>(renderer_.get());
            // Wire OSD callbacks. The OsdRenderer is lazy-initialized on the
            // window thread when the first frame arrives.
            vrto3d::osd::MenuCallbacks cb;
            cb.save_game_profile = [this](std::string toast) {
                if (prev_name_.empty()) return;
                auto cfg = stereo_display_component_->GetConfig();
                JsonManager().SaveProfileToJson(prev_name_ + "_config.json", cfg);
                if (renderer_ && renderer_->Osd()) renderer_->Osd()->SetText(toast);
            };
            cb.save_default_profile = [this](std::string toast) {
                auto cfg = stereo_display_component_->GetConfig();
                // SaveFullConfigToJson writes every key (display_index,
                // output_mode, render dims, OpenTrack, track filter, LeiaSR,
                // launch_script, etc.) — required so System-tab edits persist.
                JsonManager().SaveFullConfigToJson(DEF_CFG, cfg);
                if (renderer_ && renderer_->Osd()) renderer_->Osd()->SetText(toast);
            };
            cb.reload_game_profile = [this](std::string toast) {
                if (prev_name_.empty()) return;
                auto cfg = stereo_display_component_->GetConfig();
                if (JsonManager().LoadProfileFromJson(prev_name_ + "_config.json", cfg)) {
                    stereo_display_component_->LoadSettings(cfg);
                    SetAsync(cfg.async_enable);
                    auto_focus_.store(cfg.auto_focus);
                    hide_cursor_.store(cfg.hide_cursor);
                    lock_cursor_.store(cfg.lock_cursor);
                    app_name_ = prev_name_;
                    if (renderer_ && renderer_->Osd()) {
                        renderer_->Osd()->SetAppName(app_name_);
                        renderer_->Osd()->SetText(toast);
                    }
                }
            };
            cb.reload_default_profile = [this](std::string toast) {
                auto cfg = stereo_display_component_->GetConfig();
                // LoadProfileFromJson only reads per-profile fields; for the
                // default config we also need to refresh global driver fields
                // (display_index, output_mode, render dims, hmd_x/y/yaw,
                // OpenTrack, track filter, LeiaSR sens, async_enable, etc).
                // LoadParamsFromJson reads exactly that superset.
                JsonManager().LoadParamsFromJson(cfg);
                if (JsonManager().LoadProfileFromJson(DEF_CFG, cfg)) {
                    stereo_display_component_->LoadSettings(cfg);
                    SetAsync(cfg.async_enable);
                    auto_focus_.store(cfg.auto_focus);
                    hide_cursor_.store(cfg.hide_cursor);
                    lock_cursor_.store(cfg.lock_cursor);
                    app_name_ = "";
                    if (renderer_ && renderer_->Osd()) {
                        renderer_->Osd()->SetAppName(app_name_);
                        renderer_->Osd()->SetText(toast);
                    }
                }
            };
            cb.reset_defaults = [this](std::string toast) {
                // Factory reset: start from a fresh (in-code default) config,
                // but keep the display/output hardware fields from the live
                // config so a reset restores the stereo/shader/tracking
                // tunables without scrambling the user's monitor selection,
                // render dimensions, or refresh — those are the
                // "Requires Restart" fields and are hardware-specific.
                auto cur = stereo_display_component_->GetConfig();
                StereoDisplayDriverConfiguration def{};
                def.display_index     = cur.display_index;
                def.output_mode       = cur.output_mode;
                def.window_x          = cur.window_x;
                def.window_y          = cur.window_y;
                def.window_width      = cur.window_width;
                def.window_height     = cur.window_height;
                def.render_width      = cur.render_width;
                def.render_height     = cur.render_height;
                def.aspect_ratio      = cur.aspect_ratio;
                def.display_frequency = cur.display_frequency;
                def.display_latency   = cur.display_latency;
                def.sleep_count_max   = cur.sleep_count_max;
                stereo_display_component_->LoadSettings(def);
                SetAsync(def.async_enable);
                auto_focus_.store(def.auto_focus);
                hide_cursor_.store(def.hide_cursor);
                lock_cursor_.store(def.lock_cursor);
                if (renderer_ && renderer_->Osd()) renderer_->Osd()->SetText(toast);
            };
            cb.reset_projection = [this]() {
                stereo_display_component_->ResetProjection();
            };
            cb.get_auto_depth_enabled = [this]() {
                return stereo_display_component_->IsAutoDepthEnabled();
            };
            cb.set_auto_depth_enabled = [this](bool on) {
                stereo_display_component_->SetAutoDepthEnabled(on);
            };
            cb.get_auto_depth_target = [this]() {
                return stereo_display_component_->GetAutoDepthTargetDisparity();
            };
            cb.set_auto_depth_target = [this](float v) {
                stereo_display_component_->SetAutoDepthTargetDisparity(v);
            };
            cb.get_auto_depth_smoothing = [this]() {
                return stereo_display_component_->GetAutoDepthSmoothing();
            };
            cb.set_auto_depth_smoothing = [this](float v) {
                stereo_display_component_->SetAutoDepthSmoothing(v);
            };
            cb.get_auto_depth_logging = [this]() {
                return stereo_display_component_->IsAutoDepthLoggingEnabled();
            };
            cb.set_auto_depth_logging = [this](bool on) {
                stereo_display_component_->SetAutoDepthLoggingEnabled(on);
            };
            cb.calibrate_leiasr_head = [this]() {
#ifdef _WIN32
                if (renderer_ && renderer_->Presenter()) {
                    renderer_->Presenter()->RequestCalibrate();
                    if (renderer_->Osd())
                        renderer_->Osd()->SetText("LeiaSR head pose calibrated");
                }
#endif  // LeiaSR is a Windows-only presenter
            };
            cb.recenter_pose = [this]() {
                // Raise the flag; XInputUpdateThread picks it up on its next
                // 8ms tick and ConsumePoseReset zeros pitch/yaw + OT state
                // and dispatches the deferred OpenVR ResetZeroPose.
                stereo_display_component_->RequestPoseReset();
                if (renderer_ && renderer_->Osd())
                    renderer_->Osd()->SetText("Recentered");
            };
            cb.toggle_always_on_top = [this]() {
                is_on_top_  = !is_on_top_;
                man_on_top_ = is_on_top_.load();
            };
            cb.always_on_top = [this]() { return man_on_top_.load(); };
            cb.request_game_focus = [this]() {
                uint32_t pid = app_pid_.load();
                LOG() << "request_game_focus fired pid=" << pid;
                if (pid == 0 || !IsProcessRunning(pid)) return;
#ifndef _WIN32
                // No cross-client focus stealing on Wayland; X11 raise lives
                // in the presenter's topmost handling.
                return;
#else
                std::thread([this, pid]() {
                    // Let any held hotkey modifiers (Ctrl+Home was likely
                    // just used to close the menu) settle before we try
                    // to take foreground — the ALT-key trick inside
                    // ForceFocus misbehaves when Ctrl is still held.
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    if (!man_on_top_.load()) return;
                    HWND game_hwnd = GetHWNDFromPID(pid);
                    if (!game_hwnd) return;
                    // Skip if the game already has foreground — every
                    // ForceFocus call briefly raises the game window over our
                    // topmost VR window before the focus loop re-asserts it,
                    // which the user sees as a flicker.
                    if (GetForegroundWindow() == game_hwnd) return;
                    ForceFocus(game_hwnd,
                               GetCurrentThreadId(),
                               GetWindowThreadProcessId(game_hwnd, nullptr));
                    LOG() << "request_game_focus fg_match="
                          << (GetForegroundWindow() == game_hwnd);
                }).detach();
#endif
            };
#ifdef _WIN32
            cb.open_config_folder = [this]() {
                std::string steam = GetSteamInstallPath();
                if (steam.empty()) return;
                std::string path = steam + "\\config\\vrto3d";
                ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            };
            cb.open_screenshot_folder = [this]() {
                std::string steam = GetSteamInstallPath();
                if (steam.empty()) return;
                std::string path = steam + "\\steamapps\\common\\SteamVR\\screenshots";
                SHCreateDirectoryExA(nullptr, path.c_str(), nullptr);
                ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            };
#else
            cb.open_config_folder = [this]() {
                std::string steam = GetSteamInstallPath();
                if (steam.empty()) return;
                std::string cmd = "xdg-open '" + steam + "/config/vrto3d' >/dev/null 2>&1 &";
                [[maybe_unused]] int rc = std::system(cmd.c_str());
            };
            cb.open_screenshot_folder = [this]() {
                std::string steam = GetSteamInstallPath();
                if (steam.empty()) return;
                std::string path = steam + "/steamapps/common/SteamVR/screenshots";
                std::error_code ec;
                std::filesystem::create_directories(path, ec);
                std::string cmd = "xdg-open '" + path + "' >/dev/null 2>&1 &";
                [[maybe_unused]] int rc = std::system(cmd.c_str());
            };
#endif
            cb.set_async = [this](bool on) {
                SetAsync(on);
            };
            cb.set_auto_focus = [this](bool on) {
                auto_focus_.store(on);
                // Disabling auto focus also drops any current always-on-top
                // hold so the user's choice takes effect immediately rather
                // than only blocking future auto-focus actions.
                if (!on) {
                    is_on_top_  = false;
                    man_on_top_ = false;
                }
            };
            cb.set_hide_cursor = [this](bool on) { hide_cursor_.store(on); };
            cb.set_lock_cursor = [this](bool on) { lock_cursor_.store(on); };
            cb.download_latest_profiles = [this]() {
                // Re-entrancy guard — ignore clicks while a previous download
                // is still in flight.
                static std::atomic<bool> in_flight{false};
                bool expected = false;
                if (!in_flight.compare_exchange_strong(expected, true)) {
                    if (renderer_ && renderer_->Osd())
                        renderer_->Osd()->SetText("Profile download already running");
                    return;
                }
                std::thread([this]{
                    auto toast = [this](const std::string& msg,
                                        std::chrono::milliseconds ttl =
                                          std::chrono::milliseconds(3000)) {
                        if (renderer_ && renderer_->Osd())
                            renderer_->Osd()->SetText(msg, ttl);
                    };
                    static std::atomic<bool>& done = in_flight;
                    struct ResetOnExit { std::atomic<bool>& f; ~ResetOnExit(){ f.store(false); } } _r{done};

                    std::string steam = GetSteamInstallPath();
                    if (steam.empty()) {
                        toast("Profile download failed: Steam path not found");
                        return;
                    }
#ifndef _WIN32
                    const std::string folder = steam + "/config/vrto3d";
                    const std::string zip    = folder + "/vrto3d_profiles.zip";
                    toast("Downloading latest profiles…", std::chrono::milliseconds(60000));
                    std::error_code ec;
                    std::filesystem::create_directories(folder, ec);
                    const std::string dl =
                        "curl -fsSL -o '" + zip + "' "
                        "https://github.com/oneup03/VRto3D/releases/download/latest/vrto3d_profiles.zip";
                    if (std::system(dl.c_str()) != 0) {
                        toast("Profile download failed (curl)");
                        return;
                    }
                    toast("Extracting profiles", std::chrono::milliseconds(30000));
                    const std::string ex = "bsdtar -xf '" + zip + "' -C '" + folder + "'";
                    const int rc = std::system(ex.c_str());
                    ::remove(zip.c_str());
                    if (rc != 0) {
                        toast("Extract failed (bsdtar)");
                    } else {
                        toast("Profiles installed to config/vrto3d");
                    }
#else
                    const std::string folder = steam + "\\config\\vrto3d";
                    const std::string zip    = folder + "\\vrto3d_profiles.zip";
                    const wchar_t* url =
                        L"https://github.com/oneup03/VRto3D/releases/download/latest/vrto3d_profiles.zip";

                    toast("Downloading latest profiles…", std::chrono::milliseconds(60000));

                    // Make sure the destination folder exists.
                    CreateDirectoryA(folder.c_str(), nullptr);

                    std::wstring wzip(zip.begin(), zip.end());
                    HRESULT hr = URLDownloadToFileW(nullptr, url, wzip.c_str(), 0, nullptr);
                    if (FAILED(hr)) {
                        char buf[160];
                        std::snprintf(buf, sizeof(buf),
                                      "Profile download failed (hr=0x%08lX)",
                                      static_cast<unsigned long>(hr));
                        toast(buf);
                        return;
                    }

                    toast("Extracting profiles", std::chrono::milliseconds(30000));

                    // Use PowerShell's Expand-Archive to unpack — no extra
                    // dependency, available on every supported Windows.
                    std::string cmd =
                        "powershell.exe -NoProfile -ExecutionPolicy Bypass "
                        "-Command \"Expand-Archive -LiteralPath '" + zip +
                        "' -DestinationPath '" + folder + "' -Force\"";

                    STARTUPINFOA si{};
                    si.cb = sizeof(si);
                    si.dwFlags = STARTF_USESHOWWINDOW;
                    si.wShowWindow = SW_HIDE;
                    PROCESS_INFORMATION pi{};
                    std::vector<char> cmdline(cmd.begin(), cmd.end());
                    cmdline.push_back('\0');
                    BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr,
                                             FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                             &si, &pi);
                    DWORD exit_code = 1;
                    if (ok) {
                        WaitForSingleObject(pi.hProcess, 60000);
                        GetExitCodeProcess(pi.hProcess, &exit_code);
                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                    }
                    DeleteFileA(zip.c_str());

                    if (!ok) {
                        toast("Extract failed: PowerShell unavailable");
                    } else if (exit_code != 0) {
                        char buf[160];
                        std::snprintf(buf, sizeof(buf),
                                      "Extract failed (exit %lu)",
                                      static_cast<unsigned long>(exit_code));
                        toast(buf);
                    } else {
                        toast("Profiles installed to config\\vrto3d");
                    }
#endif
                }).detach();
            };

            // The headset HWND is created by the presenter on its window
            // thread; FindWindow is the only way to discover it from here.
            // OsdRenderer handles null gracefully — input mouse mapping just
            // skips until the HWND becomes available.
            renderer_->ConfigureOsd(stereo_display_component_.get(), std::move(cb), nullptr);
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
    if ( strcmp( pchComponentNameAndVersion, vr::IVRDriverDirectModeComponent_Version ) == 0 )
    {
        return direct_mode_component_.get();
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
// Purpose: Receive OpenTrack 3DoF updates
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::OpenTrackThread()
{
    SOCKET socket_s;
    struct sockaddr_in from = {};
#ifdef _WIN32
    int from_len = sizeof(from);
#else
    socklen_t from_len = sizeof(from);
#endif
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

#ifdef _WIN32
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
#else
    {
        struct sockaddr_in local = {};
        local.sin_family = AF_INET;
        local.sin_port = htons(ot_port);
        local.sin_addr.s_addr = INADDR_ANY;

        socket_s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_s == INVALID_SOCKET) {
            LOG() << "OpenTrack socket creation failed: " << errno;
        } else {
            fcntl(socket_s, F_SETFL, fcntl(socket_s, F_GETFL, 0) | O_NONBLOCK);
            if (bind(socket_s, (struct sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
                LOG() << "OpenTrack bind failed: " << errno;
                closesocket(socket_s);
                socket_s = INVALID_SOCKET;
            }
        }
    }
#endif

    while (is_active_) {
        // Skip the recv pump entirely when OpenTrack is off; the OSD can flip
        // use_open_track live, so we keep the thread (and bound socket) alive
        // and just sleep a coarser tick while disabled.
        if (!stereo_display_component_->GetConfig().use_open_track) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

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
#ifdef _WIN32
    WSACleanup();
#endif
}


//-----------------------------------------------------------------------------
// Purpose: Poll XInput and publish controller-derived rotation and offset
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::XInputUpdateThread()
{
    float current_pitch = 0.0f;
    vr::HmdQuaternion_t current_yaw_quat = HmdQuaternion_Identity;
    bool was_idle = false;

    while (is_active_)
    {
        const auto loop_start = std::chrono::steady_clock::now();
        const auto config = stereo_display_component_->GetConfig();

        // When neither stick is consumed and no reset is pending, skip the
        // XInput poll, math, and mutex publish. On the live→idle edge, clear
        // the published controller pose once so PoseUpdateThread doesn't keep
        // applying the last non-zero offset.
        if (!config.pitch_enable && !config.yaw_enable && !config.pose_reset)
        {
            if (!was_idle)
            {
                current_pitch = 0.0f;
                current_yaw_quat = HmdQuaternion_Identity;
                {
                    std::lock_guard<std::mutex> lock(controller_pose_mutex_);
                    controller_rotation_ = HmdQuaternion_Identity;
                    controller_pos_offset_ = { 0.0, 0.0, 0.0 };
                }
                was_idle = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }
        was_idle = false;

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
            ConsumePoseReset();
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
            // Reset on the disabled→enabled edge so the filter doesn't carry
            // stale state (potentially minutes old) into its first FilterPose
            // call when the OSD toggles use_track_filter on. Without this the
            // initial samples produce noisy output until the filter resettles.
            if (!track_filter_was_enabled_)
            {
                track_filter_.Reset();
            }
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
        int top = 0;
        int save = 0;
        int menu = 0;
        int auto_depth = 0;
    } sleep;

    const int sleep_time = static_cast<int>(floor(1000.0 / stereo_display_component_->GetConfig().display_frequency));

    auto getOsd = [this]() -> vrto3d::osd::OsdRenderer* {
        return renderer_ ? renderer_->Osd() : nullptr;
    };

    auto setOverlay = [&](const std::string& msg) {
        if (auto* osd = getOsd()) osd->SetText(msg);
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

    // Deferred projection re-sync for the depth/convergence hotkeys.
    // ResetProjection() posts SetDisplayProjectionRaw + a LensDistortionChanged
    // event that most VR mods answer by rebuilding their projection; firing it
    // at poll rate while a key autorepeats makes adjustment feel laggy. The
    // flush at the bottom of the loop resyncs immediately on the first press,
    // at most every 150ms while held, and once more on release so the final
    // value always lands.
    bool resync_pending = false;
    auto last_resync = std::chrono::steady_clock::now() - std::chrono::seconds(1);

    while (is_active_) {
        auto cfg = stereo_display_component_->GetConfig();

        // Ctrl+Home (keyboard) or Start+DPad-Down (gamepad) toggles the OSD
        // menu. Always polled (independent of disable_hotkeys) so users can
        // always recover from a runaway config. Start+Back is avoided here
        // because several VR mods already use that pair as their own pause /
        // menu chord.
        DWORD menu_pad = 0;
        const DWORD menu_chord_mask = XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_DPAD_DOWN;
        const bool menu_pad_chord =
            GetXInputButtonState(menu_pad) &&
            ((menu_pad & menu_chord_mask) == menu_chord_mask);
        if (((isCtrlDown() && isDown(VK_HOME)) || menu_pad_chord) && sleep.menu == 0) {
            if (auto* osd = getOsd()) {
                osd->ToggleMenu();
                osd->SetAppName(app_name_);
                osd->SetVersion(stereo_version_number_);
            }
            sleep.menu = cfg.sleep_count_max;
        } else if (sleep.menu > 0) {
            --sleep.menu;
        }

        // While the menu is open, suppress the rest of the hotkey poll so
        // arrow keys / numbers / Enter reach ImGui instead of bumping depth.
        if (auto* osd = getOsd(); osd && osd->MenuVisible()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
            continue;
        }

        if (!cfg.disable_hotkeys) {
            // Ctrl+F3 Decrease Depth (re-sync projection; hold Shift to skip the sync)
            if (isCtrlDown() && isDown(VK_F3)) {
                stereo_display_component_->AdjustDepth(-0.001f, true);
                if (!isDown(VK_SHIFT)) resync_pending = true;
                setOverlay(fmtDepthConv());
            }
            // Ctrl+F4 Increase Depth (re-sync projection; hold Shift to skip the sync)
            else if (isCtrlDown() && isDown(VK_F4)) {
                stereo_display_component_->AdjustDepth(0.001f, true);
                if (!isDown(VK_SHIFT)) resync_pending = true;
                setOverlay(fmtDepthConv());
            }
            // Ctrl+F5 Decrease Convergence
            else if (isCtrlDown() && isDown(VK_F5)) {
                stereo_display_component_->AdjustConvergence(0.005f, true, false);
                resync_pending = true;
                setOverlay(fmtDepthConv());
            }
            // Ctrl+F6 Increase Convergence
            else if (isCtrlDown() && isDown(VK_F6)) {
                stereo_display_component_->AdjustConvergence(-0.005f, true, false);
                resync_pending = true;
                setOverlay(fmtDepthConv());
            }
            // Ctrl+F7 Store settings into game profile
            if (isCtrlDown() && isDown(VK_F7) && sleep.save == 0) {
                if (!prev_name_.empty()) {
                    // Re-fetch so any depth/conv adjust earlier this tick is
                    // reflected in the saved file.
                    auto save_cfg = stereo_display_component_->GetConfig();
                    JsonManager().SaveProfileToJson(prev_name_ + "_config.json", save_cfg);
                    BeepSuccess();
                    setOverlay("Saved " + prev_name_ + "_config.json profile");
                }
                else {
                    BeepFailure();
                    setOverlay("Failed to save profile");
                }
                sleep.save = cfg.sleep_count_max;
            }
            // Ctrl+F10 Reload settings from Game Profile or (+Shift) Default Profile
            if (isCtrlDown() && isDown(VK_F10) && sleep.save == 0) {
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
                    auto_focus_.store(cfg.auto_focus);
                    hide_cursor_.store(cfg.hide_cursor);
                    lock_cursor_.store(cfg.lock_cursor);
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
            // Ctrl+F11 Toggle Auto-Depth
            if (isCtrlDown() && isDown(VK_F11) && sleep.auto_depth == 0) {
                const bool now = !stereo_display_component_->IsAutoDepthEnabled();
                stereo_display_component_->SetAutoDepthEnabled(now);
                setOverlay(now ? "Auto-Depth: ON"
                               : "Auto-Depth: OFF (manual restored)");
                sleep.auto_depth = cfg.sleep_count_max;
            }
            else if (sleep.auto_depth > 0) {
                --sleep.auto_depth;
            }
        }
        // Ctrl+F8 (keyboard) or Start+DPad-Up (gamepad) Toggle Always On Top.
        // Always polled (like the menu chord) so a controller-only Steam Deck
        // user can raise a lowered/buried overlay back on top without a
        // keyboard — the direct counterpart to Start+DPad-Down opening the menu.
        DWORD top_pad = 0;
        const DWORD top_chord_mask = XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_DPAD_UP;
        const bool top_pad_chord =
            GetXInputButtonState(top_pad) &&
            ((top_pad & top_chord_mask) == top_chord_mask);
        if (((isCtrlDown() && isDown(VK_F8)) || top_pad_chord) && sleep.top == 0) {
            is_on_top_ = !is_on_top_;
            man_on_top_ = is_on_top_.load();
            sleep.top = cfg.sleep_count_max;
        }
        else if (sleep.top > 0) {
            --sleep.top;
        }
        // Ctrl+F12 Take Screenshot — drains on the next composited frame
        // inside Dx11Renderer::WaitAndDrawPending.
        if (isCtrlDown() && isDown(VK_F12) && sleep.shot == 0) {
            if (renderer_) {
                std::string name = !app_name_.empty() ? app_name_
                                  : !prev_name_.empty() ? prev_name_
                                  : std::string("vrto3d");
                renderer_->RequestScreenshot(name);
            }
            sleep.shot = cfg.sleep_count_max;
        }
        else if (sleep.shot > 0) {
            --sleep.shot;
        }
        // Check User binds (preset hotkeys configured by the user — load/store)
        auto hotkey_str = stereo_display_component_->CheckUserSettings();
        if (!hotkey_str.empty()) {
            setOverlay(hotkey_str);
        }

        // Check for new profile load
        if (app_updated_)
        {
            setOverlay("Loaded " + app_name_ + "_config.json profile");
            if (auto* osd = getOsd()) osd->SetAppName(app_name_);
            app_updated_ = false;
        }
        // Check for no profile
        else if (no_profile_)
        {
            setOverlay("No profile found for " + app_name_);
            no_profile_ = false;
        }

        // Flush a deferred depth/convergence projection re-sync (see the
        // resync_pending comment above the loop for the batching rules).
        if (resync_pending) {
            const bool adjust_held = isCtrlDown() &&
                (isDown(VK_F3) || isDown(VK_F4) || isDown(VK_F5) || isDown(VK_F6));
            const auto now = std::chrono::steady_clock::now();
            if (!adjust_held || now - last_resync >= std::chrono::milliseconds(150)) {
                stereo_display_component_->ResetProjection();
                resync_pending = false;
                last_resync = now;
            }
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
    fc.app_pid     = &app_pid_;
    fc.auto_focus  = &auto_focus_;
    return fc;
}


//-----------------------------------------------------------------------------
// Purpose: Process UE3D/UEVR shared-memory monitor/depth requests
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::MonitorModeThread() {
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
// Purpose: Apply hide_cursor / lock_cursor to the connected game's window
// from out-of-process (we live in vrserver.exe, the game is its own
// process). Modeled on 3DVision4All's hide_cursor / confine_cursor knobs,
// but cross-process: instead of subclassing the game's WndProc and
// rewriting its class cursor (only possible from an injected DLL), we:
//
//   1. Poll for the game's main HWND.
//   2. When the game is the foreground process, AttachThreadInput to its
//      input queue so SetCursor / ClipCursor calls share state with the
//      game's thread (Windows otherwise blocks cursor clipping from
//      non-foreground processes as an anti-trap measure).
//   3. ClipCursor to the game's window rect (lock_cursor) and/or call
//      SetCursor(nullptr) (hide_cursor) every tick — repeated assertion is
//      required because Windows releases the clip on every foreground
//      transition and the game's WndProc re-issues its own cursor on each
//      WM_SETCURSOR. ~30 Hz is fast enough to feel instantaneous.
//
// Cleanup: when the user disables lock_cursor (or the game loses focus /
// closes), we release any clip we set. We don't try to "restore" the
// hidden cursor — the game's next WM_SETCURSOR (fires on mouse motion)
// brings its own cursor back.
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::CursorControlThread()
{
#ifndef _WIN32
    // Cursor clipping/hiding is not implementable from an external process on
    // Wayland; X11 support could ride on XGrabPointer later. Idle politely.
    while (is_active_.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return;
#else
    bool clip_active = false;

    while (is_active_.load(std::memory_order_relaxed)) {
        const bool want_hide = hide_cursor_.load(std::memory_order_relaxed);
        const bool want_lock = lock_cursor_.load(std::memory_order_relaxed);
        // Only act while our VR overlay is actually on top of the game — when
        // the user toggles topmost off (Ctrl+F8 / OSD) they're back at the
        // desktop and expect their real cursor / clip back, regardless of
        // these toggles.
        const bool on_top    = is_on_top_.load(std::memory_order_relaxed);
        // Suppress while the OSD menu is open so the user can actually click
        // ImGui widgets — both knobs would fight the menu's pointer otherwise.
        const bool menu_open = renderer_ && renderer_->Osd()
                                && renderer_->Osd()->MenuVisible();

        if (!on_top || menu_open || (!want_hide && !want_lock)) {
            if (clip_active) {
                ClipCursor(nullptr);
                clip_active = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        const uint32_t pid = app_pid_.load();
        HWND game = (pid != 0 && IsProcessRunning(pid)) ? GetHWNDFromPID(pid) : nullptr;
        const bool game_is_fg = game && GetForegroundWindow() == game;

        if (!game_is_fg) {
            if (clip_active) {
                ClipCursor(nullptr);
                clip_active = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        const DWORD game_tid = GetWindowThreadProcessId(game, nullptr);
        const DWORD my_tid   = GetCurrentThreadId();
        const BOOL  attached = (game_tid && game_tid != my_tid)
                                ? AttachThreadInput(my_tid, game_tid, TRUE)
                                : FALSE;

        if (want_lock) {
            RECT r;
            if (GetWindowRect(game, &r)) {
                ClipCursor(&r);
                clip_active = true;
            }
        } else if (clip_active) {
            ClipCursor(nullptr);
            clip_active = false;
        }

        if (want_hide) {
            SetCursor(nullptr);
        }

        if (attached) {
            AttachThreadInput(my_tid, game_tid, FALSE);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    if (clip_active) {
        ClipCursor(nullptr);
    }
#endif
}


//-----------------------------------------------------------------------------
// Purpose: Load Game Specific Settings from Steam\config\vrto3d\app_name_config.json
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::LoadSettings(const std::string& app_name, uint32_t app_pid, vr::EVREventType status)
{
    // Quick same-pid reconnect (e.g. RealVR re-inits VR after FoV/res
    // change): the disconnect branch dropped focus and armed a 15s grace
    // timer. Refocus immediately so the user isn't stuck without input
    // passthrough — but only if focus was actually on before the
    // disconnect (we don't want to override a user "always-on-top off"
    // choice). The pending 15s thread will wake later and re-assert,
    // which is a no-op since flags are already set.
    if (status == vr::VREvent_ProcessConnected
        && app_name == app_name_
        && app_pid == app_pid_
        && focus_pre_disconnect_.load()
        && !man_on_top_.load())
    {
        is_on_top_  = true;
        man_on_top_ = true;
        focus_pre_disconnect_.store(false);
        return;
    }

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
        }
        else {
            BeepFailure();
            no_profile_ = true;
        }

        SetAsync(config.async_enable);
        // Mirror the freshly-loaded profile's auto_focus to the live atomic
        // observed by presenter focus loops.
        auto_focus_.store(config.auto_focus);
        hide_cursor_.store(config.hide_cursor);
        lock_cursor_.store(config.lock_cursor);

        if (config.auto_focus) {
            // Run off-thread: this is called from RunFrame, and a 10s sleep
            // here freezes the driver host's event loop. Snapshot the pid so
            // a fast disconnect/reconnect doesn't make us focus/recenter for
            // a different app. Retry the recenter a few times because games
            // commonly call ResetSeatedZeroPose during their own VR init and
            // will clobber a single well-timed shot.
            const uint32_t pid = app_pid_.load();
            std::thread([this, pid]() {
                std::this_thread::sleep_for(std::chrono::seconds(8));
                if (!is_active_) return;
                if (app_pid_.load() != pid) return;
                is_on_top_ = true;
                man_on_top_ = true;
                // Give the game a moment after focus before kicking the
                // recenter, so the first attempt lands after the game has
                // settled into its initial pose rather than mid-init.
                std::this_thread::sleep_for(std::chrono::seconds(4));
                if (!is_active_) return;
                if (app_pid_.load() != pid) return;
                if (!man_on_top_.load()) return;
                for (int i = 0; i < 3; ++i) {
                    const std::string tag = "auto_focus#" + std::to_string(i + 1);
                    if (vrto3d::TriggerOpenVRRecenter(tag.c_str())) return;
                    if (i < 2) std::this_thread::sleep_for(std::chrono::seconds(2));
                    if (!is_active_) return;
                    if (app_pid_.load() != pid) return;
                    if (!man_on_top_.load()) return;
                }
            }).detach();
        }
    }
    else if (status == vr::VREvent_ProcessDisconnected)
    {
        // Capture the user's pre-disconnect focus preference so both the
        // 15s grace thread and the quick-reconnect path can honor a user
        // "always-on-top off" choice instead of forcing focus back on.
        const bool was_focused = man_on_top_.load();
        focus_pre_disconnect_.store(was_focused);

        is_on_top_ = false;
        man_on_top_ = false;

        if (!was_focused) return;

        // Some games briefly disconnect from SteamVR (compositor blip,
        // scene-app handoff) and immediately reconnect with the same exe
        // still alive. Wait 15s, then re-engage focus if the original
        // process is still running and no new app has connected since.
        uint32_t pid = app_pid_.load();
        std::thread([this, pid]() {
            std::this_thread::sleep_for(std::chrono::seconds(15));
            if (!is_active_) return;
            if (app_pid_.load() == pid && IsProcessRunning(pid)) {
                is_on_top_ = true;
                man_on_top_ = true;
                focus_pre_disconnect_.store(false);
            }
        }).detach();
    }
}


//-----------------------------------------------------------------------------
// Purpose: Consume the pose-reset signal. Zeros the cached OpenTrack
// attitude/position so stale UDP-derived bias can't bleed into the pose the
// next time it's consumed (e.g. when use_open_track is toggled back on, or
// when the user pressed the reset hotkey while OpenTrack was active), then
// clears the flag and asks SteamVR to take the cleaned HMD pose as the new
// seated/standing zero. The TriggerOpenVRRecenter dispatch is deferred to a
// detached thread so PoseUpdateThread has a tick to publish the cleaned pose
// — otherwise SteamVR's snapshot would bake in the stale bias we just
// cleared. Single consumption point for both the pose-reset hotkey and the
// OSD recenter-on-disable toggle paths.
//-----------------------------------------------------------------------------
void MockControllerDeviceDriver::ConsumePoseReset()
{
    {
        std::lock_guard<std::mutex> lock(trk_mutex_);
        open_track_att_ = HmdQuaternion_Identity;
        open_track_pos_ = { 0.0, 0.0, 0.0 };
    }
    stereo_display_component_->SetReset();
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        vrto3d::TriggerOpenVRRecenter("pose_reset");
    }).detach();
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
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }
        if (track_thread_.joinable()) {
            track_thread_.join();
        }
        if (cursor_thread_.joinable()) {
            cursor_thread_.join();
        }
        // Direct-mode component holds a raw Dx11Renderer*; destroy it first
        // so any in-flight SubmitLayer/Present can no longer reach the
        // renderer before the renderer is torn down.
        direct_mode_component_.reset();
        if (renderer_) {
            renderer_->Shutdown();
            renderer_.reset();
        }
    }

    // unassign our controller index (we don't want to be calling vrserver anymore after Deactivate() has been called
    device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}

MockControllerDeviceDriver::~MockControllerDeviceDriver() = default;


//-----------------------------------------------------------------------------
// DISPLAY DRIVER METHOD DEFINITIONS
//-----------------------------------------------------------------------------

StereoDisplayComponent::StereoDisplayComponent( const StereoDisplayDriverConfiguration &config )
    : config_( config ), depth_(config.depth), convergence_(config.convergence), fov_(config.fov)
{
    manual_depth_.store(config.depth);
    auto_depth_enabled_.store(config.auto_depth_enabled);
    auto_depth_target_disparity_.store(config.auto_depth_target_disparity);
    auto_depth_smoothing_.store(config.auto_depth_smoothing);
}


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
    *pnWidth  = static_cast<uint32_t>(config_.render_width);
    *pnHeight = static_cast<uint32_t>(config_.render_height);
}

//-----------------------------------------------------------------------------
// Canonical SbS eye viewport. Never called by the compositor in direct mode
// but kept functional to satisfy the IVRDisplayComponent pure-virtual contract.
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
// IVRDisplayComponent distortion / window-bounds members. Never called by the
// compositor in direct mode but kept functional to satisfy the interface's
// pure-virtual contract.
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

void StereoDisplayComponent::GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
    std::shared_lock<std::shared_mutex> lock(cfg_mutex_);
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
    StereoDisplayDriverConfiguration cfg = config_;
    // depth/convergence/fov live in atomics during runtime — config_ only
    // holds the last JSON-loaded values. Sync them so callers that round-trip
    // a snapshot through LoadSettings don't clobber live hotkey adjustments.
    // For depth specifically, report manual_depth_ (the user's intended
    // ceiling) rather than the live, possibly auto-modulated depth_ — otherwise
    // any OSD control that round-trips cfg through LoadSettings while
    // auto-depth is active would bake the auto-attenuated value into the
    // ceiling and defeat the snap-back on disable.
    cfg.depth       = manual_depth_.load(std::memory_order_relaxed);
    cfg.convergence = convergence_.load(std::memory_order_relaxed);
    cfg.fov         = fov_.load(std::memory_order_relaxed);
    return cfg;
}


//-----------------------------------------------------------------------------
// Purpose: To update the Depth value
//-----------------------------------------------------------------------------
void StereoDisplayComponent::ApplyDepth(float new_depth)
{
    if (new_depth < 0.0f) new_depth = 0.0f;
    float cur_depth = GetDepth();
    while (!depth_.compare_exchange_weak(cur_depth, new_depth, std::memory_order_relaxed));
    vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(device_index_);
    vr::VRProperties()->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, new_depth);
}


void StereoDisplayComponent::AdjustDepth(float new_depth, bool is_delta)
{
    if (is_delta) {
        new_depth += manual_depth_.load(std::memory_order_relaxed);
        if (new_depth < 0.0f) new_depth = 0.0f;
    }
    // Manual edits always update the user's intended ceiling.
    manual_depth_.store(new_depth, std::memory_order_relaxed);

    if (auto_depth_enabled_.load(std::memory_order_relaxed)) {
        // Auto on: the auto loop drives the live depth_ down toward the
        // disparity target. Don't yank the live depth up past the new
        // ceiling — clamp it from above instead.
        ApplyDepth((std::min)(GetDepth(), new_depth));
    } else {
        ApplyDepth(new_depth);
    }
}


bool StereoDisplayComponent::IsAutoDepthEnabled() const
{
    return auto_depth_enabled_.load(std::memory_order_relaxed);
}


void StereoDisplayComponent::SetAutoDepthEnabled(bool enabled)
{
    auto_depth_enabled_.store(enabled, std::memory_order_relaxed);
    // Reset the input-side disparity filters so the next enable starts fresh
    // rather than carrying stale samples from a previous session. The median
    // ring is renderer-thread-owned, so it's cleared there via the flag.
    auto_depth_disp_ema_.store(-1.0f, std::memory_order_relaxed);
    auto_depth_filter_reset_.store(true, std::memory_order_relaxed);
    if (!enabled) {
        // Snap the live depth back to the user's manual ceiling.
        ApplyDepth(manual_depth_.load(std::memory_order_relaxed));
    }
}


float StereoDisplayComponent::GetManualDepth() const
{
    return manual_depth_.load(std::memory_order_relaxed);
}


float StereoDisplayComponent::GetAutoDepthTargetDisparity() const
{
    return auto_depth_target_disparity_.load(std::memory_order_relaxed);
}


void StereoDisplayComponent::SetAutoDepthTargetDisparity(float frac)
{
    if (frac < 0.001f) frac = 0.001f;
    if (frac > 0.01f)  frac = 0.01f;
    auto_depth_target_disparity_.store(frac, std::memory_order_relaxed);
}


float StereoDisplayComponent::GetAutoDepthSmoothing() const
{
    return auto_depth_smoothing_.load(std::memory_order_relaxed);
}


void StereoDisplayComponent::SetAutoDepthSmoothing(float v)
{
    if (v < 0.005f) v = 0.005f;
    if (v > 0.25f)  v = 0.25f;
    auto_depth_smoothing_.store(v, std::memory_order_relaxed);
}


bool StereoDisplayComponent::IsAutoDepthLoggingEnabled() const
{
    return auto_depth_log_enabled_.load(std::memory_order_relaxed);
}


void StereoDisplayComponent::SetAutoDepthLoggingEnabled(bool enabled)
{
    auto_depth_log_enabled_.store(enabled, std::memory_order_relaxed);
}


void StereoDisplayComponent::FeedAutoDepthSample(uint32_t max_disp_px, uint32_t eye_w_px,
                                                 uint32_t disp_step_px)
{
    if (!auto_depth_enabled_.load(std::memory_order_relaxed) || eye_w_px == 0) {
        return;
    }
    if (auto_depth_filter_reset_.exchange(false, std::memory_order_relaxed)) {
        auto_depth_hist_n_ = 0;
        auto_depth_hist_i_ = 0;
    }
    const float ceiling     = manual_depth_.load(std::memory_order_relaxed);
    const float target_frac = auto_depth_target_disparity_.load(std::memory_order_relaxed);
    const float smoothing   = auto_depth_smoothing_.load(std::memory_order_relaxed);
    const float raw_frac    = static_cast<float>(max_disp_px) / static_cast<float>(eye_w_px);

    // Temporal median prefilter: a single-frame disparity spike (particle,
    // muzzle flash, camera clip) must not reach the EMA at all — the
    // asymmetric alpha reacts fast on the rising edge by design, so it
    // can't defend against outliers on its own.
    auto_depth_frac_hist_[auto_depth_hist_i_] = raw_frac;
    auto_depth_hist_i_ = (auto_depth_hist_i_ + 1) % kAutoDepthHist;
    if (auto_depth_hist_n_ < kAutoDepthHist) ++auto_depth_hist_n_;
    float sorted[kAutoDepthHist];
    std::copy(auto_depth_frac_hist_, auto_depth_frac_hist_ + auto_depth_hist_n_, sorted);
    std::nth_element(sorted, sorted + auto_depth_hist_n_ / 2, sorted + auto_depth_hist_n_);
    const float med_frac = sorted[auto_depth_hist_n_ / 2];

    // Input-side jitter filter: low-pass the disparity-fraction samples
    // before they drive the depth target. Uses an asymmetric EMA — fast
    // on rising disparity (close objects must clamp depth quickly for
    // comfort) and slow on falling disparity (so a noisy frame that
    // misses a close object doesn't briefly snap depth back up).
    constexpr float kAlphaUp   = 0.20f;  // disparity rising  -> moderate
    constexpr float kAlphaDown = 0.05f;  // disparity falling -> slow ease
    // Deadband: sized to the analyzer's disparity quantization (~1.5 buckets
    // covers frame-to-frame bucket jitter), capped at half the target so the
    // band can never mask a real error — the old fixed 0.005 equaled the
    // default target and swallowed multiples of it at low target settings.
    const float step_frac = static_cast<float>(disp_step_px) / static_cast<float>(eye_w_px);
    const float deadband  = (std::min)(1.5f * step_frac, 0.5f * target_frac);

    float prev_ema = auto_depth_disp_ema_.load(std::memory_order_relaxed);
    float new_ema;
    if (prev_ema < 0.0f) {
        new_ema = med_frac;  // first sample primes the filter
    } else {
        const float diff = med_frac - prev_ema;
        if (std::fabs(diff) < deadband) {
            new_ema = prev_ema;  // inside deadband — hold steady
        } else {
            const float alpha = (diff > 0.0f) ? kAlphaUp : kAlphaDown;
            new_ema = prev_ema + alpha * diff;
        }
    }
    auto_depth_disp_ema_.store(new_ema, std::memory_order_relaxed);

    // Auto pull-in floor: never attenuate below this fraction of the user's
    // ceiling — a pathological frame (huge close object) should dim the
    // stereo, not flatten it. Analog of UEVR-3D's min_convergence_m.
    constexpr float kMinDepthFrac = 0.15f;

    const float disp_frac = new_ema;
    float target_depth;
    if (disp_frac <= target_frac || disp_frac <= 0.0f) {
        target_depth = ceiling;
    } else {
        target_depth = ceiling * (target_frac / disp_frac);
        if (target_depth > ceiling)                target_depth = ceiling;
        if (target_depth < kMinDepthFrac * ceiling) target_depth = kMinDepthFrac * ceiling;
    }
    const float cur      = GetDepth();
    const float smoothed = cur + (target_depth - cur) * smoothing;
    // The lerp never exactly converges, and every ApplyDepth pushes a
    // SteamVR property write — skip sub-epsilon deltas so a settled loop
    // stops churning vrserver.
    if (std::fabs(smoothed - cur) > 1e-6f) {
        ApplyDepth(smoothed);
    }
}


//-----------------------------------------------------------------------------
// Purpose: To update the Convergence value
//-----------------------------------------------------------------------------
void StereoDisplayComponent::AdjustConvergence(float new_conv, bool is_delta, bool resync)
{
    float cur_conv = GetConvergence();
    if (is_delta) {
        new_conv += cur_conv;
        new_conv = (new_conv < 0.001) ? 0.001 : new_conv;
    }
    if (NearlyEqual(cur_conv, new_conv))
        return;
    while (!convergence_.compare_exchange_weak(cur_conv, new_conv, std::memory_order_relaxed));
    // resync=false lets the hotkey poll loop batch the projection re-sync
    // instead of firing it on every autorepeat tick.
    if (resync) ResetProjection();
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
// Purpose: Toggle Reset on
//-----------------------------------------------------------------------------
void StereoDisplayComponent::RequestPoseReset()
{
    std::unique_lock<std::shared_mutex> lock(cfg_mutex_);
    config_.pose_reset = true;
}


//-----------------------------------------------------------------------------
// Purpose: Load Game Specific Settings from Steam\config\vrto3d\app_name_config.json
//-----------------------------------------------------------------------------
void StereoDisplayComponent::LoadSettings(StereoDisplayDriverConfiguration& config)
{
    // Apply auto-depth settings BEFORE AdjustDepth so that the new ceiling /
    // enabled state are observed when AdjustDepth decides whether to clamp.
    auto_depth_target_disparity_.store(config.auto_depth_target_disparity, std::memory_order_relaxed);
    auto_depth_smoothing_.store(config.auto_depth_smoothing, std::memory_order_relaxed);
    // Disable auto temporarily so AdjustDepth applies the JSON depth as the
    // live value, then restore the desired enabled state.
    const bool want_auto = config.auto_depth_enabled;
    auto_depth_enabled_.store(false, std::memory_order_relaxed);

    // Apply loaded settings
    AdjustDepth(config.depth, false);
    AdjustConvergence(config.convergence, false);
    AdjustFoV(config.fov);

    auto_depth_enabled_.store(want_auto, std::memory_order_relaxed);

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
