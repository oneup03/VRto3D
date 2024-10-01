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

#include "json_manager.h"
#include "driverlog.h"
#include "key_mappings.h"

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <iomanip>
#include <sstream>

// Include the nlohmann/json library
#include <nlohmann/json.hpp>

JsonManager::JsonManager() {
    vrto3dFolder = getVrto3DPath();
    createFolderIfNotExist(vrto3dFolder);
}

std::string JsonManager::getDocumentsFolderPath() {
    PWSTR path = NULL;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &path);
    if (SUCCEEDED(hr)) {
        char charPath[MAX_PATH];
        size_t convertedChars = 0;
        wcstombs_s(&convertedChars, charPath, MAX_PATH, path, _TRUNCATE);
        CoTaskMemFree(path);
        return std::string(charPath);
    }
    else {
        DriverLog("Failed to get Documents folder path\n");
    }
}

std::string JsonManager::getVrto3DPath() {
    return getDocumentsFolderPath() + "\\My Games\\vrto3d";
}

void JsonManager::createFolderIfNotExist(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        std::filesystem::create_directories(path);
    }
}

bool JsonManager::writeJsonToFile(const std::string& fileName, const nlohmann::json& jsonData) {
    std::string filePath = vrto3dFolder + "\\" + fileName;
    std::ofstream file(filePath);
    if (file.is_open()) {
        file << jsonData.dump(4); // Pretty-print the JSON with an indent of 4 spaces
        file.close();
        return true;
    }
    else {
        DriverLog("Failed to save profile: %s\n", fileName);
        return false;
    }
}

nlohmann::json JsonManager::readJsonFromFile(const std::string& fileName) {
    std::string filePath = vrto3dFolder + "\\" + fileName;
    std::ifstream file(filePath);
    if (file.is_open()) {
        nlohmann::json jsonData;
        file >> jsonData;
        file.close();
        return jsonData;
    }
    else {
        DriverLog("No profile found: %s\n", fileName);
        return {};
    }
}


//-----------------------------------------------------------------------------
// Purpose: Split a string by a delimiter
//-----------------------------------------------------------------------------
std::vector<std::string> JsonManager::split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;

    while (std::getline(ss, token, delimiter)) {
        tokens.push_back(token);
    }

    return tokens;
}


//-----------------------------------------------------------------------------
// Purpose: Load the VRto3D configuration from a JSON file
//-----------------------------------------------------------------------------
bool JsonManager::LoadConfigFromJson(const std::string& filename, StereoDisplayDriverConfiguration& config) {

    // Read the JSON configuration from the file
    nlohmann::json jsonConfig = readJsonFromFile(filename);

    if (jsonConfig.is_null() && !filename._Equal("default_config.json")) {
        return false;
    }

    try {
        // Profile settings
        config.hmd_height = jsonConfig.value("hmd_height", 1.0f);
        config.depth = jsonConfig.value("depth", 0.5f);
        config.convergence = jsonConfig.value("convergence", 0.02f);

        // Controller settings
        config.pitch_enable = jsonConfig.value("pitch_enable", false);
        config.pitch_set = config.pitch_enable;
        config.yaw_enable = jsonConfig.value("yaw_enable", false);
        config.yaw_set = config.yaw_enable;

        std::string pose_reset_key = jsonConfig.value("pose_reset_key", "VK_NUMPAD7");
        if (VirtualKeyMappings.find(pose_reset_key) != VirtualKeyMappings.end()) {
            config.pose_reset_key = VirtualKeyMappings[pose_reset_key];
            config.reset_xinput = false;
        }
        else if (XInputMappings.find(pose_reset_key) != XInputMappings.end() || pose_reset_key.find('+') != std::string::npos) {
            config.pose_reset_key = 0x0;
            auto hotkeys = split(pose_reset_key, '+');
            for (const auto& hotkey : hotkeys) {
                if (XInputMappings.find(hotkey) != XInputMappings.end()) {
                    config.pose_reset_key |= XInputMappings[hotkey];
                }
            }
            config.reset_xinput = true;
        }
        config.pose_reset = true;

        std::string ctrl_toggle_key = jsonConfig.value("ctrl_toggle_key", "XINPUT_GAMEPAD_RIGHT_THUMB");
        if (VirtualKeyMappings.find(ctrl_toggle_key) != VirtualKeyMappings.end()) {
            config.ctrl_toggle_key = VirtualKeyMappings[ctrl_toggle_key];
            config.ctrl_xinput = false;
        }
        else if (XInputMappings.find(ctrl_toggle_key) != XInputMappings.end() || ctrl_toggle_key.find('+') != std::string::npos) {
            config.ctrl_toggle_key = 0x0;
            auto hotkeys = split(ctrl_toggle_key, '+');
            for (const auto& hotkey : hotkeys) {
                if (XInputMappings.find(hotkey) != XInputMappings.end()) {
                    config.ctrl_toggle_key |= XInputMappings[hotkey];
                }
            }
            config.ctrl_xinput = true;
        }

        std::string ctrl_toggle_type = jsonConfig.value("ctrl_toggle_type", "toggle");
        config.ctrl_type = KeyBindTypes[ctrl_toggle_type];

        config.pitch_radius = jsonConfig.value("pitch_radius", 0.0f);
        config.ctrl_deadzone = jsonConfig.value("ctrl_deadzone", 0.05f);
        config.ctrl_sensitivity = jsonConfig.value("ctrl_sensitivity", 1.0f);

        // Read user binds from user_settings array
        const auto& user_settings_array = jsonConfig.at("user_settings");

        // Resize vectors based on the size of the user_settings array
        size_t num_user_settings = user_settings_array.size();
        config.user_load_key.resize(num_user_settings);
        config.user_store_key.resize(num_user_settings);
        config.user_key_type.resize(num_user_settings);
        config.user_depth.resize(num_user_settings);
        config.user_convergence.resize(num_user_settings);
        config.prev_depth.resize(num_user_settings);
        config.prev_convergence.resize(num_user_settings);
        config.was_held.resize(num_user_settings);
        config.load_xinput.resize(num_user_settings);
        config.sleep_count.resize(num_user_settings);

        for (size_t i = 0; i < num_user_settings; ++i) {
            const auto& user_setting = user_settings_array.at(i);

            std::string user_load_key = user_setting.value("user_load_key", "VK_NUMPAD1");
            if (VirtualKeyMappings.find(user_load_key) != VirtualKeyMappings.end()) {
                config.user_load_key[i] = VirtualKeyMappings[user_load_key];
                config.load_xinput[i] = false;
            }
            else if (XInputMappings.find(user_load_key) != XInputMappings.end() || user_load_key.find('+') != std::string::npos) {
                config.user_load_key[i] = 0x0;
                auto hotkeys = split(user_load_key, '+');
                for (const auto& hotkey : hotkeys) {
                    if (XInputMappings.find(hotkey) != XInputMappings.end()) {
                        config.user_load_key[i] |= XInputMappings[hotkey];
                    }
                }
                config.load_xinput[i] = true;
            }

            std::string user_store_key = user_setting.value("user_store_key", "VK_NUMPAD4");
            if (VirtualKeyMappings.find(user_store_key) != VirtualKeyMappings.end()) {
                config.user_store_key[i] = VirtualKeyMappings[user_store_key];
            }

            std::string user_key_type = user_setting.value("user_key_type", "switch");
            if (KeyBindTypes.find(user_key_type) != KeyBindTypes.end()) {
                config.user_key_type[i] = KeyBindTypes[user_key_type];
            }

            config.user_depth[i] = user_setting.value("user_depth", 0.5f);
            config.user_convergence[i] = user_setting.value("user_convergence", 0.02f);
        }

    }
    catch (const nlohmann::json::exception& e) {
        DriverLog("Error reading config from %s: %s\n", filename, e.what());
    }
    if (jsonConfig.is_null()) {
        return false;
    }

    return true;
}

