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

namespace vrto3d {

bool TriggerOpenVRRecenter()
{
    vr::EVRInitError err = vr::VRInitError_None;
    vr::IVRSystem* sys = vr::VR_Init(&err, vr::VRApplication_Background);
    if (err != vr::VRInitError_None || sys == nullptr) {
        return false;
    }

    bool ok = false;
    if (vr::IVRChaperone* chap = vr::VRChaperone()) {
        chap->ResetZeroPose(vr::TrackingUniverseSeated);
        chap->ResetZeroPose(vr::TrackingUniverseStanding);
        ok = true;
    }

    vr::VR_Shutdown();
    return ok;
}

} // namespace vrto3d
