/**
 * @file    05_AddressChange.ino
 * @brief   Example 5 — reassign the I2C slave address (two-phase commit, v1.3).
 *
 * Demonstrates:
 *  - Two-step API: prepareAddressChange() arms the change; commitAddressChange()
 *    must follow within 5 seconds. The commit is a production-OK two-phase
 *    magic sequence (no special/factory mode required on v1.3 firmware).
 *  - Automatic update of the internal address field — subsequent reads
 *    target the new address with no manual change.
 *
 * Use case: build a multi-module installation by configuring each module with
 * the factory default 0x50, then running this sketch once per module while
 * only that module is on the bus. (For an automated flow over a populated bus,
 * see RbAmpFleet::provision() / assignAddress().)
 *
 * IMPORTANT: changing addresses on a populated bus risks collision. Connect one
 * module at a time, or use the fleet provisioning helpers which collision-check
 * first.
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

    /* Step 1: arm the change. No wire I/O; just validates range + arms a 5 s window. */
    if (!dev.prepareAddressChange(NEW_ADDR)) {
        Serial.print(F("prepare failed: "));
        Serial.println(RbAmp::errorString(dev.lastError()));
        return;
    }
    Serial.println(F("change armed; committing in 1 second..."));
    delay(1000);

    /* Step 2: commit — write candidate to 0x30, arm 0xA5 magic, CMD_COMMIT_ADDR,
     * then CMD_RESET. The internal address field updates on success. */
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
