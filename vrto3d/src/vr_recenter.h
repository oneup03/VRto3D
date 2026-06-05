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
//
// `tag` is included in the log line so the same TU can be called from
// several sites (auto_focus retries, hotkey, OSD toggle) and still be
// disambiguated in the driver log.
bool TriggerOpenVRRecenter(const char* tag = nullptr);

// Tear down the cached background client session created by
// TriggerOpenVRRecenter. Safe to call from RunFrame (poll path) or
// Cleanup (fallback). Idempotent.
void ShutdownOpenVRClient();

// Drain pending events on our cached client session and handle
// VREvent_Quit: acknowledge it, then shut the client down. Must be
// called from RunFrame on every tick once VR_Init has succeeded — if
// we don't acknowledge VREvent_Quit, vrserver waits 5s and force-kills
// us. No-op when no client session is open.
void PumpOpenVRClientEvents();

} // namespace vrto3d
