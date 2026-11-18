#!/usr/bin/env python3
"""
Diagnostic driver for the GameCube controller emulator.

Sends a press/release pattern for one button (A) and, in the background,
reads the Arduino's diagnostic output (RPT / STAT lines) so we can see
whether serial frames are actually being received and what report state is
being set. This is for debugging intermittent button registration.

Usage:
    python3 gc_diag.py [--port /dev/ttyUSB0] [--duration 0.2] [--cycle 1.0]
"""

import argparse
import sys
import threading
import time

import serial

SYNC = 0xA5
BAUD = 115200
RELEASED = bytes([0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x1F, 0x1F])
H1 = 0x80
CENTER = 0x80
TRIG_RELEASED = 0x1F
TRIG_PRESSED = 0xFF


def frame(report: bytes) -> bytes:
    checksum = 0
    for b in report:
        checksum ^= b
    return bytes([SYNC]) + report + bytes([checksum])


def pressed_report(b0: int, b1: int, left: int, right: int) -> bytes:
    return bytes([b0, H1 | b1, CENTER, CENTER, CENTER, CENTER, left, right])


def reader(ser: serial.Serial, stop: threading.Event) -> None:
    """Read the Arduino's line-based diagnostic output and print it."""
    line = b""
    while not stop.is_set():
        try:
            data = ser.read(1)
        except Exception as exc:
            print(f"[reader] read error: {exc}", file=sys.stderr)
            break
        if not data:
            continue
        b = data[0]
        if b == 0x0A:  # '\n'
            text = line.decode("ascii", "replace").rstrip("\r")
            if text:
                ts = time.strftime("%H:%M:%S") + f".{int(time.time()*1000)%1000:03d}"
                print(f"{ts}  {text}")
            line = b""
        else:
            line += bytes([b])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--duration", type=float, default=0.2,
                    help="seconds to hold the button (default 0.2)")
    ap.add_argument("--cycle", type=float, default=1.0,
                    help="seconds per press/release cycle (default 1.0)")
    ap.add_argument("--presses", type=int, default=0,
                    help="stop after this many presses (0 = run forever)")
    args = ap.parse_args()

    if args.duration >= args.cycle:
        ap.error("--duration must be less than --cycle")

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
    except serial.SerialException as exc:
        print(f"error: cannot open {args.port}: {exc}", file=sys.stderr)
        return 1

    stop = threading.Event()
    t = threading.Thread(target=reader, args=(ser, stop), daemon=True)
    t.start()

    press = frame(pressed_report(0x01, 0x00, TRIG_RELEASED, TRIG_RELEASED))
    rel = frame(RELEASED)

    print(f"Diag: pressing A for {args.duration:.2f}s every {args.cycle:.2f}s "
          f"on {args.port} @ {args.baud}. Ctrl-C to stop.", flush=True)

    def now_str():
        return time.strftime("%H:%M:%S") + f".{int(time.time()*1000)%1000:03d}"

    count = 0
    try:
        while True:
            end = time.monotonic() + args.duration
            print(f"{now_str()}  HOST press start ({args.duration:.2f}s)", flush=True)
            while time.monotonic() < end:
                ser.write(press)
                time.sleep(0.01)
            end = time.monotonic() + (args.cycle - args.duration)
            print(f"{now_str()}  HOST release start ({args.cycle-args.duration:.2f}s)", flush=True)
            while time.monotonic() < end:
                ser.write(rel)
                time.sleep(0.01)
            count += 1
            if args.presses and count >= args.presses:
                break
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        ser.write(rel)
        stop.set()
        time.sleep(0.2)
        ser.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
