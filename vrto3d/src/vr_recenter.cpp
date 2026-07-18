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

// This TU intentionally only includes openvr.h (the client API).
// Do NOT include openvr_driver.h here — the two headers redeclare
// types in the vr:: namespace and will not coexist in one TU.
#include "openvr.h"

#include "vr_recenter.h"

#include "vrto3dlib/debug_log.hpp"

#include <atomic>
#include <mutex>

namespace vrto3d {

// VR_Init / VR_Shutdown is effectively one-shot when called from inside the
// vrserver process. Once VR_Shutdown runs, the global "client version"
// state stays set and every subsequent VR_Init returns
// VRInitError_Init_ClientVersionAlreadyProvided (169). The wedge is
// permanent for the lifetime of vrserver.
//
// So we init a single VRApplication_Background session on first use, cache
// the chaperone pointer, and never call VR_Shutdown. Subsequent recenters
// just reuse the cached interface. The session is benign — Background apps
// are not visible to the user and don't count as scene apps.
namespace {
    enum class State {
        NotInitialized,  // VR_Init has not been attempted
        Initialized,     // VR_Init succeeded, chaperone may or may not be cached
        InitFailed,      // VR_Init failed once — never retry (err-169 ratchet)
        ShutDown,        // ShutdownOpenVRClient ran; no further calls allowed
    };

    std::mutex             g_init_mutex;
    State                  g_state     = State::NotInitialized;
    vr::IVRChaperone*      g_chaperone = nullptr;
    vr::IVRSystem*         g_vrsystem  = nullptr;  // cached so PumpOpenVRClientEvents can poll

    // Returns a usable chaperone pointer or nullptr. Tries VR_Init only
    // once per process (any retry would hit err-169); re-resolves the
    // chaperone interface every call until we get one, since it may not
    // be available immediately after VR_Init returns.
    vr::IVRChaperone* GetChaperone(const char* t)
    {
        std::lock_guard<std::mutex> lock(g_init_mutex);

        if (g_state == State::InitFailed || g_state == State::ShutDown) {
            return nullptr;
        }

        if (g_state == State::NotInitialized) {
            vr::EVRInitError err = vr::VRInitError_None;
            vr::IVRSystem* sys = vr::VR_Init(&err, vr::VRApplication_Background);
            if (err != vr::VRInitError_None || sys == nullptr) {
                LOG() << "TriggerOpenVRRecenter[" << t << "]: VR_Init failed err="
                      << static_cast<int>(err) << " ("
                      << vr::VR_GetVRInitErrorAsSymbol(err) << ")";
                g_state = State::InitFailed;
                return nullptr;
            }
            g_state = State::Initialized;
            g_vrsystem = sys;
            LOG() << "TriggerOpenVRRecenter[" << t << "]: client session initialized";
        }

        if (!g_chaperone) {
            g_chaperone = vr::VRChaperone();
            if (!g_chaperone) {
                LOG() << "TriggerOpenVRRecenter[" << t << "]: VRChaperone() still null";
            }
        }
        return g_chaperone;
    }
}

bool TriggerOpenVRRecenter(const char* tag)
{
    const char* t = tag ? tag : "recenter";

    vr::IVRChaperone* chap = GetChaperone(t);
    if (!chap) return false;

    chap->ResetZeroPose(vr::TrackingUniverseSeated);
    chap->ResetZeroPose(vr::TrackingUniverseStanding);
    LOG() << "TriggerOpenVRRecenter[" << t << "]: ResetZeroPose issued";
    return true;
}

void ShutdownOpenVRClient()
{
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_state == State::Initialized) {
        g_chaperone = nullptr;
        g_vrsystem = nullptr;
        vr::VR_Shutdown();
        LOG() << "ShutdownOpenVRClient: VR_Shutdown issued";
    }
    // Always flip to ShutDown — even from InitFailed/NotInitialized — so
    // any late TriggerOpenVRRecenter call (e.g. from a detached auto_focus
    // thread that slept past the quit event) becomes a clean no-op rather
    // than re-attempting VR_Init while vrserver is mid-teardown.
    g_state = State::ShutDown;
}

void PumpOpenVRClientEvents()
{
    // Snapshot the system pointer under the lock so we don't hold the
    // mutex across PollNextEvent (cheap call but no reason to serialize
    // unrelated TriggerOpenVRRecenter calls behind it).
    vr::IVRSystem* sys = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_init_mutex);
        if (g_state != State::Initialized) return;
        sys = g_vrsystem;
    }
    if (!sys) return;

    vr::VREvent_t ev{};
    bool saw_quit = false;
    while (sys->PollNextEvent(&ev, sizeof(ev))) {
        if (ev.eventType == vr::VREvent_Quit) {
            saw_quit = true;
            // Ack first so vrserver stops waiting on us, then break out
            // and let ShutdownOpenVRClient run outside the loop — calling
            // it while still polling would invalidate `sys`.
            sys->AcknowledgeQuit_Exiting();
            LOG() << "PumpOpenVRClientEvents: VREvent_Quit ack'd";
            break;
        }
    }
    if (saw_quit) {
        ShutdownOpenVRClient();
    }
}

} // namespace vrto3d
