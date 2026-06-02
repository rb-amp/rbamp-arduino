/**
 * @file    02_PeriodEnergyOLED.ino
 * @brief   Example 2 — 60-second period energy meter on a 128x64 SSD1306 OLED.
 *
 * Demonstrates:
 *  - readPeriodSnapshot() — the recommended one-shot period flow.
 *  - Library-owned energy accumulator (dev.energy().wh(0)).
 *  - REG_V03_PERIOD_VALID handling — stale snapshots are reported, not silently dropped.
 *
 * Dependencies:
 *   Adafruit_SSD1306 (https://github.com/adafruit/Adafruit_SSD1306)
 *
 * Wiring (ESP32 + SSD1306 sharing the same I2C bus as rbAmp):
 *   SDA -> GPIO21    (both devices)
 *   SCL -> GPIO22    (both devices)
 *   OLED addr default 0x3C
 *   rbAmp addr 0x50
 */
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RbAmp.h>

#define OLED_WIDTH   128
#define OLED_HEIGHT   64
static Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
static RbAmp dev(Wire, 0x50);

static uint32_t next_latch_ms = 0;
static uint32_t snapshots_ok  = 0;
static uint32_t snapshots_bad = 0;

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000) {}
    Wire.begin();

    if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println(F("OLED init failed — running headless"));
    } else {
        oled.clearDisplay();
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(0, 0);
        oled.println(F("rbAmp Energy Meter"));
        oled.println(F("booting..."));
        oled.display();
    }

    dev.setLogStream(&Serial);
    while (!dev.begin()) {
        Serial.println(F("rbAmp begin failed; retrying"));
        delay(1000);
    }
    next_latch_ms = millis() + 60000;
}

void loop() {
    if (millis() < next_latch_ms) {
        delay(50);
        return;
    }
    next_latch_ms = millis() + 60000;

    RbAmpPeriodSnapshot snap;
    if (!dev.readPeriodSnapshot(snap)) {
        snapshots_bad++;
        Serial.print(F("snapshot failed: "));
        Serial.println(RbAmp::errorString(dev.lastError()));
        return;
    }
    snapshots_ok++;

    Serial.print(F("avg_P0=")); Serial.print(snap.avg_p[0], 2);
    Serial.print(F("W  Wh0=")); Serial.print(dev.energy().wh(0), 4);
    Serial.print(F("  dt=")); Serial.print(snap.master_dt_ms); Serial.println(F("ms"));

    /* Update OLED */
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.setTextSize(1);
    oled.println(F("rbAmp Energy Meter"));
    oled.setCursor(0, 16);
    oled.setTextSize(2);
    oled.print(snap.avg_p[0], 1);
    oled.println(F(" W"));
    oled.setTextSize(1);
    oled.print(F("Wh:"));
    oled.println(dev.energy().wh(0), 4);
    oled.print(F("ok="));
    oled.print(snapshots_ok);
    oled.print(F(" bad="));
    oled.println(snapshots_bad);
    oled.display();
}
