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
#include "osd/osd_controllers_tab.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "imgui.h"

#include "osd/ctrl_bridge.h"
#include "osd/osd_input.h"
#include "vrto3dlib/key_codes.h"

using nlohmann::json;

namespace vrto3d::osd {

namespace {

constexpr auto kRefresh = std::chrono::milliseconds(250);

double Num(const json& j, double def)
{
    return j.is_number() ? j.get<double>() : def;
}

std::string Str(const json& j, const char* key, const std::string& def = {})
{
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
}

void HelpMarker(const json& item)
{
    const std::string help = Str(item, "help");
    if (help.empty()) return;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(help.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool EnumCombo(const char* label, const json& values, json& value)
{
    if (!values.is_array() || values.empty()) return false;
    std::vector<std::string> opts;
    for (const auto& v : values)
        if (v.is_string()) opts.push_back(v.get<std::string>());
    const std::string cur = value.is_string() ? value.get<std::string>() : std::string();
    bool changed = false;
    if (ImGui::BeginCombo(label, cur.c_str())) {
        for (const auto& o : opts) {
            const bool sel = (o == cur);
            if (ImGui::Selectable(o.c_str(), sel)) {
                value = o;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool Vec3Editor(const char* label, json& arr, float speed, float lo, float hi, const char* fmt)
{
    float v[3] = {0, 0, 0};
    if (arr.is_array())
        for (size_t i = 0; i < 3 && i < arr.size(); ++i) v[i] = (float)Num(arr[i], 0.0);
    if (ImGui::DragFloat3(label, v, speed, lo, hi, fmt)) {
        arr = json::array({v[0], v[1], v[2]});
        return true;
    }
    return false;
}

const char* StateLabel(const std::string& s, ImVec4& color)
{
    if (s == "ok") { color = ImVec4(0.4f, 0.9f, 0.4f, 1); return "running"; }
    if (s == "idle") { color = ImVec4(0.7f, 0.7f, 0.7f, 1); return "idle"; }
    if (s == "disabled") { color = ImVec4(0.6f, 0.6f, 0.6f, 1); return "off"; }
    if (s == "faulted") { color = ImVec4(1.0f, 0.35f, 0.3f, 1); return "crashed - restart SteamVR"; }
    if (s == "init_failed") { color = ImVec4(1.0f, 0.6f, 0.2f, 1); return "failed to start - see log"; }
    color = ImVec4(0.7f, 0.7f, 0.7f, 1);
    return s.c_str();
}

}  // namespace

void ControllersTab::Toast(std::string msg)
{
    toast_ = std::move(msg);
    toast_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(4);
}

void ControllersTab::Fetch(bool force)
{
    const auto now = std::chrono::steady_clock::now();
    if (!force && now < next_fetch_) return;
    // Don't overwrite a value the user is dragging right now.
    if (!force && ImGui::IsAnyItemActive()) return;
    next_fetch_ = now + kRefresh;
    json s;
    if (ctrl_bridge::GetState(s)) {
        state_ = std::move(s);
        have_state_ = true;
    } else {
        have_state_ = false;
    }
}

bool ControllersTab::DrawKeybind(const char* label, json& value, const std::string& id, OsdInput& input,
                                 bool compact)
{
    bool dirty = false;
    ImGui::PushID(id.c_str());
    if (capture_ready_ && capture_id_ == id) {
        value = captured_;
        capture_ready_ = false;
        capture_id_.clear();
        dirty = true;
    }
    const bool waiting = capture_id_ == id;
    std::string cur = value.is_string() ? value.get<std::string>() : std::string();

    if (!compact) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s", cur.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
        if (ImGui::InputText("##name", buf, sizeof(buf))) {
            value = std::string(buf);
            dirty = true;
        }
        ImGui::SameLine();
    }
    const std::string btn = waiting ? std::string("press a key...")
                                    : (compact ? (cur.empty() ? std::string("(none)") : cur) : std::string("Set"));
    if (ImGui::Button((btn + "##set").c_str())) {
        capture_id_ = id;
        capture_ready_ = false;
        input.BeginCapture(false);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
        value = "";
        dirty = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unbind");
    if (!compact) {
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
    }
    ImGui::PopID();
    return dirty;
}

bool ControllersTab::DrawTable(const json& item, json& values, const std::string& id, OsdInput& input)
{
    const std::string key = Str(item, "key");
    const json& cols = item.contains("columns") ? item["columns"] : json::array();
    if (!cols.is_array() || cols.empty()) return false;
    json& rows = values[key];
    if (!rows.is_array()) rows = json::array();

    bool dirty = false;
    ImGui::TextUnformatted(Str(item, "label", key).c_str());
    HelpMarker(item);

    const int ncols = (int)cols.size() + 1;
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX;
    const float height = ImGui::GetFrameHeightWithSpacing() * (float)((std::min)((size_t)8, rows.size()) + 1.5f);
    if (ImGui::BeginTable((id + "##table").c_str(), ncols, flags, ImVec2(0, height))) {
        for (const auto& c : cols) {
            const std::string t = Str(c, "type");
            const float w = ImGui::GetFontSize() *
                            (t == "string" ? 11.0f : t == "keybind" ? 8.0f : t == "bool" ? 2.5f : 4.5f);
            ImGui::TableSetupColumn(Str(c, "label", Str(c, "key")).c_str(), ImGuiTableColumnFlags_WidthFixed, w);
        }
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 1.5f);
        ImGui::TableHeadersRow();

        int remove = -1;
        for (size_t r = 0; r < rows.size(); ++r) {
            json& row = rows[r];
            if (!row.is_object()) row = json::object();
            ImGui::TableNextRow();
            ImGui::PushID((int)r);
            for (const auto& c : cols) {
                ImGui::TableNextColumn();
                const std::string ck = Str(c, "key");
                const std::string t = Str(c, "type");
                json& v = row[ck];
                if (v.is_null() && c.contains("default")) v = c["default"];
                ImGui::PushID(ck.c_str());
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (t == "bool") {
                    bool b = v.is_boolean() && v.get<bool>();
                    if (ImGui::Checkbox("##v", &b)) { v = b; dirty = true; }
                } else if (t == "float") {
                    float f = (float)Num(v, 0.0);
                    if (ImGui::DragFloat("##v", &f, 0.005f, (float)Num(c.value("min", json()), -1e6),
                                         (float)Num(c.value("max", json()), 1e6), "%.2f")) {
                        v = f;
                        dirty = true;
                    }
                } else if (t == "int") {
                    int i = (int)Num(v, 0.0);
                    if (ImGui::DragInt("##v", &i)) { v = i; dirty = true; }
                } else if (t == "enum") {
                    if (EnumCombo("##v", c.value("values", json::array()), v)) dirty = true;
                } else if (t == "keybind") {
                    if (DrawKeybind("", v, id + "/" + std::to_string(r) + "/" + ck, input, true)) dirty = true;
                } else {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "%s", v.is_string() ? v.get<std::string>().c_str() : "");
                    if (ImGui::InputText("##v", buf, sizeof(buf))) { v = std::string(buf); dirty = true; }
                }
                ImGui::PopID();
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("-")) remove = (int)r;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove row");
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (remove >= 0) {
            rows.erase(rows.begin() + remove);
            dirty = true;
        }
    }
    if (ImGui::SmallButton(("Add row##" + id).c_str())) {
        json row = json::object();
        for (const auto& c : cols)
            row[Str(c, "key")] = c.contains("default") ? c["default"] : json();
        rows.push_back(row);
        dirty = true;
    }
    if (item.contains("default")) {
        ImGui::SameLine();
        if (ImGui::SmallButton(("Restore defaults##" + id).c_str())) {
            rows = item["default"];
            dirty = true;
        }
    }
    return dirty;
}

bool ControllersTab::DrawSetting(const json& item, json& values, const std::string& id, OsdInput& input)
{
    if (!item.is_object()) return false;
    const std::string type = Str(item, "type");
    if (type == "header") {
        ImGui::SeparatorText(Str(item, "label").c_str());
        return false;
    }
    const std::string key = Str(item, "key");
    if (key.empty()) return false;
    const std::string label = Str(item, "label", key) + "##" + key;
    json& v = values[key];
    if (v.is_null() && item.contains("default")) v = item["default"];
    const std::string wid = id + "/" + key;

    bool dirty = false;
    ImGui::PushItemWidth(ImGui::GetFontSize() * 12.0f);
    if (type == "bool") {
        bool b = v.is_boolean() && v.get<bool>();
        if (ImGui::Checkbox(label.c_str(), &b)) { v = b; dirty = true; }
    } else if (type == "float") {
        float f = (float)Num(v, 0.0);
        const float lo = (float)Num(item.value("min", json()), 0.0);
        const float hi = (float)Num(item.value("max", json()), 1.0);
        const char* fmt = (hi - lo) <= 0.05f ? "%.5f" : (hi - lo) <= 2.0f ? "%.3f" : "%.2f";
        if (ImGui::SliderFloat(label.c_str(), &f, lo, hi, fmt)) { v = f; dirty = true; }
    } else if (type == "int") {
        int i = (int)Num(v, 0.0);
        const int lo = (int)Num(item.value("min", json()), 0.0);
        const int hi = (int)Num(item.value("max", json()), 100.0);
        if (hi - lo > 1000) {
            if (ImGui::InputInt(label.c_str(), &i)) { v = (std::clamp)(i, lo, hi); dirty = true; }
        } else if (ImGui::SliderInt(label.c_str(), &i, lo, hi)) {
            v = i;
            dirty = true;
        }
    } else if (type == "enum") {
        if (EnumCombo(label.c_str(), item.value("values", json::array()), v)) dirty = true;
    } else if (type == "keybind") {
        if (DrawKeybind(Str(item, "label", key).c_str(), v, wid, input, false)) dirty = true;
    } else if (type == "string") {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", v.is_string() ? v.get<std::string>().c_str() : "");
        if (ImGui::InputText(label.c_str(), buf, sizeof(buf))) { v = std::string(buf); dirty = true; }
    } else if (type == "table") {
        ImGui::PopItemWidth();
        return DrawTable(item, values, wid, input);
    } else {
        ImGui::TextDisabled("%s: unsupported setting type '%s'", key.c_str(), type.c_str());
    }
    ImGui::PopItemWidth();
    HelpMarker(item);
    return dirty;
}

bool ControllersTab::DrawPluginList(json& cfg)
{
    bool dirty = false;
    json& prio = cfg["plugin_priority"];
    if (!prio.is_array()) prio = json::array();
    json& plugins = cfg["plugins"];
    if (!plugins.is_object()) plugins = json::object();

    // Status / display names by plugin id.
    const json& list = state_.contains("plugins") ? state_["plugins"] : json::array();
    auto info = [&](const std::string& name) -> const json* {
        for (const auto& p : list)
            if (Str(p, "name") == name) return &p;
        return nullptr;
    };

    ImGui::TextDisabled("Higher in the list wins the hand pose; buttons from all enabled sources are combined.");
    int move_from = -1, move_to = -1;
    for (size_t i = 0; i < prio.size(); ++i) {
        const std::string name = prio[i].is_string() ? prio[i].get<std::string>() : std::string();
        const json* p = info(name);
        if (!p) continue;
        ImGui::PushID(name.c_str());
        json& ps = plugins[name];
        if (!ps.is_object()) ps = json::object();
        bool en = ps.value("enabled", false);
        const std::string disp = Str(*p, "display_name", name);
        if (ImGui::Checkbox(disp.c_str(), &en)) {
            ps["enabled"] = en;
            dirty = true;
        }
        ImGui::SameLine();
        ImVec4 col;
        const char* st = StateLabel(Str(*p, "state"), col);
        ImGui::TextColored(col, "[%s]", st);
        if (!p->value("builtin", true)) {
            ImGui::SameLine();
            ImGui::TextDisabled("plugin %s", Str(*p, "version").c_str());
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("up", ImGuiDir_Up) && i > 0) { move_from = (int)i; move_to = (int)i - 1; }
        ImGui::SameLine();
        if (ImGui::ArrowButton("down", ImGuiDir_Down) && i + 1 < prio.size()) { move_from = (int)i; move_to = (int)i + 1; }
        ImGui::PopID();
    }
    if (move_from >= 0) {
        std::swap(prio[(size_t)move_from], prio[(size_t)move_to]);
        dirty = true;
    }
    return dirty;
}

void ControllersTab::Draw(OsdInput& input, const Hints& hints)
{
    Fetch(false);

    if (!have_state_) {
        ImGui::TextWrapped("The emulated controller driver (vrto3d_ctrl) is not loaded.");
        ImGui::TextWrapped("Install it next to VRto3D in SteamVR\\drivers and restart SteamVR. "
                           "It lets keyboard + mouse, a gamepad, or other apps act as VR "
                           "motion controllers.");
        return;
    }

    // Key capture in progress: drain the picker (Esc cancels).
    if (!capture_id_.empty() && !capture_ready_) {
        ImGui::TextColored(ImVec4(1, 0.85f, 0.2f, 1), "Press a key, mouse button or gamepad button (Esc to cancel)");
        CapturedKey c = input.PollCapture();
        if (c.valid) {
            captured_ = c.key_name;
            capture_ready_ = true;
            input.CancelCapture();
        } else if (input.WasPressed(VK_ESCAPE)) {
            capture_id_.clear();
            input.CancelCapture();
        }
    }

    json cfg = state_.value("config", json::object());
    const std::string game = Str(state_, "game");
    const std::string running_type = Str(state_, "devices_type", "knuckles");
    bool dirty = false;

    ImGui::TextDisabled("Driver %s", Str(state_, "version").c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("| Game: %s", game.empty() ? "(none)" : game.c_str());

    bool enabled = cfg.value("enabled", false);
    if (ImGui::Checkbox("Enable emulated controllers", &enabled)) {
        cfg["enabled"] = enabled;
        dirty = true;
    }

    {
        static const char* kTypes[] = {"knuckles", "vive_controller"};
        static const char* kLabels[] = {"Valve Index (knuckles)", "HTC Vive wand"};
        const std::string cur = Str(cfg, "emulated_type", "knuckles");
        int sel = cur == "vive_controller" ? 1 : 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::Combo("Emulated controller", &sel, kLabels, 2)) {
            cfg["emulated_type"] = kTypes[sel];
            dirty = true;
        }
        if (Str(cfg, "emulated_type", "knuckles") != running_type) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "(save, then restart SteamVR)");
        }
    }

    // Warnings about VRto3D settings that fight the controller plugins.
    const json& plugins_cfg = cfg.contains("plugins") ? cfg["plugins"] : json::object();
    auto plugin_on = [&](const char* n) {
        return plugins_cfg.contains(n) && plugins_cfg[n].is_object() && plugins_cfg[n].value("enabled", false);
    };
    if (enabled && plugin_on("kbm") && !hints.opentrack_enabled) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1),
                           "Keyboard + Mouse head look needs OpenTrack: enable it on the Tracking tab.");
    }
    if (enabled && plugin_on("xinput") && hints.xinput_head_look) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1),
                           "XInput pitch/yaw (Tracking tab) also uses the right stick.");
    }

    if (ImGui::CollapsingHeader("Input sources", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (DrawPluginList(cfg)) dirty = true;
    }

    // One section per plugin, drawn from its schema.
    const json& list = state_.contains("plugins") ? state_["plugins"] : json::array();
    for (const auto& p : list) {
        const std::string name = Str(p, "name");
        const json& schema = p.contains("schema") ? p["schema"] : json::object();
        if (!schema.contains("settings") || !schema["settings"].is_array() || schema["settings"].empty())
            continue;
        const std::string header = Str(p, "display_name", name) + " settings###plugin_" + name;
        if (!ImGui::CollapsingHeader(header.c_str())) continue;
        json& values = cfg["plugins"][name];
        if (!values.is_object()) values = json::object();
        ImGui::PushID(name.c_str());
        ImGui::Indent();
        for (const auto& item : schema["settings"])
            if (DrawSetting(item, values, name, input)) dirty = true;
        ImGui::Unindent();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Hands")) {
        ImGui::TextDisabled("Offsets are applied to each hand's aim pose (metres / degrees).");
        const char* hands[2] = {"left_offset", "right_offset"};
        const char* names[2] = {"Left", "Right"};
        for (int h = 0; h < 2; ++h) {
            json& o = cfg[hands[h]];
            if (!o.is_object()) o = json::object();
            ImGui::PushID(h);
            if (Vec3Editor((std::string(names[h]) + " position (x, y, z)").c_str(), o["pos"], 0.005f, -1.0f, 1.0f, "%.3f")) dirty = true;
            if (Vec3Editor((std::string(names[h]) + " rotation (pitch, yaw, roll)").c_str(), o["rot"], 0.5f, -180.0f, 180.0f, "%.1f")) dirty = true;
            ImGui::PopID();
        }
        float th = (float)Num(cfg.value("click_threshold", json(0.5)), 0.5);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::SliderFloat("Trigger / grip click point", &th, 0.1f, 0.9f, "%.2f")) {
            cfg["click_threshold"] = th;
            dirty = true;
        }
        bool dv = cfg.value("derive_velocity", false);
        if (ImGui::Checkbox("Derive hand velocity (throwing)", &dv)) {
            cfg["derive_velocity"] = dv;
            dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Estimates velocity from hand motion so thrown objects fly.\n"
                              "Can add a little jitter to the hands.");
        bool lh = cfg.value("log_haptics", false);
        if (ImGui::Checkbox("Log haptic events", &lh)) {
            cfg["log_haptics"] = lh;
            dirty = true;
        }
    }

    if (dirty) {
        state_["config"] = cfg;
        if (!ctrl_bridge::ApplyConfig(cfg)) Toast("Controller driver did not accept the change");
        next_fetch_ = std::chrono::steady_clock::now() + kRefresh;
    }

    ImGui::Separator();
    if (ImGui::Button("Save Default Controllers")) {
        Toast(ctrl_bridge::Save(0) ? "Saved controllers.json" : "Save failed - see vrto3d_ctrl log");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(game.empty());
    const std::string game_btn = game.empty() ? std::string("Save Game Controllers")
                                              : "Save " + game + " Controllers";
    if (ImGui::Button(game_btn.c_str())) {
        Toast(ctrl_bridge::Save(1) ? "Saved " + game + "_controllers.json"
                                   : "Save failed - see vrto3d_ctrl log");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reload Controllers")) {
        Toast(ctrl_bridge::Reload() ? "Reloaded from disk" : "Reload failed");
        next_fetch_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    }
    if (!toast_.empty() && std::chrono::steady_clock::now() < toast_until_) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", toast_.c_str());
    }
    ImGui::TextDisabled("Files: %s", Str(state_, "config_folder").c_str());
}

}  // namespace vrto3d::osd
