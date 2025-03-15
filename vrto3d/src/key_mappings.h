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
#ifndef VIRTUAL_KEY_MAPPINGS_H
#define VIRTUAL_KEY_MAPPINGS_H

#define WIN32_LEAN_AND_MEAN

#include <string>
#include <unordered_map>
#include <windows.h>
#include <XInput.h>

// Create a mapping between string names and virtual key codes
static std::unordered_map<std::string, int> VirtualKeyMappings = {
    // Mouse buttons
    {"VK_LMOUSE", VK_LBUTTON},
    {"VK_RMOUSE", VK_RBUTTON},
    {"VK_MMOUSE", VK_MBUTTON},
    {"VK_MOUSE4", VK_XBUTTON1},
    {"VK_MOUSE5", VK_XBUTTON2},
    // Keyboard keys
    {"VK_BACKSPACE", VK_BACK},
    {"VK_TAB", VK_TAB},
    {"VK_SHIFT", VK_SHIFT},
    {"VK_CONTROL", VK_CONTROL},
    {"VK_MENU", VK_MENU},
    {"VK_PAUSE", VK_PAUSE},
    {"VK_CAPS", VK_CAPITAL},
    {"VK_ESCAPE", VK_ESCAPE},
    {"VK_SPACE", VK_SPACE},
    {"VK_PGUP", VK_PRIOR},
    {"VK_PGDWN", VK_NEXT},
    {"VK_END", VK_END},
    {"VK_HOME", VK_HOME},
    {"VK_LEFT", VK_LEFT},
    {"VK_UP", VK_UP},
    {"VK_RIGHT", VK_RIGHT},
    {"VK_DOWN", VK_DOWN},
    {"VK_SNAPSHOT", VK_SNAPSHOT},
    {"VK_INSERT", VK_INSERT},
    {"VK_DELETE", VK_DELETE},
    {"VK_NUMPAD0", VK_NUMPAD0},
    {"VK_NUMPAD1", VK_NUMPAD1},
    {"VK_NUMPAD2", VK_NUMPAD2},
    {"VK_NUMPAD3", VK_NUMPAD3},
    {"VK_NUMPAD4", VK_NUMPAD4},
    {"VK_NUMPAD5", VK_NUMPAD5},
    {"VK_NUMPAD6", VK_NUMPAD6},
    {"VK_NUMPAD7", VK_NUMPAD7},
    {"VK_NUMPAD8", VK_NUMPAD8},
    {"VK_NUMPAD9", VK_NUMPAD9},
    {"VK_MULTIPLY", VK_MULTIPLY},
    {"VK_ADD", VK_ADD},
    {"VK_SUBTRACT", VK_SUBTRACT},
    {"VK_DECIMAL", VK_DECIMAL},
    {"VK_DIVIDE", VK_DIVIDE},
    {"VK_F1", VK_F1},
    {"VK_F2", VK_F2},
    {"VK_F3", VK_F3},
    {"VK_F4", VK_F4},
    {"VK_F5", VK_F5},
    {"VK_F6", VK_F6},
    {"VK_F7", VK_F7},
    {"VK_F8", VK_F8},
    {"VK_F9", VK_F9},
    {"VK_F10", VK_F10},
    {"VK_F11", VK_F11},
    {"VK_F12", VK_F12},
    {"VK_F13", VK_F13},
    {"VK_F14", VK_F14},
    {"VK_F15", VK_F15},
    {"VK_F16", VK_F16},
    {"VK_F17", VK_F17},
    {"VK_F18", VK_F18},
    {"VK_F19", VK_F19},
    {"VK_F20", VK_F20},
    {"VK_F21", VK_F21},
    {"VK_F22", VK_F22},
    {"VK_F23", VK_F23},
    {"VK_F24", VK_F24},
    {"VK_OEM_MINUS", VK_OEM_MINUS},
    {"VK_OEM_PLUS", VK_OEM_PLUS},
    {"VK_LBRACKET", VK_OEM_4}, // [
    {"VK_RBRACKET", VK_OEM_6}, // ]
};
// XInput gamepad buttons
#define XINPUT_GAMEPAD_LEFT_TRIGGER  0x10000
#define XINPUT_GAMEPAD_RIGHT_TRIGGER 0x20000
#define XINPUT_GAMEPAD_GUIDE         0x400
static std::unordered_map<std::string, int> XInputMappings = {
    {"XINPUT_GAMEPAD_A", XINPUT_GAMEPAD_A},
    {"XINPUT_GAMEPAD_B", XINPUT_GAMEPAD_B},
    {"XINPUT_GAMEPAD_X", XINPUT_GAMEPAD_X},
    {"XINPUT_GAMEPAD_Y", XINPUT_GAMEPAD_Y},
    {"XINPUT_GAMEPAD_RIGHT_SHOULDER", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"XINPUT_GAMEPAD_LEFT_SHOULDER", XINPUT_GAMEPAD_LEFT_SHOULDER},
    {"XINPUT_GAMEPAD_LEFT_TRIGGER", XINPUT_GAMEPAD_LEFT_TRIGGER},
    {"XINPUT_GAMEPAD_RIGHT_TRIGGER", XINPUT_GAMEPAD_RIGHT_TRIGGER},
    {"XINPUT_GAMEPAD_DPAD_UP", XINPUT_GAMEPAD_DPAD_UP},
    {"XINPUT_GAMEPAD_DPAD_DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
    {"XINPUT_GAMEPAD_DPAD_LEFT", XINPUT_GAMEPAD_DPAD_LEFT},
    {"XINPUT_GAMEPAD_DPAD_RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
    {"XINPUT_GAMEPAD_START", XINPUT_GAMEPAD_START},
    {"XINPUT_GAMEPAD_BACK", XINPUT_GAMEPAD_BACK},
    {"XINPUT_GAMEPAD_GUIDE", XINPUT_GAMEPAD_GUIDE},
    {"XINPUT_GAMEPAD_LEFT_THUMB", XINPUT_GAMEPAD_LEFT_THUMB},
    {"XINPUT_GAMEPAD_RIGHT_THUMB", XINPUT_GAMEPAD_RIGHT_THUMB}
};
//Key Bind Types
#define SWITCH 1
#define TOGGLE 2
#define HOLD   3
static std::unordered_map<std::string, int> KeyBindTypes = {
    {"switch", SWITCH},
    {"toggle", TOGGLE},
    {"hold", HOLD}
};

#endif // VIRTUAL_KEY_MAPPINGS_H
