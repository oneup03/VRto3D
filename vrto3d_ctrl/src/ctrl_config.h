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

// Controller driver configuration.
//
// Files live next to VRto3D's own configs in <Steam>/config/vrto3d/:
//   controllers.json                 global defaults (always complete)
//   <Game>.exe_controllers.json      per-game overrides, merged onto the
//                                    defaults as a JSON merge patch (only the
//                                    keys that differ are stored)
//
// Plugin settings are free-form objects under "plugins": { "<name>": {...} };
// each plugin describes its keys with a schema the OSD renders.

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace vrto3d::ctrl {

enum class EmulatedType { Knuckles, ViveController };

EmulatedType EmulatedTypeFromString(const std::string& s);
const char*  EmulatedTypeToString(EmulatedType t);

struct HandOffset {
    std::array<double, 3> pos{0.0, 0.0, 0.0};  // metres, in the aim frame
    std::array<double, 3> rot{0.0, 0.0, 0.0};  // pitch, yaw, roll degrees
};

struct CtrlConfig {
    bool enabled = false;
    std::string emulated_type = "knuckles";       // takes effect on restart
    std::array<HandOffset, 2> offset{};           // [0] left, [1] right
    float click_threshold = 0.5f;                 // analog -> click point
    bool derive_velocity = false;                 // finite-difference poses
    std::string serial_prefix = "LHR-VRTO3D";     // + "L" / "R"
    bool log_haptics = false;
    std::vector<std::string> plugin_priority{"remote", "kbm", "xinput"};
    nlohmann::json plugins = nlohmann::json::object();
};

nlohmann::json ToJson(const CtrlConfig& cfg);
// Missing or malformed keys keep their defaults.
CtrlConfig FromJson(const nlohmann::json& j);

// Keys of `target` whose value differs from `base`, recursing into objects.
// Applying the result to `base` with merge_patch reproduces `target`.
nlohmann::json DiffPatch(const nlohmann::json& base, const nlohmann::json& target);

class CtrlConfigStore {
public:
    CtrlConfigStore();

    const std::filesystem::path& Folder() const { return folder_; }
    std::filesystem::path DefaultPath() const;
    std::filesystem::path GamePath(const std::string& exe_name) const;

    // Defaults (created from built-in values when missing) with the game's
    // overrides merged on top when exe_name is non-empty.
    nlohmann::json Load(const std::string& exe_name);
    nlohmann::json LoadDefaults();

    bool SaveDefaults(const nlohmann::json& full);
    // Stores only the keys that differ from the saved defaults.
    bool SaveGame(const std::string& exe_name, const nlohmann::json& full);

    // Combined modification stamp of the defaults + the game file, used to
    // notice hand edits made while SteamVR is running.
    std::string Stamp(const std::string& exe_name) const;

private:
    static bool ReadJson(const std::filesystem::path& p, nlohmann::json& out);
    static bool WriteJson(const std::filesystem::path& p, const nlohmann::json& j);

    std::filesystem::path folder_;
};

}  // namespace vrto3d::ctrl
