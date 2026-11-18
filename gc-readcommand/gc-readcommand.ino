/*
 * Test: read the full GC command in a SINGLE continuous gc_n64_get-style pass,
 * so the read-command bytes are not split across two calls (which caused byte 1
 * to be misread as 0x83 instead of 0x03).
 *
 * Reads byte 0. If byte 0 is a read command (0x40) it keeps reading bytes 1-2
 * in the same bit loop (no gap). For init (0x00) / origin (0x41/0x42) it stops
 * after byte 0 and responds immediately.
 */

#include <Nintendo.h>

#define GC_PIN 8
#define LED_PIN LED_BUILTIN

// nop helper (same as the library's Gamecube_N64.c)
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

/*
 * Read a GameCube controller command.
 *  - Reads the first byte.
 *  - If the first byte is a read command (0x40), reads two more bytes (the
 *    mode / flags) in the same continuous loop.
 *  - Otherwise stops after the first byte.
 * Returns the number of bytes read (1 for init/origin, 3 for read, 0 if the
 * console never sent anything).
 */
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

        // Line just went low. Align to the middle of the first bit.
        ".L%=_wait_for_measure:\n"
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

void setup()
{
    pinMode(LED_PIN, OUTPUT);
    Serial.begin(115200);
    Serial.println(F("GC: single-pass command read, A pressed."));
}

void loop()
{
    Gamecube_Data_t data = defaultGamecubeData;
    data.report.buttons0 = 0x01; // A pressed

    uint8_t command[3] = {0, 0, 0};
    uint8_t ret = 0;

    uint8_t oldSREG = SREG;
    cli();

    volatile uint8_t* modePort = portModeRegister(digitalPinToPort(GC_PIN));
    volatile uint8_t* outPort = portOutputRegister(digitalPinToPort(GC_PIN));
    volatile uint8_t* inPort = portInputRegister(digitalPinToPort(GC_PIN));
    uint8_t bitMask = digitalPinToBitMask(GC_PIN);

    uint8_t n = gc_read_command(command, modePort, outPort, inPort, bitMask);

    if (n == 1 && (command[0] == 0x00 || command[0] == 0xFF)) {
        gc_n64_send(data.status.raw8, sizeof(data.status), modePort, outPort, bitMask);
        ret = 1;
    }
    else if (n == 1 && (command[0] == 0x41 || command[0] == 0x42)) {
        gc_n64_send(data.origin.raw8, sizeof(data.origin), modePort, outPort, bitMask);
        ret = 2;
    }
    else if (n >= 3 && command[0] == 0x40) {
        gc_n64_send(data.report.raw8, sizeof(data.report), modePort, outPort, bitMask);
        ret = 3;
    }

    SREG = oldSREG;

    digitalWrite(LED_PIN, ret > 0 ? HIGH : LOW);

    static unsigned long total = 0;
    total++;
    if (total <= 20 || total % 200 == 0) {
        Serial.print(F("c0="));
        Serial.print(command[0], HEX);
        Serial.print(F(" c1="));
        Serial.print(command[1], HEX);
        Serial.print(F(" c2="));
        Serial.print(command[2], HEX);
        Serial.print(F(" n="));
        Serial.print(n);
        Serial.print(F(" ret="));
        Serial.print(ret);
        Serial.print(F(" t="));
        Serial.println(total);
    }
}
