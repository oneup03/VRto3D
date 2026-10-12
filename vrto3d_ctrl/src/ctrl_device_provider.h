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

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "openvr_driver.h"

#include "ctrl_config.h"
#include "ctrl_controller_device.h"
#include "ctrl_opentrack.h"
#include "ctrl_plugin_host.h"

namespace vrto3d::ctrl {

// SteamVR entry point of driver_vrto3d_ctrl. Registers a left and a right
// emulated controller and runs the input thread that polls the plugins.
//
// The provider never touches VRto3D's HMD or its settings. It only reads the
// HMD pose back from SteamVR (to place hands relative to the head) and, for
// head look, sends OpenTrack packets to VRto3D's UDP port.
class CtrlDeviceProvider : public vr::IServerTrackedDeviceProvider, public HostServices {
public:
    // IServerTrackedDeviceProvider
    vr::EVRInitError Init(vr::IVRDriverContext* pDriverContext) override;
    const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    void RunFrame() override;
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}
    void Cleanup() override;

    // HostServices (callable from any thread)
    bool SendOpenTrack(double x_cm, double y_cm, double z_cm,
                       double yaw, double pitch, double roll) override;
    bool IsGameForeground() override;
    bool IsInputSuppressed() override { return input_suppressed_.load(); }
    uint32_t GamePid() override { return game_pid_.load(); }

    // In-process bridge used by VRto3D's OSD (see vrto3d_ctrl_bridge.h).
    uint32_t GetStateJson(char* out, uint32_t cap);
    int ApplyConfigJson(const char* json);
    int SaveConfig(int target);
    int ReloadConfig();
    void SetInputSuppressed(bool suppressed) { input_suppressed_.store(suppressed); }
    // VRto3D's pose thread, right after it published an HMD pose.
    void OnHmdPose(const vr::DriverPose_t& pose);

private:
    struct HapticRequest {
        uint32_t hand;
        float duration, frequency, amplitude;
    };
    struct GameChange {
        std::string exe;
        uint32_t pid;
    };

    void InputThread();
    void ApplyOnInputThread(CtrlConfig cfg, const char* why);
    // HMD pose for this tick: the one VRto3D last handed us through
    // OnHmdPose when that hook is live, else read back from SteamVR.
    bool ReadHeadPose(Vrto3dCtrlHeadPose& head, double now, bool& from_hook);
    bool HookLive(double now) const { return now - last_hook_s_.load() < 0.1; }

    CtrlConfigStore store_;
    EmulatedType type_ = EmulatedType::Knuckles;
    std::unique_ptr<CtrlControllerDevice> devices_[2];
    std::unique_ptr<PluginHost> host_;
    OpenTrackSender opentrack_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    // Shared between RunFrame / the bridge (any thread) and the input thread.
    std::mutex mutex_;
    CtrlConfig live_;                         // last applied config
    std::string game_exe_;                    // current game, "" when none
    std::optional<nlohmann::json> pending_config_;
    bool reload_requested_ = false;
    std::optional<GameChange> pending_game_;
    std::vector<HapticRequest> haptics_;

    // HMD poses pushed by VRto3D (see OnHmdPose).
    std::mutex devices_mutex_;                // OnHmdPose vs. Cleanup
    bool devices_ready_ = false;
    std::mutex hook_mutex_;
    HeadFrame hook_head_;
    std::atomic<double> last_hook_s_{-1.0};
    bool logged_hook_ = false;

    std::atomic<uint32_t> game_pid_{0};
    std::atomic<bool> input_suppressed_{false};
    uint32_t runframe_game_pid_ = 0;          // RunFrame-only bookkeeping
};

}  // namespace vrto3d::ctrl
