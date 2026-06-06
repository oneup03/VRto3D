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

#include <array>
#include <cstdint>
#include <set>
#include <string>

struct ImGuiIO;

namespace vrto3d::osd {

// How the active presenter lays out the SBS frame on the user's screen.
// Determines how cursor coords are folded back into per-eye OSD space.
enum class StereoLayout {
    Mono,        // single composited image (LeiaSR weaved, Anaglyph,
                 // RowInterlaced, ColInterlaced, Checkerboard, NvStereoDx9):
                 // OSD appears once on screen — simple normalize-and-scale.
    HorizontalSbs, // window is 2*eye_w × eye_h, OSD appears in both halves
                   // (SbS, VirtualDesktop, DualDisplay, DualDisplayFlip).
    VerticalTab,   // window is eye_w × 2*eye_h (or +gap), OSD appears in
                   // top + bottom (TaB, FramePacked*).
};

// Identifies the surface the OSD is rendering into. Used to map mouse
// coordinates from screen space to per-eye pixel space.
struct OsdSurface {
    int eye_w = 0;       // per-eye width  (== osd_tex_ width)
    int eye_h = 0;       // per-eye height (== osd_tex_ height)
    void* hwnd = nullptr; // Win32 HWND of the headset window (for client-area mouse mapping)
    StereoLayout layout = StereoLayout::Mono;
};

// One-shot capture result returned by OsdInput::PollCapture().
// `key_name` is a raw VK_*/XINPUT_GAMEPAD_* string (or '+'-joined combo) that
// VRto3DLib's VirtualKeyMappings / XInputMappings can resolve back to a code.
struct CapturedKey {
    bool        valid = false;
    std::string key_name;
};

// Win32 keyboard + XInput pump for the OSD. Polls global state each frame
// (the SteamVR driver is never the foreground process), feeds ImGuiIO, and
// supports a one-shot key-capture mode for the User Hotkeys picker.
class OsdInput {
public:
    OsdInput();
    ~OsdInput();

    OsdInput(const OsdInput&) = delete;
    OsdInput& operator=(const OsdInput&) = delete;

    // Refresh internal key/mouse/wheel state. Call once per frame.
    void Poll();

    // Push refreshed state into ImGui's IO. Caller must have already
    // called ImGui::SetCurrentContext().
    void FeedImGui(ImGuiIO& io, const OsdSurface& surface);

    // Returns true the first frame `vk` transitions key-down. Pass a Win32
    // VK_* code for keyboard keys or an XINPUT_GAMEPAD_* mask for buttons.
    bool WasPressed(int vk) const;

    // True while `vk` is currently held.
    bool IsHeld(int vk) const;

    // Begin a one-shot key-capture session for the User Hotkeys picker.
    // `combo == true` accumulates every key/button held simultaneously and
    // commits the captured combo (joined with '+') only after the user
    // releases everything — required for XInput chord bindings like
    // "XINPUT_GAMEPAD_LEFT_SHOULDER+XINPUT_GAMEPAD_RIGHT_SHOULDER".
    void BeginCapture(bool combo = false);
    void CancelCapture();
    CapturedKey PollCapture();
    bool IsCapturing() const { return capturing_; }

    // Toggle the always-on global mouse hook. Cheap when stable; called by
    // OsdRenderer when the menu visibility changes so we don't keep a
    // WH_MOUSE_LL hook installed (and dispatched to our process for every
    // system-wide mouse event) when the OSD isn't actually consuming clicks.
    void SetMouseHookActive(bool active);

private:
    std::array<bool, 256> keys_curr_{};
    std::array<bool, 256> keys_prev_{};
    uint16_t              pad_curr_ = 0;
    uint16_t              pad_prev_ = 0;
    // Analog axes from XInput port 0, normalized to [-1,1] for sticks and
    // [0,1] for triggers. Fed to ImGui as analog gamepad keys so stick nav
    // and analog scroll work in the OSD menu.
    float                 pad_lx_ = 0.0f;
    float                 pad_ly_ = 0.0f;
    float                 pad_rx_ = 0.0f;
    float                 pad_ry_ = 0.0f;
    float                 pad_lt_ = 0.0f;
    float                 pad_rt_ = 0.0f;
    bool                  pad_connected_ = false;
    bool                  capturing_ = false;
    bool                  capture_combo_ = false;
    bool                  had_any_press_ = false;
    std::set<std::string> capture_set_;
    CapturedKey           captured_;
    void*                 cached_hwnd_ = nullptr; // HWND
    bool                  hook_active_ = false;
};

} // namespace vrto3d::osd
