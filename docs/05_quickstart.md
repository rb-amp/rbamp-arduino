# Quickstart

Five-minute hello-world: install, wire, read your first measurement,
integrate Wh. If you've already read [Hardware Setup](04_hardware.md)
for your host MCU, skip the wiring section.

## Prerequisites

- One rbAmp module (any tier — BASIC UI1 / STANDARD / PRO all work the same)
- Any I2C-capable Arduino-supported board (Uno, ESP32, ESP8266, STM32duino, RP2040)
- 4 wires: `3V3`, `GND`, `SDA`, `SCL` between module and host
- Arduino IDE 2.x (or Arduino CLI / PlatformIO)
- A live AC circuit you can clamp the CT around (lamp, kettle, electric heater)

## Step 1 — Install the library

### Arduino IDE

`Sketch → Include Library → Manage Libraries…` → search **RbAmp** →
**Install**.

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

## Step 2 — Wire it up

| rbAmp | Host MCU |
|---|---|
| `3V3` | 3.3 V supply |
| `GND` | GND |
| `SDA` | I2C SDA (Uno: A4 · ESP32: GPIO21 · ESP8266 D2 · STM32 Blue Pill PB7) |
| `SCL` | I2C SCL (Uno: A5 · ESP32: GPIO22 · ESP8266 D1 · STM32 Blue Pill PB6) |

If you're on ESP32, **set the bus to 50 kHz** in your sketch — SPEC §B.5
mandate against the PY32 v1.0 NACK pattern. Other hosts can run 100 kHz
or 400 kHz freely.

Clamp the CT around **one** AC live conductor (not the entire flex with
both live + neutral — that nets out to zero). The arrow on the clamp body
should point **into** the load.

> ⚠ **Do not wire the module's `NRST` pin to your host GPIO** unless you
> read [Hardware Setup](04_hardware.md#nrst-hazard) first
> and accept the brick risk. Use `dev.reset()` instead.

## Step 3 — First sketch

Create a new sketch (`File → New`) and paste:

```cpp
#include <Wire.h>
#include <RbAmp.h>

RbAmp dev(Wire, 0x50);    // Wire bus, default slave address 0x50

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { /* wait for USB-CDC on native ports */ }

    Wire.begin();
    // ESP32 only — uncomment for SPEC §B.5 compliance:
    // Wire.setClock(50000);

    dev.setLogStream(&Serial);   // prints library diagnostic messages

    Serial.println(F("Probing rbAmp..."));
    while (!dev.begin()) {
        Serial.print(F("begin() failed: "));
        Serial.println(RbAmp::errorString(dev.lastError()));
        delay(1000);
    }
    Serial.print(F("OK — firmware 0x"));
    Serial.print(dev.firmwareVersion(), HEX);
    Serial.print(F(", channels="));
    Serial.println(dev.channels());
}

void loop() {
    Serial.print(F("U=")); Serial.print(dev.readVoltage(), 1);
    Serial.print(F("V  P=")); Serial.print(dev.readPower(0), 1);
    Serial.print(F("W  PF=")); Serial.println(dev.readPowerFactor(0), 3);
    delay(1000);
}
```

Upload it. Open the Serial Monitor at **115200** baud. You should see:

```
Probing rbAmp...
OK — firmware 0x1, channels=3
U=230.4V  P=+48.2W  PF=+0.972
U=230.1V  P=+48.1W  PF=+0.972
...
```

The `channels=3` reading is the constructor's default `ThreePhase` hint —
override it if you know your SKU is UI1 / UI2:

```cpp
RbAmp dev(Wire, 0x50, RbAmpTopology::Single);     // for UI1 / I1 SKUs
RbAmp dev(Wire, 0x50, RbAmpTopology::SplitPhase); // for UI2 / I2 SKUs
```

Over-detection (3 hint on a UI1 module) is harmless — unused channels
read `0.0 A` and contribute `0 Wh`.

## Step 4 — Add energy metering

Modify `loop()` to use period metering + the built-in Wh accumulator:

```cpp
void loop() {
    delay(60000);   // one minute period

    RbAmpPeriodSnapshot snap;
    if (!dev.readPeriodSnapshot(snap)) {
        Serial.print(F("snapshot: "));
        Serial.println(RbAmp::errorString(dev.lastError()));
        return;
    }

    Serial.print(F("avg_P=")); Serial.print(snap.avg_p[0], 2);
    Serial.print(F("W  Wh=")); Serial.print(dev.energy().wh(0), 4);
    Serial.print(F("  dt=")); Serial.print(snap.master_dt_ms); Serial.println(F("ms"));
}
```

The `readPeriodSnapshot()` call:

1. Writes `CMD_LATCH_PERIOD` (closes the current measurement window).
2. Waits 50 ms (`SPEC §7` mandate for the firmware to finalize the snapshot).
3. Checks `REG_V03_PERIOD_VALID` — returns `false` with `lastError() == RB_ERR_STALE`
   if the snapshot was stale.
4. Reads `avg_p[0..channels-1]` + `max_p` + `latch_ms`.
5. Computes `master_dt_ms` from `millis()` since the previous successful
   snapshot.
6. Calls `energy().tick()` internally — `dev.energy().wh(0)` now reflects
   the running total.

Expected output (one line per minute, ~50 W load):

```
avg_P=50.18W  Wh=0.8364  dt=60012ms
avg_P=50.21W  Wh=1.6735  dt=60005ms
avg_P=50.19W  Wh=2.5103  dt=60008ms
...
```

The `master_dt_ms` is the master's wall-clock interval between successful
latches — authoritative for energy integration. The chip's `latch_ms`
is diagnostic only (it can drift up to 20 % on PY32's uncalibrated LSI
clock).

## Step 5 — Configure the CT model

If your CT clamp is not the default SCT-013-30A, set the right model and
persist to flash:

```cpp
void setup() {
    // ...as above...
    while (!dev.begin()) { delay(1000); }

    // Set CT model — 1=5A, 2=10A, 3=30A (default), 4=50A, 5=100A.
    if (!dev.setCTModel(1)) {   // example: SCT-013-005
        Serial.print(F("setCTModel: "));
        Serial.println(RbAmp::errorString(dev.lastError()));
    }
    // Blocks ~700 ms for the flash erase + write. Done.
}
```

On **v1.1 firmware** this also triggers preset NF + GAIN auto-load.
On v1.0 firmware, you'll additionally need to tune NF + GAIN per
[Sensor Selection](03_sensor_selection.md#calibration-procedure-mandatory).

## What you've learned

- The library wraps every SPEC-mandated protocol detail (50 ms settle,
  `PERIOD_VALID` check, retry, sanity filter, Wh integration) so your
  sketch doesn't deal with them.
- `dev.readPeriodSnapshot(snap)` is the recommended entry point for any
  energy-metering use case.
- `dev.energy().wh(ch)` returns the running per-channel Wh total —
  updated automatically on every successful snapshot.
- `dev.setCTModel(code)` configures the CT clamp once at install time.

## Next steps

- [Examples](06_examples.md) — 10 complete scenario walkthroughs
- [DIY Integrations](07_diy_integrations.md) — Home Assistant, Node-RED, OpenHAB
- [Cloud Integrations](08_cloud_integrations.md) — AWS IoT, Azure, GCP, InfluxDB
- [Troubleshooting](10_troubleshooting.md) — when readings don't make sense

## Related — main rbAmp documentation

- [API Reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference) — formal I²C register / command / error spec the library wraps
- [Arduino Examples (raw I²C)](https://www.rbamp.com/docs/modules-basic-standard-arduino-examples) — same scenarios without the library, useful for porting
- [Period Metering](https://www.rbamp.com/docs/modules-basic-standard-period-metering) — atomic latch concept and master-side energy formula
- [Hardware Connection](https://www.rbamp.com/docs/modules-basic-standard-hardware-connection) — pinout, wiring, CT installation
- [Troubleshooting](https://www.rbamp.com/docs/modules-basic-standard-troubleshooting) — module-side issues (NACK, calibration drift, bus noise)


---

[← Hardware Setup](04_hardware.md) | [Contents](README.md) | [Examples →](06_examples.md)
