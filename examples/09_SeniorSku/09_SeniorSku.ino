/*
 * 09_SeniorSku — UI5 / UI7 senior SKUs: reading channels 3..6 (v1.3)
 *
 * Senior SKUs expose 5 (UI5) or 7 (UI7) current channels. Channels 0-2 + the
 * voltage/power flat block are read exactly as on junior SKUs; channels 3+ come
 * through the "channel access window" — but the library hides that: readCurrent
 * (ch) / readPower(ch) / readPowerFactor(ch) simply accept ch = 0..channels()-1
 * and route ch>=3 through the window under the hood (re-selecting per read).
 *
 * Shows:
 *   1. Detection — readVariant() (UI5/UI7) + channels() (5/7).
 *   2. Per-channel live I / P / PF across ALL channels (transparent window).
 *   3. Aligned multichannel energy — CMD_LATCH_PERIOD via readPeriodSnapshot()
 *      fills avg_p[] for every channel atomically; energy().wh(ch) accumulates.
 *   4. readCommitSeq() — detect that the device committed a new RT sample
 *      between two reads (change flag only; there is no CRC).
 *
 * Coherence note: only the PERIOD averages (avg_p[], via one latch) are aligned
 * across channels. Instantaneous RMS (readCurrent etc.) for ch3+ is a window
 * sweep = adjacent ~20 ms samples and cannot be made simultaneous — use the
 * latched period snapshot when you need a coherent multichannel figure.
 *
 * Wiring: one senior rbAmp module on the bus.
 *   ESP32 DevKitC          rbAmp UI5/UI7
 *   GPIO21 (SDA) ───────── SDA   (4.7k pull-ups to 3V3 host logic)
 *   GPIO22 (SCL) ───────── SCL
 *   VCC 5V / GND ───────── VCC / GND
 */
#include <Wire.h>
#include <RbAmp.h>

static RbAmp dev(Wire, 0x50);

static const char* variantName(RbAmpVariant v) {
  switch (v) {
    case RbAmpVariant::UI1: return "UI1";
    case RbAmpVariant::UI2: return "UI2";
    case RbAmpVariant::UI3: return "UI3";
    case RbAmpVariant::I1:  return "I1";
    case RbAmpVariant::I2:  return "I2";
    case RbAmpVariant::I3:  return "I3";
    case RbAmpVariant::UI5: return "UI5";
    case RbAmpVariant::UI7: return "UI7";
    default:                return "Unknown";
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

  Serial.print(F("variant="));
  Serial.print(variantName(dev.readVariant()));
  Serial.print(F("  channels="));
  Serial.print(dev.channels());
  Serial.print(F("  voltage_hw="));
  Serial.println(dev.hasVoltageHw() ? F("yes") : F("no"));
}

void loop() {
  const uint8_t n = dev.channels();

  // 1. Live per-channel I / P / PF — same call for ch0-2 (flat) and ch3+ (window).
  Serial.println(F("-- live --"));
  float p_total = 0.0f;
  for (uint8_t ch = 0; ch < n; ++ch) {
    float i  = dev.readCurrent(ch);
    float p  = dev.readPower(ch);
    float pf = dev.readPowerFactor(ch);
    if (!isnan(p)) p_total += p;

    Serial.print(F("  ch")); Serial.print(ch);
    Serial.print(F("  I=")); Serial.print(isnan(i)  ? 0.0f : i, 3);
    Serial.print(F(" A  P=")); Serial.print(isnan(p)  ? 0.0f : p, 1);
    Serial.print(F(" W  PF=")); Serial.println(isnan(pf) ? 0.0f : pf, 2);
  }
  Serial.print(F("  TOTAL P=")); Serial.print(p_total, 1); Serial.println(F(" W"));

  // 2. Aligned multichannel energy — one latch fills avg_p[] for all channels.
  uint8_t seq_before = 0, seq_after = 0;
  dev.readCommitSeq(seq_before);

  RbAmpPeriodSnapshot snap;
  if (dev.readPeriodSnapshot(snap)) {
    Serial.print(F("period dt=")); Serial.print(snap.master_dt_ms); Serial.println(F("ms"));
    for (uint8_t ch = 0; ch < n; ++ch) {
      Serial.print(F("  ch")); Serial.print(ch);
      Serial.print(F("  avgP=")); Serial.print(snap.avg_p[ch], 1);
      Serial.print(F(" W  Wh=")); Serial.println(dev.energy().wh(ch), 3);
    }
  } else {
    Serial.print(F("period skipped: "));
    Serial.println(RbAmp::errorString(dev.lastError()));
  }

  // 3. Commit-change detection (integrity): did a new RT commit land mid-cycle?
  dev.readCommitSeq(seq_after);
  if (seq_before != seq_after) {
    Serial.print(F("commit advanced ")); Serial.print(seq_before);
    Serial.print(F("->")); Serial.println(seq_after);
  }

  delay(5000);
}
