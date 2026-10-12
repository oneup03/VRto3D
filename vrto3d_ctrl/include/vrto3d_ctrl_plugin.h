/*
 * vrto3d_ctrl_plugin.h - public plugin ABI for the VRto3D emulated
 * controller driver (driver_vrto3d_ctrl).
 *
 * SPDX-License-Identifier: MIT
 *
 * This header is deliberately MIT-licensed (the rest of VRto3D is LGPL-3.0)
 * so that plugins written in any language and under any license can include
 * it or transcribe it for FFI.
 *
 * Copyright (c) VRto3D contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *
 * ---------------------------------------------------------------------------
 * How plugins work
 * ---------------------------------------------------------------------------
 * A plugin is a DLL (Windows) or shared object (Linux) placed in the
 * driver's  bin/<platform>/plugins/  folder. It exports exactly one symbol,
 * VRTO3D_CTRL_PLUGIN_ENTRY_NAME, with the Vrto3dCtrlPluginEntryFn signature.
 * The driver calls it once, the plugin fills in a Vrto3dCtrlPlugin, and from
 * then on the driver calls the plugin's function pointers.
 *
 * Threading: every plugin callback (init, poll, on_*, shutdown) is invoked
 * from the same driver-owned input thread, so a plugin never needs locks for
 * its own state. The host API functions may be called from any thread.
 *
 * Crashes: on Windows every callback runs under a structured-exception
 * guard. A plugin that faults is disabled until SteamVR restarts instead of
 * taking vrserver down. Faults on threads the plugin creates itself are NOT
 * guarded.
 *
 * Coordinates: SteamVR "raw" tracking space, metres. +X right, +Y up,
 * -Z forward. Quaternions are (w, x, y, z).
 *
 * Controller poses are AIM poses: -Z points where the controller points (the
 * laser / gun barrel direction) and +Y is the top of the hand. The driver
 * converts an aim pose into the emulated controller's raw pose itself, so a
 * plugin never needs to know the render model's internal offsets.
 */
#ifndef VRTO3D_CTRL_PLUGIN_H
#define VRTO3D_CTRL_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VRTO3D_CTRL_ABI_VERSION 1u

#if defined(_WIN32)
#  define VRTO3D_CTRL_EXPORT __declspec(dllexport)
#else
#  define VRTO3D_CTRL_EXPORT __attribute__((visibility("default")))
#endif

/* Hand indices. */
enum {
    VRTO3D_HAND_LEFT  = 0,
    VRTO3D_HAND_RIGHT = 1,
    VRTO3D_HAND_COUNT = 2
};

/* Capability flags a plugin reports in Vrto3dCtrlPlugin::capabilities. */
enum {
    VRTO3D_CAP_POSE      = 1u << 0, /* fills position / rotation             */
    VRTO3D_CAP_VELOCITY  = 1u << 1, /* fills velocities (else host derives)  */
    VRTO3D_CAP_BUTTONS   = 1u << 2, /* fills digital / analog inputs         */
    VRTO3D_CAP_FINGERS   = 1u << 3, /* fills finger_curl[]                   */
    VRTO3D_CAP_HEAD_LOOK = 1u << 4, /* steers the head via send_opentrack    */
    VRTO3D_CAP_HAPTICS   = 1u << 5  /* wants on_haptic callbacks             */
};

/* Digital input bits, used by both `buttons` (pressed) and `touches`. */
enum {
    VRTO3D_BTN_SYSTEM     = 1u << 0, /* SteamVR dashboard; not seen by games */
    VRTO3D_BTN_A          = 1u << 1,
    VRTO3D_BTN_B          = 1u << 2,
    VRTO3D_BTN_TRIGGER    = 1u << 3,
    VRTO3D_BTN_GRIP       = 1u << 4,
    VRTO3D_BTN_THUMBSTICK = 1u << 5,
    VRTO3D_BTN_TRACKPAD   = 1u << 6,
    VRTO3D_BTN_MENU       = 1u << 7  /* Vive application menu               */
};

/* What a hand pose is attached to (Vrto3dCtrlControllerState::pose_anchor).
 *
 * Hands placed relative to the head (keyboard/mouse aiming, gamepad hands in
 * front of the body) must move in exactly the same instant as the HMD, or
 * they shake against the view whenever the head turns. Tell the driver how
 * your pose was derived and it re-applies the latest HMD pose to it at the
 * moment VRto3D publishes that pose, instead of whenever you were polled.
 * Always fill position/rotation in world space as usual. */
enum {
    VRTO3D_ANCHOR_WORLD   = 0, /* independently tracked (default)              */
    VRTO3D_ANCHOR_HEAD    = 1, /* rigidly follows the head (position + rotation) */
    VRTO3D_ANCHOR_HEADING = 2  /* follows head position and yaw, not pitch/roll */
};

/* Per-hand tracking state reported by a plugin. */
enum {
    VRTO3D_TRACKING_NONE         = 0, /* this plugin has nothing for the hand */
    VRTO3D_TRACKING_OK           = 1,
    VRTO3D_TRACKING_OUT_OF_RANGE = 2
};

/* Log levels for Vrto3dCtrlHostApi::log. */
enum {
    VRTO3D_LOG_DEBUG = 0,
    VRTO3D_LOG_INFO  = 1,
    VRTO3D_LOG_WARN  = 2,
    VRTO3D_LOG_ERROR = 3
};

typedef struct Vrto3dCtrlHeadPose {
    uint32_t struct_size;   /* sizeof(Vrto3dCtrlHeadPose)                    */
    uint32_t valid;         /* 0 when SteamVR has no HMD pose yet            */
    double   position[3];   /* metres                                        */
    double   rotation[4];   /* w, x, y, z                                    */
    double   sample_time_s; /* host monotonic clock, seconds                 */
} Vrto3dCtrlHeadPose;

typedef struct Vrto3dCtrlControllerState {
    uint32_t struct_size;   /* host sets this before poll                    */
    uint32_t tracking;      /* VRTO3D_TRACKING_*                             */
    double   position[3];   /* aim pose position, metres                     */
    double   rotation[4];   /* aim pose rotation, w, x, y, z                 */
    double   velocity[3];          /* m/s, only with VRTO3D_CAP_VELOCITY      */
    double   angular_velocity[3];  /* rad/s, only with VRTO3D_CAP_VELOCITY    */
    double   sample_time_s;        /* 0 = "now"                               */
    uint32_t buttons;       /* VRTO3D_BTN_* pressed bits                      */
    uint32_t touches;       /* VRTO3D_BTN_* touched bits                      */
    float    trigger;       /* 0..1                                           */
    float    grip;          /* 0..1                                           */
    float    grip_force;    /* 0..1, squeeze beyond full grip                 */
    float    thumbstick[2]; /* -1..1, +X right, +Y up                         */
    float    trackpad[2];   /* -1..1                                          */
    float    trackpad_force;/* 0..1                                           */
    float    finger_curl[5];/* thumb, index, middle, ring, pinky; 0..1        */
    uint32_t pose_anchor;   /* VRTO3D_ANCHOR_*: how the pose follows the head  */
} Vrto3dCtrlControllerState;

/* Functions the driver offers to a plugin. `ctx` is the plugin's own host
 * context pointer (Vrto3dCtrlHostApi::ctx); pass it back to every call. */
typedef struct Vrto3dCtrlHostApi {
    uint32_t struct_size;
    uint32_t host_abi_version;
    void*    ctx;

    void (*log)(void* ctx, int level, const char* utf8_message);

    /* Read this plugin's settings (stored under plugins.<name> in
     * controllers.json and edited from the VRto3D OSD Controllers tab).
     * Missing keys return the supplied default. Strings and JSON values are
     * copied into buf; the return value is the full length (excluding the
     * terminator), so a caller can retry with a larger buffer. */
    int      (*get_bool)  (void* ctx, const char* key, int default_value);
    int64_t  (*get_int)   (void* ctx, const char* key, int64_t default_value);
    double   (*get_float) (void* ctx, const char* key, double default_value);
    uint32_t (*get_string)(void* ctx, const char* key, char* buf, uint32_t buf_size);
    uint32_t (*get_json)  (void* ctx, const char* key, char* buf, uint32_t buf_size);

    /* Steer the HMD by sending one OpenTrack packet (centimetres / degrees,
     * OpenTrack conventions) to VRto3D's OpenTrack UDP port. VRto3D must
     * have OpenTrack enabled. Only the highest-priority enabled plugin with
     * VRTO3D_CAP_HEAD_LOOK is allowed; others get 0 back. */
    int (*send_opentrack)(void* ctx, double x_cm, double y_cm, double z_cm,
                          double yaw_deg, double pitch_deg, double roll_deg);

    /* 1 when the connected game (or VRto3D's own output window) has focus. */
    int (*is_game_foreground)(void* ctx);

    /* 1 while the VRto3D OSD menu is open: stop consuming keyboard/mouse. */
    int (*is_input_suppressed)(void* ctx);

    /* Process id of the connected game, 0 when none. */
    uint32_t (*get_game_pid)(void* ctx);
} Vrto3dCtrlHostApi;

/* Filled in by the plugin's entry point. Unused optional callbacks may be
 * NULL. Strings must stay valid until shutdown returns. init runs lazily,
 * the first time the plugin is enabled while controllers are enabled; a
 * plugin that is never enabled is never initialised. shutdown must stop
 * any threads the plugin started. */
typedef struct Vrto3dCtrlPlugin {
    uint32_t    struct_size;  /* sizeof(Vrto3dCtrlPlugin)                    */
    uint32_t    abi_version;  /* VRTO3D_CTRL_ABI_VERSION                      */
    const char* name;         /* unique id, [a-z0-9_]+ ; settings key         */
    const char* display_name; /* shown in the OSD                             */
    const char* version;
    uint32_t    capabilities; /* VRTO3D_CAP_*                                 */
    void*       user;         /* passed back to every callback                */

    /* Return 0 on success. `host` stays valid until shutdown returns. */
    int  (*init)(void* user, const Vrto3dCtrlHostApi* host);
    void (*shutdown)(void* user);

    /* Called at ~250 Hz while controllers are enabled. `out` arrives zeroed
     * (identity rotation, struct_size set). Leave a hand's tracking at
     * VRTO3D_TRACKING_NONE to contribute no pose for it; its buttons and
     * axes are merged with the other plugins' either way (digital OR,
     * analog max), so a buttons-only plugin just leaves tracking alone.
     * `head` is the HMD's current pose. */
    void (*poll)(void* user, double dt_s, const Vrto3dCtrlHeadPose* head,
                 Vrto3dCtrlControllerState out[VRTO3D_HAND_COUNT]);

    /* Optional. A game asked for vibration on `hand`. */
    void (*on_haptic)(void* user, uint32_t hand, float duration_s,
                      float frequency_hz, float amplitude);

    /* Optional. Settings changed (OSD edit, file reload, game profile
     * switch); re-read them through the host getters. */
    void (*on_settings_changed)(void* user);

    /* Optional. A game connected (pid != 0) or disconnected (pid == 0). */
    void (*on_game_changed)(void* user, const char* exe_name, uint32_t pid);

    /* Optional. UTF-8 JSON describing this plugin's settings so the OSD can
     * draw widgets for them without the plugin touching the UI:
     *   {"settings":[{"key":"sens","type":"float","label":"Sensitivity",
     *                 "default":1.0,"min":0.1,"max":5.0,"help":"..."}]}
     * Types: bool, int, float, enum ("values":[...]), keybind (VRto3D key
     * name such as "Key_W", "Mouse_Left", "Pad_A"), string, table
     * ("columns":[setting...], value is an array of row objects), and
     * header (a section label with no key or value).
     * Called once right after the entry point, before init, so it must not
     * depend on init having run. The pointer must stay valid until
     * shutdown. */
    const char* (*settings_schema)(void* user);
} Vrto3dCtrlPlugin;

/* The one required export. Return 0 on success. */
typedef int (*Vrto3dCtrlPluginEntryFn)(uint32_t host_abi_version,
                                       Vrto3dCtrlPlugin* out_plugin);

#define VRTO3D_CTRL_PLUGIN_ENTRY_NAME "vrto3d_ctrl_plugin_entry"

#ifdef __cplusplus
}
#endif

#endif /* VRTO3D_CTRL_PLUGIN_H */
