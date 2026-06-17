# 05 · Quickstart

A five-minute hello-world: install the library, wire up the module,
select a sensor, take your first RT reading and your first per-period
snapshot (Wh).

The details (pinout for a specific host, multi-module bus, choosing an
SCT-013 by range) live in the neighboring chapters:

- [04 · Wiring](04_hardware.md) — hardware details and per-host MCU
  sections
- [03 · Current Sensor Selection](03_sensor_selection.md) — which
  SCT-013 model to pick and why

## What You'll Need

- An rbAmp module (any tier, UI1 for simplicity)
- An Arduino-compatible host — UNO / Mega / ESP32 / ESP8266 / STM32duino / RP2040
- An SCT-013 clamp rated for your maximum current (5A / 10A / 30A / 50A / 100A)
- A 5 V supply to power the module (from the host's USB-5V or external)
- An AC circuit to measure (a lamp, a kettle, a household appliance)

## Step 1 — Install the Library

### Arduino IDE — Library Manager

`Sketch → Include Library → Manage Libraries…` → search for **RbAmp** →
**Install**. There are no dependencies. After installation the examples
appear under `File → Examples → RbAmp`.

### Arduino CLI

```sh
arduino-cli lib install RbAmp
```

### PlatformIO

```ini
[env:esp32dev]
platform   = espressif32
framework  = arduino
lib_deps   = rbamp/RbAmp@^1.0.0
```

## Step 2 — Wiring

Four wires plus an optional DRDY:

| rbAmp pin | Host |
|---|---|
| `VCC` | +5 V (range 4.5..5.5 V) |
| `GND` | GND |
| `SDA` | I²C SDA (UNO A4, ESP32 GPIO21, STM32duino PB7) |
| `SCL` | I²C SCL (UNO A5, ESP32 GPIO22, STM32duino PB6) |
| `DRDY` (optional) | any input-capable GPIO; **a 10 kΩ pull-up to 3.3 V is mandatory**, see [04_hardware.md](04_hardware.md#data_ready-drdy) |

Power **must be 5 V**. The I²C lines run on 3.3 V logic but are 5 V
tolerant — connect them directly to an Arduino UNO with no level
shifter. The module board already carries built-in 4.7 kΩ pull-up
resistors — for a single module no external ones are needed.

The full pinout table for each Arduino host is in
[04 · Wiring](04_hardware.md).

The SCT-013 clamp snaps around the **line conductor (L)**, with the
arrow on the clamp body pointing **in the direction of current toward
the load**. For more detail (the "Current sensor" section) see chapter
[04 · Wiring](04_hardware.md).

## Step 3 — First Sketch (RT reading)

A minimal sketch — verify communication without configuring the sensor:

```cpp
#include <Wire.h>
#include <RbAmp.h>

RbAmp dev(Wire, 0x50);  // default address 0x50

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { /* wait for USB-CDC */ }

    Wire.begin();
#if defined(ESP32) || defined(ESP_PLATFORM) || defined(ESP8266)
    Wire.setClock(50000);   // ESP32/ESP8266: 50 kHz — required for retry discipline
#endif

    while (!dev.begin()) {
        Serial.print(F("rbAmp begin failed: "));
        Serial.println(dev.errorString());
        delay(1000);
    }

    // v1.3: auto-detect the variant via REG_HW_VARIANT — more accurate than
    // the constructor topology hint (which is now a fallback for legacy fw).
    uint8_t variant = dev.readVariant();
    Serial.print(F("Module ready, variant="));
    Serial.println(variant);   // 1=UI1, 4=I1, 5=I2, 6=I3
}

void loop() {
    Serial.print(F("U=")); Serial.print(dev.readVoltage(), 1);
    Serial.print(F("V  I=")); Serial.print(dev.readCurrent(0), 3);
    Serial.print(F("A  P=")); Serial.print(dev.readPower(0), 1);
    Serial.print(F("W  PF=")); Serial.println(dev.readPowerFactor(0), 3);
    delay(1000);
}
```

Upload the sketch and open the Serial Monitor at **115200 baud**.

Expected output (with no sensor calibration):

```text
Module ready.
U=230.4V  I=0.000A  P=0.0W  PF=---
U=230.4V  I=0.000A  P=0.0W  PF=---
```

> `U` shows roughly the mains voltage (220-240 V on 230 V networks) —
> which means the module is connected correctly. `I=0.000 A` even with
> the load switched on is normal at this stage: the module does not yet
> know which CT clamp is in use. With `I=0`, `PF` is mathematically
> undefined (it depends on the firmware — it may be `NaN`, `0`, or a
> placeholder) — the exact form of the value does not matter while the
> current is zero. The next step fixes this.

## Step 4 — Current Sensor Configuration

On firmware v1.3 you **must** tell the module the sensor class and
model. Without that the calibration coefficients are not loaded and the
current readings stay at zero.

Add this to `setup()` after `dev.begin()`. **The `loop()` function
stays the same as in Step 3** — only `setup()` changes:

```cpp
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { /* wait for USB-CDC */ }
    Wire.begin();
#if defined(ESP32) || defined(ESP_PLATFORM) || defined(ESP8266)
    Wire.setClock(50000);   // ESP32/ESP8266: 50 kHz
#endif

    while (!dev.begin()) { delay(500); }

    // Step 1: sensor class. The current rbAmp SKU is SCT-013.
    if (!dev.setSensorClass(RbAmpSensorClass::Sct013)) {
        Serial.print(F("setSensorClass failed: "));
        Serial.println(dev.errorString());
        while (true) { delay(1000); }
    }

    // Step 2: model. For example, SCT-013-030 for a household feed up to ~7 kW.
    if (!dev.setCTModel(RbAmpCTModel::Sct013_030)) {
        Serial.print(F("setCTModel failed: "));
        Serial.println(dev.errorString());
        while (true) { delay(1000); }
    }

    Serial.println(F("Ready."));
}
```

Model codes — the production-safe (characterized) set is `{1, 2, 3, 4, 6}`:

| `code` | Model | Range | Status | Typical use |
|:---:|---|---|:---:|---|
| 1 | SCT-013-005 | 0..5 A | ✅ selectable | Small loads, a single outlet |
| 2 | SCT-013-010 | 0..10 A | ✅ selectable | Refrigerator, washing machine |
| 3 | SCT-013-030 | 0..30 A | ✅ selectable | Household feed up to ~7 kW |
| 4 | SCT-013-050 | 0..50 A | ✅ selectable | EV charger, electric heating |
| 6 | SCT-013-020 | 0..20 A | ✅ selectable | Medium feed — 3-4 kW appliances |
| 5 | SCT-013-100 | 0..100 A | ⏳ reserved | Reserved — requires bench validation, rejected by the library |

For more on selection see [03 · Current Sensor Selection](03_sensor_selection.md).

> These two calls are made **once** at first installation — the choice
> is saved to the module's flash and survives a reset. The total time
> is about **1.4 seconds** (two flash writes × ~700 ms each). The
> library serializes both calls internally — each one blocks until the
> flash write completes, so you don't need to add a `delay()` between
> them: the second call waits on its own until the first one releases.
>
> On subsequent runs of the sketch you can skip `setSensorClass()` and
> `setCTModel()` (the module remembers). But there's no harm either — a
> repeat call with the same value rewrites the same byte.

After restarting the sketch the correct current value should appear:

```text
Ready.
U=230.4V  I=0.523A  P=119.8W  PF=+0.987
```

## Step 5 — Energy Accounting (Wh)

The module returns only instantaneous quantities plus the average power
over a period. **The library itself computes the Wh** from the master
wall-clock:

```text
E_Wh += avg_P × master_dt_s / 3600
        [W]     [seconds]     →  [Wh]
```

where `master_dt_s` is the number of seconds between two successful
`readPeriodSnapshot()` calls.

A minimal periodic-accounting template (once per minute):

```cpp
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { /* wait for USB-CDC */ }
    Wire.begin();
#if defined(ESP32) || defined(ESP_PLATFORM) || defined(ESP8266)
    Wire.setClock(50000);   // ESP32/ESP8266: 50 kHz
#endif
    while (!dev.begin()) { delay(500); }
    dev.setSensorClass(RbAmpSensorClass::Sct013);
    dev.setCTModel(RbAmpCTModel::Sct013_030);
    Serial.println(F("Period meter started; first snapshot in ~60 s..."));
}

void loop() {
    RbAmpPeriodSnapshot snap;
    if (!dev.readPeriodSnapshot(snap)) {
        Serial.print(F("snapshot error: "));
        Serial.println(dev.errorString());
        delay(60000);
        return;
    }

    Serial.print(F("avg P for period: ")); Serial.print(snap.avg_p[0], 2);
    Serial.print(F(" W   accumulated: ")); Serial.print(dev.energy().wh(0), 4);
    Serial.print(F(" Wh   dt=")); Serial.print(snap.master_dt_ms);
    Serial.println(F(" ms"));

    delay(60000);  // the 60-second period sits at the END of loop — the first
                   // snapshot is taken right after setup(), so the user sees
                   // output within seconds rather than after 60 s of silence
}
```

> After Step 4, `setSensorClass()` and `setCTModel()` have already run
> once and are saved to the module's flash; in the template above they
> are there for re-runs of the sketch — the module ignores a repeat
> call with the same value. Error handling is omitted for brevity — add
> `if (!...)` guards if you want to catch the rare communication
> hiccups at startup.

What `readPeriodSnapshot()` does under the hood:

1. Sends the module a period-latch command.
2. Waits 50 ms while the module prepares the snapshot.
3. Checks the ready flag; reads the average/peak power.
4. Updates the internal Wh counter: `+= avg_p × master_dt_s / 3600`.

The first call after `begin()` is a primer: the module returns whatever
it accumulated since power-on (an interval unsuitable for tariff
accounting). The library knows on its own to discard this snapshot —
user code never sees it.

Expected output:

```text
avg P for period: 120.18 W   accumulated: 2.0036 Wh   dt=60012 ms
avg P for period: 120.21 W   accumulated: 4.0073 Wh   dt=60005 ms
...
```

## Step 6 — Quickstart for a Fleet of N Modules

This quickstart shows working with a **single** module. The library's
real canonical scenario is **several** modules on one bus under a single
`RbAmpFleet` handle (mains + N sub-loads). The full example is in
chapter [06 · Examples](06_examples.md), scenario 1 "Mains + N sub-loads
— the 80% canon". A minimal fleet skeleton:

```cpp
#include <Wire.h>
#include <RbAmp.h>
#include <RbAmpFleet.h>

RbAmpFleet fleet(Wire);

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { /* USB-CDC */ }

    Wire.begin();
#if defined(ESP32) || defined(ESP8266)
    Wire.setClock(50000);
#endif

    size_t added = 0;
    if (!fleet.scan(/*match_product=*/true, added)) {
        Serial.println(F("fleet scan failed — bus wedged?"));
        return;
    }
    Serial.print(F("fleet: ")); Serial.print((unsigned)added);
    Serial.print(F(" modules; excluded="));
    Serial.println((unsigned)fleet.excludedCount());
}

RbAmpSnapshot snaps[RBAMP_FLEET_MAX_MODULES];
RbAmpFleetPoll status[RBAMP_FLEET_MAX_MODULES];

void loop() {
    size_t n_ok = 0;
    fleet.pollAll(snaps, status, RBAMP_FLEET_MAX_MODULES, n_ok);

    float p_total = 0.0f;
    fleet.totalPower(p_total);

    Serial.print(F("fleet: "));
    Serial.print((unsigned)n_ok); Serial.print(F("/"));
    Serial.print((unsigned)fleet.count());
    Serial.print(F(" OK, total P = "));
    Serial.print(p_total, 1);
    Serial.println(F(" W"));

    delay(200);
}
```

In the canonical 80% scenario one of the modules is a `UI1` on the mains
feed (it provides `totalPower` and `totalEnergyWh`), while the rest —
`I2`/`I3` — are current sub-meters. The details and the extended
scenarios (mains+sub-loads disaggregation, GC sync for billing-grade
snapshots, the provisioning workflow) are in chapter 06.

## What's Next

- [01 · Overview](01_overview.md) — what rbAmp is and what the library does
- [02 · Module Tiers](02_tiers.md) — which tier fits which task
- [06 · Examples](06_examples.md) — working scenarios: **mains + N
  sub-loads (the 80% canon)**, provisioning workflow, multi-channel
  mixed-CT, fleet GC sync, MQTT, deep-sleep
- [07 · DIY Integrations](07_diy_integrations.md) — Home Assistant /
  Node-RED / OpenHAB
- [08 · Cloud Integrations](08_cloud_integrations.md) — AWS IoT /
  Azure / GCP / InfluxDB
- [09 · API Reference](09_api_reference.md) — the full public API
  (including `RbAmpFleet` + Multi-channel + Error model v1.3 + Identity
  & Provisioning)
- [10 · Troubleshooting](10_troubleshooting.md) — when something isn't
  working (i2c-hang three-layer mitigation, fleet conflict / wedge /
  provisioning failure modes)

