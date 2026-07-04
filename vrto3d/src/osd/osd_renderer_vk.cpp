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

// Linux/Vulkan implementation of OsdRenderer. Mirrors the D3D11 version's
// structure (osd_renderer.cpp) with one architectural difference: the D3D11
// path renders ImGui into an offscreen per-eye texture and then runs a
// composite blend pass twice (left/right viewport); here the OSD renders
// straight into out_sbs via a loadOp=LOAD render pass, and the left/right
// duplication is achieved by recording ImGui's draw data twice with a
// temporarily adjusted ImDrawData::DisplayPos/DisplaySize — the vendored
// imgui_impl_vulkan backend derives both its viewport and its projection
// push-constants from those fields, so offsetting DisplayPos by -half_w
// lands the identical draw list in the right half (ImGui's own blend state
// already handles the SrcAlpha/InvSrcAlpha pass-through).

#include "osd/osd_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include "osd/osd_input.h"
#include "osd/osd_menu.h"

#include "vk/vk_context.h"
#include "vrto3dlib/debug_log.hpp"  // LOG() + GetSteamInstallPath()

using namespace std::chrono;

namespace vrto3d::osd {

namespace {

// Geometric-mean font scale based on the per-eye OSD area. Same formula as
// the D3D11 implementation so text reads identically across platforms.
// Baseline area = 720p SbS half (640x720 = 460800 px).
float ComputeFontScale(uint32_t eye_w, uint32_t eye_h) {
    constexpr float baseline_area = 640.0f * 720.0f;
    const float area = static_cast<float>(eye_w) * static_cast<float>(eye_h);
    if (area <= 0.0f) return 1.0f;
    return std::clamp(std::sqrt(area / baseline_area), 0.75f, 3.0f);
}

// Routed into the imgui vulkan backend via InitInfo::CheckVkResultFn.
void CheckVkResult(VkResult err) {
    if (err != VK_SUCCESS) {
        LOG() << "OsdRenderer(vk): imgui backend VkResult " << static_cast<int>(err);
    }
}

} // namespace

struct OsdRenderer::Impl {
    // Vulkan handles (non-owning ctx — owned by VkRenderer).
    vrto3d::vk::DeviceCtx* ctx = nullptr;
    VkFormat               sbs_format = VK_FORMAT_UNDEFINED;

    // Per-eye dimensions (== half of the SbS frame width).
    uint32_t               eye_w = 0;
    uint32_t               eye_h = 0;

    // loadOp=LOAD pass rendering straight into out_sbs.
    VkRenderPass           osd_pass  = VK_NULL_HANDLE;
    VkDescriptorPool       desc_pool = VK_NULL_HANDLE;

    // Single-entry framebuffer cache keyed by (view, w, h). out_sbs is
    // recreated on resize, so the cached framebuffer is rebuilt whenever the
    // caller hands us a different view or extent (the caller idles the
    // device around out_sbs recreation, so destroying the old framebuffer
    // here is safe).
    VkFramebuffer          fb      = VK_NULL_HANDLE;
    VkImageView            fb_view = VK_NULL_HANDLE;
    uint32_t               fb_w = 0;
    uint32_t               fb_h = 0;

    // ImGui state.
    ImGuiContext*          imgui_ctx = nullptr;
    bool                   imgui_vk_ready = false;
    // Backing storage for io.IniFilename — ImGui only holds the const char*,
    // so the std::string must outlive the ImGui context.
    std::string            ini_path;

    // Input pump + menu (created in Init).
    std::unique_ptr<OsdInput> input;
    std::unique_ptr<OsdMenu>  menu;

    // Toast text (replacement for old GDI+ overlay).
    std::mutex               toast_mu;
    std::string              toast_msg;
    steady_clock::time_point toast_expiry;

    // Frame timing for ImGui's IO.DeltaTime.
    steady_clock::time_point last_frame_time = steady_clock::now();

    // Kept for signature parity with the Windows path; the Linux input pump
    // is virtual-cursor based and never dereferences it.
    void*                    native_window = nullptr;
    StereoDisplayComponent*  component = nullptr;

    // Menu open/close edge tracking. Linux has no WS_EX_TRANSPARENT click-
    // through toggling (evdev input is global), but the close edge still
    // hands focus back to the game like the Windows version does.
    bool                     prev_menu_visible = false;
    std::function<void()>    on_menu_closed;

    void ApplyMenuVisibility(bool now_visible) {
        if (now_visible == prev_menu_visible) return;
        if (!now_visible && on_menu_closed) on_menu_closed();
        prev_menu_visible = now_visible;
    }

    bool CreateRenderPass() {
        VkAttachmentDescription att{};
        att.format         = sbs_format;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference color_ref{};
        color_ref.attachment = 0;
        color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &color_ref;

        // in: order the loadOp against the caller's preceding SbS writes;
        // out: order our overlay writes against the repack/present sampling.
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                VK_ACCESS_TRANSFER_READ_BIT;

        VkRenderPassCreateInfo ci{};
        ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments    = &att;
        ci.subpassCount    = 1;
        ci.pSubpasses      = &sub;
        ci.dependencyCount = 2;
        ci.pDependencies   = deps;
        return vrto3d::vk::LogIfFailed(
                   vkCreateRenderPass(ctx->device, &ci, nullptr, &osd_pass),
                   "OsdRenderer(vk): vkCreateRenderPass") == VK_SUCCESS;
    }

    bool CreateDescriptorPool() {
        // The backend needs one combined-image-sampler set for the font
        // atlas (see IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE);
        // leave headroom for a few AddTexture calls.
        VkDescriptorPoolSize pool_size{};
        pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount = 8;

        VkDescriptorPoolCreateInfo ci{};
        ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        ci.maxSets       = 8;
        ci.poolSizeCount = 1;
        ci.pPoolSizes    = &pool_size;
        return vrto3d::vk::LogIfFailed(
                   vkCreateDescriptorPool(ctx->device, &ci, nullptr, &desc_pool),
                   "OsdRenderer(vk): vkCreateDescriptorPool") == VK_SUCCESS;
    }

    VkFramebuffer GetFramebuffer(VkImageView view, uint32_t w, uint32_t h) {
        if (fb != VK_NULL_HANDLE && fb_view == view && fb_w == w && fb_h == h) {
            return fb;
        }
        DestroyFramebuffer();

        VkFramebufferCreateInfo ci{};
        ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass      = osd_pass;
        ci.attachmentCount = 1;
        ci.pAttachments    = &view;
        ci.width           = w;
        ci.height          = h;
        ci.layers          = 1;
        if (vrto3d::vk::LogIfFailed(
                vkCreateFramebuffer(ctx->device, &ci, nullptr, &fb),
                "OsdRenderer(vk): vkCreateFramebuffer") != VK_SUCCESS) {
            fb = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        fb_view = view;
        fb_w    = w;
        fb_h    = h;
        return fb;
    }

    void DestroyFramebuffer() {
        if (fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(ctx->device, fb, nullptr);
            fb = VK_NULL_HANDLE;
        }
        fb_view = VK_NULL_HANDLE;
        fb_w = fb_h = 0;
    }
};

OsdRenderer::OsdRenderer()  : impl_(std::make_unique<Impl>()) {}
OsdRenderer::~OsdRenderer() { Shutdown(); }

bool OsdRenderer::Init(vrto3d::vk::DeviceCtx* ctx,
                       VkFormat sbs_format,
                       uint32_t eye_w, uint32_t eye_h,
                       void* native_window,
                       StereoDisplayComponent* component,
                       MenuCallbacks callbacks) {
    auto& s = *impl_;
    if (!ctx || ctx->device == VK_NULL_HANDLE) return false;
    s.ctx           = ctx;
    s.sbs_format    = sbs_format;
    s.eye_w         = eye_w;
    s.eye_h         = eye_h;
    s.native_window = native_window;
    s.component     = component;

    if (!s.CreateRenderPass()) {
        LOG() << "OsdRenderer(vk): failed to create OSD render pass";
        return false;
    }
    if (!s.CreateDescriptorPool()) {
        LOG() << "OsdRenderer(vk): failed to create descriptor pool";
        Shutdown();
        return false;
    }

    IMGUI_CHECKVERSION();
    s.imgui_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(s.imgui_ctx);
    ImGuiIO& io = ImGui::GetIO();
    // Persist window pos/size, table column widths, etc. to the VRto3D
    // config folder so the user's layout survives across sessions. The ini
    // path string is held on the Impl so the pointer stays valid for the
    // lifetime of the ImGui context. If Steam can't be located (CI / debug
    // shells) we fall through with IniFilename = nullptr — ImGui then runs
    // ephemerally instead of writing to an unknown location.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    if (const std::string steam = GetSteamInstallPath(); !steam.empty()) {
        const std::string folder = steam + "/config/vrto3d";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        s.ini_path = folder + "/imgui_osd.ini";
        io.IniFilename = s.ini_path.c_str();
    }
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    // Scale fonts to the per-eye area so SbS and TaB read the same at the
    // same source resolution; the formula clamps below 1.0 for very small
    // surfaces and above 1.0 for high-res so the menu stays legible.
    io.FontGlobalScale = ComputeFontScale(eye_w, eye_h);
    io.DisplaySize = ImVec2(static_cast<float>(eye_w), static_cast<float>(eye_h));

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion      = VK_API_VERSION_1_1;
    info.Instance        = ctx->instance;
    info.PhysicalDevice  = ctx->phys;
    info.Device          = ctx->device;
    info.QueueFamily     = ctx->queue_family;
    info.Queue           = ctx->queue;
    info.DescriptorPool  = s.desc_pool;
    info.RenderPass      = s.osd_pass;
    info.MinImageCount   = 2;
    // RenderFrame calls ImGui_ImplVulkan_RenderDrawData twice per frame
    // (left + right eye), and each call advances the backend's internal
    // vertex/index ring by one slot. VkRenderer keeps 2 frames in flight, so
    // 2 calls x 2 frames = 4 slots are needed before a buffer can be safely
    // reused.
    info.ImageCount      = 4;
    info.MSAASamples     = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = &CheckVkResult;
    if (!ImGui_ImplVulkan_Init(&info)) {
        LOG() << "OsdRenderer(vk): ImGui_ImplVulkan_Init failed";
        Shutdown();
        return false;
    }
    s.imgui_vk_ready = true;

    // Upload the font atlas now rather than letting ImGui_ImplVulkan_NewFrame
    // do it lazily mid-frame: this vendored backend's CreateFontsTexture
    // allocates its own one-shot command buffer and vkQueueSubmit+WaitIdle's
    // it internally, so the shared-queue mutex must be held around the call.
    {
        std::lock_guard<std::mutex> qlk(ctx->queue_mutex);
        if (!ImGui_ImplVulkan_CreateFontsTexture()) {
            LOG() << "OsdRenderer(vk): ImGui_ImplVulkan_CreateFontsTexture failed";
            Shutdown();
            return false;
        }
    }

    s.input = std::make_unique<OsdInput>();
    s.on_menu_closed = callbacks.request_game_focus;
    s.menu  = std::make_unique<OsdMenu>(component, std::move(callbacks));

    return true;
}

void OsdRenderer::OnResize(uint32_t eye_w, uint32_t eye_h) {
    auto& s = *impl_;
    if (!s.ctx) return;
    if (s.eye_w == eye_w && s.eye_h == eye_h) return;
    s.eye_w = eye_w;
    s.eye_h = eye_h;
    // No offscreen RT to rebuild here — the framebuffer cache keys on the
    // (view, w, h) the caller passes to RenderFrame and refreshes lazily.
    if (s.imgui_ctx) {
        ImGui::SetCurrentContext(s.imgui_ctx);
        ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(eye_w), static_cast<float>(eye_h));
        ImGui::GetIO().FontGlobalScale = ComputeFontScale(eye_w, eye_h);
    }
}

void OsdRenderer::SetHeadsetHwnd(void* hwnd) {
    // Kept for cross-platform call-site parity; the Linux mouse pump is
    // virtual-cursor based and never consults a native window handle.
    impl_->native_window = hwnd;
}

void OsdRenderer::Shutdown() {
    auto& s = *impl_;
    s.ApplyMenuVisibility(false);  // fire the menu-closed callback if open
    s.menu.reset();
    s.input.reset();
    if (s.ctx && s.ctx->device != VK_NULL_HANDLE) {
        // The caller doesn't guarantee a vkDeviceWaitIdle before tearing us
        // down — drain our queue (under the shared mutex) so no in-flight
        // command buffer still references the backend's buffers/framebuffer.
        if (s.ctx->queue != VK_NULL_HANDLE) {
            std::lock_guard<std::mutex> qlk(s.ctx->queue_mutex);
            vkQueueWaitIdle(s.ctx->queue);
        }
        if (s.imgui_vk_ready) {
            ImGui::SetCurrentContext(s.imgui_ctx);
            ImGui_ImplVulkan_Shutdown();
            s.imgui_vk_ready = false;
        }
        s.DestroyFramebuffer();
        if (s.desc_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(s.ctx->device, s.desc_pool, nullptr);
            s.desc_pool = VK_NULL_HANDLE;
        }
        if (s.osd_pass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(s.ctx->device, s.osd_pass, nullptr);
            s.osd_pass = VK_NULL_HANDLE;
        }
    }
    if (s.imgui_ctx) {
        ImGui::SetCurrentContext(s.imgui_ctx);
        // Force-flush any pending layout changes — ImGui's auto-save is
        // throttled by IniSavingRate (5s by default) so a quick edit-then-
        // exit could otherwise lose the user's window pos/size.
        if (!s.ini_path.empty()) {
            ImGui::SaveIniSettingsToDisk(s.ini_path.c_str());
        }
        ImGui::DestroyContext(s.imgui_ctx);
        s.imgui_ctx = nullptr;
    }
    s.ctx = nullptr;
}

bool OsdRenderer::HasContent() const {
    auto& s = *impl_;
    if (s.menu && s.menu->Visible()) return true;
    std::lock_guard<std::mutex> lk(s.toast_mu);
    return !s.toast_msg.empty() && steady_clock::now() < s.toast_expiry;
}

bool OsdRenderer::MenuVisible() const {
    auto& s = *impl_;
    return s.menu && s.menu->Visible();
}

void OsdRenderer::ToggleMenu() {
    auto& s = *impl_;
    if (s.menu) s.menu->Toggle();
}

void OsdRenderer::SetText(const std::string& msg, milliseconds ttl) {
    auto& s = *impl_;
    std::lock_guard<std::mutex> lk(s.toast_mu);
    s.toast_msg = msg;
    s.toast_expiry = steady_clock::now() + ttl;
}

void OsdRenderer::SetAppName(const std::string& app_name) {
    auto& s = *impl_;
    if (s.menu) s.menu->SetAppName(app_name);
}

void OsdRenderer::SetVersion(const std::string& version) {
    auto& s = *impl_;
    if (s.menu) s.menu->SetVersion(version);
}

void OsdRenderer::RenderFrame(VkCommandBuffer cmd, VkImage out_sbs, VkImageView out_sbs_view,
                              uint32_t sbs_w, uint32_t sbs_h) {
    auto& s = *impl_;
    if (!s.imgui_ctx || !s.imgui_vk_ready || cmd == VK_NULL_HANDLE ||
        out_sbs == VK_NULL_HANDLE || out_sbs_view == VK_NULL_HANDLE ||
        sbs_w == 0 || sbs_h == 0) {
        return;
    }

    // Fire the game-refocus callback on the menu's close edge (the Windows
    // version additionally toggles window styles here; evdev input is global
    // so there is no click-through state to manage on Linux).
    s.ApplyMenuVisibility(MenuVisible());

    // API parity with the Windows LL-hook gating; a no-op for evdev.
    if (s.input) {
        s.input->SetMouseHookActive(MenuVisible() || s.input->IsCapturing());
    }

    // Cheap early-out if there's nothing to draw and no toast pending —
    // returns without touching the command buffer.
    const bool has_content = HasContent();
    if (!has_content && (!s.input || !s.input->IsCapturing())) {
        // Still pump input edges so a future Ctrl+Home press is detected.
        if (s.input) s.input->Poll();
        return;
    }

    // ----- Set up ImGui frame -----
    ImGui::SetCurrentContext(s.imgui_ctx);
    ImGuiIO& io = ImGui::GetIO();

    auto now = steady_clock::now();
    float dt = duration_cast<duration<float>>(now - s.last_frame_time).count();
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;
    io.DeltaTime = dt;
    s.last_frame_time = now;

    if (s.input) {
        s.input->Poll();
        OsdSurface surface;
        surface.eye_w = static_cast<int>(s.eye_w);
        surface.eye_h = static_cast<int>(s.eye_h);
        surface.hwnd  = s.native_window;
        // The evdev virtual cursor already lives in per-eye space, so no
        // SbS/TaB cursor folding is needed (unlike the Win32 window-rect
        // mapping) — the layout field is ignored by the Linux OsdInput.
        surface.layout = StereoLayout::Mono;
        s.input->FeedImGui(io, surface);
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();

    // Menu UI.
    if (s.menu && s.menu->Visible() && s.input) {
        s.menu->BuildUI(*s.input);
    }

    // Toast widget — bottom-left, no decoration, bright green to match the
    // old GDI+ overlay's look.
    {
        std::string msg;
        bool active = false;
        {
            std::lock_guard<std::mutex> lk(s.toast_mu);
            if (!s.toast_msg.empty() && steady_clock::now() < s.toast_expiry) {
                msg = s.toast_msg;
                active = true;
            }
        }
        if (active) {
            const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                           ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoNav |
                                           ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_NoBackground;
            const float pad_x = 50.0f;
            const float pad_y = 30.0f;
            // Toast text is 2x the menu font so it's legible at headset
            // distance even when the menu chrome is hidden.
            const float toast_scale = 2.0f;
            // Bound text wrap to the eye width so long messages wrap instead
            // of running past the right edge. Pass the wrap position as an
            // explicit window-local pixel value rather than 0.0f — with
            // AlwaysAutoResize, a wrap-at-content-right behaves circularly
            // (window width depends on content, content wrap depends on
            // window width) and collapses to one glyph per line on first
            // frame, producing a vertical column of characters.
            const float wrap_local = (std::max)(64.0f,
                static_cast<float>(s.eye_w) - 2.0f * pad_x -
                2.0f * ImGui::GetStyle().WindowPadding.x);
            // Anchor by bottom-left pivot so wrapped multi-line toasts grow
            // upward into the screen rather than falling off the bottom.
            ImGui::SetNextWindowPos(
                ImVec2(pad_x, static_cast<float>(s.eye_h) - pad_y),
                ImGuiCond_Always,
                ImVec2(0.0f, 1.0f));
            if (ImGui::Begin("##vrto3d_toast", nullptr, flags)) {
                ImGui::SetWindowFontScale(toast_scale);
                ImGui::PushTextWrapPos(wrap_local);
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));
                ImGui::TextUnformatted(msg.c_str());
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
            }
            ImGui::End();
        }
    }

    ImGui::Render();

    ImDrawData* draw_data = ImGui::GetDrawData();
    if (!draw_data || draw_data->CmdListsCount == 0) {
        return;  // capture-only frame with nothing visible — skip the pass
    }

    VkFramebuffer fb = s.GetFramebuffer(out_sbs_view, sbs_w, sbs_h);
    if (fb == VK_NULL_HANDLE) return;

    // ----- Record the draw data into both halves of out_sbs -----
    // The caller guarantees out_sbs is in COLOR_ATTACHMENT_OPTIMAL; the pass
    // LOADs the existing SbS pixels and ImGui's pipeline alpha-blends the
    // overlay on top (mirroring the D3D11 composite blend state).
    VkRenderPassBeginInfo rp{};
    rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass        = s.osd_pass;
    rp.framebuffer       = fb;
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {sbs_w, sbs_h};
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    // The backend sets its own viewport/scissor and projection from the draw
    // data's DisplayPos/DisplaySize, so per-half placement is done by
    // temporarily rewriting those fields (the Vulkan analogue of the D3D11
    // impl's two composite viewports):
    //   - DisplaySize = full SbS extent -> the backend's viewport covers the
    //     whole image and 1 ImGui unit == 1 pixel,
    //   - DisplayPos 0 puts the eye-sized content in the left half,
    //   - DisplayPos.x = -half_w shifts the identical content (and the
    //     backend's scissor clamping, which subtracts DisplayPos from every
    //     clip rect) into the right half.
    // Clip rects are bounded by io.DisplaySize (the per-eye size used during
    // NewFrame), so neither pass bleeds across the seam.
    const float half_w    = static_cast<float>(sbs_w / 2);
    const ImVec2 orig_pos  = draw_data->DisplayPos;
    const ImVec2 orig_size = draw_data->DisplaySize;
    draw_data->DisplaySize = ImVec2(static_cast<float>(sbs_w), static_cast<float>(sbs_h));

    // Left eye.
    draw_data->DisplayPos = ImVec2(0.0f, 0.0f);
    ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);

    // Right eye.
    draw_data->DisplayPos = ImVec2(-half_w, 0.0f);
    ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);

    draw_data->DisplayPos  = orig_pos;
    draw_data->DisplaySize = orig_size;

    vkCmdEndRenderPass(cmd);
}

} // namespace vrto3d::osd

#endif  // !_WIN32
