# Overview

## What is rbAmp?

**rbAmp** (rocknroller-Amp, also known as PY32-DimmerLink) is an I2C AC
sensor / dimmer module built around the PY32F030 microcontroller. A single
module measures one to three AC channels:

- **RMS voltage** (U_RMS) and **peak voltage** — at 200 ms refresh
- **RMS current** (I0/I1/I2_RMS) — per channel, with peak too
- **Real power** (P0/P1/P2_REAL) — signed, watts; negative = net export
- **Power factor** (PF0/PF1/PF2) — −1..+1
- **Mains frequency** (50 / 60 Hz)
- **Period-averaged power** for energy integration — master latches a
  measurement period via `CMD_LATCH_PERIOD`, reads `REG_V03_PERIOD_AVG_P`,
  and integrates Wh against its own wall clock

Hardware variants:

- **UI1 / I1** — 1 voltage + 1 current (or 1 current only)
- **UI2 / I2** — 1 voltage + 2 currents (split-phase or two-circuit)
- **UI3 / I3** — 1 voltage + 3 currents (three-phase or per-appliance)

The module connects to any I2C-capable host on a 7-bit address (default
`0x50`, configurable). The cross-platform wire protocol is documented in the
[the rbAmp protocol spec](https://www.rbamp.com/docs/modules-basic-standard-api-reference) repository.

## What does this Arduino library do?

`RbAmp` wraps the wire-level protocol behind a high-level C++ class so user
sketches don't deal with register addresses, byte ordering, settle times,
or retry plumbing.

### Without the library (raw API)

```cpp
// Read U_RMS — one I2C address phase per byte (no auto-increment).
Wire.beginTransmission(0x50);
Wire.write(0x86);
Wire.endTransmission(false);
Wire.requestFrom((uint8_t)0x50, (uint8_t)1);
uint8_t b0 = Wire.read();
Wire.beginTransmission(0x50);
Wire.write(0x87);
Wire.endTransmission(false);
Wire.requestFrom((uint8_t)0x50, (uint8_t)1);
uint8_t b1 = Wire.read();
// ...repeat for b2, b3...
float u_rms;
uint8_t buf[4] = {b0, b1, b2, b3};
memcpy(&u_rms, buf, 4);
// ...and that's just one float, with no retry, no sanity check...
```

### With the library

```cpp
float u_rms = dev.readVoltage();
// 4 retries-included reads + little-endian assembly + sanity filter + error reporting,
// all in one line.
```

### Period metering — without the library

```cpp
Wire.beginTransmission(0x50);
Wire.write(0x01);    // REG_COMMAND
Wire.write(0x27);    // CMD_LATCH_PERIOD
Wire.endTransmission();
uint32_t t_latch = millis();
delay(50);           // SPEC §7 settle

Wire.beginTransmission(0x50);
Wire.write(0x07);
Wire.endTransmission(false);
Wire.requestFrom((uint8_t)0x50, (uint8_t)1);
if ((Wire.read() & 0x01) == 0) {
    // stale, retry later, must still commit t_latch otherwise next period double-counts
    return;
}
// ...read 4 bytes of avg_p, assemble float, integrate Wh manually...
```

### Period metering — with the library

```cpp
RbAmpPeriodSnapshot snap;
if (dev.readPeriodSnapshot(snap)) {
    // snap.avg_p[0] populated, dev.energy().wh(0) ticked, master_dt_ms recorded
}
```

The library handles every protocol invariant from the SPEC:

- Single-byte transactions (no slave-side auto-increment)
- 50 ms settle after `CMD_LATCH_PERIOD`, 700 ms after `CMD_SAVE_GAINS`
- `REG_V03_PERIOD_VALID` check before consuming a snapshot
- Master-wall-clock dt commit even on stale snapshots (prevents double-count)
- SPEC §B.5 retry+sanity discipline on ESP32 targets (3 attempts × 5 ms gap)
- Two-step address-change with a 5-second arm window
- Per-channel Wh accumulator with double-precision integration

## When to use the library, when to drop to raw API

| Situation | Use library | Use raw API |
|---|---|---|
| Read U / I / P / PF | ✅ | only for bus-analyser debugging |
| Period metering + Wh | ✅ | only if you own the Wh persistence externally |
| Multi-module on one bus | ✅ | only for v2 broadcast-LATCH experiments |
| Address change | ✅ (two-step API enforced) | risky — typo bricks the module |
| Custom dimmer registers (0x10..0x18) | not exposed in library v1.0 | ✅ |
| Calibration protocol (0x20..0x25) | not exposed | ✅ — see PY32-LUT-Cal firmware |
| Porting to a non-Arduino runtime | n/a | ✅ — start from the wire patterns |
| Tight RAM budget (< 4 kB free heap) | rarely fits on AVR Uno | ✅ |

For everything in the first six rows, this library is what you want. The raw
API recipes live in the cross-platform reference docs at
[raw I²C API examples](https://www.rbamp.com/docs/modules-basic-standard-arduino-examples).

## Library architecture

```
┌─────────────────────────────────────────────────────┐
│  User sketch (.ino)                                 │
│                                                     │
│   RbAmp dev(Wire, 0x50);                            │
│   dev.begin();                                      │
│   dev.readPeriodSnapshot(snap);                     │
│   dev.energy().wh(0);                               │
└─────────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────┐
│  RbAmp class — public surface (RbAmp.h)             │
│   ┌─────────────────────────────────────────────┐   │
│   │  Lifecycle:  begin / probe / waitReady      │   │
│   │  RT reads:   readVoltage / readPower / …    │   │
│   │  Period:     latchPeriod / readPeriodSnapshot│   │
│   │  Config:     setCTModel / saveGains / …     │   │
│   │  Address:    prepare/commitAddressChange    │   │
│   │  Diag:       lastError / errorString /      │   │
│   │              retryExhaustionCount / …       │   │
│   └─────────────────────────────────────────────┘   │
│                       │                             │
│                       ▼                             │
│   ┌─────────────────────────────────────────────┐   │
│   │  Private helpers — RbAmp.cpp                │   │
│   │   readU8 (retry loop, SPEC §B.5)            │   │
│   │   readFloatLE (loose sanity filter)         │   │
│   │   writeReg / writeCmd                       │   │
│   └─────────────────────────────────────────────┘   │
│                       │                             │
│                       ▼                             │
│   ┌─────────────────────────────────────────────┐   │
│   │  Owned components:                          │   │
│   │   • RbAmpEnergy — per-channel Wh            │   │
│   │   • RbAmpSnapshot / RbAmpPeriodSnapshot     │   │
│   │   • Constants from RbAmpRegisters.h         │   │
│   └─────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────┐
│  Arduino Wire library — platform-native I2C         │
└─────────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────┐
│  PY32-DimmerLink slave @ 0x50                       │
│   • V03 metering pipeline (200 ms commit)           │
│   • Period accumulator (CMD_LATCH_PERIOD)           │
│   • Calibration in flash (NF + GAIN per CT model)   │
└─────────────────────────────────────────────────────┘
```

## Sequence diagrams

### `begin()` flow

```
sketch          RbAmp                  Wire                 PY32 slave
  │               │                      │                       │
  ├─dev.begin()──►│                      │                       │
  │               ├─readU8(REG_VERSION)─►│                       │
  │               │                      ├──[0x50, 0x03]────────►│
  │               │                      │◄─────────────────ACK──┤
  │               │                      ├──[read 1 byte]───────►│
  │               │                      │◄──────────────[0x01]──┤
  │               │◄─────────────0x01────┤                       │
  │               │  (firmware OK)       │                       │
  │               │                      │                       │
  │               ├─readFloatLE(U_RMS)──►│  (4 single-byte reads)│
  │               │  → 226.3 V           │                       │
  │               │  → has_voltage_hw=true                       │
  │               │                      │                       │
  │               ├─writeCmd(LATCH)─────►│                       │
  │               │                      ├──[0x50, 0x01, 0x27]──►│
  │               │  delay(50)           │                       │
  │               │                      │                       │
  │◄────true──────┤                      │                       │
```

### `readPeriodSnapshot(snap)` flow

```
sketch          RbAmp                  Wire                 PY32 slave
  │               │                      │                       │
  ├─readPeriodSnapshot(snap)────────────►│                       │
  │               ├─writeCmd(LATCH)─────►│  master_t_now = now() │
  │               │                      │                       │
  │               │  delay(50)           │                       │
  │               │                      │                       │
  │               ├─readU8(PERIOD_VALID)►│                       │
  │               │  → bit0 = 1          │                       │
  │               │                      │                       │
  │               ├─readFloatLE(AVG_P0)─►│ (4 single-byte reads) │
  │               ├─readFloatLE(MAX_P0)─►│ (4 single-byte reads) │
  │               ├─readU32LE(LATCH_MS)─►│ (4 single-byte reads) │
  │               │                      │                       │
  │               │  snap.master_dt_ms = now() − last_latch_ms_  │
  │               │  energy_.tick(snap, channels_)               │
  │               │                      │                       │
  │◄────true──────┤                      │                       │
```

For the per-byte retry path (SPEC §B.5) — every single-byte read above
internally retries up to 3 times with a 5 ms gap on ESP32 targets. Sanity
filter rejects NaN / Inf / |x|>10000 on float reads.

## vs raw register API

Read [docs/06_examples.md](06_examples.md) for full scenario walkthroughs;
the key migration table:

| Raw API | Library equivalent |
|---|---|
| Manual 4-byte read + memcpy | `dev.readVoltage()` / `dev.readPower(ch)` |
| `Wire.write(0x01); Wire.write(0x27); delay(50)` | `dev.latchPeriod(); delay(50)` (or `dev.readPeriodSnapshot()`) |
| `Wire.read(0x07) & 1` valid check | `dev.isPeriodValid()` |
| Manual `total_wh += avg_p * dt / 3600` | `dev.energy().wh(0)` — automatic |
| `RB_ERR_*` codes from chapter 10 helpers | `dev.lastError()` + `RbAmp::errorString(code)` |
| ESP32 NACK retry loop | Built into `readU8()` — see SPEC §B.5 |
| Float sanity filter | Built into `readFloatLE()` |

The raw helpers in the cross-platform reference (~50 lines of C) collapse
to a single `#include <RbAmp.h>` + constructor.

## Next steps

- [Quickstart](05_quickstart.md) — 30-second hello-world
- [Examples](06_examples.md) — 10 scenario walkthroughs
- [API Reference](09_api_reference.md) — every public method
- [Troubleshooting](10_troubleshooting.md) — when things don't work

## Related — main rbAmp documentation

- [API Reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference) — formal I²C register / command / error spec the library wraps
- [Arduino Examples (raw I²C)](https://www.rbamp.com/docs/modules-basic-standard-arduino-examples) — same scenarios without the library, useful for porting
- [Period Metering](https://www.rbamp.com/docs/modules-basic-standard-period-metering) — atomic latch concept and master-side energy formula
- [Hardware Connection](https://www.rbamp.com/docs/modules-basic-standard-hardware-connection) — pinout, wiring, CT installation
- [Troubleshooting](https://www.rbamp.com/docs/modules-basic-standard-troubleshooting) — module-side issues (NACK, calibration drift, bus noise)


---

[Contents](README.md) | [Tier Support →](02_tiers.md)
