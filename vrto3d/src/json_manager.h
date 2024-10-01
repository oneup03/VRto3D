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
#include <nlohmann/json.hpp>

class JsonManager {
public:
    JsonManager();
    
    bool writeJsonToFile(const std::string& fileName, const nlohmann::json& jsonData);
    nlohmann::json readJsonFromFile(const std::string& fileName);

private:
    std::string vrto3dFolder;
    std::string getDocumentsFolderPath();
    std::string getVrto3DPath();
    void createFolderIfNotExist(const std::string& path);
};
