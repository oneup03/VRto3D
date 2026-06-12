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

namespace platform {

// Suppresses selected NVIDIA 3D Vision behaviours that the DX9 user-mode
// display driver (nvd3dumx.dll) inflicts on our back buffer or input stream
// once stereo is activated. All suppressions share one MinHook lifecycle and
// one Install/Uninstall, since they all live or die together with the D3D9Ex
// device.
//
//   1. OSD warnings dispatcher (FUN_1802C4850 in current builds)
//        Per-frame fan-out that iterates a 23-slot warnings bitmask at
//        offset 600 of its argument struct. We clear two bits before
//        letting the original run:
//          - bit 0  — Depth Amount slider
//          - bit 10 — "Warning: attempt to run Stereoscopic 3D in a
//                     non-stereo display mode" red overlay
//
//   2. Rating / info overlay (FUN_180284160 in current builds)
//        The function that composites the per-app rating header
//        ("Rating: Excellent/Good/Fair/Not Recommended"), the
//        "This application is not rated by NVIDIA Corp." red overlay
//        when no profile matches, the "Press X to toggle this info"
//        hint, the A–J known-issue list, and the "3D Compatibility
//        mode on/off" notice. For exes without an NVIDIA-side
//        compatibility profile (e.g. vrserver.exe), the function only
//        ever emits the "not rated" combination, so a flat no-op
//        detour is equivalent to suppressing just that.
//
//   3. Hotkey blocker on user32!GetAsyncKeyState
//        NVIDIA's hotkey dispatchers (FUN_1802A9300, FUN_1802A9840, and
//        siblings) poll specific Ctrl+F-key combos via GetAsyncKeyState
//        every frame. We hook that API and, when the caller is inside
//        nvd3dumx.dll's .text AND Ctrl is currently held, lie that the
//        polled F-key isn't pressed for a small blocklist (F3, F4, F5,
//        F6, F7, F10, F11 — Sep ±, Conv ±, WriteConfig, RHWAtScreenMore,
//        CycleFrustumAdjust). Calls from anywhere else in the process,
//        and presses without Ctrl, pass through unmodified.
//
// Hooks 1 and 2 are resolved by signature scan over nvd3dumx.dll's .text
// section. Hook 3 is resolved via GetProcAddress on user32!GetAsyncKeyState.
// All install independently — if any one fails on a future driver build, the
// others still apply, and the suppressor logs each outcome. Install()
// returns true when at least one hook landed.
class Nv3DVisionSuppressor {
public:
    Nv3DVisionSuppressor() = default;
    ~Nv3DVisionSuppressor();

    Nv3DVisionSuppressor(const Nv3DVisionSuppressor&)            = delete;
    Nv3DVisionSuppressor& operator=(const Nv3DVisionSuppressor&) = delete;

    // Locates nvd3dumx.dll + user32.dll in the current process, resolves
    // each target, installs its MinHook detour. Returns true when at least
    // one hook landed. Safe to call when nvd3dumx.dll isn't loaded yet
    // (returns false). Idempotent — repeat Install() calls after success
    // are no-ops.
    bool Install();

    // Removes any installed detour(s) and releases the MinHook reference.
    // Safe to call when not installed (no-op).
    void Uninstall();

    bool IsInstalled() const { return installed_; }

    // Number of hookable targets the suppressor manages. Public so the cpp's
    // spec-array static_assert can match it; bump in lockstep with kHooks[].
    static constexpr int kHookCount = 3;

private:
    // One slot per hookable target. Index matches the cpp's spec array so
    // Uninstall can disable + remove each hook without re-doing the scan.
    bool  installed_                          = false;
    void* installed_targets_[kHookCount]      = {};
};

}  // namespace platform
