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
#include "osd/osd_menu.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <utility>

#include "imgui.h"

#include "osd/osd_input.h"
#include "hmd_device_driver.h"
#include "platform.h"
#include "vrto3dlib/stereo_config.h"
#include "vrto3dlib/key_names.h"

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
    void DrawUserHotkeysTab(OsdInput& input);
    void DrawTrackingTab(OsdInput& input);
    void DrawShaderTab();
    void DrawSystemTab();
    void DrawFooter();
    void DrawTitleChrome();

    // Re-derive every runtime bind field from the symbolic *_str strings, then
    // push the config to the driver. Every tab's dirty handler funnels through
    // this so an OSD capture/edit takes effect immediately.
    void ApplyConfig(StereoDisplayDriverConfiguration& cfg);

    // State for the click-to-capture key picker. -1 row = none.
    int  capture_row_  = -1;
    bool capture_combo_pending_ = false;

    // Click-to-capture state for the Tracking-tab single-key pickers.
    // 0 = none, 1 = pose_reset, 2 = ctrl_toggle.
    int  tracking_capture_target_ = 0;
    bool tracking_capture_combo_  = false;

    // Footer height measured on the previous frame, used to reserve the
    // pinned bottom strip on the current frame. Zero on the first frame —
    // BuildUI falls back to a one-row estimate until we have a real number.
    float footer_h_measured = 0.0f;
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

void OsdMenu::BuildUI(OsdInput& input) {
    auto& s = *impl_;
    if (!s.visible.load() || !s.component) return;

    // Default the menu to roughly half the OSD surface — comfortable at any
    // render resolution, leaves plenty of game visible around it, and the
    // user can still drag-resize after. Min-size keeps a floor so the menu
    // remains usable on very small surfaces; max-size caps it to the OSD.
    // (std::clamp)/(std::max) paren-wraps suppress windows.h min/max macros.
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float kPad = 20.0f;
    const float kFloorW = 320.0f;
    const float kFloorH = 240.0f;
    const float min_w = (std::clamp)(disp.x - 2 * kPad, kFloorW, 720.0f);
    const float min_h = (std::clamp)(disp.y - 2 * kPad, kFloorH, 520.0f);
    const float max_w = (std::max)(disp.x, min_w);
    const float max_h = (std::max)(disp.y, min_h);
    const float init_w = (std::clamp)(disp.x * 0.5f, min_w, max_w);
    const float init_h = (std::clamp)(disp.y * 0.5f, min_h, max_h);
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_w, min_h), ImVec2(max_w, max_h));
    ImGui::SetNextWindowSize(ImVec2(init_w, init_h), ImGuiCond_FirstUseEver);
    // Center the initial position so the menu sits roughly mid-screen.
    const float init_x = (std::max)(0.0f, (disp.x - init_w) * 0.5f);
    const float init_y = (std::max)(0.0f, (disp.y - init_h) * 0.5f);
    ImGui::SetNextWindowPos(ImVec2(init_x, init_y), ImGuiCond_FirstUseEver);

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

    // Reserve space at the bottom so the footer action buttons stay pinned
    // when a tab's content overflows the viewport. The footer wraps across
    // 1–2 rows depending on window width, so use the previous frame's
    // measured height and fall back to a one-row estimate on the first
    // frame. Snap to a small minimum so the very first frame doesn't render
    // with the child eating the entire window.
    const float footer_h_fallback = ImGui::GetFrameHeightWithSpacing() +
                                     ImGui::GetStyle().ItemSpacing.y;
    const float footer_h = s.footer_h_measured > 0.0f
                            ? s.footer_h_measured
                            : footer_h_fallback;

    if (ImGui::BeginTabBar("##vrto3d_tabs")) {
        auto draw_tab = [&](const char* label, const char* child_id, auto&& fn) {
            if (ImGui::BeginTabItem(label)) {
                // Per-tab child so scroll state is preserved independently
                // and the tab content can scroll without dragging the tab
                // bar or footer along with it.
                ImGui::BeginChild(child_id, ImVec2(0, -footer_h), false,
                                   ImGuiWindowFlags_HorizontalScrollbar);
                fn();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        };
        draw_tab("Stereo",       "##scroll_stereo", [&]{ s.DrawStereoTab(); });
        draw_tab("User Hotkeys", "##scroll_user",   [&]{ s.DrawUserHotkeysTab(input); });
        draw_tab("Tracking",     "##scroll_track",  [&]{ s.DrawTrackingTab(input); });
        draw_tab("Shader",       "##scroll_shader", [&]{ s.DrawShaderTab(); });
        draw_tab("System",       "##scroll_system", [&]{ s.DrawSystemTab(); });
        ImGui::EndTabBar();
    }

    // Measure the footer's actual rendered height for next frame's
    // reservation. Captured around DrawFooter so it includes the leading
    // separator and any wrap-induced extra rows.
    const float footer_top = ImGui::GetCursorPosY();
    s.DrawFooter();
    s.footer_h_measured = ImGui::GetCursorPosY() - footer_top;

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
        const char* label = "Always on Top";
        const char* hint  = "(Ctrl + F8)";
        const ImGuiStyle& style = ImGui::GetStyle();
        const float box_w = ImGui::CalcTextSize(label).x +
                             ImGui::GetFrameHeight() +
                             style.ItemInnerSpacing.x;
        const float hint_w = ImGui::CalcTextSize(hint).x +
                              style.ItemSpacing.x;
        const float target_x = ImGui::GetWindowWidth() - box_w - hint_w -
                                style.WindowPadding.x;
        // Only right-align when there's room past the current cursor. On
        // narrow windows the profile name plus checkbox overlap; let the
        // checkbox fall to its own line in that case.
        if (target_x > ImGui::GetCursorPosX()) {
            ImGui::SameLine();
            ImGui::SetCursorPosX(target_x);
        }
        bool on = callbacks.always_on_top();
        if (ImGui::Checkbox(label, &on)) {
            callbacks.toggle_always_on_top();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", hint);
    }
    ImGui::Separator();
}

// ---------------------------------------------------------------------------
// Footer — global save/reload buttons present on every tab.
// ---------------------------------------------------------------------------
void OsdMenu::Impl::DrawFooter() {
    ImGui::Separator();

    const bool have_app = !app_name.empty();
    bool first = true;

    // SameLine before the next button only when it actually fits in the
    // remaining content region — otherwise let the default vertical layout
    // wrap it to a new row. Without this, narrow OSD surfaces clip the
    // rightmost footer buttons.
    auto try_sameline = [&](const char* next_label) {
        if (first) { first = false; return; }
        const float btn_w = ImGui::CalcTextSize(next_label).x +
                             ImGui::GetStyle().FramePadding.x * 2.0f;
        const float needed = btn_w + ImGui::GetStyle().ItemSpacing.x;
        if (ImGui::GetContentRegionAvail().x >= needed) {
            ImGui::SameLine();
        }
    };

    if (callbacks.save_game_profile) {
        const std::string label = have_app
            ? "Save " + app_name + "_config.json"
            : std::string("Save Game Cfg");
        try_sameline(label.c_str());
        ImGui::BeginDisabled(!have_app);
        if (ImGui::Button(label.c_str())) {
            callbacks.save_game_profile("Saved " + app_name + "_config.json");
        }
        ImGui::EndDisabled();
    }
    if (callbacks.save_default_profile) {
        const char* label = "Save Default Cfg";
        try_sameline(label);
        if (ImGui::Button(label)) {
            callbacks.save_default_profile("Saved default_config.json");
        }
    }
    if (callbacks.reload_game_profile) {
        const char* label = "Reload Game Cfg";
        try_sameline(label);
        ImGui::BeginDisabled(!have_app);
        if (ImGui::Button(label)) {
            callbacks.reload_game_profile("Reloaded " + app_name + "_config.json");
        }
        ImGui::EndDisabled();
    }
    if (callbacks.reload_default_profile) {
        const char* label = "Reload Default Cfg";
        try_sameline(label);
        if (ImGui::Button(label)) {
            callbacks.reload_default_profile("Reloaded default_config.json");
        }
    }
}

// ---------------------------------------------------------------------------
// Stereo tab — depth, convergence, FoV, projection helpers.
// ---------------------------------------------------------------------------
void OsdMenu::Impl::DrawStereoTab() {
    auto cfg = component->GetConfig();

    // While auto-depth is on, the slider edits the user's ceiling (manual_depth_)
    // rather than the live, auto-modulated depth — otherwise nudging the slider
    // would clobber the ceiling with whatever value the auto loop has currently
    // attenuated to. The live value is surfaced separately in the Auto-Depth
    // section below.
    const bool auto_on = callbacks.get_auto_depth_enabled
                         && callbacks.get_auto_depth_enabled();
    float depth = auto_on ? component->GetManualDepth() : component->GetDepth();
    if (ImGui::SliderFloat("Depth", &depth, 0.0f, 0.5f, "%.3f")) {
        component->AdjustDepth(depth, false);
        // Mirror Ctrl+F3/F4 — re-sync projection so the new depth
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

    int fov = static_cast<int>(component->GetFoV() + 0.5f);
    if (ImGui::SliderInt("FoV", &fov, 30, 120)) {
        component->AdjustFoV(static_cast<float>(fov));
    }

    if (callbacks.get_auto_depth_enabled && callbacks.set_auto_depth_enabled) {
        ImGui::Separator();
        bool ad = callbacks.get_auto_depth_enabled();
        if (ImGui::Checkbox("Auto-Depth", &ad)) {
            callbacks.set_auto_depth_enabled(ad);
        }
        ImGui::SameLine(); ImGui::TextDisabled("(Ctrl+F11)");

        ImGui::Text("Current Auto Depth: %.3f", component->GetDepth());

        if (callbacks.get_auto_depth_target && callbacks.set_auto_depth_target) {
            float target = callbacks.get_auto_depth_target();
            if (ImGui::SliderFloat("Comfort Target", &target, 0.001f, 0.02f, "%.3f")) {
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
        if (callbacks.get_auto_depth_logging && callbacks.set_auto_depth_logging) {
            bool log_on = callbacks.get_auto_depth_logging();
            if (ImGui::Checkbox("Log Disparity Samples", &log_on)) {
                callbacks.set_auto_depth_logging(log_on);
            }
            ImGui::SameLine(); ImGui::TextDisabled("(periodic histogram to vrto3d.txt)");
        }
        ImGui::Separator();
    }

    bool eye_swap = cfg.eye_swap;
    if (ImGui::Checkbox("Swap Eyes", &eye_swap)) {
        cfg.eye_swap = eye_swap;
        ApplyConfig(cfg);
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
    // Shared cross-platform parser (key_names accepts both the portable
    // vocabulary and legacy VK_*/XINPUT_* spellings, so OSD-entered names parse
    // identically to JsonManager). migrate=false: this only re-derives the
    // runtime code for a live edit; the string is rewritten to canonical
    // spelling on the next profile save.
    std::string name = s;
    int32_t parsed = 0;
    bool xi = false;
    vrto3d::keys::ParseBind(name, parsed, xi, /*migrate=*/false);
    code = parsed;
    if (xinput) *xinput = xi;
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

// Re-derive the runtime bind fields (load keys, xinput flags, bind types) from
// the symbolic *_str strings for every user-preset row and the pose_reset /
// ctrl_toggle keys, then push the config to the driver. All tab dirty handlers
// funnel through here: previously only the System tab re-derived, so a gamepad
// bind captured in the User Hotkeys or Tracking tab kept load_xinput=false and
// user_load_key=0 and never fired until a profile save+reload.
void OsdMenu::Impl::ApplyConfig(StereoDisplayDriverConfiguration& cfg) {
    for (size_t i = 0; i < cfg.num_user_settings; ++i) {
        bool xi = false;
        ReparseHotkey(cfg.user_load_str[i], cfg.user_load_key[i], &xi);
        cfg.load_xinput[i] = xi;
        const int kt = vrto3d::keys::KeyBindTypeFromName(cfg.user_type_str[i]);
        if (kt >= 0) cfg.user_key_type[i] = kt;
    }
    ReparseHotkey(cfg.pose_reset_str,  cfg.pose_reset_key,  &cfg.reset_xinput);
    ReparseHotkey(cfg.ctrl_toggle_str, cfg.ctrl_toggle_key, &cfg.ctrl_xinput);
    const int kt = vrto3d::keys::KeyBindTypeFromName(cfg.ctrl_type_str);
    if (kt >= 0) cfg.ctrl_type = kt;
    component->LoadSettings(cfg);
}

// ---------------------------------------------------------------------------
// User Hotkeys tab — editable rows + click-to-capture picker.
// ---------------------------------------------------------------------------
void OsdMenu::Impl::DrawUserHotkeysTab(OsdInput& input) {
    auto cfg = component->GetConfig();
    bool dirty = false;

    ImGui::TextWrapped(
        "Each row maps a Load key to a (Depth, Convergence, FoV) preset. "
        "Only \"toggle\" mode supports comma-separated values that cycle on each press; "
        "\"switch\" and \"hold\" keep only the first value. "
        "FoV = 0 means \"use the active profile FoV\". Use Copy Current to fill the "
        "row from the current settings.");
    ImGui::Spacing();

    // Active capture session — drain the input pump.
    if (capture_row_ >= 0) {
        ImGui::TextColored(ImVec4(1, 0.85f, 0.2f, 1),
                            capture_combo_pending_
                                ? "Capturing combo for Load on row %d — press a chord, then release all to commit. (Esc to cancel)"
                                : "Capturing key for Load on row %d — press a key or controller button. (Esc to cancel)",
                            capture_row_ + 1);

        CapturedKey c = input.PollCapture();
        if (c.valid) {
            const size_t r = static_cast<size_t>(capture_row_);
            if (r < cfg.num_user_settings) {
                cfg.user_load_str[r] = c.key_name;
                dirty = true;
            }
            capture_row_ = -1;
            capture_combo_pending_ = false;
            input.CancelCapture();
        }
        if (input.WasPressed(VK_ESCAPE)) {
            capture_row_ = -1;
            capture_combo_pending_ = false;
            input.CancelCapture();
        }
        ImGui::Separator();
    }

    static const char* kModes[] = { "switch", "toggle", "hold" };

    // Only "toggle" mode is allowed to keep multi-element preset cycles. For
    // "switch" and "hold" we collapse depth/conv/fov to the first entry. Used
    // both when the mode dropdown changes away from toggle and when the user
    // commits a multi-value CSV in a non-toggle row.
    auto trimRowToFirst = [&](size_t row) {
        if (cfg.user_depth[row].size() > 1)       cfg.user_depth[row].resize(1);
        if (cfg.user_convergence[row].size() > 1) cfg.user_convergence[row].resize(1);
        if (cfg.user_fov[row].size() > 1)         cfg.user_fov[row].resize(1);
        if (row < cfg.user_preset_index.size())   cfg.user_preset_index[row] = 0;
    };

    if (ImGui::BeginTable("##user_hotkeys", 5,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("#",         ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableSetupColumn("Load",      ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Mode",      ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Depth / Conv / FoV (comma = cycle, toggle only)", ImGuiTableColumnFlags_WidthStretch, 3.0f);
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
                    capture_combo_pending_ = false;
                    input.BeginCapture(false);
                }
                ImGui::SameLine();
                if (ImGui::Button("Combo##load")) {
                    capture_row_  = static_cast<int>(i);
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
                    if (cfg.user_type_str[i] != "toggle") {
                        trimRowToFirst(i);
                    }
                    dirty = true;
                }
            }

            // ----- Preset CSVs -----
            ImGui::TableNextColumn();
            {
                const bool toggle_mode = (cfg.user_type_str[i] == "toggle");
                auto editCsv = [&](const char* label_id, std::vector<float>& vec, int prec) {
                    char buf[256];
                    std::string csv = FloatsToCsv(vec, prec);
                    std::snprintf(buf, sizeof(buf), "%s", csv.c_str());
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputText(label_id, buf, sizeof(buf),
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                        auto parsed = CsvToFloats(buf);
                        if (!parsed.empty()) {
                            // Non-toggle modes never cycle, so commit only the
                            // first value regardless of how many the user typed.
                            if (!toggle_mode && parsed.size() > 1) parsed.resize(1);
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

                if (ImGui::Button("Copy Current")) {
                    // Replace the row's preset list with a single-element list
                    // containing the current depth/convergence/FoV. Edit the
                    // CSV by hand if you want to add more presets to cycle.
                    cfg.user_depth[i]       = { component->GetDepth() };
                    cfg.user_convergence[i] = { component->GetConvergence() };
                    cfg.user_fov[i]         = { component->GetFoV() };
                    if (i < cfg.user_preset_index.size())
                        cfg.user_preset_index[i] = 0;
                    dirty = true;
                }
                ImGui::SameLine();

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
                cfg.user_key_type.erase(cfg.user_key_type.begin() + i);
                cfg.user_load_str.erase(cfg.user_load_str.begin() + i);
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
        cfg.user_key_type.push_back(0);  // SWITCH
        cfg.user_load_str.push_back("");
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
        ApplyConfig(cfg);
    }
}

// ---------------------------------------------------------------------------
// Tracking tab — HMD pose, controller, OpenTrack, track filter, LeiaSR.
// ---------------------------------------------------------------------------
void OsdMenu::Impl::DrawTrackingTab(OsdInput& input) {
    auto cfg = component->GetConfig();
    bool dirty = false;

    // Helper: trigger a recenter on a true→false checkbox transition so
    // stale pitch / yaw / OpenTrack accumulation doesn't bleed into the
    // pose the next time tracking is enabled or composed.
    auto recenter_on_disable = [&](bool was_on, bool now_on) {
        if (was_on && !now_on && callbacks.recenter_pose) callbacks.recenter_pose();
    };

    if (ImGui::CollapsingHeader("HMD Pose", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::DragFloat("Origin X",      &cfg.hmd_x,      0.01f, -10.0f, 10.0f, "%.2f")) dirty = true;
        if (ImGui::DragFloat("Origin Y (fwd)",&cfg.hmd_y,      0.01f, -10.0f, 10.0f, "%.2f")) dirty = true;
        if (ImGui::DragFloat("Height",        &cfg.hmd_height, 0.01f,  0.0f,  3.0f,  "%.2f")) dirty = true;
        if (ImGui::DragFloat("Yaw",           &cfg.hmd_yaw,    0.1f,  -360.0f, 360.0f, "%.2f")) dirty = true;
        ImGui::TextDisabled("HMD pose persists with 'Save Default Cfg' in the footer.");
    }

    if (ImGui::CollapsingHeader("XInput (Xbox) Controller")) {
        const bool was_pitch = cfg.pitch_enable;
        if (ImGui::Checkbox("Pitch (right stick)", &cfg.pitch_enable)) {
            dirty = true;
            recenter_on_disable(was_pitch, cfg.pitch_enable);
        }
        const bool was_yaw = cfg.yaw_enable;
        if (ImGui::Checkbox("Yaw (right stick)",   &cfg.yaw_enable))   {
            dirty = true;
            recenter_on_disable(was_yaw, cfg.yaw_enable);
        }
        if (ImGui::SliderFloat("Pitch Radius",   &cfg.pitch_radius,    0.0f, 1.0f, "%.3f")) dirty = true;
        if (ImGui::SliderFloat("Sensitivity",    &cfg.ctrl_sensitivity,0.0f, 5.0f, "%.2f")) dirty = true;
        if (ImGui::SliderFloat("Stick Deadzone", &cfg.ctrl_deadzone,   0.0f, 1.0f, "%.3f")) dirty = true;

        ImGui::Separator();

        // Drain the input pump while a tracking-tab capture is active.
        // Combo capture supports XInput chord bindings (e.g.
        // LBUMPER+RBUMPER) — keyboard combos won't resolve through
        // json_manager's parser and will silently no-op, matching the
        // User Hotkeys tab's existing behavior.
        if (tracking_capture_target_ != 0) {
            ImGui::TextColored(ImVec4(1, 0.85f, 0.2f, 1),
                                tracking_capture_combo_
                                    ? "Capturing combo — press a chord, then release all to commit. (Esc to cancel)"
                                    : "Capturing key — press a key or controller button. (Esc to cancel)");
            CapturedKey c = input.PollCapture();
            if (c.valid) {
                if (tracking_capture_target_ == 1) {
                    cfg.pose_reset_str = c.key_name;
                } else if (tracking_capture_target_ == 2) {
                    cfg.ctrl_toggle_str = c.key_name;
                }
                dirty = true;
                tracking_capture_target_ = 0;
                tracking_capture_combo_  = false;
                input.CancelCapture();
            } else if (input.WasPressed(VK_ESCAPE)) {
                tracking_capture_target_ = 0;
                tracking_capture_combo_  = false;
                input.CancelCapture();
            }
            ImGui::Separator();
        }

        auto hotkey_row = [&](const char* label, int target, std::string& out_str) {
            ImGui::PushID(label);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", out_str.c_str());
            if (ImGui::InputText(label, buf, sizeof(buf))) {
                out_str = buf;
                dirty = true;
            }
            if (ImGui::Button("Set")) {
                tracking_capture_target_ = target;
                tracking_capture_combo_  = false;
                input.BeginCapture(false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Combo")) {
                tracking_capture_target_ = target;
                tracking_capture_combo_  = true;
                input.BeginCapture(true);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(VK_* or XINPUT_GAMEPAD_*)");
            ImGui::PopID();
        };

        hotkey_row("Pose Reset Key", 1, cfg.pose_reset_str);
        hotkey_row("Toggle Key",     2, cfg.ctrl_toggle_str);

        static const char* kModes[] = { "toggle", "hold" };
        int sel = 0;
        for (int k = 0; k < IM_ARRAYSIZE(kModes); ++k)
            if (cfg.ctrl_type_str == kModes[k]) { sel = k; break; }
        if (ImGui::Combo("Toggle Mode", &sel, kModes, IM_ARRAYSIZE(kModes))) {
            cfg.ctrl_type_str = kModes[sel];
            dirty = true;
        }
    }

    if (ImGui::CollapsingHeader("OpenTrack")) {
        const bool was_open_track = cfg.use_open_track;
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
            recenter_on_disable(was_open_track, cfg.use_open_track);
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

#ifdef _WIN32
    // LeiaSR head tracking (SR SDK) is Windows-only — the presenter is compiled
    // out on Linux, so this whole section is hidden there.
    if (cfg.output_mode == OutputMode::LeiaSR &&
        ImGui::CollapsingHeader("LeiaSR Head Tracking")) {
        if (ImGui::Checkbox("Enable LeiaSR Tracking", &cfg.sr_tracking_enabled)) dirty = true;
        ImGui::TextWrapped("Disable to drive OpenTrack from another source "
                           "(e.g. the OpenTrack app) instead of the built-in "
                           "SR head tracker.");
        ImGui::BeginDisabled(!cfg.sr_tracking_enabled);
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
        ImGui::EndDisabled();
    }
#endif  // _WIN32 (LeiaSR head tracking)

    if (dirty) {
        ApplyConfig(cfg);
    }
}

// ---------------------------------------------------------------------------
// Shader tab — display-correction post-process pass (Lift / Gamma / Gain +
// extended S-Curve). All defaults are pass-through; pipeline is skipped
// entirely when disabled, so a user who never opens this tab pays nothing.
// Intended for users with crosstalk-prone 3D displays (e.g. passive row-
// interlaced monitors), where lowering Gain and reducing the curve below
// 1.0 trades some brightness/contrast for sharply less visible crosstalk.
// ---------------------------------------------------------------------------
void OsdMenu::Impl::DrawShaderTab() {
    auto cfg = component->GetConfig();
    bool dirty = false;

    if (ImGui::Checkbox("Enable Display Correction Shader", &cfg.shader_enabled)) {
        dirty = true;
    }
    ImGui::TextWrapped("Reduces high-contrast crosstalk on some 3D displays "
                       "(e.g. passive row-interlaced monitors). Sacrifices "
                       "brightness and contrast — disable when not needed.");

    if (ImGui::CollapsingHeader("Lift / Gamma / Gain",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SliderFloat3("RGB Lift",  cfg.shader_lift,  0.0f, 2.0f, "%.3f")) dirty = true;
        ImGui::SameLine(); ImGui::TextDisabled("(shadows)");
        if (ImGui::SliderFloat3("RGB Gamma", cfg.shader_gamma, 0.0f, 2.0f, "%.3f")) dirty = true;
        ImGui::SameLine(); ImGui::TextDisabled("(midtones)");
        if (ImGui::SliderFloat3("RGB Gain",  cfg.shader_gain,  0.0f, 2.0f, "%.3f")) dirty = true;
        ImGui::SameLine(); ImGui::TextDisabled("(highlights)");
    }

    if (ImGui::CollapsingHeader("S-Curve",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SliderFloat("Curve", &cfg.shader_curve, 0.33f, 3.0f, "%.3f")) dirty = true;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("1.0 = pass-through.\n"
                              "Below 1.0: reduces midtone contrast (helps with crosstalk).\n"
                              "Above 1.0: increases midtone contrast.");
        }
        if (ImGui::SliderFloat("Curve Offset (Low)",  &cfg.shader_curve_off_low,  -1.0f, 1.0f, "%.3f")) dirty = true;
        if (ImGui::SliderFloat("Curve Offset (High)", &cfg.shader_curve_off_high, -1.0f, 1.0f, "%.3f")) dirty = true;
        if (ImGui::SliderFloat("Curve Offset (Both)", &cfg.shader_curve_off_both, -1.0f, 1.0f, "%.3f")) dirty = true;
    }

    if (ImGui::Button("Reset to Defaults")) {
        cfg.shader_lift[0]  = cfg.shader_lift[1]  = cfg.shader_lift[2]  = 1.0f;
        cfg.shader_gamma[0] = cfg.shader_gamma[1] = cfg.shader_gamma[2] = 1.0f;
        cfg.shader_gain[0]  = cfg.shader_gain[1]  = cfg.shader_gain[2]  = 1.0f;
        cfg.shader_curve          = 1.0f;
        cfg.shader_curve_off_low  = 0.0f;
        cfg.shader_curve_off_high = 0.0f;
        cfg.shader_curve_off_both = 0.0f;
        dirty = true;
    }

    if (dirty) {
        ApplyConfig(cfg);
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

        // Output modes with a per-entry "Windows-only" flag. LeiaSR (SR SDK)
        // and NvidiaDX9 (NVAPI/D3D9 3D Vision) have no Linux presenter, so
        // they're filtered out there — building the visible list this way
        // keeps the label->enum mapping correct regardless of what's hidden.
        struct ModeEntry { const char* label; OutputMode mode; bool win_only; };
        static const ModeEntry kModeEntries[] = {
            {"SbS", OutputMode::SbS, false},
            {"TaB", OutputMode::TaB, false},
            {"RowInterlaced", OutputMode::RowInterlaced, false},
            {"ColInterlaced", OutputMode::ColInterlaced, false},
            {"Checkerboard", OutputMode::Checkerboard, false},
            {"LeiaSR", OutputMode::LeiaSR, true},
            {"NvidiaDX9", OutputMode::NvidiaDX9, true},
            {"WibbleWobble", OutputMode::WibbleWobble, false},
            {"VirtualDesktop", OutputMode::VirtualDesktop, false},
            {"FramePacked720p60", OutputMode::FramePacked720p60, false},
            {"FramePacked1080p24", OutputMode::FramePacked1080p24, false},
            {"FramePacked1080p60", OutputMode::FramePacked1080p60, false},
            {"FramePacked1080p60CVT", OutputMode::FramePacked1080p60CVT, false},
            {"DualDisplay", OutputMode::DualDisplay, false},
            {"DualDisplayFlip", OutputMode::DualDisplayFlip, false},
            {"AnaglyphRedCyan", OutputMode::AnaglyphRedCyan, false},
            {"AnaglyphRedCyanDubois", OutputMode::AnaglyphRedCyanDubois, false},
            {"AnaglyphRedCyanDeghosted", OutputMode::AnaglyphRedCyanDeghosted, false},
            {"AnaglyphRedCyanCompromise", OutputMode::AnaglyphRedCyanCompromise, false},
            {"AnaglyphGreenMagenta", OutputMode::AnaglyphGreenMagenta, false},
            {"AnaglyphGreenMagentaDubois", OutputMode::AnaglyphGreenMagentaDubois, false},
            {"AnaglyphGreenMagentaDeghosted", OutputMode::AnaglyphGreenMagentaDeghosted, false},
            {"AnaglyphBlueAmber", OutputMode::AnaglyphBlueAmber, false},
            {"Mono", OutputMode::Mono, false},
        };
        std::vector<const char*> mode_labels;
        std::vector<OutputMode>  mode_vals;
        int mode_sel = 0;
        for (const auto& e : kModeEntries) {
#ifndef _WIN32
            if (e.win_only) continue;
#endif
            if (e.mode == cfg.output_mode) mode_sel = static_cast<int>(mode_labels.size());
            mode_labels.push_back(e.label);
            mode_vals.push_back(e.mode);
        }
        if (ImGui::Combo("Output Mode", &mode_sel, mode_labels.data(),
                          static_cast<int>(mode_labels.size()))) {
            cfg.output_mode = mode_vals[mode_sel];
            dirty = true;
        }

        if (ImGui::InputInt("Render Width",  &cfg.render_width))  dirty = true;
        if (ImGui::InputInt("Render Height", &cfg.render_height)) dirty = true;
        if (ImGui::InputFloat("Display Frequency", &cfg.display_frequency, 0.0f, 0.0f, "%.2f")) dirty = true;
        ImGui::SameLine(); ImGui::TextDisabled("(0.0 = use current)");
        if (ImGui::DragFloat("Aspect Ratio", &cfg.aspect_ratio, 0.001f, 0.5f, 4.0f, "%.3f")) dirty = true;
    }

    if (ImGui::CollapsingHeader("Misc")) {
        if (ImGui::Checkbox("Dashboard Enable",   &cfg.dash_enable))  dirty = true;
        if (ImGui::Checkbox("Async Reprojection", &cfg.async_enable)) {
            dirty = true;
            if (callbacks.set_async) callbacks.set_async(cfg.async_enable);
        }
#ifdef _WIN32
        // Auto Focus (game-window focus juggling) and cursor hide/lock have no
        // effect on Linux: the overlay is a non-focusable click-through surface
        // (input reaches the game directly, OSD reads evdev), and the game
        // manages its own cursor. Hidden there to avoid dead controls.
        if (ImGui::Checkbox("Auto Focus",         &cfg.auto_focus)) {
            dirty = true;
            if (callbacks.set_auto_focus) callbacks.set_auto_focus(cfg.auto_focus);
        }
#endif
        if (ImGui::Checkbox("Auto Exit SteamVR",  &cfg.auto_exit)) dirty = true;
#ifdef _WIN32
        if (ImGui::Checkbox("Hide Cursor",        &cfg.hide_cursor)) {
            dirty = true;
            if (callbacks.set_hide_cursor) callbacks.set_hide_cursor(cfg.hide_cursor);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(hide OS cursor over the game window)");
        if (ImGui::Checkbox("Lock Cursor to Game", &cfg.lock_cursor)) {
            dirty = true;
            if (callbacks.set_lock_cursor) callbacks.set_lock_cursor(cfg.lock_cursor);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(clip cursor to game window while focused)");
#endif
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", cfg.launch_script.c_str());
        if (ImGui::InputText("Launch Script", buf, sizeof(buf))) {
            cfg.launch_script = buf;
            dirty = true;
        }
        if (ImGui::Checkbox("Disable Hotkeys (menu: Ctrl+Home / Start+DPad-Down; "
                            "on-top: Ctrl+F8 / Start+DPad-Up still work)",
                            &cfg.disable_hotkeys)) {
            dirty = true;
        }
        if (callbacks.reset_defaults) {
            if (ImGui::Button("Reset Defaults")) {
                callbacks.reset_defaults("Reset to factory defaults");
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.85f, 0.2f, 1),
                               "Restores factory stereo/shader/tracking settings "
                               "(display/output kept). Reload or Save to persist.");
        }
        if (callbacks.download_latest_profiles) {
            if (ImGui::Button("Download Latest Profiles")) {
                callbacks.download_latest_profiles();
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.85f, 0.2f, 1),
                               "Warning: overwrites existing profiles");
        }
        if (callbacks.open_config_folder) {
            if (ImGui::Button("Open Profile Folder")) {
                callbacks.open_config_folder();
            }
        }
        if (callbacks.open_screenshot_folder) {
            if (callbacks.open_config_folder) ImGui::SameLine();
            if (ImGui::Button("Open Screenshot Folder")) {
                callbacks.open_screenshot_folder();
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
        ApplyConfig(cfg);
    }
}

} // namespace vrto3d::osd
