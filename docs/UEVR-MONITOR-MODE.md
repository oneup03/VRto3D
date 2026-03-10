# UEVR Monitor Mode with VRto3D

## Overview

When used with UEVR's Monitor Mode, VRto3D acts as the stereo output — it takes the left and right eye views that UEVR renders and composites them into a side-by-side (or top-and-bottom) image on your 3D display.

All the 3D rendering decisions (eye separation, convergence, depth flattening, HUD depth) are handled by UEVR. VRto3D just provides the display window. This means you don't need to fiddle with VRto3D's depth or convergence settings — UEVR controls everything through its Monitor 3D tab.

---

## Installation

1. Download the [latest VRto3D release](https://github.com/oneup03/VRto3D/releases/latest/download/vrto3d.zip).
2. Copy the `vrto3d` folder into `Steam\steamapps\common\SteamVR\drivers\`.
3. Launch SteamVR once to generate the `default_config.json` config file. You should see the VRto3D headset window appear.
4. [Set SteamVR as OpenXR Runtime](https://www.vive.com/us/support/vs/category_howto/trouble-with-openxr-titles.html).
5. Close SteamVR and edit `Steam\config\vrto3d\default_config.json` as described below.

For full installation details (including interlaced, frame packing, and SR display setups), see the [main VRto3D README](../README.md).

---

## Key Settings for Monitor Mode

Most VRto3D settings can be left at their defaults when using UEVR Monitor Mode. These are the ones that matter:

### Display Selection

**`display_index`** — Which monitor the 3D window appears on. `0` = primary display, `1` = first display, `2` = second, etc. Set this to your 3D display.

**`render_width`** / **`render_height`** — Resolution per eye. Default is 1920x1080. Reduce to save performance, or increase for higher-resolution displays.

### 3D Output Format

**`tab_enable`** — Set to `true` for top-and-bottom output, `false` (default) for side-by-side. Match this to what your display or post-processing pipeline expects.

**`reverse_enable`** — Swaps left/right eyes if the 3D appears reversed on your display.

### Depth Adjustment (Hotkeys)

VRto3D's depth can be adjusted with keyboard hotkeys during gameplay:

- `Ctrl+F3` / `Ctrl+F4` — Decrease / increase depth
- `Ctrl+F7` — Save current settings as a profile for the running game

UEVR also provides depth buttons in its Monitor 3D tab (**VRto3D++**, **VRto3D+**, **Calibrate**, **VRto3D-**, **VRto3D--**) that send commands to VRto3D through shared memory.

### What to Leave Alone

In Monitor Mode, UEVR handles all stereo rendering decisions. These VRto3D settings are either overridden or irrelevant:

- **`convergence`** — UEVR controls convergence through its projection matrix. VRto3D's convergence setting doesn't affect the game's stereo output.
- **`fov`** — UEVR manages FOV through the engine. VRto3D's FOV is used for the virtual HMD shape but doesn't change what you see.
- **Pitch/yaw emulation** — Not needed. UEVR doesn't use head rotation in monitor mode.
- **User presets** — You can still use VRto3D presets for depth hotkeys, but convergence presets won't have a visible effect since UEVR controls convergence.

---

## What UEVR Handles Automatically

When Monitor Mode is enabled in UEVR's Monitor 3D tab, these are all managed by UEVR — you don't need to configure them in VRto3D:

- **Eye separation and convergence** — Calculated from your display setup (IPD, screen height, viewing distance)
- **Depth flattening during zoom** — Automatically reduces 3D intensity when the game zooms (ADS, scope, cutscenes)
- **HUD depth and size** — Adjustable per gameplay mode through UEVR's interface
- **FOV tracking** — Reads the game's field of view each frame for zoom detection
- **Leia LookAround** — Head tracking parallax for Leia displays (if applicable)

See the UEVR Monitor Mode Guide (`docs/MONITOR-MODE-GUIDE.md` in the UEVR fork) for complete UEVR-side setup instructions.

---

## Troubleshooting

| Problem | Likely Cause | What to Do |
|---------|-------------|------------|
| Black VRto3D window | SteamVR not receiving frames | Make sure UEVR is injected and Monitor Mode is enabled. Check SteamVR is running. Restart SteamVR if needed. |
| Headset window on wrong monitor | Wrong `display_index` | Edit `default_config.json` and set `display_index` to your 3D display's number. Restart SteamVR. |
| Resolution looks wrong | `render_width`/`render_height` mismatch | Set these to match your display's per-eye resolution. For a 1920x1080 SBS display, use 960x1080 per eye (or leave at 1920x1080 for full-resolution rendering). |
| 3D is reversed (left/right swapped) | Eye order wrong for your display | Set `reverse_enable` to `true` in `default_config.json`. |
| UEVR shows "Waiting..." instead of "Connected" | VRto3D not active | Check that VRto3D is enabled in SteamVR's Settings > Startup/Shutdown > Manage Add-Ons. Disable any other virtual HMD drivers. |
| SteamVR crashes on startup | Conflicting VR drivers | Disable other virtual HMD drivers (ALVR, VRidge, iVRy, etc.) in SteamVR's Manage Add-Ons. Try a [clean SteamVR install](https://steamcommunity.com/app/250820/discussions/2/1640917625015598552/). |
