#!/usr/bin/env python3
"""Generate EDID firmware binaries for HDMI 1.4 frame-packed timings.

For Wayland sessions (no runtime modelines) or displays whose EDID doesn't
advertise the frame-packed timing, load one of these via the kernel:

    sudo mkdir -p /usr/lib/firmware/edid           # SteamOS: steamos-readonly disable first
    sudo cp vrto3d_fp1080p60cvt.bin /usr/lib/firmware/edid/
    # then add to kernel cmdline (connector name from `ls /sys/class/drm`):
    drm.edid_firmware=HDMI-A-1:edid/vrto3d_fp1080p60cvt.bin

On X11 sessions this is unnecessary — VRto3D applies the modeline at runtime
via XRandR (see presenter/x11_modeline.cpp).

Timings mirror s_frame_pack_timings in external/VRto3DLib/src/json_manager.cpp.
"""
import struct
import sys

# name, active_w, active_h, refresh, h_total, h_front, h_sync, h_back, v_total, v_front, v_sync, v_back
TIMINGS = [
    ("vrto3d_fp720p60",      1280, 1470, 60, 1650, 110, 40, 220, 1500, 5, 5, 20),
    ("vrto3d_fp1080p24",     1920, 2205, 24, 2750, 638, 44, 148, 2250, 4, 5, 36),
    ("vrto3d_fp1080p60",     1920, 2205, 60, 2750, 638, 44, 148, 2250, 4, 5, 36),
    ("vrto3d_fp1080p60cvt",  1920, 2205, 60, 2080, 48, 32, 80, 2250, 4, 5, 36),
]


def dtd(active_w, active_h, refresh, h_total, h_front, h_sync, h_back, v_total, v_front, v_sync, v_back):
    """18-byte EDID Detailed Timing Descriptor."""
    pclk_10khz = round(h_total * v_total * refresh / 10000)
    h_blank = h_total - active_w
    v_blank = v_total - active_h
    d = bytearray(18)
    struct.pack_into("<H", d, 0, pclk_10khz)
    d[2] = active_w & 0xFF
    d[3] = h_blank & 0xFF
    d[4] = ((active_w >> 8) << 4) | (h_blank >> 8)
    d[5] = active_h & 0xFF
    d[6] = v_blank & 0xFF
    d[7] = ((active_h >> 8) << 4) | (v_blank >> 8)
    d[8] = h_front & 0xFF
    d[9] = h_sync & 0xFF
    d[10] = ((v_front & 0xF) << 4) | (v_sync & 0xF)
    d[11] = (((h_front >> 8) & 3) << 6) | (((h_sync >> 8) & 3) << 4) \
          | (((v_front >> 4) & 3) << 2) | ((v_sync >> 4) & 3)
    # Image size unknown (0), no borders.
    d[17] = 0x1E  # digital separate sync, +hsync +vsync
    return bytes(d)


def display_name_descriptor(name):
    payload = (name[:12] + "\n").ljust(13, " ").encode("ascii")
    return b"\x00\x00\x00\xfc\x00" + payload


def dummy_descriptor():
    return b"\x00\x00\x00\x10\x00" + b"\x00" * 13


def make_edid(name, timing):
    e = bytearray(128)
    e[0:8] = b"\x00\xff\xff\xff\xff\xff\xff\x00"
    # Manufacturer "VTD", product 0x3D01
    mfg = ((ord("V") - 64) << 10) | ((ord("T") - 64) << 5) | (ord("D") - 64)
    struct.pack_into(">H", e, 8, mfg)
    struct.pack_into("<H", e, 10, 0x3D01)
    struct.pack_into("<I", e, 12, 1)      # serial
    e[16] = 1                              # week
    e[17] = 34                             # year (1990+34 = 2024)
    e[18] = 1                              # EDID 1.4
    e[19] = 4
    e[20] = 0xA5                           # digital, 8bpc, DisplayPort-ish caps
    e[21] = 0                              # size unknown
    e[22] = 0
    e[23] = 120                            # gamma 2.2
    e[24] = 0x0A                           # features: preferred timing native
    # Chromaticity: sRGB-ish defaults
    e[25:35] = bytes([0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54])
    # No established/standard timings
    e[35] = e[36] = e[37] = 0
    for i in range(38, 54, 2):
        e[i] = 0x01
        e[i + 1] = 0x01
    # Descriptor blocks
    e[54:72] = dtd(*timing)
    e[72:90] = display_name_descriptor(name)
    e[90:108] = dummy_descriptor()
    e[108:126] = dummy_descriptor()
    e[126] = 0  # no extensions
    e[127] = (256 - sum(e[0:127])) % 256
    return bytes(e)


def main():
    for row in TIMINGS:
        name, *timing = row
        blob = make_edid(name, timing)
        out = f"{name}.bin"
        with open(out, "wb") as f:
            f.write(blob)
        print(f"wrote {out} ({timing[0]}x{timing[1]}@{timing[2]})")


if __name__ == "__main__":
    sys.exit(main())
