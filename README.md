# VRto3D

OpenVR Driver that can render in SbS or TaB 3D.
Currently targeting OpenVR 2.5.1.
Windows-only solution currently, but there are other solutions on Linux like MonadoVR.


## Compatible 3D Displays
- 3D TVs - work great if you can find one
- 3D Projectors - work great, but need more space and may be expensive
- AR Glasses (Rokid, Xreal, Viture) - work great, relatively inexpensive
- Lume Pad - works great, a bit more expensive, requires Sunshine/Gamestream + Moonlight
- 3D Vision hardware (only RTX 20x or older) - will have game compatibility issues, hardware is hard to find


## Configuration

- Modify the `vrto3d\resources\settings\default.vrsettings` for your setup:

| Field Name          | Type    | Description                                                                                 | Default Value  |
|---------------------|---------|---------------------------------------------------------------------------------------------|----------------|
| `window_width`      | `int`   | The width of the application window.                                                        | `1920`         |
| `window_height`     | `int`   | The height of the application window.                                                       | `1080`         |
| `hmd_height`        | `float` | The height of the simulated HMD.                                                            | `1.0`          |
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
| `pitch_enable`      | `bool`  | Enables or disables Controller right stick y-axis mapped to HMD Pitch                       | `false`        |
| `yaw_enable`        | `bool`  | Enables or disables Controller right stick x-axis mapped to HMD Yaw                         | `false`        |
| `pose_reset_key`    | `string`| The [Virtual-Key Code](https://github.com/oneup03/VRto3D/blob/main/vrto3d/src/key_mappings.h) to reset the HMD position and orientation | `"VK_NUMPAD7"` |
| `ctrl_toggle_key`   | `string`| The Virtual-Key Code to toggle Pitch and Yaw emulation on/off when they are enabled         | `"XINPUT_GAMEPAD_RIGHT_THUMB"` |
| `pitch_radius`      | `float` | Radius of curvature for the HMD to move along. Useful in 3rd person games                   | `0.0`          |
| `ctrl_deadzone`     | `float` | Controller Deadzone                                                                         | `0.05`         |
| `ctrl_sensitivity`  | `float` | Controller Sensitivity                                                                      | `1.0`          |
| `num_user_settings` | `int`   | The number of user settings defined below.                                                  | `3`            |
| `user_load_key#`    | `string`| The Virtual-Key Code to load user setting # (replace # with integer number)                 | `"VK_NUMPAD1"` |
| `user_store_key#`   | `string`| The Virtual-Key Code to store user setting # (replace # with integer number)                | `"VK_NUMPAD4"` |
| `user_key_type#`    | `string`| The store key's behavior ("switch" "toggle" "hold")                                         | `"switch"`     |
| `user_depth#`       | `float` | The depth value for user setting # (replace # with integer number)                          | `0.5`          |
| `user_convergence#` | `float` | The convergence value for user setting # (replace # with integer number)                    | `0.1`          |


## Base Installation

- Get a multi-display configuration setup (see notes)
- Install SteamVR
- Download the [latest release](https://github.com/oneup03/VRto3D/releases/latest) and copy the `vrto3d` folder to your `Steam\steamapps\common\SteamVR\drivers` folder
- Edit the `Steam\steamapps\common\SteamVR\drivers\vrto3d\resources\settings\default.vrsettings` as needed
- Run SteamVR at least once to verify that you see a Headset window. This is usually not needed before running games.
	- The Headset window must be on your primary 3D display
- Try launching a VR game
- Drag everything besides the headset view to your second display
- Make the game's window in focus on your second display for control input to work
- Adjust Depth with `Ctrl+F3` and `Ctrl+F4`
- Adjust Convergence with `Ctrl+F5` and `Ctrl+F6`
- Save all Depth & Convergence settings with `Ctrl+F7`


## Interlaced, Checkerboard, and Anaglyph Installation

- Complete the Base Installation section
- Optionally set `tab_enable` to true in `Steam\steamapps\common\SteamVR\drivers\vrto3d\resources\settings\default.vrsettings` if you prefer to lose half vertical resolution instead of half horizontal resolution
	- If using interlaced mode, you want SbS for Column Interlaced and TaB for Row Interlaced
- Download the latest [ReShade](https://reshade.me/#download) with full add-on support
- Run the ReShade installer
	- Browse to to your `Steam\steamapps\common\SteamVR\bin\win64` folder
	- Select `vrserver.exe`
	- Select DirectX 11
	- Click `Uncheck All` and click Next, Next, Finish
- Download [3DToElse.fx](https://github.com/BlueSkyDefender/Depth3D/tree/master/Other%20%20Shaders) and save it to `Steam\steamapps\common\SteamVR\bin\win64\reshade-shaders\Shaders`
- Run SteamVR
- Press `Home` to open ReShade and click `Skip Tutorial`
- Select `To_Else` in the menu to enable 3DToElse
- Change 3DToElse settings:
	- Set `Stereoscopic Mode Input` to `Side by Side` (or `Top and Bottom` if you set `tab_enable` above)
	- Set `3D Display Mode` to the type needed for your display (even anaglyph)
	- `Eye Swap` can be toggled if needed
	- Don't touch `Perspective Slider`
- Once configuration is complete, you can run everything the same way as the Base Installation


## 3DVision Installation

- This will be the worst experience due to the finicky nature of 3DVision drivers. It is highly recommended to buy a different 3D Display to use moving forward. AR glasses (Rokid, Xreal, Viture) all work and provide a better experience than 3DVision ever did. AR glasses will need a [compatible adapter](https://air.msmithdev.com/adapters/) if you don't have a USBC port on your computer with DP out.
- Only Driver v425.31 or 452.06 may work, so only RTX20 series or older
	- Many DX12 games are not compatible with these old drivers (crashes)
	- Having 3DVision enabled will crash DX12 games
	- Make sure your game runs on old drivers with 3D disabled before attempting to get it working with VRto3D
- Complete the Base Installation section
- Modify the window_width and window_height in `Steam\steamapps\common\SteamVR\drivers\vrto3d\resources\settings\default.vrsettings` to match your fullscreen resolution
- Download and install [SpecialK](https://sk-data.special-k.info/SpecialK.exe)
- Under SteamVR Properties, set Launch Options to `SKIF %COMMAND%`
- Download Bo3b's [SbS to 3DVision](https://bo3b.s3.amazonaws.com/SD3D_eng.7z) tool and extract the contents to your `Steam\steamapps\common\SteamVR\bin\win64` folder
- Enable 3D and Global hack. [3D Fix Manager](https://helixmod.blogspot.com/2017/05/3d-fix-manager.html) can do this
- Run SteamVR from Steam - you will have to do this before running any game with a 3DVision setup
- Open SpecialK's menu `Ctrl+Shift+Backspace`
	- Under Direct3D 11 Settings / SwapChain Management ensure that `Use Flip Model Presentation` is checked
	- Under On Screen Display, uncheck `Show Startup Banner`
	- It may be necessary to toggle between `Windowed Mode` and `Fullscreen Mode` using SpecialK under the Menubar / Display to get 3DVision to trigger
- Press `Home` to bring up the ReShade menu and select the SBS `SBS_to_Double.fx` shader and click `Reload`
	- May need to press `Ctrl+T` to get 3D to trigger
	- If 3D isn't triggering now, disable SBS, Reload, open SpecialK, toggle fullscreen, close SpecialK, enable SBS, reload
	- If it's still not working, try closing SteamVR and trying again
- Disable 3D (SteamVR will still be running in 3D, but any newly launched apps won't) (This may not be needed if you are running a VR-native game)
- Run your Game
- Move Game window to your second display
- If needed, inject VR mod
- Bring SteamVR Headset window into focus on main display
- Switch back to the game window on second display and hopefully input works and 3D is still displaying


## Notes

- If you have a VR headset and run into issues with this driver, here's some things to try:
	- Disconnect VR headset from computer
	- [Clean SteamVR Install](https://steamcommunity.com/app/250820/discussions/2/1640917625015598552/)
	- [Set SteamVR as OpenXR Runtime](https://www.vive.com/us/support/vs/category_howto/trouble-with-openxr-titles.html)
- This project is primarily targeted for VR mods of flatscreen games, not full VR games. As such, there is only headset pitch & yaw emulation and no VR controller emulation
- Several VR controller only games can be made to work by using [Driver4VR](https://www.driver4vr.com/), a paid SteamVR Vive controller emulator. Games with mainly pointer controls work ok. Games with a lot of interaction/movement don't work well.
- Check the [Compatibility List](https://github.com/oneup03/VRto3D/wiki/Compatibility-List) to see if a game has been tested
- You will need a multi-display setup in extended mode
- The primary display will be where the "Headset" window is located and should be 3D capable
- The secondary display will need to have the game's main window in focus for control input from your mouse/keyboard/controller to work
- Here are some example configurations that are confirmed to work:
	- A single display connected to your computer twice in extended mode - switch between the inputs on the monitor as needed to move windows around
	- Multiple displays connected in extended mode - easier to move things around and manage
	- A virtual monitor will work, but will prove tricky. [This IDD one works](https://www.reddit.com/r/cloudygamer/comments/185agmk/guide_how_to_setup_hdr_with_moonlightsunshine/)
		- Sunshine/Moonlight is compatible
- Use Windows shortcut keys to move windowed programs around `Win + Left/Right Keys`
- Use Windows shortcut keys to move fullscreen programs around `Shift + Win + Left/Right`
- SteamVR may still complain about Direct Display mode, but this can be safely ignored
- Exiting SteamVR will "restart" Steam - this is normal
- Overlays generally won't work on this virtual HMD
- Recommend using a XInput controller
- OpenXR games/mods seem to be more likely to work and be stable than OpenVR ones
- Optional HMD pitch and yaw emulation can be turned on to help with games or mods that need it (maps to XInput right stick)
	- The `ctrl_toggle_key` can be set and used to toggle these settings on/off in-game (only functions if `pitch_enable` and/or `yaw_enable` is set to true)
- HMD Height can be toggled between 0.1m and `hmd_height` using `Ctrl + F9`. This is useful for games that force a calibration on the "floor"
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
