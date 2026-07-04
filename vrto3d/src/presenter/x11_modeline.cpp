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
#include "presenter/x11_modeline.h"

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "vrto3dlib/debug_log.hpp"

namespace vrto3d {

namespace {

// Resolve the RROutput for display_index using the same 1-based semantics as
// platform::EnumerateMonitors (0 = primary).
RROutput PickOutput(Display* dpy, XRRScreenResources* res, int32_t display_index)
{
    RROutput primary = XRRGetOutputPrimary(dpy, DefaultRootWindow(dpy));
    int connected_seen = 0;
    RROutput first_connected = 0;
    for (int i = 0; i < res->noutput; ++i) {
        XRROutputInfo* info = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (!info)
            continue;
        const bool connected = info->connection == RR_Connected && info->crtc != 0;
        XRRFreeOutputInfo(info);
        if (!connected)
            continue;
        ++connected_seen;
        if (!first_connected)
            first_connected = res->outputs[i];
        if (display_index > 0 && connected_seen == display_index)
            return res->outputs[i];
        if (display_index == 0 && res->outputs[i] == primary)
            return res->outputs[i];
    }
    return display_index == 0 && primary ? first_connected : first_connected;
}

}  // namespace

bool ApplyFramePackedModeX11(void* xdisplay, int32_t display_index,
                             const FramePackTimingSpec& spec, X11ModelineState& state)
{
    Display* dpy = static_cast<Display*>(xdisplay);
    if (!dpy)
        return false;

    XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, DefaultRootWindow(dpy));
    if (!res)
        return false;

    RROutput output = PickOutput(dpy, res, display_index);
    if (!output) {
        XRRFreeScreenResources(res);
        return false;
    }
    XRROutputInfo* out_info = XRRGetOutputInfo(dpy, res, output);
    if (!out_info || out_info->crtc == 0) {
        if (out_info) XRRFreeOutputInfo(out_info);
        XRRFreeScreenResources(res);
        return false;
    }
    XRRCrtcInfo* crtc_info = XRRGetCrtcInfo(dpy, res, out_info->crtc);
    if (!crtc_info) {
        XRRFreeOutputInfo(out_info);
        XRRFreeScreenResources(res);
        return false;
    }

    state.output = output;
    state.crtc = out_info->crtc;
    state.previous_mode = crtc_info->mode;
    state.prev_x = crtc_info->x;
    state.prev_y = crtc_info->y;
    state.prev_rotation = crtc_info->rotation;

    const unsigned long dot_clock =
        (unsigned long)spec.h_total * spec.v_total * (unsigned long)(spec.refresh_hz + 0.5f);

    // Reuse an existing mode with matching geometry + clock if present
    // (e.g. from a previous run that didn't clean up).
    RRMode mode_id = 0;
    for (int i = 0; i < res->nmode; ++i) {
        const XRRModeInfo& m = res->modes[i];
        if (m.width == spec.active_w && m.height == spec.active_h &&
            m.hTotal == spec.h_total && m.vTotal == spec.v_total &&
            m.dotClock == dot_clock) {
            mode_id = m.id;
            break;
        }
    }

    if (!mode_id) {
        char name[64];
        snprintf(name, sizeof(name), "VRto3D_FP_%ux%u_%.0f", spec.active_w, spec.active_h,
                 (double)spec.refresh_hz);
        XRRModeInfo mode{};
        mode.width = spec.active_w;
        mode.height = spec.active_h;
        mode.dotClock = dot_clock;
        mode.hSyncStart = spec.active_w + spec.h_front_porch;
        mode.hSyncEnd = mode.hSyncStart + spec.h_sync_width;
        mode.hTotal = spec.h_total;
        mode.vSyncStart = spec.active_h + spec.v_front_porch;
        mode.vSyncEnd = mode.vSyncStart + spec.v_sync_width;
        mode.vTotal = spec.v_total;
        mode.modeFlags = RR_HSyncPositive | RR_VSyncPositive;
        mode.name = name;
        mode.nameLength = (int)strlen(name);
        mode_id = XRRCreateMode(dpy, DefaultRootWindow(dpy), &mode);
        if (!mode_id) {
            LOG() << "x11_modeline: XRRCreateMode failed (driver mode validation?)";
            XRRFreeCrtcInfo(crtc_info);
            XRRFreeOutputInfo(out_info);
            XRRFreeScreenResources(res);
            return false;
        }
        state.custom_mode = mode_id;
        XRRAddOutputMode(dpy, output, mode_id);
        XSync(dpy, False);
    }

    Status st = XRRSetCrtcConfig(dpy, res, out_info->crtc, CurrentTime, crtc_info->x,
                                 crtc_info->y, mode_id, crtc_info->rotation, &output, 1);
    XSync(dpy, False);
    XRRFreeCrtcInfo(crtc_info);
    XRRFreeOutputInfo(out_info);
    XRRFreeScreenResources(res);

    state.active = (st == RRSetConfigSuccess);
    if (state.active) {
        LOG() << "x11_modeline: frame-packed mode " << spec.active_w << "x" << spec.active_h
              << "@" << spec.refresh_hz << " active";
    } else {
        LOG() << "x11_modeline: XRRSetCrtcConfig failed (" << (int)st
              << ") — falling back to current mode. On NVIDIA, xorg.conf "
                 "ModeValidation overrides may be required.";
    }
    return state.active;
}

void RestoreModeX11(void* xdisplay, X11ModelineState& state)
{
    Display* dpy = static_cast<Display*>(xdisplay);
    if (!dpy || !state.active)
        return;

    XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, DefaultRootWindow(dpy));
    if (!res)
        return;
    RROutput output = (RROutput)state.output;
    XRRSetCrtcConfig(dpy, res, (RRCrtc)state.crtc, CurrentTime, state.prev_x, state.prev_y,
                     (RRMode)state.previous_mode, state.prev_rotation, &output, 1);
    if (state.custom_mode) {
        XRRDeleteOutputMode(dpy, output, (RRMode)state.custom_mode);
        XRRDestroyMode(dpy, (RRMode)state.custom_mode);
    }
    XSync(dpy, False);
    XRRFreeScreenResources(res);
    state.active = false;
    LOG() << "x11_modeline: previous mode restored";
}

}  // namespace vrto3d
