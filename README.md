# RbAmp — Arduino library for rbAmp modules

[![protocol: 1.2](https://img.shields.io/badge/protocol-1.2-blue)](docs/02_tiers.md)
[![arduino: AVR · ESP32 · ESP8266 · STM32](https://img.shields.io/badge/arduino-AVR%20%C2%B7%20ESP32%20%C2%B7%20ESP8266%20%C2%B7%20STM32-brightgreen)](docs/04_hardware.md)
[![license: MIT](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)

> 🌐 [Русская версия / Russian version](README.ru.md)

`RbAmp` is an Arduino library for **rbAmp** modules — compact hardware meters for AC current and voltage with an I²C interface. The module is built around a Cortex-M0+ microcontroller with an on-board isolated analog front-end and factory calibration.

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

The module connects to the master with four wires: `VCC`, `GND`, `SDA`, `SCL`. Optionally a fifth — `DRDY` (data-ready interrupts every ~200 ms).

| Pin | Level |
|---|---|
| `VCC` | **5 V (4.5..5.5 V)** — on-board regulator and noise filtering |
| `GND` | common with the master (mandatory) |
| `SDA`, `SCL` | 3.3 V logic, **5 V-tolerant** — works with both 3.3 V masters (ESP32) and 5 V masters (Arduino UNO/Nano) |
| `DRDY` | open-drain, 3.3 V level, ~10 µs LOW pulse every ~200 ms |

The board has **built-in 4.7 kΩ pull-ups to 3.3 V** on SDA and SCL — a single-module setup needs no external pull-ups. On a multi-module bus, cut the `Pull-Up` jumper to disable them (see [04_hardware.md](docs/04_hardware.md)).

The default I²C address is `0x50` (7-bit); the bus runs at 100 kHz (Standard mode) or 400 kHz (Fast mode).

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
lib_deps   = rbamp/RbAmp@^1.0.0
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

That gives every module in the system the same time base (see [04_period_metering.md](https://github.com/rb-amp/rbamp-spec/blob/main/docs/04_period_metering.md) in the canonical spec), with no need to reconcile internal clocks across devices.

What else:

- **`RbAmp` class** — one instance per module on the bus. Named methods for every quantity: `readVoltage()`, `readPower(ch)`, `readPowerFactor(ch)`, `readFrequency()`, `readPeriodSnapshot(&snap)`, `setSensorClass(class)`, `setCTModel(code)`, and so on.
- **Current-sensor configuration** — two calls: `setSensorClass(class)` picks the sensor family (SCT-013 / built-in CT / wired CT), `setCTModel(code)` picks the model within that family. Calibration coefficients load automatically from the factory preset table.
- **Per-channel Wh accumulator** — `dev.energy().wh(ch)` returns the current value accumulated by the library. Updated automatically after each successful `readPeriodSnapshot()`. Behaviour depends on the module's tier — see [02_tiers.md](docs/02_tiers.md).
- **POD structures** `RbAmpSnapshot` / `RbAmpPeriodSnapshot` — every field of one snapshot in one struct.
- **Protocol details hidden** — byte order, settle times after commands, ready-flag polling — all handled inside. User code calls methods rather than writing to registers.

## Documentation

| Document | Purpose |
|---|---|
| [01 · Overview](docs/01_overview.md) | what rbAmp is, what the library does, comparison with raw-register access |
| [02 · Module tiers](docs/02_tiers.md) | which tier (BASIC / STANDARD / PRO) fits which use case |
| [03 · Current-sensor selection](docs/03_sensor_selection.md) | how to pick an SCT-013 (5A / 10A / 30A / 50A / 100A) and tell the module via `setCTModel()` |
| [04 · Wiring](docs/04_hardware.md) | pinout, schematic for various Arduino hosts |
| [05 · Quickstart](docs/05_quickstart.md) | first working sketch in 5 minutes |
| [06 · Examples](docs/06_examples.md) | walkthrough of the sketches in `examples/` |
| [07 · DIY integrations](docs/07_diy_integrations.md) | Home Assistant / Node-RED / OpenHAB |
| [08 · Cloud integrations](docs/08_cloud_integrations.md) | AWS IoT / Azure / GCP / InfluxDB |
| [09 · API reference](docs/09_api_reference.md) | full public library API |
| [10 · Troubleshooting](docs/10_troubleshooting.md) | common problems and how to work through them |
| [11 · Changelog](docs/11_changelog.md) | library release history |

The wire-level protocol description (shared by all client libraries) lives in the [`rbamp-spec`](https://github.com/rb-amp/rbamp-spec) repository.

## Examples

Ready sketches in [`examples/`](examples/):

1. [`01_QuickRead`](examples/01_QuickRead/) — simple U / I / P / PF read once per second
2. [`02_PeriodEnergyOLED`](examples/02_PeriodEnergyOLED/) — energy counter on a 128×64 OLED
3. [`03_MultiModuleBroadcast`](examples/03_MultiModuleBroadcast/) — three modules on one bus, synchronised periods
4. [`04_UI3PerChannelMQTT`](examples/04_UI3PerChannelMQTT/) — a UI3 module publishing per-channel data over MQTT
5. [`06_BidirectionalEnergy`](examples/06_BidirectionalEnergy/) — separate consumption and export accounting (master-side)
6. [`07_DeepSleepLogger`](examples/07_DeepSleepLogger/) — battery-powered deep-sleep logger

Each sketch is walked through in detail in [docs/en/06_examples.md](docs/06_examples.md).

## Compatibility

The library works with module firmware **v1.0..v1.2**:

| Firmware | REG_VERSION | New in this version |
|---|---|---|
| v1.0 | `0x01` | baseline publish |
| v1.1 | `0x02` | `REG_TOPOLOGY` (0x24) — channel-count autodetect |
| **v1.2** | **`0x03`** | `REG_SENSOR_CLASS` (0x25), 10 kHz sample rate |

All library versions work with all firmware versions. Registers absent on an older firmware return `0x00` for byte reads and `0.0f` for float reads. For example, a v1.2 library reading `REG_SENSOR_CLASS` (0x25) on v1.0 firmware gets `0x00 = UNSET` and automatically falls back to the constructor-hint path (`setSensorClass(SCT_013)` default, or the topology hint from the constructor).

## Sister libraries

The rbAmp module wire protocol is implemented by a family of cross-platform client libraries. They all read the same registers, run the same period-metering protocol, and produce numerically equivalent results — pick whichever matches your runtime.

| Library | Use when… | Repository |
|---|---|---|
| **Arduino** *(this library)* | Arduino IDE / PlatformIO on AVR, ESP32, ESP8266, STM32duino | [`rbamp-arduino`](https://github.com/rb-amp/rbamp-arduino) |
| **ESP-IDF component** | ESP-IDF 5.x native C with FreeRTOS, esp-mqtt, deep sleep | [`rbamp-esp-idf`](https://github.com/rb-amp/rbamp-esp-idf) |
| **Python package** (CPython + MicroPython) | Raspberry Pi / Linux SBC over `smbus2`, OR MicroPython on ESP32 / RP2040 / STM32 | [`rbamp-python`](https://github.com/rb-amp/rbamp-python) |
| **ESPHome external component** | Declarative YAML integration with Home Assistant | [`rbamp-esphome`](https://github.com/rb-amp/rbamp-esphome) |
| **STM32 HAL** | Bare HAL on STM32F1/F4/G0/G4 — no Arduino runtime, no RTOS | [`rbamp-stm32-hal`](https://github.com/rb-amp/rbamp-stm32-hal) *(coming after bench break-in + v1.2 firmware extension)* |

For the wire protocol itself (registers, commands, errors, NACK discipline), see [`rbamp-spec`](https://github.com/rb-amp/rbamp-spec).

## License

MIT — see [LICENSE](LICENSE).
