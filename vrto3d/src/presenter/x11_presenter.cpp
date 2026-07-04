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
#include "presenter/x11_presenter.h"
#include "presenter/x11_modeline.h"

#include <cstring>
#include <mutex>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xlib.h>

namespace vrto3d {

namespace {

// EWMH _NET_WM_STATE client-message actions.
constexpr long kNetWmStateRemove = 0;
constexpr long kNetWmStateAdd    = 1;

// Motif window-manager hints — decorations off.
struct MotifWmHints {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long          input_mode;
    unsigned long status;
};
constexpr unsigned long kMwmHintsDecorations = 1ul << 1;

std::once_flag g_xinit_once;

}  // namespace


X11Presenter::~X11Presenter()
{
    Shutdown();
}


std::vector<X11Presenter::OutputGeom> X11Presenter::EnumerateOutputs() const
{
    std::vector<OutputGeom> out;
    if (!dpy_) return out;

    Display* dpy = dpy_;
    Window root = DefaultRootWindow(dpy);

    XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) return out;

    RROutput primary = XRRGetOutputPrimary(dpy, root);

    for (int i = 0; i < res->noutput; ++i) {
        XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (!oi) continue;
        if (oi->connection == RR_Connected && oi->crtc != 0) {
            XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
            if (ci && ci->width > 0 && ci->height > 0) {
                OutputGeom g;
                g.x = ci->x;
                g.y = ci->y;
                g.width  = ci->width;
                g.height = ci->height;
                g.name = oi->name ? std::string(oi->name, static_cast<size_t>(oi->nameLen))
                                  : std::string();
                g.is_primary = (res->outputs[i] == primary);
                g.xinerama_index = static_cast<int32_t>(out.size());

                // Refresh from the CRTC's mode line: dotClock / (hTotal * vTotal).
                for (int m = 0; m < res->nmode; ++m) {
                    const XRRModeInfo& mi = res->modes[m];
                    if (mi.id == ci->mode && mi.hTotal > 0 && mi.vTotal > 0) {
                        g.refresh_hz = static_cast<float>(
                            static_cast<double>(mi.dotClock)
                            / (static_cast<double>(mi.hTotal) * static_cast<double>(mi.vTotal)));
                        break;
                    }
                }
                out.push_back(g);
            }
            if (ci) XRRFreeCrtcInfo(ci);
        }
        XRRFreeOutputInfo(oi);
    }
    XRRFreeScreenResources(res);
    return out;
}


void X11Presenter::SendNetWmState(long action, unsigned long property, unsigned long property2) const
{
    if (!dpy_ || !window_) return;
    Display* dpy = dpy_;

    XEvent ev{};
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = static_cast<Window>(window_);
    ev.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = action;
    ev.xclient.data.l[1]    = static_cast<long>(property);
    ev.xclient.data.l[2]    = static_cast<long>(property2);
    ev.xclient.data.l[3]    = 1;   // source: normal application

    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}


void X11Presenter::SendFullscreenMonitors(const OutputGeom& left, const OutputGeom& right) const
{
    if (!dpy_ || !window_) return;
    Display* dpy = dpy_;

    // _NET_WM_FULLSCREEN_MONITORS takes Xinerama monitor indices; the
    // connected-output enumeration order matches them on typical XRandR 1.5
    // servers (best effort — WMs ignore unknown indices).
    XEvent ev{};
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = static_cast<Window>(window_);
    ev.xclient.message_type = XInternAtom(dpy, "_NET_WM_FULLSCREEN_MONITORS", False);
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = left.xinerama_index;    // top
    ev.xclient.data.l[1]    = left.xinerama_index;    // bottom
    ev.xclient.data.l[2]    = left.xinerama_index;    // left
    ev.xclient.data.l[3]    = right.xinerama_index;   // right
    ev.xclient.data.l[4]    = 1;   // source: normal application

    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}


bool X11Presenter::Init(vrto3d::vk::DeviceCtx* ctx, const StereoDisplayDriverConfiguration& cfg)
{
    ctx_ = ctx;

    // Xlib is used from the present thread while other threads may create
    // their own connections — must be the first Xlib call in the process.
    std::call_once(g_xinit_once, []() { XInitThreads(); });

    dpy_ = XOpenDisplay(nullptr);
    if (!dpy_) {
        PresenterLog("X11Presenter: XOpenDisplay failed (DISPLAY=%s)",
                     getenv("DISPLAY") ? getenv("DISPLAY") : "<unset>");
        return false;
    }
    Display* dpy = dpy_;

    std::vector<OutputGeom> outputs = EnumerateOutputs();
    if (outputs.empty()) {
        PresenterLog("X11Presenter: no connected XRandR outputs");
        Shutdown();
        return false;
    }

    // 0 = primary; 1..N = connected-output enumeration order.
    const OutputGeom* chosen = nullptr;
    if (cfg.display_index > 0 && static_cast<size_t>(cfg.display_index) <= outputs.size()) {
        chosen = &outputs[static_cast<size_t>(cfg.display_index - 1)];
    } else {
        for (const auto& o : outputs) {
            if (o.is_primary) { chosen = &o; break; }
        }
        if (!chosen) chosen = &outputs.front();
    }

    // DualDisplay modes span the chosen output plus its contiguous right
    // neighbor with identical geometry (mirrors the Windows presenter).
    const bool spans_two = (cfg.output_mode == OutputMode::DualDisplay
                            || cfg.output_mode == OutputMode::DualDisplayFlip);
    const OutputGeom* secondary = nullptr;
    if (spans_two) {
        const int32_t right_x = chosen->x + static_cast<int32_t>(chosen->width);
        for (const auto& o : outputs) {
            if (&o == chosen) continue;
            if (o.y == chosen->y && o.x == right_x
                && o.width == chosen->width && o.height == chosen->height) {
                secondary = &o;
                break;
            }
        }
        if (!secondary) {
            PresenterLog("X11Presenter: DualDisplay requested but no contiguous right "
                         "neighbor with matching geometry — single-monitor window");
        }
    }

    width_  = chosen->width + (secondary ? secondary->width : 0);
    height_ = chosen->height;

    // Frame-packed output: switch the target output to the HDMI 1.4 custom
    // timing at runtime (the Linux analog of the NVAPI/CRU path). Restored in
    // Shutdown. Only possible in a real X session — under XWayland the mode
    // set fails and we fall back to the current mode (EDID override route
    // documented in README-linux.md).
    if (const FramePackTimingSpec* spec = GetFramePackTimingSpec(cfg.output_mode)) {
        if (ApplyFramePackedModeX11(dpy, cfg.display_index, *spec, modeline_state_)) {
            width_ = spec->active_w;
            height_ = spec->active_h;
        }
    }

    PresenterLog("X11Presenter: output '%s' %ux%u+%d+%d @%.2fHz%s (display_index=%d)",
                 chosen->name.c_str(), width_, height_, chosen->x, chosen->y,
                 chosen->refresh_hz, secondary ? " (spanning two outputs)" : "",
                 cfg.display_index);

    // --- Window creation --------------------------------------------------
    Window root = DefaultRootWindow(dpy);
    const int screen = DefaultScreen(dpy);

    XSetWindowAttributes attrs{};
    attrs.background_pixel = BlackPixel(dpy, screen);
    attrs.border_pixel     = BlackPixel(dpy, screen);
    attrs.event_mask       = StructureNotifyMask;
    attrs.override_redirect = False;

    Window win = XCreateWindow(dpy, root, chosen->x, chosen->y, width_, height_, 0,
                               CopyFromParent, InputOutput,
                               reinterpret_cast<Visual*>(CopyFromParent),
                               CWBackPixel | CWBorderPixel | CWEventMask | CWOverrideRedirect,
                               &attrs);
    if (!win) {
        PresenterLog("X11Presenter: XCreateWindow failed");
        Shutdown();
        return false;
    }
    window_ = static_cast<unsigned long>(win);

    // EWMH state BEFORE mapping — compliant WMs read the property at map time.
    Atom net_wm_state      = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom state_fullscreen  = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    Atom state_above       = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    Atom states[2] = { state_fullscreen, state_above };
    XChangeProperty(dpy, win, net_wm_state, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(states), 2);

    // No decorations (belt and braces alongside _NET_WM_STATE_FULLSCREEN).
    Atom motif = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    MotifWmHints hints{};
    hints.flags = kMwmHintsDecorations;
    hints.decorations = 0;
    XChangeProperty(dpy, win, motif, motif, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&hints), 5);

    // Respect our chosen position/size (some WMs re-place fullscreen windows
    // onto the "current" monitor otherwise).
    XSizeHints* size_hints = XAllocSizeHints();
    if (size_hints) {
        size_hints->flags = USPosition | USSize | PPosition | PSize;
        size_hints->x = chosen->x;
        size_hints->y = chosen->y;
        size_hints->width  = static_cast<int>(width_);
        size_hints->height = static_cast<int>(height_);
        XSetWMNormalHints(dpy, win, size_hints);
        XFree(size_hints);
    }

    XStoreName(dpy, win, "VRto3D");
    XClassHint class_hint;
    char wm_name[] = "vrto3d";
    char wm_class[] = "VRto3D";
    class_hint.res_name = wm_name;
    class_hint.res_class = wm_class;
    XSetClassHint(dpy, win, &class_hint);

    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    wm_delete_window_ = static_cast<unsigned long>(wm_delete);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    XMapRaised(dpy, win);

    // Move first, then (re-)assert fullscreen for running WMs: moving after
    // map beats WMs that snap fullscreen windows to the pointer's monitor.
    XMoveResizeWindow(dpy, win, chosen->x, chosen->y, width_, height_);
    SendNetWmState(kNetWmStateAdd, state_fullscreen, state_above);
    if (secondary) {
        SendFullscreenMonitors(*chosen, *secondary);
    }
    XRaiseWindow(dpy, win);
    XFlush(dpy);

    // --- Vulkan surface + swapchain ----------------------------------------
    VkXlibSurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
    sci.dpy    = dpy;
    sci.window = win;
    VkResult r = vkCreateXlibSurfaceKHR(ctx_->instance, &sci, nullptr, &vk_surface_);
    if (r != VK_SUCCESS) {
        PresenterLog("X11Presenter: vkCreateXlibSurfaceKHR failed (r=%d)", static_cast<int>(r));
        Shutdown();
        return false;
    }

    if (!swapchain_.Create(ctx_, vk_surface_, width_, height_)) {
        Shutdown();
        return false;
    }

    PresenterLog("X11Presenter: up, %ux%u", swapchain_.extent.width, swapchain_.extent.height);
    return true;
}


void X11Presenter::Shutdown()
{
    if (swapchain_.ctx) {
        swapchain_.Destroy();
    }
    if (vk_surface_ != VK_NULL_HANDLE && ctx_ && ctx_->instance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(ctx_->instance, vk_surface_, nullptr);
    }
    vk_surface_ = VK_NULL_HANDLE;

    if (dpy_) {
        RestoreModeX11(dpy_, modeline_state_);
        if (window_) {
            XDestroyWindow(dpy_, static_cast<Window>(window_));
            window_ = 0;
        }
        XCloseDisplay(dpy_);
        dpy_ = nullptr;
    }
    closed_ = false;
    ctx_ = nullptr;
}


bool X11Presenter::PumpEvents()
{
    if (!dpy_ || closed_) return false;
    Display* dpy = dpy_;

    while (XPending(dpy) > 0) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        switch (ev.type) {
            case ConfigureNotify: {
                const auto& c = ev.xconfigure;
                if (c.window == static_cast<Window>(window_)
                    && c.width > 0 && c.height > 0
                    && (static_cast<uint32_t>(c.width) != width_
                        || static_cast<uint32_t>(c.height) != height_)) {
                    width_  = static_cast<uint32_t>(c.width);
                    height_ = static_cast<uint32_t>(c.height);
                    swapchain_.SetDesiredExtent(width_, height_);
                }
                break;
            }
            case ClientMessage: {
                if (static_cast<unsigned long>(ev.xclient.data.l[0]) == wm_delete_window_) {
                    PresenterLog("X11Presenter: WM_DELETE_WINDOW — stopping");
                    closed_ = true;
                }
                break;
            }
            case DestroyNotify: {
                if (ev.xdestroywindow.window == static_cast<Window>(window_)) {
                    closed_ = true;
                }
                break;
            }
            default:
                break;
        }
    }
    return !closed_;
}


bool X11Presenter::AcquireNext(FrameTarget* out, VkSemaphore signal_sem)
{
    return swapchain_.AcquireNext(out, signal_sem);
}


bool X11Presenter::Present(uint32_t image_index, VkSemaphore wait_sem)
{
    return swapchain_.Present(image_index, wait_sem);
}


void X11Presenter::BringToTop()
{
    if (!dpy_ || !window_) return;
    SendNetWmState(kNetWmStateAdd,
                   XInternAtom(dpy_, "_NET_WM_STATE_ABOVE", False), 0);
    XRaiseWindow(dpy_, static_cast<Window>(window_));
    XFlush(dpy_);
}


void X11Presenter::ReleaseTopmost()
{
    if (!dpy_ || !window_) return;
    SendNetWmState(kNetWmStateRemove,
                   XInternAtom(dpy_, "_NET_WM_STATE_ABOVE", False), 0);
    XFlush(dpy_);
}

}  // namespace vrto3d
