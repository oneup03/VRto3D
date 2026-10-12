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

// Client side of the in-process bridge to the emulated controller driver
// (driver_vrto3d_ctrl, a separate SteamVR driver loaded into the same
// vrserver). Every call is a harmless no-op returning false when that
// driver is not installed or not loaded, so VRto3D never depends on it.

#include <cstdint>

#include <nlohmann/json.hpp>

namespace vrto3d::osd::ctrl_bridge {

// True once the controller driver's exports were resolved. Retries the
// lookup at most every couple of seconds while it is missing.
bool Available();

// Live state: config, game, plugin list with schemas (see
// vrto3d_ctrl_bridge.h for the shape).
bool GetState(nlohmann::json& out);

// Replace the live controller config (nothing is written to disk).
bool ApplyConfig(const nlohmann::json& config);

// target 0 = controllers.json, 1 = <game>.exe_controllers.json
bool Save(int target);
bool Reload();

// Tells keyboard / mouse plugins to ignore input while the OSD menu is open.
// Remembered and re-sent once the driver appears, so the order the two
// drivers load in does not matter.
void SetInputSuppressed(bool suppressed);

// Hand the HMD pose VRto3D just published (a vr::DriverPose_t) to the
// controller driver so head-anchored hands move in the same instant. Called
// from the pose thread on every update: lock-free, and a no-op until the
// controller driver is found (looked up at most every two seconds).
void NotifyHmdPose(const void* driver_pose, uint32_t size);

}  // namespace vrto3d::osd::ctrl_bridge
