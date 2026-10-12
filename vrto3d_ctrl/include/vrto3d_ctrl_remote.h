/*
 * vrto3d_ctrl_remote.h - UDP wire protocol for the "remote" plugin of the
 * VRto3D emulated controller driver. Lets another process (VertoXR, a phone
 * app, a tracker bridge, a script) drive the emulated controllers.
 *
 * SPDX-License-Identifier: MIT  (see vrto3d_ctrl_plugin.h for the text)
 *
 * Send Vrto3dCtrlRemotePacket datagrams to 127.0.0.1:<port> (default 4243;
 * set "Listen on all interfaces" in the OSD to accept LAN senders). All
 * fields are little-endian. A hand is held for `stale timeout` ms after the
 * last packet that marked it connected, then reported as disconnected.
 *
 * Poses are AIM poses in SteamVR raw space (+X right, +Y up, -Z forward,
 * metres, quaternion w,x,y,z); see vrto3d_ctrl_plugin.h. With
 * VRTO3D_REMOTE_FLAG_HEAD_RELATIVE they are expressed in the HMD's frame
 * instead and the driver adds the current HMD pose.
 *
 * When a game requests vibration the driver answers the most recent sender
 * with a Vrto3dCtrlRemoteHaptic datagram.
 */
#ifndef VRTO3D_CTRL_REMOTE_H
#define VRTO3D_CTRL_REMOTE_H

#include <stdint.h>

#define VRTO3D_REMOTE_MAGIC        "V3DC"
#define VRTO3D_REMOTE_HAPTIC_MAGIC "V3DH"
#define VRTO3D_REMOTE_VERSION      1u
#define VRTO3D_REMOTE_DEFAULT_PORT 4243

/* Packet flags */
#define VRTO3D_REMOTE_FLAG_HEAD_RELATIVE 0x0001u /* pose is in the HMD frame  */
#define VRTO3D_REMOTE_FLAG_YAW_ONLY      0x0002u /* ...using only HMD yaw    */

#pragma pack(push, 1)

typedef struct Vrto3dCtrlRemoteHand {
    uint8_t  connected;      /* 0 = leave this hand alone this packet        */
    uint8_t  tracking;       /* VRTO3D_TRACKING_* from vrto3d_ctrl_plugin.h  */
    uint8_t  reserved[2];
    float    position[3];
    float    rotation[4];    /* w, x, y, z                                   */
    float    velocity[3];
    float    angular_velocity[3];
    uint32_t buttons;        /* VRTO3D_BTN_* bits                            */
    uint32_t touches;
    float    trigger;
    float    grip;
    float    grip_force;
    float    thumbstick[2];
    float    trackpad[2];
    float    trackpad_force;
    float    finger_curl[5];
} Vrto3dCtrlRemoteHand;           /* 116 bytes */

typedef struct Vrto3dCtrlRemotePacket {
    char     magic[4];       /* "V3DC"                                       */
    uint16_t version;        /* VRTO3D_REMOTE_VERSION                        */
    uint16_t flags;          /* VRTO3D_REMOTE_FLAG_*                         */
    uint64_t timestamp_us;   /* sender clock, informational                  */
    Vrto3dCtrlRemoteHand hand[2]; /* [0] = left, [1] = right                 */
} Vrto3dCtrlRemotePacket;         /* 248 bytes */

typedef struct Vrto3dCtrlRemoteHaptic {
    char     magic[4];       /* "V3DH"                                       */
    uint16_t version;
    uint8_t  hand;           /* 0 = left, 1 = right                          */
    uint8_t  reserved;
    float    duration_s;
    float    frequency_hz;
    float    amplitude;      /* 0..1                                         */
} Vrto3dCtrlRemoteHaptic;         /* 20 bytes */

#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(Vrto3dCtrlRemoteHand) == 116, "remote hand layout");
static_assert(sizeof(Vrto3dCtrlRemotePacket) == 248, "remote packet layout");
static_assert(sizeof(Vrto3dCtrlRemoteHaptic) == 20, "remote haptic layout");
#endif

#endif /* VRTO3D_CTRL_REMOTE_H */
