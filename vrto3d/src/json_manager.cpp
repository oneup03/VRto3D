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

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <filesystem>

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
