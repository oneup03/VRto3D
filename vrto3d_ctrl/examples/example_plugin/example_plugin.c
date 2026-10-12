/*
 * example_plugin.c - smallest useful VRto3D controller plugin.
 *
 * SPDX-License-Identifier: MIT
 *
 * Makes the right hand slowly circle in front of the HMD and pulls the
 * trigger every other second. Build it as a DLL / .so, drop it into
 *   SteamVR/drivers/vrto3d_ctrl/bin/<platform>/plugins/
 * restart SteamVR, then tick "Example circle" in the VRto3D OSD
 * Controllers tab.
 */
#include <math.h>
#include <string.h>

#include "vrto3d_ctrl_plugin.h"

typedef struct {
    const Vrto3dCtrlHostApi* host;
    double t;
    double radius;
} Example;

static Example g_example;

static int example_init(void* user, const Vrto3dCtrlHostApi* host)
{
    Example* e = (Example*)user;
    e->host = host;
    e->t = 0.0;
    e->radius = host->get_float(host->ctx, "radius", 0.15);
    host->log(host->ctx, VRTO3D_LOG_INFO, "example plugin started");
    return 0;
}

static void example_shutdown(void* user)
{
    (void)user;
}

static void example_settings_changed(void* user)
{
    Example* e = (Example*)user;
    e->radius = e->host->get_float(e->host->ctx, "radius", 0.15);
}

static void example_poll(void* user, double dt, const Vrto3dCtrlHeadPose* head,
                         Vrto3dCtrlControllerState out[VRTO3D_HAND_COUNT])
{
    Example* e = (Example*)user;
    Vrto3dCtrlControllerState* r = &out[VRTO3D_HAND_RIGHT];
    e->t += dt;
    if (!head->valid) return;

    /* 40 cm in front of the eyes (world -Z), circling; ignores head rotation
     * to keep the example short. Identity rotation = pointing forward. */
    r->tracking = VRTO3D_TRACKING_OK;
    r->position[0] = head->position[0] + e->radius * cos(e->t);
    r->position[1] = head->position[1] - 0.2 + e->radius * sin(e->t);
    r->position[2] = head->position[2] - 0.4;
    r->rotation[0] = 1.0;
    r->trigger = (((int)e->t) % 2) ? 1.0f : 0.0f;
}

static const char* example_schema(void* user)
{
    (void)user;
    return "{\"settings\":[{\"key\":\"radius\",\"type\":\"float\",\"label\":\"Circle radius (m)\","
           "\"default\":0.15,\"min\":0.0,\"max\":0.5}]}";
}

VRTO3D_CTRL_EXPORT int vrto3d_ctrl_plugin_entry(uint32_t host_abi_version, Vrto3dCtrlPlugin* out)
{
    if (host_abi_version != VRTO3D_CTRL_ABI_VERSION) return -1;
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(Vrto3dCtrlPlugin);
    out->abi_version = VRTO3D_CTRL_ABI_VERSION;
    out->name = "example_circle";
    out->display_name = "Example circle";
    out->version = "1.0.0";
    out->capabilities = VRTO3D_CAP_POSE | VRTO3D_CAP_BUTTONS;
    out->user = &g_example;
    out->init = example_init;
    out->shutdown = example_shutdown;
    out->poll = example_poll;
    out->on_settings_changed = example_settings_changed;
    out->settings_schema = example_schema;
    return 0;
}
