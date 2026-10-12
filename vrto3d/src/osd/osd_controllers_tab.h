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

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

namespace vrto3d::osd {

class OsdInput;

// "Controllers" tab of the OSD menu. Edits the emulated controller driver's
// live config through ctrl_bridge; every plugin's settings are drawn from the
// JSON schema the plugin publishes, so new plugins get UI for free.
class ControllersTab {
public:
    // Facts about VRto3D's own config the tab warns about.
    struct Hints {
        bool opentrack_enabled = false;
        bool xinput_head_look = false;  // pitch_enable || yaw_enable
    };

    void Draw(OsdInput& input, const Hints& hints);

private:
    void Fetch(bool force);
    void Toast(std::string msg);
    bool DrawPluginList(nlohmann::json& cfg);
    bool DrawSetting(const nlohmann::json& item, nlohmann::json& values, const std::string& id,
                     OsdInput& input);
    bool DrawTable(const nlohmann::json& item, nlohmann::json& values, const std::string& id,
                   OsdInput& input);
    bool DrawKeybind(const char* label, nlohmann::json& value, const std::string& id, OsdInput& input,
                     bool compact);

    nlohmann::json state_ = nlohmann::json::object();
    bool have_state_ = false;
    std::chrono::steady_clock::time_point next_fetch_{};

    // Click-to-capture key picker: id of the widget waiting for a key, and the
    // captured name once the user pressed one.
    std::string capture_id_;
    std::string captured_;
    bool capture_ready_ = false;

    std::string toast_;
    std::chrono::steady_clock::time_point toast_until_{};
};

}  // namespace vrto3d::osd
