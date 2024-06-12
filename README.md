# VRto3D

OpenVR Driver that can render in SbS or TaB 3D.
Currently targeting OpenVR 2.5.1.
Windows-only solution, but there are other solutions on Linux like MonadoVR.


## Configuration

- Modify the `vrto3d\resources\settings\default.vrsettings` for your setup:

| Field Name          | Type    | Description                                                                                 | Default Value  |
|---------------------|---------|---------------------------------------------------------------------------------------------|----------------|
| `window_x`          | `int`   | The X position of the window - can be used to move view to another display                  | `0`            |
| `window_y`          | `int`   | The Y position of the window - can be used to move view to another display                  | `0`            |
| `window_width`      | `int`   | The width of the application window.                                                        | `1920`         |
| `window_height`     | `int`   | The height of the application window.                                                       | `1080`         |
| `aspect_ratio`      | `float` | The aspect ratio used to calculate vertical FoV                                             | `1.77778`      |
| `fov`               | `float` | The field of view (FoV) for the VR rendering.                                               | `90.0`         |
| `depth`             | `float` | The max depth. Overrides VR's IPD field.                                                    | `0.5`          |
| `convergence`       | `float` | Where the left and right images converge. Adjusts frustum.                                  | `0.1`          |
| `tab_enable`        | `bool`  | Enable or disable top-and-bottom (TaB) 3D output (Side by Side is default)                  | `false`        |
| `half_enable`       | `bool`  | Enable or disable half SbS/TaB 3D output.                                                   | `true`         |
| `reverse_enable`    | `bool`  | Enable or disable reversed 3D output.                                                       | `false`        |
| `ss_enable`         | `bool`  | Enable or disable supersampling.                                                            | `false`        |
| `hdr_enable`        | `bool`  | Enable or disable HDR.                                                                      | `false`        |
| `depth_gauge`       | `bool`  | Enable or disable SteamVR IPD depth gauge display.                                          | `false`        |
| `ss_scale`          | `float` | The supersample scale.                                                                      | `1.0`          |
| `display_latency`   | `float` | The display latency in seconds.                                                             | `0.011`        |
| `display_frequency` | `float` | The display refresh rate, in Hz.                                                            | `60.0`         |
| `num_user_settings` | `int`   | The number of user settings defined below.                                                  | `3`            |
| `user_load_key#`    | `string`| The [Virtual-Key Code](https://github.com/oneup03/VRto3D/blob/main/vrto3d/src/key_mappings.h) to load user setting # (replace # with integer number)                | `"VK_NUMPAD1"` |
| `user_store_key#`   | `string`| The Virtual-Key Code to store user setting # (replace # with integer number)                | `"VK_NUMPAD4"` |
| `user_depth#`       | `float` | The depth value for user setting # (replace # with integer number)                          | `0.5`          |
| `user_convergence#` | `float` | The convergence value for user setting # (replace # with integer number)                    | `0.1`          |
| `user_hold#`        | `bool`  | User setting # (replace # with integer number) requires holding the button                  | `false`        |


## Installation

- Install SteamVR
- Download the latest release and copy the `vrto3d` folder to your `Steam\steamapps\common\SteamVR\drivers` folder
- Edit the `Steam\steamapps\common\SteamVR\drivers\vrto3d\resources\settings\default.vrsettings` as needed
- Run SteamVR
- Try launching a VR game
- Adjust Depth with `Ctrl+F3` and `Ctrl+F4`
- Adjust Convergence with `Ctrl+F5` and `Ctrl+F6`


## Notes

- You will need a multi-monitor setup. A virtual monitor will work, but will prove tricky. [This IDD one works](https://www.reddit.com/r/cloudygamer/comments/185agmk/guide_how_to_setup_hdr_with_moonlightsunshine/)
- Sunshine/Moonlight is compatible
- SteamVR may still complain about Direct Display mode, but this can be safely ignored
- Overlays generally won't work on this virtual HMD
- For most games, you will need to have the "spectator view" screen in focus for your inputs to register from mouse/keyboard/controller
- Recommend using a XInput controller
- This project is primarily targeted for VR mods of flatscreen games, not full VR games. As such, there is no headset movement or VR controller emulation
- OpenXR games/mods seem to be more likely to work and be stable than OpenVR ones
- HDR doesn't seem to work currently
- Several mods/games may override your supersample and other settings
- DLSS, TAA, and other temporal based settings often create a halo around objects. UEVR has a halo fix that lets you use TAA, but others may not
- Depth and Convergence are saved to your `Steam\config\steamvr.vrsettings` when SteamVR is closed. There are only global settings, no per-game ones.
- User Depth and Convergence Binds
	- The `num_user_settings` field must match the number of user defined configurations
	- Each configuration's Field Names should end with an integer, starting from 1
	- A Load key and a Store key can be configured to load and save Depth and Convergence settings for a configuration set
	- All User Depth and Convergence settings will be saved to `Steam\config\steamvr.vrsettings` when SteamVR is closed
	- If a User Depth and Convergence setting is in `Steam\config\steamvr.vrsettings` then it will override `Steam\steamapps\common\SteamVR\drivers\vrto3d\resources\settings\default.vrsettings`


## Building

- Clone the code and initialize submodules
- Define `STEAM_PATH` environment variable with the path to your main Steam folder
- Open Solution in Visual Studio 2022
- Use the solution to build this driver
- Build output is automatically copied to your `SteamVR\drivers` folder