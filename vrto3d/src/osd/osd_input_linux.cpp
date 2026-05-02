/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Linux IOsdInput backend.
 *
 * Build modes:
 *   * VRTO3D_HAVE_LIBEVDEV defined → real implementation: opens every
 *     /dev/input/event* keyboard device via libevdev, feeds ImGuiIO with key
 *     and character events, supports the click-to-capture key picker. Mouse
 *     position is intentionally NOT pumped (v1 — would need a display-server
 *     channel that we don't yet have; the menu is keyboard-driven on Linux).
 *   * Without libevdev → no-op stub: OSD renders (toast text + menu chrome)
 *     but the menu can't be interacted with. Matches the existing Linux
 *     state where PollHotkeysThread is also stubbed.
 */

#include "osd/osd_input.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "imgui.h"

#include "osd/osd_keymap.h"

#ifdef VRTO3D_HAVE_LIBEVDEV
#  include <fcntl.h>
#  include <unistd.h>
#  include <dirent.h>
#  include <linux/input.h>
#  include <libevdev/libevdev.h>
#endif

namespace vrto3d::osd {

namespace {

#ifdef VRTO3D_HAVE_LIBEVDEV

// evdev KEY_* → portable name (subset; covers the same set as the Win32
// keymap). Anything not in this table is silently ignored. Defined as a
// function-static so the table is initialized once on first use.
struct EvdevEntry { int key; const char* portable; ImGuiKey imgui_key; char ch; };
const EvdevEntry kKeys[] = {
    // Function keys
    { KEY_F1,  "F1",  ImGuiKey_F1,  0 }, { KEY_F2,  "F2",  ImGuiKey_F2,  0 },
    { KEY_F3,  "F3",  ImGuiKey_F3,  0 }, { KEY_F4,  "F4",  ImGuiKey_F4,  0 },
    { KEY_F5,  "F5",  ImGuiKey_F5,  0 }, { KEY_F6,  "F6",  ImGuiKey_F6,  0 },
    { KEY_F7,  "F7",  ImGuiKey_F7,  0 }, { KEY_F8,  "F8",  ImGuiKey_F8,  0 },
    { KEY_F9,  "F9",  ImGuiKey_F9,  0 }, { KEY_F10, "F10", ImGuiKey_F10, 0 },
    { KEY_F11, "F11", ImGuiKey_F11, 0 }, { KEY_F12, "F12", ImGuiKey_F12, 0 },
    // Numpad
    { KEY_KP0, "Numpad0", ImGuiKey_Keypad0, '0' },
    { KEY_KP1, "Numpad1", ImGuiKey_Keypad1, '1' },
    { KEY_KP2, "Numpad2", ImGuiKey_Keypad2, '2' },
    { KEY_KP3, "Numpad3", ImGuiKey_Keypad3, '3' },
    { KEY_KP4, "Numpad4", ImGuiKey_Keypad4, '4' },
    { KEY_KP5, "Numpad5", ImGuiKey_Keypad5, '5' },
    { KEY_KP6, "Numpad6", ImGuiKey_Keypad6, '6' },
    { KEY_KP7, "Numpad7", ImGuiKey_Keypad7, '7' },
    { KEY_KP8, "Numpad8", ImGuiKey_Keypad8, '8' },
    { KEY_KP9, "Numpad9", ImGuiKey_Keypad9, '9' },
    // Navigation
    { KEY_HOME,    "Home",   ImGuiKey_Home,      0 },
    { KEY_END,     "End",    ImGuiKey_End,       0 },
    { KEY_INSERT,  "Insert", ImGuiKey_Insert,    0 },
    { KEY_DELETE,  "Delete", ImGuiKey_Delete,    0 },
    { KEY_PAGEUP,  "PgUp",   ImGuiKey_PageUp,    0 },
    { KEY_PAGEDOWN,"PgDn",   ImGuiKey_PageDown,  0 },
    { KEY_UP,      "Up",     ImGuiKey_UpArrow,   0 },
    { KEY_DOWN,    "Down",   ImGuiKey_DownArrow, 0 },
    { KEY_LEFT,    "Left",   ImGuiKey_LeftArrow, 0 },
    { KEY_RIGHT,   "Right",  ImGuiKey_RightArrow,0 },
    // Misc
    { KEY_TAB,       "Tab",       ImGuiKey_Tab,       0    },
    { KEY_ENTER,     "Enter",     ImGuiKey_Enter,     '\n' },
    { KEY_KPENTER,   "Enter",     ImGuiKey_KeypadEnter,'\n' },
    { KEY_ESC,       "Escape",    ImGuiKey_Escape,    0    },
    { KEY_SPACE,     "Space",     ImGuiKey_Space,     ' '  },
    { KEY_BACKSPACE, "Backspace", ImGuiKey_Backspace, 0    },
    // Letters (Shift-aware ASCII added at runtime)
    { KEY_A,'A',ImGuiKey_A,'a' }, { KEY_B,'B',ImGuiKey_B,'b' },
    { KEY_C,'C',ImGuiKey_C,'c' }, { KEY_D,'D',ImGuiKey_D,'d' },
    { KEY_E,'E',ImGuiKey_E,'e' }, { KEY_F,'F',ImGuiKey_F,'f' },
    { KEY_G,'G',ImGuiKey_G,'g' }, { KEY_H,'H',ImGuiKey_H,'h' },
    { KEY_I,'I',ImGuiKey_I,'i' }, { KEY_J,'J',ImGuiKey_J,'j' },
    { KEY_K,'K',ImGuiKey_K,'k' }, { KEY_L,'L',ImGuiKey_L,'l' },
    { KEY_M,'M',ImGuiKey_M,'m' }, { KEY_N,'N',ImGuiKey_N,'n' },
    { KEY_O,'O',ImGuiKey_O,'o' }, { KEY_P,'P',ImGuiKey_P,'p' },
    { KEY_Q,'Q',ImGuiKey_Q,'q' }, { KEY_R,'R',ImGuiKey_R,'r' },
    { KEY_S,'S',ImGuiKey_S,'s' }, { KEY_T,'T',ImGuiKey_T,'t' },
    { KEY_U,'U',ImGuiKey_U,'u' }, { KEY_V,'V',ImGuiKey_V,'v' },
    { KEY_W,'W',ImGuiKey_W,'w' }, { KEY_X,'X',ImGuiKey_X,'x' },
    { KEY_Y,'Y',ImGuiKey_Y,'y' }, { KEY_Z,'Z',ImGuiKey_Z,'z' },
    // Digits
    { KEY_0,"0",ImGuiKey_0,'0' }, { KEY_1,"1",ImGuiKey_1,'1' },
    { KEY_2,"2",ImGuiKey_2,'2' }, { KEY_3,"3",ImGuiKey_3,'3' },
    { KEY_4,"4",ImGuiKey_4,'4' }, { KEY_5,"5",ImGuiKey_5,'5' },
    { KEY_6,"6",ImGuiKey_6,'6' }, { KEY_7,"7",ImGuiKey_7,'7' },
    { KEY_8,"8",ImGuiKey_8,'8' }, { KEY_9,"9",ImGuiKey_9,'9' },
    // Common punctuation needed for InputText
    { KEY_COMMA,     ",",ImGuiKey_Comma,    ',' },
    { KEY_DOT,       ".",ImGuiKey_Period,   '.' },
    { KEY_SLASH,     "/",ImGuiKey_Slash,    '/' },
    { KEY_SEMICOLON, ";",ImGuiKey_Semicolon,';' },
    { KEY_MINUS,     "-",ImGuiKey_Minus,    '-' },
    { KEY_EQUAL,     "=",ImGuiKey_Equal,    '=' },
    { KEY_LEFTBRACE, "[",ImGuiKey_LeftBracket, '[' },
    { KEY_RIGHTBRACE,"]",ImGuiKey_RightBracket,']' },
    { KEY_BACKSLASH, "\\",ImGuiKey_Backslash,   '\\' },
    { KEY_GRAVE,     "`",ImGuiKey_GraveAccent,  '`' },
    { KEY_APOSTROPHE,"'",ImGuiKey_Apostrophe,   '\'' },
};

const EvdevEntry* LookupEvdev(int key) {
    for (auto& e : kKeys) if (e.key == key) return &e;
    return nullptr;
}

class LinuxInput final : public IOsdInput {
public:
    LinuxInput() {
        keys_curr_.assign(KEY_MAX + 1, false);
        keys_prev_.assign(KEY_MAX + 1, false);
        OpenDevices();
    }
    ~LinuxInput() override { CloseDevices(); }

    void Poll() override {
        keys_prev_ = keys_curr_;
        // Drain every device of pending events.
        for (auto& d : devices_) {
            input_event ev{};
            int rc;
            while ((rc = libevdev_next_event(d.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) == 0) {
                if (ev.type == EV_KEY && ev.code <= KEY_MAX) {
                    if (ev.value == 1 || ev.value == 2) keys_curr_[ev.code] = true;
                    else if (ev.value == 0)             keys_curr_[ev.code] = false;
                }
            }
            // LIBEVDEV_READ_STATUS_SYNC means the kernel dropped events; resync.
            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                while (libevdev_next_event(d.dev, LIBEVDEV_READ_FLAG_SYNC, &ev) == 0) {}
            }
        }
    }

    void FeedImGui(ImGuiIO& io, const OsdSurface& /*surface*/) override {
        // Modifier flags.
        const bool shift = keys_curr_[KEY_LEFTSHIFT] || keys_curr_[KEY_RIGHTSHIFT];
        const bool ctrl  = keys_curr_[KEY_LEFTCTRL]  || keys_curr_[KEY_RIGHTCTRL];
        const bool alt   = keys_curr_[KEY_LEFTALT]   || keys_curr_[KEY_RIGHTALT];
        const bool super = keys_curr_[KEY_LEFTMETA]  || keys_curr_[KEY_RIGHTMETA];
        io.AddKeyEvent(ImGuiMod_Ctrl,  ctrl);
        io.AddKeyEvent(ImGuiMod_Shift, shift);
        io.AddKeyEvent(ImGuiMod_Alt,   alt);
        io.AddKeyEvent(ImGuiMod_Super, super);

        // Key edges + character input on press.
        for (auto& e : kKeys) {
            const bool was = keys_prev_[e.key];
            const bool now = keys_curr_[e.key];
            if (was == now) continue;
            if (e.imgui_key != ImGuiKey_None) io.AddKeyEvent(e.imgui_key, now);
            if (now && !ctrl && !alt && e.ch != 0) {
                char c = e.ch;
                if (shift && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                io.AddInputCharacter(static_cast<unsigned>(c));
            }
        }

        // Capture sampling.
        if (capturing_) {
            if (capture_combo_) {
                bool any_held = false;
                for (auto& e : kKeys) {
                    if (keys_curr_[e.key]) {
                        any_held = true;
                        capture_set_.insert(std::string(e.portable));
                    }
                }
                if (any_held) had_any_press_ = true;
                if (had_any_press_ && !any_held && !capture_set_.empty()) {
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
                for (auto& e : kKeys) {
                    if (keys_curr_[e.key] && !keys_prev_[e.key]) {
                        captured_.valid = true;
                        captured_.portable_name = std::string(e.portable);
                        capturing_ = false;
                        break;
                    }
                }
            }
        }
    }

    bool WasPressed(const char* portable_name) override {
        for (auto& e : kKeys) {
            if (std::strcmp(e.portable, portable_name) == 0)
                return keys_curr_[e.key] && !keys_prev_[e.key];
        }
        return false;
    }
    bool IsHeld(const char* portable_name) override {
        for (auto& e : kKeys) {
            if (std::strcmp(e.portable, portable_name) == 0)
                return keys_curr_[e.key];
        }
        return false;
    }

    void BeginCapture(bool combo = false) override {
        capturing_ = true; capture_combo_ = combo; captured_ = {};
        capture_set_.clear(); had_any_press_ = false;
    }
    void CancelCapture() override {
        capturing_ = false; capture_combo_ = false; captured_ = {};
        capture_set_.clear(); had_any_press_ = false;
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
    struct DeviceSlot {
        int                fd  = -1;
        struct libevdev*   dev = nullptr;
    };

    void OpenDevices() {
        DIR* d = opendir("/dev/input");
        if (!d) {
            std::fprintf(stderr,
                "VRto3D OSD: /dev/input not readable; menu will be uninteractive. "
                "Add the user to the 'input' group and re-login.\n");
            return;
        }
        dirent* ent;
        while ((ent = readdir(d))) {
            if (std::strncmp(ent->d_name, "event", 5) != 0) continue;
            std::string path = std::string("/dev/input/") + ent->d_name;
            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            struct libevdev* dev = nullptr;
            if (libevdev_new_from_fd(fd, &dev) < 0) {
                close(fd);
                continue;
            }
            // Only keep devices that report keyboard EV_KEY events.
            if (libevdev_has_event_type(dev, EV_KEY) &&
                libevdev_has_event_code(dev, EV_KEY, KEY_A)) {
                devices_.push_back({ fd, dev });
            } else {
                libevdev_free(dev);
                close(fd);
            }
        }
        closedir(d);
        std::fprintf(stderr,
            "VRto3D OSD: opened %zu evdev keyboard device(s)\n", devices_.size());
    }

    void CloseDevices() {
        for (auto& d : devices_) {
            if (d.dev) libevdev_free(d.dev);
            if (d.fd >= 0) close(d.fd);
        }
        devices_.clear();
    }

    std::vector<bool>          keys_curr_;
    std::vector<bool>          keys_prev_;
    std::vector<DeviceSlot>    devices_;
    bool                       capturing_ = false;
    bool                       capture_combo_ = false;
    bool                       had_any_press_ = false;
    std::set<std::string>      capture_set_;
    CapturedKey                captured_;
};

#else  // !VRTO3D_HAVE_LIBEVDEV

// Stub backend — OSD still renders but receives no input. Linux without
// libevdev installed.
class LinuxInput final : public IOsdInput {
public:
    LinuxInput() {
        std::fprintf(stderr,
            "VRto3D OSD: libevdev not available at build time; menu will be "
            "uninteractive. Install libevdev-dev and rebuild.\n");
    }
    void Poll() override {}
    void FeedImGui(ImGuiIO&, const OsdSurface&) override {}
    bool WasPressed(const char*) override { return false; }
    bool IsHeld(const char*) override { return false; }
    void BeginCapture(bool = false) override {}
    void CancelCapture() override {}
    bool IsCapturing() const override { return false; }
    CapturedKey PollCapture() override { return {}; }
};

#endif  // VRTO3D_HAVE_LIBEVDEV

} // namespace

std::unique_ptr<IOsdInput> CreateOsdInput() {
    return std::make_unique<LinuxInput>();
}

} // namespace vrto3d::osd
