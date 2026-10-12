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
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ctrl_config.h"
#include "vrto3d_ctrl_plugin.h"

namespace vrto3d::ctrl {

// What the host needs from the driver to implement Vrto3dCtrlHostApi.
class HostServices {
public:
    virtual ~HostServices() = default;
    virtual bool SendOpenTrack(double x_cm, double y_cm, double z_cm,
                               double yaw, double pitch, double roll) = 0;
    virtual bool IsGameForeground() = 0;
    virtual bool IsInputSuppressed() = 0;
    virtual uint32_t GamePid() = 0;
};

// Merged output for one hand after all plugins were polled.
struct MergedHand {
    bool tracked = false;           // some plugin supplied a pose
    Vrto3dCtrlControllerState s{};  // pose from the winning plugin, inputs from all
    bool fingers_reported = false;  // some contributing plugin has CAP_FINGERS
    bool velocity_reported = false; // the pose plugin has CAP_VELOCITY
};

struct PluginStatus {
    std::string name;
    std::string display_name;
    std::string version;
    std::string state;  // ok | disabled | faulted | init_failed
    bool builtin = false;
    uint32_t capabilities = 0;
    nlohmann::json schema = nlohmann::json::object();
};

// Owns every plugin. All methods except Status() must be called from the
// driver's input thread, which is therefore the only thread that ever runs
// plugin code.
class PluginHost {
public:
    explicit PluginHost(HostServices& services);
    ~PluginHost();

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // Registers the built-ins, then every plugin found in plugin_dir.
    void LoadAll(const std::filesystem::path& plugin_dir);
    void ShutdownAll();

    // Applies enable flags, priority and settings. Fills cfg.plugins with
    // defaults from each plugin's schema so the stored config is complete.
    void ApplyConfig(CtrlConfig& cfg);

    void Poll(double dt, const Vrto3dCtrlHeadPose& head, MergedHand out[2]);
    void DispatchHaptic(uint32_t hand, float duration, float freq, float amp);
    void DispatchGameChanged(const std::string& exe, uint32_t pid);

    std::vector<PluginStatus> Status() const;

private:
    struct Slot;

    void AddPlugin(Vrto3dCtrlPluginEntryFn entry, void* module, bool builtin,
                   const std::string& origin);
    void Fault(Slot& s, const char* where);
    void RebuildOrder(const std::vector<std::string>& priority);
    void PublishStatus();

    // Vrto3dCtrlHostApi trampolines.
    static void     ApiLog(void* ctx, int level, const char* msg);
    static int      ApiGetBool(void* ctx, const char* key, int def);
    static int64_t  ApiGetInt(void* ctx, const char* key, int64_t def);
    static double   ApiGetFloat(void* ctx, const char* key, double def);
    static uint32_t ApiGetString(void* ctx, const char* key, char* buf, uint32_t cap);
    static uint32_t ApiGetJson(void* ctx, const char* key, char* buf, uint32_t cap);
    static int      ApiSendOpenTrack(void* ctx, double x, double y, double z,
                                     double yaw, double pitch, double roll);
    static int      ApiIsGameForeground(void* ctx);
    static int      ApiIsInputSuppressed(void* ctx);
    static uint32_t ApiGetGamePid(void* ctx);

    HostServices& services_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::vector<Slot*> order_;  // polling / merge priority
    std::atomic<Slot*> head_owner_{nullptr};

    mutable std::mutex status_mutex_;
    std::vector<PluginStatus> status_;
};

}  // namespace vrto3d::ctrl
