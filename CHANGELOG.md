# Changelog

All notable changes to this library are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/) and the project adheres to
[Semantic Versioning](https://semver.org/).

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
- **Address change** is a production-OK two-phase magic commit — no special/factory
  mode required.
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
