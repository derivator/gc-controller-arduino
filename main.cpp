/*
 * GameCube controller emulator driven over serial.
 *
 * Uses the NicoHood Nintendo library (https://github.com/nicohood/nintendo).
 * The Arduino acts as a GameCube *controller* plugged into a console:
 * it waits for the console to poll it and replies with the latest input state.
 * That input state is received over the serial port (USB) as binary frames.
 *
 * Wiring
 * ------
 *  - GC_PIN (single bidirectional data line) -> the GameCube console's
 *    controller port data pin. Use a 1k pull-up to 3.3V and level-shift to
 *    3.3V logic: the GameCube protocol runs at 3.3V, but the ATmega328P
 *    drives 5V. The library assumes a 16 MHz AVR (see Nintendo.h).
 *
 * Serial protocol (host -> Arduino), 8N1 at SERIAL_BAUD
 * ------------------------------------------------------
 * Each frame is FRAME_LEN bytes:
 *   [0]   sync byte   FRAME_SYNC (0xA5)
 *   [1]   report[0]   buttons0   (a,b,x,y,start,origin,errlatch,errstat)
 *   [2]   report[1]   buttons1   (dpad,z,r,l ...)
 *   [3]   report[2]   xAxis      (analog stick X, 0x80 = centre)
 *   [4]   report[3]   yAxis      (analog stick Y, 0x80 = centre)
 *   [5]   report[4]   cxAxis     (C-stick X, 0x80 = centre)
 *   [6]   report[5]   cyAxis     (C-stick Y, 0x80 = centre)
 *   [7]   report[6]   left       (analog L, 0x1F = released)
 *   [8]   report[7]   right      (analog R, 0x1F = released)
 *   [9]   checksum    XOR of report[0..7]
 *
 * A centred, all-buttons-released report is:
 *   0x00 0x80 0x80 0x80 0x80 0x80 0x1F 0x1F
 *
 * The sync byte lets the parser re-synchronise if a byte is lost, and the
 * checksum rejects corrupt frames.
 *
 * Text commands (host -> Arduino), line-based, same port
 * -------------------------------------------------------
 * When the parser is idle (not mid-frame) it also accepts printable ASCII
 * command lines terminated by '\n'. Commands are matched case-sensitively:
 *
 *   ID\n      -> identify: reply with a single line describing this device
 *                (name, firmware, protocol version, frame layout, baud).
 *
 * Commands and binary frames are mutually exclusive on the wire: the host
 * should not interleave them, but a stray command line sent while idle is
 * harmless (it is dropped if it does not match a known command).
 *
 * Known limitation
 * ----------------
 * Responding to the console's poll disables interrupts for the whole timing
 * critical transaction (we block waiting for the console to poll, then sample
 * the command bits and send the reply). Bytes arriving on the serial port at
 * the same time can be dropped, so serial input is not guaranteed to be
 * lossless while a console is actively polling. The sync byte + checksum
 * framing makes this recoverable (bad frames are dropped), and the host should
 * resend frames while a button is held (see gc_button_test.py).
 */

#include <Nintendo.h>
#include <string.h>

// Set to 1 to print diagnostic counters (RPT/STAT lines) on the serial port.
// Keep 0 for normal operation: the debug output would otherwise accumulate in
// the host's RX buffer if the host is not reading it, and could eventually
// block Serial.write() and stall the controller.
#define ENABLE_DEBUG 0

// Pin wired to the GameCube console controller port data line.
#define GC_PIN 8

// Serial port baud rate for receiving input frames.
#define SERIAL_BAUD 115200

// Byte that starts each frame; used for framing / re-sync.
#define FRAME_SYNC 0xA5

// Payload length: the 8-byte Gamecube_Report_t.
#define REPORT_LEN 8
// Total frame length: sync byte + report + checksum.
#define FRAME_LEN (REPORT_LEN + 2)

// Identification values reported in reply to the "ID" command.
#define FIRMWARE_VERSION "1.2"
// Bumped to 3: the single-pass read replaces CGamecubeConsole::write().
#define PROTOCOL_VERSION 3

// The Arduino acts as a GameCube controller plugged into a console.
// (We don't use CGamecubeConsole directly; its write() stalls on the 1-byte
// init command because gc_n64_get(len=3) waits for bytes 1-2. Instead we read
// the command with a single-pass reader that stops after byte 0 unless it is a
// read command, so we can answer init/origin inside the console's window.)
static const Gamecube_Status_t status = defaultGamecubeData.status;
static const Gamecube_Origin_t origin = defaultGamecubeData.origin;

// Latest controller report, starting from the default centred state.
static Gamecube_Report_t report = defaultGamecubeData.report;

// Frame assembly buffer.
static uint8_t frame[FRAME_LEN];
static uint8_t frameIdx = 0;

// Line-based text command buffer (used when the parser is idle).
#define CMD_LEN 16
static char cmd[CMD_LEN];
static uint8_t cmdLen = 0;

// --- Diagnostic counters (DEBUG) -------------------------------------------------
static uint32_t cntFrameOK = 0;    // valid frames received (checksum passed)
static uint32_t cntFrameBad = 0;   // frames rejected (bad checksum)
static uint32_t cntPoll = 0;       // console polls detected (any command byte)
static uint32_t cntInit = 0;       // init commands answered
static uint32_t cntOrigin = 0;     // origin commands answered
static uint32_t cntRead = 0;       // read (0x40) commands answered
static uint32_t cntNoCmd = 0;      // polls that matched no known command
static uint32_t cntReadA = 0;      // read commands answered with A pressed
static uint32_t cntReadRel = 0;    // read commands answered with A released
static uint8_t  lastReport[REPORT_LEN];
static uint8_t  haveLastReport = 0;
static uint32_t lastStatMs = 0;
static uint32_t lastRptMs = 0;
// --------------------------------------------------------------------------------

// Reply to the "ID" command with a single machine-parseable line.
static void printIdentify()
{
    Serial.print(F("GCCTRL:1 firmware="));
    Serial.print(F(FIRMWARE_VERSION));
    Serial.print(F(" proto="));
    Serial.print(PROTOCOL_VERSION);
    Serial.print(F(" report="));
    Serial.print(REPORT_LEN);
    Serial.print(F(" frame="));
    Serial.print(FRAME_LEN);
    Serial.print(F(" baud="));
    Serial.println(SERIAL_BAUD);
}

// Handle a complete, NUL-terminated command line (without the trailing '\n').
static void processCommand()
{
    if (strcmp(cmd, "ID") == 0) {
        printIdentify();
    }
    // Unknown commands are silently ignored.
}

// Accumulate one byte of a text command line; act on the line terminator.
static void processCommandByte(uint8_t b)
{
    if (b == '\n') {
        cmd[cmdLen] = '\0';
        processCommand();
        cmdLen = 0;
        return;
    }
    if (b == '\r') {
        // Tolerate CRLF line endings.
        return;
    }
    // Any non-printable byte mid-command aborts the current command.
    if (b < 0x20 || b > 0x7E) {
        cmdLen = 0;
        return;
    }
    if (cmdLen < CMD_LEN - 1) {
        cmd[cmdLen++] = (char)b;
    } else {
        // Overflow: discard the command and re-sync on the next line.
        cmdLen = 0;
    }
}

// Feed one serial byte into the frame parser.
static void processByte(uint8_t b)
{
    // Only start (or re-start) a frame on the sync byte.
    if (frameIdx == 0) {
        if (b == FRAME_SYNC) {
            frame[0] = b;
            frameIdx = 1;
        } else {
            // Idle: treat non-sync bytes as a text command line.
            processCommandByte(b);
        }
        return;
    }

    frame[frameIdx++] = b;

    if (frameIdx == FRAME_LEN) {
        // Verify the checksum (XOR of the 8 report bytes).
        uint8_t checksum = 0;
        for (uint8_t i = 0; i < REPORT_LEN; i++) {
            checksum ^= frame[1 + i];
        }

        if (checksum == frame[FRAME_LEN - 1]) {
            // Valid frame: copy the report and forward it on the next poll.
            memcpy(&report, &frame[1], REPORT_LEN);
            cntFrameOK++;
#if ENABLE_DEBUG
            // If the report state changed, print it (throttled so a flickering
            // report cannot flood the serial link).
            if (!haveLastReport ||
                memcmp(report.raw8, lastReport, REPORT_LEN) != 0) {
                uint32_t now = millis();
                if (now - lastRptMs >= 2) {
                    lastRptMs = now;
                    Serial.print(F("RPT:"));
                    Serial.print(now);
                    Serial.print(F(":"));
                    for (uint8_t i = 0; i < REPORT_LEN; i++) {
                        uint8_t v = report.raw8[i];
                        if (v < 0x10) Serial.print('0');
                        Serial.print(v, HEX);
                    }
                    Serial.println();
                    memcpy(lastReport, report.raw8, REPORT_LEN);
                    haveLastReport = 1;
                }
            }
#endif
        } else {
            cntFrameBad++;
        }
        // Reset regardless; bad frames are simply dropped.
        frameIdx = 0;
    }
}

//================================================================================
// Single-pass GameCube command reader
//================================================================================
// The library's gc_n64_get(buff, len=3) reads all 3 command bytes in one pass,
// but it waits for bytes 1-2 even for the 1-byte init/origin commands. That
// exceeds the console's ~80us init response window, so the handshake stalls.
//
// This reader reads byte 0. If it is a read command (0x40) it keeps reading
// bytes 1-2 in the same continuous bit loop (no inter-call gap, which is what
// caused byte 1 to be misread as 0x83 earlier). For init (0x00/0xFF) and
// origin (0x41/0x42) it stops after byte 0 and we answer immediately.
//
// Returns the number of bytes read (1 for init/origin, 3 for read, 0 on a
// timeout with no command). The pin is left in input mode with the pullup on.
//
// nop helper (same as the library's Gamecube_N64.c).
#define nopManual(n) nopn ## n
#define nopn0 // (0 % 3)
#define nopn1 "nop\n" // (1 % 3)
#define nopn2 "nop\nnop\n" // (2 % 3)
#define nopn3 nopn0
#define nopn4 nopn1
#define nopn5 nopn2
#define nopn6 nopn0
#define nopn7 nopn1
#define nopn8 nopn2
#define nopn9 nopn0

uint8_t gc_read_command(uint8_t* buff, volatile uint8_t* modePort,
    volatile uint8_t* outPort, volatile uint8_t* inPort, uint8_t bitMask)
{
    // prepare pin for input with pullup
    *modePort &= ~bitMask;
    *outPort |= bitMask;

    register uint8_t timeoutCount;
    register uint8_t bitCount;
    register uint8_t inputVal;
    register uint8_t data;
    register uint8_t receivedBytes;
    register uint8_t initialTimeoutCount;

    asm volatile (
        "ldi %[bitCount],0x08\n"                // bitcount = 8
        "ldi %[receivedBytes],0x00\n"           // received = 0
        "ldi %[initialTimeoutCount],0x00\n"     // outer timeout

        // Wait for the line to go low (command start). Long timeout so we catch
        // a console that only polls every ~66ms when nothing is connected.
        ".L%=_wait_for_low_extra:\n"
        "ldi %[timeoutCount],0x00\n"
        ".L%=_wait_for_low_loop_extra1:\n"
        "ld %[inputVal], %a[inPort]\n"
        "and %[inputVal], %[bitMask]\n"
        "breq .L%=_wait_for_measure\n"
        "dec %[timeoutCount]\n"
        "brne .L%=_wait_for_low_loop_extra1\n"

        "ldi %[timeoutCount],0x00\n"
        ".L%=_wait_for_low_loop_extra2:\n"
        "ld %[inputVal], %a[inPort]\n"
        "and %[inputVal], %[bitMask]\n"
        "breq .L%=_wait_for_measure\n"
        "dec %[timeoutCount]\n"
        "brne .L%=_wait_for_low_loop_extra2\n"

        ".L%=_wait_for_low:\n"
        "ldi %[timeoutCount],%[timeout]\n"
        ".L%=_wait_for_low_loop:\n"
        "ld %[inputVal], %a[inPort]\n"
        "and %[inputVal], %[bitMask]\n"
        "breq .L%=_wait_for_measure\n"
        "dec %[timeoutCount]\n"
        "brne .L%=_wait_for_low_loop\n"

        "dec %[initialTimeoutCount]\n"
        "brne .L%=_wait_for_low_extra\n"
        "rjmp .L%=_exit\n"

        // Line just went low. Disable interrupts for the timing-critical
        // sampling (they were left on during the idle wait so serial frames can
        // still be received between console polls). Align to the middle of the
        // first bit.
        ".L%=_wait_for_measure:\n"
        "cli\n"
        "ldi %[inputVal], (23/3)\n"
        ".L%=_nop_loop1:\n"
        "dec %[inputVal]\n"
        "brne .L%=_nop_loop1\n"
        nopManual(1)
        "ldi %[initialTimeoutCount],1\n"

        // Read one bit (LSB first).
        "lsl %[data]\n"
        "ld %[inputVal], %a[inPort]\n"
        "and %[inputVal], %[bitMask]\n"
        "breq .L%=_check_bit_count\n"
        "sbr %[data],0x01\n"
        ".L%=_check_bit_count:\n"
        "dec %[bitCount]\n"
        "brne .L%=_wait_for_high\n"

        // We received a full byte.
        "st %a[buff]+,%[data]\n"
        "inc %[receivedBytes]\n"
        "ldi %[bitCount],0x08\n"

        // If this is byte 0 and it is NOT a read command (0x40), stop here.
        // (We must respond to init/origin immediately, no waiting for more bytes.)
        "cpi %[receivedBytes],1\n"
        "brne .L%=_maybe_more\n"
        "cpi %[data],0x40\n"
        "breq .L%=_maybe_more\n"
        "rjmp .L%=_exit\n"
        ".L%=_maybe_more:\n"
        "cpi %[receivedBytes],3\n"
        "breq .L%=_exit\n"

        // Wait for the line to go high (between bytes), then low for next byte.
        ".L%=_wait_for_high:\n"
        "ldi %[timeoutCount],%[timeout]\n"
        ".L%=_wait_for_high_loop:\n"
        "ld %[inputVal], %a[inPort]\n"
        "and %[inputVal], %[bitMask]\n"
        "brne .L%=_wait_for_low\n"
        "dec %[timeoutCount]\n"
        "brne .L%=_wait_for_high_loop\n"
        ".L%=_exit:\n"

        : [receivedBytes] "=&d" (receivedBytes),
          [buff] "+e" (buff),
          [bitCount] "=&d" (bitCount),
          [timeoutCount] "=&d" (timeoutCount),
          [initialTimeoutCount] "=&d" (initialTimeoutCount),
          [inputVal] "=&d" (inputVal),
          [data] "=&d" (data)
        : [inPort] "e" (inPort),
          [bitMask] "r" (bitMask),
          [timeout] "M" (128)
    );

    return receivedBytes;
}

// Handle one console transaction: read the command and reply accordingly.
// Returns the library-style ret code (1 init, 2 origin, 3 read, 0 none).
static uint8_t respondToConsole()
{
    uint8_t command[3] = {0, 0, 0};

    volatile uint8_t* modePort = portModeRegister(digitalPinToPort(GC_PIN));
    volatile uint8_t* outPort = portOutputRegister(digitalPinToPort(GC_PIN));
    volatile uint8_t* inPort = portInputRegister(digitalPinToPort(GC_PIN));
    uint8_t bitMask = digitalPinToBitMask(GC_PIN);

    // Interrupts stay ON during the idle wait so serial frames can still be
    // received between console polls. gc_read_command() disables them itself
    // (cli) the moment it detects the falling edge, so the timing-critical
    // sampling + response runs with interrupts off. We save SREG here and
    // restore it afterwards so the function leaves interrupts as it found them.
    uint8_t oldSREG = SREG;

    uint8_t n = gc_read_command(command, modePort, outPort, inPort, bitMask);

    if (n > 0) cntPoll++;

    if (n == 1 && (command[0] == 0x00 || command[0] == 0xFF)) {
        // Init: send the controller status (3 bytes).
        gc_n64_send(status.raw8, sizeof(status), modePort, outPort, bitMask);
        n = 1;
        cntInit++;
    } else if (n == 1 && (command[0] == 0x41 || command[0] == 0x42)) {
        // Origin: send the initial stick positions (10 bytes).
        gc_n64_send(origin.raw8, sizeof(origin), modePort, outPort, bitMask);
        n = 2;
        cntOrigin++;
    } else if (n >= 3 && command[0] == 0x40) {
        // Read: send the current report (8 bytes).
        gc_n64_send(report.raw8, sizeof(report), modePort, outPort, bitMask);
        n = 3;
        cntRead++;
        if (report.raw8[0] & 0x01) cntReadA++;   // A bit (buttons0 bit 0)
        else cntReadRel++;
    } else {
        n = 0;
        cntNoCmd++;
    }

    SREG = oldSREG;
    return n;
}

void setup()
{
    Serial.begin(SERIAL_BAUD);
}

void loop()
{
    // Drain any incoming serial bytes into the current report.
    while (Serial.available() > 0) {
        processByte(Serial.read());
    }

    // Respond to the console's next poll with the latest report.
    // This blocks until the console polls (or times out if none is connected).
    respondToConsole();

#if ENABLE_DEBUG
    // DEBUG: dump counters once per second.
    uint32_t now = millis();
    if (now - lastStatMs >= 1000) {
        lastStatMs = now;
        Serial.print(F("STAT:"));
        Serial.print(F(" poll=")); Serial.print(cntPoll);
        Serial.print(F(" init=")); Serial.print(cntInit);
        Serial.print(F(" origin=")); Serial.print(cntOrigin);
        Serial.print(F(" read=")); Serial.print(cntRead);
        Serial.print(F(" ncmd=")); Serial.print(cntNoCmd);
        Serial.print(F(" ok=")); Serial.print(cntFrameOK);
        Serial.print(F(" bad=")); Serial.print(cntFrameBad);
        Serial.print(F(" rpt=")); Serial.print(haveLastReport);
        Serial.print(F(" mem=")); Serial.print(Serial.available());
        Serial.print(F(" rdA=")); Serial.print(cntReadA);
        Serial.print(F(" rdRel=")); Serial.print(cntReadRel);
        Serial.print(F(" up=")); Serial.println(millis());
    }
#endif
}
