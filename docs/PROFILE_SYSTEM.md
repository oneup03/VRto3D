# VRto3D Profile System

## Overview

VRto3D loads per-game JSON profiles to customize stereo parameters. When UEVR is connected, profiles can include "modifiers" that override depth curve behavior for the classification system.

## Profile Loading Pipeline

```
SteamVR RunFrame() event loop
    -> ProcessConnected event (new app detected)
        -> GetProcessName(pid) -> normalize to lowercase
            -> MockControllerDeviceDriver::LoadSettings(app_name)
                -> Look for: Documents\My Games\vrto3d\{app_name}_config.json
                -> JsonManager::LoadProfileFromJson(filename, config)
                -> StereoDisplayComponent::LoadSettings(config)
                -> parse_uevr_modifiers(app_name)
                    -> Read "uevr_modifiers" from same JSON
                    -> receiver().write_modifiers(mods) -> shared memory
```

### No-Profile Fallback

If no profile exists:
1. `no_profile_` flag is set
2. `PoseUpdateThread` detects this and calls `calculate_auto_stereo(world_scale)`
3. Auto-stereo targets `eyeOffset ~ 0.04` based on world_scale from UEVR
4. Applied once (deferred until bridge data available)

## JSON Profile Format

Location: `Documents\My Games\vrto3d\{app_name}_config.json`

### Core Fields

```json
{
  "window_x": 0,
  "window_y": 0,
  "window_width": 3840,
  "window_height": 1080,
  "render_width": 1920,
  "render_height": 1080,
  "fov": 90.0,
  "depth": 0.065,
  "convergence": 2.0,
  "tab_enable": false,
  "reverse_enable": false,
  "vd_fsbs_hack": false
}
```

### Key Stereo Fields

| Field | Type | Range | Purpose |
|-------|------|-------|---------|
| `fov` | float | 60-110 | Display FOV (degrees). Used by GetProjectionRaw in VR mode. NOT game camera FOV. |
| `depth` | float | 0.01-0.5 | Eye separation (IPD-like). Affects VR mode asymmetry. Init-only overlay IPD in monitor mode. |
| `convergence` | float | 0.5-4.0 | Focus distance multiplier. VR mode only. |

**Note**: In monitor mode, `fov` and `convergence` have no rendering effect. Only `depth` matters (init-only overlay IPD, overridden by stereo_depth_hint). `tab_enable` affects viewport layout.

### UEVR Modifiers Extension

Optional section for per-game tuning of UEVR's depth classification system.

```json
{
  "uevr_modifiers": {
    "depth_strength": 1.0,
    "depth_min_floor": 0.0,
    "ads_floor": 0.2,
    "scope_floor": 0.05,
    "cutscene_floor": 0.95,
    "base_power": 2.5,
    "extra_power": 1.5,
    "dead_zone": 1.15,
    "transition_speed": 0.08,
    "zoom_threshold": 1.15,
    "base_fov_override": 0.0,
    "blend_ads": 0.85,
    "blend_scope": 0.55,
    "blend_passive": 0.70
  }
}
```

## UEVR Command Handling

PoseUpdateThread processes UEVR commands via `command_seq` in shared memory:

| Command | Seq Value | Action |
|---------|-----------|--------|
| Calibrate | 2 | Reset depth/convergence to defaults |
| +3D | 3 | Increase depth by step |
| -3D | 4 | Decrease depth by step |
| ++3D | 5 | Large depth increase |
| --3D | 6 | Large depth decrease |
| +++3D | 8 | Maximum depth increase |
| ---3D | 9 | Maximum depth decrease |

## User Presets

3 preset slots for quick switching during gameplay (load/store via configurable hotkeys). Presets store depth, convergence, and fov. Managed in `PollHotkeysThread`.
