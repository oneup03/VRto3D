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
#include "ctrl_device_provider.h"

#include <chrono>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

#include "ctrl_math.h"
#include "ctrl_platform.h"
#include "vrto3d_ctrl_bridge.h"
#include "vrto3dlib/debug_log.hpp"

using nlohmann::json;

namespace vrto3d::ctrl {

namespace {

constexpr const char* kVersion = "1.0.0";
// The loop wakes every millisecond to watch the HMD pose, and polls the
// plugins as soon as VRto3D publishes a new one (or every 4 ms at the
// latest). Hands are placed relative to the head, so publishing them in
// lockstep with the HMD keeps them from jittering against the view while the
// head turns; sampling on an unrelated clock let them lag by 0-12 ms.
constexpr double kWakeSeconds = 0.001;
constexpr double kMaxPollInterval = 0.004;

bool SameHeadPose(const Vrto3dCtrlHeadPose& a, const Vrto3dCtrlHeadPose& b)
{
    return a.valid == b.valid &&
           std::memcmp(a.position, b.position, sizeof(a.position)) == 0 &&
           std::memcmp(a.rotation, b.rotation, sizeof(a.rotation)) == 0;
}

Quat QuatFromMatrix(const vr::HmdMatrix34_t& m)
{
    const double trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    Quat q;
    if (trace > 0.0) {
        const double s = 0.5 / std::sqrt(trace + 1.0);
        q.w = 0.25 / s;
        q.x = (m.m[2][1] - m.m[1][2]) * s;
        q.y = (m.m[0][2] - m.m[2][0]) * s;
        q.z = (m.m[1][0] - m.m[0][1]) * s;
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        const double s = 2.0 * std::sqrt(1.0 + m.m[0][0] - m.m[1][1] - m.m[2][2]);
        q.w = (m.m[2][1] - m.m[1][2]) / s;
        q.x = 0.25 * s;
        q.y = (m.m[0][1] + m.m[1][0]) / s;
        q.z = (m.m[0][2] + m.m[2][0]) / s;
    } else if (m.m[1][1] > m.m[2][2]) {
        const double s = 2.0 * std::sqrt(1.0 + m.m[1][1] - m.m[0][0] - m.m[2][2]);
        q.w = (m.m[0][2] - m.m[2][0]) / s;
        q.x = (m.m[0][1] + m.m[1][0]) / s;
        q.y = 0.25 * s;
        q.z = (m.m[1][2] + m.m[2][1]) / s;
    } else {
        const double s = 2.0 * std::sqrt(1.0 + m.m[2][2] - m.m[0][0] - m.m[1][1]);
        q.w = (m.m[1][0] - m.m[0][1]) / s;
        q.x = (m.m[0][2] + m.m[2][0]) / s;
        q.y = (m.m[1][2] + m.m[2][1]) / s;
        q.z = 0.25 * s;
    }
    return Normalize(q);
}

}  // namespace

vr::EVRInitError CtrlDeviceProvider::Init(vr::IVRDriverContext* pDriverContext)
{
    VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
    DebugLog::SetLogName("vrto3d_ctrl");
    LOG() << "VRto3D controller driver " << kVersion << " starting";

    CtrlConfig cfg = FromJson(store_.Load(""));
    type_ = EmulatedTypeFromString(cfg.emulated_type);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        live_ = cfg;
    }
    opentrack_.SetDestinationPort(platform::VRto3DOpenTrackPort());

    // Both hands are always registered. When controllers are disabled they
    // report "disconnected", so games see no hands but nothing has to be
    // re-registered when the user flips the switch in the OSD.
    const char* suffix[2] = {"L", "R"};
    for (int h = 0; h < 2; ++h) {
        devices_[h] = std::make_unique<CtrlControllerDevice>(h, type_, cfg.serial_prefix + suffix[h]);
        if (!vr::VRServerDriverHost()->TrackedDeviceAdded(devices_[h]->Serial().c_str(),
                                                          vr::TrackedDeviceClass_Controller,
                                                          devices_[h].get())) {
            LOG() << "TrackedDeviceAdded failed for " << devices_[h]->Serial()
                  << " (is activateMultipleDrivers enabled?)";
        }
    }

    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        devices_ready_ = true;
    }
    host_ = std::make_unique<PluginHost>(*this);
    running_.store(true);
    thread_ = std::thread(&CtrlDeviceProvider::InputThread, this);
    return vr::VRInitError_None;
}

void CtrlDeviceProvider::Cleanup()
{
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    host_.reset();
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        devices_ready_ = false;
        for (auto& d : devices_) d.reset();
    }
    LOG() << "VRto3D controller driver stopped";
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
}

void CtrlDeviceProvider::RunFrame()
{
    vr::VREvent_t ev;
    while (vr::VRServerDriverHost()->PollNextEvent(&ev, sizeof(ev))) {
        switch (ev.eventType) {
        case vr::VREvent_Input_HapticVibration: {
            const auto& hv = ev.data.hapticVibration;
            for (uint32_t h = 0; h < 2; ++h) {
                if (devices_[h] && devices_[h]->OwnsHaptic(hv.componentHandle)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (haptics_.size() < 64)
                        haptics_.push_back({h, hv.fDurationSeconds, hv.fFrequency, hv.fAmplitude});
                }
            }
            break;
        }
        // Same connect events VRto3D uses to pick a per-game profile.
        case vr::VREvent_ProcessConnected:
        case vr::VREvent_ActionBindingReloaded:
        case vr::VREvent_SceneApplicationChanged:
        case vr::VREvent_Input_BindingLoadFailed:
        case vr::VREvent_Input_BindingLoadSuccessful:
        case vr::VREvent_Input_ActionManifestReloaded: {
            const uint32_t pid = ev.data.process.pid;
            if (pid == 0 || pid == runframe_game_pid_) break;
            const std::string exe = platform::ProcessName(pid);
            if (!platform::IsGameProcessName(exe)) break;
            runframe_game_pid_ = pid;
            std::lock_guard<std::mutex> lock(mutex_);
            pending_game_ = GameChange{exe, pid};
            break;
        }
        case vr::VREvent_ProcessDisconnected:
        case vr::VREvent_SceneAppPipeDisconnected: {
            if (runframe_game_pid_ != 0 && ev.data.process.pid == runframe_game_pid_) {
                runframe_game_pid_ = 0;
                std::lock_guard<std::mutex> lock(mutex_);
                pending_game_ = GameChange{"", 0};
            }
            break;
        }
        default:
            break;
        }
    }
}

bool CtrlDeviceProvider::SendOpenTrack(double x_cm, double y_cm, double z_cm,
                                       double yaw, double pitch, double roll)
{
    return opentrack_.Send(x_cm, y_cm, z_cm, yaw, pitch, roll);
}

bool CtrlDeviceProvider::IsGameForeground()
{
    const uint32_t fg = platform::ForegroundProcessId();
    if (fg == 0) {
#ifdef _WIN32
        return false;
#else
        return true;  // no focus information (e.g. Wayland): don't gate
#endif
    }
    // VRto3D's output window lives in vrserver, i.e. in this process.
    return fg == game_pid_.load() || fg == platform::CurrentProcessId();
}

void CtrlDeviceProvider::OnHmdPose(const vr::DriverPose_t& p)
{
    if (!p.poseIsValid) return;
    // World pose of the head: worldFromDriver * driverPose * driverFromHead.
    const Quat wfd{p.qWorldFromDriverRotation.w, p.qWorldFromDriverRotation.x,
                   p.qWorldFromDriverRotation.y, p.qWorldFromDriverRotation.z};
    const Quat rot{p.qRotation.w, p.qRotation.x, p.qRotation.y, p.qRotation.z};
    const Quat dfh{p.qDriverFromHeadRotation.w, p.qDriverFromHeadRotation.x,
                   p.qDriverFromHeadRotation.y, p.qDriverFromHeadRotation.z};
    const Vec3 local = Vec3{p.vecPosition[0], p.vecPosition[1], p.vecPosition[2]} +
                       Rotate(rot, Vec3{p.vecDriverFromHeadTranslation[0], p.vecDriverFromHeadTranslation[1],
                                        p.vecDriverFromHeadTranslation[2]});
    HeadFrame head;
    head.valid = true;
    head.rot = Normalize(wfd * rot * dfh);
    head.pos = Vec3{p.vecWorldFromDriverTranslation[0], p.vecWorldFromDriverTranslation[1],
                    p.vecWorldFromDriverTranslation[2]} + Rotate(wfd, local);

    const double now = platform::NowSeconds();
    {
        std::lock_guard<std::mutex> lock(hook_mutex_);
        hook_head_ = head;
    }
    last_hook_s_.store(now);

    std::lock_guard<std::mutex> lock(devices_mutex_);
    if (!devices_ready_) return;
    for (auto& d : devices_)
        if (d) d->FollowHead(head, now);
}

bool CtrlDeviceProvider::ReadHeadPose(Vrto3dCtrlHeadPose& head, double now, bool& from_hook)
{
    head = {};
    head.struct_size = sizeof(head);
    head.rotation[0] = 1.0;
    head.sample_time_s = now;
    from_hook = HookLive(now);
    if (from_hook) {
        HeadFrame h;
        {
            std::lock_guard<std::mutex> lock(hook_mutex_);
            h = hook_head_;
        }
        head.valid = h.valid ? 1 : 0;
        head.position[0] = h.pos.x;
        head.position[1] = h.pos.y;
        head.position[2] = h.pos.z;
        head.rotation[0] = h.rot.w;
        head.rotation[1] = h.rot.x;
        head.rotation[2] = h.rot.y;
        head.rotation[3] = h.rot.z;
        return h.valid;
    }

    vr::TrackedDevicePose_t poses[1] = {};
    vr::VRServerDriverHost()->GetRawTrackedDevicePoses(0.0f, poses, 1);
    if (!poses[0].bPoseIsValid || !poses[0].bDeviceIsConnected) return false;

    const auto& m = poses[0].mDeviceToAbsoluteTracking;
    const Quat q = QuatFromMatrix(m);
    head.valid = 1;
    head.position[0] = m.m[0][3];
    head.position[1] = m.m[1][3];
    head.position[2] = m.m[2][3];
    head.rotation[0] = q.w;
    head.rotation[1] = q.x;
    head.rotation[2] = q.y;
    head.rotation[3] = q.z;
    return true;
}

void CtrlDeviceProvider::ApplyOnInputThread(CtrlConfig cfg, const char* why)
{
    if (EmulatedTypeFromString(cfg.emulated_type) != type_) {
        LOG() << "emulated_type '" << cfg.emulated_type
              << "' takes effect after a SteamVR restart (running as "
              << EmulatedTypeToString(type_) << ")";
    }
    host_->ApplyConfig(cfg);  // fills plugin defaults, normalises priority
    {
        std::lock_guard<std::mutex> lock(mutex_);
        live_ = std::move(cfg);
    }
    LOG() << "Controller config applied (" << why << ")";
}

void CtrlDeviceProvider::InputThread()
{
#ifdef _WIN32
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    host_->LoadAll(platform::ModuleDirectory() / "plugins");

    CtrlConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg = live_;
    }
    ApplyOnInputThread(cfg, "startup");

    std::string game_exe;
    std::string stamp = store_.Stamp(game_exe);
    double next_stamp_check = platform::NowSeconds() + 1.0;
    double last_poll = platform::NowSeconds();
    Vrto3dCtrlHeadPose last_head{};
    bool logged_no_head = false;

    auto next = std::chrono::steady_clock::now();
    while (running_.load()) {
        const double now = platform::NowSeconds();

        // --- Pick up work queued by RunFrame / the OSD bridge -------------
        std::optional<json> pending_cfg;
        std::optional<GameChange> pending_game;
        bool reload = false;
        std::vector<HapticRequest> haptics;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_cfg.swap(pending_config_);
            pending_game.swap(pending_game_);
            reload = reload_requested_;
            reload_requested_ = false;
            haptics.swap(haptics_);
        }

        if (pending_game) {
            game_exe = pending_game->exe;
            game_pid_.store(pending_game->pid);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                game_exe_ = game_exe;
            }
            LOG() << (game_exe.empty() ? std::string("Game disconnected")
                                       : "Game connected: " + game_exe + " (pid " +
                                             std::to_string(pending_game->pid) + ")");
            ApplyOnInputThread(FromJson(store_.Load(game_exe)), "game changed");
            host_->DispatchGameChanged(game_exe, pending_game->pid);
            stamp = store_.Stamp(game_exe);
        } else if (reload) {
            ApplyOnInputThread(FromJson(store_.Load(game_exe)), "reload");
            stamp = store_.Stamp(game_exe);
        } else if (pending_cfg) {
            ApplyOnInputThread(FromJson(*pending_cfg), "OSD edit");
        }

        // Hand edits to the JSON files while SteamVR runs.
        if (now >= next_stamp_check) {
            next_stamp_check = now + 1.0;
            const std::string s = store_.Stamp(game_exe);
            if (s != stamp) {
                stamp = s;
                ApplyOnInputThread(FromJson(store_.Load(game_exe)), "file changed on disk");
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            cfg = live_;
        }

        // --- Poll plugins and publish ------------------------------------
        Vrto3dCtrlHeadPose head;
        bool from_hook = false;
        const bool head_ok = ReadHeadPose(head, now, from_hook);
        if (from_hook && !logged_hook_) {
            LOG() << "Receiving HMD poses directly from VRto3D; head-anchored hands follow them in lockstep";
            logged_hook_ = true;
        }
        if (!head_ok && cfg.enabled && !logged_no_head) {
            LOG() << "No valid HMD pose yet; hands will appear once the HMD is tracking";
            logged_no_head = true;
        }

        const bool head_moved = !SameHeadPose(head, last_head);
        if (head_moved || now - last_poll >= kMaxPollInterval || !haptics.empty()) {
            last_head = head;
            const double dt = now - last_poll;
            last_poll = now;
            MergedHand merged[2];
            if (cfg.enabled) host_->Poll(dt, head, merged);
            HeadFrame frame;
            frame.valid = head.valid != 0;
            frame.pos = {head.position[0], head.position[1], head.position[2]};
            frame.rot = Normalize(Quat{head.rotation[0], head.rotation[1], head.rotation[2], head.rotation[3]});
            for (int h = 0; h < 2; ++h) {
                if (devices_[h]) devices_[h]->Update(merged[h], cfg, cfg.enabled, now, frame, from_hook);
            }
        }

        for (const auto& hr : haptics) {
            if (cfg.log_haptics) {
                LOG() << "haptic hand=" << (hr.hand == 0 ? "L" : "R") << " dur=" << hr.duration
                      << " freq=" << hr.frequency << " amp=" << hr.amplitude;
            }
            if (cfg.enabled) host_->DispatchHaptic(hr.hand, hr.duration, hr.frequency, hr.amplitude);
        }

        next += std::chrono::microseconds((int64_t)(kWakeSeconds * 1e6));
        const auto wake_now = std::chrono::steady_clock::now();
        if (next < wake_now) next = wake_now;  // don't try to catch up after a stall
        std::this_thread::sleep_until(next);
    }

    host_->ShutdownAll();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

// ---------------------------------------------------------------------------
// Bridge
// ---------------------------------------------------------------------------
uint32_t CtrlDeviceProvider::GetStateJson(char* out, uint32_t cap)
{
    json j;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        j["config"] = ToJson(live_);
        j["game"] = game_exe_;
    }
    j["bridge"] = VRTO3D_CTRL_BRIDGE_VERSION;
    j["version"] = kVersion;
    j["devices_type"] = EmulatedTypeToString(type_);
    j["config_folder"] = store_.Folder().string();
    json plugins = json::array();
    if (host_) {
        for (const auto& p : host_->Status()) {
            plugins.push_back({{"name", p.name},
                               {"display_name", p.display_name},
                               {"version", p.version},
                               {"state", p.state},
                               {"builtin", p.builtin},
                               {"capabilities", p.capabilities},
                               {"schema", p.schema}});
        }
    }
    j["plugins"] = std::move(plugins);

    const std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace);
    if (out && cap > 0) {
        const size_t n = (std::min)(s.size(), (size_t)cap - 1);
        std::memcpy(out, s.data(), n);
        out[n] = '\0';
    }
    return (uint32_t)s.size();
}

int CtrlDeviceProvider::ApplyConfigJson(const char* text)
{
    if (!text) return -1;
    try {
        json j = json::parse(text);
        if (!j.is_object()) return -1;
        std::lock_guard<std::mutex> lock(mutex_);
        pending_config_ = std::move(j);
        return 0;
    } catch (const std::exception& e) {
        LOG() << "ApplyConfigJson: " << e.what();
        return -1;
    }
}

int CtrlDeviceProvider::SaveConfig(int target)
{
    json full;
    std::string exe;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        full = ToJson(live_);
        exe = game_exe_;
    }
    bool ok = false;
    if (target == 0) {
        ok = store_.SaveDefaults(full);
        LOG() << "Saved " << store_.DefaultPath().string() << (ok ? "" : " FAILED");
    } else {
        ok = store_.SaveGame(exe, full);
        LOG() << "Saved game controller profile for '" << exe << "'" << (ok ? "" : " FAILED");
    }
    return ok ? 0 : -1;
}

int CtrlDeviceProvider::ReloadConfig()
{
    std::lock_guard<std::mutex> lock(mutex_);
    reload_requested_ = true;
    return 0;
}

}  // namespace vrto3d::ctrl
