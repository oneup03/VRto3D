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
#include "cfg_store.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace cfgmgr {

namespace {

std::string ToUtf8(const fs::path& p) { return p.u8string(); }

fs::path FromUtf8(const std::string& s) { return fs::u8path(s); }

std::string LastErrorText(const char* what) {
    std::ostringstream ss;
    ss << what << " (error " << GetLastError() << ")";
    return ss.str();
}

bool ReadFileText(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Parse JSON, tolerating comments (some driver manifests carry them).
bool ParseJson(const std::string& text, Json& out) {
    out = Json::parse(text, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    return !out.is_discarded();
}

bool ParseJsonFile(const fs::path& p, Json& out) {
    std::string text;
    return ReadFileText(p, text) && ParseJson(text, out);
}

std::wstring RegReadString(HKEY root, const wchar_t* sub, const wchar_t* value) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, sub, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

fs::path LocalAppData() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return buf;
}

// %LOCALAPPDATA%\openvr\openvrpaths.vrpath — SteamVR's own record of where
// its runtime lives and which external driver folders are registered.
bool LoadVrPath(Json& out) {
    const fs::path base = LocalAppData();
    return !base.empty() && ParseJsonFile(base / "openvr" / "openvrpaths.vrpath", out);
}

bool IsSteamVRDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p / "drivers", ec);
}

fs::path FindSteamVR(const fs::path& steam) {
    Json vrpath;
    if (LoadVrPath(vrpath) && vrpath.contains("runtime") && vrpath["runtime"].is_array()) {
        for (const auto& r : vrpath["runtime"]) {
            if (!r.is_string()) continue;
            fs::path p = FromUtf8(r.get<std::string>());
            if (IsSteamVRDir(p)) return p;
        }
    }

    // libraryfolders.vdf: `"path"   "D:\\SteamLibrary"` lines, backslashes escaped.
    std::vector<fs::path> libs{ steam };
    std::string vdf;
    if (ReadFileText(steam / "steamapps" / "libraryfolders.vdf", vdf)) {
        std::istringstream lines(vdf);
        std::string line;
        while (std::getline(lines, line)) {
            const size_t key = line.find("\"path\"");
            if (key == std::string::npos) continue;
            const size_t a = line.find('"', key + 6);
            const size_t b = line.rfind('"');
            if (a == std::string::npos || b <= a) continue;
            std::string raw = line.substr(a + 1, b - a - 1), path;
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') ++i;
                path += raw[i];
            }
            libs.push_back(FromUtf8(path));
        }
    }
    for (const auto& lib : libs) {
        fs::path p = lib / "steamapps" / "common" / "SteamVR";
        if (IsSteamVRDir(p)) return p;
    }
    return {};
}

bool ReadDriver(const fs::path& dir, bool external, DriverInfo& out) {
    Json manifest;
    if (!ParseJsonFile(dir / "driver.vrdrivermanifest", manifest) || !manifest.is_object())
        return false;
    out.dir = dir;
    out.external = external;
    out.name = manifest.value("name", std::string());
    if (out.name.empty()) out.name = ToUtf8(dir.filename());

    Json defaults;
    out.default_enable = true;
    if (ParseJsonFile(dir / "resources" / "settings" / "default.vrsettings", defaults)) {
        const std::string section = "driver_" + out.name;
        if (defaults.contains(section) && defaults[section].is_object()) {
            const Json& s = defaults[section];
            if (s.contains("enable") && s["enable"].is_boolean())
                out.default_enable = s["enable"].get<bool>();
        }
    }
    return true;
}

std::string FormatFileTime(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    FILETIME local{};
    SYSTEMTIME st{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad) ||
        !FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local) ||
        !FileTimeToSystemTime(&local, &st)) {
        return {};
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

std::string NowStamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u%02u%02u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::vector<ProfileInfo> ListVrSettingsIn(const fs::path& dir) {
    std::vector<ProfileInfo> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".vrsettings") continue;
        out.push_back({ ToUtf8(it->path().stem()), it->path(), FormatFileTime(it->path()) });
    }
    return out;
}

// Copy `src` over `dst` via a sibling temp file so a failure never leaves a
// half-written steamvr.vrsettings behind.
bool AtomicCopy(const fs::path& src, const fs::path& dst, std::string& err) {
    const fs::path tmp = fs::path(dst).concat(L".cfgmgr.tmp");
    if (!CopyFileW(src.c_str(), tmp.c_str(), FALSE)) {
        err = LastErrorText("Copy failed");
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = LastErrorText("Replace failed");
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void EraseKey(Json& doc, const std::string& section, const char* key) {
    auto it = doc.find(section);
    if (it == doc.end() || !it->is_object()) return;
    it->erase(key);
    if (it->empty()) doc.erase(section);
}

void SetKey(Json& doc, const std::string& section, const char* key, Json value) {
    if (!doc.contains(section) || !doc[section].is_object()) doc[section] = Json::object();
    doc[section][key] = std::move(value);
}

const Json* FindKey(const Json& doc, const std::string& section, const char* key) {
    auto s = doc.find(section);
    if (s == doc.end() || !s->is_object()) return nullptr;
    auto k = s->find(key);
    return k == s->end() ? nullptr : &*k;
}

}  // namespace


bool LocatePaths(Paths& out, std::string& err) {
    std::wstring steam = RegReadString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty())
        steam = RegReadString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) {
        err = "Steam was not found in the registry.";
        return false;
    }
    std::replace(steam.begin(), steam.end(), L'/', L'\\');

    out.steam       = fs::path(steam);
    out.steamvr     = FindSteamVR(out.steam);
    out.vrsettings  = out.steam / "config" / "steamvr.vrsettings";
    out.store       = out.steam / "config" / "steamvr_cfg_mgr";
    out.autobackups = out.store / "autobackups";
    std::error_code ec;
    fs::create_directories(out.autobackups, ec);
    return true;
}

std::vector<DriverInfo> ScanDrivers(const Paths& paths) {
    std::vector<DriverInfo> out;
    std::error_code ec;
    if (!paths.steamvr.empty()) {
        for (fs::directory_iterator it(paths.steamvr / "drivers", ec), end; !ec && it != end; it.increment(ec)) {
            DriverInfo d;
            if (it->is_directory(ec) && ReadDriver(it->path(), false, d)) out.push_back(std::move(d));
        }
    }
    std::sort(out.begin(), out.end(), [](const DriverInfo& a, const DriverInfo& b) { return a.name < b.name; });

    Json vrpath;
    if (LoadVrPath(vrpath) && vrpath.contains("external_drivers") && vrpath["external_drivers"].is_array()) {
        for (const auto& e : vrpath["external_drivers"]) {
            DriverInfo d;
            if (e.is_string() && ReadDriver(FromUtf8(e.get<std::string>()), true, d)) {
                const bool dup = std::any_of(out.begin(), out.end(),
                    [&](const DriverInfo& x) { return x.name == d.name; });
                if (!dup) out.push_back(std::move(d));
            }
        }
    }
    return out;
}

std::vector<ProfileInfo> ListProfiles(const Paths& paths) {
    auto out = ListVrSettingsIn(paths.store);
    std::sort(out.begin(), out.end(), [](const ProfileInfo& a, const ProfileInfo& b) {
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

std::vector<ProfileInfo> ListAutoBackups(const Paths& paths) {
    auto out = ListVrSettingsIn(paths.autobackups);
    // Names start with a sortable timestamp; newest first.
    std::sort(out.begin(), out.end(), [](const ProfileInfo& a, const ProfileInfo& b) { return a.name > b.name; });
    return out;
}

bool IsSteamVRRunning() {
    static const wchar_t* const kProcs[] = {
        L"vrserver.exe", L"vrmonitor.exe", L"vrcompositor.exe", L"vrstartup.exe",
    };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{ sizeof(pe) };
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        for (const wchar_t* name : kProcs) {
            if (_wcsicmp(pe.szExeFile, name) == 0) { found = true; break; }
        }
    }
    CloseHandle(snap);
    return found;
}

unsigned long long FileStamp(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<unsigned long long>(fad.ftLastWriteTime.dwHighDateTime) << 32)
         | fad.ftLastWriteTime.dwLowDateTime;
}

bool LoadJsonFile(const fs::path& p, Json& out, std::string& err) {
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        out = Json::object();
        return true;
    }
    std::string text;
    if (!ReadFileText(p, text)) {
        err = "Could not read " + ToUtf8(p);
        return false;
    }
    if (!ParseJson(text, out) || !out.is_object()) {
        err = ToUtf8(p.filename()) + " is not valid JSON.";
        return false;
    }
    return true;
}

bool WriteVrSettings(const Paths& paths, const Json& doc, std::string& err) {
    std::error_code ec;
    fs::create_directories(paths.vrsettings.parent_path(), ec);
    const fs::path tmp = fs::path(paths.vrsettings).concat(L".cfgmgr.tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "Could not write " + ToUtf8(tmp);
            return false;
        }
        // SteamVR writes 3-space indentation; match it to keep diffs readable.
        f << doc.dump(3, ' ', false, Json::error_handler_t::replace) << "\n";
        if (!f) {
            err = "Write to " + ToUtf8(tmp) + " failed";
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), paths.vrsettings.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = LastErrorText("Replace of steamvr.vrsettings failed");
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool AutoBackup(const Paths& paths, const std::string& reason, std::string& err) {
    std::error_code ec;
    if (!fs::exists(paths.vrsettings, ec)) return true;
    fs::create_directories(paths.autobackups, ec);
    const fs::path dst = paths.autobackups / FromUtf8(NowStamp() + " " + reason + ".vrsettings");
    if (!CopyFileW(paths.vrsettings.c_str(), dst.c_str(), FALSE)) {
        err = LastErrorText("Automatic backup failed");
        return false;
    }
    auto backups = ListAutoBackups(paths);  // newest first
    for (size_t i = kMaxAutoBackups; i < backups.size(); ++i)
        DeleteFileW(backups[i].path.c_str());
    return true;
}

std::string SanitizeProfileName(const std::string& name) {
    std::string out;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        out += (u < 32 || strchr("\\/:*?\"<>|", c)) ? '_' : c;
    }
    const size_t a = out.find_first_not_of(" .");
    const size_t b = out.find_last_not_of(" .");
    return a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
}

bool SaveProfile(const Paths& paths, const std::string& name, std::string& err) {
    const std::string clean = SanitizeProfileName(name);
    if (clean.empty()) {
        err = "Enter a profile name.";
        return false;
    }
    std::error_code ec;
    if (!fs::exists(paths.vrsettings, ec)) {
        err = "There is no steamvr.vrsettings to save yet.";
        return false;
    }
    fs::create_directories(paths.store, ec);
    return AtomicCopy(paths.vrsettings, paths.store / FromUtf8(clean + ".vrsettings"), err);
}

bool RestoreFile(const Paths& paths, const fs::path& src, std::string& err) {
    Json probe;
    if (!LoadJsonFile(src, probe, err)) return false;
    if (!AutoBackup(paths, "before restore", err)) return false;
    return AtomicCopy(src, paths.vrsettings, err);
}

bool DeleteProfile(const ProfileInfo& profile, std::string& err) {
    if (!DeleteFileW(profile.path.c_str())) {
        err = LastErrorText("Delete failed");
        return false;
    }
    return true;
}

bool ResetVrSettings(const Paths& paths, std::string& err) {
    std::error_code ec;
    if (!fs::exists(paths.vrsettings, ec)) return true;
    if (!AutoBackup(paths, "before reset", err)) return false;
    if (!DeleteFileW(paths.vrsettings.c_str())) {
        err = LastErrorText("Delete of steamvr.vrsettings failed");
        return false;
    }
    return true;
}

std::string LoadActiveProfile(const Paths& paths) {
    Json state;
    if (!ParseJsonFile(paths.store / "state.json", state) || !state.is_object()) return {};
    return state.value("active_profile", std::string());
}

void SaveActiveProfile(const Paths& paths, const std::string& name) {
    Json state = Json::object();
    state["active_profile"] = name;
    std::ofstream f(paths.store / "state.json", std::ios::binary | std::ios::trunc);
    f << state.dump(3) << "\n";
}

// ---- steamvr.vrsettings accessors ------------------------------------------

std::string GetForcedDriver(const Json& doc) {
    const Json* v = FindKey(doc, "steamvr", "forcedDriver");
    return v && v->is_string() ? v->get<std::string>() : std::string();
}

void SetForcedDriver(Json& doc, const std::string& name) {
    if (name.empty()) EraseKey(doc, "steamvr", "forcedDriver");
    else              SetKey(doc, "steamvr", "forcedDriver", name);
}

bool GetSteamVRBool(const Json& doc, const char* key, bool def) {
    const Json* v = FindKey(doc, "steamvr", key);
    return v && v->is_boolean() ? v->get<bool>() : def;
}

void SetSteamVRBool(Json& doc, const char* key, bool value, bool def) {
    if (value == def) EraseKey(doc, "steamvr", key);
    else              SetKey(doc, "steamvr", key, value);
}

bool DriverEnabled(const Json& doc, const DriverInfo& d) {
    const Json* v = FindKey(doc, "driver_" + d.name, "enable");
    return v && v->is_boolean() ? v->get<bool>() : d.default_enable;
}

void SetDriverEnabled(Json& doc, const DriverInfo& d, bool enabled) {
    const std::string section = "driver_" + d.name;
    if (enabled == d.default_enable) EraseKey(doc, section, "enable");
    else                             SetKey(doc, section, "enable", enabled);
}

bool DriverBlockedBySafeMode(const Json& doc, const DriverInfo& d) {
    const Json* v = FindKey(doc, "driver_" + d.name, "blocked_by_safe_mode");
    return v && v->is_boolean() && v->get<bool>();
}

bool SafeModeActive(const Json& doc) {
    if (GetSteamVRBool(doc, "enableSafeMode", false)) return true;
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (it.key().rfind("driver_", 0) != 0 || !it->is_object()) continue;
        auto b = it->find("blocked_by_safe_mode");
        if (b != it->end() && b->is_boolean() && b->get<bool>()) return true;
    }
    return false;
}

void ClearSafeMode(Json& doc) {
    EraseKey(doc, "steamvr", "enableSafeMode");
    std::vector<std::string> sections;
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (it.key().rfind("driver_", 0) == 0 && it->is_object() && it->contains("blocked_by_safe_mode"))
            sections.push_back(it.key());
    }
    for (const auto& s : sections) EraseKey(doc, s, "blocked_by_safe_mode");
}

}  // namespace cfgmgr
