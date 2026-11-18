/*
 * Log the full command sequence the adapter sends when we respond to init
 * immediately (gc_n64_get len=1). Prints command bytes and ret code so we can
 * see whether the adapter accepts the fast init and moves to read/origin.
 *
 * Prints each command for the first ~20, then a periodic summary.
 */

#include <Nintendo.h>

#define GC_PIN 8
#define LED_PIN LED_BUILTIN

static unsigned long count[7] = {0};
static unsigned long total = 0;

void setup()
{
    pinMode(LED_PIN, OUTPUT);
    Serial.begin(115200);
    Serial.println(F("GC seq: fast init, log commands. A pressed."));
}

void loop()
{
    Gamecube_Data_t data = defaultGamecubeData;
    data.report.buttons0 = 0x01; // A pressed

    uint8_t command[3];
    uint8_t ret = 0;

    uint8_t oldSREG = SREG;
    cli();

    volatile uint8_t* modePort = portModeRegister(digitalPinToPort(GC_PIN));
    volatile uint8_t* outPort = portOutputRegister(digitalPinToPort(GC_PIN));
    volatile uint8_t* inPort = portInputRegister(digitalPinToPort(GC_PIN));
    uint8_t bitMask = digitalPinToBitMask(GC_PIN);

    command[1] = command[2] = 0;
    uint8_t receivedBytes = gc_n64_get(command, 1, modePort, outPort, inPort, bitMask);

    if (receivedBytes == 1 && (command[0] == 0x00 || command[0] == 0xFF)) {
        gc_n64_send(data.status.raw8, sizeof(data.status), modePort, outPort, bitMask);
        ret = 1;
    }
    else if (receivedBytes == 1 && (command[0] == 0x41 || command[0] == 0x42)) {
        gc_n64_send(data.origin.raw8, sizeof(data.origin), modePort, outPort, bitMask);
        ret = 2;
    }
    else if (receivedBytes == 1 && command[0] == 0x40) {
        uint8_t more = gc_n64_get(command + 1, 2, modePort, outPort, inPort, bitMask);
        gc_n64_send(data.report.raw8, sizeof(data.report), modePort, outPort, bitMask);
        ret = 3;
    }

    SREG = oldSREG;

    if (ret <= 6) count[ret]++;
    total++;
    digitalWrite(LED_PIN, ret > 0 ? HIGH : LOW);

    if (total <= 20 || total % 200 == 0) {
        Serial.print(F("c0="));
        Serial.print(command[0], HEX);
        Serial.print(F(" c1="));
        Serial.print(command[1], HEX);
        Serial.print(F(" c2="));
        Serial.print(command[2], HEX);
        Serial.print(F(" ret="));
        Serial.print(ret);
        Serial.print(F(" n="));
        Serial.println(total);
    }
}
