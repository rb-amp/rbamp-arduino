# 04 · Wiring

This chapter covers the physical connection of the **rbAmp** module to an
Arduino-compatible host: the LV-side pinout, power
requirements, I²C level compatibility, bus length, and wiring diagrams
for specific Arduino boards (UNO, Mega, ESP32, ESP8266, STM32duino,
RP2040).

The module's mains side is already routed inside the enclosure and
galvanically isolated — you work only with the LV side (power +
I²C + optional `DRDY`).

## LV-side pinout

The module runs on 3.3 V I²C logic and is **5 V tolerant** on the
signal lines. All LV pins are fully galvanically isolated
from mains.

| Pin | Signal | Function |
|---|---|---|
| `VCC` | +5 V (4.5..5.5 V) | Module power; 4.5 V minimum for correct operation |
| `GND` | Ground | Shared with the master — **mandatory** |
| `SDA` | I²C SDA | I²C data |
| `SCL` | I²C SCL | I²C clock |
| `DRDY` | Data-ready (optional) | Open-drain, LOW pulse ~10 µs every ~200 ms |

## Power

### VCC parameters

- **Nominal voltage**: 5 V DC
- **Permissible range**: 4.5..5.5 V
- **Supply current**: ~15 mA typical, ~25 mA peak during flash writes
- **Ripple**: < 50 mV peak-to-peak (for ADC accuracy)

The module carries an on-board regulator, RC filter, ceramic capacitors, and a
ferrite bead — no additional filtering is required from the user.
The module can be powered from the `+5V` rail of any Arduino board
(UNO, ESP32 DevKitC, etc.).

### I²C level compatibility

- `SDA` / `SCL` operate at **3.3 V logic** (the built-in pull-ups
  pull the lines to 3.3 V).
- The lines are **5 V tolerant**: a 5 V master (Arduino UNO / Nano) reads
  HIGH (~3.3 V) correctly and can drive the bus without a level
  translator.
- For a bus running entirely at 5 V levels — disable the built-in pull-ups
  (see below) and add external pull-ups to +5 V on the
  master side.

### Power-on behavior

- Cold start: ~250 ms to the first valid result.
- After a reset (software or brown-out): ~250 ms to recover.
- Until the first valid result, registers read as `0` and
  `DATA_VALID` = 0.

The library waits for the module to be ready inside `dev.begin()` —
your code simply calls `begin()` and continues.

### Isolation

Inside the module there is a galvanic isolation barrier between the mains side
(CT clamp, voltage divider) and the LV side (I²C). Consequences:

- The module's `GND` is **not connected** to the mains line or neutral.
- Connecting the LV side to a 5 V master is safe — there is no risk of a short
  circuit through ground.

> ⚠ Do not open the module enclosure — this voids the factory calibration.

## On-board pull-up resistors

**Important**: every rbAmp module ships with **built-in 4.7 kΩ pull-up
resistors on SDA and SCL to 3.3 V**. For a **single module** on
the bus, external pull-ups are not required — connect the master
directly.

### When to disable the built-in pull-ups

If the bus has several rbAmp modules (or other I²C devices with
their own pull-ups), connecting them in parallel yields too low an
effective resistance, which:

- Increases bus consumption
- May exceed the maximum sink current of the I²C master's output
  stage (typically ≤ 3 mA on 3.3 V logic)
- Adds capacitive load on a long bus while doing little to improve
  noise immunity

Solution: on the bottom side of the board, near the pull-up resistors, there are
**jumper bridges** (silkscreen `Pull-Up`). Cut them with a sharp
blade to disable a pull-up:

- Leave the pull-ups enabled on **only one** module (or
  move them to a single point near the master).
- Cut the bridges on the remaining modules.
- If pull-ups are already present on the master board — cut the bridges
  on **all** rbAmp modules.

### Rule of thumb

| rbAmp modules on bus | Pull-ups |
|:---:|---|
| 1 | Leave as-is (the built-in 4.7 kΩ works) |
| 2 | Leave on one, cut on the other |
| 3+ | Cut on all modules, fit one external 2.2…4.7 kΩ pair near the master |

> **If in doubt**: measure the resistance between SDA and VCC with
> the devices unpowered. It should be in the **1.5..4.7 kΩ** range.
> Lower than that means too many active pull-ups in parallel.

## I²C bus parameters

- **Default address**: `0x50` (7-bit)
- **Address range**: `0x08..0x77` (reassigned via two-phase address commit, production-OK)
- **Recommended speed**: **50 kHz** on ESP32/ESP8266 (the host-side driver shows a ~20% NACK rate at 100 kHz; at 50 kHz + 3× retry it is < 0.8%). 100 kHz with retry is workable. 400 kHz is **not validated**.
- **Pointer auto-increment**: **on reads only**. A burst-read of `<addr> + N bytes` returns N consecutive bytes starting at `addr`. **Writes are NOT auto-increment**: a burst-write drops bytes 1..N and only the first lands. Multi-byte registers are written **byte by byte** in separate single-byte transactions. See [09 · API Reference](09_api_reference.md) — "Multi-byte writes".

### Bus length

I²C is a short bus (~30 cm by default). Validated topologies for
rbAmp:

| Cable | Max length | Speed |
|---|---|---|
| Standard JST / flat 4-conductor | up to 0.3 m | 100 kHz |
| Twisted pair UTP (cat-5/5e/6) — SDA+GND and SCL+GND in **separate pairs** | up to 1 m | 100 kHz |
| Twisted pair + I²C buffer (PCA9515 / TCA9617) | up to 3 m | 100 kHz |
| Differential bus (PCA9615 / LTC4332) | up to 100 m | 100 kHz |

> For lengths over 0.3 m, **twisted pair is mandatory**: SDA and SCL must
> be in **different** pairs, each with its own ground (for example, blue +
> blue-white for SDA, green + green-white for SCL). SDA and SCL in
> a single pair create cross-coupled capacitance that distorts the edges.

## Mains-side wiring (sensors)

### Voltage sensor (UI* variants)

The board exposes `L` (line) and `N` (neutral) terminals. **Polarity
matters** for the sign of active power:

- Correct (L → L, N → N): `P > 0` means **consumption** (current from
  the grid into the load), `P < 0` means **export** to the grid (generation).
- Swapped (L and N reversed): the sign of P inverts — consumption is
  counted as negative power.

Correct polarity is critical for:

- Correct accounting on the BASIC tier (BASIC counts only `max(P, 0)`;
  with swapped polarity the energy counter stays at zero).
- Correctly separating consumption and generation on STANDARD and PRO
  (once implemented).

> **Quick check**: on a purely resistive load (electric kettle,
> heater, incandescent lamp) `P > 0` and `PF ≈ 1.0`. If P is
> negative under a consuming load — swap L and N on the board.

### Current sensor — SCT-013 CT clamp

External CT clamps connect to the module through a 3.5 mm jack
connector. The connector is keyed — no wiring decisions need to be made.

An arrow on the clamp body shows the current direction for the "normal"
orientation. Rules:

- Clamp the CT around the **line conductor (L)** going to the load.
- The arrow on the clamp must point **in the direction of current**
  toward the load (from the breaker to the load).
- The clamp must **close fully** — with no gap between the
  halves of the magnetic core. A gap > 0.1 mm sharply increases
  magnetic reluctance and degrades accuracy.

> ⚠ **Do not run both L and N through a single clamp at once**. Their
> magnetic fluxes are equal and opposite, so they cancel each
> other out — the sensor will read ~zero even under load.

For more on choosing the SCT-013 model (5A / 10A / 30A / 50A / 100A) and
how to tell the module via `dev.setCTModel(code)` — see
[03_sensor_selection.md](03_sensor_selection.md).

> ⚠ **CT clamp polarity matters on BASIC**: if the clamp arrow points away from the load (or L/N on the terminals are swapped), `P_real` reads negative under consumption — and the BASIC tier clamps `max(P, 0)`, so the Wh counter stays at zero. Flip the clamp on the conductor physically; do not invert it in software.

## DATA_READY (DRDY)

An optional pin for polling optimization. If the application polls the
module no more often than once every 200 ms, `DRDY` can be left unconnected.

### Electrical parameters

- **Output type**: open-drain (no active pull-up to VCC)
- **Idle level**: HIGH (requires a pull-up on the master side —
  the MCU's built-in pull-up or an external resistor; 10 kΩ to
  3.3 V recommended)
- **Ready pulse**: LOW for ~10 µs after the RT registers
  (`0x86..0xCF`) have been updated with fresh data
- **Pulse frequency**: ~5 Hz (one pulse per ~200 ms RT window)

### Semantics

A falling edge on `DRDY` guarantees that **all RT registers are
synchronized and published** (the firmware updates them atomically in
an ISR before pulling the pin low). After the falling edge the master can read
`0x86..0xCD` without risk of getting a split sample.

### Use on Arduino

- **External interrupt** (`attachInterrupt` on the falling edge) — wakes
  the master and triggers a read.
- **Pull-up**: either `INPUT_PULLUP` on the MCU, or an external 10 kΩ to 3.3 V. **On AVR hosts use an external 10 kΩ to 3.3 V** — the built-in pull-up (~40 kΩ) is too weak for long DRDY lines.

```cpp
const int PIN_DRDY = 15;
volatile bool data_ready = false;

#if defined(ESP32) || defined(ESP_PLATFORM)
void IRAM_ATTR on_drdy() { data_ready = true; }   // ESP32 — ISR in IRAM
#else
void on_drdy() { data_ready = true; }             // AVR / STM32duino / RP2040 / SAMD
#endif

void setup() {
    Wire.begin();
    while (!dev.begin()) { delay(500); }

    // Raise the pull-up + attach the ISR AFTER dev.begin() — this
    // guarantees the module is already ready and the pull-up edge will not
    // cause a spurious interrupt before the first RT window is latched.
    // INPUT_PULLUP is fine for most cases; for long
    // lines or AVR hosts add an external 10 kΩ to 3.3 V.
    pinMode(PIN_DRDY, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_DRDY), on_drdy, FALLING);
}

void loop() {
    if (!data_ready) return;     // sleep until the next RT window
    data_ready = false;
    float p = dev.readPower(0);
    // ...integration / transmit...
}
```

`DRDY` is **optional** — polling at any rate ≤ 5 Hz works without
it. The library does not depend on `DRDY` in any of its paths.

## Connecting to different Arduino hosts

The module works with any Arduino-compatible board through the standard
`Wire`. Below are the standard default pins for popular variants.

### Arduino UNO / Nano (5 V logic)

| rbAmp | Arduino pin |
|---|---|
| `VCC` | `+5V` |
| `GND` | `GND` |
| `SDA` | `A4` |
| `SCL` | `A5` |
| `DRDY` (opt.) | any digital, e.g. `D2` |

```cpp
#include <Wire.h>
#include <RbAmp.h>

RbAmp dev(Wire, 0x50);

void setup() {
    Wire.begin();          // SDA=A4, SCL=A5 by default
    Wire.setClock(100000); // 100 kHz — the AVR I²C peripheral needs no 50 kHz clamp
    while (!dev.begin()) { delay(500); }
}
```

The UNO is a 5 V host, **no level translator needed** (the module's SDA/SCL
lines are 5 V tolerant). The module's built-in pull-ups pull the lines to
3.3 V, which a 5 V master reads as HIGH correctly.

### Arduino Mega 2560 (5 V logic)

| rbAmp | Mega pin |
|---|---|
| `VCC` | `+5V` |
| `GND` | `GND` |
| `SDA` | `D20` |
| `SCL` | `D21` |
| `DRDY` (opt.) | any digital, e.g. `D18` (supports interrupts) |

Identical to the UNO in code, only the physical pins differ. No level
translator needed.

### ESP32 / ESP32-S3 / ESP32-C3 (3.3 V logic)

| rbAmp | ESP32 DevKitC pin |
|---|---|
| `VCC` | `+5V` (powered from USB-5V on the DevKitC) |
| `GND` | `GND` |
| `SDA` | `GPIO21` |
| `SCL` | `GPIO22` |
| `DRDY` (opt.) | any free GPIO, e.g. `GPIO15` |

```cpp
#include <Wire.h>
#include <RbAmp.h>

RbAmp dev(Wire, 0x50);

void setup() {
    Wire.begin(21, 22);    // SDA=21, SCL=22 (or Wire.begin() with board defaults)
    Wire.setClock(50000);  // ESP32: 50 kHz — mandatory for retry discipline
    while (!dev.begin()) { delay(500); }
}
```

The ESP32 is a 3.3 V host, direct connection to the module. VCC power is taken
from the DevKitC's +5V pin (which is connected to USB-5V directly).

> **ESP32 50 kHz clamp — the recommended pattern.** Instead of an explicit `Wire.setClock(50000)` call, define the opt-in macro before the include — `begin()` will apply the clamp internally:
>
> ```cpp
> #define RBAMP_ESP32_CLAMP_HZ 50000
> #include <RbAmp.h>
> ```
>
> The macro is opt-in (not default-on) — so that explicit user calls such as `Wire.setClock(400000)` for OLED-on-shared-bus patterns are preserved. Both forms work — pick the one that reads better in your sketch.

> If your ESP32 board does not break out a 5 V pin (some compact
> ESP32-S3 / C3 modules get by with only Vin or no 5 V at all) —
> power rbAmp from an external 5 V source (for example, a USB charger +
> breakout), with GND shared with the ESP32.

A separate note on resetting the module:

> For a software reset of the module, use `dev.reset()` — the library sends the command and the module reboots in ~100 ms. If you need a hard reset (for example, to recover from a brick state) — power-cycle VCC (disconnect for 1+ second).

### ESP8266 (Wemos D1 mini / NodeMCU, 3.3 V logic)

| rbAmp | ESP8266 pin (NodeMCU) |
|---|---|
| `VCC` | `Vin` or `+5V` |
| `GND` | `GND` |
| `SDA` | `D2` (GPIO4) |
| `SCL` | `D1` (GPIO5) |
| `DRDY` (opt.) | `D5` (GPIO14) |

```cpp
#include <Wire.h>
#include <RbAmp.h>

RbAmp dev(Wire, 0x50);

void setup() {
    Wire.begin(D2, D1);      // SDA=GPIO4, SCL=GPIO5
    Wire.setClock(50000);    // ESP8266 software I²C: 50 kHz for consistency with ESP32
    while (!dev.begin()) { delay(500); }
}
```

The ESP8266 is a 3.3 V host, direct connection.

### STM32duino (STM32F1 dev-board F103 / Black Pill F411, 3.3 V logic)

| rbAmp | STM32 pin (STM32F1 dev-board) |
|---|---|
| `VCC` | `+5V` |
| `GND` | `GND` |
| `SDA` | `PB7` (I²C1_SDA) |
| `SCL` | `PB6` (I²C1_SCL) |
| `DRDY` (opt.) | any GPIO with EXTI, e.g. `PA0` |

```cpp
#include <Wire.h>
#include <RbAmp.h>

RbAmp dev(Wire, 0x50);

void setup() {
    Wire.begin();           // SDA=PB7, SCL=PB6 by default for I²C1
    Wire.setClock(100000);  // the STM32 HAL I²C needs no 50 kHz clamp
    while (!dev.begin()) { delay(500); }
}
```

STM32duino supports multiple `Wire` instances (`Wire1`,
`Wire2` for a second I²C). If the primary bus is busy — use the
second I²C peripheral.

### RP2040 / SAMD (Raspberry Pi Pico, arduino-pico)

| rbAmp | RP2040 pin (Pico) |
|---|---|
| `VCC` | `VBUS` (5 V from USB) or `+5V` |
| `GND` | `GND` |
| `SDA` | `GP4` |
| `SCL` | `GP5` |
| `DRDY` (opt.) | any GP |

```cpp
#include <Wire.h>
#include <RbAmp.h>

RbAmp dev(Wire, 0x50);

void setup() {
    Wire.setSDA(4);
    Wire.setSCL(5);
    Wire.begin();
    Wire.setClock(100000);  // the RP2040 hardware I²C needs no 50 kHz clamp
    while (!dev.begin()) { delay(500); }
}
```

The RP2040 is a 3.3 V host, direct connection. On SAMD boards (Arduino Zero,
MKR) `Wire.begin()` with defaults works the same way — the specific pins
depend on the board.

> **Pico via VSYS (battery / external power)**: the `VBUS` pin
> is present only when powered over USB. If the Pico runs from a
> battery or an external source through `VSYS` — `VBUS` will carry
> nothing. Feed 5 V from an external source to the module's `VCC`
> directly, with GND shared with the Pico.

## Reference wiring diagram (ESP32 + single module)

```text
                                rbAmp module
                            +----------------------+
                            |   LV side            |  mains side
ESP32 GPIO22 (SCL) ---------| SCL                  |◄[CT jack 3.5mm]── SCT-013 → to L
ESP32 GPIO21 (SDA) ---------| SDA                  |◄[L terminal]──── L wire
ESP32 GPIO15 (IN) ◄──[10K]─┐| DRDY (open-drain)    |◄[N terminal]──── N wire
                    to 3.3V |                      |
ESP32 +5V    ---------------| VCC                  |
ESP32 GND    ---------------| GND                  |
                            +----------------------+

In this configuration:
 - External pull-ups on I²C are NOT needed (built into the module).
 - 10K on DRDY is a mandatory external pull-up.
 - The SCT-013 jack is fully inserted.
 - The clamp is fully closed on the L wire, arrow toward the load.
```

## Reference wiring diagram (Arduino UNO + single module)

```text
                                rbAmp module
                            +----------------------+
                            |   LV side            |  mains side
UNO A5 (SCL) ---------------| SCL                  |◄[CT jack 3.5mm]── SCT-013 → to L
UNO A4 (SDA) ---------------| SDA                  |◄[L terminal]──── L wire
UNO D2 (IN, opt.) ◄──[10K]─┐| DRDY (open-drain)    |◄[N terminal]──── N wire
                    to 3.3V |                      |
UNO +5V      ---------------| VCC                  |
UNO GND      ---------------| GND                  |
                            +----------------------+

In this configuration:
 - External pull-ups on I²C are NOT needed.
 - The UNO is 5 V logic; the module's SDA/SCL are 5 V tolerant, no level
   translator required.
 - DRDY has a 3.3 V level — on the UNO a TTL input reads this as
   HIGH (VIH_TTL ≥ 2.0 V, 3.3 V > 2.0 V). A DRDY pull-up is recommended
   specifically to 3.3 V for safety: if the DRDY pin's internal
   protection circuits are not 5 V tolerant (separately from the I²C
   lines, whose 5 V tolerance is confirmed), a pull-up to 5 V would create a small
   leakage current through the ESD diodes into the module's 3.3 V rail.
   To 3.3 V — never a problem.
```

## Multi-phase model roadmap

For future multi-phase SKUs the LV pinout is **preserved** (VCC / GND
/ SDA / SCL / DRDY). Only the mains side changes — more L terminals and
more CT jack connectors. Existing user code will not need to be rewritten;
the library will add registers for the additional channels.

## Multi-module bus — the primary topology

The library's **canonical use case** is several
rbAmp modules on one I²C bus, managed by a single Arduino /
ESP32 through `RbAmpFleet`. All the rules and optimizations below are designed
for exactly this topology; a single module is a special case (the same
handle, but the fleet calls degenerate into trivial ones).

### Typical deployments

| Scenario | Configuration | Module count |
|---|---|---|
| **80% canon**: home feed + submeters | 1× UI1 (feed, mains-energy) + 3-6× UI1/I1 (loads) | 4-7 |
| Sub-panel with current breakdown | 1× UI1 (feed) + 1-2× I2/I3 (per-circuit current) | 3-5 |
| Industrial sub-metering | 1× UI1 (feed) + N× I2/I3 (per machine, current breakdown) | up to ~16 |

### Electrical limits

- **Module count**: up to ~16 (total bus capacitance ≤ 400 pF at 100 kHz; at 50 kHz there is more margin).
- **Pull-ups for the fleet bus (important)**: each module ships with built-in 4.7 kΩ. On a multi-module bus N pull-ups work in parallel → the sink is overloaded and the bus cannot pull down to LOW. **Cut the built-in pull-ups on all modules except one** (or cut them on all and keep **only external** ~4.7 kΩ to 3.3 V at a single point on the bus — recommended for long runs / noise / ESP32 as master).
- The ESP32's internal pull-ups (~45 kΩ) are **too weak** for a multi-module bus with real run lengths — **external** ~4.7 kΩ are needed. This is part of the three-layer mitigation against bus-hang on a marginal bus — see [10 · Troubleshooting](10_troubleshooting.md), the section "ESP32 project hangs".

### Addressing — provisioning and field-swap (production-OK)

The default factory address is `0x50`. Every new or replacement module
goes through **provisioning** (once): reassignment from `0x50` to a
working address + optional saving of the configuration (CT models,
group_id, label) to flash. Factory-mode is **not required** —
the address change works directly in production via two-phase address commit.

#### Method 1 — `RbAmpFleet::provision()` (recommended)

```cpp
RbAmp* dev = nullptr;
if (RbAmpFleet::provision(Wire, /*desired=*/0x52, /*save=*/true, dev)) {
    fleet.add(dev);   // ready handle, now in the fleet
}
```

A single call scans `0x50`, performs the two-phase address commit, resets,
searches for the module at the new address in the boot window, and (optionally)
issues `SAVE_USER_CONFIG` for persistence. Returns `false` on a discipline
violation (see below) or a transport failure.

#### Method 2 — low-level `prepare/commitAddressChange` (manual form)

```cpp
dev.prepareAddressChange(0x52);   // candidate in RAM
dev.commitAddressChange();        // arms 0xA5 + COMMIT_ADDR + reset
delay(300);                        // boot window
```

The same two-phase mechanism as in `provision()`, without the fleet wrapper.
Used when `RbAmpFleet` has not been created.

#### Wire protocol under the hood (for reference)

```text
1. write candidate_addr → REG_I2C_ADDRESS (0x30, in RAM)
2. write 0xA5 → REG_ADDR_COMMIT_MAGIC (0x31, armed)
3. issue CMD_COMMIT_ADDR (opcode 0x30 in REG_COMMAND)
4. wait ~700 ms (flash erase + write)
5. issue CMD_RESET (opcode 0x01) — CMD_COMMIT_ADDR does not auto-reset
6. wait ~300 ms; the module responds at the new address
```

> ✅ **`REG_I2C_ADDRESS (0x30)` reads the active address on boot** (v1.3
> addr-boot-sync). After the post-commit reboot, reading `0x30` shows the
> new active address. The staging-echo behavior persists until
> `CMD_COMMIT_ADDR` + reset.

> ⚠ **Provisioning discipline — MUST one virgin at a time.**
>
> The most common source of provisioning failures is having
> **more than one** module with the factory address `0x50` on the bus.
> It is **physically impossible** to distinguish them over I²C
> (open-drain wired-AND — both modules will ACK identically, and the
> read-back data will be "stuck together").
>
> **So the rule is strict (a normative MUST, not a recommendation):**
> at the moment you call `RbAmpFleet::provision()` (or
> `prepareAddressChange()`), there must be **exactly one** module at
> `0x50` (a new one or one returned to factory defaults via a separate
> bench tool). No "I'm sure these two modules are different — I'll just
> separate them now" — it will not work; the collision shows up later,
> once you already think the fleet is clean.
>
> **Recovery path if you suspect a discipline violation:**
> 1. Power-cycle all modules.
> 2. Physically disconnect all but one "virgin" module.
> 3. Call `RbAmpFleet::provision()` on the single-module bus.
> 4. Add the remaining modules one at a time, provisioning each separately.

### Bus power budget

At 50 kHz, one read transaction ≈ 200-400 µs. A full RT block of one channel (U + I + P + PF) ≈ 5-8 ms per channel.

| Configuration | Bus per cycle | At 50 kHz | Max polling rate |
|---|---|---|---|
| 1× UI1, RT block | ~5 ms | < 5 ms | 100+ Hz (but the module updates at 5 Hz) |
| 1× UI3, RT block + period | ~25 ms | ~25 ms | 5 Hz |
| 5× UI1, RT block | ~25 ms | ~25 ms | 5 Hz |
| 16× UI3, RT + period | ~400 ms | **400 ms > 200 ms** | **can't keep up with the module** |

> The numbers are estimates (200-400 µs/transaction depends on the MCU). Exact values will be confirmed by a bench test.

**Rule**: at 50 kHz the comfortable limits are **8-10× UI3** or **15-16× UI1** at 5 Hz polling.

### Period sync

For tariff metering, all modules must latch **the same** interval. Two strategies:

**Strategy 1 — sequential latch + common settle** (any firmware):

```cpp
RbAmp meters[N];   // array of initialized modules

void billing_cycle() {
    // Phase 1: sequential latch
    for (auto& m : meters) m.latchPeriod();

    // Phase 2: common settle (≥ 50 ms)
    delay(50);

    // Phase 3: read the snapshots with skip_latch=true
    for (auto& m : meters) {
        RbAmpSnapshot snap;
        m.readPeriodSnapshot(snap, /*skip_latch=*/true);
    }
}
```

Skew between modules: 16 modules × ~1 ms/latch = ~16 ms relative to a 60-s period → 0.027%. Negligible for billing.

**Strategy 2 — General-Call broadcast latch** (requires enable):

All modules latch with a single I²C frame on addr `0x00`:

```
addr=0x00 | A5 27 <group> <tick_lo> <tick_hi>   (5 bytes)
```

Skew = 0 (atomic). Pre-config (once on each module of the fleet):

1. Set `REG_FLEET_CONFIG.bit0 = 1` via `CMD_SAVE_USER_CONFIG`.
2. `CMD_RESET` — GC reception is activated on boot.
3. After reset, the master sends the 5-byte frame on `0x00`.
4. Witness: for each expected module, read `REG_V03_PERIOD_VALID (0x07)`. `!=1` → fall back to strategy 1.

Which to use:
- **< 8 modules**: strategy 1 (simpler, no enable required).
- **≥ 8 modules** or **billing-grade synchronization**: strategy 2.

### GC group filtering (multi-tenant bus)

`REG_GROUP_ID (0x28)` lets you have several GC domains on one bus:

- `group = 0x00` → all-call (all modules with GC enabled latch).
- `group = N` → only modules with `REG_GROUP_ID = N` latch.

Use case — separating tariff groups (day vs night) on one shared bus.

### Failure modes

| Symptom | Cause | What to do |
|---|---|---|
| `i2c bus scan` shows several modules at one address after a field add | A new module at `0x50` conflicts with an already-addressed one | Re-address the new module **separately** before adding it |
| One module NACKs more often than the others | Cable fault / VCC drop | Check VCC under load |
| One module's Wh consistently diverges | Module clock drift (LSI) | Clock-drift calibration on the bench |
| After a GC frame, one module does not latch | `FLEET_CONFIG.bit0 = 0` or group mismatch | Check `REG_FLEET_CONFIG` and `REG_GROUP_ID` |

### Bus design notes

- **Clock stretching**: the module's latch produces a short `__disable_irq` (~µs); SCL is not stretched significantly. For the Raspberry Pi BCM283x (known hw bug in stretching): the standard precaution is 50 kHz + retry.
- **Length with >4 modules**: consider an I²C buffer (PCA9515) at ≥ 8 modules + length > 1 m.

Detailed multi-module scenarios — in [06 · Examples](06_examples.md).

## What's next

- [02 · Module Tiers](02_tiers.md) — firmware behavior by tier
- [03 · Current Sensor Selection](03_sensor_selection.md) — which SCT-013
  model and how to tell the module
- [05 · Quickstart](05_quickstart.md) — your first working sketch
- [10 · Troubleshooting](10_troubleshooting.md) — what to do if the bus
  is not working / I_rms is unstable / the sign of P is wrong
