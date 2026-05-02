/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include "osd/osd_menu.h"

#include <atomic>
#include <string>
#include <utility>

#include "imgui.h"

#include "osd/osd_input.h"
#include "hmd_device_driver.h"
#include "platform/platform.h"
#include "vrto3dlib/stereo_config.h"
#include "vrto3dlib/key_mappings.h"

#include <sstream>

namespace vrto3d::osd {

struct OsdMenu::Impl {
    StereoDisplayComponent* component = nullptr;
    MenuCallbacks           callbacks;
    std::atomic<bool>       visible{false};

    std::string version;
    std::string app_name;

    // Cached monitor enumeration for the System tab combo. Refreshed whenever
    // the user opens the menu (cheap, but not free).
    std::vector<platform::MonitorInfo> monitors;
    std::vector<std::string>           monitor_labels;
    bool                               monitors_refreshed = false;
    void RefreshMonitors();

    void DrawStereoTab();
    void DrawUserHotkeysTab(IOsdInput& input);
    void DrawTrackingTab();
    void DrawSystemTab();
    void DrawFooter();
    void DrawTitleChrome();

    // State for the click-to-capture key picker. -1 row + slot 0 = none.
    int  capture_row_  = -1;
    int  capture_slot_ = 0;          // 0 = load, 1 = store
    bool capture_combo_pending_ = false;
};

OsdMenu::OsdMenu(StereoDisplayComponent* component, MenuCallbacks callbacks)
    : impl_(std::make_unique<Impl>()) {
    impl_->component = component;
    impl_->callbacks = std::move(callbacks);
}

OsdMenu::~OsdMenu() = default;

void OsdMenu::SetVisible(bool visible) {
    impl_->visible.store(visible);
    if (visible) impl_->monitors_refreshed = false;
}
void OsdMenu::Toggle() {
    bool now = !impl_->visible.load();
    impl_->visible.store(now);
    if (now) impl_->monitors_refreshed = false;
}
bool OsdMenu::Visible() const          { return impl_->visible.load(); }

void OsdMenu::SetVersion(std::string v)  { impl_->version  = std::move(v); }
void OsdMenu::SetAppName(std::string a)  { impl_->app_name = std::move(a); }

void OsdMenu::BuildUI(IOsdInput& input) {
    auto& s = *impl_;
    if (!s.visible.load() || !s.component) return;

    ImGui::SetNextWindowSizeConstraints(ImVec2(720, 520), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_FirstUseEver);

    const std::string title = "VRto3D " + s.version + "###vrto3d_menu";
    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse;
    bool open = true;
    if (!ImGui::Begin(title.c_str(), &open, wflags)) {
        ImGui::End();
        if (!open) s.visible.store(false);
        return;
    }
    if (!open) s.visible.store(false);

    s.DrawTitleChrome();

    if (ImGui::BeginTabBar("##vrto3d_tabs")) {
        if (ImGui::BeginTabItem("Stereo"))       { s.DrawStereoTab();         ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("User Hotkeys")) { s.DrawUserHotkeysTab(input); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Tracking"))     { s.DrawTrackingTab();       ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("System"))       { s.DrawSystemTab();        ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    s.DrawFooter();
    ImGui::End();
}

void OsdMenu::Impl::RefreshMonitors() {
    monitors.clear();
    monitor_labels.clear();
    monitors = platform::EnumerateMonitors();
    // Index 0 = "primary / auto"; non-zero = explicit selection.
    monitor_labels.reserve(monitors.size() + 1);
    monitor_labels.emplace_back("0: Primary (auto)");
    for (const auto& m : monitors) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%d: %s  (%ux%u @%.0fHz)%s",
                      m.index, m.device_name.c_str(),
                      m.width, m.height, m.refresh_hz,
                      m.is_primary ? " *" : "");
        monitor_labels.emplace_back(buf);
    }
    monitors_refreshed = true;
}

// ---------------------------------------------------------------------------
// Title bar chrome — rendered as a small row at the top of the menu showing
// the current profile + the always-on-top toggle.
// ---------------------------------------------------------------------------
void OsdMenu::Impl::DrawTitleChrome() {
    ImGui::TextDisabled("Profile:");
    ImGui::SameLine();
    ImGui::TextUnformatted(app_name.empty() ? "(default)" : app_name.c_str());

    if (callbacks.always_on_top && callbacks.toggle_always_on_top) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 200.0f);
        bool on = callbacks.always_on_top();
        if (ImGui::Checkbox("Always on Top", &on)) {
            callbacks.toggle_always_on_top();
        }
    }
    ImGui::Separator();
}

// ---------------------------------------------------------------------------
// Footer — global save/reload buttons present on every tab.
// ---------------------------------------------------------------------------
void OsdMenu::Impl::DrawFooter() {
    ImGui::Separator();

    const float btn_h = 0.0f;  // default height
    const bool have_app = !app_name.empty();

    if (callbacks.save_game_profile) {
        ImGui::BeginDisabled(!have_app);
        const std::string label = have_app
            ? "Save " + app_name + "_config.json"
            : std::string("Save Game Cfg");
        if (ImGui::Button(label.c_str(), ImVec2(0, btn_h))) {
            callbacks.save_game_profile("Saved " + app_name + "_config.json");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (callbacks.save_default_profile) {
        if (ImGui::Button("Save Default Cfg")) {
            callbacks.save_default_profile("Saved default_config.json");
        }
        ImGui::SameLine();
    }
    if (callbacks.reload_game_profile) {
        ImGui::BeginDisabled(!have_app);
        if (ImGui::Button("Reload Game Cfg")) {
            callbacks.reload_game_profile("Reloaded " + app_name + "_config.json");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (callbacks.reload_default_profile) {
        if (ImGui::Button("Reload Default Cfg")) {
            callbacks.reload_default_profile("Reloaded default_config.json");
        }
    }
}

// ---------------------------------------------------------------------------
// Stereo tab — depth, convergence, FoV, projection helpers.
// ---------------------------------------------------------------------------
void OsdMenu::Impl::DrawStereoTab() {
    auto cfg = component->GetConfig();

    float depth = component->GetDepth();
    if (ImGui::SliderFloat("Depth", &depth, 0.0f, 0.5f, "%.3f")) {
        component->AdjustDepth(depth, false);
        // Mirror Ctrl+Shift+F3/F4 — re-sync projection so the new depth
        // takes effect immediately rather than waiting for the next
        // convergence change or profile reload.
        if (callbacks.reset_projection) callbacks.reset_projection();
    }
    ImGui::SameLine(); ImGui::TextDisabled("(Ctrl+F3 / Ctrl+F4)");

    float conv = component->GetConvergence();
    // Reversed range — left = higher numeric (less stereo separation), right
    // = lower numeric (more separation). Matches the Ctrl+F5/F6 convention
    // where "Decrease Convergence" actually raises the numeric value.
    if (ImGui::SliderFloat("Convergence", &conv, 4.0f, 0.001f, "%.3f")) {
        component->AdjustConvergence(conv, false);
    }
    ImGui::SameLine(); ImGui::TextDisabled("(Ctrl+F5 / Ctrl+F6)");

    float fov = component->GetFoV();
    if (ImGui::SliderFloat("FoV", &fov, 40.0f, 120.0f, "%.1f")) {
        component->AdjustFoV(fov);
    }

    if (callbacks.get_auto_depth_enabled && callbacks.set_auto_depth_enabled) {
        bool ad = callbacks.get_auto_depth_enabled();
        if (ImGui::Checkbox("Auto-Depth", &ad)) {
            callbacks.set_auto_depth_enabled(ad);
        }
        ImGui::SameLine(); ImGui::TextDisabled("(Ctrl+F11)");

        if (callbacks.get_auto_depth_target && callbacks.set_auto_depth_target) {
            float target = callbacks.get_auto_depth_target();
            if (ImGui::SliderFloat("Comfort Target", &target, 0.001f, 0.01f, "%.3f")) {
                callbacks.set_auto_depth_target(target);
            }
            ImGui::SameLine(); ImGui::TextDisabled("(fraction of eye width)");
        }
        if (callbacks.get_auto_depth_smoothing && callbacks.set_auto_depth_smoothing) {
            float smoothing = callbacks.get_auto_depth_smoothing();
            if (ImGui::SliderFloat("Smoothing", &smoothing, 0.005f, 0.25f, "%.3f")) {
                callbacks.set_auto_depth_smoothing(smoothing);
            }
            ImGui::SameLine(); ImGui::TextDisabled("(higher = snappier)");
        }
    }

    bool eye_swap = cfg.eye_swap;
    if (ImGui::Checkbox("Swap Eyes", &eye_swap)) {
        cfg.eye_swap = eye_swap;
        component->LoadSettings(cfg);
    }

    float aspect = cfg.aspect_ratio;
    if (ImGui::DragFloat("Aspect Ratio", &aspect, 0.001f, 0.5f, 4.0f, "%.3f")) {
        cfg.aspect_ratio = aspect;
        component->LoadSettings(cfg);
    }
}

// Helpers for the User Hotkeys tab.
namespace {

std::string FloatsToCsv(const std::vector<float>& v, int prec = 3) {
    std::string out;
    char buf[32];
    for (size_t i = 0; i < v.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%.*f", prec, v[i]);
        if (!out.empty()) out += ", ";
        out += buf;
    }
    return out;
}

// Re-derive a hotkey row's runtime fields (user_load_key + load_xinput) from
// its symbolic string. Mirrors the parser in JsonManager::LoadProfileFromJson
// so edits made through the OSD take effect without needing a save+reload.
void ReparseHotkey(const std::string& s, int32_t& code, bool* xinput) {
    code = 0;
    if (xinput) *xinput = false;
    if (s.empty()) return;
    if (VirtualKeyMappings.find(s) != VirtualKeyMappings.end()) {
        code = VirtualKeyMappings[s];
        if (xinput) *xinput = false;
        return;
    }
    if (XInputMappings.find(s) != XInputMappings.end() || s.find('+') != std::string::npos) {
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, '+')) {
            auto it = XInputMappings.find(tok);
            if (it != XInputMappings.end()) code |= it->second;
        }
        if (xinput) *xinput = true;
    }
}

std::vector<float> CsvToFloats(const char* s) {
    std::vector<float> out;
    if (!s) return out;
    const char* p = s;
    while (*p) {
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end == p) { ++p; continue; }
        out.push_back(v);
        p = end;
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// User Hotkeys tab — editable rows + click-to-capture picker.
// ---------------------------------------------------------------------------
void OsdMenu::Impl::DrawUserHotkeysTab(IOsdInput& input) {
    auto cfg = component->GetConfig();
    bool dirty = false;

    ImGui::TextWrapped(
        "Each row maps a Load key to one or more (Depth, Convergence, FoV) presets. "
        "Press the Load key to apply / cycle through the comma-separated presets. "
        "The Store key overwrites the currently-active preset with live values.");
    ImGui::Spacing();

    // Active capture session — drain the input pump.
    if (capture_row_ >= 0) {
        ImGui::TextColored(ImVec4(1, 0.85f, 0.2f, 1),
                            capture_combo_pending_
                                ? "Capturing combo for %s on row %d — press a chord, then release all to commit. (Esc to cancel)"
                                : "Capturing key for %s on row %d — press a key or controller button. (Esc to cancel)",
                            capture_slot_ == 0 ? "Load" : "Store",
                            capture_row_ + 1);

        CapturedKey c = input.PollCapture();
        if (c.valid) {
            const size_t r = static_cast<size_t>(capture_row_);
            if (r < cfg.num_user_settings) {
                if (capture_slot_ == 0) cfg.user_load_str[r]  = c.portable_name;
                else                    cfg.user_store_str[r] = c.portable_name;
                dirty = true;
            }
            capture_row_ = -1;
            capture_combo_pending_ = false;
            input.CancelCapture();
        }
        if (input.WasPressed("Escape")) {
            capture_row_ = -1;
            capture_combo_pending_ = false;
            input.CancelCapture();
        }
        ImGui::Separator();
    }

    static const char* kModes[] = { "switch", "toggle", "hold" };

    if (ImGui::BeginTable("##user_hotkeys", 6,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("#",         ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableSetupColumn("Load",      ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Store",     ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Mode",      ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Depth / Conv / FoV (comma = cycle)", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("",          ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < cfg.num_user_settings; ++i) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));

            ImGui::TableNextColumn();
            ImGui::Text("%zu", i + 1);

            // ----- Load key -----
            ImGui::TableNextColumn();
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", cfg.user_load_str[i].c_str());
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##load", buf, sizeof(buf))) {
                    cfg.user_load_str[i] = buf;
                    dirty = true;
                }
                if (ImGui::Button("Set##load")) {
                    capture_row_  = static_cast<int>(i);
                    capture_slot_ = 0;
                    capture_combo_pending_ = false;
                    input.BeginCapture(false);
                }
                ImGui::SameLine();
                if (ImGui::Button("Combo##load")) {
                    capture_row_  = static_cast<int>(i);
                    capture_slot_ = 0;
                    capture_combo_pending_ = true;
                    input.BeginCapture(true);
                }
            }

            // ----- Store key -----
            ImGui::TableNextColumn();
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", cfg.user_store_str[i].c_str());
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##store", buf, sizeof(buf))) {
                    cfg.user_store_str[i] = buf;
                    dirty = true;
                }
                if (ImGui::Button("Set##store")) {
                    capture_row_  = static_cast<int>(i);
                    capture_slot_ = 1;
                    capture_combo_pending_ = false;
                    input.BeginCapture(false);
                }
                ImGui::SameLine();
                if (ImGui::Button("Combo##store")) {
                    capture_row_  = static_cast<int>(i);
                    capture_slot_ = 1;
                    capture_combo_pending_ = true;
                    input.BeginCapture(true);
                }
            }

            // ----- Mode -----
            ImGui::TableNextColumn();
            {
                int sel = 0;
                for (int k = 0; k < IM_ARRAYSIZE(kModes); ++k)
                    if (cfg.user_type_str[i] == kModes[k]) { sel = k; break; }
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##mode", &sel, kModes, IM_ARRAYSIZE(kModes))) {
                    cfg.user_type_str[i] = kModes[sel];
                    dirty = true;
                }
            }

            // ----- Preset CSVs -----
            ImGui::TableNextColumn();
            {
                auto editCsv = [&](const char* label_id, std::vector<float>& vec, int prec) {
                    char buf[256];
                    std::string csv = FloatsToCsv(vec, prec);
                    std::snprintf(buf, sizeof(buf), "%s", csv.c_str());
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputText(label_id, buf, sizeof(buf),
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                        auto parsed = CsvToFloats(buf);
                        if (!parsed.empty()) {
                            vec = std::move(parsed);
                            dirty = true;
                        }
                    }
                };
                ImGui::Text("Depth");      ImGui::SameLine(80);
                editCsv("##depth", cfg.user_depth[i], 3);
                ImGui::Text("Conv");       ImGui::SameLine(80);
                editCsv("##conv",  cfg.user_convergence[i], 3);
                ImGui::Text("FoV");        ImGui::SameLine(80);
                editCsv("##fov",   cfg.user_fov[i], 1);

                const size_t presets = cfg.user_depth[i].size();
                if (presets > 1) {
                    const size_t cur = i < cfg.user_preset_index.size()
                                        ? cfg.user_preset_index[i] : 0;
                    ImGui::TextDisabled("Cycle: preset %zu of %zu (Enter to apply edits)",
                                        cur + 1, presets);
                } else {
                    ImGui::TextDisabled("Enter to apply edits");
                }
            }

            // ----- Remove row -----
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("X")) {
                cfg.user_load_key.erase(cfg.user_load_key.begin() + i);
                cfg.user_store_key.erase(cfg.user_store_key.begin() + i);
                cfg.user_key_type.erase(cfg.user_key_type.begin() + i);
                cfg.user_load_str.erase(cfg.user_load_str.begin() + i);
                cfg.user_store_str.erase(cfg.user_store_str.begin() + i);
                cfg.user_type_str.erase(cfg.user_type_str.begin() + i);
                cfg.user_depth.erase(cfg.user_depth.begin() + i);
                cfg.user_convergence.erase(cfg.user_convergence.begin() + i);
                cfg.user_fov.erase(cfg.user_fov.begin() + i);
                if (i < cfg.user_preset_index.size())
                    cfg.user_preset_index.erase(cfg.user_preset_index.begin() + i);
                if (i < cfg.prev_depth.size())       cfg.prev_depth.erase(cfg.prev_depth.begin() + i);
                if (i < cfg.prev_convergence.size()) cfg.prev_convergence.erase(cfg.prev_convergence.begin() + i);
                if (i < cfg.prev_fov.size())         cfg.prev_fov.erase(cfg.prev_fov.begin() + i);
                if (i < cfg.was_held.size())         cfg.was_held.erase(cfg.was_held.begin() + i);
                if (i < cfg.load_xinput.size())      cfg.load_xinput.erase(cfg.load_xinput.begin() + i);
                if (i < cfg.sleep_count.size())      cfg.sleep_count.erase(cfg.sleep_count.begin() + i);
                cfg.num_user_settings = cfg.user_load_key.size();
                dirty = true;
                ImGui::PopID();
                break; // bail out; next frame will re-render with the new size
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("+ Add Preset Row")) {
        cfg.user_load_key.push_back(0);
        cfg.user_store_key.push_back(0);
        cfg.user_key_type.push_back(0);  // SWITCH
        cfg.user_load_str.push_back("");
        cfg.user_store_str.push_back("");
        cfg.user_type_str.push_back("switch");
        cfg.user_depth.push_back({ component->GetDepth() });
        cfg.user_convergence.push_back({ component->GetConvergence() });
        cfg.user_fov.push_back({ component->GetFoV() });
        cfg.user_preset_index.push_back(0);
        cfg.prev_depth.push_back(0.0f);
        cfg.prev_convergence.push_back(1.0f);
        cfg.prev_fov.push_back(component->GetFoV());
        cfg.was_held.push_back(false);
        cfg.load_xinput.push_back(false);
        cfg.sleep_count.push_back(0);
        cfg.num_user_settings = cfg.user_load_key.size();
        dirty = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Save profile from the footer to persist changes.");

    if (dirty) {
        component->LoadSettings(cfg);
    }
}

// ---------------------------------------------------------------------------
// Tracking tab — HMD pose, controller, OpenTrack, track filter, LeiaSR.
// ---------------------------------------------------------------------------
void OsdMenu::Impl::DrawTrackingTab() {
    auto cfg = component->GetConfig();
    bool dirty = false;

    if (ImGui::CollapsingHeader("HMD Pose", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::DragFloat("Origin X",      &cfg.hmd_x,      0.01f, -10.0f, 10.0f, "%.2f")) dirty = true;
        if (ImGui::DragFloat("Origin Y (fwd)",&cfg.hmd_y,      0.01f, -10.0f, 10.0f, "%.2f")) dirty = true;
        if (ImGui::DragFloat("Height",        &cfg.hmd_height, 0.01f,  0.0f,  3.0f,  "%.2f")) dirty = true;
        if (ImGui::DragFloat("Yaw",           &cfg.hmd_yaw,    0.1f,  -360.0f, 360.0f, "%.2f")) dirty = true;
        ImGui::TextDisabled("HMD pose persists with 'Save Default Cfg' in the footer.");
    }

    if (ImGui::CollapsingHeader("Controller")) {
        if (ImGui::Checkbox("Pitch (right stick)", &cfg.pitch_enable)) dirty = true;
        if (ImGui::Checkbox("Yaw (right stick)",   &cfg.yaw_enable))   dirty = true;
        if (ImGui::SliderFloat("Pitch Radius",   &cfg.pitch_radius,    0.0f, 1.0f, "%.3f")) dirty = true;
        if (ImGui::SliderFloat("Sensitivity",    &cfg.ctrl_sensitivity,0.0f, 5.0f, "%.2f")) dirty = true;
        if (ImGui::SliderFloat("Stick Deadzone", &cfg.ctrl_deadzone,   0.0f, 1.0f, "%.3f")) dirty = true;
    }

    if (ImGui::CollapsingHeader("OpenTrack")) {
        if (ImGui::Checkbox("Enable OpenTrack",  &cfg.use_open_track)) {
            // LeiaSR's pose pipeline depends on the AccelaHamilton track
            // filter being active; the startup loader auto-enables it for
            // LeiaSR + use_open_track, but a runtime toggle has to do the
            // same coupling so the filter actually runs.
            if (cfg.use_open_track && cfg.output_mode == OutputMode::LeiaSR &&
                !cfg.use_track_filter) {
                cfg.use_track_filter = true;
            }
            dirty = true;
        }
        if (ImGui::InputInt("UDP Port",          &cfg.open_track_port)) dirty = true;
        ImGui::SameLine(); ImGui::TextDisabled("(Requires Restart)");
    }

    if (ImGui::CollapsingHeader("Track Filter")) {
        if (ImGui::Checkbox("Enable Filter", &cfg.use_track_filter)) dirty = true;
        ImGui::BeginDisabled(!cfg.use_track_filter);
        if (ImGui::SliderFloat("Rotation Sens",   &cfg.trk_flt_rot_sens,    0.0f, 5.0f, "%.2f")) dirty = true;
        if (ImGui::SliderFloat("Position Sens",   &cfg.trk_flt_pos_sens,    0.0f, 5.0f, "%.2f")) dirty = true;
        if (ImGui::SliderFloat("Rotation DZ",     &cfg.trk_flt_rot_dz,      0.0f, 0.5f, "%.3f")) dirty = true;
        if (ImGui::SliderFloat("Position DZ",     &cfg.trk_flt_pos_dz,      0.0f, 0.5f, "%.3f")) dirty = true;
        if (ImGui::SliderFloat("Zoom Smoothing",  &cfg.trk_flt_zoom_smooth, 0.0f, 1.0f, "%.2f")) dirty = true;
        if (ImGui::SliderFloat("Max Zoom",        &cfg.trk_flt_max_zoom,    1.0f, 50.0f, "%.1f")) dirty = true;
        ImGui::EndDisabled();
    }

    if (cfg.output_mode == OutputMode::LeiaSR &&
        ImGui::CollapsingHeader("LeiaSR Head Tracking")) {
        if (callbacks.calibrate_leiasr_head) {
            if (ImGui::Button("Calibrate (snap current head pose to neutral)")) {
                callbacks.calibrate_leiasr_head();
            }
            ImGui::Separator();
        }
        // Filter cutoffs / betas
        if (ImGui::SliderFloat("Pos MinCutoff", &cfg.sr_filter_pos_mincutoff, 0.0f, 5.0f, "%.3f")) dirty = true;
        if (ImGui::SliderFloat("Pos Beta",      &cfg.sr_filter_pos_beta,      0.0f, 1.0f, "%.3f")) dirty = true;
        if (ImGui::SliderFloat("Rot MinCutoff", &cfg.sr_filter_rot_mincutoff, 0.0f, 5.0f, "%.3f")) dirty = true;
        if (ImGui::SliderFloat("Rot Beta",      &cfg.sr_filter_rot_beta,      0.0f, 1.0f, "%.3f")) dirty = true;
        if (ImGui::SliderFloat("Angle DZ (deg)", &cfg.sr_angle_deadzone_deg,  0.0f, 5.0f, "%.2f")) dirty = true;
        ImGui::Separator();
        if (ImGui::SliderFloat("Sens Yaw",   &cfg.sr_sens_yaw,   0.0f, 5.0f, "%.2f")) dirty = true;
        if (ImGui::SliderFloat("Sens Pitch", &cfg.sr_sens_pitch, 0.0f, 5.0f, "%.2f")) dirty = true;
        if (ImGui::SliderFloat("Sens Roll",  &cfg.sr_sens_roll,  0.0f, 5.0f, "%.2f")) dirty = true;
        if (ImGui::SliderFloat("Max Yaw",    &cfg.sr_max_yaw,    0.0f, 180.0f, "%.1f")) dirty = true;
        if (ImGui::SliderFloat("Max Pitch",  &cfg.sr_max_pitch,  0.0f, 180.0f, "%.1f")) dirty = true;
        if (ImGui::SliderFloat("Max Roll",   &cfg.sr_max_roll,   0.0f, 180.0f, "%.1f")) dirty = true;
        ImGui::Separator();
        static const char* track_modes[] = {
            "XYZ_YawPitch", "XYZ", "YawPitch", "Full6DOF", "YawPitchRoll" };
        int sel = 0;
        for (int i = 0; i < IM_ARRAYSIZE(track_modes); ++i) {
            if (cfg.sr_track_mode == track_modes[i]) { sel = i; break; }
        }
        if (ImGui::Combo("Track Mode", &sel, track_modes, IM_ARRAYSIZE(track_modes))) {
            cfg.sr_track_mode = track_modes[sel];
            dirty = true;
        }
    }

    if (dirty) {
        component->LoadSettings(cfg);
    }
}

// ---------------------------------------------------------------------------
// System tab — display/output, misc, about.
// ---------------------------------------------------------------------------
void OsdMenu::Impl::DrawSystemTab() {
    auto cfg = component->GetConfig();
    bool dirty = false;

    if (!monitors_refreshed) RefreshMonitors();

    if (ImGui::CollapsingHeader("Display & Output (Requires Restart)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        // Display Index combo populated from the monitor enumeration. Item 0
        // is "Primary (auto)"; items 1..N are explicit \\.\DISPLAYn entries
        // tagged with resolution and refresh.
        int sel = cfg.display_index;
        if (sel < 0 || sel > static_cast<int>(monitors.size())) sel = 0;
        std::vector<const char*> ptrs;
        ptrs.reserve(monitor_labels.size());
        for (auto& s : monitor_labels) ptrs.push_back(s.c_str());
        if (ImGui::Combo("Display Index", &sel, ptrs.data(),
                          static_cast<int>(ptrs.size()))) {
            cfg.display_index = sel;
            dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh##displays")) RefreshMonitors();

        // Mirror the OutputMode enum order. Keep in sync with stereo_config.h.
        static const char* modes[] = {
            "SbS", "TaB", "RowInterlaced", "ColInterlaced", "Checkerboard",
            "LeiaSR", "NvidiaDX9", "VirtualDesktop",
            "FramePacked720p60", "FramePacked1080p24", "FramePacked1080p60", "FramePacked1080p60CVT",
            "DualDisplay", "DualDisplayFlip",
            "AnaglyphRedCyan", "AnaglyphRedCyanDubois", "AnaglyphRedCyanDeghosted", "AnaglyphRedCyanCompromise",
            "AnaglyphGreenMagenta", "AnaglyphGreenMagentaDubois", "AnaglyphGreenMagentaDeghosted",
            "AnaglyphBlueAmber",
        };
        int mode_sel = static_cast<int>(cfg.output_mode);
        if (mode_sel < 0 || mode_sel >= IM_ARRAYSIZE(modes)) mode_sel = 0;
        if (ImGui::Combo("Output Mode", &mode_sel, modes, IM_ARRAYSIZE(modes))) {
            cfg.output_mode = static_cast<OutputMode>(mode_sel);
            dirty = true;
        }

        if (ImGui::InputInt("Render Width",  &cfg.render_width))  dirty = true;
        if (ImGui::InputInt("Render Height", &cfg.render_height)) dirty = true;
        if (ImGui::Checkbox("Dashboard Enable", &cfg.dash_enable)) dirty = true;
    }

    if (ImGui::CollapsingHeader("Misc")) {
        if (ImGui::Checkbox("Async Reprojection", &cfg.async_enable)) dirty = true;
        if (ImGui::Checkbox("Auto Focus",         &cfg.auto_focus))   dirty = true;
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", cfg.launch_script.c_str());
        if (ImGui::InputText("Launch Script", buf, sizeof(buf))) {
            cfg.launch_script = buf;
            dirty = true;
        }
        if (ImGui::Checkbox("Disable Hotkeys (Ctrl+Home still toggles menu)",
                            &cfg.disable_hotkeys)) {
            dirty = true;
        }
        if (callbacks.download_latest_profiles &&
            ImGui::Button("Download Latest Profiles")) {
            callbacks.download_latest_profiles();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(github.com/oneup03/VRto3D/releases/latest)");
        if (callbacks.open_config_folder) {
            if (ImGui::Button("Open Cfg Folder")) {
                callbacks.open_config_folder();
            }
        }
    }

    if (ImGui::CollapsingHeader("About", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("VRto3D %s", version.c_str());
        ImGui::Text("Build: %s %s", __DATE__, __TIME__);
        ImGui::Text("Profile: %s",
                    app_name.empty() ? "(default)" : app_name.c_str());
    }

    if (dirty) {
        // Re-derive load/store keys from the (possibly edited) symbolic
        // strings so changes take effect immediately, not just on reload.
        for (size_t i = 0; i < cfg.num_user_settings; ++i) {
            bool xi = false;
            ReparseHotkey(cfg.user_load_str[i],  cfg.user_load_key[i],  &xi);
            cfg.load_xinput[i] = xi;
            ReparseHotkey(cfg.user_store_str[i], cfg.user_store_key[i], nullptr);
            auto kt = KeyBindTypes.find(cfg.user_type_str[i]);
            if (kt != KeyBindTypes.end()) cfg.user_key_type[i] = kt->second;
        }
        component->LoadSettings(cfg);
    }
}

} // namespace vrto3d::osd
