# VRto3D

- OpenVR Driver (OpenXR also supported) that can render in any stereo format natively
- Compatible games play great with a XInput controller. No motion controls required!
- VRto3D itself does not "fix" games for 3D, but it allows you to run VR modded (fixed) games on a 3D Display
- Supports User Profiles for individual games
- Provides HMD Pitch and Yaw emulation for games that require it
- Currently targeting OpenVR 2.15.6.
- Windows-only solution, but there are other solutions on Linux like Monado XR.


## Compatible 3D Displays & Output Modes

VRto3D renders one canonical 2W x H side-by-side frame internally and the active Output Mode repacks it for your display. Pick the matching `output_mode` either in `default_config.json` or from the OSD `System` tab (`Ctrl + Home`). Most modes only need the [Regular Installation](#installation); the few that need extra software or display setup are called out below.

Find your display type and use the listed Output Mode(s):

<details markdown="1">
  <summary markdown="span">3D TVs & Projectors (HDMI SbS / TaB)</summary>

- **Output Modes:** `SbS`, `TaB`, or one of the `FramePacked*` modes
- Pick `SbS` or `TaB` based on what your TV / projector accepts as a stereo input
- `FramePacked*` modes target HDMI 1.4 frame-packed 3D - VRto3D will attempt to engage the custom resolution, but it still relies on your display accepting it and allowing you to engage 3D:
    - `FramePacked720p60` - 1280x1470 @60Hz, 30px gap
    - `FramePacked1080p24` - 1920x2205 @24Hz, 45px gap (most universal)
    - `FramePacked1080p60` - 1920x2205 @60Hz, 45px gap, 371.25 MHz pixel clock (HDMI 2.0+)
    - `FramePacked1080p60CVT` - 1920x2205 @60Hz with CVT reduced blanking, 45px gap, 280.8 MHz pixel clock (HDMI 2.0)
    - For any of these modes, it is recommended to start SteamVR before starting the game, as they change monitor modes, which might break games
- For FramePacked, you can also try manually creating one of these Custom Resolutions in Nvidia Control Panel or CRU first:
    - **frame_packed_720p** - 1280x1470, 60Hz
        - horizontal: 1280 active; 110 front, 40 sync, 220 back (1650 total)
        - vertical: 1470 active; 5 front, 5 sync, 20 back (1500 total)
    - **frame_packed_1080p** - 1920x2205, 24Hz / 60Hz
        - horizontal: 1920 active; 638 front, 44 sync, 148 back (2750 total)
        - vertical: 2205 active; 4 front, 5 sync, 36 back (2250 total)
    - May need to use CVT reduced blank specs for success with 1080 60Hz, but 60Hz is not standard and may not work at all
        - horizontal: 1920 active; 48 front, 32 sync, 80 back (2080 total)
        - vertical: 2205 active; 4 front, 5 sync, 36 back (2250 total)
    - It may be necessary to remove other resolutions with CRU to avoid games changing the resolution. Hopefully running them in windowed mode (required for VRto3D) will prevent issues though
    - More instructions and discussion are in <a href="https://www.mtbs3d.com/phpbb/viewtopic.php?t=26494" target="_blank" rel="noopener noreferrer">this forum thread</a>

</details>

<details markdown="1">
  <summary markdown="span">Passive / Interlaced 3D Displays</summary>

- **Output Modes:** `RowInterlaced` (most common), `ColInterlaced`, or `Checkerboard`
- `RowInterlaced` covers the vast majority of passive 3D TVs and monitors
- `ColInterlaced` is for column-interlaced passive panels
- `Checkerboard` is for DLP-link 3D projectors and the older Mitsubishi / Samsung DLP 3D TVs (`(x+y)%2` eye selection)

</details>

<details markdown="1">
  <summary markdown="span">AR Glasses (Rokid, Xreal, Viture, RayNeo)</summary>

- **Output Mode:** `SbS` (switch the glasses to Full-SbS mode first)
- If you don't have a USBC port with DP-Alt mode on your PC, you'll need a <a href="https://docs.google.com/spreadsheets/d/15ub-YF9NU5KQ4r3UsiJlasdu6mH9fk_Xd-C37OcWQgc/edit?usp=sharing" target="_blank" rel="noopener noreferrer">compatible adapter</a> - choose one with SBS and audio support
- A <a href="https://a.co/d/90y4CaY" target="_blank" rel="noopener noreferrer">USBC extension</a> is also recommended
- Optional: install <a href="https://vertoxr.com/" target="_blank" rel="noopener noreferrer">VertoXR</a> for 6DoF or 3DoF Head Tracking
    - Open VertoXR and connect to the AR glasses
    - Select `Game Mode`, place the glasses on a flat surface looking straight ahead, and click `Start Calibrate`. Calibration may need to be redone if misalignment occurs
    - Edit `OpenTrack Configuration` and disable `Enable Roll` if needed
    - `Start` the OpenTrack VertoXR plugin
    - Open the VRto3D OSD (`Ctrl + Home`) and on the `Tracking` tab tick `Enable OpenTrack` under `OpenTrack`, then set `UDP Port` to match VertoXR's OpenTrack port (restart SteamVR after a port change)
    - On the OSD `System` tab, set `Launch Script` to `start vertoxr://steamvr` so VertoXR auto-starts with OpenTrack active every time you start SteamVR
    - Click `Save Default Cfg` in the OSD footer to persist the changes
    - Use the `Recenter` button in VertoXR as needed

</details>

<details markdown="1">
  <summary markdown="span">Lume Pad, 3DS, Other Wireless 3D Displays</summary>

- **Output Mode:** `SbS`
- Lume Pad Requires <a href="https://support.leiainc.com/lume-pad-2/apps/moonlight3d" target="_blank" rel="noopener noreferrer">Sunshine/Gamestream + Moonlight</a> to stream the SbS output to the device
- 3DS Requires <a href="https://github.com/zoeyjodon/moonlight-N3DS/releases" target="_blank" rel="noopener noreferrer">Sunshine/Gamestream + Moonlight</a> with a custom resolution

</details>

<details markdown="1">
  <summary markdown="span">SR Displays (Acer Spatial Labs / Asus Spatial Vision / Samsung Odyssey 3D)</summary>

- **Output Mode:** `LeiaSR` (SR Display Weaver)
- Install Samsung Odyssey 3D Hub or Acer TrueGame first
- Check the `Add LeiaSR to PATH` option in the VRto3D installer
  - Or Manually: Open Windows Run with `Win + R`, Paste this command: `cmd /k setx PATH "C:\Program Files\LeiaSR\Platform\bin;%PATH%"` Exit the terminal and reboot
- 6DoF head tracking is built in - just enable Open Track in the OSD `Tracking` tab. Tune the LeiaSR head-tracking and filter sliders on the same tab

</details>

<details markdown="1">
  <summary markdown="span">3D Vision / Frame Sequential (Shutter Glasses)</summary>

- **Output Modes:** `WibbleWobble` (preferred) or `NvidiaDX9` (legacy / unstable)
- `WibbleWobble` requires the WibbleWobbleClient - the VRto3D installer can deploy it. Follow the [WibbleWobble SteamVR Setup Instructions](https://oneup03.github.io/VRto3D/wiki/WibbleWobbleVR3.0#steamvr-setup) (skip the `VR Config` step, but do the rest). When framerate is low, you will have eye flickering
- `NvidiaDX9` requires the [3DVision driver installed](https://oneup03.github.io/3DVision4All/docs/Native) and 3D Enabled. May freeze or crash, requiring a hard reset
- For both of these modes, it is recommended to start SteamVR before starting the game, as they change monitor modes, which might break games

</details>

<details markdown="1">
  <summary markdown="span">VR Headset (via Virtual Desktop)</summary>

- **Output Mode:** `VirtualDesktop`
- Half-height Full-SbS in a 2W x 2H window with black bars. See the [Virtual Desktop setup wiki](https://oneup03.github.io/VRto3D/wiki/VirtualDesktop) for required additional configuration

</details>

<details markdown="1">
  <summary markdown="span">Dual Display Setups (Dual Projector / Dual Monitor)</summary>

- **Output Modes:** `DualDisplay` or `DualDisplayFlip`
- `DualDisplay` puts the left eye on monitor 1 and the right eye on monitor 2 (starting from `display_index`)
- `DualDisplayFlip` is the same as `DualDisplay` but the left eye is flipped vertically - useful for mirror-based dual-monitor 3D rigs

</details>

<details markdown="1">
  <summary markdown="span">Anaglyph 3D (Color-Filter Glasses)</summary>

- **Output Modes:** any of the `Anaglyph*` modes - works on any 2D display
- Red / Cyan glasses: `AnaglyphRedCyan` (simple R \| GB split), `AnaglyphRedCyanDubois` (Dubois - best general-purpose), `AnaglyphRedCyanDeghosted`, `AnaglyphRedCyanCompromise`
- Green / Magenta glasses: `AnaglyphGreenMagenta` (simple G \| RB split), `AnaglyphGreenMagentaDubois`, `AnaglyphGreenMagentaDeghosted`
- Blue / Amber (ColorCode 3D) glasses: `AnaglyphBlueAmber`

</details>

<details markdown="1">
  <summary markdown="span">2D / Mono Display</summary>

- **Output Mode:** `Mono`
- Single-eye view on any normal 2D display. Toggle `eye_swap` to render the right eye instead of the left

</details>


## Compatible VR Games & Mods
Checkout the [Compatibility List](https://oneup03.github.io/VRto3D/wiki/Compatibility-List) to see if a game has been tested


## Hotkeys
Most adjustments now happen in the [On-Screen Menu](#user-presets-via-osd) — open it with `Ctrl + Home`. The remaining global hotkeys are intentionally minimal:

- Open / close the OSD menu with `Ctrl + Home`
- Adjust Depth (Separation) with `Ctrl + F3` and `Ctrl + F4`
    - Hold `Shift` to also re-sync the projection so the change is visible immediately (some VR mods otherwise need a reload to pick it up)
- Adjust Convergence with `Ctrl + F5` and `Ctrl + F6` - this often has issues in VR mods
- Save current Depth / Convergence / FoV (and other profile fields marked with `"+"` under [Configuration](#configuration)) to the running game's `Game.exe_config.json` with `Ctrl + F7` - a beep indicates success
- Reload the running game's `Game.exe_config.json` with `Ctrl + F10`, or reload `default_config.json` with `Ctrl + Shift + F10` - a beep indicates success
- Toggle Auto-Depth on / off with `Ctrl + F11`
- Toggle locking the SteamVR Headset Window to the foreground (and focusing the game window) with `Ctrl + F8`
- Take a SbS (and Crossview) Screenshot with `Ctrl + F12`
- All of the above (except `Ctrl + Home`) can be disabled with `disable_hotkeys` if they conflict with another 3D mod - the OSD menu remains accessible

User-defined preset hotkeys (configured under [User Presets](#user-presets-via-osd)) work alongside the built-in keys above.


## Installation

- Install <a href="https://store.steampowered.com/app/250820/SteamVR/" target="_blank" rel="noopener noreferrer">SteamVR</a>
- **Recommended**: Download [`VRto3D-Installer.exe`](https://github.com/oneup03/VRto3D/releases/download/latest/VRto3D-Installer.exe) from the latest release and run it. The installer auto-detects your Steam library, installs/updates the VRto3D driver, and offers optional cleanup of legacy ReShade and third-party SteamVR drivers. It can also install WibbleWobble and launch SteamVR for you.
    - **Manual alternative**: Download the [latest VRto3D release](https://github.com/oneup03/VRto3D/releases/latest/download/vrto3d.zip) and copy the `vrto3d` folder from inside the VRto3D.zip to your `Steam\steamapps\common\SteamVR\drivers` folder
- Launch SteamVR once to generate `default_config.json` and you should see a 1080p SbS `VRto3D` window upscaled to fullscreen
- Open the OSD menu with `Ctrl + Home` and use the `System` tab to pick `Display Index`, `Output Mode`, and render resolution for your display - see the [Output Modes](#compatible-3d-displays--output-modes) table for which mode to choose
    - Or edit `Steam\config\vrto3d\default_config.json` manually - [see what each setting does](#configuration)
    - Sometimes the display selection glitches back to the wrong screen and you may have to restart SteamVR
    - If your display is half-SbS or half-TaB, you can usually save some performance by reducing the per-eye render to half-resolution. (may break aspect ratio in some games)
- Download the latest [VRto3D profiles](https://github.com/oneup03/VRto3D/releases/download/latest/vrto3d_profiles.zip) for games and extract them to your `Steam\config\vrto3d\` folder (or click `Download Latest Profiles` on the OSD's `System` tab)
- Restart SteamVR to verify that you see the Headset window covering your entire display. This is usually not needed before running games.
    - The Headset window should appear on the configured `display_index` display
- Try launching a VR game
- Keyboard and Mouse are usable, but you may run into issues with accidentally clicking the wrong window or the cursor escaping the game window if the game's mouse control is coded poorly
    - Can try using <a href="https://github.com/James-LG/AutoCursorLock" target="_blank" rel="noopener noreferrer">AutoCursorLock</a> if the mouse keeps escaping
- Make the game run in windowed mode either in-game settings or with `Alt + Enter` This will alleviate controller input and fullscreen issues. (Borderless fullscreen/windowed sometimes also work)
- If needed, press `Ctrl + F8` to lock the 3D window to the foreground and focus the game window
    - This is automated by default with the `auto_focus` setting
- If game controls & audio aren't working, use `Alt + Tab` to switch to the game window
- To quit, exit the game and try to `Alt + Tab` out
    - If the 3D window remains in the foreground, press `Ctrl + F8` to toggle the foregrounding off, and then `Alt + Tab` out


## Configuration

- VRto3D has to be installed and SteamVR launched once for `default_config.json` to be created
- The easiest way to edit settings is the in-game OSD menu (`Ctrl + Home`); changes take effect immediately and the footer's `Save Default Cfg` / `Save Game Cfg` buttons persist them
- For manual edits, modify `Steam\config\vrto3d\default_config.json` - some changes require a SteamVR restart (display index, output mode, render size, OpenTrack port)
- Fields with a `"+"` next to them are saved to a game's profile when you press `Ctrl + F7` (or use the OSD `Save Game Cfg` footer button), and can be reloaded from the game's profile (`Ctrl + F10`) or `default_config.json` (`Ctrl + Shift + F10`)
- Reference <a href="https://github.com/oneup03/VRto3D/blob/main/external/VRto3DLib/include/vrto3dlib/key_mappings.h" target="_blank" rel="noopener noreferrer">Virtual-Key Code</a> strings for hotkey fields
- Reference [Profile Creation Steps](#profile-creation-via-osd) for creating a game-specific profile

| Field Name                    | Type    | Description                                                                                       | Default Value  |
|-------------------------------|---------|---------------------------------------------------------------------------------------------------|----------------|
| `display_index`               | `int`   | 3D display selection by display order (`0` = auto primary, `1` = first display, `2` = second, etc.) | `0`            |
| `output_mode`                 | `string`| Stereo output format. See the [Output Modes](#compatible-3d-displays--output-modes) table for valid values                | `"SbS"`        |
| `eye_swap`                    | `bool`  | Swap left and right eyes                                                                          | `false`        |
| `render_width`                | `int`   | The width to render per eye                                                                       | `1920`         |
| `render_height`               | `int`   | The height to render per eye                                                                      | `1080`         |
| `display_frequency`           | `float` | Per-eye refresh rate in Hz. `0.0` auto-detects from the target monitor at activation              | `0.0`          |
| `hmd_height` +                | `float` | The height/Z position origin of the simulated HMD                                                 | `1.0`          |
| `hmd_x`                       | `float` | The X position origin of the simulated HMD                                                        | `0.0`          |
| `hmd_y`                       | `float` | The y position origin of the simulated HMD                                                        | `0.0`          |
| `hmd_yaw`                     | `float` | The yaw attitude of the simulated HMD                                                             | `0.0`          |
| `aspect_ratio`                | `float` | The aspect ratio used to calculate vertical FoV                                                   | `1.77778`      |
| `fov` +                       | `float` | The horizontal field of view (FoV) for the VR rendering                                           | `90.0`         |
| `depth` +                     | `float` | The max separation. Overrides VR's IPD field                                                      | `0.1`          |
| `convergence` +               | `float` | Where the left and right images converge. Adjusts frustum                                         | `1.0`          |
| `async_enable` +              | `bool`  | Whether or not to use Asynchronous Reprojection. May improve or worsen smoothness                 | `false`        |
| `auto_depth_enabled` +        | `bool`  | Enable Auto-Depth (GPU disparity analysis caps depth so the closest object stays comfortable)     | `false`        |
| `auto_depth_target_disparity` +| `float`| Target max on-screen disparity, as a fraction of one eye's width                                  | `0.005`        |
| `auto_depth_smoothing` +      | `float` | Auto-Depth smoothing (higher = snappier; lower = smoother)                                        | `0.08`         |
| `disable_hotkeys`             | `bool`  | Disable the global hotkeys (Depth/Convergence/profile/save) to avoid conflict with other 3D mods. `Ctrl + Home` (OSD toggle) is unaffected | `false` |
| `dash_enable`                 | `bool`  | Enable or disable SteamVR Dashboard and Home                                                      | `false`        |
| `auto_focus`                  | `bool`  | Enable or disable automatic focusing/bringing VRto3D to foreground                                | `true`         |
| `pitch_enable` +              | `bool`  | Enables or disables Controller right stick y-axis mapped to HMD Pitch                             | `false`        |
| `yaw_enable` +                | `bool`  | Enables or disables Controller right stick x-axis mapped to HMD Yaw                               | `false`        |
| `use_open_track`              | `bool`  | Enables or disables OpenTrack 6DoF HMD Control                                                    | `false`        |
| `open_track_port`             | `int`   | UDP Port for OpenTrack                                                                            | `4242`         |
| `use_track_filter`            | `bool`  | Enables or disables Accela-Hamilton style pose filtering for tracking rotation and position       | `false`        |
| `trk_flt_rot_sens`            | `float` | Rotation smoothing threshold for track filter (lower = more smoothing)                            | `0.5`          |
| `trk_flt_pos_sens`            | `float` | Position smoothing threshold for track filter (lower = more smoothing)                            | `0.25`         |
| `trk_flt_rot_dz`              | `float` | Rotation deadzone used by track filter                                                            | `0.03`         |
| `trk_flt_pos_dz`              | `float` | Position deadzone used by track filter                                                            | `0.02`         |
| `trk_flt_zoom_smooth`         | `float` | Additional rotation smoothing when moving toward the display                                      | `0.0`          |
| `trk_flt_max_zoom`            | `float` | Max Z distance used for scaling zoom smoothing                                                    | `10.0`         |
| `sr_filter_pos_mincutoff`     | `float` | LeiaSR built-in head tracking: One-Euro position min cutoff                                       | `0.08`         |
| `sr_filter_pos_beta`          | `float` | LeiaSR built-in head tracking: One-Euro position beta                                             | `0.08`         |
| `sr_filter_rot_mincutoff`     | `float` | LeiaSR built-in head tracking: One-Euro rotation min cutoff                                       | `0.12`         |
| `sr_filter_rot_beta`          | `float` | LeiaSR built-in head tracking: One-Euro rotation beta                                             | `0.01`         |
| `sr_angle_deadzone_deg`       | `float` | LeiaSR built-in head tracking: angular deadzone in degrees                                        | `0.2`          |
| `sr_sens_yaw`                 | `float` | LeiaSR built-in head tracking: yaw sensitivity                                                    | `1.0`          |
| `sr_sens_pitch`               | `float` | LeiaSR built-in head tracking: pitch sensitivity                                                  | `1.0`          |
| `sr_sens_roll`                | `float` | LeiaSR built-in head tracking: roll sensitivity                                                   | `1.0`          |
| `sr_max_yaw`                  | `float` | LeiaSR built-in head tracking: max yaw clamp (deg)                                                | `70.0`         |
| `sr_max_pitch`                | `float` | LeiaSR built-in head tracking: max pitch clamp (deg)                                              | `70.0`         |
| `sr_max_roll`                 | `float` | LeiaSR built-in head tracking: max roll clamp (deg)                                               | `70.0`         |
| `sr_track_mode`               | `string`| LeiaSR built-in head tracking mode (`"XYZ_YawPitch"`, `"XYZ"`, `"YawPitch"`, `"Full6DOF"`, `"YawPitchRoll"`) | `"XYZ_YawPitch"` |
| `launch_script`               | `string`| Command executed once when VRto3D driver activates (e.g. `"start vertoxr://steamvr"`)             | `""`           |
| `pose_reset_key` +            | `string`| The Virtual-Key Code to reset the HMD position and orientation                                    | `"VK_NUMPAD7"` |
| `ctrl_toggle_key` +           | `string`| The Virtual-Key Code to toggle Pitch and Yaw emulation on/off when they are enabled               | `"VK_NUMPAD8"` |
| `ctrl_toggle_type` +          | `string`| The ctrl_toggle_key's behavior (`"toggle"`, `"hold"`)                                             | `"toggle"`     |
| `pitch_radius` +              | `float` | Radius of curvature for the HMD to pitch along. Useful in 3rd person VR games                     | `0.0`          |
| `ctrl_deadzone` +             | `float` | Controller Deadzone when using pitch or yaw emulation                                             | `0.05`         |
| `ctrl_sensitivity` +          | `float` | Controller Sensitivity when using pitch or yaw emulation                                          | `1.0`          |
| `user_settings[].user_load_key` +    | `string`| The Virtual-Key Code (or XInput chord) to load a user preset                               | `"VK_NUMPAD1"` |
| `user_settings[].user_key_type` +    | `string`| The load key's behavior. Only `"toggle"` supports multi-preset cycles; `"switch"` and `"hold"` keep just the first value | `"switch"` |
| `user_settings[].user_depth` +       | `float` or `[float]` | The separation value(s) for a user preset (array = cycle on each press, `toggle` mode only) | `0.1`          |
| `user_settings[].user_convergence` + | `float` or `[float]` | The convergence value(s) for a user preset (array = cycle on each press, `toggle` mode only) | `1.0`          |
| `user_settings[].user_fov` +         | `float` or `[float]` | The fov value(s) for a user preset; `0` falls back to the global FoV (array = cycle on each press, `toggle` mode only) | `90.0`         |


## Notes
- The game's main window has to be in focus for control input from your mouse/keyboard/controller to work. Auto Focus should do this for you, but sometimes you may need to `Alt + Tab` to the game
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
- This project is primarily targeted for VR mods of flatscreen games, not full VR games. As such, there is currently no VR controller emulation
- You can setup a Lighthouse + Vive Tracker + tracked controllers with VRto3D for a seated play area. See [this guide](https://oneup03.github.io/VRto3D/wiki/Motion-Controls-&-Tracking) for details
- Several VR controller only games can be made to work by using <a href="https://www.driver4vr.com/" target="_blank" rel="noopener noreferrer">Driver4VR</a>, a paid SteamVR Vive controller emulator. Games with mainly pointer controls work ok. Games with a lot of interaction/movement don't work well.
- Optional XInput right-stick HMD Pitch / Yaw emulation can be turned on for games or mods that need it - configure it from the OSD `Tracking` tab (`Ctrl + Home` -> `XInput (Xbox) Controller`)
    - Tick `Pitch (right stick)` and / or `Yaw (right stick)` to map them
    - `Sensitivity`, `Stick Deadzone`, and `Pitch Radius` (curves the view along a semicircle - useful in 3rd-person games) all live in the same panel
    - `Toggle Key` (default `VK_NUMPAD8`) toggles pitch/yaw on/off in-game; the `Mode` combo next to it switches between `toggle` and `hold` behavior. `Reset Key` (default `VK_NUMPAD7`) recenters. Both bindings accept any keyboard/mouse key or XInput button/chord - click `Set` to capture a single key/button or `Combo` to capture an XInput chord, or type a string from <a href="https://github.com/oneup03/VRto3D/blob/main/external/VRto3DLib/include/vrto3dlib/key_mappings.h" target="_blank" rel="noopener noreferrer">key_mappings.h</a> directly
- OpenTrack 6DoF / 3DoF support is available over UDP loopback - tick `Enable OpenTrack` on the OSD `Tracking` tab (`OpenTrack` panel) and set the UDP port to match your sender (default `4242`; port changes need a SteamVR restart)
    - Works alongside XInput Pitch/Yaw emulation and the HMD offsets
    - SR Displays are supported natively (no bridge required) - enable OpenTrack with `output_mode: "LeiaSR"` and tune the One-Euro filter / sensitivities / clamps in the OSD `Tracking` tab's `LeiaSR Head Tracking` panel
    - AR glasses are compatible via 3rd party apps like <a href="https://vertoxr.com/" target="_blank" rel="noopener noreferrer">VertoXR</a> - set the `Launch Script` field on the OSD `System` tab to `start vertoxr://steamvr` to auto-start it with SteamVR
    - You can also use the <a href="https://github.com/opentrack/opentrack" target="_blank" rel="noopener noreferrer">OpenTrack</a> app to do tracking with cameras, IMUs, phone apps, etc
- Track filtering of 6DoF/3DoF input can be enabled with `use_track_filter`
    - This filter is useful for reducing jitter while preserving responsiveness through sensitivity/deadzone tuning
    - Adjust filter sensitivities and deadzones from the OSD `Tracking` tab

#### User Presets (via OSD)
- Press `Ctrl + Home` to open the OSD menu and select the `User Hotkeys` tab
- Each row maps a Load key to a `(Depth, Convergence, FoV)` preset. Only `toggle` mode supports multi-preset cycles - `switch` and `hold` rows are trimmed to the first value on edit and on profile load
    - Click `Set` to capture a single key, mouse button, or XInput button
    - Click `Combo` to capture an XInput chord (e.g. `XINPUT_GAMEPAD_LEFT_SHOULDER+XINPUT_GAMEPAD_RIGHT_SHOULDER`); the chord commits when you release everything
    - You can also type any <a href="https://github.com/oneup03/VRto3D/blob/main/external/VRto3DLib/include/vrto3dlib/key_mappings.h" target="_blank" rel="noopener noreferrer">Virtual-Key Code</a> string into the Load field directly
- Choose the load behavior under `Mode`:
    - `switch` - jump to the preset and stay there
    - `toggle` - bounce between the preset and the previous setting every 1.5s
    - `hold` - apply the preset only while the key is held
- Edit `Depth`, `Conv`, and `FoV` directly, or click `Copy Current` to fill them from the current Stereo tab values
    - Comma-separated values create a cycle - each press of the Load key advances to the next entry (`toggle` mode only; `switch` and `hold` keep only the first value)
    - `FoV = 0` falls back to the active profile's global FoV
- `+ Add Preset Row` adds a new empty row; the `X` button removes a row
- It is recommended to keep one `switch` preset that matches your default depth/convergence so you can easily get back to baseline
- If you swap between different convergence settings in-game, sometimes you will end up with black bars on the sides of the screen or you may not see a change immediately - reload/restart/reinitialize the VR mod to pick it up. Using a single convergence value across all presets in a game avoids the issue entirely
- Changes are live as soon as you edit them but only persist when you click `Save Game Cfg` or `Save Default Cfg` in the footer

#### Profile Creation (via OSD)
1. Launch the game and let VRto3D load - the OSD title bar will show `Profile: (default)` if no per-game profile exists yet
2. Press `Ctrl + Home` to open the OSD menu
3. **Stereo tab**: dial in `Depth`, `Convergence`, `FoV`, and toggle `Swap Eyes` / `Auto-Depth` as needed
4. **User Hotkeys tab**: add preset rows for the depth/convergence/fov values you want, bind their Load keys, and use `Copy Current` to capture the current values into a row
5. **Tracking tab** (only if the game needs it): set `Height` under `HMD Pose`, and enable `Pitch (right stick)` / `Yaw (right stick)` under `XInput (Xbox) Controller` for games that need camera control
6. **System tab**: toggle `Async Reprojection` under `Misc` if it helps smoothness for this game
7. In the footer click `Save Game Cfg` to write `Steam\config\vrto3d\Game.exe_config.json` (the button label includes the game name and is disabled until VRto3D detects a running game). `Save Default Cfg` writes the equivalent values to `default_config.json` for use as a global baseline
8. (Optional) Open the file in a text editor for final tweaks - making all convergence values match avoids rendering / performance issues, and you can hand-edit virtual-key strings or other fields the OSD doesn't expose
9. Restart the game; you should hear a beep when the profile loads. Continue tuning in-OSD and re-save with `Ctrl + F7` (or the footer button) at any time
10. Share your `Steam\config\vrto3d\Game.exe_config.json` with others

#### Troubleshooting
- If SteamVR appears on the wrong display, set `display_index` to the correct display enumeration order (or pick it from the OSD `System` tab) and restart SteamVR
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
