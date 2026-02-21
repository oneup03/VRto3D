# VRto3D

- OpenVR Driver that can render in SbS or TaB 3D with other formats converted to through ReShade
- Compatible games play great with a XInput controller. No motion controls required!
- VRto3D itself does not "fix" games for 3D, but it allows you to run VR modded (fixed) games on a 3D Display
- Supports User Profiles for individual games
- Provides HMD Pitch and Yaw emulation for games that require it
- Currently targeting OpenVR 2.5.1.
- Windows-only solution currently, but there are other solutions on Linux like Monado XR.


## Compatible 3D Displays
- 3D TVs & Projectors - work great, use [Base Installation](#base-installation) in SbS/TaB mode or potentially [Frame Packing](#framepacking-hdmi-3d-only-if-you-need-this-output-format) instructions
- Passive/Interlaced 3D displays - work great, use [Interlaced](#interlaced-checkerboard-and-anaglyph-installation-only-if-you-need-this-output-format) instructions
- AR Glasses (Rokid, Xreal, Viture, RayNeo) - work great, use [Base Installation](#base-installation) instructions. If you don't have a USBC port with DP-Alt mode on your PC, they require a <a href="https://docs.google.com/spreadsheets/d/15ub-YF9NU5KQ4r3UsiJlasdu6mH9fk_Xd-C37OcWQgc/edit?usp=sharing" target="_blank" rel="noopener noreferrer">compatible adapter</a> - choose one with SBS and audio support. A <a href="https://a.co/d/90y4CaY" target="_blank" rel="noopener noreferrer">USBC extension</a> is also recommended. VertoXR can be used for 3DoF Head Tracking
- Lume Pad - works great, use [Base Installation](#base-installation) instructions, requires <a href="https://support.leiainc.com/lume-pad-2/apps/moonlight3d" target="_blank" rel="noopener noreferrer">Sunshine/Gamestream + Moonlight</a>
- SR Displays (Acer Spatial Labs / Asus Spatial Vision / Samsung Odyssey 3D) - work great, use [SR Displays](#sr-simulated-reality-displays-only-if-you-need-this-output-format) instructions
- 3D Vision/Frame Sequential - use [WibbleWobbleVR](https://oneup03.github.io/VRto3D/wiki/WibbleWobbleVR3.0) instead
- Virtual Desktop with a VR headset - works with [additional setup](https://oneup03.github.io/VRto3D/wiki/VirtualDesktop)
 

## Compatible VR Games & Mods
Checkout the [Compatibility List](https://oneup03.github.io/VRto3D/wiki/Compatibility-List) to see if a game has been tested


## Hotkeys
- Adjust Depth (Separation) with `Ctrl + F3` and `Ctrl + F4`
    - Synchronize the convergence setting by also holding `Shift` - this often has issues in VR mods
- Adjust Convergence with `Ctrl + F5` and `Ctrl + F6` - this often has issues in VR mods
- Save all current settings (ones indicated with a `"+"` under [Configuration](#configuration)) as a profile for the currently running game with `Ctrl + F7` A beep will indicate success
- Reload the profile settings (ones with a `"+"`) from the current game's `game.exe_config.json` with `Ctrl + F10` A beep will indicate success
- Reload the profile settings (ones with a `"+"`) from `default_config.json` with `Ctrl + Shift + F10` A beep will indicate success
- Toggle locking the SteamVR Headset Window to the foreground and focusing the game window with `Ctrl + F8`
- Adjust HMD position and yaw origin with `Ctrl + Home/End` for Y, `Ctrl + Delete/PageDown` for X, `Ctrl + Insert/PageUp` for Yaw, and `Ctrl + Shift + PageUp/PageDown` for Height
    - Save `hmd_height, hmd_x, hmd_y, hmd_yaw` using `Ctrl + F9`
    - This is useful if you want to align the HMD to a lighthouse tracked position
- Check the [Controls](#controls) section and the Configuration table below to setup HMD camera controls for VR games (check the compatibility list to see if they are needed)
- Check the [User Presets](#user-presets) section for instructions on setting up your own Depth/Separation and Convergence presets and also reference the Configuration table below
- When Pitch/Yaw emulation is enabled, you can adjust the ctrl_sensitivity with `Ctrl -` and `Ctrl +` and the pitch_radius with `Ctrl [` and `Ctrl ]`
- Toggle Auto Depth listener off/on with `Ctrl + F11` (only works with VR mods that support it)
- Attempt to take a SbS Screenshot with `Ctrl + F12` (doesn't always work)


## Configuration

- VRto3D has to be installed and SteamVR launched once for this config file to show up
- Modify the `Steam\config\vrto3d\default_config.json` for your setup
- Some changes made to this configuration require a restart of SteamVR to take effect
- Fields with a `"+"` next to them will be saved to a game's profile when you press `Ctrl + F7` and can be reloaded from either the game's profile using `Ctrl + F10` or the `default_config.json` using `Ctrl + Shift + F10`
- Reference <a href="https://github.com/oneup03/VRto3D/blob/main/vrto3d/src/key_mappings.h" target="_blank" rel="noopener noreferrer">Virtual-Key Code</a> strings for user hotkeys
- Reference [Profile Creation Steps](#profile-creation-steps) for creating a game-specific profile

| Field Name          | Type    | Description                                                                                 | Default Value  |
|---------------------|---------|---------------------------------------------------------------------------------------------|----------------|
| `display_index`     | `int`   | 3D Display number using Windows DISPLAY# (`0` = auto primary, `1` = DISPLAY1, etc.)         | `0`            |
| `render_width`      | `int`   | The width to render per eye                                                                 | `1920`         |
| `render_height`     | `int`   | The height to render per eye                                                                | `1080`         |
| `hmd_height` +      | `float` | The height/Z position origin of the simulated HMD                                           | `1.0`          |
| `hmd_x`             | `float` | The X position origin of the simulated HMD                                                  | `0.0`          |
| `hmd_y`             | `float` | The y position origin of the simulated HMD                                                  | `0.0`          |
| `hmd_yaw`           | `float` | The yaw attitude of the simulated HMD                                                       | `0.0`          |
| `aspect_ratio`      | `float` | The aspect ratio used to calculate vertical FoV                                             | `1.77778`      |
| `fov` +             | `float` | The horizontal field of view (FoV) for the VR rendering                                     | `90.0`         |
| `depth` +           | `float` | The max separation. Overrides VR's IPD field                                                | `0.1`          |
| `convergence` +     | `float` | Where the left and right images converge. Adjusts frustum                                   | `1.0`          |
| `async_enable` +    | `bool`  | Whether or not to use Asynchronous Reprojection. May improve or worsen smoothness           | `false`        |
| `disable_hotkeys`   | `bool`  | Disable Depth & Convergence adjustment hotkeys to avoid conflict with other 3D mods         | `false`        |
| `tab_enable`        | `bool`  | Enable or disable top-and-bottom (TaB/OU) 3D output (Side by Side is default)               | `false`        |
| `framepack_offset`  | `int`   | Pixel gap between left and right views in TaB mode. Use for framepacking/HDMI 3D            | `0`            |
| `reverse_enable`    | `bool`  | Enable or disable reversed 3D output                                                        | `false`        |
| `vd_fsbs_hack`      | `bool`  | Enable or disable half height Full-SbS for Virtual Desktop                                  | `false`        |
| `dash_enable`       | `bool`  | Enable or disable SteamVR Dashboard and Home                                                | `false`        |
| `auto_focus`        | `bool`  | Enable or disable automatic focusing/bringing VRto3D to foreground                          | `true`         |
| `display_latency`   | `float` | The display latency in seconds                                                              | `0.011`        |
| `display_frequency` | `float` | The display refresh rate per-eye, in Hz                                                     | `60.0`         |
| `pitch_enable` +    | `bool`  | Enables or disables Controller right stick y-axis mapped to HMD Pitch                       | `false`        |
| `yaw_enable` +      | `bool`  | Enables or disables Controller right stick x-axis mapped to HMD Yaw                         | `false`        |
| `use_open_track`    | `bool`  | Enables or disables OpenTrack 3DoF HMD Control                                              | `false`        |
| `open_track_port`   | `int`   | UDP Port for OpenTrack                                                                      | `4242`         |
| `launch_script`     | `string`| Command executed once when VRto3D driver activates (`"start vertoxr://steamvr"`)            | `""`           |
| `pose_reset_key` +  | `string`| The Virtual-Key Code to reset the HMD position and orientation                              | `"VK_NUMPAD7"` |
| `ctrl_toggle_key` + | `string`| The Virtual-Key Code to toggle Pitch and Yaw emulation on/off when they are enabled         | `"XINPUT_GAMEPAD_RIGHT_THUMB"` |
| `ctrl_toggle_type` +| `string`| The ctrl_toggle_key's behavior ("toggle" "hold")                                            | `"toggle"`     |
| `pitch_radius` +    | `float` | Radius of curvature for the HMD to pitch along. Useful in 3rd person VR games               | `0.0`          |
| `ctrl_deadzone` +   | `float` | Controller Deadzone when using pitch or yaw emulation                                       | `0.05`         |
| `ctrl_sensitivity` +| `float` | Controller Sensitivity when using pitch or yaw emulation                                    | `1.0`          |
| `user_load_key` +   | `string`| The Virtual-Key Code to load user preset                                                    | `"VK_NUMPAD1"` |
| `user_store_key` +  | `string`| The Virtual-Key Code to store user preset temporarily                                       | `"VK_NUMPAD4"` |
| `user_key_type` +   | `string`| The store key's behavior ("switch" "toggle" "hold")                                         | `"switch"`     |
| `user_depth` +      | `float` | The separation value for a user preset                                                      | `0.1`          |
| `user_convergence` +| `float` | The convergence value for a user preset                                                     | `1.0`          |
| `user_fov` +        | `float` | The fov value for a user preset (optional, will default to global fov)                      | `90.0`         |


## Base Installation

- Install <a href="https://store.steampowered.com/app/250820/SteamVR/" target="_blank" rel="noopener noreferrer">SteamVR</a>
- Download the [latest VRto3D release](https://github.com/oneup03/VRto3D/releases/latest/download/vrto3d.zip) and copy the `vrto3d` folder from inside the VRto3D.zip to your `Steam\steamapps\common\SteamVR\drivers` folder
- Launch SteamVR once to generate the `default_config.json` and you should see a 1080p SbS `Headset Window` upscaled to fullscreen
- <a href="https://www.vive.com/us/support/vs/category_howto/trouble-with-openxr-titles.html" target="_blank" rel="noopener noreferrer">Set SteamVR as OpenXR Runtime</a>
- Close SteamVR
- Edit the `Steam\config\vrto3d\default_config.json` as needed - [see what each setting does](#configuration)
    - Set `display_index` to your 3D display using Windows numbering (`DISPLAY1`, `DISPLAY2`, etc). Leave it as `0` to auto-use the current primary display
    - Set your render resolution per eye to what you want - can save some performance by reducing this. If your display is half-SbS or half-TaB, then you can try setting this to that half-resolution
    - Configure any `Virtual-Key Code` settings to use keys that you want (especially `user_load_keys` settings as these load a defined depth+convergence preset)
- Download the latest [VRto3D profiles](https://github.com/oneup03/VRto3D/releases/download/latest/vrto3d_profiles.zip) for games and extract them to your `Steam\config\vrto3d\` folder
- Run SteamVR to verify that you see the Headset window covering your entire display. This is usually not needed before running games.
    - The Headset window should appear on the configured `display_index` monitor
    - Dismiss Headset Notice about `Enable Direct Display Mode` as this does nothing
- Try launching a VR game
- Keyboard and Mouse are usable, but you may run into issues with accidentally clicking the wrong window or the cursor escaping the game window if the game's mouse control is coded poorly
    - Can try using <a href="https://github.com/James-LG/AutoCursorLock" target="_blank" rel="noopener noreferrer">AutoCursorLock</a> if the mouse keeps escaping
- Make the game run in windowed mode either in-game settings or with `Alt + Enter` This will alleviate controller input and fullscreen issues. (Borderless fullscreen/windowed sometimes also work)
- If needed, press `Ctrl + F8` to lock the 3D window to the foreground and focus the game window
    - This is automated by default with the `auto_focus` setting when a VRto3D profile exists for the game
- If game controls & audio aren't working, use `Alt + Tab` to switch to the game window
- To quit, exit the game and try to `Alt + Tab` out
    - If the 3D window remains in the foreground, press `Ctrl + F8` to toggle the foregrounding off, and then `Alt + Tab` out


## Interlaced, Checkerboard, and Anaglyph Installation (only if you need this output format)

- Complete the [Base Installation](#base-installation) section
- Optionally set `tab_enable` to true in `Steam\config\vrto3d\default_config.json` if you prefer to lose half vertical resolution instead of half horizontal resolution
    - If using interlaced mode, you want SbS for Column Interlaced and TaB for Row/Line Interlaced. Most interlaced displays should use TaB
- Download the latest <a href="https://reshade.me/#download" target="_blank" rel="noopener noreferrer">ReShade</a> with full add-on support
- Run the ReShade installer
    - Browse to to your `Steam\steamapps\common\SteamVR\bin\win64` folder
    - Select `vrserver.exe` and click Next
    - Select `DirectX 11` and click Next
    - Click `Uncheck All` and click Next, Next, Finish
- Download <a href="https://github.com/BlueSkyDefender/Depth3D/raw/refs/heads/master/Other%20%20Shaders/3DToElse.fx" download>3DToElse.fx</a> and save it to `Steam\steamapps\common\SteamVR\bin\win64\reshade-shaders\Shaders`
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
- If settings don't save, you may have to manually edit `Steam\steamapps\common\SteamVR\bin\win64\ReShade.ini` and disable Tutorial with `TutorialProgress=4`


## FramePacking, HDMI 3D (only if you need this output format)

- Complete the [Base Installation](#base-installation) section
- In `Steam\config\vrto3d\default_config.json` set these settings:
    - Set `display_index` to the 3D display where frame-packed output should appear
    - `tab_enable` to true
    - `framepack_offset` to `45` for 1920x2205 or `30` for 1280x1470 (this may vary by display)
- More instructions and discussion are in <a href="https://www.mtbs3d.com/phpbb/viewtopic.php?t=26494" target="_blank" rel="noopener noreferrer">this forum</a>
- Create one of these Custom Resolutions in Nvidia Control Panel or CRU:
- frame_packed_720p : resolution 1280x1470, 60Hz
    - horizontal: 1280 active; 110 front, 40 sync, 220 back (1650 total)
    - vertical: 1470 active; 5 front, 5 sync, 20 back (1500 total)
- frame_packed_1080p : resolution 1920x2205, 24Hz/60Hz
    - horizontal: 1920 active; 638 front, 44 sync, 148 back (2750 total)
    - vertical: 2205 active; 4 front, 5 sync, 36 back (2250 total)
- May need to use CVT reduced blank specs for success with 1080 60Hz, but 60Hz is not standard and may not work at all
    - horizontal: 1920 active; 48 front, 32 sync, 80 back (2080 total)
    - vertical: 2205 active; 4 front, 5 sync, 36 back (2250 total)
- It may be necessary to remove other resolutions with CRU to avoid games changing the resolution. Hopefully running them in windowed mode (required for VRto3D) will prevent issues though


## SR (Simulated Reality) Displays (only if you need this output format)

- Complete the [Base Installation](#base-installation) section
- If your display supports higher refresh rates than 60hz, you can optionally set `display_frequency` to match in `Steam\config\vrto3d\default_config.json`
- Install the software package provided with your SR display (Samsung Odyssey 3D Hub or Acer TrueGame)
- Download the latest <a href="https://reshade.me/#download" target="_blank" rel="noopener noreferrer">ReShade</a> with full add-on support
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
    - Expand the srReshade dropdown and if you get a `Status: Inactive - Unable to load all SR DLLs` then you may need to do these additional steps:
        - Open Windows Run with `Win + R`
        - Paste this command: `cmd /k setx PATH "C:\Program Files\LeiaSR\Platform\bin;%PATH%"`
        - Exit the terminal and reboot
    - 3D can be toggled on and off by using srReshade's `Ctrl + 2` hotkey
- Click on the `Home` tab
    - Enable ReShade's `Performance Mode` checkbox
- Once configuration is complete, you can run everything the same way as the Base Installation
- If settings don't save, you may have to manually edit `Steam\steamapps\common\SteamVR\bin\win64\ReShade.ini` and disable Tutorial with `TutorialProgress=4`
- If you experience a super dark screen, try enabling a random ReShade shader


## Frame Sequential (3DVision)
- Use [WibbleWobbleVR](https://oneup03.github.io/VRto3D/wiki/WibbleWobbleVR3.0) instead - it has support for VRto3D profiles


## Notes
- The `Headset` window is placed on the monitor selected by `display_index` (`0` means auto primary)
- `display_index` uses Windows DISPLAY numbering (`DISPLAY1`, `DISPLAY2`, ...)
- The game's main window has to be in focus for control input from your mouse/keyboard/controller to work
- SteamVR may still complain about Direct Display mode, but this can be safely dismissed
- Exiting SteamVR may "restart" Steam - this is normal
- Overlays generally won't work on this virtual HMD
- XInput controller is recommended
- SteamVR doesn't support HDR currently
    - AutoHDR may work, but some games will be too dark or too bright
- DLSS, TAA, and other temporal based settings can create a halo around objects. Most VR mods have fixes for this, but some may not

#### Controls
- If you want to use Steam Input
    - Open Steam->Settings->Controller
    - Toggle on `Enable Steam Input for Xbox Controllers`
    - Click `Edit` on the `Desktop Layout` and then select `Disable Steam Input`
    - On SteamVR's library page, click the `Controller Icon` and select `Disable Steam Input`
    - Generally you need to start SteamVR first and separately from the game for Steam Input to work
- This project is primarily targeted for VR mods of flatscreen games, not full VR games. As such, there is only HMD pitch & yaw emulation and no VR controller emulation
- <a href="https://vertoxr.com/" target="_blank" rel="noopener noreferrer">VertoXR</a> can be paired with VRto3D to provide 3DoF Head Tracking for popular AR glasses
    - Connect AR glasses, switch to Full-SbS mode, and set `display_index` to the glasses' Windows DISPLAY number
    - Open VertoXR, connect to the AR glasses
    - Select `Game Mode`, place glasses on a flat surface looking straight ahead, and click `Start Calibrate`. Calibration may need to be redone if misalignment occurs
    - Edit `OpenTrack Configuration` and disable `Enable Roll` if needed
    - `Start` the OpenTrack VertoXR plugin
    - Set `use_open_track` to true and ensure `open_track_port` matches the VertoXR OpenTrack port in `default_config.json`
    - Once everything is setup, you can set `launch_script` to `"start vertoxr://steamvr"` and VertoXR will be auto started with Open Track active every time you start SteamVR
- You can setup a Lighthouse + Vive Tracker + tracked controllers with VRto3D for a seated play area. See [this guide](https://oneup03.github.io/VRto3D/wiki/Motion-Controls-&-Tracking) for details
- Several VR controller only games can be made to work by using <a href="https://www.driver4vr.com/" target="_blank" rel="noopener noreferrer">Driver4VR</a>, a paid SteamVR Vive controller emulator. Games with mainly pointer controls work ok. Games with a lot of interaction/movement don't work well.
- Optional HMD `pitch_enable` and `yaw_enable` emulation can be turned on to help with games or mods that need it (maps to XInput right stick)
    - Reference <a href="https://github.com/oneup03/VRto3D/blob/main/vrto3d/src/key_mappings.h" target="_blank" rel="noopener noreferrer">Virtual-Key Code</a> to find the strings to use for these hotkeys
    - The `ctrl_toggle_key` can be set and used to toggle these settings on/off in-game (only functions if `pitch_enable` and/or `yaw_enable` is set to true)
    - The `ctrl_toggle_type` can be set to either `"toggle"` pitch/yaw on/off or `"hold"` that disables them while the button is held
    - The `pose_reset_key` can be set to allow resetting the view to the original position and orientation
    - Both of these keys can be set to XInput buttons & combinations or single keyboard/mouse keys as outlined in User Presets - Load Keys below
    - The `pitch_radius` can be set to make the pitch emulation move along a semicircle instead of just tilting up/down in place. Use the [Hotkeys](#hotkeys) to adjust this in-game
- OpenTrack 3DoF support is available over UDP loopback at the configured `open_track_port` when `use_open_track` is true. It can be used in combination with Pitch/Yaw emulation

#### User Presets
- If you swap between different convergence settings in-game, sometimes you will end up with black bars on the sides of the screen or you may not see a change immediately. If you reload/restart/reinitialize the VR mod, you should see the change
- It is recommended to use a single convergence setting for all your presets given the above issue with some VR mods
- Create any number of user depth/separation & convergence hotkeys in the `user_settings` area of the `default_config.json`
    - A user preset looks like this:
    - ```
        {
            "user_load_key": "VK_NUMPAD1",
            "user_store_key": "VK_NUMPAD4",
            "user_key_type": "switch",
            "user_depth": 0.1,
            "user_convergence": 1.0,
            "user_fov": 70.0
        },
      ```
- A Load key and a Store key can be configured to load and save Depth/Separation and Convergence settings for a preset
    - Load keys can use XInput buttons & combinations as well as single keyboard/mouse keys
        - The Guide button can be used, but not in combinations
        - XInput Combinations can be set like this `"XINPUT_GAMEPAD_A+XINPUT_GAMEPAD_B"`
    - Store keys can only use single keyboard/mouse keys
    - Reference <a href="https://github.com/oneup03/VRto3D/blob/main/vrto3d/src/key_mappings.h" target="_blank" rel="noopener noreferrer">Virtual-Key Code</a> to find the strings to use for these hotkeys
- The Load key can be configured to `"switch"` to the user depth/separation & convergence setting, `"toggle"` between the preset and the previous setting every 1.5s, or `"hold"` the user setting until the key is released
- The Store key will update your user Depth/Separation and Convergence setting to the current value (this only saves while the game is running - you need to create a game profile as detailed below to store it permanently)
- It is recommended to have a single user preset of `"switch"` type that matches the default depth/separation & convergence so you can easily get back to the default

#### Profile Creation Steps:
1. Modify or copy and create user preset(s) in `default_config.json` (or preferably in the `Game.exe_config.json` if one already exists) for the game you want to play
2. If applicable, modify `hmd_height, fov, pitch_enable, yaw_enable, pose_reset_key, ctrl_toggle_key, ctrl_toggle_type, pitch_radius, ctrl_deadzone, ctrl_sensitivity` for the game profile
3. If the game is already running, use `Ctrl + Shift + F10` to reload the `default_config.json` (or `Ctrl + F10` to reload the `Game.exe_config.json`) with your new settings and presets
4. Adjust depth/separation (`Ctrl + F3` and `Ctrl + F4` with `+ shift` if possible) & convergence (`Ctrl + F5` and `Ctrl + F6`) for a preset
5. Use the configured `user_store_key` to temporarily save the current depth/separation & convergence values to the preset
6. Repeat 4 & 5 for each preset you need
7. Adjust depth/separation & convergence back to what you want the default to be (if you have a default `"switch"` preset, you can use its configured `user_load_key`)
8. If applicable, adjust the `ctrl_sensitivity` with `Ctrl -` and `Ctrl +` and the `pitch_radius` with `Ctrl [` and `Ctrl ]`
9. Save the profile with `Ctrl + F7`
10. Open your new profile from `Steam\config\vrto3d\` in a text editor and make final adjustments like: making all the convergence values match to avoid rendering or performance issues, changing virtual-key mappings, or tweaking other values/settings
11. Close out of SteamVR and the game and restart the game. You should hear a loud beep to indicate the profile loaded. Test the profile and you can still make any adjustments per above instructions
12. Share your `Steam\config\vrto3d\Game.exe_config.json` with others

#### Troubleshooting
- If SteamVR appears on the wrong monitor, set `display_index` to the correct Windows DISPLAY number and restart SteamVR
- If SteamVR crashes and disables add-ons, you will need to re-enable VRto3D in the SteamVR Status window
- The first thing to try is deleting your `Steam\config\steamvr.vrsettings` and `Steam\config\vrto3d\default_config.json`
- If you have used other SteamVR drivers that also create a virtual HMD, you will need to disable and/or uninstall them
    - Run SteamVR
    - On the SteamVR Status window, go to `Menu -> Settings`
    - Change to the `Startup / Shutdown` tab
    - Click `Manage Add-Ons`
    - Turn `Off` any virtual HMD drivers (ALVR, VRidge, OpenTrack, VCR, iVRy, etc)
    - if issues still arise, try a <a href="https://steamcommunity.com/app/250820/discussions/2/1640917625015598552/" target="_blank" rel="noopener noreferrer">Clean SteamVR Install</a> and delete your `Steam\steamapps\common\SteamVR` folder
- Conversely, to uninstall or disable VRto3D, you can disable it under the same `Manage Add-Ons` menu or delete the `vrto3d` folder from your `Steam\steamapps\common\SteamVR\drivers` folder
- If you have a VR headset and run into issues with this driver, here's some things to try:
    - Disconnect VR headset from computer
    - <a href="https://steamcommunity.com/app/250820/discussions/2/1640917625015598552/" target="_blank" rel="noopener noreferrer">Clean SteamVR Install</a>
    - <a href="https://www.vive.com/us/support/vs/category_howto/trouble-with-openxr-titles.html" target="_blank" rel="noopener noreferrer">Set SteamVR as OpenXR Runtime</a>


## Building

- Clone the code and initialize submodules
- Define `STEAM_PATH` environment variable with the path to your main Steam folder
- Open Solution in Visual Studio 2022 or VSCode
- Use the solution to build this driver
- Build output is automatically copied to your `SteamVR\drivers` folder
