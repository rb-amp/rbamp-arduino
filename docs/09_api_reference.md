# API reference

Complete reference for the public API of the `RbAmp` Arduino library v1.0.0.
Source: [`src/RbAmp.h`](../src/RbAmp.h) (Doxygen-style comments) and
[`src/RbAmpSnapshot.h`](../src/RbAmpSnapshot.h) /
[`src/RbAmpEnergy.h`](../src/RbAmpEnergy.h).

For wire-level register addresses + bit semantics, see the
[the rbAmp protocol spec](https://www.rbamp.com/docs/modules-basic-standard-api-reference) repository.

## Header files

| Include | Contains |
|---|---|
| `<RbAmp.h>` | All public types (pulls in the others) |
| `<RbAmpSnapshot.h>` | `RbAmpSnapshot`, `RbAmpPeriodSnapshot`, `RbAmpTopology` |
| `<RbAmpEnergy.h>` | `RbAmpEnergy` |
| `<RbAmpRegisters.h>` | Auto-generated register / command / settle constants — do not edit |

User sketches typically only `#include <RbAmp.h>`.

## Class `RbAmp`

The main client class. One instance per slave device on the I2C bus.

### Constructor

```cpp
explicit RbAmp(TwoWire& bus,
               uint8_t addr = 0x50,
               RbAmpTopology hint = RbAmpTopology::ThreePhase) noexcept;
```

| Param | Description |
|---|---|
| `bus` | Reference to a `TwoWire` bus (typically `Wire`). Caller must call `bus.begin()` before `begin()`. |
| `addr` | 7-bit slave address (0x08..0x77, default 0x50). |
| `hint` | Topology hint when v1.0 firmware can't auto-detect. `ThreePhase` (default) over-detects harmlessly on UI1/UI2 modules (unused channels read `0.0 A`). v1.1 firmware uses `REG_TOPOLOGY` instead — see `rawTopology()`. |

No I2C traffic in the constructor. Subsequent `begin()` does the probe.

### Lifecycle methods

#### `bool begin() noexcept`

Probe the device, set topology from constructor hint, run primer LATCH.

Sequence:

1. Read `REG_VERSION` (0x03) — fails with `RB_ERR_NACK` if no ACK or
   `RB_ERR_VERSION` if returns `0x00` / `0xFF`.
2. Variant detect: voltage hardware via U_RMS threshold > 1.0 V. Channel
   count from constructor `hint` (v1.0) or future `REG_TOPOLOGY` (v1.1).
3. Write `CMD_LATCH_PERIOD` primer (50 ms settle) — discards first
   snapshot to clean the accumulator.
4. Records `master_t_last` for subsequent energy integration.

**Returns**: `true` on success; `false` otherwise — check `lastError()`.

Idempotent — safe to call multiple times (each call issues a fresh primer
LATCH).

#### `bool probe() noexcept`

Lightweight alive check. Single byte read of `REG_VERSION`. No side
effects (no LATCH, no state change).

**Returns**: `true` if the slave ACKs and reports a supported firmware
version (not 0 / 0xFF).

#### `bool waitReady(uint32_t timeout_ms = 1000) noexcept`

Poll `REG_STATUS` (0x00) bit 0 until the device reports valid data.
Useful at boot when the device may need up to 200 ms for its first RT
window. Polls every 10 ms.

**Returns**: `true` if bit 0 observed within timeout; `false` on
`RB_ERR_TIMEOUT`.

#### `uint8_t firmwareVersion() noexcept`

**Returns**: `REG_VERSION` byte. Returns `0` on I2C failure
(`lastError() == RB_ERR_NACK`).

#### `RbAmpTopology topology() const noexcept`

**Returns**: Cached topology — set by constructor hint, used by every
RT and period read for channel filtering. Does not issue any I2C
traffic.

#### `uint8_t channels() const noexcept`

**Returns**: 1, 2, or 3 — derived from `topology()`.

#### `bool hasVoltageHw() const noexcept`

**Returns**: `true` if voltage sensing hardware was detected during
`begin()` (U_RMS > 1.0 V). Cached, no bus traffic.

#### `uint8_t address() const noexcept`

**Returns**: Current 7-bit I2C address — updates after a successful
`commitAddressChange()`.

#### `uint8_t rawTopology() noexcept`

Direct wire read of `REG_TOPOLOGY` (0x24). Distinct from `topology()`
which returns the constructor hint.

**Returns**:

- `1` / `2` / `3` — v1.1 firmware reports `SINGLE` / `SPLIT_PHASE` / `THREE_PHASE`.
- `0x00` — v1.0 firmware (REG_TOPOLOGY unmapped).
- `0xFF` — I2C failure.

### Real-time reads (200 ms refresh on device)

All real-time reads return `float`; failures return `NAN` and set
`lastError()`. Single-value reads internally use the SPEC §B.5 retry +
sanity filter (on ESP32 targets, 3 attempts × 5 ms gap; loose-finite
sanity always-on).

#### `float readVoltage(uint8_t phase = 0) noexcept`

Reads `REG_V03_U_RMS` (0x86, float32 LE) in V. Only `phase = 0`
supported in v1.0 — anything else returns `NAN` with
`RB_ERR_PARAM`.

#### `float readVoltagePeak(uint8_t phase = 0) noexcept`

Reads `REG_V03_U_PEAK` (0x8A, float32 LE) in V.

#### `float readCurrent(uint8_t ch = 0) noexcept`

Reads `REG_V03_I0_RMS + ch*4` (0x8E / 0x92 / 0x96) in A. Channel index
must be < `channels()` — otherwise `NAN` + `RB_ERR_PARAM`.

#### `float readCurrentPeak(uint8_t ch = 0) noexcept`

Reads `REG_V03_I0_PEAK + ch*4` (0x9A / 0x9E / 0xA2) in A.

#### `float readPower(uint8_t ch = 0) noexcept`

Reads `REG_V03_P0_REAL + ch*4` (0xA6 / 0xAA / 0xAE) in W. **Signed** —
negative means net export on bidirectional installations.

#### `float readPowerFactor(uint8_t ch = 0) noexcept`

Reads `REG_V03_PF0 + ch*4` (0xB2 / 0xB6 / 0xBA). Dimensionless, −1..+1.

#### `float readFrequency() noexcept`

Reads `REG_AC_FREQ` (0x20, uint8) in Hz. Returns `50.0` or `60.0` on a
healthy mains.

#### `bool readAll(RbAmpSnapshot& out) noexcept`

One-shot read of the full RT block into a `RbAmpSnapshot`. Equivalent to
calling `readVoltage()` + `readCurrent(0..N)` + `readCurrentPeak(0..N)` +
`readPower(0..N)` + `readPowerFactor(0..N)` + `readFrequency()` in sequence.

Unused channels (per `channels()`) are zeroed.

**Returns**: `true` on success; `false` if any underlying read failed
(check `lastError()` for the first failure code).

### Period metering (SPEC §7)

#### `bool latchPeriod() noexcept`

Writes `CMD_LATCH_PERIOD` (0x27) to `REG_COMMAND`. Does not wait. Caller
is responsible for the 50 ms settle plus `REG_V03_PERIOD_VALID` check
before reading the latched block.

For most usage, prefer `readPeriodSnapshot()` which encapsulates the
full sequence.

**Returns**: `true` on bus-write success.

#### `bool isPeriodValid() noexcept`

Reads `REG_V03_PERIOD_VALID` (0x07) bit 0.

**Returns**: `true` if the latched snapshot at 0xDC / 0xE0 / 0xEC is
fresh; `false` if stale (or on I2C failure — distinguish via
`lastError()`).

#### `float readPeriodAvgPower(uint8_t ch = 0) noexcept`

Reads `REG_V03_PERIOD_AVG_P_F<ch>` for one channel:

- ch=0 → 0xDC
- ch=1 → 0xC2
- ch=2 → 0xC6

Non-contiguous register addresses (per SPEC §7). Must be called after
`latchPeriod()` + 50 ms settle + valid check.

#### `float readPeriodMaxPower() noexcept`

Reads `REG_V03_PERIOD_MAX_P_F0` (0xE0) — peak power on channel 0 during
the latched period, in W.

#### `uint32_t readPeriodLatchMs() noexcept`

Reads `REG_V03_PERIOD_LATCH_MS` (0xEC). Device's view of the period
duration in ms. **Diagnostic only** — use master `master_dt_ms` for
energy integration. PY32's LSI clock can drift up to 20 %.

#### `bool readPeriodSnapshot(RbAmpPeriodSnapshot& out, uint16_t settle_ms = 50, bool skip_latch = false) noexcept`

**Recommended entry point** for period metering. Full sequence:

1. Skip the LATCH if `skip_latch` (use after a `broadcastLatch()` or
   multi-module sequential LATCH burst).
2. Else write `CMD_LATCH_PERIOD`.
3. Capture master wall-clock for `master_dt_ms` computation.
4. `delay(settle_ms)` — default 50 ms per SPEC §7.
5. Check `REG_V03_PERIOD_VALID`. If stale: commit master timestamp
   (to prevent next-cycle double-count) + set `RB_ERR_STALE`.
6. Read `avg_p[0..channels-1]` + `max_p` + `latch_ms`.
7. Commit master timestamp + call `energy().tick(out, channels_)`.

**Returns**: `true` if `out.valid == true`; `false` if the read failed
or the snapshot was stale.

**Stale handling**: the master timestamp commit on stale is critical —
without it, the next successful snapshot's `master_dt_ms` would span
2× the period interval and Wh would be double-counted.

### Energy accessor

#### `RbAmpEnergy& energy() noexcept`

#### `const RbAmpEnergy& energy() const noexcept`

Returns the per-device Wh accumulator (owned by this `RbAmp` instance).
See [`RbAmpEnergy`](#class-rbampenergy) below.

### Configuration (SPEC §10, §11)

#### `bool setCTModel(uint8_t code) noexcept`

Writes `REG_CT_MODEL` (0x05) and persists via `CMD_SAVE_GAINS`
(700 ms blocking).

| `code` | SKU |
|---|---|
| 1 | SCT-013-005 |
| 2 | SCT-013-010 |
| 3 | SCT-013-030 |
| 4 | SCT-013-050 |
| 5 | SCT-013-100 |

Out-of-range `code` (0, 6+) returns `false` with `RB_ERR_PARAM`.

On v1.1 firmware, the write additionally triggers preset NF + GAIN
auto-load — see [Tier Support](02_tiers.md#firmware-version-gating).

#### `bool saveGains() noexcept`

Bare `CMD_SAVE_GAINS` (0x26) + 700 ms settle. Flushes in-memory gain
registers to flash. Use after manual register writes via raw `Wire`
calls (`setCTModel()` and `commitAddressChange()` call this internally).

#### `bool prepareAddressChange(uint8_t new_addr) noexcept`

Step 1 of the SPEC §10 two-step protocol:

1. Validates `new_addr` (0x08..0x77, ≠ current).
2. Reads `REG_MODE` (0x04), refuses if not develop mode (returns `false`
   with `RB_ERR_MODE`).
3. Records the arm timestamp internally — caller must call
   `commitAddressChange()` within 5 seconds or the arm expires.

#### `bool commitAddressChange() noexcept`

Step 2 of the two-step protocol. Sequence:

1. Verify arm is fresh (≤ 5 s old).
2. Write `REG_I2C_ADDRESS` (0x30).
3. Write `CMD_SAVE_GAINS` + 700 ms settle.
4. Write `CMD_RESET` + 100 ms settle.
5. Update internal address field.

If arm window expired, returns `false` with `RB_ERR_TIMEOUT` and clears
the arm flag — caller must re-arm with `prepareAddressChange()`.

#### `bool factoryReset() noexcept`

Writes `CMD_FACTORY_RESET` (0xAA) + 1500 ms settle. Erases all flash
params, reboots the device. Bus unavailable during reset.

#### `bool reset() noexcept`

Writes `CMD_RESET` (0x01) + 100 ms settle. Soft-reboot.

### Multi-module / static

#### `static bool broadcastLatch(TwoWire& bus) noexcept`

I2C General-Call broadcast LATCH — RESERVED FOR v2.

**On v1 firmware**: always returns `false` without touching the wire
(General-Call disabled in PY32 peripheral per SPEC §9). Callers must
fall back to per-device sequential `latchPeriod()` — see
[scenario 3 in 06_examples.md](06_examples.md#scenario-3--multi-module-monitor).

### Diagnostics

#### `int8_t lastError() const noexcept`

**Returns**: One of `rbamp::RB_OK` (0) or `RB_ERR_*` (negative). Updated
on every public-API call. See [Troubleshooting](10_troubleshooting.md#error-codes).

#### `static const char* errorString(int8_t code) noexcept`

**Returns**: Static const string for the error code. Never returns NULL.

#### `void setLogStream(Stream* stream) noexcept`

Optional diagnostic log sink. If set, the library prints brief
diagnostic lines (probe results, stale snapshots, mode-gate refusals)
to `stream`. Pass `nullptr` to disable. Disabled by default.

Use `Serial` for typical debugging:

```cpp
dev.setLogStream(&Serial);
```

#### `uint32_t retryExhaustionCount() const noexcept`

SPEC §B.5 diagnostic counter — total per-byte retry-loop exhaustions
since boot or last `resetCounters()`. Steady-state: **0**.

Non-zero indicates either bus-level issues or insufficient
`RBAMP_NACK_RETRY_ATTEMPTS` for the workload — see SPEC §B.5
dense-workload tuning recipe.

#### `uint32_t sanityRejectCount() const noexcept`

SPEC §B.5 diagnostic counter — total floats rejected by the loose
sanity filter (`!isfinite(x) || fabsf(x) > 10000`). Steady-state: **0**.

Non-zero after retry + 50 kHz mitigation usually means the ESP-IDF
buffer-leak ghost survived the retry layer.

#### `void resetCounters() noexcept`

Zeroes both `retryExhaustionCount()` and `sanityRejectCount()`. Use at
the start of a soak / regression test window.

## Class `RbAmpEnergy`

Per-channel Wh accumulator. Owned by `RbAmp`, accessed via `dev.energy()`.

### Public methods

```cpp
double wh(uint8_t ch = 0) const noexcept;
```

Returns the running total Wh for one channel. Signed (negative = net
export). Out-of-range `ch` returns 0.0.

```cpp
void reset(uint8_t ch = 0) noexcept;
void resetAll() noexcept;
```

Zero one channel's or all channels' accumulators.

```cpp
void disable() noexcept;
void enable() noexcept;
bool isEnabled() const noexcept;
```

Toggle automatic integration. While disabled, `readPeriodSnapshot()`
still works but doesn't tick the accumulator — useful when the master
owns Wh persistence (e.g. [scenario 9 in 06_examples.md](06_examples.md#scenario-9--battery-deep-sleep-logger)).

### Internal `tick()`

```cpp
void tick(const RbAmpPeriodSnapshot& snap, uint8_t channels) noexcept;
```

Called automatically by `RbAmp::readPeriodSnapshot()` on success — user
code does not invoke directly. Idempotent: snapshots with `valid == false`
are ignored.

Integration formula:

```text
wh[ch] += snap.avg_p[ch] * snap.master_dt_ms / 1000.0 / 3600.0
```

### Precision

Platform-dependent — see [Hardware Setup](04_hardware.md#arduino-uno--mega--nano-avr-atmega328--atmega2560):

- ESP32 / ESP8266 / STM32duino / SAMD / RP2040: 64-bit `double` accumulator.
  Drift < 1 LSB / year at 60 s polling cadence.
- Arduino AVR: 32-bit `float` (toolchain defines `double` as `float`).
  Reset periodically for long-soak deployments.

## POD structs

### `RbAmpSnapshot`

Returned by `readAll()`. All fields are SI units. Unused channels (per
`channels`) are zeroed.

```cpp
struct RbAmpSnapshot {
    float voltage;          // V (REG_V03_U_RMS, 0x86)
    float voltage_peak;     // V (REG_V03_U_PEAK, 0x8A)
    float current[3];       // A per channel (REG_V03_I*_RMS)
    float current_peak[3];  // A per channel (REG_V03_I*_PEAK)
    float power[3];         // W per channel, signed (REG_V03_P*_REAL)
    float power_factor[3];  // dimensionless -1..+1 (REG_V03_PF*)
    float frequency;        // Hz (REG_AC_FREQ, 0x20)
    RbAmpTopology topology; // SINGLE/SPLIT_PHASE/THREE_PHASE
    uint8_t channels;       // 1..3
    bool has_voltage_hw;
};
```

### `RbAmpPeriodSnapshot`

Returned by `readPeriodSnapshot()`. The energy-metering primitive.

```cpp
struct RbAmpPeriodSnapshot {
    float avg_p[3];        // W per channel — period-averaged real power
    float max_p;            // W — peak power on channel 0 during period
    uint32_t latch_ms;      // ms — device-reported period duration (diagnostic)
    uint32_t master_dt_ms;  // ms — master wall-clock dt since previous successful latch
    bool valid;             // true if REG_V03_PERIOD_VALID bit0 == 1 at read time
};
```

**Authoritative** for energy integration: `master_dt_ms`, NOT `latch_ms`.
See SPEC §7 LSI drift caveat — the chip's view of the period can drift
up to 20 % on PY32's uncalibrated LSI clock.

### `RbAmpTopology`

```cpp
enum class RbAmpTopology : uint8_t {
    Single      = 1,    // UI1 / I1
    SplitPhase  = 2,    // UI2 / I2
    ThreePhase  = 3,    // UI3 / I3
};
```

## Error codes (namespace `rbamp`)

Defined in `RbAmpRegisters.h`. Used as values of `lastError()`.

```cpp
namespace rbamp {
    constexpr int8_t RB_OK                  =  0;
    constexpr int8_t RB_ERR_IO              = -1;   // I2C transport failure
    constexpr int8_t RB_ERR_NACK            = -2;   // retry-exhausted NACK
    constexpr int8_t RB_ERR_TIMEOUT         = -3;   // bus timeout or wait_ready / commit-addr expired
    constexpr int8_t RB_ERR_NOT_READY       = -4;   // (reserved)
    constexpr int8_t RB_ERR_STALE           = -5;   // REG_V03_PERIOD_VALID == 0
    constexpr int8_t RB_ERR_PARAM           = -6;   // bad channel / address
    constexpr int8_t RB_ERR_MODE            = -7;   // operation requires develop mode
    constexpr int8_t RB_ERR_CHECKSUM        = -8;   // codegen parity mismatch (reserved)
    constexpr int8_t RB_ERR_VERSION         = -9;   // REG_VERSION returned 0 or 0xFF
    constexpr int8_t RB_ERR_NOT_IMPLEMENTED = -10;  // reserved for v2 features
    constexpr int8_t RB_ERR_NON_PHYSICAL    = -11;  // sanity-rejected float
}
```

See [Troubleshooting](10_troubleshooting.md#error-codes)
for a full diagnostic flow on each code.

## Compile-time configuration

Define BEFORE `#include <RbAmp.h>`:

```cpp
#define RBAMP_NACK_RETRY_ATTEMPTS 5    // override SPEC §B.5 default (3 on ESP32, 1 elsewhere)
#define RBAMP_NACK_RETRY_GAP_MS 5      // override default 5 ms
#include <RbAmp.h>
```

Used internally by `readU8()`. The defaults are:

```cpp
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
#  define RBAMP_NACK_RETRY_ATTEMPTS 3
#else
#  define RBAMP_NACK_RETRY_ATTEMPTS 1
#endif
#define RBAMP_NACK_RETRY_GAP_MS 5
```

See SPEC §B.5 for the full rationale.

## Settle-time constants

Defined in `RbAmpRegisters.h`. Library calls these internally — exposed
for advanced raw-API users who want to honour the same timing:

```cpp
constexpr uint16_t SETTLE_MS_LATCH_PERIOD  =  50;
constexpr uint16_t SETTLE_MS_SAVE_GAINS    = 700;
constexpr uint16_t SETTLE_MS_RESET         = 100;
constexpr uint16_t SETTLE_MS_FACTORY_RESET = 1500;
```

## Reference

- Doxygen-style header source: [`src/RbAmp.h`](../src/RbAmp.h)
- Wire-level register reference: [the rbAmp protocol spec](https://www.rbamp.com/docs/modules-basic-standard-api-reference)
- [Quickstart](05_quickstart.md) for first-use walkthrough
- [Troubleshooting](10_troubleshooting.md) for error-code diagnostic recipes

## Related — main rbAmp documentation

- [API Reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference) — formal I²C register / command / error spec the library wraps
- [Arduino Examples (raw I²C)](https://www.rbamp.com/docs/modules-basic-standard-arduino-examples) — same scenarios without the library, useful for porting
- [Period Metering](https://www.rbamp.com/docs/modules-basic-standard-period-metering) — atomic latch concept and master-side energy formula
- [Hardware Connection](https://www.rbamp.com/docs/modules-basic-standard-hardware-connection) — pinout, wiring, CT installation
- [Troubleshooting](https://www.rbamp.com/docs/modules-basic-standard-troubleshooting) — module-side issues (NACK, calibration drift, bus noise)


---

[← Cloud Integrations](08_cloud_integrations.md) | [Contents](README.md) | [Troubleshooting →](10_troubleshooting.md)
