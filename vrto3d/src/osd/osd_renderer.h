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

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#ifdef _WIN32
#include <wrl/client.h>
#include <d3d11.h>
#else
#include <vulkan/vulkan.h>
namespace vrto3d::vk { struct DeviceCtx; }
#endif

class StereoDisplayComponent;

namespace vrto3d::osd {

class OsdMenu;
class OsdInput;
struct MenuCallbacks;

// OsdRenderer owns the ImGui context, an offscreen RGBA render target sized
// per-eye, and a two-pass composite shader that overlays the OSD into both
// halves of the SbS frame produced by the SteamVR compositor.
//
// Lifetime: created lazily by Dx11Renderer once it knows the per-eye
// dimensions; destroyed when Dx11Renderer is shut down.
//
// Thread model: all methods are called from the window thread (the same
// thread that owns the D3D11 immediate context).
class OsdRenderer {
public:
    OsdRenderer();
    ~OsdRenderer();

#ifdef _WIN32
    // Initialize ImGui + composite resources. `eye_w`/`eye_h` are the per-eye
    // dimensions of the SbS frame (i.e. width = full_sbs_width / 2).
    // `headset_hwnd` is used for mouse coordinate mapping.
    bool Init(ID3D11Device* device,
              ID3D11DeviceContext* context,
              UINT eye_w, UINT eye_h,
              void* headset_hwnd,
              StereoDisplayComponent* component,
              MenuCallbacks callbacks);

    // Re-create the offscreen RT if the per-eye dimensions changed (e.g.
    // user switched output_mode and the SbS frame size moved). Cheap no-op
    // when the dims match the current target.
    void OnResize(UINT eye_w, UINT eye_h);
#else
    // Vulkan/Linux variant. `sbs_format` is out_sbs_'s VkFormat (the OSD
    // renders straight into out_sbs_ via a LOAD render pass — no offscreen
    // RT + composite pass like the D3D11 path needs).
    bool Init(vrto3d::vk::DeviceCtx* ctx,
              VkFormat sbs_format,
              uint32_t eye_w, uint32_t eye_h,
              void* native_window,
              StereoDisplayComponent* component,
              MenuCallbacks callbacks);

    void OnResize(uint32_t eye_w, uint32_t eye_h);
#endif

    void Shutdown();

    // Update the headset_hwnd used for mouse coordinate mapping. Used by
    // presenters whose display surface is owned by an external process and
    // only becomes available some time after Init() ran.
    void SetHeadsetHwnd(void* hwnd);

    // Returns true if the menu OR a live toast is currently visible.
    // Dx11Renderer can use this to skip the composite pass when there's
    // nothing to draw.
    bool HasContent() const;

    // True when the menu is open. Used by hmd_device_driver to gate the
    // hotkey poll loop (so number keys / arrows reach ImGui instead of
    // adjusting depth/conv).
    bool MenuVisible() const;
    void ToggleMenu();

    // Replaces the old GDI+ setOverlay lambda. `ttl` defaults to 3 seconds.
    void SetText(const std::string& msg,
                 std::chrono::milliseconds ttl = std::chrono::milliseconds(3000));

    // Per-frame entry point. Called by Dx11Renderer right after the SbS
    // CopyResource and before handing the frame to the presenter.
    //
    // Performs:
    //   1. input poll + ImGui::NewFrame
    //   2. menu BuildUI()
    //   3. toast text widget
    //   4. ImGui::Render → render draw lists into offscreen osd_tex_
    //   5. Two-pass blend onto out_sbs (left half + right half)
#ifdef _WIN32
    void RenderFrame(ID3D11Texture2D* out_sbs);
#else
    // Vulkan variant: records the ImGui draw data twice (left/right eye
    // viewports) into out_sbs via a loadOp=LOAD render pass, inside the
    // caller's command buffer. `out_sbs` must be in COLOR_ATTACHMENT_OPTIMAL.
    void RenderFrame(VkCommandBuffer cmd, VkImage out_sbs, VkImageView out_sbs_view,
                     uint32_t sbs_w, uint32_t sbs_h);
#endif

    // Plumbing for the menu's app-name display + version strings.
    void SetAppName(const std::string& app_name);
    void SetVersion(const std::string& version);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vrto3d::osd
