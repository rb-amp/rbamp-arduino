# 11 · Changelog

The full, detailed history is in [`CHANGELOG.md`](../CHANGELOG.md) at the
repository root; this chapter keeps the short version.

## 1.6.1 — STD micro-resolution energy, SCT-013 rating correction, sensor-config guard

Released together with 1.6.0 (the STD line) as the first public release of
the two-line library.

- **STD line**: micro-resolution energy counters (µWh / µvarh), the starting
  current for energy (`stdSetEnergyStartCurrent()` / `stdReadEnergyStartCurrent()`),
  voltage phase angle (`RbAmpStdVoltage::angle_deg`) and phase sequence
  (`stdReadPhaseSequence()`) — for modules whose image publishes them (image
  `0x0001007B` or later); older modules behave exactly as before. `stdEnergyDelta()`
  covers the new fields.
- **BASIC line — SCT-013 rating correction**: on firmware up to 1.4.19 a
  channel switched to another SCT-013 model is read relative to the gain the
  module applies. `begin()` reads each channel and scales current / power /
  period power (and the energy derived from it); power factor and voltage are
  unchanged. New: `sctRatingRefresh()`, `sctRating()`, `sctRatingScale()`,
  `sctPresetGain()`. Firmware 1.4.20 and later corrects itself. The model
  setters restart the module after the save on firmware that needs it (one
  extra ~0.5 s per call).
- **Sensor class / CT model setters read before they write**
  (`setSensorClass()`, `setCTModel()`, `configureChannels()`): a stored
  configuration can be re-applied on every boot without touching the module's
  calibration. A module of class `WiredCT` / `BuiltinCT` is not reconfigured
  (`RB_ERR_MODE`); `setCTModelChForce()` is the service path. New:
  `readSensorClass()`, `RbAmp::sensorModel()` / `RbAmp::sensorModelRatedA()`
  (registry lookups with the model's current limits).
- SCT-013-060 and -100 are accepted (they were refused client-side as reserved); sensor-model clip limits updated to measured values.
- **Both lines**: the measurement window / publish cadence is documented as
  100 ms (200 ms kept only as a note for older images); the channel count can
  never exceed `RBAMP_MAX_CHANNELS` (channels beyond it return
  `RB_ERR_PARAM`); host unit tests (`extras/host_tests`) run the real sources
  against a mock bus.
- `RbAmpStdEnergy` gained fields in this version: if your application saves
  it as a raw binary blob, store only the fields you need in your own fixed
  format.

## 1.6.0 — STD line (16-bit register model)

One library, both product lines: `begin()` detects whether the module speaks
the BASIC (8-bit) or the STD (16-bit, fixed-point) register model and routes
every call internally — the method surface is the same on either line.

- `line()` (`RbAmpLine::Basic` / `RbAmpLine::Std`); unified readers on STD
  (`readVoltage(phase)`, `readCurrent()`, `readPower()` …) plus new
  `readReactivePower()` / `readApparentPower()` (STD only).
- The STD block API: `stdInfo()`, `stdReadChannel()`, `stdReadTotals()`,
  `stdReadVoltage()`, `stdReadHealth()`, `stdReadConfig()`,
  `stdSetHarmonics()`, `stdCalThdFloor()`; latch banks (`stdArmBanks()`,
  `stdLatch()`, `stdReadLatch()`, `stdLatchWh()`); life-time counters
  (`stdReadEnergy()`, `stdEnergyDelta()`).
- Period source chosen from the module's capabilities (latch banks or
  persisted counters); latch energy survives a module reset; the period
  snapshot gained `avg_p_total`, `wh_net`, `module_restarted`.
- Identity, clock, history: `stdIdentify()`, `RbAmp::stdProbe()`,
  `stdReadImageVersion()`, `stdReadBoardId()`, `stdSetTime()` / `stdReadTime()`,
  `stdHistInfo()` / `stdHistRead()` / `stdHistReadRaw()` / `stdEventRead()` /
  `stdHistFlush()`, `stdCommand()` / `stdWaitIdle()` / `stdLocate()`,
  `RbAmp::stdSetAddressByUid()`.
- Configuration and commands work on STD (`setSensorClass()`, `setCTModel()`,
  `configureChannels()`, `saveUserConfig()`, `saveGains()`, address change);
  every command waits for the module to report it finished and checks its
  verdict. On STD the CT model is a reference label only.
- Firmware update over I²C: `stdOtaBegin()` → `stdOtaWritePage()` →
  `stdOtaCommit()` → `stdOtaActivate()`, `stdOtaWritePreparedImage()` (images
  prepared by the manufacturer), `stdOtaDiag()`, `stdOtaAbort()`,
  `stdOtaBoardOk()`.
- Data-ready line and events on both lines: `setDrdyPin()`, `waitDataReady()`,
  `readAllReady()`, `readDataSeq()`, `beginEventFlags()`, `setEventMask()` /
  `readEventMask()`, `setEventThresholds()`; `readEventFlags()` /
  `clearEventFlags()` / `hasError()` work on STD modules that provide the
  event block.
- BASIC-only calls (`factoryReset()`, General-Call and label helpers,
  `readCapability()`, `readCommitSeq()`, `readPeriodLatchMs()`) return
  `RB_ERR_NOT_IMPLEMENTED` on an STD instance. No BASIC API change.
- `11_StdLine` example.

## 1.5.0 — Per-channel CT models + sensor-model registry

- `setCTModel(channel, code)` / `configureChannels()` bind any channel,
  including senior-SKU channels 3+ (through the channel window); every write
  is confirmed by a read-back, and a model the module does not accept is
  reported as `RB_ERR_PARAM` (the channel keeps its previous model).
- `readCTModelCh()` covers all channels.
- Generated sensor-model registry `RbAmpSensorModels.h` (`RBAMP_CT_*` defines,
  descriptors, `rbamp_sensor_model_lookup()`); CT-model validation is
  registry-driven (the client only rejects codes absent from the registry —
  acceptance is decided by the module). `RbAmpCTModel` is a frozen alias for the
  seven SCT-013 codes.
- CT model / sensor class now persist on production modules
  (`CMD_SAVE_USER_CONFIG`).
- `10_PerChannelModels` example.

## 1.4.0 — Senior-SKU support (UI5/UI7)

Adds support for the 7-channel senior-SKU modules (UI5, UI7) and
refreshes the GC broadcast example. Silicon-verified on a heterogeneous
fleet (UI1/UI2/UI3/UI5/UI7), firmware contract bd2ad8e.

### Added

- **`09_SeniorSku` example** — 7-channel polling + energy integration
  for UI5/UI7 modules.
- **`readCommitSeq()`** — firmware commit-sequence readback.

### Changed

- **`RBAMP_MAX_CHANNELS`** raised from 3 to 7. Existing sketches that
  use `RbAmpSnapshot` arrays will grow — **recompile required** (no
  source changes needed unless you hard-coded array sizes).
- **`03_MultiModuleBroadcast`** rewritten for v1.3 General-Call
  broadcast latch (coherent single-frame sync replaces the old
  per-device sequential latch).
- Regenerated register headers (v1.4.0 contract).
- Energy/Snapshot: senior-SKU array growth + channel-count dispatch.

## 1.3.0 — Fleet + v1.3 protocol

Brought the Arduino library into line with the rbAmp v1.3 wire contract
and the cross-platform reference model (STM32 HAL / ESP-IDF / Python).
HW-validated on a heterogeneous fleet (UI1 + I2 + I3) through the public
API with raw-register ground-truth (38/38 full-coverage suite + extended
app-WDT soak).

### Added

- **`RbAmpFleet`** — multi-module manager: bus `scan()` (with collision
  detection), batched `pollAll()`, fleet-wide `totalPower()` /
  `totalEnergyWh()` / `pollErrors()`, General-Call sync (`enableGcAll()` /
  `gcLatch()` / `checkSync()`), `assignAddress()`, `checkConflict()`,
  and `provision()` for bringing a factory-fresh module onto the bus.
- **Identity / capability**: `readVariant()`, `readCapability()`,
  `readProductId()`, `readUid()`.
- **Event channel**: `readEventFlags()`, `clearEventFlags()`, `hasError()`,
  `clearError()`, `readLastError()`.
- **Per-channel CT configuration**: `configureChannels()` (batched, one
  flash save) and `readCTModelCh()` (applied-model mirror read).
- **On-device fleet primitives**: `enableGc()`, `setGroupId()`,
  `readGroupId()`, `readGcTick()`, `readFleetConfig()`, `readLabel()`,
  `writeLabel()`, `saveUserConfig()`, static `broadcastLatchGroup()`.
- `08_FleetSync` example (scan → GC sync → aggregation).
- `RbAmpSnapshot.implausible` per-field mask: a field that fails the
  physical-sanity filter is set to `NaN` and flagged, while the rest of the
  snapshot stays usable (the full read fails only on a transport failure).

### Changed

- **Energy** is integrated against the master wall-clock, **never**
  against the chip's diagnostic `latch_ms` (a module diagnostic timer, not
  for energy). On a stale period anchor, integration is **held** — the next
  valid window is not undercounted.
- **CT model** codes follow the v1.3 per-class accepted set — `Sct013`
  accepts `{005, 010, 030, 050, 100, 020, 060}`.
  `REG_CT_MODEL` is pure staging: bind via the per-channel command,
  multi-channel binds are **order-independent**, and never clobber
  channel 0.
- **Address change** is a production-OK two-phase magic commit; factory
  mode is **not required**.
- **Variant detection** reads `REG_HW_VARIANT`; the constructor topology
  hint is now a fallback for pre-v1.3 firmware. The default hint is
  `Single` (safe — no spurious readings from absent channels).
- I²C writes now share the same NACK-retry discipline as reads (a config
  write that silently drops on a contended bus is otherwise invisible).
- `setSensorClass()` rejects out-of-enum classes client-side (firmware
  silently accepts a bad class, so the client guard is the only safeguard).

### Notes

- **AVR**: `double` is 32-bit (== `float`) on the classic AVR toolchain,
  so the Wh accumulator loses precision during long-soak logging — reset it
  periodically or use a 32/64-bit-`double` core (ESP32, SAMD, STM32,
  RP2040).
- **Marginal bus / ESP32**: the Arduino-ESP32 `Wire` wraps a driver that
  can hang on a held bus below the library level. Use proper external
  ~4.7 kΩ pull-ups, avoid the debugger/NRST in production, and arm an
  app-level task-watchdog on the polling task as a recovery path.

## 1.0.0 — Initial release

Single-device RT metering (RMS U / I / P / PF / frequency), period
energy (Wh) integration, CT-model + sensor-class configuration, and
I²C-address change.
