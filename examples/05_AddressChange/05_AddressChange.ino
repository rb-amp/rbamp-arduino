/**
 * @file    05_AddressChange.ino
 * @brief   Example 5 — reassign the I2C slave address (develop-mode only).
 *
 * Demonstrates:
 *  - REG_MODE gate: device must be in develop mode (PB5 HIGH on PY32F030).
 *  - Two-step API: prepareAddressChange() arms the change; commitAddressChange()
 *    must follow within 5 seconds.
 *  - Automatic update of the internal address field — subsequent reads
 *    target the new address with no manual change.
 *
 * Use case: build a multi-module installation by flashing each rbAmp with
 * the default 0x50, then running this sketch once per module while only
 * that module is on the bus.
 *
 * IMPORTANT: changing addresses on a populated bus risks collision. Disconnect
 * other modules or run this on a bench setup with one module at a time.
 */
#include <Wire.h>
#include <RbAmp.h>

static const uint8_t CURRENT_ADDR = 0x50;
static const uint8_t NEW_ADDR     = 0x51;

static RbAmp dev(Wire, CURRENT_ADDR);

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000) {}
    Wire.begin();
    dev.setLogStream(&Serial);

    Serial.print(F("Probing at 0x")); Serial.println(CURRENT_ADDR, HEX);
    if (!dev.begin()) {
        Serial.println(F("device not found — aborting"));
        return;
    }
    Serial.print(F("firmware v0x")); Serial.println(dev.firmwareVersion(), HEX);

    /* Step 1: arm the change. Library checks REG_MODE == develop. */
    if (!dev.prepareAddressChange(NEW_ADDR)) {
        Serial.print(F("prepare failed: "));
        Serial.println(RbAmp::errorString(dev.lastError()));
        if (dev.lastError() == rbamp::RB_ERR_MODE) {
            Serial.println(F("Set device to develop mode (PB5 HIGH) and retry."));
        }
        return;
    }
    Serial.println(F("change armed; committing in 1 second..."));
    delay(1000);

    /* Step 2: commit. Internally writes 0x30, CMD_SAVE_GAINS (700 ms), CMD_RESET (100 ms). */
    if (!dev.commitAddressChange()) {
        Serial.print(F("commit failed: "));
        Serial.println(RbAmp::errorString(dev.lastError()));
        return;
    }
    Serial.print(F("device now at 0x")); Serial.println(dev.address(), HEX);

    /* Verify by re-reading at the new address. */
    if (!dev.probe()) {
        Serial.println(F("WARN: probe at new address failed"));
        return;
    }
    Serial.println(F("verified: device responds at new address"));
}

void loop() {
    /* one-shot — nothing else to do */
    delay(60000);
}
