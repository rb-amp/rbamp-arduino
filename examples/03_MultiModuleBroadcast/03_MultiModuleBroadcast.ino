/**
 * @file    03_MultiModuleBroadcast.ino
 * @brief   Example 3 — three rbAmp modules on one bus, period-synced by a
 *          SINGLE v1.3 General-Call BROADCAST LATCH (coherent, one frame).
 *
 * @par v1.3 General-Call latch
 * v1.3 firmware implements the General-Call broadcast latch (CAP_GC_LATCH): a
 * module opts in with enableGc(true), then one all-call frame to address 0x00
 * latches every enabled module's period accumulator on the same instant —
 * coherent by construction, no per-device skew to measure. This supersedes the
 * old per-device sequential latchPeriod() loop this example used on v1
 * firmware. On v1.0/v1.1 (GC disabled) the broadcast is harmlessly ignored;
 * there you would fall back to sequential latchPeriod() (see the git history of
 * this file, or example 08_FleetSync for the RbAmpFleet manager).
 *
 * Demonstrates:
 *  - Multiple RbAmp instances sharing the same Wire bus.
 *  - enableGc() opt-in (persisted + reset; one-time in setup).
 *  - RbAmp::broadcastLatchGroup() — ONE frame latches the whole bus.
 *  - readPeriodSnapshot(snap, settle, skip_latch=true) — read the already-
 *    latched window without re-latching.
 *  - readGcTick() — verify each module accepted the broadcast tick (missed-
 *    frame / sync check).
 *
 * Each module must have a UNIQUE I2C address (assign each one once, one module
 * on the bus at a time — see example 5, or the RbAmpFleet provisioning helpers).
 *
 * Example output:
 *   tick 1: 3/3 in sync
 *     mod0 @0x50 ok dt=60011 P0=  234W  Wh0=12.345
 *     mod1 @0x51 ok dt=60011 P0=  108W  Wh0= 4.072
 *     mod2 @0x52 ok dt=60011 P0=   75W  Wh0= 1.954
 *     TOTAL P=417W  Wh=18.371
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

static const uint8_t  GC_GROUP  = 0x00;      /* 0x00 = all-call (latch every module) */
static const uint32_t WINDOW_MS = 60000UL;   /* 60 s billing window */

static uint32_t next_tick_ms = 0;
static uint16_t g_tick       = 0;

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000) {}
    Wire.begin();
    Wire.setClock(50000);   /* ESP32-Arduino rule; a non-ESP32 host uses 100 kHz. NOT 400 kHz. */

    for (uint8_t i = 0; i < N_MODULES; ++i) {
        modules[i]->setLogStream(&Serial);
        if (!modules[i]->begin()) {
            Serial.print(F("module ")); Serial.print(i);
            Serial.print(F(" begin failed: "));
            Serial.println(RbAmp::errorString(modules[i]->lastError()));
            continue;
        }
        /* Opt this module into General-Call latch (persisted + reset; ~1 s). */
        modules[i]->enableGc(true);
        Serial.print(F("module ")); Serial.print(i);
        Serial.print(F(" @0x")); Serial.print(modules[i]->address(), HEX);
        Serial.print(F(" channels=")); Serial.println(modules[i]->channels());
    }
    next_tick_ms = millis() + WINDOW_MS;
}

void loop() {
    if (millis() < next_tick_ms) {
        delay(50);
        return;
    }
    next_tick_ms = millis() + WINDOW_MS;
    g_tick++;

    /* 1. ONE broadcast latches every GC-enabled module coherently. */
    RbAmp::broadcastLatchGroup(Wire, GC_GROUP, g_tick);

    /* 2. Single shared settle for all modules. */
    delay(50);

    /* 3. Read each module's already-latched snapshot (skip_latch=true) and
     *    confirm it accepted this tick. Aggregate a fleet total. */
    uint8_t  in_sync = 0;
    float    p_total = 0.0f;
    double   wh_total = 0.0;
    char     lines[N_MODULES][80];

    for (uint8_t i = 0; i < N_MODULES; ++i) {
        RbAmpPeriodSnapshot snap;
        const bool ok = modules[i]->readPeriodSnapshot(snap, /*settle_ms=*/0, /*skip_latch=*/true);

        uint16_t tick = 0;
        const bool synced = modules[i]->readGcTick(tick) && (tick == g_tick);
        if (synced) in_sync++;

        if (!ok) {
            snprintf(lines[i], sizeof(lines[i]), "  mod%u @0x%02X ERR %s",
                     i, modules[i]->address(), RbAmp::errorString(modules[i]->lastError()));
            continue;
        }
        for (uint8_t ch = 0; ch < modules[i]->channels(); ++ch)
            p_total += snap.avg_p[ch];
        wh_total += modules[i]->energy().wh(0);

        snprintf(lines[i], sizeof(lines[i]),
                 "  mod%u @0x%02X ok dt=%lu P0=%.0fW Wh0=%.3f%s",
                 i, modules[i]->address(),
                 static_cast<unsigned long>(snap.master_dt_ms),
                 snap.avg_p[0],
                 modules[i]->energy().wh(0),
                 synced ? "" : "  [missed tick]");
    }

    Serial.print(F("tick ")); Serial.print(g_tick);
    Serial.print(F(": ")); Serial.print(in_sync);
    Serial.print(F("/")); Serial.print(N_MODULES);
    Serial.println(F(" in sync"));
    for (uint8_t i = 0; i < N_MODULES; ++i) Serial.println(lines[i]);
    Serial.print(F("  TOTAL P=")); Serial.print(p_total, 0);
    Serial.print(F("W  Wh=")); Serial.println(wh_total, 3);
    Serial.println();
}
