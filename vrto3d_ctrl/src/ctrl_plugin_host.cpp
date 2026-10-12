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
#include "ctrl_plugin_host.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "ctrl_plugin_guard.h"
#include "plugins/builtin_plugins.h"
#include "vrto3dlib/debug_log.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace vrto3d::ctrl {

struct PluginHost::Slot {
    PluginHost*       host = nullptr;
    Vrto3dCtrlPlugin  p{};
    Vrto3dCtrlHostApi api{};
    void*             module = nullptr;
    bool              builtin = false;
    std::string       name;
    std::string       origin;
    json              schema = json::object();

    bool enabled = false;
    bool initialized = false;
    bool init_failed = false;
    bool faulted = false;

    std::mutex settings_mutex;  // getters may be called from plugin threads
    json       settings = json::object();
    bool       settings_dirty = false;
};

namespace {

bool ValidName(const char* n)
{
    if (!n || !*n) return false;
    for (const char* c = n; *c; ++c) {
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_';
        if (!ok) return false;
    }
    return true;
}

bool DefaultEnabled(bool builtin, const std::string& name)
{
    // Keyboard + mouse takes over the mouse, so it is opt-in. Third-party
    // plugins are opt-in too, so dropping a DLL in the folder never changes
    // behaviour until the user ticks it in the OSD.
    if (!builtin) return false;
    return name != "kbm";
}

uint32_t CopyOut(const std::string& s, char* buf, uint32_t cap)
{
    if (buf && cap > 0) {
        const size_t n = (std::min)(s.size(), (size_t)cap - 1);
        std::memcpy(buf, s.data(), n);
        buf[n] = '\0';
    }
    return (uint32_t)s.size();
}

void UnloadModule(void* m)
{
    if (!m) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(m));
#else
    dlclose(m);
#endif
}

// Picks the 2D vector with the larger magnitude.
void MergeAxis(float dst[2], const float src[2])
{
    const float a = dst[0] * dst[0] + dst[1] * dst[1];
    const float b = src[0] * src[0] + src[1] * src[1];
    if (b > a) {
        dst[0] = src[0];
        dst[1] = src[1];
    }
}

}  // namespace

PluginHost::PluginHost(HostServices& services) : services_(services) {}

PluginHost::~PluginHost()
{
    ShutdownAll();
}

void PluginHost::AddPlugin(Vrto3dCtrlPluginEntryFn entry, void* module, bool builtin,
                           const std::string& origin)
{
    auto slot = std::make_unique<Slot>();
    slot->host = this;
    slot->module = module;
    slot->builtin = builtin;
    slot->origin = origin;

    int rc = -1;
    if (!guard::Entry(entry, VRTO3D_CTRL_ABI_VERSION, &slot->p, &rc)) {
        // Leave the module loaded: code from it may still be on some stack.
        LOG() << "Plugin " << origin << ": entry point crashed; skipped";
        return;
    }
    if (rc != 0) {
        LOG() << "Plugin " << origin << ": entry point returned " << rc << "; skipped";
        UnloadModule(module);
        return;
    }
    if (slot->p.abi_version != VRTO3D_CTRL_ABI_VERSION ||
        slot->p.struct_size < sizeof(Vrto3dCtrlPlugin)) {
        LOG() << "Plugin " << origin << ": ABI " << slot->p.abi_version
              << " (struct " << slot->p.struct_size << " bytes) does not match host ABI "
              << VRTO3D_CTRL_ABI_VERSION << "; skipped";
        UnloadModule(module);
        return;
    }
    if (!ValidName(slot->p.name)) {
        LOG() << "Plugin " << origin << ": invalid name (use [a-z0-9_]+); skipped";
        UnloadModule(module);
        return;
    }
    slot->name = slot->p.name;
    for (const auto& s : slots_) {
        if (s->name == slot->name) {
            LOG() << "Plugin " << origin << ": name '" << slot->name
                  << "' already taken by " << s->origin << "; skipped";
            UnloadModule(module);
            return;
        }
    }

    const char* schema_text = nullptr;
    if (!guard::Schema(&slot->p, &schema_text)) {
        LOG() << "Plugin " << slot->name << ": settings_schema crashed; skipped";
        return;
    }
    if (schema_text && *schema_text) {
        try {
            json parsed = json::parse(schema_text);
            if (parsed.is_object() && parsed.contains("settings") && parsed["settings"].is_array())
                slot->schema = std::move(parsed);
            else
                LOG() << "Plugin " << slot->name << ": schema has no \"settings\" array; ignored";
        } catch (const std::exception& e) {
            LOG() << "Plugin " << slot->name << ": schema is not valid JSON (" << e.what() << ")";
        }
    }

    Vrto3dCtrlHostApi& api = slot->api;
    api.struct_size = sizeof(Vrto3dCtrlHostApi);
    api.host_abi_version = VRTO3D_CTRL_ABI_VERSION;
    api.ctx = slot.get();
    api.log = &ApiLog;
    api.get_bool = &ApiGetBool;
    api.get_int = &ApiGetInt;
    api.get_float = &ApiGetFloat;
    api.get_string = &ApiGetString;
    api.get_json = &ApiGetJson;
    api.send_opentrack = &ApiSendOpenTrack;
    api.is_game_foreground = &ApiIsGameForeground;
    api.is_input_suppressed = &ApiIsInputSuppressed;
    api.get_game_pid = &ApiGetGamePid;

    LOG() << "Plugin registered: " << slot->name << " ("
          << (slot->p.display_name ? slot->p.display_name : slot->name.c_str()) << " "
          << (slot->p.version ? slot->p.version : "") << ") from " << origin;
    slots_.push_back(std::move(slot));
}

void PluginHost::LoadAll(const fs::path& plugin_dir)
{
    for (const auto& b : builtin::Plugins()) {
        AddPlugin(b.entry, nullptr, true, std::string("builtin:") + b.name);
    }

    std::error_code ec;
    // Create the drop folder so users can find it (archives may omit it).
    if (!plugin_dir.empty() && !fs::exists(plugin_dir, ec)) fs::create_directories(plugin_dir, ec);
    if (plugin_dir.empty() || !fs::is_directory(plugin_dir, ec)) {
        LOG() << "No plugin folder at " << plugin_dir.string();
        PublishStatus();
        return;
    }
    LOG() << "Scanning plugins in " << plugin_dir.string();

    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(plugin_dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
#ifdef _WIN32
        const char* ext = ".dll";
#else
        const char* ext = ".so";
#endif
        std::string x = e.path().extension().string();
        std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (x == ext) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());

    for (const auto& path : files) {
#ifdef _WIN32
        // Resolve the plugin's own dependencies from its folder first.
        HMODULE m = LoadLibraryExW(path.c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m) {
            LOG() << "Plugin " << path.string() << ": LoadLibrary failed (" << GetLastError() << ")";
            continue;
        }
        auto entry = reinterpret_cast<Vrto3dCtrlPluginEntryFn>(
            GetProcAddress(m, VRTO3D_CTRL_PLUGIN_ENTRY_NAME));
        void* module = m;
#else
        void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!module) {
            LOG() << "Plugin " << path.string() << ": dlopen failed (" << dlerror() << ")";
            continue;
        }
        auto entry = reinterpret_cast<Vrto3dCtrlPluginEntryFn>(
            dlsym(module, VRTO3D_CTRL_PLUGIN_ENTRY_NAME));
#endif
        if (!entry) {
            LOG() << "Plugin " << path.string() << ": no " << VRTO3D_CTRL_PLUGIN_ENTRY_NAME
                  << " export; skipped";
            UnloadModule(module);
            continue;
        }
        AddPlugin(entry, module, false, path.string());
    }
    PublishStatus();
}

void PluginHost::ShutdownAll()
{
    head_owner_.store(nullptr);
    order_.clear();
    for (auto& s : slots_) {
        if (s->initialized && !s->faulted) {
            if (!guard::Shutdown(&s->p))
                LOG() << "Plugin " << s->name << ": crashed during shutdown";
        }
        s->initialized = false;
        // A faulted plugin may still own threads; keep its code mapped.
        if (!s->faulted) UnloadModule(s->module);
        s->module = nullptr;
    }
    slots_.clear();
    PublishStatus();
}

void PluginHost::Fault(Slot& s, const char* where)
{
    if (s.faulted) return;
    s.faulted = true;
    LOG() << "Plugin " << s.name << " crashed in " << where
          << " and has been disabled until SteamVR restarts";
    if (head_owner_.load() == &s) head_owner_.store(nullptr);
    PublishStatus();
}

void PluginHost::RebuildOrder(const std::vector<std::string>& priority)
{
    order_.clear();
    for (const auto& n : priority) {
        for (auto& s : slots_) {
            if (s->name == n && std::find(order_.begin(), order_.end(), s.get()) == order_.end())
                order_.push_back(s.get());
        }
    }
    std::vector<Slot*> rest;
    for (auto& s : slots_) {
        if (std::find(order_.begin(), order_.end(), s.get()) == order_.end()) rest.push_back(s.get());
    }
    std::sort(rest.begin(), rest.end(), [](Slot* a, Slot* b) { return a->name < b->name; });
    order_.insert(order_.end(), rest.begin(), rest.end());
}

void PluginHost::ApplyConfig(CtrlConfig& cfg)
{
    if (!cfg.plugins.is_object()) cfg.plugins = json::object();

    for (auto& sp : slots_) {
        Slot& s = *sp;
        json& ps = cfg.plugins[s.name];
        if (!ps.is_object()) ps = json::object();
        if (!ps.contains("enabled") || !ps["enabled"].is_boolean())
            ps["enabled"] = DefaultEnabled(s.builtin, s.name);
        for (const auto& item : s.schema["settings"]) {
            if (!item.is_object() || !item.contains("key") || !item["key"].is_string()) continue;
            const std::string key = item["key"].get<std::string>();
            if (!ps.contains(key) && item.contains("default")) ps[key] = item["default"];
        }
        s.enabled = ps["enabled"].get<bool>();
        {
            std::lock_guard<std::mutex> lock(s.settings_mutex);
            if (s.settings != ps) {
                s.settings = ps;
                s.settings_dirty = true;
            }
        }
    }

    RebuildOrder(cfg.plugin_priority);
    cfg.plugin_priority.clear();
    for (Slot* s : order_) cfg.plugin_priority.push_back(s->name);

    Slot* owner = nullptr;
    for (Slot* s : order_) {
        if (s->enabled && !s->faulted && !s->init_failed &&
            (s->p.capabilities & VRTO3D_CAP_HEAD_LOOK)) {
            owner = s;
            break;
        }
    }
    head_owner_.store(owner);

    for (Slot* s : order_) {
        if (s->faulted) continue;
        if (cfg.enabled && s->enabled && !s->initialized && !s->init_failed) {
            int rc = -1;
            if (!guard::Init(&s->p, &s->api, &rc)) {
                Fault(*s, "init");
                continue;
            }
            if (rc != 0) {
                s->init_failed = true;
                LOG() << "Plugin " << s->name << ": init returned " << rc << "; disabled";
                if (head_owner_.load() == s) head_owner_.store(nullptr);
                continue;
            }
            s->initialized = true;
            s->settings_dirty = false;  // init read the current settings
            LOG() << "Plugin " << s->name << " initialized";
        }
        if (s->initialized && s->settings_dirty) {
            s->settings_dirty = false;
            if (!guard::SettingsChanged(&s->p)) Fault(*s, "on_settings_changed");
        }
    }
    PublishStatus();
}

void PluginHost::Poll(double dt, const Vrto3dCtrlHeadPose& head, MergedHand out[2])
{
    for (int h = 0; h < 2; ++h) {
        out[h] = MergedHand{};
        out[h].s.struct_size = sizeof(Vrto3dCtrlControllerState);
        out[h].s.rotation[0] = 1.0;
    }

    for (Slot* s : order_) {
        if (!s->enabled || !s->initialized || s->faulted) continue;

        Vrto3dCtrlControllerState st[2] = {};
        for (auto& x : st) {
            x.struct_size = sizeof(Vrto3dCtrlControllerState);
            x.rotation[0] = 1.0;
        }
        if (!guard::Poll(&s->p, dt, &head, st)) {
            Fault(*s, "poll");
            continue;
        }

        const uint32_t caps = s->p.capabilities;
        for (int h = 0; h < 2; ++h) {
            const Vrto3dCtrlControllerState& in = st[h];
            MergedHand& m = out[h];

            if (!m.tracked && (caps & VRTO3D_CAP_POSE) && in.tracking != VRTO3D_TRACKING_NONE) {
                m.tracked = true;
                m.velocity_reported = (caps & VRTO3D_CAP_VELOCITY) != 0;
                m.s.tracking = in.tracking;
                std::memcpy(m.s.position, in.position, sizeof(in.position));
                std::memcpy(m.s.rotation, in.rotation, sizeof(in.rotation));
                std::memcpy(m.s.velocity, in.velocity, sizeof(in.velocity));
                std::memcpy(m.s.angular_velocity, in.angular_velocity, sizeof(in.angular_velocity));
                m.s.sample_time_s = in.sample_time_s;
                m.s.pose_anchor = in.pose_anchor;
            }

            // Inputs from every plugin: digital OR, analog max.
            m.s.buttons |= in.buttons;
            m.s.touches |= in.touches;
            m.s.trigger = (std::max)(m.s.trigger, in.trigger);
            m.s.grip = (std::max)(m.s.grip, in.grip);
            m.s.grip_force = (std::max)(m.s.grip_force, in.grip_force);
            m.s.trackpad_force = (std::max)(m.s.trackpad_force, in.trackpad_force);
            MergeAxis(m.s.thumbstick, in.thumbstick);
            MergeAxis(m.s.trackpad, in.trackpad);
            if (caps & VRTO3D_CAP_FINGERS) {
                m.fingers_reported = true;
                for (int f = 0; f < 5; ++f)
                    m.s.finger_curl[f] = (std::max)(m.s.finger_curl[f], in.finger_curl[f]);
            }
        }
    }
}

void PluginHost::DispatchHaptic(uint32_t hand, float duration, float freq, float amp)
{
    for (Slot* s : order_) {
        if (!s->enabled || !s->initialized || s->faulted) continue;
        if (!(s->p.capabilities & VRTO3D_CAP_HAPTICS)) continue;
        if (!guard::Haptic(&s->p, hand, duration, freq, amp)) Fault(*s, "on_haptic");
    }
}

void PluginHost::DispatchGameChanged(const std::string& exe, uint32_t pid)
{
    for (Slot* s : order_) {
        if (!s->initialized || s->faulted) continue;
        if (!guard::GameChanged(&s->p, exe.c_str(), pid)) Fault(*s, "on_game_changed");
    }
}

void PluginHost::PublishStatus()
{
    std::vector<PluginStatus> st;
    st.reserve(slots_.size());
    // Report in priority order so the OSD list matches merge order.
    std::vector<Slot*> ordered = order_;
    for (auto& s : slots_)
        if (std::find(ordered.begin(), ordered.end(), s.get()) == ordered.end()) ordered.push_back(s.get());
    for (Slot* s : ordered) {
        PluginStatus ps;
        ps.name = s->name;
        ps.display_name = s->p.display_name ? s->p.display_name : s->name;
        ps.version = s->p.version ? s->p.version : "";
        ps.builtin = s->builtin;
        ps.capabilities = s->p.capabilities;
        ps.schema = s->schema;
        if (s->faulted) ps.state = "faulted";
        else if (s->init_failed) ps.state = "init_failed";
        else if (!s->enabled) ps.state = "disabled";
        else if (!s->initialized) ps.state = "idle";
        else ps.state = "ok";
        st.push_back(std::move(ps));
    }
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_ = std::move(st);
}

std::vector<PluginStatus> PluginHost::Status() const
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

// ---------------------------------------------------------------------------
// Host API trampolines. `ctx` is the plugin's Slot.
// ---------------------------------------------------------------------------
void PluginHost::ApiLog(void* ctx, int level, const char* msg)
{
    auto* s = static_cast<Slot*>(ctx);
    static const char* kLevel[] = {"debug", "info", "warn", "error"};
    const int l = (level < 0 || level > 3) ? 1 : level;
    LOG() << "[" << (s ? s->name : std::string("?")) << "] " << kLevel[l] << ": "
          << (msg ? msg : "");
}

int PluginHost::ApiGetBool(void* ctx, const char* key, int def)
{
    auto* s = static_cast<Slot*>(ctx);
    if (!s || !key) return def;
    std::lock_guard<std::mutex> lock(s->settings_mutex);
    auto it = s->settings.find(key);
    if (it == s->settings.end()) return def;
    if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
    if (it->is_number()) return it->get<double>() != 0.0 ? 1 : 0;
    return def;
}

int64_t PluginHost::ApiGetInt(void* ctx, const char* key, int64_t def)
{
    auto* s = static_cast<Slot*>(ctx);
    if (!s || !key) return def;
    std::lock_guard<std::mutex> lock(s->settings_mutex);
    auto it = s->settings.find(key);
    if (it == s->settings.end()) return def;
    if (it->is_number()) return (int64_t)std::llround(it->get<double>());
    if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
    return def;
}

double PluginHost::ApiGetFloat(void* ctx, const char* key, double def)
{
    auto* s = static_cast<Slot*>(ctx);
    if (!s || !key) return def;
    std::lock_guard<std::mutex> lock(s->settings_mutex);
    auto it = s->settings.find(key);
    if (it == s->settings.end() || !it->is_number()) return def;
    return it->get<double>();
}

uint32_t PluginHost::ApiGetString(void* ctx, const char* key, char* buf, uint32_t cap)
{
    auto* s = static_cast<Slot*>(ctx);
    std::string v;
    if (s && key) {
        std::lock_guard<std::mutex> lock(s->settings_mutex);
        auto it = s->settings.find(key);
        if (it != s->settings.end() && it->is_string()) v = it->get<std::string>();
    }
    return CopyOut(v, buf, cap);
}

uint32_t PluginHost::ApiGetJson(void* ctx, const char* key, char* buf, uint32_t cap)
{
    auto* s = static_cast<Slot*>(ctx);
    std::string v = "null";
    if (s) {
        std::lock_guard<std::mutex> lock(s->settings_mutex);
        if (!key || !*key) {
            v = s->settings.dump();
        } else {
            auto it = s->settings.find(key);
            if (it != s->settings.end()) v = it->dump();
        }
    }
    return CopyOut(v, buf, cap);
}

int PluginHost::ApiSendOpenTrack(void* ctx, double x, double y, double z,
                                 double yaw, double pitch, double roll)
{
    auto* s = static_cast<Slot*>(ctx);
    if (!s || s->host->head_owner_.load() != s) return 0;
    return s->host->services_.SendOpenTrack(x, y, z, yaw, pitch, roll) ? 1 : 0;
}

int PluginHost::ApiIsGameForeground(void* ctx)
{
    auto* s = static_cast<Slot*>(ctx);
    return (s && s->host->services_.IsGameForeground()) ? 1 : 0;
}

int PluginHost::ApiIsInputSuppressed(void* ctx)
{
    auto* s = static_cast<Slot*>(ctx);
    return (s && s->host->services_.IsInputSuppressed()) ? 1 : 0;
}

uint32_t PluginHost::ApiGetGamePid(void* ctx)
{
    auto* s = static_cast<Slot*>(ctx);
    return s ? s->host->services_.GamePid() : 0;
}

}  // namespace vrto3d::ctrl
