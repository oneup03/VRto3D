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

// Everything the SteamVR Config Manager does to disk: locating Steam/SteamVR,
// scanning installed drivers, editing <Steam>\config\steamvr.vrsettings, and
// keeping named copies of that file ("profiles") so users can swap between
// e.g. a VRto3D setup and their normal headset setup.
//
// The UI layer (main.cpp) never touches the filesystem directly.

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cfgmgr {

namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

struct Paths {
    fs::path steam;        // <Steam>
    fs::path steamvr;      // <SteamVR runtime>, may be empty if not found
    fs::path vrsettings;   // <Steam>\config\steamvr.vrsettings
    fs::path store;        // <Steam>\config\steamvr_cfg_mgr (profiles)
    fs::path autobackups;  // <store>\autobackups
};

// Resolve Steam from the registry, SteamVR from openvrpaths.vrpath (falling
// back to libraryfolders.vdf). Returns false only when Steam itself is missing.
bool LocatePaths(Paths& out, std::string& err);

struct DriverInfo {
    std::string name;            // manifest "name" (settings section is driver_<name>)
    fs::path    dir;
    bool        external = false;   // registered via openvrpaths.vrpath, not in SteamVR\drivers
    bool        default_enable = true; // driver_<name>.enable from its own default.vrsettings
};

// Builtin drivers (SteamVR\drivers\*) followed by externally registered ones.
std::vector<DriverInfo> ScanDrivers(const Paths& paths);

struct ProfileInfo {
    std::string name;      // UTF-8, file stem
    fs::path    path;
    std::string modified;  // local time, "yyyy-mm-dd hh:mm"
};

std::vector<ProfileInfo> ListProfiles(const Paths& paths);
std::vector<ProfileInfo> ListAutoBackups(const Paths& paths);

// vrserver/vrmonitor/vrcompositor/vrstartup. SteamVR holds the settings in
// memory and writes them back on exit, so edits made while it runs are lost.
bool IsSteamVRRunning();

// Last-write time of a file as an opaque comparable value (0 if missing).
unsigned long long FileStamp(const fs::path& p);

// Parse a vrsettings-style JSON file. A missing file loads as {} and succeeds.
bool LoadJsonFile(const fs::path& p, Json& out, std::string& err);

// Write `doc` over steamvr.vrsettings via temp file + atomic replace.
bool WriteVrSettings(const Paths& paths, const Json& doc, std::string& err);

// Copy the current steamvr.vrsettings into autobackups (pruned to the newest
// kMaxAutoBackups). Succeeds trivially when there is no file to back up.
constexpr size_t kMaxAutoBackups = 20;
bool AutoBackup(const Paths& paths, const std::string& reason, std::string& err);

// Profile operations. Names are UTF-8 and sanitized for the filesystem.
std::string SanitizeProfileName(const std::string& name);
bool SaveProfile(const Paths& paths, const std::string& name, std::string& err);
bool RestoreFile(const Paths& paths, const fs::path& src, std::string& err);
bool DeleteProfile(const ProfileInfo& profile, std::string& err);
// Auto-backup, then remove steamvr.vrsettings so SteamVR starts from defaults.
bool ResetVrSettings(const Paths& paths, std::string& err);

// Name of the profile last saved or restored, persisted in <store>\state.json.
std::string LoadActiveProfile(const Paths& paths);
void        SaveActiveProfile(const Paths& paths, const std::string& name);

// ---- steamvr.vrsettings accessors -----------------------------------------
// Setters erase a key when it is set back to SteamVR's (or the driver's)
// default, and drop sections left empty, so the file only carries real
// overrides.

std::string GetForcedDriver(const Json& doc);
void        SetForcedDriver(Json& doc, const std::string& name);   // "" = auto

bool GetSteamVRBool(const Json& doc, const char* key, bool def);
void SetSteamVRBool(Json& doc, const char* key, bool value, bool def);

bool DriverEnabled(const Json& doc, const DriverInfo& d);
void SetDriverEnabled(Json& doc, const DriverInfo& d, bool enabled);
bool DriverBlockedBySafeMode(const Json& doc, const DriverInfo& d);

bool SafeModeActive(const Json& doc);
// Clear steamvr.enableSafeMode and every driver_*.blocked_by_safe_mode flag.
void ClearSafeMode(Json& doc);

}  // namespace cfgmgr
