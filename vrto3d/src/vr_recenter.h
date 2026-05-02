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

// Thin wrapper around openvr.h (the *client* API). Kept in its own
// translation unit so the rest of the driver can keep including
// openvr_driver.h without hitting redeclaration errors from the two
// headers sharing the vr:: namespace.

namespace vrto3d {

// Triggers a SteamVR recenter via the OpenVR client API
// (IVRChaperone::ResetZeroPose). Replaces the old
// PostMessage(vr_window, WM_KEYDOWN, 'Z', 0) hack.
//
// Initializes a transient background-app session, issues the reset for
// both the seated and standing universes, and shuts the session back
// down. Safe to call from any thread; returns false if the runtime
// could not be reached.
bool TriggerOpenVRRecenter();

} // namespace vrto3d
