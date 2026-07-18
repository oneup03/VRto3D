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
#include "presenter/vk_presenter.h"

#include <cstdlib>
#include <cstring>

#include "presenter/vk_swapchain_util.h"
#include "presenter/wayland_presenter.h"
#include "presenter/wibblewobble_presenter_linux.h"
#include "presenter/x11_presenter.h"

namespace vrto3d {

namespace {

bool EnvSet(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0';
}

}  // namespace

// Session-based selection. VRTO3D_PRESENTER=x11|wayland|wibblewobble forces a
// backend; otherwise output_mode==WibbleWobble routes to the WibbleWobbleLinux
// handoff, and the rest pick by session env (WAYLAND_DISPLAY -> Wayland, else
// DISPLAY -> X11).
std::unique_ptr<IVkPresenter> MakeVkPresenter(const StereoDisplayDriverConfiguration& cfg)
{
    const char* forced = std::getenv("VRTO3D_PRESENTER");
    if ((forced && std::strcmp(forced, "wibblewobble") == 0) ||
        cfg.output_mode == OutputMode::WibbleWobble) {
        PresenterLog("MakeVkPresenter: WibbleWobble output — streaming to wwserver");
        return std::make_unique<WibbleWobblePresenter>();
    }
    if (forced && std::strcmp(forced, "x11") == 0) {
        PresenterLog("MakeVkPresenter: VRTO3D_PRESENTER=x11");
        return std::make_unique<X11Presenter>();
    }
    if (forced && std::strcmp(forced, "wayland") == 0) {
        PresenterLog("MakeVkPresenter: VRTO3D_PRESENTER=wayland");
        return std::make_unique<WaylandPresenter>();
    }
    if (forced && forced[0] != '\0') {
        PresenterLog("MakeVkPresenter: unknown VRTO3D_PRESENTER='%s' — using auto-detect", forced);
    }

    if (EnvSet("WAYLAND_DISPLAY")) {
        PresenterLog("MakeVkPresenter: WAYLAND_DISPLAY set — Wayland presenter");
        return std::make_unique<WaylandPresenter>();
    }
    if (EnvSet("DISPLAY")) {
        PresenterLog("MakeVkPresenter: DISPLAY set — X11 presenter");
        return std::make_unique<X11Presenter>();
    }

    PresenterLog("MakeVkPresenter: neither WAYLAND_DISPLAY nor DISPLAY set — no presenter");
    return nullptr;
}

}  // namespace vrto3d
