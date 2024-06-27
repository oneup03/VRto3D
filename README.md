# VRto3D

OpenVR Driver that can render in SbS or TaB 3D.
Currently targeting OpenVR 2.5.1.
Windows-only solution, but there are other solutions on Linux like MonadoVR.


## Configuration

- Modify the `vrto3d\resources\settings\default.vrsettings` for your setup:

| Field Name          | Type    | Description                                                                                 | Default Value  |
|---------------------|---------|---------------------------------------------------------------------------------------------|----------------|
| `window_width`      | `int`   | The width of the application window.                                                        | `1920`         |
| `window_height`     | `int`   | The height of the application window.                                                       | `1080`         |
| `aspect_ratio`      | `float` | The aspect ratio used to calculate vertical FoV                                             | `1.77778`      |
| `fov`               | `float` | The field of view (FoV) for the VR rendering.                                               | `90.0`         |
| `depth`             | `float` | The max depth. Overrides VR's IPD field.                                                    | `0.5`          |
| `convergence`       | `float` | Where the left and right images converge. Adjusts frustum.                                  | `0.1`          |
| `tab_enable`        | `bool`  | Enable or disable top-and-bottom (TaB) 3D output (Side by Side is default)                  | `false`        |
| `half_enable`       | `bool`  | Enable or disable half SbS/TaB 3D output.                                                   | `true`         |
| `reverse_enable`    | `bool`  | Enable or disable reversed 3D output.                                                       | `false`        |
| `hdr_enable`        | `bool`  | Enable or disable HDR.                                                                      | `false`        |
| `depth_gauge`       | `bool`  | Enable or disable SteamVR IPD depth gauge display.                                          | `false`        |
| `display_latency`   | `float` | The display latency in seconds.                                                             | `0.011`        |
| `display_frequency` | `float` | The display refresh rate, in Hz.                                                            | `60.0`         |
| `ctrl_enable`       | `bool`  | Enables or disables Controller right stick y-axis mapped to HMD Pitch                       | `false`        |
| `ctrl_deadzone`     | `float` | Controller Deadzone                                                                         | `0.05`         |
| `ctrl_sensitivity`  | `float` | Controller Sensitivity                                                                      | `1.0`          |
| `num_user_settings` | `int`   | The number of user settings defined below.                                                  | `3`            |
| `user_load_key#`    | `string`| The [Virtual-Key Code](https://github.com/oneup03/VRto3D/blob/main/vrto3d/src/key_mappings.h) to load user setting # (replace # with integer number)                | `"VK_NUMPAD1"` |
| `user_store_key#`   | `string`| The Virtual-Key Code to store user setting # (replace # with integer number)                | `"VK_NUMPAD4"` |
| `user_key_type#`    | `string`| The store key's behavior ("switch" "toggle" "hold")                                         | `"switch"`     |
| `user_depth#`       | `float` | The depth value for user setting # (replace # with integer number)                          | `0.5`          |
| `user_convergence#` | `float` | The convergence value for user setting # (replace # with integer number)                    | `0.1`          |


## Installation

- Get a multi-display configuration setup (see notes)
- Install SteamVR
- Download the latest release and copy the `vrto3d` folder to your `Steam\steamapps\common\SteamVR\drivers` folder
- Edit the `Steam\steamapps\common\SteamVR\drivers\vrto3d\resources\settings\default.vrsettings` as needed
- Run SteamVR at least once to verify that you see a Headset window. This is usually not needed before running games.
- Try launching a VR game
- Drag everything besides the headset view to your second display
- Make the game's window in focus on your second display for control input to work
- Adjust Depth with `Ctrl+F3` and `Ctrl+F4`
- Adjust Convergence with `Ctrl+F5` and `Ctrl+F6`
- Save all Depth & Convergence settings with `Ctrl+F7`


## Notes

- You will need a multi-display setup in extended mode
- The primary display will be where the "Headset" window is located
- The secondary display will need to have the game's main window in focus for control input from your mouse/keyboard/controller to work
- Here are some example configurations that are confirmed to work:
	- A single display connected to your computer twice in extended mode - switch between the inputs on the monitor as needed to move windows around
	- Multiple displays connected in extended mode - easier to move things around and manage
	- A virtual monitor will work, but will prove tricky. [This IDD one works](https://www.reddit.com/r/cloudygamer/comments/185agmk/guide_how_to_setup_hdr_with_moonlightsunshine/)
		- Sunshine/Moonlight is compatible
- Use Windows shortcut keys to move windowed programs around `Win + Left/Right Keys`
- Use Windows shortcut keys to move fullscreen programs around `Shift + Win + Left/Right`
- SteamVR may still complain about Direct Display mode, but this can be safely ignored
- Overlays generally won't work on this virtual HMD
- Recommend using a XInput controller
- This project is primarily targeted for VR mods of flatscreen games, not full VR games. As such, there is only headset pitch emulation and no VR controller emulation
- OpenXR games/mods seem to be more likely to work and be stable than OpenVR ones
	- Select the OpenXR toggle in UEVR GUI
	- Delete openvr_api.dll for REFramework
- Optional HMD pitch emulation can be turned on to help with games or mods that prevent you from adjusting the game camera's pitch with a controller/mouse (maps to XInput right stick Y-axis)
	- REFramework lua files can be modified to remove the pitch lock. Search for `Stop the player from rotating the camera vertically` and remove the block of code from the `if` to its `end`
- HDR doesn't seem to work currently
- Several mods/games may override your settings
- DLSS, TAA, and other temporal based settings often create a halo around objects. UEVR has a halo fix that lets you use TAA, but others may not
- Depth and Convergence are saved to your `Steam\config\steamvr.vrsettings` when you press `Ctrl+F7`. There are only global settings, no per-game ones.
- User Depth and Convergence Binds
	- The `num_user_settings` field must match the number of user defined configurations - as many as you want
	- Each configuration's Field Names should end with an integer, starting from 1
	- A Load key and a Store key can be configured to load and save Depth and Convergence settings for a configuration set
	- The Load key can be configured to `"switch"` to the user depth & convergence setting, `"toggle"` between the user and current every 1.5s, or `"hold"` the user setting until the key is released
	- The Store key will update your user Depth and Convergence setting to the current value
	- Changed values are saved to `Steam\config\steamvr.vrsettings` if you press `Ctrl+F7`
	- If a User Depth and Convergence setting is in `Steam\config\steamvr.vrsettings` then it will override `Steam\steamapps\common\SteamVR\drivers\vrto3d\resources\settings\default.vrsettings`


## Building

- Clone the code and initialize submodules
- Define `STEAM_PATH` environment variable with the path to your main Steam folder
- Open Solution in Visual Studio 2022
- Use the solution to build this driver
- Build output is automatically copied to your `SteamVR\drivers` folder