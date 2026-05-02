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

#include <atomic>
#include <functional>
#include <memory>
#include <string>

class StereoDisplayComponent;

namespace vrto3d::osd {

class IOsdInput;

// Callbacks the menu uses to perform actions that live outside the
// StereoDisplayComponent's API surface (profile save/load, file-manager
// shell-out, etc). All are invoked on the OSD render thread.
struct MenuCallbacks {
    // Save the live config to "{app_name}_config.json". Empty app_name means
    // there is no game-specific profile (button should be disabled).
    std::function<void(std::string toast)> save_game_profile;
    // Save the live config to "default_config.json".
    std::function<void(std::string toast)> save_default_profile;
    // Reload "{app_name}_config.json" into the live config.
    std::function<void(std::string toast)> reload_game_profile;
    // Reload "default_config.json" into the live config.
    std::function<void(std::string toast)> reload_default_profile;
    // Reset projection (mirrors Ctrl+Shift+F3).
    std::function<void()> reset_projection;
    // Snap the current LeiaSR head pose as the neutral zero. No-op when the
    // active presenter isn't LeiaSR.
    std::function<void()> calibrate_leiasr_head;
    // Toggle "always on top" (mirrors Ctrl+F8).
    std::function<void()> toggle_always_on_top;
    // True iff the headset window is currently always-on-top.
    std::function<bool()> always_on_top;
    // Open the vrto3d Steam config folder in the OS file manager.
    std::function<void()> open_config_folder;
    // Re-assert input focus on the connected game window. Called when the
    // OSD menu closes — while the menu was open the VR window held focus,
    // and the user expects keystrokes to land back in the game.
    std::function<void()> request_game_focus;
    // Kick off a download of the latest vrto3d_profiles.zip from GitHub
    // releases and extract into the config folder. Reports progress via toast.
    std::function<void()> download_latest_profiles;

    // Auto-depth feature: toggle + comfort-target slider (fraction of one
    // eye's width). The Stereo tab binds these to read/write the live
    // StereoDisplayComponent state.
    std::function<bool()>      get_auto_depth_enabled;
    std::function<void(bool)>  set_auto_depth_enabled;
    std::function<float()>     get_auto_depth_target;
    std::function<void(float)> set_auto_depth_target;
    std::function<float()>     get_auto_depth_smoothing;
    std::function<void(float)> set_auto_depth_smoothing;
};

// Renders the 4-tab configuration menu and the persistent chrome (title bar +
// footer). All ImGui calls happen between OsdRenderer's NewFrame/Render
// bracket — this class only contributes widget code.
class OsdMenu {
public:
    OsdMenu(StereoDisplayComponent* component, MenuCallbacks callbacks);
    ~OsdMenu();

    void SetVisible(bool visible);
    void Toggle();
    bool Visible() const;

    // Returns the current toast string set by SetText, or empty if expired.
    // Owned + decayed by OsdRenderer; menu doesn't draw the toast itself.

    // Build the menu's ImGui widgets. Called once per frame between
    // ImGui::NewFrame and ImGui::Render.
    void BuildUI(IOsdInput& input);

    // Provide the full version string for the title bar + System tab.
    void SetVersion(std::string version);

    // Provide the current loaded-app name (`app_name_` from
    // hmd_device_driver.cpp). Used for the footer's game-profile save button
    // label and the Stereo tab's read-only profile readout.
    void SetAppName(std::string app_name);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vrto3d::osd
