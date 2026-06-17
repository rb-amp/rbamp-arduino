/**
 * @file    03_MultiModuleBroadcast.ino
 * @brief   Example 3 — three rbAmp modules on one bus, period-synced via
 *          PER-DEVICE SEQUENTIAL LATCH (broadcast LATCH unavailable in v1).
 *
 * @par v1 firmware caveat (SPEC §9)
 * The v1 rbAmp firmware sets `I2C_GENERALCALL_DISABLE` and does not respond to
 * address 0x00. `RbAmp::broadcastLatch()` therefore returns @c false
 * without touching the wire (it is a @c static method and cannot set
 * @c lastError() — the @c false return is the only signal). Until v2 enables
 * the GC ISR handler, this sketch
 * synchronises three modules by issuing latchPeriod() to each device in turn
 * and measuring the per-device skew. Skew at 100 kHz is roughly 200-400 µs per
 * I2C transaction (write 2 bytes + STOP) — far below the 200 ms RT window, so
 * energy integration error is < 0.2 % per period.
 *
 * Demonstrates:
 *  - Multiple RbAmp instances sharing the same Wire bus.
 *  - Per-device sequential LATCH with shared settle.
 *  - readPeriodSnapshot(snap, settle, /\*skip_latch=\*\/true) — read without
 *    re-latching after the shared LATCH burst.
 *
 * Each module must have a UNIQUE I2C address (set with prepareAddressChange /
 * commitAddressChange — see example 5 — or the RbAmpFleet provisioning helpers).
 *
 * Example output:
 *   sync_us=312  mod0 ok dt=60012 P0=  234W  Wh0=12.345
 *                mod1 ok dt=60012 P0=  108W  Wh0= 4.072
 *                mod2 ok dt=60012 P0=   75W  Wh0= 1.954
 */
#include <Wire.h>
#include <RbAmp.h>

/* Module names mod0/mod1/mod2 — not T0/T1/T2, which collide with the
 * touch-pin macros (`static const uint8_t T0 = 4;`) exposed by ESP32 core
 * 3.x in `pins_arduino.h`. */
static RbAmp mod0(Wire, 0x50);
static RbAmp mod1(Wire, 0x51);
static RbAmp mod2(Wire, 0x52);
static RbAmp* modules[] = { &mod0, &mod1, &mod2 };
static const uint8_t N_MODULES = sizeof(modules) / sizeof(modules[0]);

static uint32_t next_tick_ms = 0;

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000) {}
    Wire.begin();
    Wire.setClock(400000);

    for (uint8_t i = 0; i < N_MODULES; ++i) {
        modules[i]->setLogStream(&Serial);
        if (!modules[i]->begin()) {
            Serial.print(F("module ")); Serial.print(i);
            Serial.print(F(" begin failed: "));
            Serial.println(RbAmp::errorString(modules[i]->lastError()));
        } else {
            Serial.print(F("module ")); Serial.print(i);
            Serial.print(F(" @0x")); Serial.print(modules[i]->address(), HEX);
            Serial.print(F(" channels=")); Serial.println(modules[i]->channels());
        }
    }
    next_tick_ms = millis() + 60000;
}

void loop() {
    if (millis() < next_tick_ms) {
        delay(50);
        return;
    }
    next_tick_ms = millis() + 60000;

    /* 1. Sequential LATCH — measure skew so caller knows how much error
     *    creeps into per-channel dt. v2: replace with broadcastLatch(). */
    const uint32_t sync_start_us = micros();
    for (uint8_t i = 0; i < N_MODULES; ++i) {
        modules[i]->latchPeriod();
    }
    const uint32_t sync_us = micros() - sync_start_us;

    /* 2. Single shared settle for all modules. */
    delay(50);

    /* 3. Read each module's snapshot — skip the latch, it already happened. */
    Serial.print(F("sync_us=")); Serial.print(sync_us); Serial.println();
    for (uint8_t i = 0; i < N_MODULES; ++i) {
        RbAmpPeriodSnapshot snap;
        const bool ok = modules[i]->readPeriodSnapshot(snap, /*settle_ms=*/0, /*skip_latch=*/true);
        Serial.print(F("           mod")); Serial.print(i);
        if (!ok) {
            Serial.print(F(" ERR "));
            Serial.println(RbAmp::errorString(modules[i]->lastError()));
            continue;
        }
        Serial.print(F(" ok dt=")); Serial.print(snap.master_dt_ms);
        for (uint8_t ch = 0; ch < modules[i]->channels(); ++ch) {
            Serial.print(F(" P")); Serial.print(ch); Serial.print(F("="));
            Serial.print(snap.avg_p[ch], 0); Serial.print(F("W"));
        }
        Serial.print(F("  Wh0=")); Serial.println(modules[i]->energy().wh(0), 3);
    }
    Serial.println();
}
