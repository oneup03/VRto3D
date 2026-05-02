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
#include "osd/osd_keymap.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <Xinput.h>
#endif

namespace vrto3d::osd {

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Legacy alias table — old Windows-flavored names → portable form.
// Loaded into the lookup map on first use.
struct AliasEntry { const char* legacy; const char* portable; };
const AliasEntry kAliases[] = {
    // Function keys
    { "VK_F1",  "F1"  }, { "VK_F2",  "F2"  }, { "VK_F3",  "F3"  }, { "VK_F4",  "F4"  },
    { "VK_F5",  "F5"  }, { "VK_F6",  "F6"  }, { "VK_F7",  "F7"  }, { "VK_F8",  "F8"  },
    { "VK_F9",  "F9"  }, { "VK_F10", "F10" }, { "VK_F11", "F11" }, { "VK_F12", "F12" },
    // Numpad
    { "VK_NUMPAD0", "Numpad0" }, { "VK_NUMPAD1", "Numpad1" },
    { "VK_NUMPAD2", "Numpad2" }, { "VK_NUMPAD3", "Numpad3" },
    { "VK_NUMPAD4", "Numpad4" }, { "VK_NUMPAD5", "Numpad5" },
    { "VK_NUMPAD6", "Numpad6" }, { "VK_NUMPAD7", "Numpad7" },
    { "VK_NUMPAD8", "Numpad8" }, { "VK_NUMPAD9", "Numpad9" },
    // Navigation cluster
    { "VK_HOME",   "Home"   }, { "VK_END",    "End"    },
    { "VK_INSERT", "Insert" }, { "VK_DELETE", "Delete" },
    { "VK_PRIOR",  "PgUp"   }, { "VK_NEXT",   "PgDn"   },
    { "VK_UP",     "Up"     }, { "VK_DOWN",   "Down"   },
    { "VK_LEFT",   "Left"   }, { "VK_RIGHT",  "Right"  },
    // Modifiers
    { "VK_SHIFT",   "Shift"   },
    { "VK_CONTROL", "Ctrl"    },
    { "VK_MENU",    "Alt"     },
    // Misc
    { "VK_TAB",    "Tab"    }, { "VK_RETURN", "Enter"  },
    { "VK_ESCAPE", "Escape" }, { "VK_SPACE",  "Space"  },
    { "VK_BACK",   "Backspace" },
    // XInput buttons
    { "XINPUT_GAMEPAD_A",              "Pad.A"        },
    { "XINPUT_GAMEPAD_B",              "Pad.B"        },
    { "XINPUT_GAMEPAD_X",              "Pad.X"        },
    { "XINPUT_GAMEPAD_Y",              "Pad.Y"        },
    { "XINPUT_GAMEPAD_GUIDE",          "Pad.Guide"    },
    { "XINPUT_GAMEPAD_START",          "Pad.Start"    },
    { "XINPUT_GAMEPAD_BACK",           "Pad.Back"     },
    { "XINPUT_GAMEPAD_LEFT_THUMB",     "Pad.LStick"   },
    { "XINPUT_GAMEPAD_RIGHT_THUMB",    "Pad.RStick"   },
    { "XINPUT_GAMEPAD_LEFT_SHOULDER",  "Pad.LBumper"  },
    { "XINPUT_GAMEPAD_RIGHT_SHOULDER", "Pad.RBumper"  },
    { "XINPUT_GAMEPAD_DPAD_UP",        "Pad.DPadUp"   },
    { "XINPUT_GAMEPAD_DPAD_DOWN",      "Pad.DPadDown" },
    { "XINPUT_GAMEPAD_DPAD_LEFT",      "Pad.DPadLeft" },
    { "XINPUT_GAMEPAD_DPAD_RIGHT",     "Pad.DPadRight" },
};

const std::unordered_map<std::string, std::string>& LegacyToPortable() {
    static const auto map = []{
        std::unordered_map<std::string, std::string> m;
        for (auto& e : kAliases) m.emplace(ToLower(e.legacy), e.portable);
        return m;
    }();
    return map;
}

} // namespace

std::string ToPortableName(const std::string& s) {
    if (s.empty()) return s;
    auto& map = LegacyToPortable();
    auto it = map.find(ToLower(s));
    return it != map.end() ? it->second : s;
}

bool IsLegacyKeyName(const std::string& s) {
    if (s.empty()) return false;
    auto& map = LegacyToPortable();
    return map.find(ToLower(s)) != map.end();
}

#ifdef _WIN32

namespace {

// Portable-name → (VK code, XInput mask). Only one of the two fields is
// non-zero per entry.
struct PortableEntry {
    const char* name;
    int32_t     vk;
    uint32_t    pad_mask;
};

const PortableEntry kPortable[] = {
    // Function keys
    { "F1",  VK_F1,  0 }, { "F2",  VK_F2,  0 }, { "F3",  VK_F3,  0 }, { "F4",  VK_F4,  0 },
    { "F5",  VK_F5,  0 }, { "F6",  VK_F6,  0 }, { "F7",  VK_F7,  0 }, { "F8",  VK_F8,  0 },
    { "F9",  VK_F9,  0 }, { "F10", VK_F10, 0 }, { "F11", VK_F11, 0 }, { "F12", VK_F12, 0 },
    // Numpad
    { "Numpad0", VK_NUMPAD0, 0 }, { "Numpad1", VK_NUMPAD1, 0 },
    { "Numpad2", VK_NUMPAD2, 0 }, { "Numpad3", VK_NUMPAD3, 0 },
    { "Numpad4", VK_NUMPAD4, 0 }, { "Numpad5", VK_NUMPAD5, 0 },
    { "Numpad6", VK_NUMPAD6, 0 }, { "Numpad7", VK_NUMPAD7, 0 },
    { "Numpad8", VK_NUMPAD8, 0 }, { "Numpad9", VK_NUMPAD9, 0 },
    // Navigation
    { "Home",   VK_HOME,   0 }, { "End",    VK_END,    0 },
    { "Insert", VK_INSERT, 0 }, { "Delete", VK_DELETE, 0 },
    { "PgUp",   VK_PRIOR,  0 }, { "PgDn",   VK_NEXT,   0 },
    { "Up",     VK_UP,     0 }, { "Down",   VK_DOWN,   0 },
    { "Left",   VK_LEFT,   0 }, { "Right",  VK_RIGHT,  0 },
    // Modifiers
    { "Shift", VK_SHIFT, 0 }, { "Ctrl", VK_CONTROL, 0 }, { "Alt", VK_MENU, 0 },
    // Misc
    { "Tab",       VK_TAB,    0 }, { "Enter",  VK_RETURN, 0 },
    { "Escape",    VK_ESCAPE, 0 }, { "Space",  VK_SPACE,  0 },
    { "Backspace", VK_BACK,   0 },
    // XInput buttons
    { "Pad.A",        0, XINPUT_GAMEPAD_A              },
    { "Pad.B",        0, XINPUT_GAMEPAD_B              },
    { "Pad.X",        0, XINPUT_GAMEPAD_X              },
    { "Pad.Y",        0, XINPUT_GAMEPAD_Y              },
    { "Pad.Guide",    0, 0x0400 /* XINPUT_GAMEPAD_GUIDE — undocumented */ },
    { "Pad.Start",    0, XINPUT_GAMEPAD_START          },
    { "Pad.Back",     0, XINPUT_GAMEPAD_BACK           },
    { "Pad.LStick",   0, XINPUT_GAMEPAD_LEFT_THUMB     },
    { "Pad.RStick",   0, XINPUT_GAMEPAD_RIGHT_THUMB    },
    { "Pad.LBumper",  0, XINPUT_GAMEPAD_LEFT_SHOULDER  },
    { "Pad.RBumper",  0, XINPUT_GAMEPAD_RIGHT_SHOULDER },
    { "Pad.DPadUp",    0, XINPUT_GAMEPAD_DPAD_UP    },
    { "Pad.DPadDown",  0, XINPUT_GAMEPAD_DPAD_DOWN  },
    { "Pad.DPadLeft",  0, XINPUT_GAMEPAD_DPAD_LEFT  },
    { "Pad.DPadRight", 0, XINPUT_GAMEPAD_DPAD_RIGHT },
};

struct PortableMaps {
    std::unordered_map<std::string, const PortableEntry*> by_name;
    std::unordered_map<int32_t, const PortableEntry*>     by_vk;
    std::unordered_map<uint32_t, const PortableEntry*>    by_pad;
};

const PortableMaps& Maps() {
    static const auto m = []{
        PortableMaps r;
        for (auto& e : kPortable) {
            r.by_name.emplace(ToLower(e.name), &e);
            if (e.vk)       r.by_vk.emplace(e.vk, &e);
            if (e.pad_mask) r.by_pad.emplace(e.pad_mask, &e);
        }
        return r;
    }();
    return m;
}

} // namespace

bool ResolveWin32(const std::string& portable_name, int32_t& vk_out, uint32_t& pad_mask_out) {
    vk_out = 0;
    pad_mask_out = 0;
    if (portable_name.empty()) return false;

    // Single A-Z / 0-9 — derive VK directly so we don't have to enumerate
    // every alphanumeric in the table.
    if (portable_name.size() == 1) {
        unsigned char ch = static_cast<unsigned char>(std::toupper(portable_name[0]));
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            vk_out = ch;
            return true;
        }
    }

    auto& map = Maps().by_name;
    auto it = map.find(ToLower(portable_name));
    if (it == map.end()) return false;
    vk_out       = it->second->vk;
    pad_mask_out = it->second->pad_mask;
    return true;
}

std::string FromWin32Vk(int32_t vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::string(1, static_cast<char>(vk));
    }
    auto& map = Maps().by_vk;
    auto it = map.find(vk);
    return it != map.end() ? std::string(it->second->name) : std::string{};
}

std::string FromWin32PadMask(uint32_t pad_mask) {
    auto& map = Maps().by_pad;
    auto it = map.find(pad_mask);
    return it != map.end() ? std::string(it->second->name) : std::string{};
}

#endif // _WIN32

} // namespace vrto3d::osd
