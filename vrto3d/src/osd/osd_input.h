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
#include <memory>
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

// One-shot capture result returned by IOsdInput::PollCapture().
struct CapturedKey {
    bool         valid = false;       // false if no key/button captured this poll
    std::string  portable_name;       // canonical portable name (e.g. "Numpad1", "Pad.Guide")
};

// Cross-platform input pump. Concrete implementations live in
// osd_input_win32.cpp / osd_input_linux.cpp and are selected at build time.
class IOsdInput {
public:
    virtual ~IOsdInput() = default;

    // Refresh internal key/mouse/wheel state. Call once per frame.
    virtual void Poll() = 0;

    // Push refreshed state into ImGui's IO. Caller must have already
    // called ImGui::SetCurrentContext().
    virtual void FeedImGui(ImGuiIO& io, const OsdSurface& surface) = 0;

    // Returns true the first frame `portable_name` transitions key-down. Used
    // by the hotkey poll loop to detect chords like Ctrl+Home / Ctrl+F1.
    // Implementations should track edge state across Poll() calls.
    virtual bool WasPressed(const char* portable_name) = 0;

    // True while `portable_name` is currently held.
    virtual bool IsHeld(const char* portable_name) = 0;

    // Begin a one-shot key-capture session for the User Hotkeys picker.
    // While capturing, normal ImGui input may still flow but the next
    // discrete key/button press is recorded and returned by PollCapture().
    // `combo == true` accumulates every key/button held simultaneously and
    // commits the captured combo (joined with '+') only after the user
    // releases everything — required for XInput chord bindings like
    // "Pad.LBumper+Pad.RBumper".
    virtual void BeginCapture(bool combo = false) = 0;
    virtual void CancelCapture() = 0;
    virtual CapturedKey PollCapture() = 0;
    virtual bool IsCapturing() const = 0;
};

// Factory — implemented in the per-platform .cpp.
std::unique_ptr<IOsdInput> CreateOsdInput();

} // namespace vrto3d::osd
