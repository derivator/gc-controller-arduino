/*
 * Minimal GameCube controller test: repeatedly press A.
 *
 * No serial input. It just makes the Arduino act as a GameCube controller
 * and hold the A button for 400 ms, then release it for 600 ms, forever.
 * This isolates whether the emulator/hardware wiring works at all, without
 * any dependency on the serial frame protocol.
 *
 * Wiring: GC_PIN (D8) -> level-shifted 3.3V data line to the console port.
 * See AGENTS.md for the wiring reference.
 */

#include <Nintendo.h>

// Pin wired to the GameCube console controller port data line.
#define GC_PIN 8

// On-board LED (D13): blinks in sync with the A-press so the sketch is
// visibly running even without a console/game connected.
#define LED_PIN LED_BUILTIN

// Timing, in milliseconds.
#define PRESS_MS 400
#define RELEASE_MS 600

// The Arduino acts as a GameCube controller plugged into a console.
static CGamecubeConsole console(GC_PIN);

void setup()
{
    // LED on during the press, off during the release.
    pinMode(LED_PIN, OUTPUT);
}

void loop()
{
    // --- Press A for PRESS_MS ---
    digitalWrite(LED_PIN, HIGH);
    Gamecube_Report_t report = defaultGamecubeData.report;
    report.buttons0 = 0x01; // bit 0 = A
    unsigned long start = millis();
    while (millis() - start < PRESS_MS) {
        // Blocks until the console polls (or times out), sending the report.
        console.write(report);
    }

    // --- Release A for RELEASE_MS ---
    digitalWrite(LED_PIN, LOW);
    report.buttons0 = 0x00; // all face buttons released
    start = millis();
    while (millis() - start < RELEASE_MS) {
        console.write(report);
    }
}
