/**
 * @file    conformance.ino
 * @brief   Bench-smoke conformance test — exercises ~20 operations against
 *          a real DUT, emits one JSON line per op for ingestion by
 *          tools/lib_codegen/bench_smoke.py.
 *
 * Same operation list as every other platform's conformance sketch. The
 * shared validator consumes the JSON and asserts wire-level invariants
 * (period-valid handshake, settle times, byte order) hold byte-identically
 * across libraries.
 *
 * Run:
 *   arduino-cli compile --fqbn esp32:esp32:esp32 path/to/conformance
 *   arduino-cli upload  --fqbn esp32:esp32:esp32 --port COMx ...
 *   python tools/lib_codegen/bench_smoke.py --platform arduino --serial COMx
 *
 * The Python harness expects:
 *   - one JSON line per op on Serial @ 115200 (no other stdout traffic)
 *   - line ends with '\n', no '\r'
 *   - keys: op, ok, error (string), value (number or string, optional),
 *           ms (millis() since boot at op start)
 */
#include <Wire.h>
#include <RbAmp.h>

static RbAmp dev(Wire, 0x50);

/* Tiny JSON emitter — avoids ArduinoJson dependency in the conformance build. */
static void emit(const char* op, bool ok, const char* err = "",
                 const char* vkey = nullptr, double vnum = 0.0,
                 const char* vstr = nullptr) {
    Serial.print(F("{\"op\":\""));   Serial.print(op);
    Serial.print(F("\",\"ok\":"));   Serial.print(ok ? F("true") : F("false"));
    Serial.print(F(",\"ms\":"));     Serial.print(millis());
    Serial.print(F(",\"error\":\"")); Serial.print(err); Serial.print(F("\""));
    if (vkey) {
        Serial.print(F(",\""));      Serial.print(vkey); Serial.print(F("\":"));
        if (vstr) {
            Serial.print(F("\""));   Serial.print(vstr); Serial.print(F("\""));
        } else {
            Serial.print(vnum, 6);
        }
    }
    Serial.println(F("}"));
    Serial.flush();
}

static void op_float(const char* name, float v, int8_t err) {
    if (isnan(v)) {
        emit(name, false, RbAmp::errorString(err));
    } else {
        emit(name, true, "", "value", v);
    }
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) {}
    Wire.begin();
    Wire.setClock(100000);  /* Use 100 kHz for the conformance run */
    delay(100);

    /* 1. begin() — probe + variant detect + primer LATCH */
    if (!dev.begin()) {
        emit("begin", false, RbAmp::errorString(dev.lastError()));
        return;
    }
    emit("begin", true);
    emit("topology", true, "", "value", static_cast<double>(dev.channels()));
    emit("has_voltage_hw", true, "", "value",
         dev.hasVoltageHw() ? 1.0 : 0.0);
    emit("fw_version", true, "", "value",
         static_cast<double>(dev.firmwareVersion()));

    /* 2. Real-time reads */
    op_float("readVoltage",        dev.readVoltage(),        dev.lastError());
    op_float("readVoltagePeak",    dev.readVoltagePeak(),    dev.lastError());
    for (uint8_t ch = 0; ch < dev.channels(); ++ch) {
        char buf[20];
        snprintf(buf, sizeof(buf), "readCurrent_%u", ch);
        op_float(buf, dev.readCurrent(ch), dev.lastError());
        snprintf(buf, sizeof(buf), "readPower_%u", ch);
        op_float(buf, dev.readPower(ch), dev.lastError());
        snprintf(buf, sizeof(buf), "readPowerFactor_%u", ch);
        op_float(buf, dev.readPowerFactor(ch), dev.lastError());
    }
    op_float("readFrequency", dev.readFrequency(), dev.lastError());

    /* 3. readAll — single-call snapshot */
    {
        RbAmpSnapshot s;
        const bool ok = dev.readAll(s);
        emit("readAll", ok, ok ? "" : RbAmp::errorString(dev.lastError()),
             "voltage", s.voltage);
    }

    /* 4. Period snapshot — primer was done in begin(); wait for next window */
    delay(1000);
    {
        RbAmpPeriodSnapshot snap;
        const bool ok = dev.readPeriodSnapshot(snap);
        emit("readPeriodSnapshot", ok,
             ok ? "" : RbAmp::errorString(dev.lastError()),
             "avg_p0", snap.avg_p[0]);
    }

    /* 5. Broadcast LATCH */
    {
        const bool ok = RbAmp::broadcastLatch(Wire);
        emit("broadcastLatch", ok, ok ? "" : "general-call failed");
    }
    delay(50);

    /* 6. CT model write (idempotent at value 3 = SCT_013_030, the default) */
    {
        const bool ok = dev.setCTModel(3);
        emit("setCTModel(3)", ok, ok ? "" : RbAmp::errorString(dev.lastError()));
    }

    /* 7. Address-change attempt — should fail in production-mode firmware */
    {
        const bool ok = dev.prepareAddressChange(0x51);
        emit("prepareAddressChange", ok, ok ? "" : RbAmp::errorString(dev.lastError()));
    }

    emit("done", true);
}

void loop() {
    /* one-shot — wait for harness to disconnect */
    delay(5000);
}
