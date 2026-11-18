# GameCube Controller Emulator (Arduino Nano)

An Arduino sketch that makes an ATmega328P (Arduino Nano) **act as a GameCube
controller**. It connects to a GameCube console (or a USB GameCube adapter) on
the controller port, waits for the console to poll it, and replies with the
latest input state.

The input state is **not** read from physical buttons. Instead it is received
over the Arduino's serial port (USB) as binary frames. A host program (or a real
controller with an MCU in between) writes frames that describe which buttons and
analog stick positions to report. This lets you inject button presses into a
GameCube console or a GameCube-to-USB adapter from a PC.

## Repository layout

| Path | Purpose |
|------|---------|
| `main.cpp` | The main firmware: serial frame parser + GameCube command handler. |
| `gc-controller-arduino.ino` | Near-empty entry point so the folder is a valid Arduino sketch. **Do not delete** — arduino-cli refuses to compile a folder without a matching `.ino`. |
| `build.sh` | Build / flash helper (see [Build & flash](#build--flash)). |
| `gc_button_test.py` | Host tool that cycles through all 12 buttons (or a single one) for testing in Dolphin/console. |
| `gc_diag.py` | Diagnostic driver: sends a press/release pattern and, when `ENABLE_DEBUG` is on, prints the firmware's counters. |
| `sketches/` | Additional Arduino sketches (test/dev), each in its own folder. |
| `sketches/gc-a-press/` | Minimal test sketch: repeatedly presses A with no serial input. |
| `sketches/gc-debug/` | Dev-only sketch that logs the adapter's command sequence (superseded; kept as reference). |
| `sketches/gc-readcommand/` | Test sketch for the single-pass GameCube command reader. |
| `AGENTS.md` | Detailed build/wiring/troubleshooting notes for contributors. |

## Hardware wiring

> # ⚠️⚠️⚠️ WARNING — RISK OF PERMANENT CONSOLE DAMAGE ⚠️⚠️⚠️
>
> This project wires an Arduino directly to a **real GameCube console / controller
> port**. Getting the wiring wrong — especially **swapping the red DATA (3.3V) and
> blue 3.3V supply lines**, or connecting the data line **without a bidirectional
> 3.3V/5V logic-level converter** — can **permanently damage the console's
> controller port, the console, and/or the Arduino**.
>
> - **Verify every wire with a multimeter before powering anything on.** The
>   cable colours vary by manufacturer; do **not** trust them.
> - **Do not** connect the Arduino data pin directly to the console. The library
>   actively drives the line to 5V, but the GameCube port uses 3.3V logic — a
>   level shifter (e.g. BSS138) is mandatory.
> - **You proceed entirely at your own risk.**
>
> **The authors and contributors provide this project AS-IS, with no warranty of
> any kind, and accept NO liability whatsoever** for any damage to hardware,
> consoles, adapters, or anything else arising from its use or misuse. You are
> solely responsible for what you connect and how.

The Arduino acts as a GameCube **controller** plugged into a console. The
GameCube protocol runs at **3.3V logic**, but the NicoHood library's
`gc_n64_send()` **actively drives the data line HIGH to the AVR supply (5V)**, not
open-drain. A **bidirectional logic-level converter (e.g. BSS138 module) is
required** between the Arduino data pin and the cable's data line. Direct
connection risks damaging the console port.

GameCube controller cable (7-pin plug, 6 used — pin 5 is N/C). Colours vary;
**verify with a multimeter before powering on** (red DATA and blue 3.3V are the
dangerous pair to swap).

| Lead | Pin | Function |
|------|-----|----------|
| Red | 2 | DATA (bidirectional, 3.3V) |
| Blue | 6 | 3.3V logic supply |
| Yellow | 1 | 5V (rumble only — unused here) |
| Green | 3 | GND |
| White | 4 | GND |
| Black | 7 | GND / shield |

Connections (console supplies its own power; Arduino is USB-powered):

- Blue (3.3V) → level shifter `VCC_LV`
- Red (DATA) → level shifter `LV1`; level shifter `HV1` → Arduino `D8` (`GC_PIN`)
- Arduino `5V` → level shifter `VCC_HV`
- Green / White / Black (GND) → level shifter `GND` and Arduino `GND`
- Yellow (5V) → leave disconnected (rumble not used)

See `AGENTS.md` for the full reference.

## Build & flash

Requirements: `arduino-cli` with the `arduino:avr` core and the `Nintendo`
library (v1.4.0, from the Arduino library index).

The attached Nano has the **old bootloader** (57600 baud), so it is always
targeted with `arduino:avr:nano:cpu=atmega328old`. The bare `arduino:avr:nano`
selects optiboot (115200) and will **not** sync with this board.

```bash
# Compile only
./build.sh

# Compile and upload to the Nano (/dev/ttyUSB0)
./build.sh --flash

# Upload to a different serial port
./build.sh --flash --port /dev/ttyUSB1

# Low-level avrdude fallback (for a bootloader baud mismatch)
./build.sh --flash --avrdude

# Remove generated build output
./build.sh --clean
```

The `--avrdude` fallback builds the `.hex` with arduino-cli and flashes it with
`avrdude` at 57600, which is the reliable fix for the classic
`not in sync` / `programmer is not responding` error.

The device signature should read back as `1E 95 0F` (ATmega328P).

### Verify the firmware is running

Send the text command `ID\n` over the serial port (115200 8N1). The firmware
replies with a single line:

```
GCCTRL:1 firmware=1.2 proto=3 report=8 frame=10 baud=115200
```

## Serial protocol (host → Arduino)

8N1 at 115200. Each frame is 10 bytes: a sync byte, the 8-byte
`Gamecube_Report_t`, and an XOR checksum of those 8 bytes.

```
[0]  sync byte   FRAME_SYNC (0xA5)
[1]  report[0]   buttons0   (a,b,x,y,start,origin,errlatch,errstat)
[2]  report[1]   buttons1   (dpad,z,r,l ...)
[3]  report[2]   xAxis      (analog stick X, 0x80 = centre)
[4]  report[3]   yAxis      (analog stick Y, 0x80 = centre)
[5]  report[4]   cxAxis     (C-stick X, 0x80 = centre)
[6]  report[5]   cyAxis     (C-stick Y, 0x80 = centre)
[7]  report[6]   left       (analog L, 0x1F = released)
[8]  report[7]   right      (analog R, 0x1F = released)
[9]  checksum    XOR of report[0..7]
```

A centred, all-buttons-released report is `0x00 0x80 0x80 0x80 0x80 0x80 0x1F 0x1F`.
The sync byte lets the parser re-synchronise if a byte is lost, and the checksum
rejects corrupt frames.

Text commands are line-based and only accepted while the parser is idle:

- `ID\n` → identify (see above).

## Host tools

The Python tools use `pyserial`; run them with the project virtualenv or
install `pyserial` yourself:

```bash
.venv/bin/python gc_button_test.py --button A --cycle 1.0
```

### `gc_button_test.py`

Cycles through all 12 buttons (A, B, X, Y, Start, D-Left/Right/Down/Up, Z, L,
R), holding each for `--duration` seconds then releasing it. Use `--button NAME`
to repeatedly tap a single button — useful for confirming a specific button in a
game or in Dolphin.

```bash
# Tap A once per second
.venv/bin/python gc_button_test.py --button A --cycle 1.0

# Cycle all buttons, holding each 0.3s every 1.5s
.venv/bin/python gc_button_test.py --duration 0.3 --cycle 1.5

# Run the cycle once and exit
.venv/bin/python gc_button_test.py --once
```

### `gc_diag.py`

Sends a press/release pattern and reads the firmware's diagnostic output. It
requires the firmware to be built with `ENABLE_DEBUG 1` in `main.cpp`; it then
prints the `RPT` (report state changes) and `STAT` (poll/read counters) lines
so you can confirm the console is actually receiving the pressed report.

## How the GameCube transaction works

The NicoHood `CGamecubeConsole::write()` stalls on the 1-byte init command:
its `gc_n64_get(len=3)` waits for command bytes 1–2 even for init/origin, which
exceeds the console's ~80µs init response window and breaks the handshake.

`main.cpp` instead uses a custom **single-pass** reader, `gc_read_command()`, that
reads command byte 0 and stops immediately for init (`0x00`/`0xFF`) and origin
(`0x41`/`0x42`), only continuing to bytes 1–2 for a read command (`0x40`). It
keeps the read command's bytes in one continuous bit loop (no inter-call gap,
which previously caused byte 1 to be misread as `0x83`), and responds with the
latest report via `gc_n64_send()`.

Interrupts are left **on** during the idle wait so serial frames can still be
received between console polls, and disabled only at the falling edge inside
`gc_read_command()` so the timing-critical sampling runs clean.

## Known limitations

- Responding to a console poll briefly disables interrupts for the timing-critical
  transaction, so serial input is **not guaranteed lossless** while a console is
  actively polling. The sync + checksum framing makes this recoverable, and the
  host should resend frames while a button is held (as `gc_button_test.py` does).
- The library supports only 16 MHz AVRs (see the `#if (F_CPU != 16000000)` check).
- Rumble is not implemented (the 5V rumble line is left disconnected).
