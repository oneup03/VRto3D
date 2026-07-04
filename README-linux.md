# VRto3D on Linux

Native Linux SteamVR driver — same direct-mode architecture as Windows, no
virtual display or capture chain. The SteamVR compositor renders games into
driver-allocated Vulkan images (shared via dmabuf), VRto3D repacks them into
your chosen 3D format and presents fullscreen on the selected display.

## Requirements

- SteamVR for Linux (Steam → SteamVR → install; ~2.16+)
- A Vulkan-capable GPU (tested: AMD/RADV on Steam Deck)
- Desktop session (X11 or Wayland). KDE, GNOME, Hyprland/Sway all work;
  always-on-top is guaranteed on KDE/wlroots (layer-shell) and X11.
- For hotkeys/gamepad: your user in the `input` group:
  ```
  sudo usermod -aG input $USER   # then log out/in
  ```

## Build

```
cd vrto3d
cmake -B build -G Ninja
cmake --build build
```
Dependencies: gcc/clang C++17, cmake, Vulkan headers, libX11 + libXrandr,
wayland-client, libxkbcommon. (Shaders ship pre-compiled as SPIR-V headers;
regenerate with `shaders/compile_shaders.sh` if you edit them — needs glslc.)

## Install

```
~/.local/share/Steam/steamapps/common/SteamVR/bin/vrpathreg.sh adddriver \
    <checkout>/vrto3d/build/output/drivers/vrto3d
```
Then in `~/.local/share/Steam/config/steamvr.vrsettings` set:
```json
"steamvr": { "requireHmd": true, "forcedDriver": "vrto3d", "activateMultipleDrivers": true }
```

Config lives in `<steam>/config/vrto3d/default_config.json`, same schema as
Windows. Keybind names use the portable vocabulary (`Key_A`, `Numpad7`,
`Pad_A`, `Pad_Start+Pad_DPadDown`); legacy `VK_*`/`XINPUT_*` names still load
and are rewritten on the next profile save.

## Display selection & presenters

`display_index` picks the output (0 = primary, 1..N = connected order). The
presenter is chosen by session: Wayland (layer-shell overlay surface — always
on top on KDE/Hyprland/Sway, plain fullscreen on GNOME) or X11 (borderless
`_NET_WM_STATE_ABOVE` window). Override with env `VRTO3D_PRESENTER=x11|wayland`
(e.g. force X11/XWayland when you need runtime frame-packed modelines).

## Frame-packed (HDMI 1.4 3D TVs)

- **X11 session**: VRto3D applies the 1280x1470/1920x2205 custom modeline at
  runtime via XRandR — no EDID hacking (AMD/Intel; NVIDIA proprietary needs
  `ModeValidation` overrides in xorg.conf).
- **Wayland session**: runtime custom modelines aren't possible. Generate an
  EDID override with `tools/make_fp_edid.py` and load it via
  `drm.edid_firmware=<connector>:edid/<file>.bin` on the kernel cmdline.
- Either way the HDMI 3D InfoFrame is not emitted (kernel limitation) — most
  3D TVs sync to the raw timing or let you force 3D mode manually, exactly
  like CRU-based setups on Windows.

## Not available on Linux (compiled out)

- NVIDIA 3D Vision (`NvidiaDX9`) — driver stack doesn't exist on Linux
- LeiaSR / Simulated Reality displays — no Linux SR SDK in-tree
- WibbleWobble lightfield output — Windows client only (may get its own port)
- UEVR monitor-mode bridge (shared memory) — deferred; planned via a
  Proton-visible `/dev/shm` mapping
- Cursor lock/hide (`hide_cursor`/`lock_cursor`) and focus stealing — no
  cross-client mechanism on Wayland; X11 cursor grab may come later

## Troubleshooting

- `vrto3d.txt` log: `<steam>/logs/vrto3d.txt` (plus stderr in vrserver.txt)
- No hotkeys → check `groups` includes `input`; the OSD shows a warning toast
- No output window → check `WAYLAND_DISPLAY`/`DISPLAY` reach vrserver (launch
  SteamVR from a desktop session, not a raw console)
- Screenshots land in `<steam>/steamapps/common/SteamVR/screenshots`
