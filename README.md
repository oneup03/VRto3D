# VRto3D

- OpenVR Driver that can render in SbS or TaB 3D with other formats converted to through ReShade
- Compatible games play great with a XInput controller. No motion controls required!
- Currently targeting OpenVR 2.5.1.
- Windows-only solution currently, but there are other solutions on Linux like MonadoVR.
- Check out the video guide here:

[![Video guide available here](https://img.youtube.com/vi/0caYbmcthkg/hqdefault.jpg)](https://www.youtube.com/watch?v=0caYbmcthkg)


## Compatible 3D Displays
- 3D TVs - work great if you can find one
- 3D Projectors - work great, but need more space and may be expensive
- AR Glasses (Rokid, Xreal, Viture) - work great, relatively inexpensive. If you don't have a USBC port with DP-Alt mode on your PC they require a [compatible adapter](https://docs.google.com/spreadsheets/d/15ub-YF9NU5KQ4r3UsiJlasdu6mH9fk_Xd-C37OcWQgc/edit?usp=sharing) - choose one with SBS and audio support. A [USBC extension](https://a.co/d/90y4CaY) is also recommended
- Lume Pad - works great, a bit more expensive, requires [Sunshine/Gamestream + Moonlight](https://support.leiainc.com/lume-pad-2/apps/moonlight3d)
- SR Displays (Acer Spatial Labs / Asus Spatial Vision / Samsung Odyssey 3D) - work great, currently expensive
- 3D Vision hardware (only RTX 20x or older) - will have game compatibility issues, hardware is hard to find
 

## Compatible VR Games & Mods
Checkout the [Compatibility List](https://github.com/oneup03/VRto3D/wiki/Compatibility-List) to see if a game has been tested


## Hotkeys
- Adjust Depth with `Ctrl + F3` and `Ctrl + F4`
- Adjust Convergence with `Ctrl + F5` and `Ctrl + F6`
- Save all Depth & Convergence settings (including ones temporarily set with the `user_store_keys`) and all hotkeys and pitch/yaw emulation settings as a profile for the currently running game with `Ctrl + F7` a beep will indicate success
- Reload the settings from `default.vrsettings` with `Ctrl + F10` a beep will indicate success
- Toggle locking the SteamVR Headset Window to the foreground with `Ctrl + F8`
- Toggle HMD Height between 0.1m and configured `hmd_height` using `Ctrl + F9`. This is useful for games that force a calibration on the "floor"
- Check the [Controls](#controls) section and the Configuration table below to setup HMD camera controls for VR games (check the compatibility list to see if they are needed) and Luke Ross mods (not for other mods)
- Check the [User Settings](#user-settings) section for instructions on setting up your own Depth and Convergence presets and also reference the Configuration table below


## Configuration

- Modify the `Steam\steamapps\common\SteamVR\drivers\vrto3d\resources\settings\default.vrsettings` for your setup
- Any changes made to this configuration require a restart of SteamVR to take effect
- Fields with a `+` next to them will be saved to a game's profile when you press `Ctrl + F7`
- If a game's profile exists in `Steam\config\steamvr.vrsettings` then it will override `default.vrsettings` you will hear a beep to indicate a profile loaded

| Field Name          | Type    | Description                                                                                 | Default Value  |
|---------------------|---------|---------------------------------------------------------------------------------------------|----------------|
| `window_width`      | `int`   | The width of the application window.                                                        | `1920`         |
| `window_height`     | `int`   | The height of the application window.                                                       | `1080`         |
| `render_width`      | `int`   | The width to render per eye (can be higher or lower than the application window)            | `1920`         |
| `render_height`     | `int`   | The height to render per eye (can be higher or lower than the application window)           | `1080`         |
| `hmd_height` +      | `float` | The height of the simulated HMD.                                                            | `1.0`          |
| `aspect_ratio`      | `float` | The aspect ratio used to calculate vertical FoV                                             | `1.77778`      |
| `fov`               | `float` | The field of view (FoV) for the VR rendering.                                               | `90.0`         |
| `depth` +           | `float` | The max depth. Overrides VR's IPD field.                                                    | `0.5`          |
| `convergence` +     | `float` | Where the left and right images converge. Adjusts frustum.                                  | `0.02`         |
| `disable_hotkeys`   | `bool`  | Disable Depth & Convergence adjustment hotkeys to avoid conflict with other 3D mods         | `false`        |
| `tab_enable`        | `bool`  | Enable or disable top-and-bottom (TaB) 3D output (Side by Side is default)                  | `false`        |
| `reverse_enable`    | `bool`  | Enable or disable reversed 3D output.                                                       | `false`        |
| `depth_gauge`       | `bool`  | Enable or disable SteamVR IPD depth gauge display.                                          | `false`        |
| `debug_enable`      | `bool`  | Borderless Windowed. Not 3DVision compatible. Breaks running some mods in OpenVR mode.      | `true`         |
| `display_latency`   | `float` | The display latency in seconds.                                                             | `0.011`        |
| `display_frequency` | `float` | The display refresh rate, in Hz.                                                            | `60.0`         |
| `pitch_enable` +    | `bool`  | Enables or disables Controller right stick y-axis mapped to HMD Pitch                       | `false`        |
| `yaw_enable` +      | `bool`  | Enables or disables Controller right stick x-axis mapped to HMD Yaw                         | `false`        |
| `pose_reset_key` +  | `string`| The [Virtual-Key Code](https://github.com/oneup03/VRto3D/blob/main/vrto3d/src/key_mappings.h) to reset the HMD position and orientation | `"VK_NUMPAD7"` |
| `ctrl_toggle_key` + | `string`| The Virtual-Key Code to toggle Pitch and Yaw emulation on/off when they are enabled         | `"XINPUT_GAMEPAD_RIGHT_THUMB"` |
| `pitch_radius` +    | `float` | Radius of curvature for the HMD to pitch along. Useful in 3rd person VR and RealVR games    | `0.0`          |
| `ctrl_deadzone` +   | `float` | Controller Deadzone when using pitch or yaw emulation                                       | `0.05`         |
| `ctrl_sensitivity` +| `float` | Controller Sensitivity when using pitch or yaw emulation                                    | `1.0`          |
| `num_user_settings` + | `int`   | The number of user settings defined below.                                                  | `3`            |
| `user_load_key#` +  | `string`| The Virtual-Key Code to load user setting # (replace # with integer number)                 | `"VK_NUMPAD1"` |
| `user_store_key#` + | `string`| The Virtual-Key Code to store user setting # (replace # with integer number)                | `"VK_NUMPAD4"` |
| `user_key_type#` +  | `string`| The store key's behavior ("switch" "toggle" "hold")  (replace # with integer number)        | `"switch"`     |
| `user_depth#` +     | `float` | The depth value for user setting # (replace # with integer number)                          | `0.5`          |
| `user_convergence#` + | `float` | The convergence value for user setting # (replace # with integer number)                    | `0.02`         |


## Base Installation

- A multi-display configuration setup in fullscreen mode will be the most compatible - see [notes](#displays) for working setups, but single displays can be used. Some mods or games may not work with a single display
- Install SteamVR
- Download the [latest release](https://github.com/oneup03/VRto3D/releases/latest) and copy the `vrto3d` folder to your `Steam\steamapps\common\SteamVR\drivers` folder
- Edit the `Steam\steamapps\common\SteamVR\drivers\vrto3d\resources\settings\default.vrsettings` as needed - [see what each setting does](#configuration)
    - Set your window resolution to match your fullscreen resolution (i.e. 3840x1080 for Full-SbS or 1920x1080 for Half-SbS)
    - Set your render resolution per eye to what you want - can save some performance by reducing this
    - Configure any `Virtual-Key Code` settings to use keys that you want (especially `user_load_keys` settings as these load a defined depth+convergence preset)
    - Single Display Mode: make sure the `debug_enable` flag is set to `true` to make more games work (not 3DVision compatible)
- Run SteamVR at least once to verify that you see a Headset window. This is usually not needed before running games.
    - The Headset window must be on your primary 3D display
    - Dismiss Headset Notice about `Enable Direct Display Mode` as this does nothing
- Try launching a VR game
- Multi-Display setups:
    - Move all windows besides the `Headset Window` over to your second display
        - Can use mouse to drag over
        - Can use Windows shortcut keys to move windowed programs around `Win + Left/Right`
        - Can use Windows shortcut keys to move fullscreen programs and the SteamVR Headset Window around `Shift + Win + Left/Right`
        - May need to make the game windowed either in-game settings or with `Alt + Enter`
    - If running SteamVR in fullscreen mode (not the default but can be set with debug_enable=false)
        - Click on the headset window to make it fullscreen on your primary display
        - If the Headset Window isn't fullscreen then you may get a black screen or some UI may not render in-game
        - AVOID using `Alt + Tab` as this is more likely to exit fullscreen
        - SteamVR Status will notify you if your headset window isn't fullscreen. Click on the `Enable Fullscreen Mode` notice or the headset window again to fix it
    - Click on the game's window on your second display for control input to work
- Single Display setup:
    - Mouse controls will not be usable in single display mode as you will click on the headset window in the foreground and input will not register in-game.
    - Make the game run in windowed mode either in-game settings or with `Alt + Enter` This will alleviate controller input and fullscreen issues
    - Make the SteamVR Headset Window in focus on your display
    - Press `Ctrl + F8` to toggle locking the headset window to the foreground
    - Use `Alt + Tab` to switch to the game window (has to be in focus for control input to work)
    - To quit, `Alt + Tab` to switch to the headset window and press `Ctrl + F8` to toggle the headset foregrounding off, and then `Alt + Tab` out


## Interlaced, Checkerboard, and Anaglyph Installation (only if you need this output format)

- Complete the [Base Installation](#base-installation) section
- Optionally set `tab_enable` to true in `Steam\steamapps\common\SteamVR\drivers\vrto3d\resources\settings\default.vrsettings` if you prefer to lose half vertical resolution instead of half horizontal resolution
    - If using interlaced mode, you want SbS for Column Interlaced and TaB for Row/Line Interlaced
- Download the latest [ReShade](https://reshade.me/#download) with full add-on support
- Run the ReShade installer
    - Browse to to your `Steam\steamapps\common\SteamVR\bin\win64` folder
    - Select `vrserver.exe` and click Next
    - Select `DirectX 11` and click Next
    - Click `Uncheck All` and click Next, Next, Finish
- Download [3DToElse.fx](https://github.com/BlueSkyDefender/Depth3D/blob/master/Other%20%20Shaders/3DToElse.fx) and save it to `Steam\steamapps\common\SteamVR\bin\win64\reshade-shaders\Shaders`
- Run SteamVR
- Press `Home` to open ReShade and click `Skip Tutorial`
- Select `To_Else` in the menu to enable 3DToElse
- Disable ReShade's `Performance Mode` checkbox
- Change 3DToElse settings:
    - Set `Stereoscopic Mode Input` to `Side by Side` (or `Top and Bottom` if you set `tab_enable` above)
    - Set `3D Display Mode` to the type needed for your display (even anaglyph)
    - `Eye Swap` can be toggled if needed
    - Don't touch `Perspective Slider`
- Enable ReShade's `Performance Mode` checkbox
- Once configuration is complete, you can run everything the same way as the Base Installation


## SR (Simulated Reality) Displays (only if you need this output format)

- When [XRGameBridge](https://github.com/JoeyAnthony/XRGameBridge/releases) is more stable, that will be preferable to use instead of VRto3D for games/mods with OpenXR support
- SR displays work in either Multi or Single Display environments
    - For both, read the Base Installation configuration and usage instructions to ensure that you get a proper 3D image and can control the game
- Complete the [Base Installation](#base-installation) section
- Install the software package provided with your SR display, if yours did not come with one, install the `SR-VERSION-win64.exe` and `simulatedreality-VERSION-win64-Release.exe` from the [LeiaInc Github](https://github.com/LeiaInc/leiainc.github.io/tree/master/SRSDK)
- Download the latest [ReShade](https://reshade.me/#download) with full add-on support
- Run the ReShade installer
    - Browse to to your `Steam\steamapps\common\SteamVR\bin\win64` folder
    - Select `vrserver.exe` and click Next
    - Select `DirectX 11` and click Next
    - Click `Uncheck All` and click Next
    - Select `3DGameBridge by Janthony & DinnerBram` and click Next
    - Click Finish
- Run SteamVR
- Press `Home` to open ReShade and click `Skip Tutorial`
- Click on the `Add-Ons` tab
- Select `srReshade` in the menu to enable it
    - Expand the srReshade dropdown and if you get a `Status: Inactive - Unable to load all SR DLLs` then you need to install the SR Runtime + SDK from Leia's Github above
    - 3D can be toggled on and off by using srReshade's `Ctrl + 2` hotkey
- Click on the `Home` tab
    - Enable ReShade's `Performance Mode` checkbox
- Once configuration is complete, you can run everything the same way as the Base Installation


## 3DVision Installation (only if you need this output format)

- This will be the worst experience due to the finicky nature of 3DVision drivers. It is highly recommended to get a different 3D Display to use moving forward.
- Only Driver v425.31 or 452.06 may work, so only RTX20 series or older
    - Some DX12 games are not compatible with these old drivers (crashes)
    - Having 3DVision enabled will crash DX12 games
    - Make sure your game runs on old drivers with 3D disabled before attempting to get it working with VRto3D
    - If you get a black screen while trying to run SteamVR + 3DVision, you may have to hard reset
    - Only Multi-Display setups will work due to 3DVision needing to always be fullscreen to activate
    - If you run into one-eye issues or other weirdness, try using DDU and reinstalling the driver
- Complete the [Base Installation](#base-installation) section
- If you want full resolution per eye, enable DSR 4x in Nvidia Control Panel -> Manage 3D Settings
- Modify the `window_width` and `window_height` in `Steam\steamapps\common\SteamVR\drivers\vrto3d\resources\settings\default.vrsettings` to match your fullscreen resolution
    - This should match your DSR 4x resolution if you're using DSR
- Modify the `render_width` and `render_height` to be the resolution you want per eye, can be lower or higher than the window setting
    - Recommended to set this to your monitors normal non-DSR resolution to avoid the performance hit
- Set the `debug_enable` flag to `false` to enable fullscreen mode ***3D will not work if you miss this setting***
- Download Bo3b's [SbS to 3DVision](https://bo3b.s3.amazonaws.com/SD3D_eng.7z) tool and extract the contents to your `Steam\steamapps\common\SteamVR\bin\win64` folder
- Enable 3D and Global hack. [3D Fix Manager](https://helixmod.blogspot.com/2017/05/3d-fix-manager.html) can do this
- Run SteamVR from Steam - you will have to do this before running any game with a 3DVision setup
- This configuration step should only need to be done once:
    - Press `Home` to bring up the ReShade menu and select the SBS `SBS_to_Double.fx` shader and click `Reload`
    - Enable ReShade's `Performance Mode` checkbox
    - The Headset window has to be in focus for 3DVision to trigger - try clicking on it
    - May need to press `Ctrl + T` to get 3D to trigger
    - If it's still not working, try closing SteamVR and trying again
    - If 3D flickers on and off, try toggling 3D in Nvidia Control Panel
- Disable 3D from Nvidia Control Panel (This may not be needed if you are running a VR-native game)
- Run your Game
- Move Game window to your second display
- If needed, inject VR mod
- Make SteamVR Headset window fullscreen on main display by clicking on it
    - If you get complaints about running in a non-stereo mode, Enable 3D in Nvidia Control Panel again
    - SteamVR Status will notify you if your headset window isn't fullscreen. Click on the `Enable Fullscreen Mode` notice or the headset window again to fix it
- Click back on the game window on second display and hopefully input works and 3D is still displaying
    - Changing the game window to windowed mode may help to make sure it doesn't trigger a go-fullscreen event and thus disable 3D on your 3D display


## Notes
- The primary display will be where the "Headset" window is located and should be 3D capable
- The game's main window has to be in focus for control input from your mouse/keyboard/controller to work
- SteamVR may still complain about Direct Display mode, but this can be safely dismissed
- Exiting SteamVR will "restart" Steam - this is normal
- Overlays generally won't work on this virtual HMD
- XInput controller is recommended (required for Single-Display mode)
- SteamVR doesn't support HDR currently
- Some mods/games may override your VR settings
- DLSS, TAA, and other temporal based settings often create a halo around objects. UEVR has a halo fix that lets you use TAA, but others may not

#### Controls
- This project is primarily targeted for VR mods of flatscreen games, not full VR games. As such, there is only HMD pitch & yaw emulation and no VR controller emulation
- Several VR controller only games can be made to work by using [Driver4VR](https://www.driver4vr.com/), a paid SteamVR Vive controller emulator. Games with mainly pointer controls work ok. Games with a lot of interaction/movement don't work well.
- Optional HMD `pitch_enable` and `yaw_enable` emulation can be turned on to help with games or mods that need it (maps to XInput right stick)
    - The `ctrl_toggle_key` can be set and used to toggle these settings on/off in-game (only functions if `pitch_enable` and/or `yaw_enable` is set to true)
    - The `pose_reset_key` can be set to allow resetting the view to the original position and orientation
    - Both of these keys can be set to XInput buttons & combinations or single keyboard keys as outlined in User Settings - Load Keys
    - The `pitch_radius` can be set to make the pitch emulation move along a semicircle instead of just tilting up/down in place

#### User Settings
- Depth and Convergence (including ones temporarily set with the `user_store_keys`) are saved to your `Steam\config\steamvr.vrsettings` when you press `Ctrl+F7`. There are only global settings, no per-game ones.
- If you swap between different convergence settings in-game, sometimes you will end up with black bars on the sides of the screen. So if you experience black bars, find a setting you like, save it, and then restart the game
- It is recommended to use a single convergence setting for all your presets
- The `num_user_settings` field in the default.vrsettings must match the number of user defined configurations - as many as you want
- Each configuration's Field Names should end with an integer, starting from 1
- A Load key and a Store key can be configured to load and save Depth and Convergence settings for a configuration set
    - Load keys can use XInput buttons & combinations as well as single keyboard keys
        - The Guide button can be used, but not in combinations
        - XInput Combinations can be set like this `"XINPUT_GAMEPAD_A+XINPUT_GAMEPAD_B"`
    - Store keys can only use single keyboard keys
    - Reference [Virtual-Key Codes](https://github.com/oneup03/VRto3D/blob/main/vrto3d/src/key_mappings.h) to find the strings to use for the keys
- The Load key can be configured to `"switch"` to the user depth & convergence setting, `"toggle"` between the user and current every 1.5s, or `"hold"` the user setting until the key is released
- The Store key will update your user Depth and Convergence setting to the current value
- If a User Depth and Convergence setting is in `Steam\config\steamvr.vrsettings` then it will override `Steam\steamapps\common\SteamVR\drivers\vrto3d\resources\settings\default.vrsettings`

#### Displays
- Here are some example multi-display configurations that are confirmed to work:
    - A single display connected to your computer twice in extended mode - switch between the inputs on the monitor as needed to move windows around
    - Multiple displays connected in extended mode - easier to move things around and manage
    - A virtual monitor will work, but will prove tricky. [This IDD one works](https://www.reddit.com/r/cloudygamer/comments/185agmk/guide_how_to_setup_hdr_with_moonlightsunshine/)
        - Sunshine/Moonlight is compatible

#### Troubleshooting
- If you have used other SteamVR drivers that also create a virtual HMD, you will need to disable and/or uninstall them
    - Run SteamVR
    - On the SteamVR Status window, go to `Menu -> Settings`
    - Change to the `Startup / Shutdown` tab
    - Click `Manage Add-Ons`
    - Turn `Off` any virtual HMD drivers (ALVR, VRidge, OpenTrack, VCR, iVRy, etc)
    - if issues still arise, try a [Clean SteamVR Install](https://steamcommunity.com/app/250820/discussions/2/1640917625015598552/) and delete your `Steam\steamapps\common\SteamVR` folder
- If you have a VR headset and run into issues with this driver, here's some things to try:
    - Disconnect VR headset from computer
    - [Clean SteamVR Install](https://steamcommunity.com/app/250820/discussions/2/1640917625015598552/)
    - [Set SteamVR as OpenXR Runtime](https://www.vive.com/us/support/vs/category_howto/trouble-with-openxr-titles.html)


## Building

- Clone the code and initialize submodules
- Define `STEAM_PATH` environment variable with the path to your main Steam folder
- Open Solution in Visual Studio 2022
- Use the solution to build this driver
- Build output is automatically copied to your `SteamVR\drivers` folder
