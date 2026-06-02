/**
 * @file    06_BidirectionalEnergy.ino
 * @brief   Example 6 — master-side bidirectional energy accounting.
 *
 * Demonstrates how to split incoming (consume) vs outgoing (export) energy
 * from one rbAmp channel. The device reports SIGNED real power; the library
 * accumulator sums it as-is. To separate the two flows we sample real power
 * at the RT cadence (200 ms) and integrate consume/export buckets manually.
 *
 * The library's built-in energy accumulator (dev.energy().wh(0)) yields the
 * NET balance. This sketch keeps two additional double-precision counters
 * for gross consumption and gross export.
 *
 * Useful for solar-with-battery installations where you bill differently
 * for kWh imported vs exported.
 */
#include <Wire.h>
#include <RbAmp.h>

static RbAmp dev(Wire, 0x50);

static double consume_wh = 0.0;
static double export_wh  = 0.0;
static uint32_t last_sample_ms = 0;

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000) {}
    Wire.begin();
    dev.setLogStream(&Serial);
    while (!dev.begin()) {
        Serial.println(F("rbAmp begin failed"));
        delay(500);
    }
    last_sample_ms = millis();
}

void loop() {
    /* Sample RT real power every 200 ms — matches device's commit cadence */
    if (millis() - last_sample_ms < 200) {
        delay(5);
        return;
    }
    const uint32_t now_ms = millis();
    const uint32_t dt_ms = now_ms - last_sample_ms;
    last_sample_ms = now_ms;

    const float p_w = dev.readPower(0);
    if (isnan(p_w)) {
        Serial.print(F("read fail: "));
        Serial.println(RbAmp::errorString(dev.lastError()));
        return;
    }
    const double dwh = static_cast<double>(p_w) * dt_ms / 3600000.0;  /* dt_ms→h via 3,600,000 */
    if (p_w >= 0.0f) {
        consume_wh += dwh;
    } else {
        export_wh  += -dwh;  /* p_w is negative → -dwh is positive */
    }

    /* Print 5×/s */
    static uint32_t next_print = 0;
    if (millis() >= next_print) {
        next_print = millis() + 200;
        Serial.print(F("P=")); Serial.print(p_w, 1); Serial.print(F("W   "));
        Serial.print(F("consume=")); Serial.print(consume_wh, 4); Serial.print(F("Wh  "));
        Serial.print(F("export="));  Serial.print(export_wh, 4);  Serial.print(F("Wh  "));
        Serial.print(F("net="));     Serial.print(consume_wh - export_wh, 4); Serial.println(F("Wh"));
    }
}
