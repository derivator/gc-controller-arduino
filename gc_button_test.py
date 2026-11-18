#!/usr/bin/env python3
"""
GameCube controller button-cycle test.

Sends serial frames to the Arduino (see main.cpp) that make the emulated
controller press each button in turn. Each button is held for PRESS_DURATION
then released; the whole cycle advances one button every CYCLE seconds.

The frame is sent repeatedly while a button is held, because the Arduino can
drop serial bytes while it is busy servicing the console's poll (the library
disables interrupts during the GameCube transaction). Sending many copies makes
it very likely at least one complete, valid frame lands in each window.

Usage:
    python3 gc_button_test.py
    python3 gc_button_test.py --port /dev/ttyUSB0 --duration 0.2 --cycle 1.0
    python3 gc_button_test.py --button A --cycle 1.0   # tap A once per second
"""

import argparse
import sys
import time

import serial

SYNC = 0xA5
BAUD = 115200

# Default all-buttons-released / centred report (bytes 1..8).
RELEASED = bytes([0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x1F, 0x1F])

# The 'high1' bit (bit 7 of buttons1) is always set in a normal report.
H1 = 0x80

# Centre values for the analog sticks / C-stick.
CENTER = 0x80
# Analog trigger released / fully-pressed values.
TRIG_RELEASED = 0x1F
TRIG_PRESSED = 0xFF

# (name, buttons0 bit, buttons1 bit, left analog, right analog)
# buttons0 bits (LSB first): A, B, X, Y, Start.
# buttons1 bits (LSB first): D-Left, D-Right, D-Down, D-Up, Z, R, L, high1.
# L/R also drive the analog trigger bytes to 0xFF for a full press.
BUTTONS = [
    ("A",       0x01, 0x00, TRIG_RELEASED, TRIG_RELEASED),
    ("B",       0x02, 0x00, TRIG_RELEASED, TRIG_RELEASED),
    ("X",       0x04, 0x00, TRIG_RELEASED, TRIG_RELEASED),
    ("Y",       0x08, 0x00, TRIG_RELEASED, TRIG_RELEASED),
    ("Start",   0x10, 0x00, TRIG_RELEASED, TRIG_RELEASED),
    ("D-Left",  0x00, 0x01, TRIG_RELEASED, TRIG_RELEASED),
    ("D-Right", 0x00, 0x02, TRIG_RELEASED, TRIG_RELEASED),
    ("D-Down",  0x00, 0x04, TRIG_RELEASED, TRIG_RELEASED),
    ("D-Up",    0x00, 0x08, TRIG_RELEASED, TRIG_RELEASED),
    ("Z",       0x00, 0x10, TRIG_RELEASED, TRIG_RELEASED),
    ("L",       0x00, 0x40, TRIG_PRESSED,  TRIG_RELEASED),
    ("R",       0x00, 0x20, TRIG_RELEASED, TRIG_PRESSED),
]


def frame(report: bytes) -> bytes:
    """Build a full 10-byte frame: sync + 8-byte report + XOR checksum."""
    checksum = 0
    for b in report:
        checksum ^= b
    return bytes([SYNC]) + report + bytes([checksum])


def pressed_report(b0_bit: int, b1_bit: int, left: int, right: int) -> bytes:
    """Build an 8-byte report with one button held."""
    b0 = 0x00 | b0_bit
    b1 = H1 | b1_bit
    return bytes([b0, b1, CENTER, CENTER, CENTER, CENTER, left, right])


def send_for(ser: serial.Serial, data: bytes, duration: float) -> None:
    """Write `data` to the serial port repeatedly for `duration` seconds."""
    end = time.monotonic() + duration
    while time.monotonic() < end:
        ser.write(data)
        # Pace the loop so we do not overrun the OS/AVR serial buffer; the
        # writes are tiny, so a short pause between copies is plenty.
        time.sleep(0.01)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyUSB0",
                        help="Serial port of the Arduino (default /dev/ttyUSB0)")
    parser.add_argument("--baud", type=int, default=BAUD,
                        help="Serial baud rate (default 115200)")
    parser.add_argument("--duration", type=float, default=0.2,
                        help="Seconds to hold each button (default 0.2)")
    parser.add_argument("--cycle", type=float, default=1.0,
                        help="Seconds per button in the cycle (default 1.0)")
    parser.add_argument("--once", action="store_true",
                        help="Run the cycle once and exit instead of looping")
    parser.add_argument("--button", default=None,
                        help="Only press this single button every cycle (e.g. 'A') "
                             "instead of cycling all buttons")
    args = parser.parse_args()

    if args.duration >= args.cycle:
        parser.error("--duration must be less than --cycle")

    # Restrict the cycle to one button if requested.
    buttons = BUTTONS
    if args.button:
        target = args.button.lower()
        buttons = [b for b in BUTTONS if b[0].lower() == target]
        if not buttons:
            parser.error(f"unknown button '{args.button}'. Choose from: "
                         f"{', '.join(b[0] for b in BUTTONS)}")

    rel_frame = frame(RELEASED)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as exc:
        print(f"error: cannot open {args.port}: {exc}", file=sys.stderr)
        return 1

    print(f"Opened {args.port} @ {args.baud}. Cycling {len(buttons)} button(s) "
          f"(hold {args.duration:.2f}s every {args.cycle:.2f}s). Ctrl-C to stop.")
    print(f"Buttons: {', '.join(name for name, *_ in buttons)}")

    try:
        while True:
            for name, b0_bit, b1_bit, left, right in buttons:
                press = frame(pressed_report(b0_bit, b1_bit, left, right))
                print(f"{time.strftime('%H:%M:%S')} press   {name:7s}", flush=True)
                send_for(ser, press, args.duration)
                print(f"{time.strftime('%H:%M:%S')} release {name:7s}", flush=True)
                # Release the button, then idle until the next 1s slot.
                send_for(ser, rel_frame, args.cycle - args.duration)
            if args.once:
                break
    except KeyboardInterrupt:
        print("\nStopped. Sending release frame.")
    finally:
        ser.write(rel_frame)
        ser.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
