#!/usr/bin/env python3
"""Drive VRto3D's emulated controllers over UDP (the "Remote (UDP)" source).

Reference sender for the packet format in vrto3d_ctrl/include/vrto3d_ctrl_remote.h.
Enable "Remote (UDP)" in the VRto3D OSD Controllers tab, start SteamVR, then:

    python tools/remote_controller_test.py            # right hand circles, trigger pulses
    python tools/remote_controller_test.py --hand left --rate 120

Positions are sent head-relative (HMD frame), so the hand stays in front of
your eyes wherever the head turns. Haptic requests from the game are printed.
"""
import argparse
import math
import socket
import struct
import time

MAGIC = b"V3DC"
HAPTIC_MAGIC = b"V3DH"
VERSION = 1
FLAG_HEAD_RELATIVE = 0x0001

TRACKING_OK = 1
BTN_A, BTN_B, BTN_TRIGGER, BTN_GRIP, BTN_THUMBSTICK = 1 << 1, 1 << 2, 1 << 3, 1 << 4, 1 << 5

# One hand: connected, tracking, 2 pad bytes, pos[3], rot[4] (w,x,y,z), vel[3],
# angvel[3], buttons, touches, trigger, grip, grip_force, thumbstick[2],
# trackpad[2], trackpad_force, finger_curl[5]  -> 116 bytes
HAND = struct.Struct("<BB2x3f4f3f3fIIfff2f2ff5f")
PACKET_HEADER = struct.Struct("<4sHHQ")  # magic, version, flags, timestamp_us -> 16 bytes
HAPTIC = struct.Struct("<4sHBxfff")      # magic, version, hand, pad, duration, freq, amp -> 20 bytes
assert HAND.size == 116 and PACKET_HEADER.size + 2 * HAND.size == 248 and HAPTIC.size == 20


def hand_bytes(connected=False, pos=(0, 0, 0), rot=(1, 0, 0, 0), buttons=0, trigger=0.0,
               grip=0.0, thumbstick=(0.0, 0.0)):
    return HAND.pack(1 if connected else 0, TRACKING_OK if connected else 0,
                     *pos, *rot, 0, 0, 0, 0, 0, 0,
                     buttons, buttons, trigger, grip, 0.0, *thumbstick, 0.0, 0.0, 0.0,
                     0, 0, 0, 0, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4243)
    ap.add_argument("--hand", choices=["left", "right"], default="right")
    ap.add_argument("--rate", type=float, default=90.0, help="packets per second")
    ap.add_argument("--radius", type=float, default=0.12, help="circle radius in metres")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setblocking(False)
    dest = (args.host, args.port)
    idx = 0 if args.hand == "left" else 1
    side = -0.15 if idx == 0 else 0.15
    t0 = time.monotonic()
    period = 1.0 / args.rate
    print(f"Sending {args.hand} hand to {dest[0]}:{dest[1]} at {args.rate:.0f} Hz - Ctrl+C to stop")

    try:
        while True:
            t = time.monotonic() - t0
            pulling = int(t) % 2 == 0
            this = hand_bytes(
                connected=True,
                # metres in the HMD frame: +X right, +Y up, -Z forward
                pos=(side + args.radius * math.cos(t), -0.25 + args.radius * math.sin(t), -0.45),
                rot=(1.0, 0.0, 0.0, 0.0),  # aim pose: pointing straight ahead
                buttons=BTN_TRIGGER if pulling else 0,
                trigger=1.0 if pulling else 0.0,
            )
            other = hand_bytes(connected=False)
            hands = (this, other) if idx == 0 else (other, this)
            header = PACKET_HEADER.pack(MAGIC, VERSION, FLAG_HEAD_RELATIVE, int(t * 1e6))
            sock.sendto(header + hands[0] + hands[1], dest)

            try:
                while True:
                    data, _ = sock.recvfrom(64)
                    if len(data) == HAPTIC.size and data[:4] == HAPTIC_MAGIC:
                        _, _, h, dur, freq, amp = HAPTIC.unpack(data)
                        print(f"haptic: hand={'LR'[h]} duration={dur:.3f}s freq={freq:.0f}Hz amp={amp:.2f}")
            except BlockingIOError:
                pass
            except ConnectionResetError:
                pass  # Windows reports ICMP port-unreachable when nothing listens yet

            time.sleep(period)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
