# Examples

Walkthrough of the 7 bundled `examples/` sketches plus 3 composed scenarios.
Every numbered example below has matching code in [`examples/`](../examples/)
or [`../examples/` at the rbAmp mono-repo level](../../examples/) — open the
real sketches in the Arduino IDE for the full source.

| # | Title | Difficulty | Source file |
|:---:|---|:---:|---|
| 1 | [Quick read](#scenario-1--quick-read) | minimal | [`01_QuickRead.ino`](../examples/01_QuickRead/01_QuickRead.ino) |
| 2 | [60-second meter on OLED](#scenario-2--60-second-meter-on-oled) | low | [`02_PeriodEnergyOLED.ino`](../examples/02_PeriodEnergyOLED/02_PeriodEnergyOLED.ino) |
| 3 | [Multi-module monitor (3 devices)](#scenario-3--multi-module-monitor) | low | [`03_MultiModuleBroadcast.ino`](../examples/03_MultiModuleBroadcast/03_MultiModuleBroadcast.ino) |
| 4 | [Per-appliance UI3 + MQTT](#scenario-4--per-appliance-ui3--mqtt) | medium | [`04_UI3PerChannelMQTT.ino`](../examples/04_UI3PerChannelMQTT/04_UI3PerChannelMQTT.ino) |
| 5 | [Master-side bidirectional accounting](#scenario-5--master-side-bidirectional-accounting) | medium | [`06_BidirectionalEnergy.ino`](../examples/06_BidirectionalEnergy/06_BidirectionalEnergy.ino) |
| 6 | [Home energy balance](#scenario-6--home-energy-balance) | high | (composed) |
| 7 | [Event detection (EMA)](#scenario-7--event-detection-ema) | medium | (composed) |
| 8 | [12-hour autonomous bench monitor](#scenario-8--12-hour-autonomous-bench-monitor) | medium | [`SoakMonitor.ino`](../../examples/SoakMonitor/SoakMonitor.ino) |
| 9 | [Battery deep-sleep logger](#scenario-9--battery-deep-sleep-logger) | medium | [`07_DeepSleepLogger.ino`](../examples/07_DeepSleepLogger/07_DeepSleepLogger.ino) |
| 10 | [I2C address change](#scenario-10--i2c-address-change) | low | [`05_AddressChange.ino`](../examples/05_AddressChange/05_AddressChange.ino) |

---

## Scenario 1 — Quick read

Goal: print U / I / P / PF / freq once per second. The "hello world" you
just wrote in [Quickstart](05_quickstart.md), but using
`dev.readAll(s)` for a one-shot full RT block read instead of individual
property calls.

```cpp
#include <Wire.h>
#include <RbAmp.h>

static RbAmp dev(Wire, 0x50);

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) {}
    Wire.begin();
    dev.setLogStream(&Serial);
    while (!dev.begin()) {
        Serial.print(F("begin: ")); Serial.println(RbAmp::errorString(dev.lastError()));
        delay(1000);
    }
}

void loop() {
    RbAmpSnapshot s;
    if (!dev.readAll(s)) {
        Serial.print(F("read fail: ")); Serial.println(RbAmp::errorString(dev.lastError()));
        delay(1000); return;
    }
    Serial.print(F("U=")); Serial.print(s.voltage, 1); Serial.print(F("V "));
    Serial.print(F("f=")); Serial.print(s.frequency, 1); Serial.print(F("Hz   "));
    for (uint8_t ch = 0; ch < s.channels; ++ch) {
        Serial.print(F("I")); Serial.print(ch); Serial.print(F("="));
        Serial.print(s.current[ch], 2); Serial.print(F("A "));
        Serial.print(F("P")); Serial.print(ch); Serial.print(F("="));
        Serial.print(s.power[ch], 1); Serial.print(F("W "));
        Serial.print(F("PF")); Serial.print(ch); Serial.print(F("="));
        Serial.print(s.power_factor[ch], 2); Serial.print(F("  "));
    }
    Serial.println();
    delay(1000);
}
```

**What's on the wire**: `readAll()` on a UI3 issues 53 single-byte
transactions (13 floats × 4 bytes + 1 byte frequency). At 100 kHz with
3-attempt retry headroom, ~12-15 ms total. Drop to per-call reads
(`dev.voltage`, `dev.current[ch]`) if you only need a subset.

---

## Scenario 2 — 60-second meter on OLED

Wh counter refreshed once per minute on a 128×64 SSD1306 OLED sharing the
I2C bus (OLED at 0x3C). Key library mechanic: `readPeriodSnapshot()` does
latch + settle + valid-check + read + Wh tick in one call.

```cpp
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RbAmp.h>

static Adafruit_SSD1306 oled(128, 64, &Wire, -1);
static RbAmp dev(Wire, 0x50);
static uint32_t next_latch_ms = 0;
static uint32_t snapshots_ok = 0, snapshots_bad = 0;

void setup() {
    Serial.begin(115200);
    Wire.begin();
    oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    dev.setLogStream(&Serial);
    while (!dev.begin()) { delay(1000); }
    next_latch_ms = millis() + 60000;
}

void loop() {
    if (millis() < next_latch_ms) { delay(50); return; }
    next_latch_ms = millis() + 60000;

    RbAmpPeriodSnapshot snap;
    if (!dev.readPeriodSnapshot(snap)) { snapshots_bad++; return; }
    snapshots_ok++;

    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);  oled.setTextSize(1); oled.println(F("rbAmp Energy Meter"));
    oled.setCursor(0, 16); oled.setTextSize(2); oled.print(snap.avg_p[0], 1); oled.println(F(" W"));
    oled.setTextSize(1);
    oled.print(F("Wh:")); oled.println(dev.energy().wh(0), 4);
    oled.print(F("ok=")); oled.print(snapshots_ok);
    oled.print(F(" bad=")); oled.println(snapshots_bad);
    oled.display();
}
```

Full sketch in [`02_PeriodEnergyOLED.ino`](../examples/02_PeriodEnergyOLED/02_PeriodEnergyOLED.ino).

**Stale snapshot handling**: `readPeriodSnapshot()` returns `false` with
`lastError() == RB_ERR_STALE` if `REG_V03_PERIOD_VALID == 0`. The library
**still commits** the master wall-clock timestamp internally — this
prevents the next successful snapshot from double-counting Wh over a
2× period interval.

---

## Scenario 3 — Multi-module monitor

Three rbAmp modules on one bus at 0x50 / 0x51 / 0x52. Each must have a
unique I2C address — use [scenario 10](#scenario-10--i2c-address-change)
to assign addresses during installation.

> **v1.3 coherent latch (`CAP_GC_LATCH`):** every module that has opted into
> the General-Call latch (`enableGc(true)`, persisted) captures the *same*
> mains period on a single all-call frame —
> `RbAmp::broadcastLatchGroup(Wire, 0x00, tick)`. Fleet skew is < 10 µs, so
> per-module powers sum coherently. Each module tags its snapshot with the
> broadcast `tick`; read it back with `readGcTick()` to catch a module that
> missed the frame. (On v1.0/v1.1 firmware, General-Call latch was
> unavailable and this sketch fell back to per-device sequential latching —
> kept in the file's history comment for older modules.)

```cpp
#include <Wire.h>
#include <RbAmp.h>

static RbAmp T0(Wire, 0x50);
static RbAmp T1(Wire, 0x51);
static RbAmp T2(Wire, 0x52);
static RbAmp* modules[] = { &T0, &T1, &T2 };
static const uint8_t N = sizeof(modules) / sizeof(modules[0]);
static uint16_t g_tick = 0;

void setup() {
    Serial.begin(115200);
    Wire.begin();
    Wire.setClock(50000);   // 50 kHz on ESP32 / 100 kHz on non-ESP32 masters
    for (uint8_t i = 0; i < N; ++i) {
        modules[i]->begin();
        modules[i]->enableGc(true);   // opt into General-Call latch (persisted)
    }
}

void loop() {
    g_tick++;
    RbAmp::broadcastLatchGroup(Wire, /*group=*/0x00, g_tick);  // 0x00 = all-call
    delay(50);   // single shared settle for the whole bus

    for (uint8_t i = 0; i < N; ++i) {
        RbAmpPeriodSnapshot snap;
        if (modules[i]->readPeriodSnapshot(snap, /*settle_ms=*/0, /*skip_latch=*/true)) {
            uint16_t tick;
            bool synced = modules[i]->readGcTick(tick) && tick == g_tick;
            Serial.print(F("T")); Serial.print(i);
            Serial.print(F(" P=")); Serial.print(snap.avg_p[0], 0);
            Serial.print(F("W  Wh=")); Serial.print(modules[i]->energy().wh(0), 3);
            Serial.println(synced ? F("  [sync]") : F("  [MISSED FRAME]"));
        }
    }
    delay(60000);
}
```

Full sketch (with bus-scan probe + error reporting) in
[`03_MultiModuleBroadcast.ino`](../examples/03_MultiModuleBroadcast/03_MultiModuleBroadcast.ino).

---

## Scenario 4 — Per-appliance UI3 + MQTT

A UI3 module with three CT clamps on three loads. Independent Wh counters
per channel, published to MQTT once per minute. Demonstrates that
`dev.energy().wh(ch)` works on every channel independently — no manual
`total_wh[3]` array needed.

```cpp
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <RbAmp.h>

static RbAmp dev(Wire, 0x50);
static const char* CH_NAMES[3] = {"main", "heatpump", "lights"};
static WiFiClient espClient;
static PubSubClient mqtt(espClient);

void publish_channel(const char* name, float avg_p, double e_wh) {
    char topic[64], payload[96];
    snprintf(topic, sizeof(topic), "rbamp/%s/state", name);
    snprintf(payload, sizeof(payload), "{\"power\":%.1f,\"energy\":%.4f}", avg_p, e_wh);
    mqtt.publish(topic, payload, true);
}

void setup() {
    Serial.begin(115200);
    WiFi.begin("ssid", "pass");
    while (WiFi.status() != WL_CONNECTED) delay(500);
    mqtt.setServer("192.168.1.10", 1883);
    mqtt.setKeepAlive(60);
    mqtt.connect("rbamp-ui3");

    Wire.begin();
    Wire.setClock(50000);    // SPEC §B.5 on ESP32
    dev.setLogStream(&Serial);
    while (!dev.begin()) { delay(500); }
}

void loop() {
    if (!mqtt.connected()) mqtt.connect("rbamp-ui3");
    mqtt.loop();

    static uint32_t last_period = 0;
    if (last_period == 0) last_period = millis();
    if (millis() - last_period < 60000) { delay(50); return; }
    last_period = millis();

    RbAmpPeriodSnapshot snap;
    if (!dev.readPeriodSnapshot(snap)) return;

    for (uint8_t ch = 0; ch < dev.channels(); ++ch) {
        publish_channel(CH_NAMES[ch], snap.avg_p[ch], dev.energy().wh(ch));
    }
}
```

Full sketch in [`04_UI3PerChannelMQTT.ino`](../examples/04_UI3PerChannelMQTT/04_UI3PerChannelMQTT.ino).
For HA MQTT Auto-discovery on top of this base, see
[DIY Integrations](07_diy_integrations.md#home-assistant-mqtt-auto-discovery).

---

## Scenario 5 — Master-side bidirectional accounting

Split signed RT power into gross consume / gross export buckets. Use on
BASIC tier modules where the period accumulator clamps negative — sample
`dev.readPower(0)` at 5 Hz on the master side.

```cpp
#include <Wire.h>
#include <RbAmp.h>

static RbAmp dev(Wire, 0x50);
static double consume_wh = 0.0;
static double export_wh  = 0.0;
static uint32_t last_sample_ms = 0;

void setup() {
    Serial.begin(115200);
    Wire.begin();
    dev.setLogStream(&Serial);
    while (!dev.begin()) { delay(500); }
    last_sample_ms = millis();
}

void loop() {
    if (millis() - last_sample_ms < 200) { delay(5); return; }   // 5 Hz — matches RT cadence
    const uint32_t now_ms = millis();
    const uint32_t dt_ms  = now_ms - last_sample_ms;
    last_sample_ms = now_ms;

    const float p_w = dev.readPower(0);
    if (isnan(p_w)) return;

    const double dwh = (double)p_w * dt_ms / 3600000.0;
    if (p_w >= 0.0f) consume_wh += dwh;
    else             export_wh  += -dwh;

    static uint32_t next_print = 0;
    if (millis() >= next_print) {
        next_print = millis() + 200;
        Serial.print(F("P=")); Serial.print(p_w, 1);
        Serial.print(F("W   cons=")); Serial.print(consume_wh, 4);
        Serial.print(F("Wh  exp=")); Serial.print(export_wh, 4);
        Serial.print(F("Wh  net=")); Serial.print(consume_wh - export_wh, 4);
        Serial.println(F("Wh"));
    }
}
```

Full sketch in [`06_BidirectionalEnergy.ino`](../examples/06_BidirectionalEnergy/06_BidirectionalEnergy.ino).
On STANDARD / PRO tier, `dev.energy().wh(0)` already gives you the
signed net balance — you only need this master-side split for gross
consume vs gross export reporting.

---

## Scenario 6 — Home energy balance

Three modules: mains (bidir, STANDARD/PRO) + solar (gen-only, BASIC OK)
+ loads (UI3 BASIC for per-appliance). Combined whole-house dashboard
published every 60 s.

```cpp
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <RbAmp.h>

static RbAmp mains(Wire, 0x50);    // bidirectional
static RbAmp solar(Wire, 0x51);    // generation only
static RbAmp loads(Wire, 0x52);    // UI3
static RbAmp* mods[] = { &mains, &solar, &loads };
static WiFiClient espClient;
static PubSubClient mqtt(espClient);

void setup() {
    Serial.begin(115200);
    WiFi.begin("ssid", "pass");
    while (WiFi.status() != WL_CONNECTED) delay(500);
    mqtt.setServer("192.168.1.10", 1883); mqtt.connect("home-balance");

    Wire.begin();
    Wire.setClock(50000);   // 50 kHz on ESP32 (100 kHz on non-ESP32 masters)
    for (auto m : mods) { while (!m->begin()) delay(500); m->enableGc(true); }
}

void loop() {
    delay(60000);

    static uint16_t g_tick = 0;
    RbAmp::broadcastLatchGroup(Wire, /*group=*/0x00, ++g_tick);  // coherent all-call latch
    delay(50);

    RbAmpPeriodSnapshot sm, ss, sl;
    mains.readPeriodSnapshot(sm, 0, true);
    solar.readPeriodSnapshot(ss, 0, true);
    loads.readPeriodSnapshot(sl, 0, true);

    char payload[512];
    snprintf(payload, sizeof(payload),
        "{\"mains\":%.3f,\"solar\":%.3f,\"hp\":%.3f,\"ac\":%.3f,\"ev\":%.3f}",
        mains.energy().wh(0),
        solar.energy().wh(0),
        loads.energy().wh(0),
        loads.energy().wh(1),
        loads.energy().wh(2));
    mqtt.publish("home/energy/balance", payload, true);
    mqtt.loop();
}
```

For the explicit consume/export split on the mains module, combine with
Scenario 5's pattern (run a separate 5 Hz loop on `mains.readPower(0)`).

---

## Scenario 7 — Event detection (EMA)

On every 200 ms RT window, compare P against an exponentially-weighted
moving average. Log significant deviations (microwave / kettle).

```cpp
#include <Wire.h>
#include <RbAmp.h>

static RbAmp dev(Wire, 0x50);
static float p_ema = 0.0f;
static const float EMA_ALPHA = 0.05f;
static const float EVENT_THRESHOLD_W = 200.0f;

void setup() {
    Serial.begin(115200);
    Wire.begin();
    dev.setLogStream(&Serial);
    while (!dev.begin()) { delay(500); }
    p_ema = dev.readPower(0);     // seed EMA so first read isn't a spurious event
    if (isnan(p_ema)) p_ema = 0.0f;
}

void loop() {
    delay(200);
    const float p = dev.readPower(0);
    if (isnan(p)) return;

    const float delta = p - p_ema;
    p_ema = (1.0f - EMA_ALPHA) * p_ema + EMA_ALPHA * p;

    if (fabsf(delta) > EVENT_THRESHOLD_W) {
        Serial.print(millis()); Serial.print(F("  "));
        Serial.print(delta > 0 ? F("TURN_ON ") : F("TURN_OFF "));
        Serial.print(F("delta=")); Serial.print(delta, 0); Serial.print(F("W  "));
        Serial.print(F("P=")); Serial.print(p, 0); Serial.print(F("W  "));
        Serial.print(F("EMA=")); Serial.println(p_ema, 0);
    }
}
```

Composed scenario — no dedicated example file. Combine with Scenario 4's
MQTT publish for HA-side event automation, or log to SD with
`Adafruit_SD` for offline event tracking.

---

## Scenario 8 — 12-hour autonomous bench monitor

Long-soak validation harness — runs the library against a real DUT for
12-15 hours under a dense per-cycle workload, emits one CSV row per
5 minutes with full diagnostic counters. The reference recipe used to
qualify v1.1 firmware on the Arduino library reference master.

Key features:

- **`RBAMP_NACK_RETRY_ATTEMPTS=5` override** — define BEFORE the include
  to tune retry for dense workloads (≥10 single-byte reads per cycle).
  Default 3 suffices for ESPHome 60 s polling; SoakMonitor reads ~45
  bytes per cycle and needs the extra headroom per SPEC §B.5 Phase 2.
- **`dev.rawTopology()`** — direct wire read of REG_TOPOLOGY (0x24) for
  v1.1 detection. Returns 1/2/3 on v1.1, `0x00` on v1.0, `0xFF` on
  I2C failure.
- **`dev.retryExhaustionCount()` + `dev.sanityRejectCount()`** — both must
  stay at **0** for a passing 12 h run.

Per-cycle skeleton:

```cpp
#define RBAMP_NACK_RETRY_ATTEMPTS 5
#include <Wire.h>
#include <RbAmp.h>

RbAmp dev(Wire, 0x50);

void emitCycle() {
    const float u = dev.readVoltage();
    float i[3]={NAN,NAN,NAN}, p[3]={NAN,NAN,NAN}, pf[3]={NAN,NAN,NAN};
    for (uint8_t c = 0; c < dev.channels(); c++) {
        i[c]  = dev.readCurrent(c);
        p[c]  = dev.readPower(c);
        pf[c] = dev.readPowerFactor(c);
    }
    const float freq = dev.readFrequency();
    RbAmpPeriodSnapshot snap;
    const bool snap_ok = dev.readPeriodSnapshot(snap, 60);

    /* CSV row — see SoakMonitor.ino for full 26-column format */
    Serial.printf("%lu,%.3f,%.4f,%.3f,%.4f,%.1f,%.4f,%lu,%lu,%.6f,%lu,%lu,%u\n",
        millis(), u, i[0], p[0], pf[0], freq,
        snap.avg_p[0], snap.latch_ms, snap.master_dt_ms,
        dev.energy().wh(0),
        dev.retryExhaustionCount(),
        dev.sanityRejectCount(),
        snap_ok ? 1 : 0);
}

void setup() {
    Serial.begin(115200);
    Wire.begin(21, 22); Wire.setClock(50000);
    while (!dev.begin()) delay(500);
    dev.resetCounters();
    emitCycle();
}

void loop() {
    static uint32_t next_ms = 5UL * 60UL * 1000UL;
    if (millis() >= next_ms) { emitCycle(); next_ms += 5UL * 60UL * 1000UL; }
    delay(100);
}
```

Full SoakMonitor sketch in [`../../examples/SoakMonitor/SoakMonitor.ino`](../../examples/SoakMonitor/SoakMonitor.ino).
Run with `arduino-cli monitor -p COM3 -c baudrate=115200 > soak.csv` and
leave 12-15 h unattended. Acceptance: `retry_exhaust == 0`,
`sanity_reject == 0`, `period_ok / period_total ≥ 99 %`, Wh monotonic.

---

## Scenario 9 — Battery deep-sleep logger

ESP32 wakes every 10 minutes, latches one period, publishes via WiFi+MQTT,
deep-sleeps. Energy persists across sleep via RTC memory. Library
accumulator is **disabled** — master owns Wh persistence.

Library-relevant skeleton:

```cpp
#include <Wire.h>
#include <RbAmp.h>
#include "esp_sleep.h"

RTC_DATA_ATTR uint32_t rtc_magic = 0;
RTC_DATA_ATTR double   rtc_total_wh = 0;
RTC_DATA_ATTR bool     rtc_primer_done = false;

void setup() {
    Serial.begin(115200);
    Wire.begin();
    RbAmp dev(Wire, 0x50);

    if (rtc_magic != 0xCAFEFEED) {
        rtc_magic = 0xCAFEFEED; rtc_total_wh = 0; rtc_primer_done = false;
    }

    dev.begin();
    dev.energy().disable();   // master owns Wh

    if (!rtc_primer_done) {
        dev.latchPeriod();
        rtc_primer_done = true;
        esp_sleep_enable_timer_wakeup(10ULL * 60ULL * 1000000ULL);
        esp_deep_sleep_start();
    }

    RbAmpPeriodSnapshot snap;
    if (dev.readPeriodSnapshot(snap)) {
        // master_dt_ms measures actual wake-to-wake interval
        rtc_total_wh += (double)snap.avg_p[0] * snap.master_dt_ms / 3600000.0;
        publish_via_wifi(snap.avg_p[0], rtc_total_wh, snap.master_dt_ms);
    }

    esp_sleep_enable_timer_wakeup(10ULL * 60ULL * 1000000ULL);
    esp_deep_sleep_start();
}
void loop() {}
```

Full sketch (with WiFi setup, MQTT publish, RTC magic-marker guards) in
[`07_DeepSleepLogger.ino`](../examples/07_DeepSleepLogger/07_DeepSleepLogger.ino).

**Power budget** (ESP32-WROOM, 10-min wake interval):

- Wake duration: ~3 s (WiFi + MQTT + I2C)
- Wake current: ~80 mA avg → ~0.07 mAh per wake
- Sleep current: ~10 µA (RTC + retained domains)
- ~10 mAh/day total → 2000 mAh Li-ion lasts ~6 months

---

## Scenario 10 — I2C address change

Reassign an rbAmp's slave address from `0x50` to `0x51`. Used once per
module when building a multi-module installation. Disconnect other modules
during this operation to avoid bus collisions.

On current firmware (v1.3+), changing a module's I²C address needs no
special mode, jumper, or factory tooling — you stage the new address,
arm the change, and commit it (a two-phase, magic-armed sequence so an
address can't change by accident), and the module reboots at its new
address. The library enforces the SPEC §10 two-step protocol —
`prepareAddressChange()` arms, `commitAddressChange()` writes within a
5-second window.

```cpp
#include <Wire.h>
#include <RbAmp.h>

static const uint8_t CURRENT_ADDR = 0x50;
static const uint8_t NEW_ADDR     = 0x51;

static RbAmp dev(Wire, CURRENT_ADDR);

void setup() {
    Serial.begin(115200);
    Wire.begin();
    dev.setLogStream(&Serial);

    if (!dev.begin()) {
        Serial.println(F("device not found")); return;
    }
    Serial.print(F("fw=0x")); Serial.println(dev.firmwareVersion(), HEX);

    // Step 1: arm the change (no special mode needed on firmware v1.3+).
    if (!dev.prepareAddressChange(NEW_ADDR)) {
        Serial.print(F("prepare: ")); Serial.println(RbAmp::errorString(dev.lastError()));
        if (dev.lastError() == rbamp::RB_ERR_MODE) {
            Serial.println(F("Module firmware is older than v1.3 - update to v1.3+ for the no-mode two-phase path."));
        }
        return;
    }
    delay(1000);

    // Step 2: commit — writes 0x30, CMD_SAVE_GAINS (700 ms), CMD_RESET (100 ms).
    if (!dev.commitAddressChange()) {
        Serial.print(F("commit: ")); Serial.println(RbAmp::errorString(dev.lastError()));
        return;
    }
    Serial.print(F("device now at 0x")); Serial.println(dev.address(), HEX);

    if (dev.probe()) Serial.println(F("verified"));
}

void loop() { delay(60000); }
```

Full sketch in [`05_AddressChange.ino`](../examples/05_AddressChange/05_AddressChange.ino).
If the 5-second arm window expires before commit, the library returns
`false` with `lastError() == RB_ERR_TIMEOUT` and resets the arm flag — you
must call `prepareAddressChange()` again. This prevents typo-bricks where a
single bad call could lose the device.

---

## Comparison table

| # | LOC | I2C bus | Period? | DRDY? | MQTT? | Persistence |
|:---:|:---:|:---:|:---:|:---:|:---:|---|
| 1 | ~30 | shared | no | no | no | none |
| 2 | ~50 | shared with OLED | yes | no | no | RAM |
| 3 | ~50 | shared 3 modules | yes | no | no | RAM |
| 4 | ~80 | dedicated | yes | no | yes | retained MQTT |
| 5 | ~50 | dedicated | no (RT) | no | no | RAM (master-owned) |
| 6 | ~70 | shared 3 modules | yes | no | yes | retained MQTT |
| 7 | ~40 | dedicated | no (RT) | no | no | RAM |
| 8 | ~250 | dedicated | yes | no | no | none (CSV stream) |
| 9 | ~150 | dedicated | yes | no | yes | RTC memory + MQTT |
| 10 | ~50 | dedicated | n/a | no | no | flash (slave-side) |

## What next

- [DIY Integrations](07_diy_integrations.md) — wire these scenarios into Home Assistant / Node-RED / OpenHAB
- [Cloud Integrations](08_cloud_integrations.md) — AWS IoT / Azure / GCP / InfluxDB pipelines
- [API Reference](09_api_reference.md) — every public method documented
- [Troubleshooting](10_troubleshooting.md) — when scenarios misbehave on your bench

## Related — main rbAmp documentation

- [API Reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference) — formal I²C register / command / error spec the library wraps
- [Arduino Examples (raw I²C)](https://www.rbamp.com/docs/modules-basic-standard-arduino-examples) — same scenarios without the library, useful for porting
- [Period Metering](https://www.rbamp.com/docs/modules-basic-standard-period-metering) — atomic latch concept and master-side energy formula
- [Hardware Connection](https://www.rbamp.com/docs/modules-basic-standard-hardware-connection) — pinout, wiring, CT installation
- [Troubleshooting](https://www.rbamp.com/docs/modules-basic-standard-troubleshooting) — module-side issues (NACK, calibration drift, bus noise)

---

*Source & issues: [`rb-amp/rbamp-arduino`](https://github.com/rb-amp/rbamp-arduino) · this page in the repo: [`docs/06_examples.md`](https://github.com/rb-amp/rbamp-arduino/blob/main/docs/06_examples.md)*
