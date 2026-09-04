/*
 * 10_PerChannelModels — per-channel CT model assignment (v1.5.0 / fw v1.4.18+)
 *
 * A module's channels need NOT share one CT model. This sketch assigns a
 * DIFFERENT model per channel and reads each one back. Channels 0-2 bind
 * through the flat command path; channels 3+ (senior SKUs UI5/UI7) bind through
 * the channel window (field 15) — the library hides that: setCTModel(ch, code)
 * accepts ch = 0..channels()-1 and routes automatically.
 *
 * The model codes come from the generated registry (RbAmpSensorModels.h,
 * generated from libs/spec/sensor_models.yaml) — use the RBAMP_CT_* defines,
 * never a hand-typed number. A code the module does not accept (reserved or
 * uncharacterised) is surfaced as RB_ERR_PARAM and the channel keeps its
 * previous model — the write is always confirmed by a read-back.
 *
 * Shows:
 *   1. Naming models by generated define (RBAMP_CT_SCT013_030, ...).
 *   2. Human descriptors via rbamp_sensor_model_lookup() for init-time logging.
 *   3. Per-channel assignment across all channels + read-back verification.
 *
 * Wiring: one rbAmp module on the bus.
 *   ESP32 DevKitC          rbAmp
 *   GPIO21 (SDA) ───────── SDA   (4.7k pull-ups to 3V3 host logic)
 *   GPIO22 (SCL) ───────── SCL
 *   VCC 5V / GND ───────── VCC / GND
 */
#include <Wire.h>
#include <RbAmp.h>

static RbAmp dev(Wire, 0x50);

/* Desired model per channel (index = channel). 0 = leave channel unchanged.
 * Fill only as many entries as the module has channels — extra entries are
 * ignored. Every value is a generated RBAMP_CT_* code, so this table cannot
 * drift out of sync with the firmware's preset table. */
static const uint8_t kWantModel[RBAMP_MAX_CHANNELS] = {
  RBAMP_CT_SCT013_005,   // ch0 — 5 A clamp
  RBAMP_CT_SCT013_030,   // ch1 — 30 A clamp
  RBAMP_CT_SCT013_020,   // ch2 — 20 A clamp
  RBAMP_CT_SCT013_030,   // ch3 — senior channel (window path)
  0, 0, 0                // ch4-6 — leave as-is
};

static void printModel(uint8_t sensor_class, uint8_t code) {
  const rbamp_sensor_model_info_t* m = rbamp_sensor_model_lookup(sensor_class, code);
  if (m) {
    Serial.print(m->descriptor);
    if (m->status == RBAMP_MODEL_STATUS_RESERVED) Serial.print(F(" [reserved]"));
    else if (m->status == RBAMP_MODEL_STATUS_LEGACY) Serial.print(F(" [legacy]"));
  } else {
    Serial.print(F("code "));
    Serial.print(code);
    Serial.print(F(" (not in registry)"));
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}
  Wire.begin();
  Wire.setClock(50000);   // 50 kHz ESP32 / 100 kHz non-ESP32

  dev.setLogStream(&Serial);
  while (!dev.begin()) {
    Serial.print(F("begin failed: "));
    Serial.println(RbAmp::errorString(dev.lastError()));
    delay(1000);
  }

  const uint8_t n = dev.channels();
  Serial.print(F("Module has "));
  Serial.print(n);
  Serial.println(F(" current channel(s)."));

  /* Sensor class first — CT models are per-class. */
  if (!dev.setSensorClass(RbAmpSensorClass::Sct013)) {
    Serial.print(F("setSensorClass failed: "));
    Serial.println(RbAmp::errorString(dev.lastError()));
    return;
  }

  /* Assign highest channel first so the ch0-2 clobber side-effect (see
   * setCTModel docs) resolves to the intended ch0 model last. Channels 3+ have
   * no clobber, so their order is irrelevant. */
  for (int ch = n - 1; ch >= 0; --ch) {
    const uint8_t code = kWantModel[ch];
    if (code == 0) continue;

    Serial.print(F("ch"));
    Serial.print(ch);
    Serial.print(F(" <- "));
    printModel(RBAMP_SENSOR_CLASS_SCT_013, code);

    if (dev.setCTModel(static_cast<uint8_t>(ch), code)) {
      Serial.println(F("  ... OK"));
    } else {
      Serial.print(F("  ... FAILED: "));
      Serial.println(RbAmp::errorString(dev.lastError()));
    }
  }

  /* Read every channel's applied model back and print its descriptor. */
  Serial.println(F("\nApplied models:"));
  for (uint8_t ch = 0; ch < n; ++ch) {
    uint8_t applied = 0;
    if (dev.readCTModelCh(ch, applied)) {
      Serial.print(F("  ch"));
      Serial.print(ch);
      Serial.print(F(": "));
      if (applied == 0) {
        Serial.println(F("unset"));
      } else {
        printModel(RBAMP_SENSOR_CLASS_SCT_013, applied);
        Serial.println();
      }
    } else {
      Serial.print(F("  ch"));
      Serial.print(ch);
      Serial.print(F(": read failed: "));
      Serial.println(RbAmp::errorString(dev.lastError()));
    }
  }
}

void loop() {
  /* Configuration-only sketch — nothing to poll. */
  delay(1000);
}
