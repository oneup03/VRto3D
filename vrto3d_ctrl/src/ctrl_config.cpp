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
#include "ctrl_config.h"

#include <fstream>
#include <sstream>
#include <system_error>

#include "ctrl_platform.h"
#include "vrto3dlib/debug_log.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace vrto3d::ctrl {

namespace {

constexpr const char* kDefaultFile = "controllers.json";
constexpr const char* kGameSuffix  = "_controllers.json";

template <typename T>
void Read(const json& j, const char* key, T& out)
{
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    try {
        out = it->get<T>();
    } catch (const std::exception&) {
        LOG() << "controllers.json: ignoring malformed '" << key << "'";
    }
}

json OffsetToJson(const HandOffset& o)
{
    return json{{"pos", o.pos}, {"rot", o.rot}};
}

void OffsetFromJson(const json& j, const char* key, HandOffset& o)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_object()) return;
    Read(*it, "pos", o.pos);
    Read(*it, "rot", o.rot);
}

}  // namespace

EmulatedType EmulatedTypeFromString(const std::string& s)
{
    return s == "vive_controller" ? EmulatedType::ViveController : EmulatedType::Knuckles;
}

const char* EmulatedTypeToString(EmulatedType t)
{
    return t == EmulatedType::ViveController ? "vive_controller" : "knuckles";
}

json ToJson(const CtrlConfig& c)
{
    json j = json::object();
    j["enabled"] = c.enabled;
    j["emulated_type"] = c.emulated_type;
    j["left_offset"] = OffsetToJson(c.offset[0]);
    j["right_offset"] = OffsetToJson(c.offset[1]);
    j["click_threshold"] = c.click_threshold;
    j["derive_velocity"] = c.derive_velocity;
    j["serial_prefix"] = c.serial_prefix;
    j["log_haptics"] = c.log_haptics;
    j["plugin_priority"] = c.plugin_priority;
    j["plugins"] = c.plugins.is_object() ? c.plugins : json::object();
    return j;
}

CtrlConfig FromJson(const json& j)
{
    CtrlConfig c;
    if (!j.is_object()) return c;
    Read(j, "enabled", c.enabled);
    Read(j, "emulated_type", c.emulated_type);
    OffsetFromJson(j, "left_offset", c.offset[0]);
    OffsetFromJson(j, "right_offset", c.offset[1]);
    Read(j, "click_threshold", c.click_threshold);
    Read(j, "derive_velocity", c.derive_velocity);
    Read(j, "serial_prefix", c.serial_prefix);
    Read(j, "log_haptics", c.log_haptics);
    Read(j, "plugin_priority", c.plugin_priority);
    auto it = j.find("plugins");
    if (it != j.end() && it->is_object()) c.plugins = *it;

    if (c.click_threshold < 0.05f) c.click_threshold = 0.05f;
    if (c.click_threshold > 0.95f) c.click_threshold = 0.95f;
    // Normalise so the OSD combo always matches one of its entries.
    c.emulated_type = EmulatedTypeToString(EmulatedTypeFromString(c.emulated_type));
    return c;
}

json DiffPatch(const json& base, const json& target)
{
    json out = json::object();
    if (!target.is_object()) return out;
    for (auto it = target.begin(); it != target.end(); ++it) {
        auto b = base.is_object() ? base.find(it.key()) : base.end();
        if (base.is_object() && b != base.end()) {
            if (b->is_object() && it->is_object()) {
                json sub = DiffPatch(*b, *it);
                if (!sub.empty()) out[it.key()] = std::move(sub);
            } else if (*b != *it) {
                out[it.key()] = *it;
            }
        } else {
            out[it.key()] = *it;
        }
    }
    return out;
}

CtrlConfigStore::CtrlConfigStore()
{
    const std::string steam = platform::SteamInstallPath();
    folder_ = steam.empty() ? fs::path(".") : fs::path(steam) / "config" / "vrto3d";
}

fs::path CtrlConfigStore::DefaultPath() const { return folder_ / kDefaultFile; }

fs::path CtrlConfigStore::GamePath(const std::string& exe_name) const
{
    return folder_ / (exe_name + kGameSuffix);
}

bool CtrlConfigStore::ReadJson(const fs::path& p, json& out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    try {
        out = json::parse(in, nullptr, true, true);  // allow comments
        return true;
    } catch (const std::exception& e) {
        LOG() << "Failed to parse " << p.string() << ": " << e.what();
        return false;
    }
}

bool CtrlConfigStore::WriteJson(const fs::path& p, const json& j)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            LOG() << "Failed to open " << tmp.string() << " for writing";
            return false;
        }
        out << j.dump(4) << "\n";
        if (!out.good()) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        LOG() << "Failed to replace " << p.string() << ": " << ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

json CtrlConfigStore::LoadDefaults()
{
    json defaults = ToJson(CtrlConfig{});
    json file;
    if (ReadJson(DefaultPath(), file) && file.is_object()) {
        defaults.merge_patch(file);
    } else if (!fs::exists(DefaultPath())) {
        WriteJson(DefaultPath(), defaults);
        LOG() << "Created " << DefaultPath().string();
    }
    return defaults;
}

json CtrlConfigStore::Load(const std::string& exe_name)
{
    json merged = LoadDefaults();
    if (!exe_name.empty()) {
        json game;
        if (ReadJson(GamePath(exe_name), game) && game.is_object()) {
            merged.merge_patch(game);
            LOG() << "Applied game controller profile " << GamePath(exe_name).string();
        }
    }
    return merged;
}

bool CtrlConfigStore::SaveDefaults(const json& full)
{
    return WriteJson(DefaultPath(), full);
}

bool CtrlConfigStore::SaveGame(const std::string& exe_name, const json& full)
{
    if (exe_name.empty()) return false;
    const json patch = DiffPatch(LoadDefaults(), full);
    return WriteJson(GamePath(exe_name), patch);
}

std::string CtrlConfigStore::Stamp(const std::string& exe_name) const
{
    std::ostringstream s;
    auto add = [&](const fs::path& p) {
        std::error_code ec;
        auto t = fs::last_write_time(p, ec);
        s << (ec ? 0LL : (long long)t.time_since_epoch().count()) << ";";
    };
    add(DefaultPath());
    if (!exe_name.empty()) add(GamePath(exe_name));
    return s.str();
}

}  // namespace vrto3d::ctrl
