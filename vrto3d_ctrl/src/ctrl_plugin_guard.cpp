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
#include "ctrl_plugin_guard.h"

#ifdef _WIN32
#include <windows.h>
// Each body below must hold no C++ objects with destructors (MSVC C2712).
#define GUARD_BEGIN __try {
#define GUARD_END   } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } return true;
#else
#define GUARD_BEGIN try {
#define GUARD_END   } catch (...) { return false; } return true;
#endif

namespace vrto3d::ctrl::guard {

bool Entry(Vrto3dCtrlPluginEntryFn fn, uint32_t abi, Vrto3dCtrlPlugin* out, int* rc)
{
    GUARD_BEGIN
        *rc = fn(abi, out);
    GUARD_END
}

bool Init(const Vrto3dCtrlPlugin* p, const Vrto3dCtrlHostApi* host, int* rc)
{
    GUARD_BEGIN
        *rc = p->init ? p->init(p->user, host) : 0;
    GUARD_END
}

bool Shutdown(const Vrto3dCtrlPlugin* p)
{
    GUARD_BEGIN
        if (p->shutdown) p->shutdown(p->user);
    GUARD_END
}

bool Poll(const Vrto3dCtrlPlugin* p, double dt, const Vrto3dCtrlHeadPose* head,
          Vrto3dCtrlControllerState* out)
{
    GUARD_BEGIN
        if (p->poll) p->poll(p->user, dt, head, out);
    GUARD_END
}

bool Haptic(const Vrto3dCtrlPlugin* p, uint32_t hand, float dur, float freq, float amp)
{
    GUARD_BEGIN
        if (p->on_haptic) p->on_haptic(p->user, hand, dur, freq, amp);
    GUARD_END
}

bool SettingsChanged(const Vrto3dCtrlPlugin* p)
{
    GUARD_BEGIN
        if (p->on_settings_changed) p->on_settings_changed(p->user);
    GUARD_END
}

bool GameChanged(const Vrto3dCtrlPlugin* p, const char* exe, uint32_t pid)
{
    GUARD_BEGIN
        if (p->on_game_changed) p->on_game_changed(p->user, exe, pid);
    GUARD_END
}

bool Schema(const Vrto3dCtrlPlugin* p, const char** out)
{
    GUARD_BEGIN
        *out = p->settings_schema ? p->settings_schema(p->user) : nullptr;
    GUARD_END
}

}  // namespace vrto3d::ctrl::guard
