# VRto3D

OpenVR Driver that can render in SbS or TaB 3D.
Currently targeting OpenVR 2.5.1.
Windows-only solution, but there are other solutions on Linux like MonadoVR.


## Configuration

- Modify the `vrto3d\resources\settings\default.vrsettings` for your setup:

| Field Name          | Type    | Description                                                                                 | Default Value |
|---------------------|---------|---------------------------------------------------------------------------------------------|---------------|
| `window_x`          | `int`   | The X position of the window - can be used to move view to another display                  | `0`           |
| `window_y`          | `int`   | The Y position of the window - can be used to move view to another display                  | `0`           |
| `window_width`      | `int`   | The width of the application window.                                                        | `1920`        |
| `window_height`     | `int`   | The height of the application window.                                                       | `1080`        |
| `aspect_ratio`      | `float` | The aspect ratio used to calculate vertical FoV                                             | `1.77778`     |
| `fov`               | `float` | The field of view (FoV) for the VR rendering.                                               | `90.0`        |
| `depth`             | `float` | The max depth. Overrides VR's IPD field.                                                    | `0.5`         |
| `convergence`       | `float` | Where the left and right images converge. Adjusts frustum.                                  | `0.1`         |
| `tab_enable`        | `bool`  | Enable or disable top-and-bottom (TaB) 3D output (Side by Side is default)                  | `false`       |
| `half_enable`       | `bool`  | Enable or disable half SbS/TaB 3D output.                                                   | `true`        |
| `reverse_enable`    | `bool`  | Flip Left and Right eyes.                                                                   | `false`       |
| `ss_enable`         | `bool`  | Enable or disable supersampling.                                                            | `false`       |
| `hdr_enable`        | `bool`  | Enable or disable HDR rendering.                                                            | `false`       |
| `ss_scale`          | `float` | The supersampling scale factor.                                                             | `1.0`         |
| `display_latency`   | `float` | The latency from V-Sync to photons hitting the display, in seconds.                         | `0.011`       |
| `display_frequency` | `float` | The display refresh rate, in Hz.                                                            | `60.0`        |


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


## Building

- Clone the code and initialize submodules
- Define `STEAM_PATH` environment variable with the path to your main Steam folder
- Open Solution in Visual Studio 2022
- Use the solution to build this driver
- Copy from the `output` folder to the `SteamVR` folder