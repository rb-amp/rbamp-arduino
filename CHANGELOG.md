# Changelog

All notable changes to this library are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/) and the project adheres to
[Semantic Versioning](https://semver.org/).

## 1.5.0 — Per-channel CT models + sensor-model registry

Adds per-channel CT-model assignment for every channel (including senior-SKU
channels 3+) via the channel window (field 15), and moves the CT-model list off
a hand-maintained enum onto the generated registry. Firmware: v1.4.18 (fw ver
0x0A), where window field 15 is signed on silicon.

### Added
- **Per-channel CT models on channels 3+** — `setCTModel(channel, code)` and
  `configureChannels()` now bind any channel `0..channels()-1`. Channels 0-2 use
  the flat `CMD_SET_CT_MODEL_CHn` path; channels 3+ bind through the window
  (field 15), which the firmware applies immediately (NF+GAIN preset). Every
  window write is confirmed by a read-back; a model the module does not accept
  is surfaced as `RB_ERR_PARAM` and the channel keeps its previous model.
- **`readCTModelCh()` covers all channels** — channels 3+ read the applied model
  from the window; the flat mirrors `0x51-0x53` still serve ch0-2.
- **Generated sensor-model registry** — `RbAmpSensorModels.h` (generated from
  `libs/spec/sensor_models.yaml`): `RBAMP_CT_*` code defines for both classes
  (SCT-013 + WIRED_CT 1..13), human descriptors, per-model status, and
  `rbamp_sensor_model_lookup()`. Use the `RBAMP_CT_*` defines to name a model.
- `10_PerChannelModels` example.

### Changed
- **CT-model validation is registry-driven** — the client no longer keeps a
  per-class accept-list (it had drifted: WIRED_CT was absent entirely, and the
  new clamp models 6-9 plus reserved 10-13 would have been rejected). The client
  now only fast-fails codes absent from the registry; **acceptance is runtime
  truth** — the module returns `RB_ERR_PARAM` for a code without a preset row.
- `RbAmpCTModel` enum is now a frozen backward-compat alias for the seven
  SCT-013 codes; its values are sourced from the generated `RBAMP_CT_SCT013_*`
  defines so they cannot drift. New code (and all WIRED_CT models) uses the
  generated defines directly.
- The `code > 7` hard caps in `setCTModel()` / `configureChannels()` are gone —
  the valid range is whatever the registry defines.
- Internal: the ch<3 / ch3+ routing for the RT metrics (I_RMS / I_PEAK / P_REAL
  / PF) is consolidated into a single `readChannelMetric()` decision point.

### Fixed
- **CT model / sensor class now persist in production.** `setCTModel()`,
  `configureChannels()` and `setSensorClass()` persisted via `CMD_SAVE_GAINS`,
  which is factory-gated — on a production module the save was a silent
  no-op, so the binding applied to RAM but was lost on the next reboot. These
  registers are `user_config`, so they now persist via `CMD_SAVE_USER_CONFIG`
  (ungated). No API change; same ~700 ms settle.

## 1.4.0 — Senior SKUs (UI5 / UI7)

Adds support for the 5- and 7-channel senior SKUs (UI5, UI7). Hardware-verified
on silicon (UI7 board, bench, 4/4 PASS). Cross-platform-aligned with the
STM32 HAL / ESP-IDF / Python bundle.

### Added
- **Senior SKU detection** — `begin()` now reads the channel count from
  `REG_TOPOLOGY` (0x24, values 1/2/3/5/7) and the SKU + voltage presence from
  `REG_HW_VARIANT` (0x55): `UI5` and `UI7` added to `RbAmpVariant`; `Five` /
  `Seven` added to `RbAmpTopology`.
- **Channel access window** — `readCurrent()` / `readCurrentPeak()` /
  `readPower()` / `readPowerFactor()` now accept `ch = 0..channels()-1`.
  Channels 0-2 use the flat block; channels 3+ are read transparently through
  the device's channel window (re-selected on every read). No new public read
  methods — the existing per-channel calls just extend.
- **`readCommitSeq()`** — reads the digest commit sequence byte; increments once
  per RT commit (~20 ms), for detecting that the device advanced a sample
  between two reads. Change flag only (there is no CRC in the digest).
- Period energy now covers all channels: `readPeriodSnapshot()` fills `avg_p[]`
  for every channel (ch3+ via the window's latched period field) and
  `energy().wh(ch)` accumulates per channel.
- `09_SeniorSku` example.

### Changed
- **`RBAMP_MAX_CHANNELS` (=7)** now sizes the per-channel arrays in
  `RbAmpSnapshot` (`current` / `current_peak` / `power` / `power_factor`),
  `RbAmpPeriodSnapshot.avg_p`, and the Wh accumulator (was hardcoded 3).
  ⚠ **Source-recompile note:** the snapshot structs grew — recompile any code
  that includes this library against the new headers. Wire protocol and the
  method surface for junior SKUs are unchanged.

## 1.3.0 — Fleet + v1.3 protocol

Aligns the Arduino library with the v1.3 rbAmp wire contract and the
cross-platform reference (STM32 HAL / ESP-IDF / Python). Hardware-validated on a
heterogeneous fleet (UI1 + I2 + I3) through the public API with raw-register
ground-truth (38/38 full-coverage suite, plus an extended app-WDT soak).

### Added
- **`RbAmpFleet`** multi-module manager: bus `scan()` (with collision
  detection), batched `pollAll()`, fleet-wide `totalPower()` / `totalEnergyWh()`
  / `pollErrors()`, General-Call sync (`enableGcAll()` / `gcLatch()` /
  `checkSync()`), `assignAddress()`, `checkConflict()`, and `provision()` for
  bringing a factory-fresh module onto the bus.
- **Identity / capability**: `readVariant()`, `readCapability()`,
  `readProductId()`, `readUid()`.
- **Event channel**: `readEventFlags()`, `clearEventFlags()`, `hasError()`,
  `clearError()`, `readLastError()`.
- **Per-channel CT configuration**: `configureChannels()` (batched, one flash
  save) and `readCTModelCh()` (applied-model mirror read).
- **Fleet primitives on the device**: `enableGc()`, `setGroupId()`,
  `readGroupId()`, `readGcTick()`, `readFleetConfig()`, `readLabel()`,
  `writeLabel()`, `saveUserConfig()`, static `broadcastLatchGroup()`.
- `08_FleetSync` example (scan → GC sync → aggregate).
- `RbAmpSnapshot.implausible` per-field mask: a field that fails the physical
  sanity filter is set `NaN` and flagged, leaving the rest of the snapshot
  usable (only a transport failure fails the whole read).

### Changed
- **Energy** integrates over the master's wall-clock, never the chip's
  diagnostic `latch_ms` (the chip timer under-counts ~25-30%). On a stale period
  the integration anchor is held so the next valid window is not under-counted.
- **CT model** codes follow the v1.3 per-class accepted set — `Sct013` accepts
  `{005, 010, 030, 050, 020}`; `SCT-013-100` and `-060` are recognised SKUs but
  uncharacterised and rejected client-side. `REG_CT_MODEL` is pure staging:
  binding is via the per-channel command, so multi-channel binds are
  order-independent and never clobber channel 0.
- **Address change** is a production-OK two-phase magic commit — no factory mode
  required.
- **Variant detection** reads `REG_HW_VARIANT`; the constructor topology hint is
  now a fallback for pre-v1.3 firmware. The default hint is `Single` (safe — no
  spurious polls of absent channels).
- I2C writes now share the same NACK-retry discipline as reads (a silently
  dropped config write on a contended bus is otherwise invisible).
- `setSensorClass()` rejects out-of-enum classes client-side (the firmware
  silently accepts a bad class, so the client guard is the only defense).

### Notes
- **AVR**: `double` is 32-bit (== `float`) on the classic AVR toolchain, so the
  Wh accumulator loses precision on long soak logging — reset periodically or
  prefer a 32/64-bit-`double` core (ESP32, SAMD, STM32, RP2040).
- **Marginal bus / ESP32**: Arduino-ESP32's `Wire` wraps a driver that can spin
  on a held bus below the library. Use proper external ~4.7 kΩ pull-ups, avoid a
  debugger/NRST in production, and arm an app-level task watchdog on the polling
  task as the recovery path.

## 1.0.0 — Initial release

Single-device real-time metering (RMS U / I / P / PF / frequency), period energy
(Wh) integration, CT-model + sensor-class configuration, and I2C address change.
