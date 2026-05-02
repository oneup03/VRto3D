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

#ifdef _WIN32

#include <string>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vrto3d {
namespace display_utils {

struct DisplayPositionSnapshot {
    std::wstring deviceName;
    POINTL       position;
};

inline std::vector<DisplayPositionSnapshot> SnapshotDisplayPositions()
{
    std::vector<DisplayPositionSnapshot> snapshots;
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
            dd = {}; dd.cb = sizeof(dd); continue;
        }
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsExW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0)) {
            DisplayPositionSnapshot snap;
            snap.deviceName = dd.DeviceName;
            snap.position   = dm.dmPosition;
            snapshots.push_back(snap);
        }
        dd = {}; dd.cb = sizeof(dd);
    }
    return snapshots;
}

inline void RestoreDisplayPositions(const std::vector<DisplayPositionSnapshot>& snapshots)
{
    if (snapshots.empty()) return;

    UINT32 numPaths = 0, numModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPaths, &numModes) != ERROR_SUCCESS)
        return;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPaths, paths.data(),
                           &numModes, modes.data(), nullptr) != ERROR_SUCCESS)
        return;
    paths.resize(numPaths);
    modes.resize(numModes);

    std::unordered_map<std::wstring, POINTL> posMap;
    for (const auto& snap : snapshots) posMap[snap.deviceName] = snap.position;

    for (auto& mode : modes) {
        if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName = {};
        srcName.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        srcName.header.size      = sizeof(srcName);
        srcName.header.adapterId = mode.adapterId;
        srcName.header.id        = mode.id;
        if (DisplayConfigGetDeviceInfo(&srcName.header) != ERROR_SUCCESS) continue;
        auto it = posMap.find(srcName.viewGdiDeviceName);
        if (it == posMap.end()) continue;
        mode.sourceMode.position.x = it->second.x;
        mode.sourceMode.position.y = it->second.y;
    }

    SetDisplayConfig(numPaths, paths.data(), numModes, modes.data(),
                     SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_NO_OPTIMIZATION);
}

// Pump messages and sleep to let a modeset settle.
inline void WaitForModesetSettle(DWORD timeoutMs)
{
    DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

}  // namespace display_utils
}  // namespace vrto3d

#endif  // _WIN32
