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
#include "osd/osd_input.h"

#include <atomic>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <Xinput.h>

#include "imgui.h"
#include "vrto3dlib/key_mappings.h"

namespace vrto3d::osd {

namespace {

// Shared low-level mouse hook for wheel + button events. Installed once on
// first OsdInput::SetMouseHookActive(true); removed on last release.
// GetAsyncKeyState(VK_LBUTTON) returns 0 when our process isn't foreground
// (the SteamVR driver never is), so we have to source button state from the
// hook too.
std::atomic<int>           g_mouse_hook_refs{0};
HHOOK                      g_mouse_hook = nullptr;
std::atomic<float>         g_wheel_delta_y{0.0f};
std::atomic<float>         g_wheel_delta_x{0.0f};
std::atomic<bool>          g_mb_left{false};
std::atomic<bool>          g_mb_right{false};
std::atomic<bool>          g_mb_middle{false};

// std::atomic<float>::fetch_add is C++20 — emulate via CAS for C++17.
void AtomicFloatAdd(std::atomic<float>& a, float v) {
    float old = a.load();
    while (!a.compare_exchange_weak(old, old + v)) {}
}

LRESULT CALLBACK MouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        switch (wParam) {
            case WM_MOUSEWHEEL: {
                const short delta = static_cast<short>(HIWORD(ms->mouseData));
                AtomicFloatAdd(g_wheel_delta_y, static_cast<float>(delta) / WHEEL_DELTA);
                break;
            }
            case WM_MOUSEHWHEEL: {
                const short delta = static_cast<short>(HIWORD(ms->mouseData));
                AtomicFloatAdd(g_wheel_delta_x, static_cast<float>(delta) / WHEEL_DELTA);
                break;
            }
            case WM_LBUTTONDOWN: g_mb_left.store(true);    break;
            case WM_LBUTTONUP:   g_mb_left.store(false);   break;
            case WM_RBUTTONDOWN: g_mb_right.store(true);   break;
            case WM_RBUTTONUP:   g_mb_right.store(false);  break;
            case WM_MBUTTONDOWN: g_mb_middle.store(true);  break;
            case WM_MBUTTONUP:   g_mb_middle.store(false); break;
            default: break;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void EnsureMouseHook() {
    if (g_mouse_hook_refs.fetch_add(1) == 0) {
        g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(nullptr), 0);
    }
}

void ReleaseMouseHook() {
    if (g_mouse_hook_refs.fetch_sub(1) == 1) {
        if (g_mouse_hook) {
            UnhookWindowsHookEx(g_mouse_hook);
            g_mouse_hook = nullptr;
        }
    }
}

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

// Reverse lookup: VK_*/XINPUT_GAMEPAD_* numeric code → its name string in
// VRto3DLib's VirtualKeyMappings / XInputMappings tables. Used by the
// click-to-capture key picker to round-trip a captured key into the same
// string format that JsonManager parses.
const std::unordered_map<int, std::string>& KeyCodeToName() {
    static const auto map = []{
        std::unordered_map<int, std::string> m;
        for (auto& kv : VirtualKeyMappings) m.emplace(kv.second, kv.first);
        for (auto& kv : XInputMappings)     m.emplace(kv.second, kv.first);
        return m;
    }();
    return map;
}

std::string NameForVk(int vk) {
    auto& map = KeyCodeToName();
    auto it = map.find(vk);
    return it != map.end() ? it->second : std::string{};
}

std::string NameForPadMask(uint32_t mask) {
    auto& map = KeyCodeToName();
    auto it = map.find(static_cast<int>(mask));
    return it != map.end() ? it->second : std::string{};
}

} // namespace

OsdInput::OsdInput() {
    // Mouse hook deferred — OsdRenderer enables it via SetMouseHookActive
    // only when the menu is open (or capturing), so a closed OSD doesn't
    // route every system-wide mouse event through our process.
}

OsdInput::~OsdInput() {
    if (hook_active_) ReleaseMouseHook();
}

void OsdInput::SetMouseHookActive(bool active) {
    if (active == hook_active_) return;
    if (active) EnsureMouseHook();
    else        ReleaseMouseHook();
    hook_active_ = active;
}

void OsdInput::Poll() {
    keys_prev_ = keys_curr_;
    for (int vk = 8; vk < 256; ++vk) {
        keys_curr_[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
    // XInput poll — only port 0; matches existing PollHotkeysThread scope.
    // `pad_curr_` keeps the raw 16-bit `wButtons` mask (no triggers) so the
    // existing chord-capture iteration stays valid; trigger pressure and
    // stick deflection are tracked separately for ImGui analog nav.
    XINPUT_STATE st{};
    pad_prev_ = pad_curr_;
    if (XInputGetState(0, &st) == ERROR_SUCCESS) {
        pad_connected_ = true;
        pad_curr_ = st.Gamepad.wButtons;
        auto norm_stick = [](SHORT v, SHORT dz) {
            int a = (v >= 0) ? v : -v;
            if (a <= dz) return 0.0f;
            float n = (a - dz) / static_cast<float>(32767 - dz);
            if (n > 1.0f) n = 1.0f;
            return (v < 0) ? -n : n;
        };
        pad_lx_ = norm_stick(st.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        pad_ly_ = norm_stick(st.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        pad_rx_ = norm_stick(st.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        pad_ry_ = norm_stick(st.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        pad_lt_ = st.Gamepad.bLeftTrigger  / 255.0f;
        pad_rt_ = st.Gamepad.bRightTrigger / 255.0f;
    } else {
        pad_connected_ = false;
        pad_curr_ = 0;
        pad_lx_ = pad_ly_ = pad_rx_ = pad_ry_ = 0.0f;
        pad_lt_ = pad_rt_ = 0.0f;
    }
}

void OsdInput::FeedImGui(ImGuiIO& io, const OsdSurface& surface) {
    // Cursor → per-eye coords. The headset window's client size depends
    // on the active OutputMode (SbS=2W×H, weaved/anaglyph=W×H,
    // FramePacked=W×(2H+gap), VirtualDesktop=2W×2H, etc.). We just
    // normalize the cursor against the actual client rect and remap
    // into the OSD's per-eye dimensions — works uniformly for every
    // presenter without needing per-mode special cases.
    HWND hwnd = static_cast<HWND>(surface.hwnd);
    if (!hwnd) {
        HWND cached = static_cast<HWND>(cached_hwnd_);
        if (!cached || !IsWindow(cached)) {
            cached = FindWindowW(L"VRto3D_PresentWindow", nullptr);
            cached_hwnd_ = cached;
        }
        hwnd = cached;
    }
    if (hwnd) {
        POINT p;
        RECT  client{};
        if (GetCursorPos(&p) && GetClientRect(hwnd, &client)) {
            ScreenToClient(hwnd, &p);
            const int cw = client.right  - client.left;
            const int ch = client.bottom - client.top;
            if (cw > 0 && ch > 0 && surface.eye_w > 0 && surface.eye_h > 0) {
                float u = static_cast<float>(p.x) / static_cast<float>(cw);
                float v = static_cast<float>(p.y) / static_cast<float>(ch);
                if (surface.layout == StereoLayout::HorizontalSbs) {
                    // Fold right half back so clicks on either eye-half
                    // land on the same OSD coordinate.
                    u = (u >= 0.5f) ? (u - 0.5f) * 2.0f : u * 2.0f;
                } else if (surface.layout == StereoLayout::VerticalTab) {
                    v = (v >= 0.5f) ? (v - 0.5f) * 2.0f : v * 2.0f;
                }
                if (u < 0.0f) u = 0.0f; if (u > 1.0f) u = 1.0f;
                if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
                io.AddMousePosEvent(u * static_cast<float>(surface.eye_w),
                                    v * static_cast<float>(surface.eye_h));
            }
        }
    }

    // Mouse buttons — read from the LL hook, not GetAsyncKeyState which
    // returns 0 unless the calling process owns the foreground window.
    io.AddMouseButtonEvent(0, g_mb_left.load());
    io.AddMouseButtonEvent(1, g_mb_right.load());
    io.AddMouseButtonEvent(2, g_mb_middle.load());

    // Mouse wheel — drained from the global hook.
    const float wy = g_wheel_delta_y.exchange(0.0f);
    const float wx = g_wheel_delta_x.exchange(0.0f);
    if (wy != 0.0f || wx != 0.0f) io.AddMouseWheelEvent(wx, wy);

    // Modifiers.
    io.AddKeyEvent(ImGuiMod_Ctrl,  keys_curr_[VK_CONTROL]);
    io.AddKeyEvent(ImGuiMod_Shift, keys_curr_[VK_SHIFT]);
    io.AddKeyEvent(ImGuiMod_Alt,   keys_curr_[VK_MENU]);
    io.AddKeyEvent(ImGuiMod_Super, keys_curr_[VK_LWIN] || keys_curr_[VK_RWIN]);

    // Edges for everything else — only fire on transitions. On press, also
    // translate to a printable Unicode character via the active layout so
    // ImGui::InputText receives WM_CHAR-equivalent text input (we don't
    // have a real message pump here).
    BYTE kbd_state[256];
    bool kbd_state_ok = (GetKeyboardState(kbd_state) != 0);
    HKL  kbd_layout = GetKeyboardLayout(0);
    for (int vk = 8; vk < 256; ++vk) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
            vk == VK_CONTROL || vk == VK_SHIFT   || vk == VK_MENU)
            continue;
        if (keys_curr_[vk] != keys_prev_[vk]) {
            ImGuiKey ik = VkToImGuiKey(vk);
            if (ik != ImGuiKey_None) io.AddKeyEvent(ik, keys_curr_[vk]);

            // Character input on press only.
            if (keys_curr_[vk] && !keys_prev_[vk] && kbd_state_ok &&
                !keys_curr_[VK_CONTROL] && !keys_curr_[VK_MENU]) {
                UINT scan = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, kbd_layout);
                wchar_t wbuf[8] = {};
                int n = ToUnicodeEx(static_cast<UINT>(vk), scan, kbd_state,
                                    wbuf, 8, 0, kbd_layout);
                // n > 0: characters produced; n < 0: dead key (skip);
                // n == 0: no translation. Filter out control characters.
                if (n > 0) {
                    for (int k = 0; k < n; ++k) {
                        unsigned cp = static_cast<unsigned>(wbuf[k]);
                        if (cp >= 0x20 && cp != 0x7F) io.AddInputCharacter(cp);
                    }
                }
            }
        }
    }

    // XInput → ImGui gamepad nav keys. ImGui ignores gamepad input unless
    // both ConfigFlags_NavEnableGamepad AND BackendFlags_HasGamepad are set
    // — toggle the backend bit with controller-connected so keyboard-only
    // users don't get phantom nav highlights when there's no pad attached.
    // Defaults: A=activate, B=cancel, X=text-input, Y=tweak-slower, D-pad +
    // left stick navigate, shoulders page through tabs, right stick scrolls.
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

    // Capture sampling.
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
