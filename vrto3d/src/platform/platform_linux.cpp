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

#ifndef _WIN32

// Linux implementation (v1 skeleton): SDL2 windowing + DXVK-interop texture import.
// Full bring-up is a v1.5 task — see plan. These stubs let the driver compile
// against DXVK + SDL2 so the Linux build is smoke-testable.

#include "platform.h"

#include "vrto3dlib/debug_log.hpp"

#if __has_include(<SDL2/SDL.h>)
#  include <SDL2/SDL.h>
#  define VRTO3D_HAVE_SDL2 1
#else
#  define VRTO3D_HAVE_SDL2 0
#endif

namespace platform {

std::vector<MonitorInfo> EnumerateMonitors()
{
#if VRTO3D_HAVE_SDL2
    std::vector<MonitorInfo> out;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return out;

    const int n = SDL_GetNumVideoDisplays();
    for (int i = 0; i < n; ++i) {
        SDL_Rect r{};
        SDL_DisplayMode mode{};
        if (SDL_GetDisplayBounds(i, &r) != 0)           continue;
        if (SDL_GetDesktopDisplayMode(i, &mode) != 0)   continue;

        MonitorInfo m;
        m.index       = i + 1;
        m.x           = r.x;
        m.y           = r.y;
        m.width       = static_cast<uint32_t>(r.w);
        m.height      = static_cast<uint32_t>(r.h);
        m.refresh_hz  = static_cast<float>(mode.refresh_rate);
        m.is_primary  = (i == 0);
        const char* name = SDL_GetDisplayName(i);
        m.device_name = name ? name : std::string("display") + std::to_string(i);
        out.push_back(m);
    }
    return out;
#else
    LOG() << "Linux platform: SDL2 not available at build time; EnumerateMonitors returning empty";
    return {};
#endif
}

bool ResolveTargetMonitors(int32_t display_index, bool multi_display,
                           MonitorInfo& out_primary, MonitorInfo& out_secondary)
{
    out_secondary = {};
    auto monitors = EnumerateMonitors();
    if (monitors.empty()) return false;

    if (display_index <= 0 || display_index > static_cast<int32_t>(monitors.size()))
        out_primary = monitors.front();
    else
        out_primary = monitors[static_cast<size_t>(display_index - 1)];

    if (multi_display) {
        const int32_t right_x = out_primary.x + static_cast<int32_t>(out_primary.width);
        for (const auto& m : monitors) {
            if (m.index == out_primary.index) continue;
            if (m.y == out_primary.y && m.x == right_x
                && m.width == out_primary.width && m.height == out_primary.height) {
                out_secondary = m;
                break;
            }
        }
    }
    return true;
}

float QueryRefreshHz(const MonitorInfo& monitor, float fallback_hz)
{
    return monitor.refresh_hz > 1.0f ? monitor.refresh_hz : fallback_hz;
}

LUID PrimaryAdapterLuid()
{
    // DXVK reports the VK physical-device LUID via DXGI_ADAPTER_DESC; we pick
    // it up through CreateD3D11Device below. For the property write, zero is
    // acceptable — the compositor picks its GPU itself if LUID is zero.
    return LUID{};
}

bool CreateD3D11Device(LUID /*adapter_luid*/,
                       Microsoft::WRL::ComPtr<ID3D11Device>& /*out_device*/,
                       Microsoft::WRL::ComPtr<ID3D11DeviceContext>& /*out_context*/,
                       Microsoft::WRL::ComPtr<IDXGIAdapter1>& /*out_adapter*/)
{
    // v1.5: link against DXVK's libdxvk_d3d11 and call D3D11CreateDevice the
    // same way as Win32. Adapter LUID matching works through DXVK's DXGI shim.
    LOG() << "Linux: CreateD3D11Device is a v1.5 TODO (requires DXVK linkage)";
    return false;
}

bool ImportSharedTexture(ID3D11Device* /*device*/,
                         vr::SharedTextureHandle_t /*handle*/,
                         Microsoft::WRL::ComPtr<ID3D11Texture2D>& /*out_texture*/)
{
    // v1.5: use ID3D11VkExtDevice::CreateTexture2DFromVkImage (DXVK) to import
    // the Vulkan image (or DMABUF fd imported to VkImage first) that SteamVR
    // Linux hands us via PresentInfo_t::backbufferTextureHandle.
    LOG() << "Linux: ImportSharedTexture is a v1.5 TODO (requires DXVK VK-interop)";
    return false;
}


namespace {

#if VRTO3D_HAVE_SDL2
class SDL2PresentWindow : public PresentWindow {
public:
    SDL2PresentWindow(SDL_Window* win, uint32_t w, uint32_t h) : win_(win), w_(w), h_(h) {}
    ~SDL2PresentWindow() override { if (win_) SDL_DestroyWindow(win_); }

    void*    NativeHandle() const override { return win_; }
    uint32_t Width()        const override { return w_; }
    uint32_t Height()       const override { return h_; }

    void PollEvents() override
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) should_close_ = true;
        }
    }

    void BringToTop() override
    {
        if (is_topmost_) return;
        SDL_RaiseWindow(win_);
        SDL_SetWindowAlwaysOnTop(win_, SDL_TRUE);
        is_topmost_ = true;
    }

    void ReleaseTopmost() override
    {
        if (!is_topmost_) return;
        SDL_SetWindowAlwaysOnTop(win_, SDL_FALSE);
        is_topmost_ = false;
    }

    void MultiDisplayNudge() override
    {
        int x = 0, y = 0;
        SDL_GetWindowPosition(win_, &x, &y);
        SDL_SetWindowPosition(win_, x + 1, y);
        SDL_SetWindowPosition(win_, x,     y);
    }

    bool ShouldClose() const override { return should_close_; }

private:
    SDL_Window* win_ = nullptr;
    uint32_t    w_ = 0, h_ = 0;
    bool        is_topmost_ = false;
    bool        should_close_ = false;
};
#endif

}  // namespace

std::unique_ptr<PresentWindow> CreatePresentWindow(const MonitorInfo& primary,
                                                   const MonitorInfo* secondary_for_multi_display,
                                                   uint32_t override_height,
                                                   const char* title)
{
#if VRTO3D_HAVE_SDL2
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        LOG() << "SDL_InitSubSystem(VIDEO) failed: " << SDL_GetError();
        return nullptr;
    }

    uint32_t w = primary.width;
    uint32_t h = override_height > 0 ? override_height : primary.height;
    if (secondary_for_multi_display && secondary_for_multi_display->width > 0) {
        w = primary.width + secondary_for_multi_display->width;
    }

    SDL_Window* win = SDL_CreateWindow(
        title ? title : "VRto3D",
        primary.x, primary.y,
        static_cast<int>(w), static_cast<int>(h),
        SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_SHOWN);
    if (!win) {
        LOG() << "SDL_CreateWindow failed: " << SDL_GetError();
        return nullptr;
    }
    return std::make_unique<SDL2PresentWindow>(win, w, h);
#else
    (void)primary; (void)secondary_for_multi_display; (void)override_height; (void)title;
    LOG() << "Linux: CreatePresentWindow requires SDL2 at build time";
    return nullptr;
#endif
}

bool IsProcessRunning(uint32_t pid)
{
    if (pid == 0) return false;
    // /proc/<pid>/status exists while the process is alive.
    std::string path = "/proc/" + std::to_string(pid) + "/status";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    fclose(f);
    return true;
}

}  // namespace platform

#endif  // !_WIN32
