# Changelog

## V3.2.0 (V3.2.0) - 2025-05-26T19:48:42Z

- Added On Screen Display for: Profile load/save notifications, Depth & Convergence, HMD height, Controller Sensitivity & Pitch Radius, User Save binding updates
    - This isn't perfect as the text flashes on screen and it doesn't show up when using WibbleWobble
    - Removed `depth_gauge` setting, but the gauge will show up if you set the `dash_enable` setting for SteamVR Dashboard
- Updated Asynchronous Reprojection handling:
    - Added `async_enable` global setting that can be turned on if it performs better on your system
    - VRto3D now forces this setting for all VR games (no longer Steam-only)
- Updated foregrounding/focusing feature:
    - Added `auto_focus` global setting that will automatically bring the VRto3D Headset Window to the foreground when you launch a game that has a VRto3D profile. DO NOT use this with WibbleWobble
    - VRto3D will now always release focus when a game exits
- Hotkey updates:
    - Added Depth Sync hotkey `Ctrl + Shift + F3/F4` that updates the convergence as depth is adjusted (this is the same effect as 3DV depth). This won't work in UEVR or RealVR without Restart/Reinitialize as it changes convergence
    - Changed `Ctrl + F10` hotkey to reload the current game's VRto3D profile
    - Moved default config reload hotkey to `Ctrl + Shift + F10`
- Added VRto3D profiles for many native VR games including: Battle Zone, Doom VFR, Humanity, Lucky's Tale VR, Microsoft Flight Simulator, Psychonauts RoR, Sayonara Umihara Kawase, Sky Squadron, Star Wars Squadrons, Subnautica, Tetris Effect Connected, Thumper, Zone of the Enders 2. All games and instructions available on [3DJ's Database](https://airtable.com/appByPZJsOQSVGDID/shrAfMuGs1IOIEpRT?gtyDn=b%3AWzAsWyI3T05lYiIsNixbInNlbEJNS1VkQkVVV0tnNE9yIl0sImdGWUgwIl1d&uMgeK)
- Additional UEVR profiles that may not get posted on the blog are available [here](https://github.com/oneup03/VRto3D/tree/main/game_configs/uevr). Some recent additions: Eternal Strands, Kiborg, Robocop, Still Wakes the Deep. Follow the [UEVR Quickstart](https://github.com/oneup03/VRto3D/wiki/UEVR-QuickStart)


**Full Changelog**: https://github.com/oneup03/VRto3D/compare/V3.1.0...V3.2.0


## V3.1.0 (V3.1.0) - 2025-03-17T18:30:15Z

- Allow mouse passthrough, making it possible to play games using a mouse on a single display!
    - Make sure the game window is in a good position to receive mouse input before making VRto3D on top using `Ctrl+F8`
    - If the game doesn't properly capture the mouse, you may end up losing control of the game. [AutoCursorLock](https://github.com/James-LG/AutoCursorLock) might help
- Removed `debug_enable` flag as it will now always be set as all 3D display modes work with it now and it only causes user confusion
    - Legacy 3DVision plugin is now incompatible, but users should be moving to use WibbleWobble instead
- Force Async Reprojection to be disabled for Steam Games, making them run a lot smoother at lower framerates
    - You can [manually set this](https://github.com/oneup03/VRto3D/wiki/Disable-Async-Reprojection) for Non-Steam Games

**Full Changelog**: https://github.com/oneup03/VRto3D/compare/V3.0.1...V3.1.0


## V3.0.1 (V3.0.1) - 2025-02-25T03:23:18Z

- Significantly better Frame Sequential (3DVision) support thanks to PhartGames' WibbleWobble software! Runs much more stably and can now run independent of Nvidia driver versions
- Fixed Convergence/Zero Parallax calculation to use the proper Off-Axis calculation instead of the incorrect Toe-In calculation that was used previously. All released profiles have been updated to use proper convergence. If you created any personal profiles, you will need to update them. `4.0` is the new default convergence and a good neutral value to start from
- Added Auto Depth Listener so that mods can command a different depth value dynamically. Use `Ctrl + F11` to toggle it off/on (on is default on startup)
- Added 3DoF [OpenTrack UDP](https://github.com/opentrack/opentrack) support. Set `use_open_track` to true and change `open_track_port` if you need to in default_config.json to use it. Can be used in combination with Pitch/Yaw emulation similar to gyro aim
- Added option to use SteamVR dashboard. Set `dash_enable` to true in default_config.json to use it
- FoV settings now also store in game profiles, so you can have different settings per-game
- Beep pitch lowered to be less annoying
- All [UEVR](https://github.com/oneup03/VRto3D/wiki/UEVR)/[RealVR](https://github.com/oneup03/VRto3D/wiki/Luke-Ross-RealVR-Mods) configuration files and instructions updated for DLSS4

**Full Changelog**: https://github.com/oneup03/VRto3D/compare/V2.0.0...V3.0.1


## V2.0.0 (V2.0.0) - 2024-10-05T20:44:54Z

### This release is not backwards compatible with previous releases!
Delete your `Steam\steamapps\common\SteamVR\drivers\vrto3d` folder and `Steam\config\steamvr.vrsettings` before installing this one!

- Main configuration moved to `Documents\My Games\vrto3d\default_config.json`
- `default_config.json` is generated on first startup of SteamVR after VRto3D is installed
- Settings with a `+` are stored/loaded to/from profiles and can be reloaded from `default_config.json` using `Ctrl + F10`
- Rewrote Profiles so that they are more easily modified and shareable
    - depth & convergence, user hotkeys, and pitch/yaw emulation settings are stored
    - Make changes in `default_config.json` as needed for a game ( and then reload with `Ctrl + F10` if SteamVR is running)
    - Start the game
    - Make additional adjustments in-game using hotkeys
    - Press `Ctrl+F7` to save the profile
    - Profile will automatically load each time you launch the game
    - Profiles are stored in `Documents\My Games\vrto3d\`
- Official RealVR support as of RealVR V14.1.0. [See Instructions](https://github.com/oneup03/VRto3D/wiki/Luke-Ross-RealVR-Mods)
- Added `ctrl_toggle_type` so that pitch/yaw control can be either `"toggle"` or `"hold"` off
- `num_user_settings` and numbering your user presets are deprecated and no longer needed

**Full Changelog**: https://github.com/oneup03/VRto3D/compare/V1.7.0...V2.0.0


## V1.7.0 (V1.7.0) - 2024-09-23T17:42:31Z

- Rewrote pitch/yaw emulation handling. Significantly improved judder
- Added hotkeys to adjust `ctrl_sensitivity` using `Ctrl -` and `Ctrl +`
- Added hotkeys to adjust `pitch_radius` using `Ctrl [` and `Ctrl ]`

**Full Changelog**: https://github.com/oneup03/VRto3D/compare/V1.6.0...V1.7.0


## V1.6.0 (V1.6.0) - 2024-09-21T17:09:41Z

**OUTDATED PROFILE INFO. See V2.0.0**

- Added per-game configurations!
    - depth & convergence, user hotkeys, and pitch/yaw emulation settings are stored
    - Make changes in `default.vrsettings` as needed for the game (need to restart SteamVR after changing)
    - Load the game
    - Make additional adjustments in-game using hotkeys
    - Press `Ctrl+F7` to save the profile
    - Profile will automatically load each time you launch the game
    - Profiles are stored in `Steam\config\steamvr.vrsettings`
- Added a new hotkey to reload the startup settings using `Ctrl + F10`
    - Can load these defaults then press `Ctrl+F7` to save over a game's profile if needed
- Slightly Improve judder of pitch/yaw emulation
- Cleanup and add some keyboard/mouse hotkeys

**Full Changelog**: https://github.com/oneup03/VRto3D/compare/V1.5.0...V1.6.0


## V1.5.0 (V1.5.0) - 2024-09-14T16:52:17Z

- Make Borderless Windowed the default again as OpenVR compatibility seems to have improved
- Fix stutter when using pitch/yaw emulation in single display mode
- Fix Yaw position wraparound error
- Make default presets all use same convergence setting. Changing convergence in-game can cause black bars on the side of the screen, so you should find a convergence you like, save it for all your presets, and restart the game.
- Improve performance when switching to another preset
- Add XInput Guide button as a hotkey (cannot be used in combinations)
- Add Combination hotkeys for XInput. They can be defined like this: `"XINPUT_GAMEPAD_A+XINPUT_GAMEPAD_B"`
- Update SR Display instructions for 1.0 release of srReShade

**Full Changelog**: https://github.com/oneup03/VRto3D/compare/V1.4.1...V1.5.0


## V1.4.1 (V1.4.1) - 2024-08-31T20:46:23Z

- Force SteamVR to use the render resolution defined in VRto3D's settings
- Added instructions for Leia/Acer SR/SpatialLabs displays
- Cleanup readme instructions to be more organized

**Full Changelog**: https://github.com/oneup03/VRto3D/compare/V1.4.0...V1.4.1


## V1.4.0 (V1.4.0) - 2024-07-24T05:23:37Z

- Re-added debug display mode as an optional parameter `debug_enable` in default.vrsettings. This will make more games work in Single-Display mode, but may cause framerate issues and is incompatible with some OpenVR mods (use OpenXR instead) and incompatible with 3DVision
    - UEVR GUI elements will display properly and Luke Ross mods will work for example
- SteamVR Status window will now let you know when your Headset window isn't fullscreen and let you toggle it on. This is useful for Multi-Display mode as when the Headset window isn't fullscreen, the game may not render or render incorrectly.
- Removed `hdr_enable` setting as it did nothing and SteamVR doesn't fully support HDR currently

**Full Changelog**: https://github.com/oneup03/VRto3D/compare/V1.3.0...V1.4.0


##  (V1.3.0) - 2024-07-20T13:06:28Z

- Added support for Single Display mode by locking the headset window in the foreground using `Ctrl+F8` see README for details. Doesn't work with 3DVision
- Improved 3DVision support
    - Full resolution per-eye can now be achieved by using DSR 4x without a performance hit - see README
    - Stability of 3D activation greatly improved
    - Compatibility with VR mods improved - you should now be able to run any game+mod that doesn't require newer drivers
    - The need to use SpecialK is now gone (remove SteamVR launch parameters if you have any)
    - If you run into issues, do a [clean install](https://steamcommunity.com/app/250820/discussions/2/1640917625015598552/) and also delete your `Steam\steamapps\common\SteamVR` folder
- Added `render_width` and `render_height` to set the resolution per-eye
    - `half_enable` is now deprecated and shouldn't be used
- Fixed compatibility with OpenVR mods - should now be able to run UEVR and REFramework in OpenVR mode
- Fixed framerate/refresh rate mismatch
- Added configurable `pose_reset_key` that is useful when using pitch/yaw emulation
- Added `pitch_radius` setting that can make the camera move up and over/down and under while it is pitching up/down. This is useful for 3rd person games and particularly Luke Ross mods

**Full Changelog**: https://github.com/oneup03/VRto3D/compare/V1.2.0...V1.3.0


## V1.2.0 (V1.2.0) - 2024-07-13T04:35:12Z

- Added Instructions for 3DVision, Interlaced, Checkerboard, Anaglyph rendering
- Added configurable toggle key to turn on/off pitch & yaw emulation
- Added toggle for HMD height to Ctrl + F9 for games that force a floor calibration
- Fixed HMD proximity sensor causing some VR games to pause


**Full Changelog**: https://github.com/oneup03/VRto3D/compare/V1.1.0...V1.2.0


## V1.1.0 (V1.1.0) - 2024-06-29T01:14:47Z

- Added User-bindable hotkeys for Depth+Convergence
- Added HMD Pitch and Yaw emulation that are useful for REALVR and VR games
- Added ability to save Depth+Convergence using Ctrl+F7
- Added reverse eye rendering

**Full Changelog**: https://github.com/oneup03/VRto3D/compare/V1.0.0...V1.1.0


## Initial Release V1.0.0 (V1.0.0) - 2024-06-11T05:15:47Z

Initial version with SbS, TaB output support and depth/convergence hotkeys
[vrto3d.zip](https://github.com/user-attachments/files/15782584/vrto3d.zip)

**Full Changelog**: https://github.com/oneup03/VRto3D/commits/V1.0.0

