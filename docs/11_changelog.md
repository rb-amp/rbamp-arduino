# Changelog

Per-release notes for the `RbAmp` Arduino library. Library SemVer is
independent of the rbAmp protocol version — see the rbAmp protocol spec CHANGELOG_PROTOCOL.md (see [API Reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference))
for the wire-protocol history (the protocol bumped from 1.0 to 1.1 on
2026-05-24 — additive, fully backward-compatible).

## v1.0.0 — 2026-05-25 (initial release)

First public release of the `RbAmp` Arduino library. Implements
the canonical API surface from the rbAmp protocol spec §12 for **protocol v1.0**.

### Features

- **`RbAmp` class** — one instance per slave device, public methods for
  every metering operation:
  - Lifecycle: `begin()`, `probe()`, `waitReady()`, `firmwareVersion()`,
    `topology()`, `channels()`, `hasVoltageHw()`, `address()`,
    `rawTopology()`
  - Real-time reads (200 ms refresh): `readVoltage()`, `readVoltagePeak()`,
    `readCurrent(ch)`, `readCurrentPeak(ch)`, `readPower(ch)`,
    `readPowerFactor(ch)`, `readFrequency()`, `readAll(&snapshot)`
  - Period metering: `latchPeriod()`, `isPeriodValid()`,
    `readPeriodAvgPower(ch)`, `readPeriodMaxPower()`,
    `readPeriodLatchMs()`, `readPeriodSnapshot(&snap, settle_ms, skip_latch)`
  - Configuration: `setCTModel(code)`, `saveGains()`,
    `prepareAddressChange(addr)`, `commitAddressChange()`,
    `factoryReset()`, `reset()`
  - Multi-module: `RbAmp::broadcastLatch(bus)` (returns `false` on v1
    firmware per SPEC §9 — General-Call disabled)
  - Diagnostics: `lastError()`, `errorString(code)`, `setLogStream(&Serial)`,
    `retryExhaustionCount()`, `sanityRejectCount()`, `resetCounters()`

- **`RbAmpEnergy` per-channel Wh accumulator** — owned by each `RbAmp`
  instance, exposed via `dev.energy()`. Updated automatically by
  `readPeriodSnapshot()`. Signed (negative = net export). Opt-out via
  `dev.energy().disable()`.

- **`RbAmpSnapshot` / `RbAmpPeriodSnapshot` POD structs** — readAll() and
  period snapshot result containers.

- **`RbAmpTopology` enum** — `Single` / `SplitPhase` / `ThreePhase` for
  the variant hint.

- **SPEC §B.5 retry+sanity discipline** built in:
  - Per-byte retry on ESP32 targets (default 3 attempts × 5 ms gap)
  - Configurable via `#define RBAMP_NACK_RETRY_ATTEMPTS / _GAP_MS`
    before `#include <RbAmp.h>`
  - Loose sanity filter on float reads (`!isfinite(x) || fabsf(x) > 10000`)
  - Diagnostic counters (`retryExhaustionCount()` / `sanityRejectCount()`)
  - Auto-disabled on non-ESP32 platforms (single attempt default)

- **v1.1 forward-readiness**:
  - `dev.rawTopology()` reads `REG_TOPOLOGY` (0x24) directly. Returns
    `1`/`2`/`3` on v1.1 firmware, `0x00` on v1.0 (unmapped), `0xFF`
    on I2C failure.
  - `dev.setCTModel(code)` automatically benefits from v1.1 firmware's
    preset NF + GAIN auto-load — no API change needed.

- **Two-step address-change protocol** (SPEC §10) — `prepareAddressChange()`
  arms, `commitAddressChange()` must follow within 5 s. Library
  validates `REG_MODE == develop` and the 5 s window. Internal handle
  address updates automatically on successful commit.

### Supported platforms

- Arduino AVR (Uno / Mega / Nano) — 32-bit `float` energy accumulator
- arduino-esp32 v3.x (ESP32 / S2 / S3 / C3) — `RBAMP_NACK_RETRY_ATTEMPTS=3` default
- arduino-esp8266 — single attempt default, no NACK pattern
- STM32duino (F1 / F4 / G4) — single attempt default
- SAMD / RP2040 (arduino-pico) — lightly tested, expected to work

### Examples (7 bundled sketches)

- `01_QuickRead` — smoke test: U/I/P/PF per second
- `02_PeriodEnergyOLED` — 60-second Wh meter on SSD1306 OLED
- `03_MultiModuleBroadcast` — 3 modules, per-device sequential LATCH
- `04_UI3PerChannelMQTT` — UI3 + MQTT per-channel publish
- `05_AddressChange` — two-step I2C address reassignment
- `06_BidirectionalEnergy` — master-side consume / export split
- `07_DeepSleepLogger` — ESP32 deep-sleep with RTC-memory Wh

Plus `../examples/SoakMonitor` (12-hour autonomous bench monitor for
v1.1 firmware validation).

### Documentation

11 reference documents under [`docs/`](.) covering installation,
hardware, tiers, sensor selection, examples, DIY + cloud integrations,
API reference, troubleshooting, and changelog.

### Known limitations

These are deliberate omissions from v1.0 — tracked for v1.x or v2:

- `broadcastLatch()` always returns `false` (v1 firmware has
  General-Call disabled — SPEC §9). v2 firmware will enable it; this
  library will return `true` automatically once the wire transaction
  succeeds.
- No explicit `setNoiseFloor()` / `setGain()` helpers — these
  registers fall under the calibration namespace (out of scope for v1
  client libraries). For bench tuning use raw `Wire` writes per
  [Sensor Selection](03_sensor_selection.md#raw-nf-write-until-library-adds-a-helper).
- No reactive-power read — RESERVED for v2 (STANDARD / PRO tier feature,
  not exposed in protocol v1).
- No dimmer control (`REG_DIM0_*` 0x10..0x18 untouched). Out of scope
  for v1 — see future `RbAmpDimmer` companion library.

### Wire-protocol compatibility matrix

| Library v | Firmware v | Behaviour |
|---|---|---|
| 1.0 | 1.0 | Library uses constructor topology hint. Fully functional. |
| 1.0 | 1.1 | Library uses constructor hint; REG_TOPOLOGY ignored. Functionally identical to 1.0/1.0. CT_MODEL preset auto-load works via existing `setCTModel()` call. |
| 1.1 (planned) | 1.0 | Library reads REG_TOPOLOGY = 0x00, falls back to hint. Identical to 1.0/1.0. |
| 1.1 (planned) | 1.1 | Library reads REG_TOPOLOGY ∈ {1,2,3}, uses it as authoritative. Constructor hint silently ignored. |

### Bench validation

Phase 1 closed 2026-05-24 (root commit `4b16cbb`) — reference master for
the v1.0 firmware validation. Calibration baseline: NF=6, GAIN=2.1094
on the reference SCT-013-5A bench. Final acceptance #4 (100-paired-sample
capture via CalibSampler @ 4 Hz):

| Quantity | DUT | METER | Ratio | Spec | Status |
|---|---|---|---|---|---|
| U (V) | 225.30 | 225.24 | 1.0003 | ±2% | ✓ |
| I (A) | 0.793 | 0.786 | 1.0090 | ±2% | ✓ |
| P (W) | 111.05 | 114.17 | 0.9727 | ±2% | ⚠ borderline -2.7% |
| PF | 0.620 | 0.643 | 0.964 | ±2% | ⚠ -3.6% |

P/PF marginally outside ±2% due to SMPS load flicker (mean-of-100
converges to ~1 %; a stable resistive reference would close the gap).
See the rbAmp protocol spec PHASE1_LESSONS_LEARNED (see [API Reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference)) §2.5.

Additional 12-hour SoakMonitor validation (`ard-amp-log.csv`, 145 cycles):
100 % `period_valid`, 1 retry exhaust, 0 sanity reject.

## Future releases — planned

### v1.0.x (patch — bug fixes only)

- TBD per user reports

### v1.1.0 (minor — additive, when v1.1 firmware ships)

- `dev.begin()` reads `REG_TOPOLOGY` when `firmwareVersion() >= 0x02`
  and uses it as authoritative (constructor hint becomes fallback only).
- `dev.setNoiseFloor(ch, value)` / `dev.setGain(ch, value)` helper
  methods (deprecates raw `Wire` writes from chapter 03).
- Optional: `RBAMP_NACK_RETRY_ATTEMPTS=1` becomes the default on ESP32
  if firmware version ≥ 0x02 detected at `begin()` (since the slave-side
  NACK fix removes the need for retry).

### v2.0.0 (major — breaking, when v2 firmware ships)

- `broadcastLatch()` actually transmits via I2C General-Call (when
  PY32 v2 firmware enables GC).
- Reactive-power register exposed: `readReactivePower(ch)`.
- Dimmer control exposed: `setDimmer(level)`, `getDimmer()`.
- Possible breaking: switch single-byte reads to burst reads on the
  contiguous V03 float block (0x86..0xBD) for ~4× speedup — this is
  *technically* a change but would not break user code (the public API
  stays the same).

### v2.1+ (TBD)

- Per-channel NF / GAIN registers (firmware v2.1 — for mixed-CT UI3
  installations).
- Calibration protocol exposure (currently RESERVED for v2 in SPEC).

## Reporting issues + contributing

Open issues at [github.com/rb-amp/rbamp-arduino/issues](https://github.com/rb-amp/rbamp-arduino/issues)
with the diagnostic info checklist from [Troubleshooting](10_troubleshooting.md#when-to-escalate).

Pull requests welcome — the library is in pure standard-Arduino C++,
no platform-specific dependencies. Run the conformance test in
`extras/conformance/` against a real DUT before submitting (PR template
will guide).

## License

MIT — see [LICENSE](../LICENSE).

## Related — main rbAmp documentation

- [API Reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference) — formal I²C register / command / error spec the library wraps
- [Arduino Examples (raw I²C)](https://www.rbamp.com/docs/modules-basic-standard-arduino-examples) — same scenarios without the library, useful for porting
- [Period Metering](https://www.rbamp.com/docs/modules-basic-standard-period-metering) — atomic latch concept and master-side energy formula
- [Hardware Connection](https://www.rbamp.com/docs/modules-basic-standard-hardware-connection) — pinout, wiring, CT installation
- [Troubleshooting](https://www.rbamp.com/docs/modules-basic-standard-troubleshooting) — module-side issues (NACK, calibration drift, bus noise)


---

[← Troubleshooting](10_troubleshooting.md) | [Contents](README.md)
