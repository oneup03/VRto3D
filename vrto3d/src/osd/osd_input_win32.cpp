/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include "osd/osd_input.h"

#include <array>
#include <atomic>
#include <chrono>
#include <set>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <Xinput.h>

#include "imgui.h"

#include "osd/osd_keymap.h"

namespace vrto3d::osd {

namespace {

// Shared low-level mouse hook for wheel + button events. Installed once on
// first Win32Input construction; removed on last destruction.
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

class Win32Input final : public IOsdInput {
public:
    Win32Input() {
        EnsureMouseHook();
        keys_curr_.fill(false);
        keys_prev_.fill(false);
        pad_curr_  = pad_prev_ = 0;
    }
    ~Win32Input() override {
        ReleaseMouseHook();
    }

    void Poll() override {
        keys_prev_ = keys_curr_;
        for (int vk = 8; vk < 256; ++vk) {
            keys_curr_[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        }
        // XInput poll — only port 0; matches existing PollHotkeysThread scope.
        XINPUT_STATE st{};
        pad_prev_ = pad_curr_;
        if (XInputGetState(0, &st) == ERROR_SUCCESS) {
            pad_curr_ = st.Gamepad.wButtons;
        } else {
            pad_curr_ = 0;
        }
    }

    void FeedImGui(ImGuiIO& io, const OsdSurface& surface) override {
        // Cursor → per-eye coords. The headset window's client size depends
        // on the active OutputMode (SbS=2W×H, weaved/anaglyph=W×H,
        // FramePacked=W×(2H+gap), VirtualDesktop=2W×2H, etc.). We just
        // normalize the cursor against the actual client rect and remap
        // into the OSD's per-eye dimensions — works uniformly for every
        // presenter without needing per-mode special cases.
        HWND hwnd = static_cast<HWND>(surface.hwnd);
        if (!hwnd) {
            if (!cached_hwnd_ || !IsWindow(cached_hwnd_)) {
                cached_hwnd_ = FindWindowW(L"VRto3D_PresentWindow", nullptr);
            }
            hwnd = cached_hwnd_;
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
                    // Mono: normalize + scale (handles LeiaSR weave, anaglyph,
                    // interlaced, NvStereo, etc — single image on screen).
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

        // Capture sampling.
        if (capturing_) {
            if (capture_combo_) {
                // Combo mode: accumulate everything held until release-all.
                bool any_held = false;
                for (int vk = 8; vk < 256; ++vk) {
                    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
                    if (keys_curr_[vk]) {
                        any_held = true;
                        std::string name = FromWin32Vk(vk);
                        if (!name.empty()) capture_set_.insert(name);
                    }
                }
                for (uint32_t bit = 1; bit; bit <<= 1) {
                    if (pad_curr_ & bit) {
                        any_held = true;
                        std::string name = FromWin32PadMask(bit);
                        if (!name.empty()) capture_set_.insert(name);
                    }
                }
                if (any_held) had_any_press_ = true;
                if (had_any_press_ && !any_held && !capture_set_.empty()) {
                    // Commit on release-all.
                    std::string out;
                    for (const auto& n : capture_set_) {
                        if (!out.empty()) out += '+';
                        out += n;
                    }
                    captured_.valid = true;
                    captured_.portable_name = std::move(out);
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
                        captured_.valid = true;
                        captured_.portable_name = FromWin32Vk(vk);
                        capturing_ = false;
                        break;
                    }
                }
                const WORD newly_down = pad_curr_ & ~pad_prev_;
                if (capturing_ && newly_down) {
                    for (uint32_t bit = 1; bit; bit <<= 1) {
                        if (newly_down & bit) {
                            std::string name = FromWin32PadMask(bit);
                            if (!name.empty()) {
                                captured_.valid = true;
                                captured_.portable_name = std::move(name);
                                capturing_ = false;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    bool WasPressed(const char* portable_name) override {
        int32_t vk = 0; uint32_t pad = 0;
        if (!ResolveWin32(portable_name, vk, pad)) return false;
        if (vk)  return keys_curr_[vk]  && !keys_prev_[vk];
        if (pad) return (pad_curr_ & pad) && !(pad_prev_ & pad);
        return false;
    }

    bool IsHeld(const char* portable_name) override {
        int32_t vk = 0; uint32_t pad = 0;
        if (!ResolveWin32(portable_name, vk, pad)) return false;
        if (vk)  return keys_curr_[vk];
        if (pad) return (pad_curr_ & pad) != 0;
        return false;
    }

    void BeginCapture(bool combo = false) override {
        capturing_      = true;
        capture_combo_  = combo;
        captured_       = {};
        capture_set_.clear();
        had_any_press_  = false;
    }
    void CancelCapture() override {
        capturing_      = false;
        capture_combo_  = false;
        captured_       = {};
        capture_set_.clear();
        had_any_press_  = false;
    }
    bool IsCapturing() const override { return capturing_; }
    CapturedKey PollCapture() override {
        if (captured_.valid) {
            auto out = captured_;
            captured_ = {};
            return out;
        }
        return {};
    }

private:
    std::array<bool, 256> keys_curr_;
    std::array<bool, 256> keys_prev_;
    WORD                  pad_curr_ = 0;
    WORD                  pad_prev_ = 0;
    bool                  capturing_ = false;
    bool                  capture_combo_ = false;
    bool                  had_any_press_ = false;
    std::set<std::string> capture_set_;
    CapturedKey           captured_;
    HWND                  cached_hwnd_ = nullptr;
};

} // namespace

std::unique_ptr<IOsdInput> CreateOsdInput() {
    return std::make_unique<Win32Input>();
}

} // namespace vrto3d::osd
