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

// SteamVR Config Manager: pick which SteamVR driver is active and keep named
// copies of steamvr.vrsettings so a VRto3D setup and a normal headset setup
// can be swapped without losing either. Win32 + D3D11 + Dear ImGui, following
// imgui's example_win32_directx11 bootstrap.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "cfg_store.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "shell32.lib")

using namespace cfgmgr;

namespace {

// ---------------------------------------------------------------------------
// D3D11 plumbing (straight from the imgui example)
// ---------------------------------------------------------------------------

ID3D11Device*           g_device = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
IDXGISwapChain*         g_swapchain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool                    g_occluded = false;
UINT                    g_resize_w = 0, g_resize_h = 0;

void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

void CleanupRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &sd, &g_swapchain, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION, &sd, &g_swapchain, &g_device, &got, &g_context);
    if (hr != S_OK) return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swapchain) { g_swapchain->Release(); g_swapchain = nullptr; }
    if (g_context)   { g_context->Release();   g_context = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

struct App {
    Paths       paths;
    bool        paths_ok = false;
    std::string paths_err;

    std::vector<DriverInfo>  drivers;
    std::vector<ProfileInfo> profiles;
    std::vector<ProfileInfo> autobackups;

    Json        doc;    // steamvr.vrsettings as on disk
    Json        edit;   // working copy the driver controls edit
    bool        doc_ok = false;
    std::string doc_err;
    unsigned long long doc_stamp = 0;

    std::string active_profile;
    bool        steamvr_running = false;
    ULONGLONG   next_poll = 0;

    char new_profile_name[128] = {};
    bool save_back = true;

    std::string status;
    bool        status_is_error = false;

    // Action picked from a table row. Run after the frame is built because it
    // reloads the lists the rows are iterating.
    std::function<void()> deferred;

    // Pending confirmation dialog.
    bool                  open_confirm = false;
    std::string           confirm_text;
    std::function<void()> confirm_action;

    bool Dirty() const { return doc_ok && edit != doc; }
    bool CanWrite() const { return paths_ok && doc_ok && !steamvr_running; }

    void SetStatus(std::string msg, bool error = false) {
        status = std::move(msg);
        status_is_error = error;
    }

    void Confirm(std::string text, std::function<void()> action) {
        confirm_text = std::move(text);
        confirm_action = std::move(action);
        open_confirm = true;
    }

    void ReloadSettings() {
        doc_err.clear();
        doc_ok = LoadJsonFile(paths.vrsettings, doc, doc_err);
        if (!doc_ok) doc = Json::object();
        edit = doc;
        doc_stamp = FileStamp(paths.vrsettings);
    }

    void ReloadLists() {
        profiles = ListProfiles(paths);
        autobackups = ListAutoBackups(paths);
        active_profile = LoadActiveProfile(paths);
    }

    void ReloadAll() {
        paths_err.clear();
        paths_ok = LocatePaths(paths, paths_err);
        if (!paths_ok) return;
        drivers = ScanDrivers(paths);
        ReloadSettings();
        ReloadLists();
    }

    // Once a second: track SteamVR, and pick up outside edits to the file
    // (e.g. SteamVR rewriting it on exit) as long as nothing is pending here.
    void Poll() {
        const ULONGLONG now = GetTickCount64();
        if (now < next_poll || !paths_ok) return;
        next_poll = now + 1000;
        steamvr_running = IsSteamVRRunning();
        if (!Dirty() && FileStamp(paths.vrsettings) != doc_stamp) {
            ReloadSettings();
            ReloadLists();
        }
    }

    bool ProfileExists(const std::string& name) const {
        for (const auto& p : profiles)
            if (_stricmp(p.name.c_str(), name.c_str()) == 0) return true;
        return false;
    }

    // ---- actions ----------------------------------------------------------

    void ApplyEdits() {
        std::string err;
        if (!AutoBackup(paths, "before edit", err) || !WriteVrSettings(paths, edit, err)) {
            SetStatus(err, true);
            return;
        }
        ReloadSettings();
        ReloadLists();
        SetStatus("Driver settings saved to steamvr.vrsettings.");
    }

    void SaveAs(const std::string& name) {
        std::string err;
        const std::string clean = SanitizeProfileName(name);
        if (!SaveProfile(paths, clean, err)) {
            SetStatus(err, true);
            return;
        }
        SaveActiveProfile(paths, clean);
        ReloadLists();
        SetStatus("Saved current settings as profile '" + clean + "'.");
    }

    void Restore(const ProfileInfo& target, bool is_profile) {
        std::string err;
        if (save_back && !active_profile.empty() && ProfileExists(active_profile) &&
            (!is_profile || _stricmp(active_profile.c_str(), target.name.c_str()) != 0)) {
            if (!SaveProfile(paths, active_profile, err)) {
                SetStatus("Could not save current settings back to '" + active_profile + "': " + err, true);
                return;
            }
        }
        if (!RestoreFile(paths, target.path, err)) {
            SetStatus(err, true);
            return;
        }
        SaveActiveProfile(paths, is_profile ? target.name : std::string());
        ReloadSettings();
        ReloadLists();
        SetStatus("Restored '" + target.name + "'. Start SteamVR to use it.");
    }

    void Reset() {
        std::string err;
        if (!ResetVrSettings(paths, err)) {
            SetStatus(err, true);
            return;
        }
        SaveActiveProfile(paths, std::string());
        ReloadSettings();
        ReloadLists();
        SetStatus("steamvr.vrsettings removed (backed up). SteamVR will start from its defaults.");
    }
};

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void OpenFolder(const fs::path& p) {
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void DrawHeader(App& app) {
    if (!app.paths_ok) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", app.paths_err.c_str());
        if (ImGui::Button("Retry")) app.ReloadAll();
        return;
    }
    ImGui::Text("Steam:    %s", app.paths.steam.u8string().c_str());
    ImGui::Text("SteamVR:  %s", app.paths.steamvr.empty() ? "(not found)" : app.paths.steamvr.u8string().c_str());
    ImGui::Text("Settings: %s", app.paths.vrsettings.u8string().c_str());

    if (ImGui::Button("Refresh")) {
        app.ReloadAll();
        app.SetStatus("Reloaded.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open profiles folder")) OpenFolder(app.paths.store);
    ImGui::SameLine();
    if (ImGui::Button("Open Steam config folder")) OpenFolder(app.paths.vrsettings.parent_path());

    if (app.steamvr_running) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1, 0.65f, 0.2f, 1),
            "SteamVR is running. Close it before making changes - it rewrites steamvr.vrsettings on exit.");
    }
    if (!app.doc_ok) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s Editing is disabled; restore a profile or reset to fix it.",
                           app.doc_err.c_str());
    }
}

void DrawDrivers(App& app) {
    if (!ImGui::CollapsingHeader("Active SteamVR driver", ImGuiTreeNodeFlags_DefaultOpen)) return;

    ImGui::BeginDisabled(!app.CanWrite());

    // Forced HMD driver.
    const std::string forced = GetForcedDriver(app.edit);
    std::string preview = forced.empty() ? "Auto (SteamVR chooses)" : forced;
    const bool forced_known = forced.empty() || std::any_of(app.drivers.begin(), app.drivers.end(),
        [&](const DriverInfo& d) { return d.name == forced; });
    if (!forced_known) preview += " (not installed)";

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    if (ImGui::BeginCombo("Forced HMD driver", preview.c_str())) {
        if (ImGui::Selectable("Auto (SteamVR chooses)", forced.empty())) SetForcedDriver(app.edit, "");
        for (const auto& d : app.drivers) {
            if (ImGui::Selectable(d.name.c_str(), d.name == forced)) SetForcedDriver(app.edit, d.name);
        }
        ImGui::EndCombo();
    }
    HelpMarker("steamvr.forcedDriver. 'Auto' lets SteamVR use whichever headset it detects. "
               "Forcing a driver makes SteamVR use it as the headset even when another one is connected.");

    bool multi = GetSteamVRBool(app.edit, "activateMultipleDrivers", false);
    if (ImGui::Checkbox("Activate multiple drivers", &multi))
        SetSteamVRBool(app.edit, "activateMultipleDrivers", multi, false);
    HelpMarker("steamvr.activateMultipleDrivers. Lets other drivers (e.g. lighthouse controllers and "
               "trackers) load alongside the forced HMD driver.");

    bool require_hmd = GetSteamVRBool(app.edit, "requireHmd", true);
    if (ImGui::Checkbox("Require HMD", &require_hmd))
        SetSteamVRBool(app.edit, "requireHmd", require_hmd, true);
    HelpMarker("steamvr.requireHmd. When off, SteamVR starts even without a headset.");

    ImGui::Spacing();
    const ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("drivers", 4, tflags)) {
        ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Driver");
        ImGui::TableSetupColumn("Location");
        ImGui::TableSetupColumn("Notes");
        ImGui::TableHeadersRow();
        for (const auto& d : app.drivers) {
            ImGui::PushID(d.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool en = DriverEnabled(app.edit, d);
            if (ImGui::Checkbox("##en", &en)) SetDriverEnabled(app.edit, d, en);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(d.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(d.external ? d.dir.u8string().c_str() : "SteamVR\\drivers");
            ImGui::TableNextColumn();
            std::string notes;
            if (d.name == forced) notes += "Forced HMD. ";
            if (!d.default_enable) notes += "Off by default. ";
            if (DriverBlockedBySafeMode(app.edit, d)) notes += "Blocked by safe mode. ";
            ImGui::TextUnformatted(notes.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (SafeModeActive(app.edit)) {
        ImGui::TextColored(ImVec4(1, 0.65f, 0.2f, 1),
            "SteamVR safe mode is on - add-on drivers are blocked from loading.");
        ImGui::SameLine();
        if (ImGui::Button("Clear safe mode")) ClearSafeMode(app.edit);
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::BeginDisabled(!app.CanWrite() || !app.Dirty());
    if (ImGui::Button("Apply changes")) app.ApplyEdits();
    ImGui::SameLine();
    if (ImGui::Button("Discard changes")) app.edit = app.doc;
    ImGui::EndDisabled();
    if (app.Dirty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "Unsaved changes");
    }
}

void DrawProfiles(App& app) {
    if (!ImGui::CollapsingHeader("Settings profiles", ImGuiTreeNodeFlags_DefaultOpen)) return;

    ImGui::TextWrapped("A profile is a saved copy of the whole steamvr.vrsettings file. Save one for each "
                       "setup (e.g. 'VRto3D' and 'Index'), then restore the one you want before starting SteamVR.");
    ImGui::Text("Current profile: %s",
                app.active_profile.empty() ? "(none)" : app.active_profile.c_str());

    const bool can_swap = app.paths_ok && !app.steamvr_running && !app.Dirty();
    if (app.Dirty())
        ImGui::TextDisabled("Apply or discard the driver changes above before saving or restoring profiles.");

    ImGui::BeginDisabled(!can_swap);

    if (!app.active_profile.empty() && app.ProfileExists(app.active_profile)) {
        const std::string label = "Before restoring, save current settings back into '" + app.active_profile + "'";
        ImGui::Checkbox(label.c_str(), &app.save_back);
        HelpMarker("Keeps the profile you are switching away from up to date with any changes "
                   "SteamVR made while you used it.");
    }

    const ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (app.profiles.empty()) {
        ImGui::TextDisabled("No profiles saved yet.");
    } else if (ImGui::BeginTable("profiles", 3, tflags)) {
        ImGui::TableSetupColumn("Profile");
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const auto& p : app.profiles) {
            ImGui::PushID(p.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool active = _stricmp(p.name.c_str(), app.active_profile.c_str()) == 0;
            if (active) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "%s (current)", p.name.c_str());
            else        ImGui::TextUnformatted(p.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.modified.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Restore")) {
                const ProfileInfo target = p;
                app.deferred = [&app, target]() { app.Restore(target, true); };
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Update")) {
                const std::string name = p.name;
                app.Confirm("Replace profile '" + name + "' with the current steamvr.vrsettings?",
                            [&app, name]() { app.SaveAs(name); });
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) {
                const ProfileInfo target = p;
                app.Confirm("Delete profile '" + target.name + "'?", [&app, target]() {
                    std::string err;
                    if (DeleteProfile(target, err)) app.SetStatus("Deleted '" + target.name + "'.");
                    else                            app.SetStatus(err, true);
                    app.ReloadLists();
                });
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    const bool enter = ImGui::InputTextWithHint("##newname", "Profile name, e.g. VRto3D", app.new_profile_name,
                                                sizeof(app.new_profile_name), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Save current settings as profile") || enter) {
        const std::string name = SanitizeProfileName(app.new_profile_name);
        if (name.empty()) {
            app.SetStatus("Enter a profile name.", true);
        } else if (app.ProfileExists(name)) {
            app.Confirm("Profile '" + name + "' already exists. Replace it?", [&app, name]() { app.SaveAs(name); });
        } else {
            app.SaveAs(name);
            app.new_profile_name[0] = '\0';
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset steamvr.vrsettings to SteamVR defaults...")) {
        app.Confirm("Remove steamvr.vrsettings so SteamVR starts from its defaults?\n"
                    "A copy is kept under Automatic backups.", [&app]() { app.Reset(); });
    }

    if (ImGui::TreeNode("autobackups", "Automatic backups (%d)", static_cast<int>(app.autobackups.size()))) {
        ImGui::TextDisabled("Taken before every change this tool makes. The newest %d are kept.",
                            static_cast<int>(kMaxAutoBackups));
        if (!app.autobackups.empty() && ImGui::BeginTable("backups", 2, tflags)) {
            ImGui::TableSetupColumn("Backup");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            for (const auto& b : app.autobackups) {
                ImGui::PushID(b.name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(b.name.c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Restore")) {
                    const ProfileInfo target = b;
                    app.Confirm("Restore automatic backup '" + target.name + "'?",
                                [&app, target]() { app.Restore(target, false); });
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }

    ImGui::EndDisabled();
}

void DrawConfirm(App& app) {
    if (app.open_confirm) {
        ImGui::OpenPopup("Confirm");
        app.open_confirm = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(app.confirm_text.c_str());
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(ImGui::GetFontSize() * 6, 0))) {
            if (app.confirm_action) app.confirm_action();
            app.confirm_action = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 6, 0))) {
            app.confirm_action = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DrawUI(App& app) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("SteamVR Config Manager", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    DrawHeader(app);
    if (app.paths_ok) {
        ImGui::Separator();
        DrawDrivers(app);
        DrawProfiles(app);
    }

    if (!app.status.empty()) {
        ImGui::Separator();
        const ImVec4 col = app.status_is_error ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(0.5f, 0.9f, 0.5f, 1);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextWrapped("%s", app.status.c_str());
        ImGui::PopStyleColor();
    }

    DrawConfirm(app);
    ImGui::End();

    if (app.deferred) {
        auto action = std::move(app.deferred);
        app.deferred = nullptr;
        action();
    }
}

}  // namespace

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) return 0;
        g_resize_w = LOWORD(lparam);
        g_resize_h = HIWORD(lparam);
        return 0;
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hinst,
                    LoadIconW(nullptr, IDI_APPLICATION), LoadCursorW(nullptr, IDC_ARROW),
                    nullptr, nullptr, L"VRto3D_SteamVRCfgMgr", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"SteamVR Config Manager", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              static_cast<int>(1000 * scale), static_cast<int>(760 * scale),
                              nullptr, nullptr, hinst, nullptr);
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hinst);
        MessageBoxW(nullptr, L"Could not create a Direct3D 11 device.", L"SteamVR Config Manager", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    wchar_t windir[MAX_PATH] = {};
    GetWindowsDirectoryW(windir, MAX_PATH);
    const fs::path font = fs::path(windir) / "Fonts" / "segoeui.ttf";
    std::error_code ec;
    if (!fs::exists(font, ec) || !io.Fonts->AddFontFromFileTTF(font.u8string().c_str(), 17.0f * scale)) {
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&cfg);
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    App app;
    app.ReloadAll();
    app.steamvr_running = IsSteamVRRunning();

    bool done = false;
    while (!done) {
        // Idle politely: block until input arrives or the 1s poll is due.
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_occluded && g_swapchain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) continue;
        g_occluded = false;

        if (g_resize_w != 0 && g_resize_h != 0) {
            CleanupRenderTarget();
            g_swapchain->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            CreateRenderTarget();
        }

        app.Poll();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUI(app);
        ImGui::Render();

        const float clear[4] = { 0.06f, 0.06f, 0.07f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_occluded = g_swapchain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hinst);
    return 0;
}
