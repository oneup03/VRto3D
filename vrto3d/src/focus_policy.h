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

#include <cstdint>

namespace vrto3d {

// Shared "should the overlay be on top?" decision, used by both the Windows
// WindowPresenter::FocusThreadLoop and the Linux VkRenderer present-thread
// focus block so the two can't drift. Pure logic over the FocusContext atoms
// (read into these fields by the caller) plus a per-caller latch.
//
// Rule: start lowered; man_on_top (Ctrl+F8 / "Always on Top") forces on top;
// a still-running already-focused app stays on top; and a newly-connected app
// auto-raises exactly once per PID (latching is_on_top/man_on_top via the
// out-params so the user can Ctrl+F8 it back down). The caller supplies any
// extra force-on-top signal (e.g. an open OSD) via force_on_top.
struct FocusInputs {
    bool     is_on_top = false;    // FocusContext::is_on_top
    bool     man_on_top = false;   // FocusContext::man_on_top
    bool     auto_focus = true;    // FocusContext::auto_focus
    uint32_t app_pid = 0;          // FocusContext::app_pid
    bool     app_running = false;  // platform IsProcessRunning(app_pid)
    bool     force_on_top = false; // caller override (OSD open, etc.)
};

struct FocusLatchState {
    uint32_t last_auto_focused_pid = 0;
};

// Returns whether the overlay should be on top this tick. May set
// *set_is_on_top / *set_man_on_top true (the auto-focus latch) — the caller
// writes those back into its atomics. Mirrors WindowPresenter::FocusThreadLoop.
inline bool ComputeWantOnTop(const FocusInputs& in, FocusLatchState& latch,
                             bool* set_is_on_top, bool* set_man_on_top)
{
    *set_is_on_top = false;
    *set_man_on_top = false;

    // Reset the latch when the tracked app is gone so a relaunch re-triggers.
    if (in.app_pid == 0 || !in.app_running)
        latch.last_auto_focused_pid = 0;

    if (in.force_on_top)
        return true;
    if (in.man_on_top)
        return true;
    if (in.is_on_top && in.app_running)
        return true;
    if (in.auto_focus && !in.is_on_top && in.app_running && in.app_pid != 0 &&
        in.app_pid != latch.last_auto_focused_pid) {
        // Auto-raise once per new app PID; latch is_on_top+man_on_top so the
        // user can still Ctrl+F8 it back down for this app.
        *set_is_on_top = true;
        *set_man_on_top = true;
        latch.last_auto_focused_pid = in.app_pid;
        return true;
    }
    return false;
}

}  // namespace vrto3d
