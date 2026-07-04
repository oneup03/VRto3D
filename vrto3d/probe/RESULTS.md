# Milestone 0 — Linux direct-mode spike: **GO** ✅

Date: 2026-07-03 · Hardware: Steam Deck (AMD Custom GPU 0405, RADV VANGOGH) · SteamVR 2.16.7, desktop-mode KDE Wayland session · Probe log: `~/vrto3d_probe.log`

## Verdict

Linux vrcompositor **fully drives driver-side `IVRDriverDirectModeComponent`**. The
VRto3D direct-mode architecture ports to Linux with **no virtual display, no capture
chain, and no fallback needed**.

## Evidence

1. **Direct mode wired up**: `GetComponent("IVRDriverDirectModeComponent_009")` requested
   immediately after HMD Activate. (Compositor also probes `IVRVirtualDisplay_002`,
   `IVRCameraComponent_003`, `IVRControllerComponent_001`.)
2. **Swap texture allocation works**: `CreateSwapTextureSet` (2 sets, 1476×828,
   `nFormat=43` = `VK_FORMAT_R8G8B8A8_SRGB`) → `VRIPCResourceManager()->NewSharedVulkanImage(...)`
   returned handles the compositor accepted and continuously renders into.
3. **Full frame loop at display cadence**: `SubmitLayer → Present → PostPresent →
   GetNextSwapTextureSetIndex`, dt ≈ 16.7 ms (60 fps, matching our VsyncEvent thread);
   drops to 10 fps in standby; jumps back to 60 when an app connects.
4. **dmabuf import viable**: `RefResource → ReceiveSharedFd` → fd (5,505,024 bytes) →
   `vkGetMemoryFdPropertiesKHR` = `VK_SUCCESS` (memoryTypeBits=0x81) on RADV.
5. **Real OpenXR app end-to-end**: SteamVR's `helloxr_vulkan -g Vulkan2` created a session
   against the probe HMD (`System Properties: Name=SteamVR/OpenXR : vrto3d_probe`),
   per-eye swapchains 1468×824, frames flowed at 60 fps.

## Design facts learned (feed into M3)

- **Single-layer model on Linux**: app pids never get driver texture sets — vrcompositor
  pre-composites everything (apps, dashboard, overlays) and submits ONE layer pair per
  frame from its own pid. The multi-layer composite path + layer_policy pid sorting are
  Windows-only concerns; `VkRenderer` needs only: import the compositor's 2×3 shared
  images once, per frame sample L/R into `out_sbs_`, repack, present.
- **`nFormat` is a VkFormat** on Linux (not DXGI): observed 43 (`R8G8B8A8_SRGB`).
- **`Present(syncTexture)` passes 0** on Linux → no keyed-mutex analog; implicit dmabuf
  sync is the intended model (matches plan baseline).
- **`VRIPCResourceManager()` is NULL during `Init()`** (initializes async); available by
  first `CreateSwapTextureSet`. Do not cache it early.
- Missing `Prop_GraphicsAdapterLuid_Uint64` is fine — no LUID handshake on Linux.
- **Runtime environment**: vrserver runs under the **sniper** pressure-vessel container
  (`PRESSURE_VESSEL_RUNTIME=sniper_platform_3.0.*`) but sees the session's `DISPLAY=:0`,
  `WAYLAND_DISPLAY=wayland-0`, and `XDG_RUNTIME_DIR=/run/user/1000` — so the driver can
  connect to X11/Wayland directly. Driver `.so` built against glibc 2.42 SDK loaded fine
  (max versioned symbol GLIBC_2.4 / GLIBCXX_3.4.22).
- `GetDmabufFormats` at Init couldn't run (resource manager null); re-query later if the
  format list is ever needed — direction A (compositor-allocated images) makes it mostly moot.

## Repro

```
cd vrto3d/probe && cmake -B build -G Ninja && cmake --build build
~/.local/share/Steam/steamapps/common/SteamVR/bin/vrpathreg.sh adddriver $PWD/build/drivers/vrto3d_probe
# steamvr.vrsettings: requireHmd=true, forcedDriver="vrto3d_probe", activateMultipleDrivers=true
steam steam://run/250820
tail -f ~/vrto3d_probe.log
```
