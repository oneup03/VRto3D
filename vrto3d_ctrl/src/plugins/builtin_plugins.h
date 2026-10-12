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

// Plugins compiled into driver_vrto3d_ctrl. They implement the same C ABI as
// external plugin DLLs and go through the same loader.

#include <vector>

#include "vrto3d_ctrl_plugin.h"

extern "C" int vrto3d_builtin_xinput_entry(uint32_t abi, Vrto3dCtrlPlugin* out);
extern "C" int vrto3d_builtin_remote_entry(uint32_t abi, Vrto3dCtrlPlugin* out);
#ifdef _WIN32
extern "C" int vrto3d_builtin_kbm_entry(uint32_t abi, Vrto3dCtrlPlugin* out);
#endif

namespace vrto3d::ctrl::builtin {

struct Entry {
    const char* name;
    Vrto3dCtrlPluginEntryFn entry;
};

inline std::vector<Entry> Plugins()
{
    return {
        {"remote", &vrto3d_builtin_remote_entry},
#ifdef _WIN32
        {"kbm", &vrto3d_builtin_kbm_entry},
#endif
        {"xinput", &vrto3d_builtin_xinput_entry},
    };
}

}  // namespace vrto3d::ctrl::builtin
