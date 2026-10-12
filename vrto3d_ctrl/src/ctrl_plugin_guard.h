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

// Guarded calls into plugin code. Every function returns false when the
// plugin raised a hardware exception (access violation, etc.) or let a C++
// exception escape; the host then latches the plugin as faulted and never
// calls it again, so a broken plugin cannot take vrserver down with it.
//
// Windows uses structured exception handling. The guarded bodies live in
// their own translation unit with no C++ objects in scope, which MSVC
// requires for __try. On Linux there is no equivalent for hardware faults;
// only C++ exceptions are caught.

#include <cstdint>

#include "vrto3d_ctrl_plugin.h"

namespace vrto3d::ctrl::guard {

bool Entry(Vrto3dCtrlPluginEntryFn fn, uint32_t abi, Vrto3dCtrlPlugin* out, int* rc);
bool Init(const Vrto3dCtrlPlugin* p, const Vrto3dCtrlHostApi* host, int* rc);
bool Shutdown(const Vrto3dCtrlPlugin* p);
bool Poll(const Vrto3dCtrlPlugin* p, double dt, const Vrto3dCtrlHeadPose* head,
          Vrto3dCtrlControllerState* out);
bool Haptic(const Vrto3dCtrlPlugin* p, uint32_t hand, float dur, float freq, float amp);
bool SettingsChanged(const Vrto3dCtrlPlugin* p);
bool GameChanged(const Vrto3dCtrlPlugin* p, const char* exe, uint32_t pid);
bool Schema(const Vrto3dCtrlPlugin* p, const char** out);

}  // namespace vrto3d::ctrl::guard
