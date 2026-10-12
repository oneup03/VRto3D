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

// In-process bridge between driver_vrto3d (the HMD + OSD) and
// driver_vrto3d_ctrl (the emulated controllers). Both drivers are loaded into
// the same vrserver process, so the OSD resolves these C exports from the
// already-loaded controller module (GetModuleHandle + GetProcAddress, or
// dlopen(RTLD_NOLOAD) + dlsym). Nothing here is required: when the
// controller driver is not installed the OSD simply shows it as missing.

#include <stdint.h>

#define VRTO3D_CTRL_BRIDGE_VERSION 1

#ifdef _WIN32
#define VRTO3D_CTRL_MODULE_NAME L"driver_vrto3d_ctrl.dll"
#else
#define VRTO3D_CTRL_MODULE_NAME "driver_vrto3d_ctrl.so"
#endif

extern "C" {

// Returns VRTO3D_CTRL_BRIDGE_VERSION.
typedef int (*Vrto3dCtrl_GetBridgeVersionFn)(void);

// Writes the live state as UTF-8 JSON:
//   {"version":"...","game":"x.exe","config":{...controllers.json...},
//    "plugins":[{"name","display_name","version","state","builtin",
//                "capabilities","schema":{...}}],
//    "devices_type":"knuckles"}
// Returns the full length (excluding the terminator); when it is >= cap the
// output was truncated and the caller should retry with a bigger buffer.
typedef uint32_t (*Vrto3dCtrl_GetStateJsonFn)(char* out, uint32_t cap);

// Replace the live config with this controllers.json-shaped object. Applied
// on the next input tick. Nothing is written to disk. Returns 0 on success.
typedef int (*Vrto3dCtrl_ApplyConfigJsonFn)(const char* json);

// Persist the live config. target 0 = controllers.json (global defaults),
// 1 = <game>.exe_controllers.json (only the keys that differ from the
// defaults). Returns 0 on success.
typedef int (*Vrto3dCtrl_SaveConfigFn)(int target);

// Re-read the files from disk (defaults, then the current game's overrides).
typedef int (*Vrto3dCtrl_ReloadConfigFn)(void);

// Called by the OSD when its menu opens / closes so keyboard and mouse
// plugins stop consuming input while the user is navigating the menu.
typedef void (*Vrto3dCtrl_SetInputSuppressedFn)(int suppressed);

// Called by VRto3D's pose thread right after every HMD pose it publishes,
// with that exact vr::DriverPose_t (size = sizeof(vr::DriverPose_t)). Hands
// anchored to the head are re-published from inside this call, so they can
// never lag the HMD by a frame. Must stay cheap: it runs on VRto3D's pose
// thread.
typedef void (*Vrto3dCtrl_OnHmdPoseFn)(const void* driver_pose, uint32_t size);

}  // extern "C"

#define VRTO3D_CTRL_EXPORT_GET_BRIDGE_VERSION "Vrto3dCtrl_GetBridgeVersion"
#define VRTO3D_CTRL_EXPORT_GET_STATE_JSON     "Vrto3dCtrl_GetStateJson"
#define VRTO3D_CTRL_EXPORT_APPLY_CONFIG_JSON  "Vrto3dCtrl_ApplyConfigJson"
#define VRTO3D_CTRL_EXPORT_SAVE_CONFIG        "Vrto3dCtrl_SaveConfig"
#define VRTO3D_CTRL_EXPORT_RELOAD_CONFIG      "Vrto3dCtrl_ReloadConfig"
#define VRTO3D_CTRL_EXPORT_SET_INPUT_SUPPRESSED "Vrto3dCtrl_SetInputSuppressed"
#define VRTO3D_CTRL_EXPORT_ON_HMD_POSE        "Vrto3dCtrl_OnHmdPose"
