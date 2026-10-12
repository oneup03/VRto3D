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
#include "ctrl_platform.h"

#include <algorithm>
#include <chrono>
#include <fstream>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include "vrto3dlib/win32_helper.hpp"
#else
#include <dlfcn.h>
#include <unistd.h>
#include "vrto3dlib/linux_helper.hpp"
#endif

namespace vrto3d::ctrl::platform {

std::string SteamInstallPath()
{
    return GetSteamInstallPath();
}

std::string ProcessName(uint32_t pid)
{
    return GetProcessName(pid);
}

bool IsGameProcessName(const std::string& name)
{
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (Skip_Processes.find(lower) != Skip_Processes.end()) return false;
#ifdef _WIN32
    return lower.find("exe") != std::string::npos;
#else
    return !lower.empty() && lower != "<unknown>";
#endif
}

uint32_t ForegroundProcessId()
{
#ifdef _WIN32
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid;
#else
    return 0;
#endif
}

uint32_t CurrentProcessId()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return (uint32_t)getpid();
#endif
}

std::filesystem::path ModuleDirectory()
{
#ifdef _WIN32
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDirectory), &self)) {
        return {};
    }
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(self, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) return {};
    return std::filesystem::path(buf).parent_path();
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&ModuleDirectory), &info) && info.dli_fname)
        return std::filesystem::path(info.dli_fname).parent_path();
    return {};
#endif
}

double NowSeconds()
{
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int VRto3DOpenTrackPort()
{
    const std::string steam = SteamInstallPath();
    if (steam.empty()) return 4242;
    std::ifstream in(std::filesystem::path(steam) / "config" / "vrto3d" / "default_config.json",
                     std::ios::binary);
    if (!in.is_open()) return 4242;
    try {
        const auto j = nlohmann::json::parse(in, nullptr, true, true);
        const int port = j.value("open_track_port", 4242);
        return (port > 0 && port < 65536) ? port : 4242;
    } catch (const std::exception&) {
        return 4242;
    }
}

}  // namespace vrto3d::ctrl::platform
