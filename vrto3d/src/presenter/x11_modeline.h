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

// Runtime frame-packed modelines via XRandR — the Linux equivalent of the
// NVAPI/CRU custom-timing path in display_timing_helper.cpp. Only works in a
// real X11 session (XWayland exposes virtual outputs with no hardware
// modesetting); the EDID-firmware override documented in the README's Linux
// section is the fallback for Wayland or stubborn sinks.
//
// Note: this sets the raw 1280x1470/1920x2205 timing. Like CRU-without-3D-flag
// on Windows, no HDMI 3D InfoFrame is emitted (not controllable from
// userspace) — most frame-packed TVs sync to the timing or offer a manual 3D
// mode selection.

#include <cstdint>

#include "vrto3dlib/stereo_config.h"

namespace vrto3d {

struct X11ModelineState {
    unsigned long output = 0;        // RROutput
    unsigned long crtc = 0;          // RRCrtc
    unsigned long previous_mode = 0; // RRMode active before we switched
    unsigned long custom_mode = 0;   // RRMode we created (0 if reused existing)
    int prev_x = 0, prev_y = 0;
    unsigned short prev_rotation = 1; // RR_Rotate_0
    bool active = false;
};

// Applies `spec` to the output selected by display_index (1-based, 0 = primary)
// on an already-open Display*. Saves restore state into `state`.
bool ApplyFramePackedModeX11(void* xdisplay, int32_t display_index,
                             const FramePackTimingSpec& spec, X11ModelineState& state);

// Restores the pre-switch mode and deletes the custom mode if we created one.
void RestoreModeX11(void* xdisplay, X11ModelineState& state);

}  // namespace vrto3d
