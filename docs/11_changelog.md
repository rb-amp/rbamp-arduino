# 11 · Changelog

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
  against the chip's diagnostic `latch_ms` (the chip timer undercounts by
  ~25-30%). On a stale period anchor, integration is **held** — the next
  valid window is not undercounted.
- **CT model** codes follow the v1.3 per-class accepted set — `Sct013`
  accepts `{005, 010, 030, 050, 020}`; `SCT-013-100` and `-060` are
  recognised SKUs but **uncharacterised**, and are rejected client-side.
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
