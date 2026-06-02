/**
 * @file    01_QuickRead.ino
 * @brief   Example 1 — smoke test. Reads U / I / P / PF / freq once per second.
 *
 * Wiring (ESP32 example):
 *   rbAmp SDA -> GPIO21
 *   rbAmp SCL -> GPIO22
 *   rbAmp GND -> GND
 *   rbAmp 3V3 -> 3V3 (or 5V if the module has on-board regulator)
 *
 * Works on AVR (Uno/Mega), ESP32, ESP8266 and STM32duino out of the box.
 * Adjust @c SDA / @c SCL pin macros if your board needs explicit pins.
 *
 * Expected output (UI1 variant):
 *   topology=1 channels=1 has_voltage=yes
 *   U=230.4V f=50.0Hz   I0=2.13A P0=  489.0W PF0=+0.98
 *   ...
 */
#include <Wire.h>
#include <RbAmp.h>

static RbAmp dev(Wire, 0x50);

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { /* wait for USB-CDC on native ports */ }
    Serial.println();
    Serial.println(F("rbAmp — Example 1: QuickRead"));

    Wire.begin();
    /* Optional: Wire.setClock(400000); // bus is happy at 400 kHz */

    dev.setLogStream(&Serial);  /* prints variant-detect + errors */

    while (!dev.begin()) {
        Serial.print(F("begin() failed: "));
        Serial.println(RbAmp::errorString(dev.lastError()));
        delay(1000);
    }
    Serial.print(F("topology=")); Serial.print((int)dev.topology());
    Serial.print(F(" channels=")); Serial.print(dev.channels());
    Serial.print(F(" has_voltage=")); Serial.println(dev.hasVoltageHw() ? F("yes") : F("no"));
}

void loop() {
    RbAmpSnapshot s;
    if (!dev.readAll(s)) {
        Serial.print(F("read failed: "));
        Serial.println(RbAmp::errorString(dev.lastError()));
        delay(1000);
        return;
    }
    Serial.print(F("U=")); Serial.print(s.voltage, 1); Serial.print(F("V "));
    Serial.print(F("f=")); Serial.print(s.frequency, 1); Serial.print(F("Hz   "));
    for (uint8_t ch = 0; ch < s.channels; ++ch) {
        Serial.print(F("I")); Serial.print(ch); Serial.print(F("=")); Serial.print(s.current[ch], 2); Serial.print(F("A "));
        Serial.print(F("P")); Serial.print(ch); Serial.print(F("=")); Serial.print(s.power[ch], 1); Serial.print(F("W "));
        Serial.print(F("PF")); Serial.print(ch); Serial.print(F("=")); Serial.print(s.power_factor[ch], 2); Serial.print(F("  "));
    }
    Serial.println();
    delay(1000);
}
