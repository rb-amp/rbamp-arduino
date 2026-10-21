/**
 * @file    11_StdLine.ino
 * @brief   Example 11 — one sketch, both product lines. begin() detects
 *          whether the module speaks the BASIC or the STD register model and
 *          the same read calls work on either; the STD-only extras (reactive /
 *          apparent power, per-channel block reads, health counters, latch
 *          banks) are shown behind a line() check.
 *
 * Wiring (ESP32 example):
 *   rbAmp SDA -> GPIO21   (override: -DRBAMP_SDA=25)
 *   rbAmp SCL -> GPIO22   (override: -DRBAMP_SCL=26)
 *   rbAmp GND -> GND
 *   rbAmp VCC -> per the module's label
 *
 * Expected output (STD line, 1 voltage + 1 current channel):
 *   line=STD map=0x34 channels=1 voltage=yes
 *   U=230.4V f=50.00Hz  I0=2.13A P0=489.0W Q0=-31.2var S0=490.0VA PF0=+1.00
 *   health: status=0x07 frames=+7012 crc_err=0
 *   latch[1]: dt=10.0s P_avg=488.7W import=1.3575Wh export=0.0000Wh
 *
 * Expected output (BASIC line, e.g. a 3-channel module):
 *   line=BASIC fw=0x0B channels=3 voltage=yes
 *   U=230.4V f=50.0Hz  I0=... P0=... PF0=...  (no Q / S — not measured on BASIC)
 */
#include <Wire.h>
#include <RbAmp.h>

#ifndef RBAMP_SDA
#define RBAMP_SDA 21
#endif
#ifndef RBAMP_SCL
#define RBAMP_SCL 22
#endif

static RbAmp dev(Wire, 0x50);

/* Demo latch bank 1 (bank 0 is used internally by readPeriodSnapshot()). */
static const uint8_t  kBank = 1;
static const uint32_t kLatchPeriodMs = 10000;
static uint32_t last_latch_ms = 0;
static uint32_t last_frames = 0;

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { }
    Serial.println();
    Serial.println(F("rbAmp — Example 11: StdLine (BASIC + STD, one sketch)"));

#if defined(ARDUINO_ARCH_ESP32)
    Wire.begin(RBAMP_SDA, RBAMP_SCL);
#else
    Wire.begin();
#endif
    dev.setLogStream(&Serial);

    while (!dev.begin()) {
        Serial.print(F("begin() failed: "));
        Serial.println(dev.errorString());
        delay(1000);
    }

    if (dev.line() == RbAmpLine::Std) {
        RbAmpStdInfo info;
        dev.stdInfo(info);
        Serial.print(F("line=STD map=0x")); Serial.print(info.map_version, HEX);
        Serial.print(F(" channels=")); Serial.print(dev.channels());
        Serial.print(F(" voltage=")); Serial.println(dev.hasVoltageHw() ? F("yes") : F("no"));

        /* Arm bank 1 for the demo period alongside bank 0 (keep bit 0 set —
         * readPeriodSnapshot() / energy() depend on it). */
        dev.stdArmBanks((1u << 0) | (1u << kBank));
        dev.stdLatch(kBank);            /* starts the first period of bank 1 */
        last_latch_ms = millis();
    } else {
        Serial.print(F("line=BASIC fw=0x")); Serial.print(dev.firmwareVersion(), HEX);
        Serial.print(F(" channels=")); Serial.print(dev.channels());
        Serial.print(F(" voltage=")); Serial.println(dev.hasVoltageHw() ? F("yes") : F("no"));
    }
}

void loop() {
    /* Same call, either line. */
    RbAmpSnapshot s;
    if (!dev.readAll(s)) {
        Serial.print(F("read failed: ")); Serial.println(dev.errorString());
        delay(1000);
        return;
    }
    Serial.print(F("U=")); Serial.print(s.voltage, 1); Serial.print(F("V "));
    Serial.print(F("f=")); Serial.print(s.frequency, 2); Serial.print(F("Hz  "));
    for (uint8_t ch = 0; ch < s.channels; ++ch) {
        Serial.print(F("I")); Serial.print(ch); Serial.print(F("=")); Serial.print(s.current[ch], 2); Serial.print(F("A "));
        Serial.print(F("P")); Serial.print(ch); Serial.print(F("=")); Serial.print(s.power[ch], 1); Serial.print(F("W "));
        if (dev.line() == RbAmpLine::Std) {
            /* Reactive / apparent power exist only on the STD line. */
            Serial.print(F("Q")); Serial.print(ch); Serial.print(F("=")); Serial.print(dev.readReactivePower(ch), 1); Serial.print(F("var "));
            Serial.print(F("S")); Serial.print(ch); Serial.print(F("=")); Serial.print(dev.readApparentPower(ch), 1); Serial.print(F("VA "));
        }
        Serial.print(F("PF")); Serial.print(ch); Serial.print(F("=")); Serial.print(s.power_factor[ch], 2); Serial.print(F("  "));
    }
    Serial.println();

    if (dev.line() != RbAmpLine::Std) {
        delay(1000);
        return;
    }

    /* STD extras: liveness (frame counter must grow between polls). */
    RbAmpStdHealth h;
    if (dev.stdReadHealth(h)) {
        Serial.print(F("health: status=0x")); Serial.print(h.status, HEX);
        Serial.print(F(" frames=+")); Serial.print(h.frame_count - last_frames);
        Serial.print(F(" crc_err=")); Serial.println(h.crc_errors);
        last_frames = h.frame_count;
    }

    /* Master-timed period on bank 1: latch, wait one measurement window, read,
     * convert the signed energy buckets with the master's own dt. */
    if (millis() - last_latch_ms >= kLatchPeriodMs) {
        const uint32_t now = millis();
        const uint32_t dt  = now - last_latch_ms;
        if (dev.stdLatch(kBank)) {
            delay(300);   /* latch lands at the next window boundary: safe upper bound (window 100 ms from image 0x7C, up to ~260 ms before) */
            RbAmpStdLatch L;
            if (dev.stdReadLatch(kBank, L) && L.win_count > 0) {
                double wh_in = 0, wh_out = 0;
                RbAmp::stdLatchWh(L, dt, RBAMP_STD_TOTAL, wh_in, wh_out);
                Serial.print(F("latch[")); Serial.print(kBank); Serial.print(F("]: dt="));
                Serial.print(dt / 1000.0, 1); Serial.print(F("s P_avg=")); Serial.print(L.p_avg, 1);
                Serial.print(F("W import=")); Serial.print(wh_in, 4);
                Serial.print(F("Wh export=")); Serial.print(wh_out, 4); Serial.println(F("Wh"));
            } else {
                Serial.print(F("latch read: ")); Serial.println(dev.errorString());
            }
        }
        last_latch_ms = now;
    }
    delay(1000);
}
