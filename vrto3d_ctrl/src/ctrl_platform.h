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

// The controller driver's only window into OS-specific helpers. Everything
// else in vrto3d_ctrl (except the built-in input plugins) is portable.

#include <cstdint>
#include <filesystem>
#include <string>

namespace vrto3d::ctrl::platform {

std::string SteamInstallPath();

// Executable file name of a process ("Game.exe"), "<unknown>" on failure.
std::string ProcessName(uint32_t pid);

// Same rule VRto3D uses to decide that a connecting process is a game whose
// profile should load (skips SteamVR's own helpers, launchers, etc).
bool IsGameProcessName(const std::string& name);

// Process owning the focused window, 0 when unknown (e.g. Wayland).
uint32_t ForegroundProcessId();
uint32_t CurrentProcessId();

// Folder containing driver_vrto3d_ctrl's binary (bin/<platform>).
std::filesystem::path ModuleDirectory();

// Monotonic clock in seconds; same epoch as VRto3D's pose timestamps.
double NowSeconds();

// VRto3D's OpenTrack UDP port from default_config.json (4242 if missing).
int VRto3DOpenTrackPort();

}  // namespace vrto3d::ctrl::platform
