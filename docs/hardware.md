# Hardware — pinout and wiring per host MCU

This document covers the **host MCU side** (Arduino, ESP32, ESP8266, STM32,
etc.) wiring to an rbAmp / PY32-DimmerLink module. The module's own pinout
(CT input, mains terminals, NRST, DAPLink connector) is documented in the
[the rbAmp protocol spec](https://www.rbamp.com/docs/modules-basic-standard-api-reference) repository.

> ⚠ **Mains safety** — the rbAmp module sits at mains potential on its
> primary side. Install behind a circuit breaker, never work on a live
> board, and double-check the optical isolation of the I2C side before
> connecting your host MCU. The CT clamp is isolated by construction.

## rbAmp module — connector pinout (I2C side)

The I2C-side connector on the module exposes:

| Pin | Signal | Notes |
|---|---|---|
| 1 | `3V3` | 3.3 V regulator output (~15 mA quiescent + RT spikes) |
| 2 | `GND` | Common with host MCU |
| 3 | `SDA` | I2C data — open-drain, needs pull-up (typically 4.7 kΩ to 3V3) |
| 4 | `SCL` | I2C clock — open-drain, needs pull-up |
| 5 | `DRDY` | (optional) data-ready strobe — falling edge every ~200 ms when RT window commits |
| 6 | `NRST` | (optional, ⚠ see below) module reset, active-low |

The pull-ups for `SDA` / `SCL` may be on the module or on the host board
— check your variant's schematic to avoid double-pulling (4.7 kΩ // 4.7 kΩ
= 2.35 kΩ, may be too aggressive for slow-rise boards).

### `NRST` hazard

**Do not drive `NRST` from your host MCU's GPIO during boot.** Per
[the rbAmp protocol spec](https://www.rbamp.com/docs/modules-basic-standard-api-reference),
repeated NRST glitches during host re-flash cycles can land the PY32
slave in a state recoverable only via DAPLink + Keil. If your sketch
genuinely needs to drive `NRST`:

```cpp
void setup() {
    // 1. Bring up I2C FIRST so the slave is alive and pull-ups settle.
    Wire.begin();
    delay(50);

    // 2. Only THEN configure the NRST GPIO. Avoid OUTPUT_OPEN_DRAIN until you actually need to reset.
    pinMode(NRST_PIN, INPUT);   // tri-state, slave's own pull-up keeps it HIGH

    // 3. If you need a clean reset, do it deliberately, once:
    pinMode(NRST_PIN, OUTPUT);
    digitalWrite(NRST_PIN, LOW);
    delay(2);
    pinMode(NRST_PIN, INPUT);   // release immediately, let internal pull-up restore HIGH
    delay(100);                 // slave boot
}
```

**Better alternative**: don't wire `NRST` to the host at all. Use the
library's `dev.reset()` (issues `CMD_RESET` via I2C, takes 100 ms) — gets
to the same reset handler without the GPIO glitch surface.

## Host MCU wiring

### Arduino Uno / Mega / Nano (AVR ATmega328 / ATmega2560)

| rbAmp pin | Arduino pin | Notes |
|---|---|---|
| 3V3 | 3.3V | ⚠ Uno's regulator only sources ~50 mA — adequate for one rbAmp; multi-module installations should use external supply |
| GND | GND | |
| SDA | A4 (Uno) / 20 (Mega) | |
| SCL | A5 (Uno) / 21 (Mega) | |
| DRDY | any digital pin (e.g. D2 for `attachInterrupt`) | optional |

```cpp
Wire.begin();   // SDA=A4, SCL=A5 on Uno (board-determined)
// Optional:
// Wire.setClock(400000);
```

**AVR limitations** (Uno / Mega):

- Energy accumulator uses 32-bit `float` (AVR FPU absence — `double == float`
  on this toolchain). Drift accumulates over multi-week runs at 60 s
  cadence. Reset periodically (e.g. weekly via your persistence store).
- Free RAM is tight (2 kB on Uno) — `RbAmp` + `RbAmpEnergy` + one
  `RbAmpPeriodSnapshot` consume ~140 bytes; manageable but leaves no
  headroom for big strings. Use `F()` macro for all literal strings.
- `Wire.setClock(400000)` works but only on the ATmega328's "TWI" — some
  shield combinations show signal-integrity issues. 100 kHz is reliable.

### ESP32 / ESP32-S2 / ESP32-S3 / ESP32-C3 (arduino-esp32 v3.x)

| rbAmp pin | ESP32 pin (default) | Notes |
|---|---|---|
| 3V3 | 3V3 | Plenty of current available |
| GND | GND | |
| SDA | GPIO21 | configurable via `Wire.begin(sda, scl)` |
| SCL | GPIO22 | |
| DRDY | any GPIO (e.g. GPIO15 for `attachInterrupt`) | optional |

```cpp
Wire.begin(21, 22);              // explicit pins recommended on ESP32
Wire.setClock(50000);            // ⚠ SPEC §B.5 mandates 50 kHz on ESP32 + PY32 v1.0
```

**SPEC §B.5 on ESP32**: the arduino-esp32 v3.x core wraps the ESP-IDF v5
`i2c_master` driver, which shares the **~20 % NACK rate** behaviour against
the PY32 slave at 100 kHz. The library defaults
`RBAMP_NACK_RETRY_ATTEMPTS = 3` on ESP32 targets to absorb this. Combined
with 50 kHz bus speed, residual error rate drops to < 0.8 %.

For **dense-read workloads** (≥10 single-byte reads per cycle — e.g. full
UI3 RT block + period snapshot), bump retry to 5 BEFORE the include:

```cpp
#define RBAMP_NACK_RETRY_ATTEMPTS 5
#include <RbAmp.h>
```

See [Troubleshooting](troubleshooting.md) for the full
NACK + retry diagnostic flow.

### ESP8266 (NodeMCU / Wemos D1 mini)

| rbAmp pin | NodeMCU pin | GPIO | Notes |
|---|---|---|---|
| 3V3 | 3V3 | — | ESP8266 has plenty |
| GND | G | — | |
| SDA | D2 | GPIO4 | default I2C SDA on `Wire` |
| SCL | D1 | GPIO5 | default I2C SCL on `Wire` |
| DRDY | D5 (or any) | GPIO14 | optional |

```cpp
Wire.begin(D2, D1);              // SDA, SCL
Wire.setClock(100000);           // ESP8266 not affected by the SPEC §B.5 NACK pattern
```

**ESP8266 specifics**:

- The library's retry default is **1 attempt** on ESP8266 (no `ESP_PLATFORM`
  macro means the `RBAMP_NACK_RETRY_ATTEMPTS` auto-default picks 1).
- ESP8266 single-core — watch out for WDT timeouts during long settle
  delays (`setCTModel()` blocks 700 ms; that's safe but `factoryReset()`
  blocks 1500 ms which is at the edge of the default WDT).

### STM32duino (Blue Pill F103 / Black Pill F411 / Nucleo)

| rbAmp pin | Blue Pill F103 pin | Notes |
|---|---|---|
| 3V3 | 3.3V | F103 regulator handles ~100 mA |
| GND | GND | |
| SDA | PB7 (I2C1_SDA) | board-default on `Wire` |
| SCL | PB6 (I2C1_SCL) | |
| DRDY | any GPIO (e.g. PA0 for `attachInterrupt`) | optional |

```cpp
Wire.begin();                    // uses board-default I2C1 pins
Wire.setClock(100000);
```

**STM32duino specifics**:

- HAL-backed `Wire` implementation, no NACK pattern with PY32 — single
  retry default is fine.
- For F411 Black Pill, double-check your variant — some pin out I2C1 on
  PB8/PB9 instead of PB7/PB6.

### SAMD / RP2040 (arduino-pico)

| rbAmp pin | RP2040 default | Notes |
|---|---|---|
| 3V3 | 3V3(OUT) | |
| GND | GND | |
| SDA | GPIO4 (Wire0) or GPIO2 (Wire1) | configurable |
| SCL | GPIO5 (Wire0) or GPIO3 (Wire1) | configurable |

```cpp
Wire.setSDA(4);
Wire.setSCL(5);
Wire.begin();
Wire.setClock(100000);
```

**arduino-pico specifics**: well-tested I2C, no NACK pattern issue. If you
need an alternative high-level API on RP2040, the rbAmp Arduino library
through arduino-pico is the only path in v1 (no native Pico SDK library).

## Multi-module installations

Three rbAmp modules on one I2C bus is the typical home-balance topology
(mains / solar / loads). Each module needs a unique I2C address — use
the [`05_AddressChange`](../examples/05_AddressChange/) sketch once per
module during installation.

### Bus loading

- Each rbAmp module presents ~10 pF + the pull-up. Three modules on one bus
  → ~30 pF + one pull-up pair (if pull-ups are NOT on the modules).
- At 100 kHz, the I2C spec allows up to 400 pF — three modules + ~1 m of
  cable runs is comfortable.
- At 400 kHz, the budget is 100 pF — use shorter cabling (< 30 cm) or
  drop to 100 kHz.

### Pull-up sizing

If the modules don't have onboard pull-ups (check schematic), add one pair
at the host MCU end:

| Bus speed | Pull-up to 3.3V |
|---|---|
| 100 kHz | 4.7 kΩ |
| 400 kHz | 2.2 kΩ |

Stronger pull-ups for faster speeds — the bus capacitance × resistance
determines the rise time, and the I2C spec needs < 300 ns at 400 kHz.

### Cable routing

- Twisted pair for SDA + SCL (or SDA + GND, SCL + GND for two separate
  pairs) keeps capacitive coupling manageable.
- Keep cabling away from mains AC wiring — even with optical isolation, EMI
  pickup can manifest as ADC noise on the analog side of the module.
- For runs > 1 m, consider an I2C bus extender (e.g. PCA9600 / PCA9617)
  rather than just bigger pull-ups.

## DRDY / interrupt-driven reads

The rbAmp module asserts `DRDY` (active LOW, open-drain) on every RT window
commit (every ~200 ms). Use an interrupt to wake the host MCU only when
fresh data is available — useful for the master-side bidirectional accounting
pattern ([scenario 5 in 06_examples.md](examples.md#scenario-5--master-side-bidirectional-accounting)):

```cpp
const int PIN_DRDY = 15;
volatile bool data_ready = false;

void IRAM_ATTR on_drdy() { data_ready = true; }   // ESP32 — keep ISR tiny

void setup() {
    Wire.begin(21, 22);
    Wire.setClock(50000);
    delay(300);

    // Wait for the slave to come online BEFORE arming the ISR — a settling
    // pull-up edge can otherwise fire a spurious interrupt before the
    // first valid window.
    while (!dev.begin()) delay(500);
    pinMode(PIN_DRDY, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_DRDY), on_drdy, FALLING);
}

void loop() {
    if (!data_ready) return;       // sleep until next RT window
    data_ready = false;
    float p = dev.readPower(0);
    // ...integrate...
}
```

DRDY is **optional** — polling at any rate ≤ 5 Hz works fine without it.
The library doesn't depend on DRDY in any code path.

## Reference

- [the rbAmp protocol spec](https://www.rbamp.com/docs/modules-basic-standard-api-reference) — module schematic, isolation, NRST hazard
- [Quickstart](quickstart.md) — minimal hello-world wiring + sketch
- [Examples](examples.md#scenario-5--master-side-bidirectional-accounting) — DRDY-driven example
- [Troubleshooting](troubleshooting.md) — bus-level debugging recipes

## Related — main rbAmp documentation

- [API Reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference) — formal I²C register / command / error spec the library wraps
- [Arduino Examples (raw I²C)](https://www.rbamp.com/docs/modules-basic-standard-arduino-examples) — same scenarios without the library, useful for porting
- [Period Metering](https://www.rbamp.com/docs/modules-basic-standard-period-metering) — atomic latch concept and master-side energy formula
- [Hardware Connection](https://www.rbamp.com/docs/modules-basic-standard-hardware-connection) — pinout, wiring, CT installation
- [Troubleshooting](https://www.rbamp.com/docs/modules-basic-standard-troubleshooting) — module-side issues (NACK, calibration drift, bus noise)


---

[← Sensor Selection](sensor-selection.md) | [Contents](README.md) | [Quickstart →](quickstart.md)
