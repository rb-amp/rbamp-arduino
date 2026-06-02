# Product tiers — BASIC / STANDARD / PRO

> **Shared content note**: tier semantics are identical across every
> platform (Arduino / ESP-IDF / MicroPython / CPython / STM32-HAL). The
> definitive reference is [`_shared/tier_comparison.md`](../../../docs/_shared/tier_comparison.md)
> *(created in a future Phase 4 session)*. This page documents the tiers
> from the Arduino library's perspective — what API surface each tier
> exposes and how to detect which tier you have.

## Tier overview

| Tier | Voltage HW | Current channels | Period accumulator | Bidirectional energy | Notes |
|---|---|---|---|---|---|
| **BASIC** | optional (UI1/UI2/UI3 vs I1/I2/I3 SKUs) | 1 / 2 / 3 | unsigned (clamps negative samples to 0) | master-side only — see scenario 5 | Cheapest, intended for residential metering |
| **STANDARD** | always present | 1 / 2 / 3 | signed (preserves negative samples — solar export visible) | native | Mid-tier, solar-with-grid use case |
| **PRO** | always present | 3 | signed + per-channel reactive power + harmonics | native | Industrial / three-phase audit |

All three tiers expose the **same I2C register map** at v1.0 of the
protocol. The library API surface (`readVoltage()`, `readPower(ch)`, …) is
identical for every tier. Tier differences manifest in:

1. **Which channels exist** — discovered via constructor topology hint
   (v1.0 firmware) or `REG_TOPOLOGY` (v1.1 firmware, see below).
2. **Whether voltage hardware is present** — `dev.hasVoltageHw()`.
3. **Sign of the period-averaged power** — BASIC clamps negative; STANDARD
   and PRO preserve sign. Affects whether you can do native bidirectional
   accounting (`dev.energy().wh(0)` going negative on export) or need to
   split master-side at 5 Hz RT cadence.
4. **Whether the reactive-power register is populated** — STANDARD / PRO
   only; library v1.0 doesn't expose it (RESERVED for v2 — see
   [Changelog](changelog.md)).

## What the library exposes per tier

```cpp
RbAmp dev(Wire, 0x50);
dev.begin();

dev.channels();        // 1, 2, or 3 — set by constructor hint (v1.0) or REG_TOPOLOGY (v1.1+)
dev.hasVoltageHw();    // true on UI* SKUs, false on I*-only SKUs
dev.topology();        // RbAmpTopology::Single / SplitPhase / ThreePhase
```

The same code runs on any tier. For BASIC modules where you want
bidirectional accounting at the master, follow [scenario 5 in
06_examples.md](examples.md#scenario-5--master-side-bidirectional-accounting)
— sample `dev.readPower(0)` at 5 Hz, split into two double-precision
buckets.

## Detecting tier at runtime

### BASIC vs STANDARD/PRO

`dev.hasVoltageHw()` distinguishes UI* (voltage hardware present) from
pure-I* SKUs. To distinguish BASIC-UI* from STANDARD/PRO (both have
voltage hardware), watch the **sign** of `readPower(0)` over a known-export
load:

```cpp
// Force a small export condition (load below solar generation)
float p = dev.readPower(0);
if (p < 0) {
    Serial.println(F("STANDARD or PRO (signed period accumulator works)"));
} else if (p == 0 && load_below_solar) {
    // Library returned 0 W but we know the load is exporting →
    // accumulator must be unsigned (BASIC tier)
    Serial.println(F("BASIC — use master-side bidirectional accounting"));
}
```

For installations where you don't know the tier ahead of time, the safe
default is to assume **BASIC + master-side split** — this works on every
tier (signed RT `readPower(0)` is always signed, only the *period
accumulator* differs).

### UI1 / UI2 / UI3 channel count

#### v1.0 firmware

The library cannot reliably auto-detect channel count on v1.0 firmware —
unmapped register reads return `0x00` rather than NACK (SPEC §8). The
constructor's `hint` is authoritative:

```cpp
RbAmp dev(Wire, 0x50);                          // defaults to ThreePhase — over-detect harmless
RbAmp dev(Wire, 0x50, RbAmpTopology::Single);   // explicit UI1
RbAmp dev(Wire, 0x50, RbAmpTopology::SplitPhase); // explicit UI2
```

Unused channels on a UI1 module accessed via `dev.readCurrent(2)` (with a
THREE_PHASE hint) read `0.0 A` and contribute `0 Wh` to integration. Over-detection
is therefore harmless — it just means your dashboards show "3 channels"
instead of "1".

#### v1.1 firmware

`REG_TOPOLOGY` (0x24, R-only byte) reports the compile-time channel count.
Read it via `dev.rawTopology()`:

```cpp
const uint8_t topo = dev.rawTopology();
switch (topo) {
    case 1: Serial.println(F("UI1 / I1")); break;
    case 2: Serial.println(F("UI2 / I2")); break;
    case 3: Serial.println(F("UI3 / I3")); break;
    case 0: Serial.println(F("v1.0 firmware — REG_TOPOLOGY unmapped, falling back to hint")); break;
    default: Serial.print(F("I2C error or unknown — got 0x")); Serial.println(topo, HEX);
}
```

A future v1.1 release of the Arduino library will read `REG_TOPOLOGY`
inside `begin()` and override the constructor hint when the firmware
version is ≥ 0x02. Your existing `RbAmp dev(Wire, 0x50)` call benefits
automatically — no code change needed.

### Firmware version gating

```cpp
const uint8_t fw = dev.firmwareVersion();
if (fw >= 0x02) {
    // v1.1+ features available:
    //  - REG_TOPOLOGY auto-detection
    //  - CT_MODEL preset auto-load
    //  - 5.2.E ISR race fix
}
```

## Which tier for which use case

| Use case | Recommended tier | Why |
|---|---|---|
| Single-room appliance metering | **BASIC** | One channel, no export, cheapest |
| Whole-house consumption | **BASIC UI1** | Mains feed, unidirectional |
| Solar + grid (sell back) | **STANDARD UI1** | Signed period accumulator — `dev.energy().wh(0)` goes negative on net export |
| Per-appliance multi-channel | **BASIC UI3** | 3 CT clamps on 3 loads, no per-circuit export expected |
| Three-phase industrial | **PRO** | 3 channels + reactive power + harmonics (v2 features) |
| Solar inverter monitoring | **STANDARD UI1** | Generation side — signed reading lets you see clip / curtailment |
| Mains + Solar + Loads dashboard | **STANDARD UI1 × 2 + BASIC UI3** | Mains bidir, solar gen-only, loads per-circuit |

See [scenario 6 in 06_examples.md](examples.md#scenario-6--home-energy-balance)
for the full home-balance recipe.

## Tier upgrade path

A module's tier is fixed at manufacture (analog front-end populates voltage
hardware or not; firmware build sets `V03_N_I`). **You cannot upgrade a BASIC
to a STANDARD in software** — the hardware is missing the voltage divider.

To migrate from BASIC to STANDARD:

1. Replace the module physically.
2. New module ships with a fresh I2C address (default `0x50`) — use
   [scenario 10 in 06_examples.md](examples.md#scenario-10--i2c-address-change-develop-mode)
   to assign your old address.
3. Calibration is per-module (NF + GAIN in flash) — re-tune per
   [Sensor Selection](sensor-selection.md) for the new module's
   CT clamp.
4. **Energy totals do not transfer** between modules — the chip itself
   doesn't store Wh, only the master does. After swap, restore your
   master-side total to the new `dev.energy()` accumulator via library
   `reset()` + manual write to your own persistence store.

## Related documentation

- [`_shared/tier_comparison.md`](../../../docs/_shared/tier_comparison.md) — cross-platform tier reference *(future Phase 4)*
- [Sensor Selection](sensor-selection.md) — CT model selection
- [Hardware Setup](hardware.md) — wiring per tier
- [Changelog](changelog.md) — v1.0 / v1.1 feature differences

## Related — main rbAmp documentation

- [API Reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference) — formal I²C register / command / error spec the library wraps
- [Arduino Examples (raw I²C)](https://www.rbamp.com/docs/modules-basic-standard-arduino-examples) — same scenarios without the library, useful for porting
- [Period Metering](https://www.rbamp.com/docs/modules-basic-standard-period-metering) — atomic latch concept and master-side energy formula
- [Hardware Connection](https://www.rbamp.com/docs/modules-basic-standard-hardware-connection) — pinout, wiring, CT installation
- [Troubleshooting](https://www.rbamp.com/docs/modules-basic-standard-troubleshooting) — module-side issues (NACK, calibration drift, bus noise)


---

[← Overview](overview.md) | [Contents](README.md) | [Sensor Selection →](sensor-selection.md)
