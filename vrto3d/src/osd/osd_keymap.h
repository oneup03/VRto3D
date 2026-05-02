/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <cstdint>
#include <string>

namespace vrto3d::osd {

// Portable key/button naming used in JSON config so the same profile works on
// Windows and Linux. Examples: "Numpad1", "F7", "Home", "Pad.A", "Pad.Guide".
//
// Translation between portable names and platform-specific scancodes lives in
// the osd_input backends.

// Convert a legacy Windows-flavored config string ("VK_NUMPAD1",
// "XINPUT_GAMEPAD_GUIDE") to the portable form ("Numpad1", "Pad.Guide").
// If the input is already portable (or unknown), it is returned unchanged.
std::string ToPortableName(const std::string& legacy_or_portable);

// True iff the input string is a recognized legacy Windows form. Used by
// JsonManager to decide whether the loaded config needs migration + resave.
bool IsLegacyKeyName(const std::string& s);

// Translate a portable name (e.g. "Numpad1") into a Windows VK_/XInput code.
// Returns:
//   * vk_out   = VK_* code if the portable name maps to a keyboard key, else 0
//   * pad_mask = XINPUT_GAMEPAD_* bit mask if it's a controller button, else 0
// At most one of vk_out / pad_mask will be non-zero.
// Returns false if the name is unrecognized.
bool ResolveWin32(const std::string& portable_name, int32_t& vk_out, uint32_t& pad_mask_out);

// Reverse lookup — given a Win32 VK or XInput mask that just fired, return
// the canonical portable name. Used by the click-to-capture key picker.
// Defined only for the Win32 build; the Linux backend has its own KEY_* table
// inline in osd_input_linux.cpp.
#ifdef _WIN32
std::string FromWin32Vk(int32_t vk);
std::string FromWin32PadMask(uint32_t pad_mask);
#endif

} // namespace vrto3d::osd
