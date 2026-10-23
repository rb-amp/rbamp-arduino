# RbAmp — Arduino library for rbAmp modules

[![protocol: 1.3](https://img.shields.io/badge/protocol-1.3-blue)](docs/02_tiers.md)
[![arduino: AVR · ESP32 · ESP8266 · STM32](https://img.shields.io/badge/arduino-AVR%20%C2%B7%20ESP32%20%C2%B7%20ESP8266%20%C2%B7%20STM32-brightgreen)](docs/04_hardware.md)
[![license: MIT](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)


`RbAmp` is an Arduino library for **rbAmp** modules — compact hardware meters for AC current and voltage with an I²C interface. The module has an on-board isolated analog front-end and factory calibration.

> **Mains voltage can kill.** Disconnect and lock out the circuit before connecting the L/N terminals or fitting the clamp; have a qualified person do mains wiring and any work inside a panel. Clamp ONE conductor only (L) — never the whole cable (L+N cancel to ~0). Fuse the L tap per local rules.

From an integrator's point of view, rbAmp behaves like any other I²C slave device: power it up, read registers, get values in physical units (volts, amperes, watts). No signal processing on the master side is required.

The same API is available on other platforms (ESP-IDF, MicroPython, CPython, STM32 HAL) — switching between them does not require relearning the surface.

```cpp
#include <Wire.h>
#include <RbAmp.h>

RbAmp dev(Wire, 0x50);

void setup() {
    Serial.begin(115200);
    Wire.begin();
    while (!dev.begin()) { delay(1000); }
}

void loop() {
    RbAmpPeriodSnapshot snap;
    if (dev.readPeriodSnapshot(snap)) {
        Serial.print(F("P=")); Serial.print(snap.avg_p[0]); Serial.print(F(" W  "));
        Serial.print(F("Wh=")); Serial.println(dev.energy().wh(0), 4);
    }
    delay(60000);
}
```

## Wiring

The module connects to the master with four wires: `VCC`, `GND`, `SDA`, `SCL`. Optionally a fifth — `DRDY` (data-ready interrupts once per measurement window, every 100 ms).

| Pin | Level |
|---|---|
| `VCC` | **5 V (4.5..5.5 V)** — on-board regulator and noise filtering |
| `GND` | common with the master (mandatory) |
| `SDA`, `SCL` | 3.3 V logic, **5 V-tolerant** — works with both 3.3 V masters (ESP32) and 5 V masters (Arduino UNO/Nano) |
| `DRDY` | open-drain, 3.3 V level, ~10 µs LOW pulse every 100 ms (one per measurement window; STD images up to 0x7B: every 200 ms) |

The board has **built-in 4.7 kΩ pull-ups to 3.3 V** on SDA and SCL — a single-module setup needs no external pull-ups. On a multi-module bus, cut the `Pull-Up` jumper to disable them (see [04_hardware.md](docs/04_hardware.md)).

The default I²C address is `0x50` (7-bit). Bus speed: UNO / Mega / STM32 / RP2040 100 kHz; ESP32 / ESP8266 50 kHz. Do not use 400 kHz.

## Installation

### Arduino IDE — Library Manager

`Sketch → Include Library → Manage Libraries…` → search for **RbAmp** → **Install**. No dependencies. Examples appear under `File → Examples → RbAmp` after installation.

### Arduino CLI

```sh
arduino-cli lib install RbAmp
```

### PlatformIO

```ini
[env:esp32dev]
platform   = espressif32
framework  = arduino
lib_deps   = rbamp/RbAmp@^1.6.1
```

### Manual

```sh
git clone https://github.com/rb-amp/rbamp-arduino.git \
    "$HOME/Arduino/libraries/RbAmp"
```

## Supported platforms

| Core | Status | Notes |
|---|---|---|
| Arduino AVR (Uno / Mega / Nano) | working | Wh accumulator on 32-bit float (see below) |
| arduino-esp32 (ESP32 / S2 / S3 / C3) | working | |
| arduino-esp8266 | working | |
| STM32duino (F1 / F4 / G4) | working | |
| SAMD / RP2040 (arduino-pico) | should work | tested in limited scope |

## What the library gives you

The module returns **only instantaneous and period-averaged quantities** — voltage, current, power in watts. Energy accumulation in Wh is done by the library itself using the master's clock:

```text
E_Wh += PERIOD_AVG_P_W × master_dt_seconds / 3600
```

That gives every module in the system the same time base (see [Period metering](https://www.rbamp.com/docs/modules-basic-standard-period-metering) in the module documentation), with no need to reconcile internal clocks across devices. The library integrates against the **master's wall-clock**, never the module's diagnostic period timer; on a missed/stale period the integration anchor is held so the next valid window is not under-counted.

What else:

- **Read values** — `readVoltage()`, `readCurrent(ch)`, `readPower(ch)`, `readPowerFactor(ch)`, `readFrequency()`, or everything at once with `readAll(snap)`. A value that fails a plausibility check comes back as `NaN`; the rest of the read stays usable.
- **Energy** — `readPeriodSnapshot(snap)` once per period, then `dev.energy().wh(ch)` gives the Wh total kept by the library.
- **Several modules** — `RbAmpFleet` scans the bus, polls every module in one call and adds up the totals (see [06 · Examples](docs/06_examples.md)).
- **STANDARD (STD) modules** — the same calls work; `begin()` detects the module line, and the extra STD calls are listed in [02 · Module tiers](docs/02_tiers.md) and [09 · API reference](docs/09_api_reference.md#std-line-api).
- **Everything else** — sensor setup, DRDY line, identity, error handling: [09 · API reference](docs/09_api_reference.md).

**Words used in these docs**

| Word | Meaning |
|---|---|
| Period | The time between two energy readings that you take (for example 60 s). |
| Snapshot | A command that makes the module freeze its running average for the period so you can read it. |
| Window | One 100 ms measurement slot of the module; real-time values refresh once per window. |
| BASIC / STD | The two module lines. Check the label on your module; `dev.line()` prints it after `begin()`. |
| DRDY | Optional "data ready" output pin; it pulses LOW when fresh values are published. |
| Image version | The version of the module's internal firmware image, read with `stdReadImageVersion()` (STD modules only). |


> **Robustness on a multi-module / marginal bus (ESP32).** Arduino-ESP32's `Wire` wraps a driver that can block on a held bus *below* the library, which a software timeout cannot interrupt. Use proper external **~4.7 kΩ pull-ups**, keep a debugger/NRST off the bus in production, and arm an **app-level task watchdog** on your polling task as the recovery path. This is the same three-layer posture validated in extended soak on the bench.

## Documentation

| Document | Purpose |
|---|---|
| [01 · Overview](docs/01_overview.md) | what rbAmp is, what the library does, comparison with raw-register access |
| [02 · Module tiers](docs/02_tiers.md) | which module line (BASIC / STANDARD (STD)) fits which use case |
| [03 · Current-sensor selection](docs/03_sensor_selection.md) | how to pick an SCT-013 (5A / 10A / 30A / 50A / 100A) and tell the module via `setCTModel()` |
| [04 · Wiring](docs/04_hardware.md) | pinout, schematic for various Arduino hosts |
| [05 · Quickstart](docs/05_quickstart.md) | first working sketch in 5 minutes |
| [06 · Examples](docs/06_examples.md) | walkthrough of the sketches in `examples/` |
| [07 · DIY integrations](docs/07_diy_integrations.md) | Home Assistant / Node-RED / OpenHAB |
| [08 · Cloud integrations](docs/08_cloud_integrations.md) | AWS IoT / Azure / GCP / InfluxDB |
| [09 · API reference](docs/09_api_reference.md) | full public library API |
| [10 · Troubleshooting](docs/10_troubleshooting.md) | common problems and how to work through them |
| [11 · Changelog](docs/11_changelog.md) | library release history |

The wire-level protocol description (shared by all client libraries) is documented in the [`rbamp-docs`](https://github.com/rb-amp/rbamp-docs) repository.

## Examples

Ready sketches in [`examples/`](examples/):

1. [`01_QuickRead`](examples/01_QuickRead/) — simple U / I / P / PF read once per second
2. [`02_PeriodEnergyOLED`](examples/02_PeriodEnergyOLED/) — energy counter on a 128×64 OLED
3. [`03_MultiModuleBroadcast`](examples/03_MultiModuleBroadcast/) — three modules on one bus, synchronised periods
4. [`04_UI3PerChannelMQTT`](examples/04_UI3PerChannelMQTT/) — a UI3 module publishing per-channel data over MQTT
5. [`05_AddressChange`](examples/05_AddressChange/) — reassign a module's I2C address (two-phase commit)
6. [`06_BidirectionalEnergy`](examples/06_BidirectionalEnergy/) — separate consumption and export accounting (master-side)
7. [`07_DeepSleepLogger`](examples/07_DeepSleepLogger/) — battery-powered deep-sleep logger
8. [`08_FleetSync`](examples/08_FleetSync/) — scan a fleet, General-Call period sync, fleet-wide aggregation
9. [`09_SeniorSku`](examples/09_SeniorSku/) — 5- and 7-channel modules through the same per-channel calls
10. [`10_PerChannelModels`](examples/10_PerChannelModels/) — a different current sensor on every channel
11. [`11_StdLine`](examples/11_StdLine/) — one sketch for both product lines; STD extras (Q / S, health, snapshot banks)

Each sketch is walked through in detail in [docs/06_examples.md](docs/06_examples.md). A bench-only validation harness lives in [`extras/`](extras/) (not a Library-Manager example).

## Compatibility

The library works with module firmware **v1.0..v1.3**:

| Firmware | REG_VERSION | New in this version |
|---|---|---|
| v1.0 | `0x01` | baseline publish |
| v1.1 | `0x02` | `REG_TOPOLOGY` (0x24) — channel-count autodetect |
| v1.2 | `0x03` | `REG_SENSOR_CLASS` (0x25) |
| **v1.3** | **`0x04`** | `REG_HW_VARIANT` / `REG_CAPABILITY` / UID, per-channel CT, General-Call fleet sync, two-phase address commit, event channel |

**STD line modules** (the 16-bit register model, map version `0x30` and later) are supported from library **1.6.0**: `begin()` detects the line automatically, so the same sketch serves both lines. Firmware update over I2C needs map `0x33+`; synchronous command verdicts need `0x34+` (on older STD maps a rejected write is not reported). Micro-resolution energy, the starting current, voltage angles and the phase sequence (library 1.6.1) need an image that publishes them (`stdReadImageVersion()` ≥ `0x0001007B`) — on older images the library reports them as derived / not available.

All library versions work with all firmware versions. Registers absent on an older firmware return `0x00` for byte reads and `0.0f` for float reads — the library falls back accordingly (e.g. on pre-v1.3 firmware `REG_HW_VARIANT` reads `0x00` and variant detection uses the constructor topology hint). The fleet General-Call sync requires v1.3 firmware; on older firmware fall back to per-device sequential `latchPeriod()` (see `03_MultiModuleBroadcast`).

## Sister libraries

The rbAmp module wire protocol is implemented by a family of cross-platform client libraries. They all read the same registers, run the same period-metering protocol, and produce numerically equivalent results — pick whichever matches your runtime.

| Library | Use when… | Repository |
|---|---|---|
| **Arduino** *(this library)* | Arduino IDE / PlatformIO on AVR, ESP32, ESP8266, STM32duino | [`rbamp-arduino`](https://github.com/rb-amp/rbamp-arduino) |
| **ESP-IDF component** | ESP-IDF 5.x native C with FreeRTOS, esp-mqtt, deep sleep | [`rbamp-esp-idf`](https://github.com/rb-amp/rbamp-esp-idf) |
| **Python package** (CPython + MicroPython) | Raspberry Pi / Linux SBC over `smbus2`, OR MicroPython on ESP32 / RP2040 / STM32 | [`rbamp-python`](https://github.com/rb-amp/rbamp-python) |
| **ESPHome external component** | Declarative YAML integration with Home Assistant | [`rbamp-esphome`](https://github.com/rb-amp/rbamp-esphome) |
| **STM32 HAL** | Bare HAL on STM32F1/F4/G0/G4 — no Arduino runtime, no RTOS | STM32 HAL examples are in [`rbamp-docs`](https://github.com/rb-amp/rbamp-docs) *(no separate library)* |

For the wire protocol itself (registers, commands, errors, NACK discipline), see [`rbamp-docs`](https://github.com/rb-amp/rbamp-docs).

## License

MIT — see [LICENSE](LICENSE).
