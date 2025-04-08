# VRto3D

- OpenVR Driver that can render in SbS or TaB 3D with other formats converted to through ReShade
- Compatible games play great with a XInput controller. No motion controls required!
- VRto3D itself does not "fix" games for 3D, but it allows you to run VR modded (fixed) games on a 3D Display
- Currently targeting OpenVR 2.5.1.
- Windows-only solution currently, but there are other solutions on Linux like Monado XR.
- Check out the video guide here (there are 2 parts):


<a href="https://www.youtube.com/watch?v=0caYbmcthkg" target="_blank" rel="noopener noreferrer">
  <img src="https://img.youtube.com/vi/0caYbmcthkg/hqdefault.jpg" alt="Video guide available here">
</a>


## Compatible 3D Displays
- 3D TVs - work great if you can find one
- 3D Projectors - work great, but need more space
- Passive 3D displays - work great, but hard to find
- AR Glasses (Rokid, Xreal, Viture, RayNeo) - work great, relatively inexpensive. If you don't have a USBC port with DP-Alt mode on your PC, they require a <a href="https://docs.google.com/spreadsheets/d/15ub-YF9NU5KQ4r3UsiJlasdu6mH9fk_Xd-C37OcWQgc/edit?usp=sharing" target="_blank" rel="noopener noreferrer">compatible adapter</a> - choose one with SBS and audio support. A <a href="https://a.co/d/90y4CaY" target="_blank" rel="noopener noreferrer">USBC extension</a> is also recommended
- Lume Pad - works great, a bit more expensive, requires <a href="https://support.leiainc.com/lume-pad-2/apps/moonlight3d" target="_blank" rel="noopener noreferrer">Sunshine/Gamestream + Moonlight</a>
- SR Displays (Acer Spatial Labs / Asus Spatial Vision / Samsung Odyssey 3D) - work great, currently expensive
- 3D Vision hardware works thanks to WibbleWobble
- Virtual Desktop with a VR headset - works with [additional setup](https://oneup03.github.io/VRto3D/wiki/VirtualDesktop)
 

## Compatible VR Games & Mods
Checkout the [Compatibility List](https://oneup03.github.io/VRto3D/wiki/Compatibility-List) to see if a game has been tested


## Hotkeys
- Adjust Depth (Separation) with `Ctrl + F3` and `Ctrl + F4`
- Adjust Convergence with `Ctrl + F5` and `Ctrl + F6`
- Save all current settings (ones indicated with a `"+"` under [Configuration](#configuration)) as a profile for the currently running game with `Ctrl + F7` A beep will indicate success
- Reload the profile settings (ones with a `"+"`) from `default_config.json` with `Ctrl + F10` A beep will indicate success
- Toggle locking the SteamVR Headset Window to the foreground with `Ctrl + F8`
- Toggle HMD Height between 0.1m and configured `hmd_height` using `Ctrl + F9`. This is useful for games that force a calibration on the "floor"
- Check the [Controls](#controls) section and the Configuration table below to setup HMD camera controls for VR games (check the compatibility list to see if they are needed)
- Check the [User Presets](#user-presets) section for instructions on setting up your own Depth/Separation and Convergence presets and also reference the Configuration table below
- When Pitch/Yaw emulation is enabled, you can adjust the ctrl_sensitivity with `Ctrl -` and `Ctrl +` and the pitch_radius with `Ctrl [` and `Ctrl ]`
- Toggle Auto Depth listener off/on with `Ctrl + F11`


## Configuration

- VRto3D has to be installed and SteamVR launched once for this config file to show up
- Modify the `Documents\My Games\vrto3d\default_config.json` for your setup
- Some changes made to this configuration require a restart of SteamVR to take effect
- Fields with a `"+"` next to them will be saved to a game's profile when you press `Ctrl + F7` and can be reloaded from `default_config.json` using `Ctrl + F10`
- Reference <a href="https://github.com/oneup03/VRto3D/blob/main/vrto3d/src/key_mappings.h" target="_blank" rel="noopener noreferrer">Virtual-Key Code</a> strings for user hotkeys
- Reference [Profile Creation Steps](#profile-creation-steps) for creating a game-specific profile

| Field Name          | Type    | Description                                                                                 | Default Value  |
|---------------------|---------|---------------------------------------------------------------------------------------------|----------------|
| `window_width`      | `int`   | The width of the application window.                                                        | `1920`         |
| `window_height`     | `int`   | The height of the application window.                                                       | `1080`         |
| `render_width`      | `int`   | The width to render per eye (can be higher or lower than the application window)            | `1920`         |
| `render_height`     | `int`   | The height to render per eye (can be higher or lower than the application window)           | `1080`         |
| `hmd_height` +      | `float` | The height of the simulated HMD.                                                            | `1.0`          |
| `aspect_ratio`      | `float` | The aspect ratio used to calculate vertical FoV                                             | `1.77778`      |
| `fov` +             | `float` | The horizontal field of view (FoV) for the VR rendering.                                    | `90.0`         |
| `depth` +           | `float` | The max separation. Overrides VR's IPD field.                                               | `0.4`          |
| `convergence` +     | `float` | Where the left and right images converge. Adjusts frustum.                                  | `4.0`          |
| `disable_hotkeys`   | `bool`  | Disable Depth & Convergence adjustment hotkeys to avoid conflict with other 3D mods         | `false`        |
| `tab_enable`        | `bool`  | Enable or disable top-and-bottom (TaB/OU) 3D output (Side by Side is default)               | `false`        |
| `reverse_enable`    | `bool`  | Enable or disable reversed 3D output.                                                       | `false`        |
| `depth_gauge`       | `bool`  | Enable or disable SteamVR IPD gauge display.                                                | `false`        |
| `dash_enable`       | `bool`  | Enable or disable SteamVR Dashboard and Home.                                               | `false`        |
| `display_latency`   | `float` | The display latency in seconds.                                                             | `0.011`        |
| `display_frequency` | `float` | The display refresh rate, in Hz.                                                            | `60.0`         |
| `pitch_enable` +    | `bool`  | Enables or disables Controller right stick y-axis mapped to HMD Pitch                       | `false`        |
| `yaw_enable` +      | `bool`  | Enables or disables Controller right stick x-axis mapped to HMD Yaw                         | `false`        |
| `use_open_track`    | `bool`  | Enables or disables OpenTrack 3DoF HMD Control                                              | `false`        |
| `open_track_port`   | `int`   | UDP Port for OpenTrack                                                                      | `4242`         |
| `pose_reset_key` +  | `string`| The Virtual-Key Code to reset the HMD position and orientation                              | `"VK_NUMPAD7"` |
| `ctrl_toggle_key` + | `string`| The Virtual-Key Code to toggle Pitch and Yaw emulation on/off when they are enabled         | `"XINPUT_GAMEPAD_RIGHT_THUMB"` |
| `ctrl_toggle_type` +| `string`| The ctrl_toggle_key's behavior ("toggle" "hold")                                            | `"toggle"`     |
| `pitch_radius` +    | `float` | Radius of curvature for the HMD to pitch along. Useful in 3rd person VR games               | `0.0`          |
| `ctrl_deadzone` +   | `float` | Controller Deadzone when using pitch or yaw emulation                                       | `0.05`         |
| `ctrl_sensitivity` +| `float` | Controller Sensitivity when using pitch or yaw emulation                                    | `1.0`          |
| `user_load_key` +   | `string`| The Virtual-Key Code to load user preset                                                    | `"VK_NUMPAD1"` |
| `user_store_key` +  | `string`| The Virtual-Key Code to store user preset temporarily                                       | `"VK_NUMPAD4"` |
| `user_key_type` +   | `string`| The store key's behavior ("switch" "toggle" "hold")                                         | `"switch"`     |
| `user_depth` +      | `float` | The separation value for a user preset                                                      | `0.4`          |
| `user_convergence` +| `float` | The convergence value for a user preset                                                     | `4.0`          |


## Base Installation

- A Single Display configuration is recommended, but you can also use a multi-display configuration if desired
- Install SteamVR
- If you want to use Steam Input
    - Open Steam->Settings->Controller
    - Toggle on `Enable Steam Input for Xbox Controllers`
    - Click `Edit` on the `Desktop Layout` and then select `Disable Steam Input`
    - On SteamVR's library page, click the `Controller Icon` and select `Disable Steam Input`
    - Generally you need to start SteamVR first and separately from the game for Steam Input to work
- Download the [latest release](https://github.com/oneup03/VRto3D/releases/latest/download/vrto3d.zip) and copy the `vrto3d` folder to your `Steam\steamapps\common\SteamVR\drivers` folder
- Launch SteamVR once to generate the `default_config.json` and you should see a 1080p SbS `Headset Window`
- Close SteamVR
- Edit the `Documents\My Games\vrto3d\default_config.json` as needed - [see what each setting does](#configuration)
    - Set your window resolution to match your fullscreen resolution (i.e. 3840x1080 for Full-SbS or 1920x1080 for Half-SbS)
    - Set your render resolution per eye to what you want - can save some performance by reducing this. If your display is half-SbS or half-TaB, then you can try setting this to that half-resolution
    - Configure any `Virtual-Key Code` settings to use keys that you want (especially `user_load_keys` settings as these load a defined depth+convergence preset)
- Download the latest [VRto3D profiles](https://github.com/oneup03/VRto3D/releases/download/latest/vrto3d_profiles.zip) for games and extract them to your `Documents\My Games\vrto3d\` folder
- Run SteamVR to verify that you see the Headset window covering your entire display. This is usually not needed before running games.
    - The Headset window must be on your primary 3D display
    - Dismiss Headset Notice about `Enable Direct Display Mode` as this does nothing
- Try launching a VR game
#### Single-Display Setup:
- Keyboard and Mouse are usable, but you may run into issues with accidentally clicking the wrong window or the cursor escaping the game window if the game's mouse control is coded poorly
    - Can try using <a href="https://github.com/James-LG/AutoCursorLock" target="_blank" rel="noopener noreferrer">AutoCursorLock</a> if the mouse keeps escaping
- Make the game run in windowed mode either in-game settings or with `Alt + Enter` This will alleviate controller input and fullscreen issues
- Press `Ctrl + F8` to toggle locking the headset window to the foreground
- If needed, use `Alt + Tab` to switch to the game window (has to be in focus for control input to work)
- To quit, `Alt + Tab` to switch to the headset window and press `Ctrl + F8` to toggle the headset foregrounding off, and then `Alt + Tab` out
#### Multi-Display Setup:
- Keyboard and Mouse are usable, but make sure the mouse is captured by the 2D game's window
- Make sure you set your displays to ***EXTENDED MODE*** or this will not work
- Move all windows besides the `Headset Window` over to your second display
    - Some games provide the option to change which display to use - this is preferred over the options below
    - Can use mouse to drag over
    - Can use Windows shortcut keys to move windowed programs around `Win + Left/Right`
    - Can use Windows shortcut keys to move fullscreen programs and the SteamVR Headset Window around `Shift + Win + Left/Right`
    - May need to make the game windowed either in-game settings or with `Alt + Enter`


## Interlaced, Checkerboard, and Anaglyph Installation (only if you need this output format)

- Complete the [Base Installation](#base-installation) section
- Optionally set `tab_enable` to true in `Documents\My Games\vrto3d\default_config.json` if you prefer to lose half vertical resolution instead of half horizontal resolution
    - If using interlaced mode, you want SbS for Column Interlaced and TaB for Row/Line Interlaced
- Download the latest <a href="https://reshade.me/#download" target="_blank" rel="noopener noreferrer">ReShade</a> with full add-on support
- Run the ReShade installer
    - Browse to to your `Steam\steamapps\common\SteamVR\bin\win64` folder
    - Select `vrserver.exe` and click Next
    - Select `DirectX 11` and click Next
    - Click `Uncheck All` and click Next, Next, Finish
- Download <a href="https://github.com/BlueSkyDefender/Depth3D/blob/master/Other%20%20Shaders/3DToElse.fx" target="_blank" rel="noopener noreferrer">3DToElse.fx</a> and save it to `Steam\steamapps\common\SteamVR\bin\win64\reshade-shaders\Shaders`
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


## SR (Simulated Reality) Displays (only if you need this output format)

- When <a href="https://github.com/JoeyAnthony/XRGameBridge/releases" target="_blank" rel="noopener noreferrer">XRGameBridge</a> is more stable, that will be preferable to use instead of VRto3D for games/mods with OpenXR support
- SR displays work in either Multi or Single Display environments
    - For both, read the Base Installation configuration and usage instructions to ensure that you get a proper 3D image and can control the game
- Complete the [Base Installation](#base-installation) section
- Install the software package provided with your SR display, if yours did not come with one, install the `SR-VERSION-win64.exe` and `simulatedreality-VERSION-win64-Release.exe` from the <a href="https://github.com/LeiaInc/leiainc.github.io/tree/master/SRSDK" target="_blank" rel="noopener noreferrer">LeiaInc Github</a>
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
    - Expand the srReshade dropdown and if you get a `Status: Inactive - Unable to load all SR DLLs` then you need to install the SR Runtime + SDK from Leia's Github above
    - 3D can be toggled on and off by using srReshade's `Ctrl + 2` hotkey
- Click on the `Home` tab
    - Enable ReShade's `Performance Mode` checkbox
- Once configuration is complete, you can run everything the same way as the Base Installation
- If settings don't save, you may have to manually edit `Steam\steamapps\common\SteamVR\bin\win64\ReShade.ini` and disable Tutorial with `TutorialProgress=4`


## Frame Sequential (WibbleWobble) Installation (only if you need this output format)

- Using WibbleWobble is at your own risk to your monitor/emitter/glasses. It works well but is unofficial
- WibbleWobble works with 3DVision, Open3DOLED, and several DIY hardware solutions
- If you don't need VRto3D features (User Presets, Auto-Depth, Pitch/Yaw Emulation, etc) it will be easier to use the <a href="https://github.com/PHARTGAMES/WibbleWobbleCore#steamvr-setup-openvr-client" target="_blank" rel="noopener noreferrer">WibbleWobbleVR plugin directly</a>
### Initial Install
- 3DVision installation detailed below. See WibbleWobble GitHub for other configurations
- Complete the [Base Installation](#base-installation) section
    - Set the Render Resolution to match your Fullscreen Resolution or something lower for performance
    - Set the `window_width` to 2x your `render_width`
    - Leave it in the default SBS display mode
    - If you decide to use 100Hz (recommended for performance/stability), set `display_frequency` to `50.0`
- Use <a href="https://helixmod.blogspot.com/2017/05/3d-fix-manager.html" target="_blank" rel="noopener noreferrer">3D Fix Manager</a> to install the 3D Vision Driver
    - If the emitter driver didn't install, browse to the place you installed the manager and run the `Drivers\3DVisionControllerDriver\3dvisioncontrollerdriver.exe`
- Download the latest <a href="https://github.com/PHARTGAMES/WibbleWobbleCore/releases" target="_blank" rel="noopener noreferrer">WibbleWobble Release</a> and extract it somewhere without spaces in the path like `C:\`
- Under the `WibbleWobbleClient` folder, run `Register.bat` as an administrator and reply `Yes` if prompted
- Under the `WWReshadeAddon\Reshade` folder, run the included ReShade Setup
    - Browse to to your `Steam\steamapps\common\SteamVR\bin\win64` folder
    - Select `vrserver.exe` and click Next
    - Select `DirectX 11` and click Next
    - Click `Uncheck All` and click Next, Next, Finish
- Under the `WWReshadeAddon\Release\x64` folder, copy all files to your `Steam\steamapps\common\SteamVR\bin\win64` folder
- Download the latest <a href="https://customresolutionutility.net/" target="_blank" rel="noopener noreferrer">CRU Release</a> and save it to your WibbleWobble folder
- Run `CRU.exe` and grant it admin
    - Take note of your monitor's EDID (first 7 characters in the dropdown list)
    - Exit CRU
- Download the latest <a href="https://github.com/rajkosto/NvTimingsEd/releases" target="_blank" rel="noopener noreferrer">NvTimingsEd Release</a> and save it to your WibbleWobble folder
- Run `NvTimingsEd.exe` and grant it admin
    - From the drop down list, select your monitor's EDID (AUS_27B1 for Asus PG278QR for example)
    - Select the framerate you want to run 3D at
    - Click `Edit` but don't change anything
    - Copy the exact refresh rate for WibbleWobble's `Sync Rate HZ`
    - Copy the `X` value for WibbleWobble's `Open Delay Microseconds`
    - Copy the `Y` value for WibbleWobble's `Open Duration Microseconds`
    - Copy the `W` value for WibbleWobble's `W`
- Open Nvidia Control Panel 
    - Under the `Change Resolution` tab, set `Refresh Rate` to `100Hz` or `120Hz` and click `Apply`
    - Under `Set up G-Sync` tab, uncheck `Enable G-Sync` and click `Apply`
- Run SteamVR
    - Press `Shift + END` to start WibbleWobble
    - Press `Shift + END` again to bring up the WibbleWobble menu
    - Press `Shift + /` to (un)focus WibbleWobble (necessary to see anything else on desktop)
    - Select `Client Config`
        - Set `Window Size X/Y` for your Fullscreen Resolution
        - Set `Source Format` to `Side By Side Half`
        - Set `Reprojection` to `Disabled`
        - Click `Save` and `X` to close the window
    - Select `Game Config`
        - Set `Window Size X` to be your VRto3D horizontal render resolution
        - Set `Window Size Y` to be your VRto3D vertical render resolution
        - Click `Save` and `X` to close the window
        - This may crash SteamVR, just close out and try again
    - Select `Emitter Config`
        - Set `Sync Rate HZ` to match your exact refresh rate from NvTimingsEd
        - Click `Nvidia Config`
            - Select `Enabled`
            - Set `Open Delay Microseconds` to the NvTimingsEd `X` value
            - Set `Open Duration Microseconds` to the NvTimingsEd `Y` value
            - Set `W` to the NvTimingsEd `W` value
            - Click `Save` and `X` to close the window
        - Click `Save` and `X` to close the window
    - Press `Shift + END` to close the WibbleWobble menu
    - Use `Shift + /` to focus/unfocus WibbleWobble when starting VR games/mods
    - Close the `SteamVR Status` window to completely exit out of things
- Edit `Steam\steamapps\common\SteamVR\bin\win64\ReShade.ini` and disable Tutorial with `TutorialProgress=4` and `PerformanceMode=1`
- If you experience ghosting, try turning on your monitor's `Ultra Low Motion Blur` or `ULMB` feature. You may have to enable 3DVision in Nvidia Control Panel for this setting to be available
### Playing a Game
- Once configuration is complete, you can run everything in single display mode similar to the Base Installation
- Start SteamVR
- Press `Shift + END` to start WibbleWobble
    - If SteamVR crashes, close everything, you may need to temporarily edit your VRto3D `window_width` to half, run SteamVR and activate WibbleWobble, close everything, then revert to full
- Press `Shift + /` to unfocus WibbleWobble (Don't use `Ctrl + F8`!)
- Launch VR Game/Mod
- Press `Shift + /` to focus WibbleWobble (Don't use `Ctrl + F8`!)
- `Alt + Tab` to the game window for controls to work if the game window wasn't already active
- Mouse controls can work on a single display as long as you are clicking somewhere on the game window that is covered by WibbleWobble


## Notes
- The primary display will be where the "Headset" window is located and should be 3D capable
- The game's main window has to be in focus for control input from your mouse/keyboard/controller to work
- SteamVR may still complain about Direct Display mode, but this can be safely dismissed
- Exiting SteamVR will "restart" Steam - this is normal
- Overlays generally won't work on this virtual HMD
- XInput controller is recommended (required for Single-Display mode)
- SteamVR doesn't support HDR currently
    - AutoHDR may work, but some games will be too dark or too bright
- Some mods/games may override your VR settings
- DLSS, TAA, and other temporal based settings often create a halo around objects. UEVR has a halo fix that lets you use TAA, but others may not

#### Controls
- This project is primarily targeted for VR mods of flatscreen games, not full VR games. As such, there is only HMD pitch & yaw emulation and no VR controller emulation
- Several VR controller only games can be made to work by using <a href="https://www.driver4vr.com/" target="_blank" rel="noopener noreferrer">Driver4VR</a>, a paid SteamVR Vive controller emulator. Games with mainly pointer controls work ok. Games with a lot of interaction/movement don't work well.
- Optional HMD `pitch_enable` and `yaw_enable` emulation can be turned on to help with games or mods that need it (maps to XInput right stick)
    - Reference <a href="https://github.com/oneup03/VRto3D/blob/main/vrto3d/src/key_mappings.h" target="_blank" rel="noopener noreferrer">Virtual-Key Code</a> to find the strings to use for these hotkeys
    - The `ctrl_toggle_key` can be set and used to toggle these settings on/off in-game (only functions if `pitch_enable` and/or `yaw_enable` is set to true). The `ctrl_toggle_type` can be set to either `"toggle"` them on/off or `"hold"` that disables them while the button is held
    - The `pose_reset_key` can be set to allow resetting the view to the original position and orientation
    - Both of these keys can be set to XInput buttons & combinations or single keyboard/mouse keys as outlined in User Settings - Load Keys
    - The `pitch_radius` can be set to make the pitch emulation move along a semicircle instead of just tilting up/down in place
- OpenTrack 3DoF support is available over UDP loopback at the default 4242 port when `use_open_track` is true. It can be used in combination with Pitch/Yaw emulation

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
            "user_depth": 0.5,
            "user_convergence": 0.1
        },
      ```
- A Load key and a Store key can be configured to load and save Depth/Separation and Convergence settings for a preset
    - Load keys can use XInput buttons & combinations as well as single keyboard/mouse keys
        - The Guide button can be used, but not in combinations
        - XInput Combinations can be set like this `"XINPUT_GAMEPAD_A+XINPUT_GAMEPAD_B"`
    - Store keys can only use single keyboard/mouse keys
    - Reference <a href="https://github.com/oneup03/VRto3D/blob/main/vrto3d/src/key_mappings.h" target="_blank" rel="noopener noreferrer">Virtual-Key Code</a> to find the strings to use for these hotkeys
- The Load key can be configured to `"switch"` to the user depth/separation & convergence setting, `"toggle"` between the preset and the previous setting every 1.5s, or `"hold"` the user setting until the key is released
- The Store key will update your user Depth/Separation and Convergence setting to the current value (this only saves while the game is running - you need to create a game profile to store it permanently)
- It is recommended to have a single user preset of `"switch"` type that matches the default depth/separation & convergence so you can easily get back to the default

#### Profile Creation Steps:
1. Modify or copy and create user preset(s) in `default_config.json` for the game you want to play
2. If applicable, modify `hmd_height, pitch_enable, yaw_enable, pose_reset_key, ctrl_toggle_key, ctrl_toggle_type, pitch_radius, ctrl_deadzone, ctrl_sensitivity` for the game profile
3. If the game is already running, use `Ctrl + F10` to reload the `default_config.json` with your new settings and presets
4. Adjust depth/separation (`Ctrl + F3` and `Ctrl + F4`) & convergence (`Ctrl + F5` and `Ctrl + F6`) for a preset
5. Use the configured `user_store_key` to temporarily save the current depth/separation & convergence values to the preset
6. Repeat 4 & 5 for each preset you need
7. Adjust depth/separation & convergence back to what you want the default to be (if you have a default `"switch"` preset, you can use its configured `user_load_key`)
8. If applicable, adjust the `ctrl_sensitivity` with `Ctrl -` and `Ctrl +` and the `pitch_radius` with `Ctrl [` and `Ctrl ]`
9. Save the profile with `Ctrl + F7`
10. Open your new profile from `Documents\My Games\vrto3d` in a text editor and make final adjustments like: making all the convergence values match to avoid rendering or performance issues, changing virtual-key mappings, or tweaking other values/settings
11. Close out of SteamVR and the game and restart the game. You should hear a loud beep to indicate the profile loaded. Test the profile and you can still repeat steps 4-10 if needed
12. Share your `Documents\My Games\vrto3d\Game.exe_config.json` with others

#### Troubleshooting
- If a Non-Steam Game is running poorly, try [Disabling Async Reprojection](https://oneup03.github.io/VRto3D/wiki/Disable-Async-Reprojection)
- The first thing to try is deleting your `Steam\config\steamvr.vrsettings`
- If you have used other SteamVR drivers that also create a virtual HMD, you will need to disable and/or uninstall them
    - Run SteamVR
    - On the SteamVR Status window, go to `Menu -> Settings`
    - Change to the `Startup / Shutdown` tab
    - Click `Manage Add-Ons`
    - Turn `Off` any virtual HMD drivers (ALVR, VRidge, OpenTrack, VCR, iVRy, etc)
    - You can also try forcing SteamVR to use the VRto3D driver by editing `Steam\config\steamvr.vrsettings` and under the `"steamvr" : {` section, add this line: `"forcedDriver" : "vrto3d",`
    - if issues still arise, try a <a href="https://steamcommunity.com/app/250820/discussions/2/1640917625015598552/" target="_blank" rel="noopener noreferrer">Clean SteamVR Install</a> and delete your `Steam\steamapps\common\SteamVR` folder
- If you have a VR headset and run into issues with this driver, here's some things to try:
    - Disconnect VR headset from computer
    - <a href="https://steamcommunity.com/app/250820/discussions/2/1640917625015598552/" target="_blank" rel="noopener noreferrer">Clean SteamVR Install</a>
    - <a href="https://www.vive.com/us/support/vs/category_howto/trouble-with-openxr-titles.html" target="_blank" rel="noopener noreferrer">Set SteamVR as OpenXR Runtime</a>[]()


## Building

- Clone the code and initialize submodules
- Define `STEAM_PATH` environment variable with the path to your main Steam folder
- Open Solution in Visual Studio 2022
- Use the solution to build this driver
- Build output is automatically copied to your `SteamVR\drivers` folder
