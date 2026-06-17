# 06 · Examples

This chapter opens with the **flagship scenarios** — the real-world
deployment patterns the library was designed around (mains +
N sub-loads on a single bus via `RbAmpFleet`, GC-synchronized
snapshots, multi-channel mixed-CT via `configureChannels()`).
After those come the single-module scenarios — from a minimal "hello
world" to a 12-hour soak test.

Most scenarios have a ready-made sketch in
[`examples/`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/) that you can open directly in the Arduino
IDE — the code below is a **distillation of the key logic**, not the
full sketch (`setup()` / includes / WiFi guards are omitted for
brevity).

| # | Scenario | Complexity | Source |
|:---:|---|:---:|---|
| **1** | **Mains + N sub-loads — the 80% canon** | medium | [`08_FleetSync.ino`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/08_FleetSync/08_FleetSync.ino) |
| **2** | **Provisioning workflow (virgin → fleet)** | low | (composition) |
| **3** | **Multi-channel mixed-CT (I3, different models)** | medium | (composition) |
| **4** | **Fleet GC sync — billing-grade synchronicity** | medium | [`08_FleetSync.ino`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/08_FleetSync/08_FleetSync.ino) |
| 5 | Quick read (single module) | minimal | [`01_QuickRead.ino`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/01_QuickRead/01_QuickRead.ino) |
| 6 | 60-second meter on an OLED | low | [`02_PeriodEnergyOLED.ino`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/02_PeriodEnergyOLED/02_PeriodEnergyOLED.ino) |
| 7 | Monitoring 3 modules (legacy sequential) | low | [`03_MultiModuleBroadcast.ino`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/03_MultiModuleBroadcast/03_MultiModuleBroadcast.ino) |
| 8 | UI variant + per-channel MQTT | medium | [`04_UI3PerChannelMQTT.ino`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/04_UI3PerChannelMQTT/04_UI3PerChannelMQTT.ino) |
| 9 | Bidirectional metering on the master side | medium | [`06_BidirectionalEnergy.ino`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/06_BidirectionalEnergy/06_BidirectionalEnergy.ino) |
| 10 | Whole-home energy balance (3 modules) | high | (composition) |
| 11 | Event-based logging (EMA) | medium | (composition) |
| 12 | 12-hour soak monitor | medium | [`SoakMonitor.ino`](https://github.com/rb-amp/rbamp-arduino) |
| 13 | Battery logger with deep-sleep | medium | [`07_DeepSleepLogger.ino`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/07_DeepSleepLogger/07_DeepSleepLogger.ino) |
| 14 | I3 with different CT per channel (mixed-load deep-dive) | medium | (composition) |
| 15 | Sub-metering: 5×UI1 on a shared bus (billing-grade) | high | (composition) |
| 16 | GC broadcast latch (≥8 modules, billing-grade sync) | high | (composition) |
| 17 | GC group filtering (multi-tenant billing) | high | (composition) |

> Scenarios 1–4 are the flagship ones, built around the library's
> canonical deployment. Their code uses `RbAmpFleet` and is validated
> on bench hardware (a heterogeneous UI1+I2+I3 fleet). Scenarios 5–13
> are single-module and compose patterns; 14–17 are extended fleet
> scenarios (a deep-dive into the capabilities from 1–4). Scenario 7
> is the legacy sequential per-module latch without the fleet API,
> kept for comparison and as a migration path.

> All scenarios assume the sensor class and CT model are already
> configured via `setSensorClass()` + `setCTModel()` (or in a single
> `configureChannels()` call for multi-channel). See
> [05 · Quickstart](05_quickstart.md) Step 4. This is done once at
> install time — the settings are saved to the module's flash and
> survive a reset.

---

## Scenario 1 — Mains + N sub-loads (the 80% canon): integrated metering system

**Goal:** the library's canonical deployment — a coherent metering
system on a heterogeneous fleet, contained in a single loop (discover →
configure → arm GC → loop[gcLatch + checkSync + pollAll + totals]).

**Hardware (HW-validated on the Fix-A fleet):** UI1@0x50 (mains: U+I+P)
+ I2@0x51 (2-channel current sub-meter) + I3@0x52 (3-channel current
sub-meter); ~0.58 A load; **external 4.7 kΩ pull-up**.

```cpp
#include <Wire.h>
#include <RbAmp.h>
#include <RbAmpFleet.h>

RbAmpFleet fleet(Wire);
RbAmpSnapshot   snaps[RBAMP_FLEET_MAX_MODULES];
RbAmpFleetPoll  status[RBAMP_FLEET_MAX_MODULES];
RbAmpFleetSync  sync_status[RBAMP_FLEET_MAX_MODULES];

static void configureOne(RbAmp* dev) {
    if (!dev) return;
    uint8_t variant = dev->readVariant();
    if (variant == 1) {                              // UI1 mains
        dev->setSensorClass(RbAmpSensorClass::Sct013);
        dev->setCTModel(0, 3);                        // SCT-013-030
        dev->saveUserConfig();
    } else if (variant == 5) {                       // I2
        uint8_t models[2] = { 1, 3 };
        dev->configureChannels(RbAmpSensorClass::Sct013, models, 2);
    } else if (variant == 6) {                       // I3
        uint8_t models[3] = { 1, 3, 6 };
        dev->configureChannels(RbAmpSensorClass::Sct013, models, 3);
    }
}

void setup() {
    Serial.begin(115200);
    Wire.begin();
#if defined(ESP32) || defined(ESP8266)
    Wire.setClock(50000);
#endif

    size_t added = 0;
    if (!fleet.scan(true, added)) { Serial.println(F("bus wedged")); return; }
    Serial.print(F("fleet_scan: ")); Serial.print((unsigned)added);
    Serial.print(F(" module(s), ")); Serial.print((unsigned)fleet.excludedCount());
    Serial.println(F(" excluded"));

    for (size_t i = 0; i < fleet.count(); ++i) configureOne(fleet.get(i));

    size_t gc_ok = 0;
    fleet.enableGcAll(0, gc_ok);
    Serial.print(F("enableGcAll → ")); Serial.print((unsigned)gc_ok);
    Serial.print(F("/")); Serial.print((unsigned)fleet.count());
    Serial.println(F(" armed"));
}

void loop() {
    static uint16_t tick = 0;

    fleet.gcLatch(0, tick, 50);
    size_t n_missed = 0;
    fleet.checkSync(tick, sync_status, RBAMP_FLEET_MAX_MODULES, n_missed);

    size_t n_ok = 0;
    fleet.pollAll(snaps, status, RBAMP_FLEET_MAX_MODULES, n_ok);

    float p_total = 0; double e_total = 0;
    fleet.totalPower(p_total);
    fleet.totalEnergyWh(e_total);

    size_t n_total = fleet.count();
    Serial.print(F("ITER ")); Serial.print(tick);
    Serial.print(F(": GC ")); Serial.print((unsigned)(n_total - n_missed));
    Serial.print(F("/")); Serial.print((unsigned)n_total);
    Serial.print(F(" in_sync | poll n_ok=")); Serial.print((unsigned)n_ok);
    Serial.print(F("/")); Serial.print((unsigned)n_total);
    Serial.print(F(" | P_total=")); Serial.print(p_total, 1);
    Serial.print(F(" W  E_total=")); Serial.print(e_total, 3);
    Serial.println(F(" Wh"));

    tick++;
    delay(1000);
}
```

### Bench output (HW-validated, ~0.58 A load)

```text
fleet_scan: 3 module(s), 0 excluded
  [0] @0x50  channels=1  voltage=yes   (mains meter: U + 1 current)
  [1] @0x51  channels=2  voltage=no    (2-channel current sub-meter)
  [2] @0x52  channels=3  voltage=no    (3-channel current sub-meter)
configureChannels(2ch) → mirrors [1 2 -]
configureChannels(3ch) → mirrors [1 2 3]
enableGcAll → 3/3 modules armed

ITER N: GC 3/3 in_sync | poll n_ok=3/3 | P_total≈1260 W  E_total: 0 → 27.84 Wh
   @0x50  V=232.6  I=[5.79]                f=50.00  impl=0x00
   @0x51  V= —     I=[11.58, 3.87]         f=50.00  impl=0x00
   @0x52  V= —     I=[ 3.84, 2.55, 5.84]   f=50.00  impl=0x00  (3rd input free)
```

> The bench figures come from uncalibrated DUTs — they illustrate the
> shape of the data and the flow through the system, **not**
> metrological accuracy. The library's Wh accounting is
> **mathematically exact** (bench measure: relative error vs
> ground-truth = **0.0000%**).

### Notes for deployment

- **One synchronized loop — the whole system.** Each tick: one
  `gcLatch()` → all modules atomically start the period latch
  (`checkSync` = 3/3, skew = 0) → one `pollAll()` → aggregation.
- **`totalPower()` in a heterogeneous fleet.** UI1 supplies active
  power; I2/I3 are current-only and contribute `0`. In the 80% canon,
  `totalPower` ≈ the mains module's power. For a per-load reading on a
  sub-meter: `P_subload[k] ≈ V_mains × I_sub[k]` (an approximation
  when PFs are close).
- **Energy (Wh) — master wall-clock.** The library integrates each
  period against the **master `millis()`**, not the chip `latch_ms`
  (which undercounted dt by −27% in the same session —
  diagnostic-only). See
  [10 · Troubleshooting](10_troubleshooting.md), the section
  "snap.latch_ms reads ~27% lower than the real period".
- **MISS-resilient.** If one module drops out, `pollAll()` marks
  `status[i].ok = false` and continues; the same goes for
  `checkSync()` — `RbAmpFleetSync::reachable = false`.
- **Balance analytics.** In the 80% canon:
  `balance = P_mains − Σ(V_mains × I_sub[k])` — an estimate of "the
  rest".
- **Per-channel disaggregation.** Through `pollAll()` the master sees
  all current channels (1+2+3 = 6 on our bench fleet) at once.
- **L8 soak validated**: a 12-minute run of 1300 cycles under an
  app-WDT (8s timeout, trigger_panic) — `ok=3/3` on every HB,
  `miss=0`, `pollfail=0`, `gcmiss=0`, **0 leak**, **0 reboots**, **0
  WDT triggers**. External 4.7 kΩ pull-up. See
  [10 · Troubleshooting](10_troubleshooting.md).

---

## Scenario 2 — Provisioning workflow (virgin → fleet)

**Goal:** move a new module from the factory address `0x50` to a
working address and join it to an existing fleet, with its
configuration saved to flash.

```cpp
RbAmpFleet fleet(Wire);

void provisionNewModule(uint8_t desired_addr, const char* label = nullptr) {
    /* PRECONDITION: exactly one virgin on the bus at 0x50.
     * MUST be one-virgin-at-a-time — see ch.04 + ch.10. */
    RbAmp* dev = nullptr;
    if (!RbAmpFleet::provision(Wire, desired_addr, /*save_config=*/true, dev)) {
        Serial.print(F("provision @0x"));
        Serial.print(desired_addr, HEX);
        Serial.println(F(" failed — discipline violation? see ch.10"));
        return;
    }
    Serial.print(F("provisioned virgin → 0x"));
    Serial.print(desired_addr, HEX);
    Serial.println(F(" (+saved)"));

    if (label) dev->writeLabel(label);
    dev->setSensorClass(RbAmpSensorClass::Sct013);
    dev->setCTModel(0, 3);
    dev->saveUserConfig();
    fleet.add(dev);
}
```

### Bench output (HW-validated success path)

```text
before: present(0x50)=yes  present(0x52)=no
virgin @0x50: variant=3-channel, channels=3
RbAmpFleet::provision(→0x52, save_config=true) = OK
  (lib log: "provisioned virgin → 0x52 (+saved)")
post:   handle now answers @0x52, isProvisioned()=true
after:  present(0x50)=no   present(0x52)=yes
```

### Failure paths

| Return | Condition | Recovery |
|---|---|---|
| `false` + virgin not found | Nothing answers at `0x50` | Check VCC/I2C; bus-scan ([10 · Troubleshooting](10_troubleshooting.md)) |
| `false` + conflict | Conflict at `0x50`/`desired_addr` | **Discipline violation** — more than one virgin. Power-cycle + disconnect all but one |
| `false` + timeout | Two-phase commit went through, module never showed up | Power-cycle; if it recurs — flash-cycle issue |
| `false` + addr out of range | `desired_addr` outside `0x08..0x77` | Use a valid 7-bit address |

> **Signature (lib-arduino confirmed):**
> `static bool RbAmpFleet::provision(TwoWire& bus, uint8_t desired_addr, bool save_config, RbAmp*& out_dev)`.
> `out_dev` belongs to the caller on success (typically passed to
> `fleet.add(out_dev)`).

---

## Scenario 3 — Multi-channel mixed-CT (I3, different model per channel)

**Goal:** demonstrate `configureChannels()` on an I3 — three channels
with different CT models (ch0=SCT-013-005, ch1=SCT-013-030,
ch2=SCT-013-020) with a **single** terminal flash-cycle, and confirm
persistence across a reboot.

```cpp
#include <RbAmp.h>

RbAmp dev(Wire, 0x52);   // I3 sub-meter

void configureMixedCT() {
    uint8_t models[3] = { 1, 3, 6 };
    /* 1=SCT-013-005 (5A), 3=SCT-013-030 (30A), 6=SCT-013-020 (20A) */

    uint32_t t0 = millis();
    if (!dev.configureChannels(RbAmpSensorClass::Sct013, models, 3)) {
        uint8_t reg_err = 0;
        dev.readLastError(reg_err);
        Serial.print(F("configure rejected: REG_ERROR=0x"));
        Serial.println(reg_err, HEX);
        return;
    }
    uint32_t t1 = millis();
    Serial.print(F("configureChannels latency = "));
    Serial.print(t1 - t0); Serial.println(F(" ms"));

    /* Verify mirror */
    uint8_t m0 = 0, m1 = 0, m2 = 0;
    dev.readCTModelCh(0, m0);
    dev.readCTModelCh(1, m1);
    dev.readCTModelCh(2, m2);
    Serial.print(F("applied: ["));
    Serial.print(m0); Serial.print(F(" "));
    Serial.print(m1); Serial.print(F(" "));
    Serial.print(m2); Serial.println(F("]  (expected [1 3 6])"));

    /* Persist verify — reset + re-read */
    dev.reset();
    delay(300);
    dev.readCTModelCh(0, m0);
    dev.readCTModelCh(1, m1);
    dev.readCTModelCh(2, m2);
    Serial.print(F("after reboot: ["));
    Serial.print(m0); Serial.print(F(" "));
    Serial.print(m1); Serial.print(F(" "));
    Serial.print(m2); Serial.println(F("]"));
}
```

### Bench output (HW-validated on the Fix-A I3)

```text
configureChannels(Sct013, {1, 3, 6}, 3) = OK
  → readCTModelCh 0/1/2 = [1, 3, 6]   raw mirror 0x51/52/53 = [01 03 06]
  → ch0 NOT clobbered (Fix-A canon: bind order-independent)
configureChannels latency ≈ 1.4 s
after reboot: [1 3 6]  (persisted through reset)
```

### The "single SAVE" property — why it matters

`configureChannels()` issues a **single** terminal
`CMD_SAVE_USER_CONFIG`. The alternative (per-channel `setCTModel()` +
`saveUserConfig()` after each one) means three flash erase + write
cycles.

**Bench latency comparison (on the same I3):**

| Approach | Latency | Flash-cycles |
|---|---|---|
| `configureChannels(...)` — 1 SAVE | **~1.4 s** | **1** |
| 3× `(setCTModel + saveUserConfig)` | **~3.5 s** | **3** |

3× slower — `(~2.5×)` × `~700 ms`. The key thing is the **3× wear-cycle
difference** on the same flash page.

> **Valid codes (Bench-characterized).** The SCT-013 family on the
> current bench: `{1, 2, 3, 4, 6}`. Codes `5` (SCT-013-100) and `7`
> (SCT-013-060) exist in the API but are **uncharacterised** — they
> return `false` + `lastError() == RB_ERR_PARAM` (a client-side
> reject with no I²C transaction).

---

## Scenario 4 — Fleet GC sync (billing-grade synchronicity)

**Goal:** demonstrate an atomic latch across the whole fleet with a
single I²C General-Call frame. Unlike the sequential per-module
`latchPeriod()`, GC synchronizes all modules in **one** bus
transaction.

```cpp
#include <RbAmpFleet.h>

RbAmpFleet fleet(Wire);
RbAmpFleetSync sync_status[RBAMP_FLEET_MAX_MODULES];

void setup() {
    Wire.begin(); Wire.setClock(50000);
    size_t added = 0;
    fleet.scan(true, added);

    size_t gc_ok = 0;
    fleet.enableGcAll(0, gc_ok);
    Serial.print(F("GC armed: "));
    Serial.print((unsigned)gc_ok); Serial.print(F("/"));
    Serial.println((unsigned)fleet.count());
}

void loop() {
    static uint16_t tick = 0;
    fleet.gcLatch(0, tick, 50);

    size_t n_missed = 0;
    fleet.checkSync(tick, sync_status, RBAMP_FLEET_MAX_MODULES, n_missed);
    size_t n_total = fleet.count();
    Serial.print(F("tick=")); Serial.print(tick);
    Serial.print(F("  in_sync="));
    Serial.print((unsigned)(n_total - n_missed));
    Serial.print(F("/")); Serial.println((unsigned)n_total);

    tick++;
    delay(100);
}
```

### Bench output (HW-validated)

```text
enableGcAll → 3/3 modules armed
tick=0   in_sync=3/3
tick=1   in_sync=3/3
...
tick=9   in_sync=3/3      skew=0 (validated on tick=0xABCD)
```

### When to use GC sync

- **Billing-grade synchronicity.** A sequential per-module latch on
  those same 3 modules accumulates 100–300 µs per module × N of skew.
  GC delivers `skew = 0` by bench measurement.
- **Large fleet.** The gap between "sequential N × round_trip" and "a
  single GC frame" grows linearly.

**Witness via `checkSync()`:** confirms that all modules actually
received the GC frame. If a module was busy booting / in a SAVE cycle
— `RbAmpFleetSync::reachable = false`.

**Precondition.** GC must be **enabled** on every module
(`fleet.enableGcAll()` or per-device `dev.enableGc(true)`). A fresh
module with factory defaults does **not** accept GC — by design, a
guard against accidental broadcast on a bring-up bus.

---

## Scenario 5 — Quick read (single module)

**Goal:** print U / I / P / PF / frequency to Serial once per second.
The very same "hello world" you wrote in
[05 · Quickstart](05_quickstart.md), but via `dev.readAll(s)` — a
single-shot read of the entire RT block into one structure.

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
        Serial.print(F("begin: "));
        Serial.println(dev.errorString());
        delay(1000);
    }
}

void loop() {
    RbAmpSnapshot s;
    if (!dev.readAll(s)) {
        Serial.print(F("read fail: "));
        Serial.println(dev.errorString());
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

**What happens on the bus:** `readAll()` on a UI3 performs ~53
single-byte transactions (13 float values × 4 bytes + 1 frequency
byte). At 100 kHz with retry headroom, that's on the order of 12–15 ms.
If you don't need all the values, call the per-property methods
(`readVoltage()`, `readCurrent(ch)`) — fewer bytes on the bus.

---

## Scenario 6 — 60-second meter on an OLED

**Goal:** a Wh meter, refreshed once a minute, on a 128×64 SSD1306
OLED sitting on the same I²C bus (address 0x3C). The library's key
mechanism — `readPeriodSnapshot()` encapsulates
latch + settle + valid-check + read + Wh-tick in a single call.

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
    oled.setCursor(0, 16); oled.setTextSize(2);
    oled.print(snap.avg_p[0], 1); oled.println(F(" W"));
    oled.setTextSize(1);
    oled.print(F("Wh:")); oled.println(dev.energy().wh(0), 4);
    oled.print(F("ok=")); oled.print(snapshots_ok);
    oled.print(F(" bad=")); oled.println(snapshots_bad);
    oled.display();
}
```

Full sketch — [`02_PeriodEnergyOLED.ino`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/02_PeriodEnergyOLED/02_PeriodEnergyOLED.ino).

**Handling stale snapshots.** `readPeriodSnapshot()` returns `false`
with `RB_ERR_STALE` if the module didn't manage to prepare a new
snapshot by read time. The library **still records** its own
wall-clock timestamp internally — the next successful snapshot won't
be double-counted over the interval.

---

## Scenario 7 — Monitoring 3 modules (legacy sequential) on a single bus

**Goal:** poll 3 modules at addresses 0x50 / 0x51 / 0x52 from one
master. Each module must have a unique address (addresses are assigned
at the factory/integrator install stage — see
[09 · API Reference](09_api_reference.md) on the address-change
methods).

> On v1.3 firmware (`REG_VERSION = 0x04`) `RbAmp::broadcastLatch()` is
> the canonical fleet-sync path once General-Call has been enabled
> fleet-wide (via `enableGc(true)` per device, or `RbAmpFleet::enableGcAll()`
> on a fleet handle). On legacy v1.0-v1.2 firmware (or when GC has been
> left disabled on the modules) the method returns `false` without
> touching the bus, and this sketch falls back to a sequential series
> of `latchPeriod()` calls with one shared 50 ms settle. Skew between
> modules at 100 kHz in the sequential fallback: ~810 µs total across
> the 3 devices, which is < 0.0015% of the 60-second period.

```cpp
#include <Wire.h>
#include <RbAmp.h>

// Names mod0/mod1/mod2 (not T0/T1/T2) — on the ESP32 the identifiers
// T0/T1/T2 are already taken in the core by the touch-pin macros from
// `pins_arduino.h` and would cause a compile error when declared as
// global variables.
static RbAmp mod0(Wire, 0x50);
static RbAmp mod1(Wire, 0x51);
static RbAmp mod2(Wire, 0x52);
static RbAmp* modules[] = { &mod0, &mod1, &mod2 };
static const uint8_t N = sizeof(modules) / sizeof(modules[0]);

void setup() {
    Serial.begin(115200);
    Wire.begin();
    Wire.setClock(400000);   // 400 kHz reduces skew to ~100 µs
    for (uint8_t i = 0; i < N; ++i) modules[i]->begin();
}

void loop() {
    for (uint8_t i = 0; i < N; ++i) modules[i]->latchPeriod();
    delay(50);   // one shared settle for all 3 modules

    for (uint8_t i = 0; i < N; ++i) {
        RbAmpPeriodSnapshot snap;
        if (modules[i]->readPeriodSnapshot(snap, /*settle_ms=*/0,
                                                  /*skip_latch=*/true)) {
            Serial.print(F("mod")); Serial.print(i);
            Serial.print(F(" P=")); Serial.print(snap.avg_p[0], 0);
            Serial.print(F("W  Wh="));
            Serial.println(modules[i]->energy().wh(0), 3);
        }
    }
    delay(60000);
}
```

Full sketch (with bus-scan probe + error reporting) —
[`03_MultiModuleBroadcast.ino`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/03_MultiModuleBroadcast/03_MultiModuleBroadcast.ino).

---

## Scenario 8 — UI variant + per-channel MQTT

**Goal:** a UI3 module with 3 CT clamps on three independent lines.
Per-channel Wh counters, published to MQTT once a minute. It shows
that `dev.energy().wh(ch)` works on each channel independently — no
manual `total_wh[3]` array is needed on the master side.

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
    snprintf(payload, sizeof(payload),
             "{\"power\":%.1f,\"energy\":%.4f}", avg_p, e_wh);
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
    Wire.setClock(50000);    // ESP32 baseline mitigation — see 10 · Troubleshooting
    dev.setLogStream(&Serial);
    while (!dev.begin()) { delay(500); }
}

void loop() {
    if (!mqtt.connected()) mqtt.connect("rbamp-ui3");
    mqtt.loop();             // every iteration — for keepalive

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

Full sketch — [`04_UI3PerChannelMQTT.ino`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/04_UI3PerChannelMQTT/04_UI3PerChannelMQTT.ino).
For HA MQTT Auto-discovery built on this pattern — see
[07 · DIY integrations](07_diy_integrations.md).

> A note on MQTT topics. The snippet above is a pedagogical
> simplification with aggregated JSON in a single `rbamp/<name>/state`
> topic. The `04_UI3PerChannelMQTT.ino` sketch itself uses a
> per-field topic structure (`rbamp/<id>/ch<n>/power_w`,
> `rbamp/<id>/ch<n>/energy_wh`) — which fits HA Auto-discovery better
> (each entity gets its own state topic). If you're building an
> integration for HA, use the per-field pattern — details in
> [07 · DIY integrations](07_diy_integrations.md).

---

## Scenario 9 — Bidirectional metering on the master side

**Goal:** split signed instantaneous power into gross-consume and
gross-export. Use this pattern on the BASIC tier, where the
**firmware** clips negative values in `snap.avg_p[ch]` (the
period-averaged power) — sample `dev.readPower(0)` (instantaneous
power, **signed on all tiers**) at 5 Hz on the master side and bucket
it yourself.

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
    if (millis() - last_sample_ms < 200) { delay(5); return; }  // 5 Hz
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
        Serial.print(F("Wh  net="));
        Serial.print(consume_wh - export_wh, 4);
        Serial.println(F("Wh"));
    }
}
```

Full sketch — [`06_BidirectionalEnergy.ino`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/06_BidirectionalEnergy/06_BidirectionalEnergy.ino).

On future STANDARD / PRO firmware tiers (planned for v1.3+) the period
accumulator `dev.energy().wh(0)` will already return a signed net
balance — this master-side split is only needed for separate
gross-consume / gross-export reporting.

---

## Scenario 10 — Whole-home energy balance

**Goal:** 3 modules — mains (bidirectional), solar (generation only),
loads (UI3 for per-appliance). A combined dashboard is published once a
minute.

```cpp
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <RbAmp.h>

static RbAmp mains(Wire, 0x50);    // bidirectional mains
static RbAmp solar(Wire, 0x51);    // generation only
static RbAmp loads(Wire, 0x52);    // UI3 — per-appliance
static RbAmp* mods[] = { &mains, &solar, &loads };
static WiFiClient espClient;
static PubSubClient mqtt(espClient);

void setup() {
    Serial.begin(115200);
    WiFi.begin("ssid", "pass");
    while (WiFi.status() != WL_CONNECTED) delay(500);
    mqtt.setServer("192.168.1.10", 1883);
    mqtt.connect("home-balance");

    Wire.begin();
    for (auto m : mods) while (!m->begin()) delay(500);
}

void loop() {
    delay(60000);

    for (auto m : mods) m->latchPeriod();
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

For an explicit gross-consume / gross-export split on mains — combine
this with the pattern from Scenario 5 (a separate 5 Hz loop on
`mains.readPower(0)`).

This is a composite scenario with no dedicated sketch — assemble it
from
[`04_UI3PerChannelMQTT.ino`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/04_UI3PerChannelMQTT/04_UI3PerChannelMQTT.ino)
and [`06_BidirectionalEnergy.ino`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/06_BidirectionalEnergy/06_BidirectionalEnergy.ino).

---

## Scenario 11 — Event-based logging (EMA)

**Goal:** on every 200 ms RT window, compare instantaneous power
against an exponential moving average. Log significant deviations —
loads such as a microwave, kettle, or hairdryer "appear" in the log as
a turn-on / turn-off event.

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
    p_ema = dev.readPower(0);  // seed so the first reading isn't an event
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

A composite scenario — no dedicated sketch. Combine it with the
MQTT publishing from Scenario 4 for HA-side automations, or write the
events to SD via `Adafruit_SD` for offline logging.

---

## Scenario 12 — 12-hour soak monitor

**Goal:** a long soak test of the library against a real DUT — 12–15
hours of dense per-cycle load, one CSV row every 5 minutes with a
full set of diagnostic counters. Used for regression qualification of
the firmware and the library.

Key features:

- **`RBAMP_NACK_RETRY_ATTEMPTS=5` override** — defined BEFORE the
  `#include` to raise the retry budget under dense load (≥10
  single-byte reads per cycle). The default of 3 is enough for
  ESPHome-style 60 s polling, but SoakMonitor reads ~45 bytes per
  cycle and needs extra headroom.
- **`dev.rawTopology()`** — a direct wire-read of `REG_TOPOLOGY`
  (0x24). Returns 1/2/3 (SINGLE/SPLIT_PHASE/THREE_PHASE) or `0xFF` on
  an I²C error. For full SKU detection use `dev.hwVariant()` (reads
  `REG_HW_VARIANT 0x55`: 1=UI1, 2=UI2, 3=UI3, 4=I1, 5=I2, 6=I3).
- **`dev.retryExhaustionCount()` + `dev.sanityRejectCount()`** — both
  must stay **at zero** for the entire run.

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

    /* CSV row — the full 28-column format is in SoakMonitor.ino */
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
    if (millis() >= next_ms) {
        emitCycle();
        next_ms += 5UL * 60UL * 1000UL;
    }
    delay(100);
}
```

Full sketch — [`SoakMonitor.ino`](https://github.com/rb-amp/rbamp-arduino).
To run it:

```bash
arduino-cli monitor -p COM3 -c baudrate=115200 > soak.csv
```

Leave it running unattended for 12–15 hours. Acceptance criteria:
`retry_exhaust == 0`, `sanity_reject == 0`,
`period_ok / period_total ≥ 99%`, Wh growing monotonically.

---

## Scenario 13 — Battery logger with deep-sleep

**Goal:** the ESP32 wakes once every 10 minutes, does a single period
latch, publishes over WiFi+MQTT, and goes back into deep-sleep. Energy
persists across sleep cycles through RTC memory. The library's
accumulator is **disabled** — the master owns the Wh persistence
itself.

Library-relevant skeleton:

> ⚠ **Important about deep-sleep and the v1.0/v1.1 firmware.** On the
> current firmware `dev.begin()` always issues a CMD_LATCH_PERIOD
> primer, which resets the firmware's period accumulator. That means
> after a deep-sleep wake you can't simply call `readPeriodSnapshot()`
> with defaults — that call would re-latch and return near-zero data
> ~50 ms after the primer. The correct pattern below uses
> `skip_latch=true` to read the data accumulated over the sleep
> interval, and the **master's own RTC timer** for the interval
> between wakeups (the `snap.master_dt_ms` field reflects only the time
> within the current wake cycle, not wake-to-wake).

```cpp
#include <Wire.h>
#include <RbAmp.h>
#include "esp_sleep.h"
#include "esp_timer.h"

RTC_DATA_ATTR uint32_t rtc_magic        = 0;
RTC_DATA_ATTR double   rtc_total_wh     = 0;
RTC_DATA_ATTR bool     rtc_primer_done  = false;
RTC_DATA_ATTR uint64_t rtc_last_wake_us = 0;

void setup() {
    Serial.begin(115200);
    Wire.begin();
    RbAmp dev(Wire, 0x50);

    if (rtc_magic != 0xCAFEFEED) {
        rtc_magic = 0xCAFEFEED;
        rtc_total_wh = 0;
        rtc_primer_done = false;
        rtc_last_wake_us = 0;
    }

    dev.begin();              // begin() issues the primer LATCH itself
    dev.energy().disable();   // the master owns the Wh

    if (!rtc_primer_done) {
        // First wake: the primer just latched "dirty" pre-wake data.
        // Note the time and go straight back to sleep — the next wake
        // in 10 min will get a proper 10-minute accumulator.
        rtc_last_wake_us = esp_timer_get_time();
        rtc_primer_done = true;
        esp_sleep_enable_timer_wakeup(10ULL * 60ULL * 1000000ULL);
        esp_deep_sleep_start();
    }

    // skip_latch=true reads what begin()'s primer latched —
    // i.e. the accumulator over the elapsed 10-minute sleep interval.
    RbAmpPeriodSnapshot snap;
    if (dev.readPeriodSnapshot(snap, /*settle_ms=*/0, /*skip_latch=*/true)) {
        // master_dt_ms here is the time within the current wake (after
        // the primer); for energy integration we use our own RTC timer,
        // which survived the deep-sleep.
        const uint64_t now_us = esp_timer_get_time();
        const double   dt_s   = (double)(now_us - rtc_last_wake_us) / 1000000.0;
        rtc_last_wake_us = now_us;

        rtc_total_wh += (double)snap.avg_p[0] * dt_s / 3600.0;
        publish_via_wifi(snap.avg_p[0], rtc_total_wh, (uint32_t)(dt_s * 1000));
    }

    esp_sleep_enable_timer_wakeup(10ULL * 60ULL * 1000000ULL);
    esp_deep_sleep_start();
}
void loop() {}
```

Full sketch (with WiFi setup, MQTT publish, RTC magic-marker guards) —
[`07_DeepSleepLogger.ino`](https://github.com/rb-amp/rbamp-arduino/tree/main/examples/07_DeepSleepLogger/07_DeepSleepLogger.ino).

> 🛣 **Roadmap.** A future library version (v1.3+) is expected to
> bring `warmBegin()` — a lightweight initialization variant without
> the CMD_LATCH_PERIOD primer, which will make it simpler to read
> `readPeriodSnapshot()` with defaults after a deep-sleep wake. Until
> then, the pattern above (skip_latch=true + the master's RTC timer)
> is the canonical one.

**Energy budget** (ESP32-WROOM, 10-minute wake interval):

- Active cycle duration: ~3 s (WiFi + MQTT + I²C)
- Active current: ~80 mA average → ~0.07 mAh per wake
- Sleep current: ~10 µA (RTC + retained domains)
- ~10 mAh per day → a 2000 mAh Li-ion runs for ~6 months

---

## Scenario 14 — I3 with different CT per channel (mixed-load monitoring)

A UI3 module monitors **three independent lines on a single phase** with different load levels:
- Channel 0 — a single outlet (SCT-013-005, ≤ 5 A) — TV / router / standby.
- Channel 1 — the kitchen line (SCT-013-030, ≤ 30 A) — kettle / microwave.
- Channel 2 — a water-heater boiler (SCT-013-020, ≤ 20 A) — a typical ~3-5 kW load.

All three channels are fed from a **single phase** (in phase with the U channel) — P/PF/Q are correct for all of them. See the single-voltage caveat in [03 · Current sensor selection](03_sensor_selection.md).

```cpp
#include <Wire.h>
#include <RbAmp.h>

RbAmp dev(Wire, 0x50);

void setup() {
    Wire.begin();
#if defined(ESP32) || defined(ESP_PLATFORM) || defined(ESP8266)
    Wire.setClock(50000);
#endif
    Serial.begin(115200);
    while (!dev.begin()) { delay(500); }

    // Per-channel CT model — order doesn't matter on v1.3 (Fix A pure-staging)
    dev.setSensorClass(RbAmpSensorClass::Sct013);
    dev.setCTModel(2, RbAmpCTModel::Sct013_020);   // ch2: boiler (code 6)
    dev.setCTModel(1, RbAmpCTModel::Sct013_030);   // ch1: kitchen (code 3)
    dev.setCTModel(0, RbAmpCTModel::Sct013_005);   // ch0: outlet (code 1)
}

void loop() {
    float u  = dev.readVoltage();
    float i0 = dev.readCurrent(0), p0 = dev.readPower(0);
    float i1 = dev.readCurrent(1), p1 = dev.readPower(1);
    float i2 = dev.readCurrent(2), p2 = dev.readPower(2);

    Serial.printf("U=%.1f V\n", u);
    Serial.printf("  outlet:  I=%.3f A  P=%.1f W\n", i0, p0);
    Serial.printf("  kitchen: I=%.2f A  P=%.1f W\n", i1, p1);
    Serial.printf("  boiler:  I=%.2f A  P=%.1f W\n", i2, p2);
    Serial.printf("  total:   P=%.1f W\n", p0 + p1 + p2);

    delay(2000);
}
```

**When to use:** a single phase with three load groups of different current magnitudes. Each clamp's resolution matches its range (SCT-013-005 is accurate at a 50 W standby load, SCT-013-020 doesn't saturate on a typical 3-5 kW boiler). For peaks > 11.5 kW (a large EV charger / an industrial load) — several channels with SCT-013-050 in parallel, or a WIRED_CT SKU (roadmap).

---

## Scenario 15 — Sub-metering: 5×UI1 on a shared bus (billing-grade)

Five independent lines in a single-phase apartment, one UI1 on each. Period sync via **strategy 1** (sequential latch + shared settle) — see [04 · Wiring](04_hardware.md).

```cpp
#define N_METERS 5
RbAmp meters[N_METERS] = {
    {Wire, 0x50}, {Wire, 0x51}, {Wire, 0x52},
    {Wire, 0x53}, {Wire, 0x54},
};

unsigned long period_start_ms = 0;
const unsigned long PERIOD_MS = 60000;   // 60-second metering period

void setup() {
    Wire.begin();
#if defined(ESP32) || defined(ESP_PLATFORM) || defined(ESP8266)
    Wire.setClock(50000);
#endif
    Serial.begin(115200);

    for (int i = 0; i < N_METERS; i++) {
        while (!meters[i].begin()) { delay(500); }
    }
    period_start_ms = millis();
}

void loop() {
    if (millis() - period_start_ms < PERIOD_MS) {
        delay(100);
        return;
    }

    // Phase 1: sequential latch (~5 ms skew between modules at 50 kHz)
    for (auto& m : meters) m.latchPeriod();

    // Phase 2: shared settle
    delay(50);

    // Phase 3: read the snapshots
    float total_wh = 0.0f;
    for (int i = 0; i < N_METERS; i++) {
        RbAmpSnapshot snap;
        if (meters[i].readPeriodSnapshot(snap, /*skip_latch=*/true) && snap.valid) {
            float wh = snap.avg_p[0] * (snap.period_ms / 3600000.0f);
            Serial.printf("[%d] avg_p=%.1f W  Wh_period=%.2f\n",
                          i, snap.avg_p[0], wh);
            total_wh += wh;
        }
    }

    Serial.printf("TOTAL for the period: %.2f Wh\n\n", total_wh);
    period_start_ms = millis();
}
```

**Skew analysis:** 5 modules × ~1 ms/latch = ~5 ms relative to the 60 s period = 0.008%. Negligible for billing.

**Bus budget:** 5 modules × ~5 ms RT block = 25 ms per full round; a 5 Hz polling rate fits. Headroom up to ~15 UI1 modules.

---

## Scenario 16 — GC broadcast latch (≥8 modules, billing-grade sync)

At ≥ 8 modules the skew of strategy 1 approaches ~10 ms. **Strategy 2** — a General-Call broadcast latch, zero skew.

**Preconfiguration (once per fleet module)**:

```cpp
// Enable GC reception on each module
dev.writeReg(0x27, 0x01);              // REG_FLEET_CONFIG.bit0 = 1
dev.saveUserConfig();                  // CMD_SAVE_USER_CONFIG (production-OK)
dev.reset();                           // CMD_RESET; settle ~300 ms
```

**Runtime broadcast with a witness**:

```cpp
#define N_METERS 12
RbAmp meters[N_METERS] = { /* initialization */ };
uint16_t tick_counter = 0;

bool billing_grade_latch() {
    // GC frame: A5 27 <group> <tick_lo> <tick_hi> on addr 0x00
    Wire.beginTransmission(0x00);
    Wire.write(0xA5);                          // GC_FRAME_MAGIC
    Wire.write(0x27);                          // CMD_LATCH_PERIOD opcode
    Wire.write(0x00);                          // group = 0 (all-call)
    Wire.write(tick_counter & 0xFF);
    Wire.write(tick_counter >> 8);
    if (Wire.endTransmission() != 0) {
        return false;   // GC frame transmission failed
    }
    tick_counter++;

    delay(50);   // settle + 1 commit cycle for witness refresh

    // Witness check
    int n_ok = 0;
    for (int i = 0; i < N_METERS; i++) {
        uint8_t valid = meters[i].readReg(0x07);   // REG_V03_PERIOD_VALID
        if (valid == 1) n_ok++;
    }

    if (n_ok < N_METERS) {
        // Fall back to a sequential latch for the modules that missed
        for (int i = 0; i < N_METERS; i++) {
            uint8_t valid = meters[i].readReg(0x07);
            if (valid != 1) meters[i].latchPeriod();
        }
        delay(50);
    }

    return n_ok == N_METERS;
}
```

**Skew analysis**: a GC frame is ~1 ms at 50 kHz → all 12 modules latch **simultaneously** at the bus level. Skew is practically 0.

**When to use**: ≥ 8 modules and/or billing-grade accuracy.

**When NOT to use**: on a mixed bus with non-rbAmp devices (GC may conflict with standard I²C reset commands).

---

## Scenario 17 — GC group filtering (multi-tenant billing)

10 modules on a single bus, **subsets** latch independently (e.g. tariff A during the day, tariff B at night).

**Setup (once)**:

```cpp
// Tariff A — modules 0..4 → group = 1
for (int i = 0; i < 5; i++) {
    meters[i].writeReg(0x28, 1);   // REG_GROUP_ID = 1
    meters[i].saveUserConfig();
    meters[i].reset();
}

// Tariff B — modules 5..9 → group = 2
for (int i = 5; i < 10; i++) {
    meters[i].writeReg(0x28, 2);
    meters[i].saveUserConfig();
    meters[i].reset();
}
```

**Runtime — independent latch per tariff**:

```cpp
void daytime_cycle(uint16_t tick) {
    Wire.beginTransmission(0x00);
    Wire.write(0xA5); Wire.write(0x27);
    Wire.write(0x01);             // group = 1 (tariff A only)
    Wire.write(tick & 0xFF); Wire.write(tick >> 8);
    Wire.endTransmission();
    delay(50);
    // read the 5 tariff-A modules
}

void nighttime_cycle(uint16_t tick) {
    Wire.beginTransmission(0x00);
    Wire.write(0xA5); Wire.write(0x27);
    Wire.write(0x02);             // group = 2 (tariff B only)
    Wire.write(tick & 0xFF); Wire.write(tick >> 8);
    Wire.endTransmission();
    delay(50);
    // read the 5 tariff-B modules
}
```

---

## Scenario summary table

| # | LOC | I²C bus | Period? | DRDY? | MQTT? | Persistence |
|:---:|:---:|:---:|:---:|:---:|:---:|---|
| 1 | ~30 | shared | no | no | no | no |
| 2 | ~50 | shared with OLED | yes | no | no | RAM |
| 3 | ~50 | shared 3 modules | yes | no | no | RAM |
| 4 | ~80 | dedicated | yes | no | yes | retained MQTT |
| 5 | ~50 | dedicated | no (RT) | no | no | RAM (master-owned) |
| 6 | ~70 | shared 3 modules | yes | no | yes | retained MQTT |
| 7 | ~40 | dedicated | no (RT) | no | no | RAM |
| 8 | ~250 | dedicated | yes | no | no | no (CSV stream) |
| 9 | ~150 | dedicated | yes | no | yes | RTC memory + MQTT |
| **10** | ~60 | dedicated UI3 | no (RT) | no | no | RAM |
| **11** | ~80 | shared 5×UI1 | yes (billing) | no | no | RAM |
| **12** | ~100 | shared 12 modules | yes (GC sync) | no | no | RAM |
| **13** | ~60 | shared 10 modules multi-tenant | yes (GC groups) | no | no | RAM |

## What's next

- [07 · DIY integrations](07_diy_integrations.md) — Home Assistant /
  Node-RED / OpenHAB built on these scenarios
- [08 · Cloud integrations](08_cloud_integrations.md) — AWS IoT /
  Azure / GCP / InfluxDB
- [09 · API Reference](09_api_reference.md) — every public method
  documented
- [10 · Troubleshooting](10_troubleshooting.md) — when scenarios
  behave differently on your bench

