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

// Suppresses the 3D Vision OSD red-text overlay
//   "Warning: attempt to run Stereoscopic 3D in a non-stereo display mode,
//    please change to an acceptable mode. See documentation for acceptable
//    Stereoscopic 3D modes."
// rendered into the back buffer by NVIDIA's DX9 user-mode display driver
// (nvd3dumx.dll) when 3D Vision is activated on a panel that the driver
// doesn't classify as stereo-certified. The OSD survives our compositor
// pass because the driver writes it after PresentEx, so the only way to
// stop it is to neutralise the code that emits it from inside the same
// process.
//
// Approach: nvd3dumx.dll contains a per-frame OSD dispatcher
// (FUN_1802c4850 in current driver builds) that reads a 23-slot warnings
// bitmask at offset 600 of its argument struct and renders each set bit as
// a red/yellow text block. Bit 10 corresponds to the "non-stereo display
// mode" warning. We MinHook the dispatcher, clear bit 10 before the
// original runs, and pass through — every other driver warning still
// fires normally.
//
// The hook is resolved by signature scan over nvd3dumx.dll's .text section
// (no hardcoded offset), so it survives driver updates as long as
// NVIDIA doesn't reshuffle this specific function's prologue. If the
// signature doesn't match (older / future driver), Install() returns
// false and the warning is left untouched.
class Nvd3dumOsdPatcher {
public:
    Nvd3dumOsdPatcher() = default;
    ~Nvd3dumOsdPatcher();

    Nvd3dumOsdPatcher(const Nvd3dumOsdPatcher&) = delete;
    Nvd3dumOsdPatcher& operator=(const Nvd3dumOsdPatcher&) = delete;

    // Locates nvd3dumx.dll in the current process, signature-scans the
    // dispatcher, installs the MinHook detour. Safe to call when the DLL
    // isn't loaded (returns false). Idempotent — repeat Install() calls
    // after success are no-ops.
    bool Install();

    // Removes the detour and releases the MinHook reference. Safe to call
    // when not installed (no-op).
    void Uninstall();

    bool IsInstalled() const { return installed_; }

private:
    bool  installed_ = false;
    void* target_    = nullptr;   // address of the dispatcher in nvd3dumx.dll
};

}  // namespace platform
