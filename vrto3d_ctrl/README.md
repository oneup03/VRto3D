# VRto3D emulated controllers (`vrto3d_ctrl`)

`vrto3d_ctrl` is a second SteamVR driver that ships with VRto3D. It adds a left and a right hand controller that impersonate a **Valve Index controller** ("knuckles") or an **HTC Vive wand**. Input comes from plugins: keyboard + mouse, an XInput gamepad, a UDP feed from another app, or a DLL you write.

It is separate from `driver_vrto3d` on purpose. It never touches the HMD or VRto3D's settings. It can be disabled in SteamVR's *Manage Add-ons* without affecting VRto3D.

User documentation is in the main [README](../README.md#emulated-motion-controllers). This page is for people who want to feed the controllers from their own hardware or app.

## How it fits together

```
vrserver.exe
 ├─ driver_vrto3d.dll        HMD, 3D output, OSD (Controllers tab)
 │     └─ ctrl_bridge ──── in-process C exports (live config, status) ───┐
 └─ driver_vrto3d_ctrl.dll   two controllers + 250 Hz input thread  <────┘
       ├─ built-in plugins:  remote (UDP)  kbm (keyboard+mouse)  xinput (gamepad)
       └─ bin/<platform>/plugins/*.dll|.so   third-party plugins
```

- Every 4 ms the input thread reads the HMD pose from SteamVR and polls each enabled plugin in priority order.
- **Pose:** the first plugin in the list that reports a pose for a hand wins that hand.
- **Inputs:** buttons are OR'd across all enabled plugins and analog values take the maximum. A gamepad can add buttons on top of a pose that comes from somewhere else.
- **Head look:** a plugin that wants to turn the view sends OpenTrack packets to VRto3D's OpenTrack port through the host API. Only the highest-priority plugin with `VRTO3D_CAP_HEAD_LOOK` is allowed to.
- **Haptics:** SteamVR haptic events are forwarded to plugins with `VRTO3D_CAP_HAPTICS`.

## Two ways to integrate

| | Remote UDP | Plugin DLL |
|---|---|---|
| Runs in | your own process | vrserver |
| Language | anything that can send UDP | anything with a C ABI (C, C++, Rust, Zig, Dart FFI...) |
| Crash isolation | full | SEH-guarded on Windows; a crash disables the plugin |
| Settings UI in the OSD | port / timeout only | your own settings, drawn from your schema |
| Header | [`vrto3d_ctrl_remote.h`](include/vrto3d_ctrl_remote.h) | [`vrto3d_ctrl_plugin.h`](include/vrto3d_ctrl_plugin.h) |

Both public headers are MIT-licensed so closed-source or differently licensed code can use them. The rest of VRto3D is LGPL-3.0.

### Remote UDP

1. In the OSD Controllers tab, enable `Remote (UDP)`. The default port is `4243`. Tick `Listen on all interfaces` to accept other devices on the network.
2. Send `Vrto3dCtrlRemotePacket` datagrams (248 bytes, little-endian, packed) at 60 to 250 Hz.
3. Set `hand[i].connected = 1` for each hand you drive. A hand disconnects after the stale timeout (500 ms by default) with no packets.
4. Read `Vrto3dCtrlRemoteHaptic` datagrams that come back to your socket when the game vibrates a hand.

[`tools/remote_controller_test.py`](../tools/remote_controller_test.py) is a complete sender in about 100 lines.

### Plugin DLL

A plugin exports one function:

```c
#include "vrto3d_ctrl_plugin.h"

VRTO3D_CTRL_EXPORT int vrto3d_ctrl_plugin_entry(uint32_t host_abi, Vrto3dCtrlPlugin* out)
{
    if (host_abi != VRTO3D_CTRL_ABI_VERSION) return -1;
    memset(out, 0, sizeof(*out));
    out->struct_size  = sizeof(*out);
    out->abi_version  = VRTO3D_CTRL_ABI_VERSION;
    out->name         = "my_tracker";         /* [a-z0-9_]+, unique */
    out->display_name = "My Tracker";
    out->version      = "1.0.0";
    out->capabilities = VRTO3D_CAP_POSE | VRTO3D_CAP_BUTTONS;
    out->init = my_init; out->shutdown = my_shutdown; out->poll = my_poll;
    out->settings_schema = my_schema;       /* optional */
    return 0;
}
```

Copy the DLL into `SteamVR/drivers/vrto3d_ctrl/bin/win64/plugins/` and restart SteamVR. Then tick it in the OSD. Third-party plugins start disabled. [`examples/example_plugin`](examples/example_plugin/example_plugin.c) is a complete plugin in about 80 lines of C.

Contract:
- **One thread.** `init`, `poll`, `on_*` and `shutdown` are all called from the driver's input thread, so your state needs no locks. Host API functions may be called from any thread.
- **Lazy start.** `settings_schema` is called once, right after the entry point. `init` runs the first time the user enables your plugin while controllers are enabled.
- **Shutdown cleanly.** `shutdown` must stop any threads you started. Crashes on your own threads are not guarded.
- **Poll fast.** `poll` runs at about 250 Hz and must not block. Read your device on your own thread if it is slow.
- **Fill only what you have.** `out[]` arrives zeroed with an identity rotation. Leave `tracking` at `VRTO3D_TRACKING_NONE` for a hand you have no pose for. Your buttons for that hand still count.

### Coordinates

- SteamVR raw tracking space, in metres: `+X` right, `+Y` up, `-Z` forward. Quaternions are `(w, x, y, z)`.
- The HMD pose arrives in every `poll` (`head`), so head-relative placement is just `head.position + rotate(head.rotation, offset)`.
- **Poses are aim poses.** `-Z` is where the controller points, the way a laser or gun barrel would, and `+Y` is the back of the hand. The driver converts that to the raw pose of the emulated model, so the game's pointer lands where you aimed. You never need the Index or Vive render-model offsets.
- **Say what the hand is attached to.** If you place a hand relative to the head (in front of the face, at the hip, from a head-relative tracker), set `pose_anchor` to `VRTO3D_ANCHOR_HEAD`, or to `VRTO3D_ANCHOR_HEADING` if it follows only the head's yaw. VRto3D hands the controller driver every HMD pose the moment it publishes it, and the driver re-places anchored hands in that same instant. Without this, a head-relative hand trails the view by a few milliseconds and shakes whenever the head turns. Leave it at `VRTO3D_ANCHOR_WORLD` (0) for independently tracked hands.
- Velocities are optional. Set `VRTO3D_CAP_VELOCITY` only if you fill them; otherwise they are left at zero, or estimated if the user turns on `Derive hand velocity`.

### Settings and the OSD

Return a JSON schema from `settings_schema` and the VRto3D OSD draws the widgets for you:

```json
{"settings": [
  {"type": "header", "label": "Tracking"},
  {"key": "port", "type": "int", "label": "Port", "default": 5555, "min": 1024, "max": 65535},
  {"key": "smoothing", "type": "float", "label": "Smoothing", "default": 0.2, "min": 0, "max": 1,
   "help": "Shown as a tooltip"},
  {"key": "mode", "type": "enum", "label": "Mode", "default": "a", "values": ["a", "b"]},
  {"key": "recenter", "type": "keybind", "label": "Recenter key", "default": "Key_End"},
  {"key": "presets", "type": "table", "label": "Presets", "default": [],
   "columns": [{"key": "name", "type": "string"}, {"key": "x", "type": "float"}]}
]}
```

- Types are `bool`, `int`, `float`, `enum`, `keybind`, `string`, `table` and `header` (a label with no value).
- `keybind` values are VRto3D key names: `Key_A`, `Key_Space`, `Mouse_Left`, `Pad_A`, `Numpad7` and so on. The full list is in `external/VRto3DLib/src/key_names.cpp`.
- Values are stored in `controllers.json` under `plugins.<name>`. Missing keys are filled from `default`. Read them with the host getters (`get_bool`, `get_float`, `get_string`, `get_json`...).
- `on_settings_changed` fires after an OSD edit, a file reload or a game-profile switch. Re-read your settings there.

## Configuration files

Both files live in `<Steam>/config/vrto3d/`:

| File | Contents |
|---|---|
| `controllers.json` | Global settings, always complete. Created on first start with controllers disabled. |
| `<Game>.exe_controllers.json` | Only the keys that differ for that game, merged on top when it connects. |

The driver notices hand edits to either file within about a second. Its log is `<Steam>/logs/vrto3d_ctrl.txt`.

## Building

- **Windows:** the solution builds `vrto3d_ctrl` alongside `vrto3d` and copies both to `SteamVR\drivers`.
- **Linux:** `vrto3d/CMakeLists.txt` adds this folder, so `cmake -S vrto3d -B build` stages both drivers under `build/output/drivers`.
- **Example plugin:** configure with `-DVRTO3D_CTRL_BUILD_EXAMPLES=ON`, or build `examples/example_plugin` on its own.

## Credits

- The keyboard + mouse control scheme follows r57zone's [Half-Life: Alyx NoVR](https://github.com/r57zone/Half-Life-Alyx-novr) driver. Only the behaviour was borrowed; the code is written from scratch.
- Impersonating Index controllers through SteamVR's bundled input profile follows the approach of [OpenGloves](https://github.com/LucidVR/opengloves-driver) and [driver_leap](https://github.com/SDraw/driver_leap).
