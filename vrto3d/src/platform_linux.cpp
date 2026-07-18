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

// Linux implementation of the portable platform:: helpers (platform.h).
// Monitor enumeration prefers XRandR when DISPLAY is set (also covers
// XWayland), falling back to a minimal wl_output listing on pure Wayland
// sessions. Everything else is /proc + CLOCK_MONOTONIC.

#include "platform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#include <wayland-client.h>

namespace platform {

namespace {

// ---------------------------------------------------------------------------
// XRandR enumeration
// ---------------------------------------------------------------------------
std::vector<MonitorInfo> EnumerateMonitorsX11()
{
    std::vector<MonitorInfo> out;

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return out;

    Window root = DefaultRootWindow(dpy);
    XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) {
        XCloseDisplay(dpy);
        return out;
    }

    RROutput primary = XRRGetOutputPrimary(dpy, root);

    for (int i = 0; i < res->noutput; ++i) {
        XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (!oi) continue;
        if (oi->connection == RR_Connected && oi->crtc != 0) {
            XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
            if (ci && ci->width > 0 && ci->height > 0) {
                MonitorInfo m;
                m.index  = static_cast<int32_t>(out.size()) + 1;
                m.x      = ci->x;
                m.y      = ci->y;
                m.width  = ci->width;
                m.height = ci->height;
                m.is_primary = (res->outputs[i] == primary);
                if (oi->name) {
                    m.device_name.assign(oi->name, static_cast<size_t>(oi->nameLen));
                }

                // Refresh from the CRTC's mode line: dotClock / (hTotal * vTotal).
                for (int mm = 0; mm < res->nmode; ++mm) {
                    const XRRModeInfo& mi = res->modes[mm];
                    if (mi.id == ci->mode && mi.hTotal > 0 && mi.vTotal > 0) {
                        m.refresh_hz = static_cast<float>(
                            static_cast<double>(mi.dotClock)
                            / (static_cast<double>(mi.hTotal) * static_cast<double>(mi.vTotal)));
                        break;
                    }
                }
                out.push_back(m);
            }
            if (ci) XRRFreeCrtcInfo(ci);
        }
        XRRFreeOutputInfo(oi);
    }

    XRRFreeScreenResources(res);
    XCloseDisplay(dpy);
    return out;
}

// ---------------------------------------------------------------------------
// Minimal Wayland wl_output enumeration (pure Wayland session, no XWayland).
// Connect, bind every wl_output, roundtrip until geometry/mode/name/done have
// arrived, then disconnect.
// ---------------------------------------------------------------------------
struct WlOutputState {
    wl_output*  output = nullptr;
    int32_t     x = 0;
    int32_t     y = 0;
    int32_t     width = 0;
    int32_t     height = 0;
    int32_t     refresh_mhz = 0;
    std::string name;
    std::string model;
    bool        done = false;
};

struct WlEnumState {
    std::vector<WlOutputState*> outputs;
};

void WlOutputGeometry(void* data, wl_output*, int32_t x, int32_t y,
                      int32_t, int32_t, int32_t,
                      const char*, const char* model, int32_t)
{
    auto* o = static_cast<WlOutputState*>(data);
    o->x = x;
    o->y = y;
    if (model) o->model = model;
}

void WlOutputMode(void* data, wl_output*, uint32_t flags,
                  int32_t width, int32_t height, int32_t refresh)
{
    auto* o = static_cast<WlOutputState*>(data);
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        o->width = width;
        o->height = height;
        o->refresh_mhz = refresh;
    }
}

void WlOutputDone(void* data, wl_output*)
{
    static_cast<WlOutputState*>(data)->done = true;
}

void WlOutputScale(void*, wl_output*, int32_t) {}

void WlOutputName(void* data, wl_output*, const char* name)
{
    auto* o = static_cast<WlOutputState*>(data);
    if (name) o->name = name;
}

void WlOutputDescription(void*, wl_output*, const char*) {}

const wl_output_listener kWlOutputListener = {
    WlOutputGeometry,
    WlOutputMode,
    WlOutputDone,
    WlOutputScale,
    WlOutputName,
    WlOutputDescription,
};

void WlRegistryGlobal(void* data, wl_registry* registry, uint32_t name,
                      const char* interface, uint32_t version)
{
    auto* state = static_cast<WlEnumState*>(data);
    if (std::strcmp(interface, wl_output_interface.name) == 0) {
        uint32_t v = version < 4 ? version : 4;
        auto* o = new WlOutputState();
        o->output = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, v));
        wl_output_add_listener(o->output, &kWlOutputListener, o);
        state->outputs.push_back(o);
    }
}

void WlRegistryGlobalRemove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kWlRegistryListener = {
    WlRegistryGlobal,
    WlRegistryGlobalRemove,
};

std::vector<MonitorInfo> EnumerateMonitorsWayland()
{
    std::vector<MonitorInfo> out;

    wl_display* display = wl_display_connect(nullptr);
    if (!display) return out;

    WlEnumState state;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kWlRegistryListener, &state);

    // First roundtrip binds the outputs, further ones drain their events.
    wl_display_roundtrip(display);
    for (int i = 0; i < 4; ++i) {
        bool all_done = true;
        for (const auto* o : state.outputs) {
            if (!o->done) { all_done = false; break; }
        }
        if (all_done) break;
        if (wl_display_roundtrip(display) < 0) break;
    }

    for (auto* o : state.outputs) {
        MonitorInfo m;
        m.index  = static_cast<int32_t>(out.size()) + 1;
        m.x      = o->x;
        m.y      = o->y;
        m.width  = o->width  > 0 ? static_cast<uint32_t>(o->width)  : 0;
        m.height = o->height > 0 ? static_cast<uint32_t>(o->height) : 0;
        if (o->refresh_mhz > 0) {
            m.refresh_hz = static_cast<float>(o->refresh_mhz) / 1000.0f;
        }
        m.device_name = !o->name.empty() ? o->name : o->model;
        // Wayland has no "primary output" concept — treat the first as primary.
        m.is_primary = out.empty();
        out.push_back(m);

        wl_output_destroy(o->output);
        delete o;
    }
    state.outputs.clear();

    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return out;
}

bool EnvSet(const char* name)
{
    const char* v = getenv(name);
    return v != nullptr && v[0] != '\0';
}

}  // namespace


std::vector<MonitorInfo> EnumerateMonitors()
{
    std::vector<MonitorInfo> out;
    if (EnvSet("DISPLAY")) {
        out = EnumerateMonitorsX11();
    } else if (EnvSet("WAYLAND_DISPLAY")) {
        out = EnumerateMonitorsWayland();
    }

    // Ensure primary is at index 1 if possible (mirrors the Windows helper).
    auto primary_it = std::find_if(out.begin(), out.end(),
                                   [](const MonitorInfo& m) { return m.is_primary; });
    if (primary_it != out.end() && primary_it != out.begin()) {
        std::iter_swap(out.begin(), primary_it);
        for (size_t i = 0; i < out.size(); ++i) out[i].index = static_cast<int32_t>(i) + 1;
    }
    return out;
}


bool ResolveTargetMonitors(int32_t display_index,
                           bool multi_display,
                           MonitorInfo& out_primary,
                           MonitorInfo& out_secondary)
{
    out_secondary = {};
    auto monitors = EnumerateMonitors();
    if (monitors.empty()) return false;

    // 0 or out-of-range -> primary (first).
    if (display_index <= 0 || display_index > static_cast<int32_t>(monitors.size())) {
        out_primary = monitors.front();
    } else {
        out_primary = monitors[static_cast<size_t>(display_index - 1)];
    }

    if (multi_display) {
        // Find a contiguous right neighbor with matching width/height + same top Y.
        const int32_t right_x = out_primary.x + static_cast<int32_t>(out_primary.width);
        for (const auto& m : monitors) {
            if (m.index == out_primary.index) continue;
            if (m.y == out_primary.y
                && m.x == right_x
                && m.width  == out_primary.width
                && m.height == out_primary.height) {
                out_secondary = m;
                break;
            }
        }
        if (out_secondary.width == 0) {
            std::fprintf(stderr, "[vrto3d] multi_display=true but no contiguous right "
                                 "neighbor matched; continuing single-monitor.\n");
        }
    }
    return true;
}


float QueryRefreshHz(const MonitorInfo& monitor, float fallback_hz)
{
    if (monitor.refresh_hz > 1.0f) return monitor.refresh_hz;
    return fallback_hz;
}


double MonotonicSeconds()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}


std::string GetProcessNameByPid(uint32_t pid)
{
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%u/comm", pid);

    FILE* f = std::fopen(path, "r");
    if (!f) return "";

    char buf[256] = {};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);

    std::string name(buf, n);
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r'
                             || name.back() == ' ' || name.back() == '\t')) {
        name.pop_back();
    }
    return name;
}


bool IsProcessRunning(uint32_t pid)
{
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%u", pid);
    struct stat st{};
    return stat(path, &st) == 0;
}

}  // namespace platform
