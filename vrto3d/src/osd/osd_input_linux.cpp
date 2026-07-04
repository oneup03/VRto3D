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
#ifndef _WIN32

// Linux implementation of OsdInput. Same public API as the Win32 version in
// osd_input.cpp so osd_menu.cpp / osd_renderer_vk.cpp compile unchanged.
//
// Sources all state from VRto3DLib's portable evdev pump (input_state.h):
//   - keyboard level state via IsKeyDown (GetAsyncKeyState equivalent),
//   - keyboard edges + typed text via DrainKeyEvents / DrainTypedUtf8
//     (replaces the win version's GetKeyboardState+ToUnicodeEx dance),
//   - mouse via GetMouseState — the evdev virtual cursor already lives in
//     per-eye pixel space (clamped to SetMouseRegion), so unlike the Win32
//     path there is no window-rect normalize/fold step,
//   - gamepad via GetGamepadState (XINPUT_GAMEPAD-shaped).
// Everything evdev reports is global, so the WH_MOUSE_LL hook the Windows
// build needs is unnecessary here; SetMouseHookActive is a stub.

#include "osd/osd_input.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"

#include "vrto3dlib/input_state.h"

// VK_* / XINPUT_GAMEPAD_* numeric codes. The shared header is being added by
// the input_state port; fall back to the canonical Win32 values (guarded so
// the shared definitions win once the header exists).
#if __has_include("vrto3dlib/key_codes.h")
#include "vrto3dlib/key_codes.h"
#endif

#ifndef VK_ESCAPE
#define VK_LBUTTON    0x01
#define VK_RBUTTON    0x02
#define VK_MBUTTON    0x04
#define VK_XBUTTON1   0x05
#define VK_XBUTTON2   0x06
#define VK_BACK       0x08
#define VK_TAB        0x09
#define VK_RETURN     0x0D
#define VK_SHIFT      0x10
#define VK_CONTROL    0x11
#define VK_MENU       0x12
#define VK_PAUSE      0x13
#define VK_CAPITAL    0x14
#define VK_ESCAPE     0x1B
#define VK_SPACE      0x20
#define VK_PRIOR      0x21
#define VK_NEXT       0x22
#define VK_END        0x23
#define VK_HOME       0x24
#define VK_LEFT       0x25
#define VK_UP         0x26
#define VK_RIGHT      0x27
#define VK_DOWN       0x28
#define VK_SNAPSHOT   0x2C
#define VK_INSERT     0x2D
#define VK_DELETE     0x2E
#define VK_LWIN       0x5B
#define VK_RWIN       0x5C
#define VK_NUMPAD0    0x60
#define VK_NUMPAD1    0x61
#define VK_NUMPAD2    0x62
#define VK_NUMPAD3    0x63
#define VK_NUMPAD4    0x64
#define VK_NUMPAD5    0x65
#define VK_NUMPAD6    0x66
#define VK_NUMPAD7    0x67
#define VK_NUMPAD8    0x68
#define VK_NUMPAD9    0x69
#define VK_MULTIPLY   0x6A
#define VK_ADD        0x6B
#define VK_SUBTRACT   0x6D
#define VK_DECIMAL    0x6E
#define VK_DIVIDE     0x6F
#define VK_F1         0x70
#define VK_F2         0x71
#define VK_F3         0x72
#define VK_F4         0x73
#define VK_F5         0x74
#define VK_F6         0x75
#define VK_F7         0x76
#define VK_F8         0x77
#define VK_F9         0x78
#define VK_F10        0x79
#define VK_F11        0x7A
#define VK_F12        0x7B
#define VK_F13        0x7C
#define VK_F14        0x7D
#define VK_F15        0x7E
#define VK_F16        0x7F
#define VK_F17        0x80
#define VK_F18        0x81
#define VK_F19        0x82
#define VK_F20        0x83
#define VK_F21        0x84
#define VK_F22        0x85
#define VK_F23        0x86
#define VK_F24        0x87
#define VK_LSHIFT     0xA0
#define VK_RSHIFT     0xA1
#define VK_LCONTROL   0xA2
#define VK_RCONTROL   0xA3
#define VK_LMENU      0xA4
#define VK_RMENU      0xA5
#define VK_OEM_1      0xBA
#define VK_OEM_PLUS   0xBB
#define VK_OEM_COMMA  0xBC
#define VK_OEM_MINUS  0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2      0xBF
#define VK_OEM_3      0xC0
#define VK_OEM_4      0xDB
#define VK_OEM_5      0xDC
#define VK_OEM_6      0xDD
#define VK_OEM_7      0xDE
#endif  // VK_ESCAPE

#ifndef XINPUT_GAMEPAD_A
#define XINPUT_GAMEPAD_DPAD_UP        0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN      0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT      0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT     0x0008
#define XINPUT_GAMEPAD_START          0x0010
#define XINPUT_GAMEPAD_BACK           0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB     0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB    0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER  0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A              0x1000
#define XINPUT_GAMEPAD_B              0x2000
#define XINPUT_GAMEPAD_X              0x4000
#define XINPUT_GAMEPAD_Y              0x8000
#endif  // XINPUT_GAMEPAD_A
#ifndef XINPUT_GAMEPAD_GUIDE
#define XINPUT_GAMEPAD_GUIDE          0x400
#endif
#ifndef XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
#define XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE  7849
#endif
#ifndef XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE
#define XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE 8689
#endif

namespace vrto3d::osd {

namespace {

// Latest evdev snapshot, refreshed by Poll(). File-scope (not members) since
// the OsdInput layout in osd_input.h is shared with the Win32 build; there is
// exactly one OsdInput per driver, mirroring the win version's file-scope
// hook state. Poll() replaces the previous batch every frame, so edge/text
// events that were never fed to ImGui (menu closed) are discarded instead of
// flooding the UI when it opens.
struct PumpState {
    vrto3d::input::MouseState              mouse;
    std::vector<vrto3d::input::KeyEvent>   key_events;
    std::string                            typed_utf8;
    int                                    region_w = 0;
    int                                    region_h = 0;
};
PumpState g_pump;

ImGuiKey VkToImGuiKey(int vk) {
    switch (vk) {
        case VK_TAB: return ImGuiKey_Tab;
        case VK_LEFT: return ImGuiKey_LeftArrow;
        case VK_RIGHT: return ImGuiKey_RightArrow;
        case VK_UP: return ImGuiKey_UpArrow;
        case VK_DOWN: return ImGuiKey_DownArrow;
        case VK_PRIOR: return ImGuiKey_PageUp;
        case VK_NEXT: return ImGuiKey_PageDown;
        case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End;
        case VK_INSERT: return ImGuiKey_Insert;
        case VK_DELETE: return ImGuiKey_Delete;
        case VK_BACK: return ImGuiKey_Backspace;
        case VK_SPACE: return ImGuiKey_Space;
        case VK_RETURN: return ImGuiKey_Enter;
        case VK_ESCAPE: return ImGuiKey_Escape;
        case VK_LCONTROL: return ImGuiKey_LeftCtrl;
        case VK_RCONTROL: return ImGuiKey_RightCtrl;
        case VK_LSHIFT:   return ImGuiKey_LeftShift;
        case VK_RSHIFT:   return ImGuiKey_RightShift;
        case VK_LMENU:    return ImGuiKey_LeftAlt;
        case VK_RMENU:    return ImGuiKey_RightAlt;
        case VK_OEM_COMMA:  return ImGuiKey_Comma;
        case VK_OEM_MINUS:  return ImGuiKey_Minus;
        case VK_OEM_PERIOD: return ImGuiKey_Period;
        case VK_OEM_2: return ImGuiKey_Slash;
        case VK_OEM_1: return ImGuiKey_Semicolon;
        case VK_OEM_PLUS: return ImGuiKey_Equal;
        case VK_OEM_4: return ImGuiKey_LeftBracket;
        case VK_OEM_6: return ImGuiKey_RightBracket;
        case VK_OEM_5: return ImGuiKey_Backslash;
        case VK_OEM_3: return ImGuiKey_GraveAccent;
        case VK_OEM_7: return ImGuiKey_Apostrophe;
        default: break;
    }
    if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    if (vk >= VK_F1 && vk <= VK_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (vk - VK_F1));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (vk - VK_NUMPAD0));
    return ImGuiKey_None;
}

// Reverse lookup: VK_*/XINPUT_GAMEPAD_* numeric code → its name string. Kept
// local (rather than including vrto3dlib/key_mappings.h, which is still
// Win32-only) but the strings match VirtualKeyMappings / XInputMappings
// exactly so captured names round-trip through JsonManager unchanged. Split
// into two tables so a D-pad bit (0x0001) can't alias VK_LBUTTON (0x01) the
// way a single combined map would.
struct NamedCode {
    const char* name;
    int         code;
};

const NamedCode kVkNames[] = {
    {"VK_LMOUSE", VK_LBUTTON}, {"VK_RMOUSE", VK_RBUTTON}, {"VK_MMOUSE", VK_MBUTTON},
    {"VK_MOUSE4", VK_XBUTTON1}, {"VK_MOUSE5", VK_XBUTTON2},
    {"VK_BACKSPACE", VK_BACK}, {"VK_TAB", VK_TAB}, {"VK_RETURN", VK_RETURN},
    {"VK_SHIFT", VK_SHIFT}, {"VK_CONTROL", VK_CONTROL}, {"VK_MENU", VK_MENU},
    {"VK_PAUSE", VK_PAUSE}, {"VK_CAPS", VK_CAPITAL}, {"VK_ESCAPE", VK_ESCAPE},
    {"VK_SPACE", VK_SPACE}, {"VK_PGUP", VK_PRIOR}, {"VK_PGDWN", VK_NEXT},
    {"VK_END", VK_END}, {"VK_HOME", VK_HOME},
    {"VK_LEFT", VK_LEFT}, {"VK_UP", VK_UP}, {"VK_RIGHT", VK_RIGHT}, {"VK_DOWN", VK_DOWN},
    {"VK_SNAPSHOT", VK_SNAPSHOT}, {"VK_INSERT", VK_INSERT}, {"VK_DELETE", VK_DELETE},
    {"VK_NUMPAD0", VK_NUMPAD0}, {"VK_NUMPAD1", VK_NUMPAD1}, {"VK_NUMPAD2", VK_NUMPAD2},
    {"VK_NUMPAD3", VK_NUMPAD3}, {"VK_NUMPAD4", VK_NUMPAD4}, {"VK_NUMPAD5", VK_NUMPAD5},
    {"VK_NUMPAD6", VK_NUMPAD6}, {"VK_NUMPAD7", VK_NUMPAD7}, {"VK_NUMPAD8", VK_NUMPAD8},
    {"VK_NUMPAD9", VK_NUMPAD9},
    {"VK_MULTIPLY", VK_MULTIPLY}, {"VK_ADD", VK_ADD}, {"VK_SUBTRACT", VK_SUBTRACT},
    {"VK_DECIMAL", VK_DECIMAL}, {"VK_DIVIDE", VK_DIVIDE},
    {"VK_F1", VK_F1}, {"VK_F2", VK_F2}, {"VK_F3", VK_F3}, {"VK_F4", VK_F4},
    {"VK_F5", VK_F5}, {"VK_F6", VK_F6}, {"VK_F7", VK_F7}, {"VK_F8", VK_F8},
    {"VK_F9", VK_F9}, {"VK_F10", VK_F10}, {"VK_F11", VK_F11}, {"VK_F12", VK_F12},
    {"VK_F13", VK_F13}, {"VK_F14", VK_F14}, {"VK_F15", VK_F15}, {"VK_F16", VK_F16},
    {"VK_F17", VK_F17}, {"VK_F18", VK_F18}, {"VK_F19", VK_F19}, {"VK_F20", VK_F20},
    {"VK_F21", VK_F21}, {"VK_F22", VK_F22}, {"VK_F23", VK_F23}, {"VK_F24", VK_F24},
    {"VK_OEM_MINUS", VK_OEM_MINUS}, {"VK_OEM_PLUS", VK_OEM_PLUS},
    {"VK_LBRACKET", VK_OEM_4}, {"VK_RBRACKET", VK_OEM_6},
    {"VK_A", 'A'}, {"VK_B", 'B'}, {"VK_C", 'C'}, {"VK_D", 'D'},
    {"VK_E", 'E'}, {"VK_F", 'F'}, {"VK_G", 'G'}, {"VK_H", 'H'},
    {"VK_I", 'I'}, {"VK_J", 'J'}, {"VK_K", 'K'}, {"VK_L", 'L'},
    {"VK_M", 'M'}, {"VK_N", 'N'}, {"VK_O", 'O'}, {"VK_P", 'P'},
    {"VK_Q", 'Q'}, {"VK_R", 'R'}, {"VK_S", 'S'}, {"VK_T", 'T'},
    {"VK_U", 'U'}, {"VK_V", 'V'}, {"VK_W", 'W'}, {"VK_X", 'X'},
    {"VK_Y", 'Y'}, {"VK_Z", 'Z'},
    {"VK_0", '0'}, {"VK_1", '1'}, {"VK_2", '2'}, {"VK_3", '3'},
    {"VK_4", '4'}, {"VK_5", '5'}, {"VK_6", '6'}, {"VK_7", '7'},
    {"VK_8", '8'}, {"VK_9", '9'},
};

const NamedCode kPadNames[] = {
    {"XINPUT_GAMEPAD_A", XINPUT_GAMEPAD_A},
    {"XINPUT_GAMEPAD_B", XINPUT_GAMEPAD_B},
    {"XINPUT_GAMEPAD_X", XINPUT_GAMEPAD_X},
    {"XINPUT_GAMEPAD_Y", XINPUT_GAMEPAD_Y},
    {"XINPUT_GAMEPAD_RIGHT_SHOULDER", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"XINPUT_GAMEPAD_LEFT_SHOULDER", XINPUT_GAMEPAD_LEFT_SHOULDER},
    {"XINPUT_GAMEPAD_DPAD_UP", XINPUT_GAMEPAD_DPAD_UP},
    {"XINPUT_GAMEPAD_DPAD_DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
    {"XINPUT_GAMEPAD_DPAD_LEFT", XINPUT_GAMEPAD_DPAD_LEFT},
    {"XINPUT_GAMEPAD_DPAD_RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
    {"XINPUT_GAMEPAD_START", XINPUT_GAMEPAD_START},
    {"XINPUT_GAMEPAD_BACK", XINPUT_GAMEPAD_BACK},
    {"XINPUT_GAMEPAD_GUIDE", XINPUT_GAMEPAD_GUIDE},
    {"XINPUT_GAMEPAD_LEFT_THUMB", XINPUT_GAMEPAD_LEFT_THUMB},
    {"XINPUT_GAMEPAD_RIGHT_THUMB", XINPUT_GAMEPAD_RIGHT_THUMB},
};

std::string NameForVk(int vk) {
    static const auto map = []{
        std::unordered_map<int, std::string> m;
        for (const auto& e : kVkNames) m.emplace(e.code, e.name);
        return m;
    }();
    auto it = map.find(vk);
    return it != map.end() ? it->second : std::string{};
}

std::string NameForPadMask(uint32_t mask) {
    static const auto map = []{
        std::unordered_map<int, std::string> m;
        for (const auto& e : kPadNames) m.emplace(e.code, e.name);
        return m;
    }();
    auto it = map.find(static_cast<int>(mask));
    return it != map.end() ? it->second : std::string{};
}

}  // namespace

OsdInput::OsdInput() {
    // Make sure the evdev pump is running. Idempotent — the driver's hotkey
    // thread has usually started it already; this covers standalone use.
    vrto3d::input::Start();
}

OsdInput::~OsdInput() {
    // The evdev pump is shared driver-wide (hotkeys keep using it); nothing
    // to release here — no per-instance hook like the Win32 build.
}

void OsdInput::SetMouseHookActive(bool active) {
    // evdev input is always global; there is no LL hook to install/remove.
    // Track the flag anyway for API parity.
    hook_active_ = active;
}

void OsdInput::Poll() {
    keys_prev_ = keys_curr_;
    for (int vk = 8; vk < 256; ++vk) {
        keys_curr_[vk] = vrto3d::input::IsKeyDown(vk);
    }

    // Mouse snapshot. GetMouseState drains the wheel accumulator, so take it
    // once per Poll and let FeedImGui consume the copy. Mirror the buttons
    // into keys_curr_ so WasPressed/IsHeld and the capture exclusion lists
    // treat VK_LBUTTON & co. the same way GetAsyncKeyState does on Windows.
    g_pump.mouse = vrto3d::input::GetMouseState();
    keys_curr_[VK_LBUTTON]  = g_pump.mouse.left;
    keys_curr_[VK_RBUTTON]  = g_pump.mouse.right;
    keys_curr_[VK_MBUTTON]  = g_pump.mouse.middle;
    keys_curr_[VK_XBUTTON1] = g_pump.mouse.x1;
    keys_curr_[VK_XBUTTON2] = g_pump.mouse.x2;

    // Drain key edges + typed text, replacing whatever the previous frame
    // left unconsumed (stale input from frames where the OSD wasn't fed must
    // not burst into ImGui when the menu opens).
    g_pump.key_events.clear();
    vrto3d::input::KeyEvent ev_buf[64];
    for (;;) {
        const int n = vrto3d::input::DrainKeyEvents(ev_buf, 64);
        for (int i = 0; i < n; ++i) g_pump.key_events.push_back(ev_buf[i]);
        if (n < 64) break;
    }
    g_pump.typed_utf8.clear();
    char text_buf[512];
    if (vrto3d::input::DrainTypedUtf8(text_buf, sizeof(text_buf)) > 0) {
        g_pump.typed_utf8.assign(text_buf);
    }

    // Gamepad poll — merged state across connected pads; matches the win
    // version's XInput port 0 scope. `pad_curr_` keeps the raw 16-bit
    // wButtons mask so the chord-capture iteration stays valid; triggers and
    // stick deflection are tracked separately for ImGui analog nav.
    pad_prev_ = pad_curr_;
    const vrto3d::input::GamepadState pad = vrto3d::input::GetGamepadState();
    if (pad.connected) {
        pad_connected_ = true;
        pad_curr_ = pad.wButtons;
        auto norm_stick = [](int16_t v, int16_t dz) {
            int a = (v >= 0) ? v : -v;
            if (a <= dz) return 0.0f;
            float n = (a - dz) / static_cast<float>(32767 - dz);
            if (n > 1.0f) n = 1.0f;
            return (v < 0) ? -n : n;
        };
        pad_lx_ = norm_stick(pad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        pad_ly_ = norm_stick(pad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        pad_rx_ = norm_stick(pad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        pad_ry_ = norm_stick(pad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        pad_lt_ = pad.bLeftTrigger  / 255.0f;
        pad_rt_ = pad.bRightTrigger / 255.0f;
    } else {
        pad_connected_ = false;
        pad_curr_ = 0;
        pad_lx_ = pad_ly_ = pad_rx_ = pad_ry_ = 0.0f;
        pad_lt_ = pad_rt_ = 0.0f;
    }
}

void OsdInput::FeedImGui(ImGuiIO& io, const OsdSurface& surface) {
    // The evdev virtual cursor already lives in per-eye pixel space — just
    // keep its clamp region synced to the OSD surface and use it directly.
    // (The Win32 build has to normalize against the headset window's client
    // rect and fold SbS/TaB halves; none of that applies here.)
    if (surface.eye_w > 0 && surface.eye_h > 0 &&
        (surface.eye_w != g_pump.region_w || surface.eye_h != g_pump.region_h)) {
        vrto3d::input::SetMouseRegion(surface.eye_w, surface.eye_h);
        g_pump.region_w = surface.eye_w;
        g_pump.region_h = surface.eye_h;
    }
    io.AddMousePosEvent(static_cast<float>(g_pump.mouse.x),
                        static_cast<float>(g_pump.mouse.y));

    io.AddMouseButtonEvent(0, g_pump.mouse.left);
    io.AddMouseButtonEvent(1, g_pump.mouse.right);
    io.AddMouseButtonEvent(2, g_pump.mouse.middle);

    if (g_pump.mouse.wheel != 0) {
        io.AddMouseWheelEvent(0.0f, static_cast<float>(g_pump.mouse.wheel));
        g_pump.mouse.wheel = 0;  // consumed — don't repeat if fed again
    }

    // Modifiers (level-triggered, like the win version).
    io.AddKeyEvent(ImGuiMod_Ctrl,  keys_curr_[VK_CONTROL]);
    io.AddKeyEvent(ImGuiMod_Shift, keys_curr_[VK_SHIFT]);
    io.AddKeyEvent(ImGuiMod_Alt,   keys_curr_[VK_MENU]);
    io.AddKeyEvent(ImGuiMod_Super, keys_curr_[VK_LWIN] || keys_curr_[VK_RWIN]);

    // Key edges from the evdev queue. Generic modifier codes are covered by
    // the AddKeyEvent(ImGuiMod_*) calls above; mouse buttons by the
    // AddMouseButtonEvent calls.
    for (const auto& ev : g_pump.key_events) {
        if (ev.vk <= 0) continue;
        if (ev.vk == VK_LBUTTON || ev.vk == VK_RBUTTON || ev.vk == VK_MBUTTON ||
            ev.vk == VK_CONTROL || ev.vk == VK_SHIFT   || ev.vk == VK_MENU)
            continue;
        ImGuiKey ik = VkToImGuiKey(ev.vk);
        if (ik != ImGuiKey_None) io.AddKeyEvent(ik, ev.down);
    }
    g_pump.key_events.clear();

    // Typed text (xkbcommon-translated by the evdev pump). Suppress while
    // Ctrl/Alt are held, mirroring the win version's ToUnicodeEx gate, and
    // filter ASCII control bytes (single bytes in UTF-8, so a byte-wise
    // filter can't split a multi-byte sequence).
    if (!g_pump.typed_utf8.empty() &&
        !keys_curr_[VK_CONTROL] && !keys_curr_[VK_MENU]) {
        std::string filtered;
        filtered.reserve(g_pump.typed_utf8.size());
        for (char c : g_pump.typed_utf8) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u >= 0x20 && u != 0x7F) filtered.push_back(c);
        }
        if (!filtered.empty()) io.AddInputCharactersUTF8(filtered.c_str());
    }
    g_pump.typed_utf8.clear();

    // Gamepad → ImGui nav keys. ImGui ignores gamepad input unless both
    // ConfigFlags_NavEnableGamepad AND BackendFlags_HasGamepad are set —
    // toggle the backend bit with controller-connected so keyboard-only
    // users don't get phantom nav highlights when there's no pad attached.
    if (pad_connected_) io.BackendFlags |=  ImGuiBackendFlags_HasGamepad;
    else                io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    {
        auto pad_bit = [&](uint16_t mask) -> bool { return (pad_curr_ & mask) != 0; };
        io.AddKeyEvent(ImGuiKey_GamepadFaceDown,  pad_bit(XINPUT_GAMEPAD_A));
        io.AddKeyEvent(ImGuiKey_GamepadFaceRight, pad_bit(XINPUT_GAMEPAD_B));
        io.AddKeyEvent(ImGuiKey_GamepadFaceLeft,  pad_bit(XINPUT_GAMEPAD_X));
        io.AddKeyEvent(ImGuiKey_GamepadFaceUp,    pad_bit(XINPUT_GAMEPAD_Y));
        io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,  pad_bit(XINPUT_GAMEPAD_DPAD_LEFT));
        io.AddKeyEvent(ImGuiKey_GamepadDpadRight, pad_bit(XINPUT_GAMEPAD_DPAD_RIGHT));
        io.AddKeyEvent(ImGuiKey_GamepadDpadUp,    pad_bit(XINPUT_GAMEPAD_DPAD_UP));
        io.AddKeyEvent(ImGuiKey_GamepadDpadDown,  pad_bit(XINPUT_GAMEPAD_DPAD_DOWN));
        io.AddKeyEvent(ImGuiKey_GamepadL1,        pad_bit(XINPUT_GAMEPAD_LEFT_SHOULDER));
        io.AddKeyEvent(ImGuiKey_GamepadR1,        pad_bit(XINPUT_GAMEPAD_RIGHT_SHOULDER));
        io.AddKeyEvent(ImGuiKey_GamepadL3,        pad_bit(XINPUT_GAMEPAD_LEFT_THUMB));
        io.AddKeyEvent(ImGuiKey_GamepadR3,        pad_bit(XINPUT_GAMEPAD_RIGHT_THUMB));
        io.AddKeyEvent(ImGuiKey_GamepadStart,     pad_bit(XINPUT_GAMEPAD_START));
        io.AddKeyEvent(ImGuiKey_GamepadBack,      pad_bit(XINPUT_GAMEPAD_BACK));

        auto axis = [&](ImGuiKey k, float v) {
            io.AddKeyAnalogEvent(k, v > 0.0f, v);
        };
        axis(ImGuiKey_GamepadL2,           pad_lt_);
        axis(ImGuiKey_GamepadR2,           pad_rt_);
        axis(ImGuiKey_GamepadLStickLeft,   pad_lx_ < 0.0f ? -pad_lx_ : 0.0f);
        axis(ImGuiKey_GamepadLStickRight,  pad_lx_ > 0.0f ?  pad_lx_ : 0.0f);
        axis(ImGuiKey_GamepadLStickUp,     pad_ly_ > 0.0f ?  pad_ly_ : 0.0f);
        axis(ImGuiKey_GamepadLStickDown,   pad_ly_ < 0.0f ? -pad_ly_ : 0.0f);
        axis(ImGuiKey_GamepadRStickLeft,   pad_rx_ < 0.0f ? -pad_rx_ : 0.0f);
        axis(ImGuiKey_GamepadRStickRight,  pad_rx_ > 0.0f ?  pad_rx_ : 0.0f);
        axis(ImGuiKey_GamepadRStickUp,     pad_ry_ > 0.0f ?  pad_ry_ : 0.0f);
        axis(ImGuiKey_GamepadRStickDown,   pad_ry_ < 0.0f ? -pad_ry_ : 0.0f);
    }

    // Capture sampling — identical flow to the Win32 implementation.
    if (capturing_) {
        if (capture_combo_) {
            // Combo mode: accumulate everything held until release-all.
            bool any_held = false;
            for (int vk = 8; vk < 256; ++vk) {
                if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
                if (keys_curr_[vk]) {
                    any_held = true;
                    std::string name = NameForVk(vk);
                    if (!name.empty()) capture_set_.insert(std::move(name));
                }
            }
            for (uint32_t bit = 1; bit; bit <<= 1) {
                if (pad_curr_ & bit) {
                    any_held = true;
                    std::string name = NameForPadMask(bit);
                    if (!name.empty()) capture_set_.insert(std::move(name));
                }
            }
            if (any_held) had_any_press_ = true;
            if (had_any_press_ && !any_held && !capture_set_.empty()) {
                std::string out;
                for (const auto& n : capture_set_) {
                    if (!out.empty()) out += '+';
                    out += n;
                }
                captured_.valid    = true;
                captured_.key_name = std::move(out);
                capturing_ = false;
                capture_set_.clear();
                had_any_press_ = false;
            }
        } else {
            // Single-key mode: first newly-pressed key/button wins.
            for (int vk = 8; vk < 256; ++vk) {
                if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
                    vk == VK_SHIFT   || vk == VK_CONTROL || vk == VK_MENU    ||
                    vk == VK_LSHIFT  || vk == VK_RSHIFT  || vk == VK_LCONTROL||
                    vk == VK_RCONTROL|| vk == VK_LMENU   || vk == VK_RMENU)
                    continue;
                if (keys_curr_[vk] && !keys_prev_[vk]) {
                    captured_.valid    = true;
                    captured_.key_name = NameForVk(vk);
                    capturing_ = false;
                    break;
                }
            }
            const uint16_t newly_down = static_cast<uint16_t>(pad_curr_ & ~pad_prev_);
            if (capturing_ && newly_down) {
                for (uint32_t bit = 1; bit; bit <<= 1) {
                    if (newly_down & bit) {
                        std::string name = NameForPadMask(bit);
                        if (!name.empty()) {
                            captured_.valid    = true;
                            captured_.key_name = std::move(name);
                            capturing_ = false;
                            break;
                        }
                    }
                }
            }
        }
    }
}

bool OsdInput::WasPressed(int vk) const {
    if (vk <= 0 || vk >= 256) return false;
    return keys_curr_[vk] && !keys_prev_[vk];
}

bool OsdInput::IsHeld(int vk) const {
    if (vk <= 0 || vk >= 256) return false;
    return keys_curr_[vk];
}

void OsdInput::BeginCapture(bool combo) {
    capturing_     = true;
    capture_combo_ = combo;
    captured_      = {};
    capture_set_.clear();
    had_any_press_ = false;
}

void OsdInput::CancelCapture() {
    capturing_     = false;
    capture_combo_ = false;
    captured_      = {};
    capture_set_.clear();
    had_any_press_ = false;
}

CapturedKey OsdInput::PollCapture() {
    if (captured_.valid) {
        auto out = captured_;
        captured_ = {};
        return out;
    }
    return {};
}

} // namespace vrto3d::osd

#endif  // !_WIN32
