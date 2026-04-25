/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include "openvr_driver.h"

namespace platform {

struct MonitorInfo {
    int32_t     index = 0;              // 1-based enumeration order; 0 = primary fallback
    int32_t     x = 0;
    int32_t     y = 0;
    uint32_t    width = 0;
    uint32_t    height = 0;
    float       refresh_hz = 60.0f;
    std::string device_name;            // OS display device name ("\\.\DISPLAY1" on Win)
    bool        is_primary = false;
};

// Enumerate all monitors in display-index order (1-based).
std::vector<MonitorInfo> EnumerateMonitors();

// Resolve display_index (0 = primary/auto, 1..N = enumeration order). If
// multi_display is true, out_secondary is filled with the contiguous right
// neighbor when it has matching geometry; otherwise out_secondary.width == 0.
bool ResolveTargetMonitors(int32_t display_index,
                           bool multi_display,
                           MonitorInfo& out_primary,
                           MonitorInfo& out_secondary);

// Refresh rate lookup for the resolved primary monitor.
float QueryRefreshHz(const MonitorInfo& monitor, float fallback_hz = 60.0f);

// LUID of the default graphics adapter — compositor picks GPU off the HMD's
// Prop_GraphicsAdapterLuid_Uint64.
LUID PrimaryAdapterLuid();

// Create a D3D11 device on the adapter matching the given LUID. Returns true on success.
bool CreateD3D11Device(LUID adapter_luid,
                       Microsoft::WRL::ComPtr<ID3D11Device>& out_device,
                       Microsoft::WRL::ComPtr<ID3D11DeviceContext>& out_context,
                       Microsoft::WRL::ComPtr<IDXGIAdapter1>& out_adapter);

// Import a SteamVR-provided SharedTextureHandle_t into a D3D11 texture on the
// given device. On Windows: OpenSharedResource1/OpenSharedResource. On Linux
// via DXVK: ID3D11VkExtDevice::CreateTexture2DFromVkImage.
bool ImportSharedTexture(ID3D11Device* device,
                         vr::SharedTextureHandle_t handle,
                         Microsoft::WRL::ComPtr<ID3D11Texture2D>& out_texture);


// Borderless topmost present window. One per presenter. Owns its native handle.
class PresentWindow {
public:
    virtual ~PresentWindow() = default;

    // Returns HWND on Windows, SDL_Window* on Linux (caller casts).
    virtual void* NativeHandle() const = 0;

    // Monitor geometry that was used to size the window.
    virtual uint32_t Width()  const = 0;
    virtual uint32_t Height() const = 0;

    // Drain the OS message/event queue; must be called from the thread that created the window.
    virtual void PollEvents() = 0;

    // Set topmost / z-order. No-op if already in desired state.
    virtual void BringToTop() = 0;
    virtual void ReleaseTopmost() = 0;

    // For multi_display layouts: nudge the window right by one pixel then back
    // to work around a two-monitor placement glitch on first show.
    virtual void MultiDisplayNudge() = 0;

    virtual bool ShouldClose() const = 0;
};

std::unique_ptr<PresentWindow> CreatePresentWindow(const MonitorInfo& primary,
                                                   const MonitorInfo* secondary_for_multi_display,
                                                   uint32_t override_height,   // 0 = use monitor height; non-zero for vd_fsbs_hack
                                                   const char* title);


// Lightweight process helpers (for focus-thread's "is the tracked app still running?" check).
bool IsProcessRunning(uint32_t pid);

}  // namespace platform
