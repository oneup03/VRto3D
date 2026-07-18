# VRto3D Installer

A Windows installer (Inno Setup) that automates VRto3D installation, optional WibbleWobble install, and a few common cleanup tasks.

## What it does

The installer wizard offers five tasks:

- **Install / update VRto3D driver** (default on) — downloads `vrto3d.zip` from the latest GitHub release and copies the `vrto3d` folder into SteamVR's `drivers/` directory. Falls back to a `vrto3d.zip` placed next to the installer .exe if the download fails.
- **Install WibbleWobble for Frame Sequential 3D** (default off) — downloads `WibbleWobbleBeta7.zip`, lets the user pick a parent folder, extracts the `WibbleWobble` folder there, and runs `WibbleWobble\WibbleWobbleClient\Register.bat` elevated. Also removes the legacy `<SteamVR>\drivers\WibbleWobbleVR` driver, which conflicts with VRto3D's built-in WibbleWobble presenter.
- **Remove legacy ReShade from `SteamVR\bin\win64`** (default on) — deletes only files identified as ReShade artifacts (DLL hooks containing the `ReShade` signature, plus `ReShade.ini`/`.log`/`reshade-shaders\`/`reshade-presets\`/`reshade-cache\` and `3DToElse.fx`). SteamVR's own files are not touched.
- **Remove third-party SteamVR drivers and reset `steamvr.vrsettings`** (default on) — deletes any folder under `<SteamVR>\drivers\` that isn't on the Valve allowlist (`gamepad`, `htc`, `indexcontroller`, `indexhmd`, `lighthouse`, `null`, `oculus`, `oculus_legacy`, `prism`, `tundra_labs`, `vrlink`, `vrto3d`) and deletes `<Steam>\config\steamvr.vrsettings`.
- **Launch SteamVR when finished** (default on) — starts SteamVR via `steam://run/250820` from the Finish page.

## How SteamVR is located

1. Reads `HKCU\Software\Valve\Steam\SteamPath` from the registry.
2. Parses `<Steam>\steamapps\libraryfolders.vdf` and looks for app `250820` (SteamVR) — handles users with SteamVR installed on a different drive.
3. Falls back to `<Steam>\steamapps\common\SteamVR` if the VDF parse misses.

If neither Steam nor SteamVR is detected, the installer offers to open Steam's install page (`steam://install/250820`) or lets the user point to a SteamVR folder manually.

## Building

Requires Inno Setup 6.4 or later (for the bundled 7-Zip extractor).

```powershell
choco install innosetup -y
iscc installer\VRto3D-Installer.iss
```

Output: `installer\Output\VRto3D-Installer.exe`.

## Distribution

Built in CI by `.github/workflows/Build.yml` and uploaded as a separate release asset alongside `vrto3d.zip`. Users can either:

- Download just `VRto3D-Installer.exe` and let it pull `vrto3d.zip` over the network at install time.
- Download both `VRto3D-Installer.exe` and `vrto3d.zip` into the same folder for offline installs — the installer auto-detects the local zip when the download fails.

## Logs

Every action is appended to `<SteamPath>\logs\VRto3D-Installer.log` (e.g. `C:\Program Files (x86)\Steam\logs\VRto3D-Installer.log`). If Steam isn't detected, the log falls back to `%TEMP%\VRto3D-Installer.log`. Useful for diagnosing skipped tasks (e.g. "SteamVR path not set, skipping").
