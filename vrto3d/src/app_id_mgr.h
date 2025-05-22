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

#include <string>
#include <unordered_set>
#include <vector>

class AppIdMgr {
public:
    AppIdMgr();
    std::vector<std::string> GetSteamAppIDs();

private:
    void SetSteamInstallPath();

    std::unordered_set<std::string> excluded_app_keys_ = {
        "system.systemui",
        "steam.overlay.250820",
        "system.generated.steamwebhelper.exe",
        "steam.client",
        "openvr.component.vrcompositor",
        "system.keyboard",
        "system.vrwebhelper.controllerbinding"
    };

    std::string steam_path_;
};
