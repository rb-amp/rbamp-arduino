/**
 * @file    07_DeepSleepLogger.ino
 * @brief   Example 7 — battery-friendly periodic logger using ESP32 deep sleep.
 *
 * Demonstrates:
 *  - Persisting per-channel Wh across deep-sleep cycles via RTC slow-RAM
 *    (RTC_DATA_ATTR — survives deep sleep, lost on power-cycle).
 *  - Magic-marker guard for first-boot vs warm-wake disambiguation.
 *  - Single-shot pattern: boot, latch (or read skip_latch), log, sleep N minutes.
 *  - Library accumulator disabled — master fully owns Wh persistence.
 *
 * Target: ESP32 only. Power-down current during deep sleep is ~10 µA.
 *
 * @warning Deep-sleep on v1.0/v1.1 firmware — primer-reset behaviour.
 *          On every warm wake `dev.begin()` issues CMD_LATCH_PERIOD primer
 *          that resets the firmware-side period accumulator. The data
 *          accumulated DURING sleep is captured in the latched register by
 *          this primer — so we must use @c readPeriodSnapshot(snap, 0, true)
 *          (skip_latch=true) to read it BEFORE a subsequent default
 *          @c readPeriodSnapshot() would re-latch and discard it.
 *          Additionally, @c snap.master_dt_ms reflects only the time
 *          WITHIN the current wake (begin() resets it), NOT the wake-to-wake
 *          interval. For energy integration we use a master-side timestamp
 *          stored in RTC slow-RAM (@c rtc_last_wake_us) which survives deep
 *          sleep. A future v1.3+ @c warmBegin() that skips the primer will
 *          let this pattern use default @c readPeriodSnapshot() instead —
 *          see 06_examples.md Sc9 roadmap callout.
 *
 * Note: deep sleep resets WiFi state; for MQTT publishing combine with
 *       example 4 (replace the publish_serial() stub).
 */
#if !defined(ESP32)
#error "This example requires an ESP32 board."
#endif

#include <Wire.h>
#include <RbAmp.h>
#include <esp_sleep.h>
#include <esp_timer.h>

static const uint64_t SLEEP_INTERVAL_US = 10ULL * 60ULL * 1000000ULL;  /* 10 minutes */
static const uint32_t RTC_MAGIC         = 0xCAFEFEEDUL;

/* RTC slow-RAM — survives deep sleep, lost on power-cycle (or magic mismatch). */
RTC_DATA_ATTR static uint32_t rtc_magic        = 0;
RTC_DATA_ATTR static double   rtc_total_wh[3]  = {0.0, 0.0, 0.0};
RTC_DATA_ATTR static uint64_t rtc_last_wake_us = 0;
RTC_DATA_ATTR static bool     rtc_primer_done  = false;
RTC_DATA_ATTR static uint32_t boot_count       = 0;

static RbAmp dev(Wire, 0x50);

/* Replace with WiFi+MQTT publish for a production logger. Stub here to keep
 * the example focused on the deep-sleep correctness pattern. */
static void publish_serial(uint8_t ch, float avg_p_w, double total_wh, double dt_s) {
    Serial.print(F("ch")); Serial.print(ch); Serial.print(F(": "));
    Serial.print(avg_p_w, 1); Serial.print(F("W "));
    Serial.print(total_wh, 4); Serial.print(F("Wh "));
    Serial.print(F("dt=")); Serial.print(dt_s, 1); Serial.println(F("s"));
}

void setup() {
    Serial.begin(115200);
    delay(200);
    boot_count++;
    Serial.print(F("Boot #")); Serial.println(boot_count);

    /* Cold-boot detection: RTC magic mismatch -> power-cycle or first ever boot.
     * Zero all RTC state so we start from a clean energy total. */
    if (rtc_magic != RTC_MAGIC) {
        rtc_magic        = RTC_MAGIC;
        rtc_total_wh[0]  = rtc_total_wh[1] = rtc_total_wh[2] = 0.0;
        rtc_last_wake_us = 0;
        rtc_primer_done  = false;
        Serial.println(F("RTC magic mismatch — cold boot, state reset."));
    }

    Wire.begin();
    Wire.setClock(50000);   /* SPEC §B.5 dense-workload baseline */

    if (!dev.begin()) {
        Serial.print(F("rbAmp begin failed: "));
        Serial.println(RbAmp::errorString(dev.lastError()));
        Serial.println(F("Sleeping anyway."));
        goto sleep;
    }

    /* Library accumulator OFF — master owns Wh persistence via rtc_total_wh[]. */
    dev.energy().disable();

    if (!rtc_primer_done) {
        /* First wake after cold boot: begin()'s primer just latched whatever was
         * accumulated since power-on (a few ms of boot junk). Capture the wake
         * timestamp and sleep — the NEXT wake will see a clean 10-min sleep
         * interval's worth of data. */
        rtc_last_wake_us = esp_timer_get_time();
        rtc_primer_done  = true;
        Serial.println(F("Primer wake — seeding rtc_last_wake_us and sleeping."));
        goto sleep;
    }

    {
        /* Warm wake: begin()'s primer just latched the firmware accumulator
         * which covers the previous sleep interval. Read it with skip_latch=true
         * so we don't issue ANOTHER latch (which would overwrite this data
         * with the ~50 ms post-primer accumulator). */
        RbAmpPeriodSnapshot snap;
        if (!dev.readPeriodSnapshot(snap, /*settle_ms=*/0, /*skip_latch=*/true)) {
            Serial.print(F("snapshot fail: "));
            Serial.println(RbAmp::errorString(dev.lastError()));
            goto sleep;
        }

        /* snap.master_dt_ms is wall-clock time WITHIN the current wake (begin()
         * just reset it). For energy integration we need the actual wake-to-wake
         * interval — use the RTC-RAM timestamp that survived deep sleep. */
        const uint64_t now_us = esp_timer_get_time();
        const double   dt_s   = (double)(now_us - rtc_last_wake_us) / 1000000.0;
        rtc_last_wake_us = now_us;

        Serial.print(F("dt=")); Serial.print(dt_s, 1); Serial.println(F("s"));
        for (uint8_t ch = 0; ch < dev.channels(); ++ch) {
            const double dwh = (double)snap.avg_p[ch] * dt_s / 3600.0;
            rtc_total_wh[ch] += dwh;
            publish_serial(ch, snap.avg_p[ch], rtc_total_wh[ch], dt_s);
        }
    }

sleep:
    Serial.print(F("Sleeping "));
    Serial.print((uint32_t)(SLEEP_INTERVAL_US / 1000000ULL));
    Serial.println(F("s..."));
    Serial.flush();
    esp_sleep_enable_timer_wakeup(SLEEP_INTERVAL_US);
    esp_deep_sleep_start();
    /* never returns */
}

void loop() {
    /* unreachable — setup() ends in esp_deep_sleep_start() */
}
