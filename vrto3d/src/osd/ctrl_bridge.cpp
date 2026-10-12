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
#include "osd/ctrl_bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "vrto3d_ctrl_bridge.h"

namespace vrto3d::osd::ctrl_bridge {

namespace {

struct Api {
    Vrto3dCtrl_GetBridgeVersionFn version = nullptr;
    Vrto3dCtrl_GetStateJsonFn get_state = nullptr;
    Vrto3dCtrl_ApplyConfigJsonFn apply = nullptr;
    Vrto3dCtrl_SaveConfigFn save = nullptr;
    Vrto3dCtrl_ReloadConfigFn reload = nullptr;
    Vrto3dCtrl_SetInputSuppressedFn suppress = nullptr;
    bool ok = false;
};

std::mutex g_mutex;
Api g_api;
std::chrono::steady_clock::time_point g_next_try{};
bool g_suppressed = false;

void* Lookup(void* module, const char* name)
{
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name));
#else
    return dlsym(module, name);
#endif
}

// Caller holds g_mutex.
bool ResolveLocked()
{
    if (g_api.ok) return true;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_try) return false;
    g_next_try = now + std::chrono::seconds(2);

#ifdef _WIN32
    // Only look at an already-loaded module; never load it ourselves.
    void* module = GetModuleHandleW(VRTO3D_CTRL_MODULE_NAME);
#else
    void* module = dlopen(VRTO3D_CTRL_MODULE_NAME, RTLD_NOW | RTLD_NOLOAD);
#endif
    if (!module) return false;

    Api a;
    a.version = reinterpret_cast<Vrto3dCtrl_GetBridgeVersionFn>(Lookup(module, VRTO3D_CTRL_EXPORT_GET_BRIDGE_VERSION));
    a.get_state = reinterpret_cast<Vrto3dCtrl_GetStateJsonFn>(Lookup(module, VRTO3D_CTRL_EXPORT_GET_STATE_JSON));
    a.apply = reinterpret_cast<Vrto3dCtrl_ApplyConfigJsonFn>(Lookup(module, VRTO3D_CTRL_EXPORT_APPLY_CONFIG_JSON));
    a.save = reinterpret_cast<Vrto3dCtrl_SaveConfigFn>(Lookup(module, VRTO3D_CTRL_EXPORT_SAVE_CONFIG));
    a.reload = reinterpret_cast<Vrto3dCtrl_ReloadConfigFn>(Lookup(module, VRTO3D_CTRL_EXPORT_RELOAD_CONFIG));
    a.suppress = reinterpret_cast<Vrto3dCtrl_SetInputSuppressedFn>(Lookup(module, VRTO3D_CTRL_EXPORT_SET_INPUT_SUPPRESSED));
    if (!a.version || !a.get_state || !a.apply || !a.save || !a.reload || !a.suppress) return false;
    if (a.version() != VRTO3D_CTRL_BRIDGE_VERSION) return false;

    a.ok = true;
    g_api = a;
    g_api.suppress(g_suppressed ? 1 : 0);
    return true;
}

}  // namespace

bool Available()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return ResolveLocked();
}

bool GetState(nlohmann::json& out)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ResolveLocked()) return false;
    std::string buf(16 * 1024, '\0');
    uint32_t n = g_api.get_state(&buf[0], (uint32_t)buf.size());
    if (n >= buf.size()) {
        buf.assign((size_t)n + 1, '\0');
        n = g_api.get_state(&buf[0], (uint32_t)buf.size());
    }
    buf.resize((std::min)((size_t)n, buf.size()));
    try {
        out = nlohmann::json::parse(buf);
        return out.is_object();
    } catch (const std::exception&) {
        return false;
    }
}

bool ApplyConfig(const nlohmann::json& config)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ResolveLocked()) return false;
    const std::string s = config.dump();
    return g_api.apply(s.c_str()) == 0;
}

bool Save(int target)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ResolveLocked()) return false;
    return g_api.save(target) == 0;
}

bool Reload()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ResolveLocked()) return false;
    return g_api.reload() == 0;
}

void SetInputSuppressed(bool suppressed)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_suppressed = suppressed;
    if (ResolveLocked()) g_api.suppress(suppressed ? 1 : 0);
}

void NotifyHmdPose(const void* driver_pose, uint32_t size)
{
    // Separate from g_api/g_mutex: this runs on the pose thread at 2x the
    // display rate and must never wait on an OSD call.
    static std::atomic<Vrto3dCtrl_OnHmdPoseFn> hook{nullptr};
    static std::atomic<int64_t> next_try_ms{0};

    Vrto3dCtrl_OnHmdPoseFn fn = hook.load(std::memory_order_acquire);
    if (!fn) {
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_ms < next_try_ms.load(std::memory_order_relaxed)) return;
        next_try_ms.store(now_ms + 2000, std::memory_order_relaxed);
#ifdef _WIN32
        void* module = GetModuleHandleW(VRTO3D_CTRL_MODULE_NAME);
#else
        void* module = dlopen(VRTO3D_CTRL_MODULE_NAME, RTLD_NOW | RTLD_NOLOAD);
#endif
        if (!module) return;
        fn = reinterpret_cast<Vrto3dCtrl_OnHmdPoseFn>(Lookup(module, VRTO3D_CTRL_EXPORT_ON_HMD_POSE));
        if (!fn) return;
        hook.store(fn, std::memory_order_release);
    }
    fn(driver_pose, size);
}

}  // namespace vrto3d::osd::ctrl_bridge
