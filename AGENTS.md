# AGENTS.md

## What this is
An Arduino sketch (C++) that makes a board act as a GameCube controller. It uses
the NicoHood Nintendo library (`CGamecubeConsole`, the "device/act-as-controller"
class) to respond to a GameCube console's polls, and takes its input state from a
binary protocol on the serial port. It is compiled for an Arduino Nano (ATmega328P,
16 MHz) with the Arduino CLI.

## Hardware wiring
The Arduino acts as a GameCube *controller* plugged into a console. The protocol runs
at **3.3V logic**, and the library's `gc_n64_send()` **actively drives the data line HIGH
to the AVR supply (5V)**, not open-drain — so a **bidirectional logic-level converter
(e.g. BSS138 module) is required** between the Arduino data pin and the cable's data
line. Direct connection risks damaging the console port.

GameCube controller cable (7-pin plug, 6 used — pin 5 is N/C). Colours vary by
manufacturer; the common scheme is below, but verify with a multimeter before powering
on (red DATA and blue 3.3V are the dangerous pair to swap).

| Lead | Pin | Function |
|------|-----|----------|
| Red | 2 | DATA (bidirectional, 3.3V) |
| Blue | 6 | 3.3V logic supply |
| Yellow | 1 | 5V (rumble only — unused here) |
| Green | 3 | GND |
| White | 4 | GND |
| Black | 7 | GND / shield |

Connections (console supplies its own power; Arduino is USB-powered):

- Blue (3.3V) → level shifter `VCC_LV` (low-side ref)
- Red (DATA) → level shifter `LV1`; level shifter `HV1` → Arduino `D8` (`GC_PIN`)
- Arduino `5V` → level shifter `VCC_HV` (high-side ref)
- Green / White / Black (GND) → level shifter `GND` and Arduino `GND` (common ground)
- Yellow (5V) → leave disconnected (rumble not used)
- The console's port already provides the ~1k pull-up on the data line to 3.3V.

## Build
- Toolchain: `arduino-cli` with the `arduino:avr` core and the `Nintendo` library
  (v1.4.0, from the Arduino library index).
- Target board: `arduino:avr:nano`. The attached Nano has the **old bootloader**
  (57600 baud), so always use `--fqbn arduino:avr:nano:cpu=atmega328old` for both
  compile and upload. The bare `arduino:avr:nano` actually selects `cpu=atmega328`
  (optiboot, 115200) and will NOT sync with this board.
- The sketch folder is the repo root. `main.cpp` holds the program (`setup()`/`loop()`);
  `gc-controller-arduino.ino` is an intentionally near-empty entry so the folder is a
  valid Arduino sketch. Do not delete it — arduino-cli refuses to compile a folder
  without a matching `.ino`.
- Build:
  ```
  arduino-cli compile --fqbn arduino:avr:nano:cpu=atmega328old .
  ```
  (compile confirms the library's `#if (F_CPU != 16000000)` check: this only supports
  16 MHz AVRs, so do not build for a 8 MHz variant.)

## Flash / upload
- Board must be wired to the host as `/dev/ttyUSB0`.
- The upload needs write access to the serial port. `docker`/container setups commonly
  create `/dev/ttyUSB0` as `root:storage 660`; the `coder` user is not in `storage` and
  has no passwordless sudo. If you get `cannot open port /dev/ttyUSB0: Permission denied`,
  ask for (or run) `sudo chmod 666 /dev/ttyUSB0` (one-time; reverts if unplugged) or add
  the user to `storage`.
- Upload:
  ```
  arduino-cli upload -p /dev/ttyUSB0 --fqbn arduino:avr:nano:cpu=atmega328old .
  ```
- If `arduino-cli upload` reports `not in sync` / `programmer is not responding`, it
  usually means a bootloader baud mismatch. The reliable low-level fallback is
  `avrdude` at 57600 (path under `~/.arduino15/packages/arduino/tools/avrdude/...`):
  ```
  arduino-cli compile --fqbn arduino:avr:nano:cpu=atmega328old --build-path /tmp/gc-build .
  avrdude -C <avrdude.conf> -v -p atmega328p -c arduino -P /dev/ttyUSB0 -b 57600 \
    -D -U flash:w:/tmp/gc-build/gc-controller-arduino.ino.hex:i
  ```
  The device signature reads back as `1E 95 0F` (ATmega328P).

## Serial protocol (host -> Arduino), 8N1 @ 115200
Each frame is 10 bytes: sync `0xA5`, then the 8-byte `Gamecube_Report_t`
(`buttons0, buttons1, xAxis, yAxis, cxAxis, cyAxis, left, right`), then an XOR
checksum of those 8 bytes. A centred/all-released report is
`0x00 0x80 0x80 0x80 0x80 0x80 0x1F 0x1F`. The sync byte re-synchronises framing and
the checksum rejects corrupt frames. Host code is not yet in this repo.

### Text command: `ID\n`
When the parser is idle (not mid-frame) it also accepts printable ASCII command
lines terminated by `\n` (CRLF is tolerated). Commands are matched
case-sensitively and unknown lines are silently ignored.

- `ID\n` → the Arduino replies with a single line:
  ```
  GCCTRL:1 firmware=1.1 proto=2 report=8 frame=10 baud=115200
  ```
  This is the reliable way to confirm the sketch is running and to learn the
  current frame layout / baud before sending binary frames.

## Known limitation
`CGamecubeConsole::write()` disables interrupts while it blocks waiting for the console
to poll, so bytes arriving on the serial port at the same time can be dropped. The
sync+checksum framing makes this recoverable (bad frames are dropped), but serial input
is not guaranteed lossless while a console is actively polling.
