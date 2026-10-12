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
#include <cstring>

#include "openvr_driver.h"

#include "ctrl_device_provider.h"
#include "vrto3d_ctrl_bridge.h"

#ifdef _WIN32
#define CTRL_DLL_EXPORT extern "C" __declspec(dllexport)
#else
#define CTRL_DLL_EXPORT extern "C" __attribute__((visibility("default")))
#endif

static vrto3d::ctrl::CtrlDeviceProvider g_provider;

CTRL_DLL_EXPORT void* HmdDriverFactory(const char* pInterfaceName, int* pReturnCode)
{
    if (0 == std::strcmp(vr::IServerTrackedDeviceProvider_Version, pInterfaceName))
        return &g_provider;
    if (pReturnCode) *pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}

// ---------------------------------------------------------------------------
// In-process bridge for VRto3D's OSD. Signatures must match the typedefs in
// vrto3d_ctrl_bridge.h.
// ---------------------------------------------------------------------------
CTRL_DLL_EXPORT int Vrto3dCtrl_GetBridgeVersion(void)
{
    return VRTO3D_CTRL_BRIDGE_VERSION;
}

CTRL_DLL_EXPORT uint32_t Vrto3dCtrl_GetStateJson(char* out, uint32_t cap)
{
    return g_provider.GetStateJson(out, cap);
}

CTRL_DLL_EXPORT int Vrto3dCtrl_ApplyConfigJson(const char* json)
{
    return g_provider.ApplyConfigJson(json);
}

CTRL_DLL_EXPORT int Vrto3dCtrl_SaveConfig(int target)
{
    return g_provider.SaveConfig(target);
}

CTRL_DLL_EXPORT int Vrto3dCtrl_ReloadConfig(void)
{
    return g_provider.ReloadConfig();
}

CTRL_DLL_EXPORT void Vrto3dCtrl_SetInputSuppressed(int suppressed)
{
    g_provider.SetInputSuppressed(suppressed != 0);
}

CTRL_DLL_EXPORT void Vrto3dCtrl_OnHmdPose(const void* driver_pose, uint32_t size)
{
    if (!driver_pose || size < sizeof(vr::DriverPose_t)) return;
    g_provider.OnHmdPose(*static_cast<const vr::DriverPose_t*>(driver_pose));
}
