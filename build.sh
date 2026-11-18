#!/usr/bin/env bash
#
# Build (and optionally flash) the GameCube controller Arduino sketch.
#
# The sketch lives in this directory (the repo root). The attached Nano uses
# the OLD bootloader (57600 baud), so it is always compiled/uploaded with
#
#     arduino:avr:nano:cpu=atmega328old
#
# The bare `arduino:avr:nano` selects the optiboot 115200 variant and will NOT
# sync with this board (the classic "not in sync" error).
#
# Usage:
#   ./build.sh                      compile only (no flash)
#   ./build.sh --flash              compile and upload to the Nano
#   ./build.sh --port /dev/ttyUSB1  use a different serial port
#   ./build.sh --clean              remove generated build output and exit
#   ./build.sh --avrdude            flash via the avrdude fallback (low-level)
#
# The avrdude fallback is used when the bootloader won't sync over arduino-cli
# upload (see AGENTS.md). It needs the .hex built by arduino-cli.

set -euo pipefail

# --- Defaults ----------------------------------------------------------
SKETCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="/dev/ttyUSB0"
FQBN="arduino:avr:nano:cpu=atmega328old"
FLASH=0
AVRDUDE=0
CLEAN=0

# arduino-cli may live on PATH or in ~/.local/bin.
ARDUINO_CLI="$(command -v arduino-cli || true)"
if [ -z "$ARDUINO_CLI" ] && [ -x "$HOME/.local/bin/arduino-cli" ]; then
    ARDUINO_CLI="$HOME/.local/bin/arduino-cli"
fi
if [ -z "$ARDUINO_CLI" ]; then
    echo "error: arduino-cli not found (install it or add it to PATH)" >&2
    exit 1
fi

# --- Parse arguments ---------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --flash|-f) FLASH=1 ;;
        --avrdude|-a) AVRDUDE=1 ;;
        --clean|-c) CLEAN=1 ;;
        --port) PORT="$2"; shift ;;
        --port=*) PORT="${1#--port=}" ;;
        -h|--help)
            grep '^#' "$0" | sed 's/^# \{0,1\}//' | head -n 20
            exit 0
            ;;
        *) echo "error: unknown option '$1'" >&2; exit 1 ;;
    esac
    shift
done

# --- Clean -------------------------------------------------------------
if [ "$CLEAN" -eq 1 ]; then
    rm -rf "$SKETCH_DIR/build"
    find "$SKETCH_DIR" -maxdepth 1 -type f \
        \( -name '*.i' -o -name '*.ii' -o -name '*.s' -o -name '*.o' \
           -o -name '*.elf' -o -name '*.hex' -o -name '*.bin' \) -delete
    echo "Cleaned build output."
    exit 0
fi

echo "Using arduino-cli: $ARDUINO_CLI"
echo "Sketch:            $SKETCH_DIR"
echo "FQBN:              $FQBN"

# --- Compile -----------------------------------------------------------
echo "==> Compiling ($FQBN)"
"$ARDUINO_CLI" compile --fqbn "$FQBN" "$SKETCH_DIR"

# --- Flash -------------------------------------------------------------
if [ "$FLASH" -eq 1 ]; then
    if [ "$AVRDUDE" -eq 1 ]; then
        # Low-level avrdude fallback for a bootloader baud mismatch.
        # Locate avrdude under the arduino15 packages directory.
        AVRDUDE_BIN="$(find "$HOME/.arduino15/packages/arduino/tools/avrdude" \
                        -name avrdude -type f 2>/dev/null | sort -V | tail -1 || true)"
        AVRDUDE_CONF="$(dirname "$(dirname "$AVRDUDE_BIN")")/etc/avrdude.conf"
        if [ -z "$AVRDUDE_BIN" ] || [ ! -x "$AVRDUDE_BIN" ]; then
            echo "error: avrdude not found under ~/.arduino15" >&2
            exit 1
        fi
        HEX="$(find "$SKETCH_DIR/build" -name '*.hex' 2>/dev/null | head -1)"
        if [ -z "$HEX" ]; then
            echo "error: no .hex in build output; run with --flash without --avrdude first" >&2
            exit 1
        fi
        echo "==> Flashing via avrdude (57600) on $PORT"
        "$AVRDUDE_BIN" -C "$AVRDUDE_CONF" -v -p atmega328p -c arduino \
            -P "$PORT" -b 57600 -D -U "flash:w:$HEX:i"
    else
        echo "==> Uploading via arduino-cli on $PORT"
        "$ARDUINO_CLI" upload -p "$PORT" --fqbn "$FQBN" "$SKETCH_DIR"
    fi
else
    echo "Compile OK. Use --flash to upload to $PORT."
fi

echo "Done."
