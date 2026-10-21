# Changelog

All notable changes to this library are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/) and the project adheres to
[Semantic Versioning](https://semver.org/).

Public releases resume with this version; intermediate versions were released
internally only.

## 1.6.1 — STD micro-resolution energy, SCT-013 rating correction, sensor-config guard

Released together with 1.6.0 (the STD line) as the first public release of
the two-line library. In short:

- **STD line**: micro-resolution energy counters (µWh / µvarh), the
  starting current for energy, voltage phase angle and phase sequence — for
  modules whose image publishes them (capability bit 15, images 0x7A / 0x7B);
  older modules behave exactly as before.
- **BASIC line**: SCT-013 rating correction — on firmware up to 1.4.19 a
  channel switched to another SCT-013 model is read relative to the gain the
  module applies; firmware 1.4.20+ reports capability bit 12
  (`CAP_SCT_RATING_SELF`) and corrects itself. Sensor class / CT model
  setters read before they write (a stored configuration can be re-applied on
  every boot without touching the module's calibration); factory-paired
  classes are refused.
- **Both lines**: the measurement window / publish cadence is documented as
  100 ms; the channel count can never exceed the snapshot arrays; a host unit
  test suite (`extras/host_tests`) runs the real sources against a mock bus.

### Added
- **SCT-013 rating correction** (BASIC firmware 1.2 .. 1.4.19, sensor class
  SCT-013; libs/spec/SCT_RATING_SWITCH.md). Those modules are calibrated on the
  SCT-013-030 and after a restart apply that calibrated gain whatever model a
  channel shows. `begin()` now reads each channel's model, applied gain and
  noise floor and scales `readCurrent()` / `readCurrentPeak()` / `readPower()`
  / `readAll()` / `readPeriodAvgPower()` / `readPeriodMaxPower()` /
  `readPeriodSnapshot()` (and the energy derived from it) by
  preset(model) × c(model) / preset(030) relative to the applied gain; power
  factor and voltage are unchanged, plausibility bounds and
  `setEventThresholds()` are in true units. A channel that applies the raw
  preset (model switched in the running boot) makes `begin()` restart the
  module once (CMD_RESET, same UID, period re-primed); an applied gain more
  than 5 % from the calibration preset is left uncorrected with a warning.
  New: `sctRatingRefresh()`, `sctRating(ch, out)` (`RbAmpSctRating` with
  model / applied_gain / noise_floor / scale / status), `sctRatingScale(ch)`,
  `sctPresetGain(model)`. `readEventFlags()` re-reads the correction once per
  `EVENT_RESET_OCCURRED` (re-armed by `clearEventFlags()`). A module that
  reports CAPABILITY bit 12 (`CAP_SCT_RATING_SELF`, firmware 1.4.20+)
  corrects itself (scale 1, plain model write; the VERSION byte is not the
  gate), STD modules and the wired / built-in CT classes are never scaled. Host test `extras/host_tests/sct_rating_test.cpp` (124
  checks, negative control with the formula removed).
- **µWh / µvarh counters** in `RbAmpStdEnergy`: `act_import_uwh` /
  `act_export_uwh` / `act_net_uwh`, `rea_import_uvarh` / `rea_export_uvarh`,
  per-channel `ch_import_uwh[]` / `ch_export_uwh[]`, and `fine`. The micro
  fields are ALWAYS filled: the module's own counters when `fine` is true,
  otherwise derived from the coarse ones (mWh·1000, Wh·10⁶ below map 0x36,
  varh·10⁶) — so a caller can use them on any module. The grand micro block
  (40 B) and the per-channel block are each read in one transaction (one
  coherent module snapshot). Reactive µvarh is the module's counter from
  image 0x7B (derived from varh on 0x7A).
- `stdEnergyDelta()` covers the new fields with the same rules: generation
  change → no delta; uptime drop → `module_restarted`; a counter below the
  previous reading clamps to 0; net = import − export of the clamped deltas;
  `delta.fine` only when both readings were fine.
- **Starting current for energy** — `stdSetEnergyStartCurrent(ua)` /
  `stdReadEnergyStartCurrent(ua)` (image 0x7B+): below this channel current
  the module counts no energy for the channel; readings are not affected;
  default 400 µA. `0` = off, else 10..2540 µA in 10 µA steps — anything else
  is `RB_ERR_PARAM` with no bus write. Readback-verified, applies at once,
  persisted by `saveUserConfig()`. `RB_ERR_NOT_IMPLEMENTED` on modules
  without it.
- **Voltage phase angle** — `RbAmpStdVoltage::angle_deg` / `angle_valid`
  from `stdReadVoltage(v)`: degrees vs voltage 0 (image 0x7A+; invalid on
  older images, on an input below 50 V and on single-voltage modules for
  v > 0).
- **Phase sequence** — `stdReadPhaseSequence(RbAmpStdPhaseSeq&)`:
  `NotAvailable` / `Forward` / `Reverse` / `Abnormal`; `NotAvailable` without
  bus traffic on images that do not publish it.
- Host unit tests (`extras/host_tests/run.sh`): the real library sources
  against a synthetic register image — parsing, gating, one-transaction
  reads, delta rules, argument validation — on a 128-byte and a 32-byte Wire
  buffer, plus a negative control.
- `readSensorClass(uint8_t&)` — the module's current sensor class (both lines).
- `setCTModelChForce(channel, code)` — service path that sets a channel's
  model regardless of the module's class and current model. It replaces the
  calibration the unit carries for that channel with the model's generic
  preset; follow it with a calibration.
- `RbAmp::sensorModel(class, code)` / `RbAmp::sensorModelRatedA(class, code)`
  — pure registry lookups (no bus traffic). The generated registry entry now
  carries the model's current limits: `nominal_a` (rated RMS), `fs_a` (RMS at
  clip onset for a sine) and `peak_a` (instantaneous clip ceiling); 0 = not
  known for that model.
- Host test `extras/host_tests/sensor_guard_test.cpp` (BASIC 8-bit transport
  with a firmware side-effect model, and STD) with its own negative control.

### Changed
- `setCTModel()` (both forms) and `configureChannels()` on BASIC firmware up
  to 1.4.19 with the SCT-013 class restart the module after the save
  (CMD_RESET, wait for the same UID, period re-primed) so it applies the
  calibrated gain for the new model, then re-read the rating correction.
  One extra ~0.5 s per call; the read-before-write guard is unchanged.
- `begin()` caps the BASIC channel count at `RBAMP_MAX_CHANNELS` (as the STD
  path already did) so a `readPeriodSnapshot()` on a UI5 / UI7 can never run
  past the snapshot arrays when the library is built with a smaller
  `RBAMP_MAX_CHANNELS`; channels beyond the cap are not exposed (`RB_ERR_PARAM`).
  With the default arrays (7 wide) nothing changes. Host test
  `extras/host_tests/channel_overflow_test.cpp` reads 5/7-channel BASIC and
  7/9-channel STD modules into guard-word-wrapped snapshots, with the arrays
  at 7 and at 3, plus a negative control with the cap removed.
- **Sensor class / CT model setters read before they write.**
  `setSensorClass()`, `setCTModel()` (both forms) and `configureChannels()`
  no longer rewrite a class or model the module already holds: that is a
  no-op with no bus write and no flash cycle. On the module a class write
  clears every channel's CT model and calibration shape even when the value
  is unchanged, and a model bind re-applies the generic preset over the
  unit's own calibration even when the model is unchanged — so a host that
  re-applied its stored configuration on every reconnect used to destroy the
  factory calibration. A module of class `WiredCT` or `BuiltinCT` (shipped
  and calibrated together with its sensor) is not reconfigured: `false` with
  `RB_ERR_MODE` and no bus write; `configureChannels()` refuses a request to
  bind models under one of those classes before writing anything.
  `setCTModelChForce()` is the service path. Same behaviour on the STD line.
- `RB_ERR_MODE` text: "Not allowed in the module's current mode (factory
  mode / service path required)".
- Docs: the family measurement window / publish cadence is **100 ms** on both
  lines (BASIC since firmware v1.2 — all shipped batches; STD from image
  0x7C). Every statement of "200 ms" in the API docs, README and examples
  now says 100 ms, with 200 ms kept only as the note for older images (BASIC
  before v1.2, STD up to 0x7B). Example 06 samples real power every 100 ms
  (at 200 ms it skipped every second window). Energy is unaffected.
- The counter period path (`readPeriodSnapshot()` / `energy()` on modules
  with life-time counters) integrates the micro fields, i.e. the finest
  counters the module has.
- Doc note: `ST_CAL_OK` on image 0x7A+ comes from a marker written by the
  module's calibration save — a unit calibrated on an older image reads
  cal_ok = 0 until `saveGains()` runs once; readings are unaffected.

### Upgrade note
- The energy snapshot structure (`RbAmpStdEnergy`) gained fields in this
  version. If your application saves it as a raw binary blob (NVS, EEPROM,
  file), store only the fields you need in your own fixed format; a blob
  saved by an older version will not match the new size.

## 1.6.0 — STD line (16-bit register model)

One library, both product lines. `begin()` now detects whether the module
speaks the BASIC (8-bit pointer, float registers) or the STD (16-bit big-endian
pointer, fixed-point family map) register model and routes every call
internally — the method surface is the same on either line. STD support
covers discovery, real-time reads, the period/latch path, configuration /
commands and firmware update over I2C (the cross-platform
plan).

### Added
- **Line detection** — `line()` returns `RbAmpLine::Basic` / `RbAmpLine::Std`.
  The STD identity probe (one 16-bit burst of the identity block: magic, map
  version, capabilities, channel counts, variant) is issued FIRST in `begin()`,
  before any 8-bit transaction, and is benign on a BASIC module. Channel count
  on STD is discovery truth (`N_CURRENT`, capped at `RBAMP_MAX_CHANNELS`); the
  constructor topology hint is ignored there.
- **16-bit transport** — chunked burst reads/writes with an explicit pointer per
  chunk sized from the core's Wire buffer (128 bytes on ESP32 — a block in
  one transaction; 32 on the classic AVR buffer; `RBAMP_STD_BURST_MAX`
  overrides), same NACK-retry discipline as the 8-bit primitives.
- **Unified readers on STD** — `readVoltage(phase)` / `readVoltagePeak(phase)`
  (phase `0..n_voltage-1`), `readCurrent` / `readCurrentPeak` / `readPower` /
  `readPowerFactor` (ch `0..channels()-1`), `readFrequency()` (0.01 Hz
  resolution), `readAll()` (one burst per block), with the same per-field
  sanity fold as BASIC. New `readReactivePower(ch)` / `readApparentPower(ch)`
  (STD only; `RB_ERR_NOT_IMPLEMENTED` on BASIC).
- **STD block API** — `stdInfo()`, `stdReadChannel(ch)`, `stdReadTotals(k)`,
  `stdReadVoltage(v)`, `stdReadHealth()` and the structs `RbAmpStdInfo` /
  `RbAmpStdChannel` / `RbAmpStdVoltage` / `RbAmpStdHealth`.
- **Latch banks** — `stdArmBanks(mask)`, `stdLatch(bank)`, `stdReadLatch(bank)`,
  static `stdLatchWh(snap, dt_ms, ch)` and `RbAmpStdLatch`. The existing
  period path (`latchPeriod()` / `isPeriodValid()` / `readPeriodAvgPower()` /
  `readPeriodMaxPower()` / `readPeriodSnapshot()` / `energy()`) runs on STD via
  bank 0 (armed by `begin()`); the settle is raised to ≥ 300 ms there because
  the module services a latch at its next measurement-window boundary. On STD
  `latch_ms` is 0 (the module has no clock) and `master_dt_ms` is the period.
- `RbAmpVariant::U3I3` / `U3I6`; `readVariant()`, `readUid()`,
  `readLastError()`, `reset()`, `probe()`, `waitReady()` and
  `firmwareVersion()` route on STD. `readProductId()` reports the sensor family
  on an STD instance so a product-matched fleet scan adopts STD modules.
- **Configuration on STD** — `setSensorClass()`, `setCTModel()` (both forms),
  `configureChannels()`, `readCTModelCh()`, `saveUserConfig()` (SAVE_CONFIG),
  `saveGains()` (SAVE_CAL), `prepareAddressChange()` + `commitAddressChange()`
  (persisted address field → SAVE_CONFIG → RESET, same two-phase call sequence)
  and `readActiveAddress()` route on the STD line. Every command's verdict is
  read from the module's synchronous error register after its settle (map
  0x34+): a rejected write returns `RB_ERR_PARAM` and the module is unchanged.
  On STD the CT model is a reference label only (sets no gain; the module is
  calibrated per channel) — written to the per-channel map entry, read back,
  persisted; no channel-0 clobber side-effect there.
- **STD extras** — `stdReadConfig()` (`RbAmpStdConfig`), `stdSetHarmonics()`
  (RAM-only THD / displacement-PF switch, readback-verified, gated on the
  module's THD capability) and `stdCalThdFloor()` (no-load sensor-floor
  capture + persist).
- **Firmware update over I2C (two-phase OTA)** — `stdOtaBegin()` →
  `stdOtaWritePage()` ×N → `stdOtaCommit()` (verifies, arms the swap, no
  reboot) → `stdOtaActivate()` (reboots into the new image, waits ~4 s,
  rediscovers the module, confirms the running bank changed, then waits for
  the new image to confirm itself and checks the running version is the
  staged one). `stdCrc32()` helper. The bank size is read from the module so
  the same call serves every module family. Gated on map 0x33+ and the
  module's OTA capability. A rejected image reports `RB_ERR_CHECKSUM` (bad
  image) or `RB_ERR_VERSION` (older than the running one); the module keeps
  running the old firmware until activated. `stdOtaWriteImage()` (a
  host-built, unsigned image) exists for source compatibility only and
  refuses with `RB_ERR_NOT_IMPLEMENTED`: no current module accepts one.
- **Prepared (signed) bank images** — `stdOtaWritePreparedImage()` streams a
  bank image built on the manufacturer's build host exactly as-is (size must
  equal the module's bank size; page 0 first, blank pages skipped, the image's
  own header page last; no host-built header), then commits. Buffer, `Stream`
  and two-image (library picks the bank that is not running) forms. Helpers:
  `stdOtaActiveBank()`, `stdOtaStagedVersion()`, `stdOtaDiag()` (module's
  reason for a rejected image) and `stdOtaAbort()`. OTA commands are now
  waited on until the module reports them finished (pending / busy), so
  families that program pages in slots work with the same code; a page whose
  payload arrived corrupted is resent once. Measured on a three-phase module (
  100 kHz, 176 KB image, 303 non-blank pages): about 80 s to armed, and a
  wrong-signature image is refused at commit with the module's reason.
- `11_StdLine` example (one sketch, both lines).
- **Period source chosen from the module's capabilities (STD).** The first STD
  modules keep latch banks; the current firmware family keeps persisted
  life-time energy counters instead and has no banks at all. `begin()` now
  decides once (`RbAmpStdInfo::period_source`): with banks the period path is
  unchanged; with counters a period is the difference of two counter readings
  on the master's schedule (`readPeriodSnapshot()` routes onto it, `energy()`
  takes the module's own figures, per channel on map 0x36+), `latchPeriod()`
  is a no-op and the bank calls refuse without any bus traffic. The snapshot
  gained `avg_p_total`, `wh_net` and `module_restarted`; the accumulator an
  all-channel total (`RBAMP_ENERGY_CH_TOTAL`, `addWh()`) and `energyWh(ch)`
  answers NaN where the module publishes no per-channel energy.
- **Data-ready (DRDY) and events, both lines** — `setDrdyPin(pin)` (internal
  pull-up, falling-edge interrupt; the ISR only counts and time-stamps the
  edge), `waitDataReady()` / `readAllReady()` (wait for this module's next
  snapshot, confirmed by its sequence on a shared wired-OR line; a line held
  LOW 5 ms after the edge is an alarm: the masked event flags of every module
  on the line are read and cleared and reported through `RbAmpDrdyEvent`;
  polling fallback without a line), `readDataSeq()`, `beginEventFlags()`,
  `readEventMask()` / `setEventMask()`, `setEventThresholds()`;
  `readEventFlags()` / `clearEventFlags()` / `hasError()` now work on STD map
  0x36+ (image 0x75+) with the same bit layout as BASIC v1.3.
- **STD map 0x36 readers** — `stdReadEnergy()` + `stdEnergyDelta()` (life-time
  counters, mWh and per-channel on 0x36+), per-phase `stdReadTotals(k)` on
  single-phase modules too, `stdIdentify()` / static `stdProbe()` /
  `stdReadImageVersion()` / `stdReadBoardId()` (`RbAmpStdIdentity`),
  `stdSetTime()` / `stdReadTime()` (the module clock is lost at every module
  restart), history and event log (`stdHistInfo()`, `stdHistRead()`,
  `stdHistReadRaw()`, `stdEventRead()`, `stdHistFlush()` with
  `RbAmpStdHistRecord` / `RbAmpStdEvent` decoded from the generated record
  layouts), `stdReadHealth()` extended (clock / storage / frame-loss
  diagnostics, uptime, and on image 0x76+ the reset cause and boot counter),
  a generic `stdCommand()` / `stdWaitIdle()` / `stdLocate()` with the module's
  command-completion handshake and full verdict mapping, static
  `stdSetAddressByUid()` (commission one module among several on a shared
  address), and `stdOtaBoardOk()` (refuse an image built for another board
  before sending it).
- Per-channel CT model writes refuse on STD map 0x35+ without the module's
  per-channel-persist capability (no traffic, `RB_ERR_NOT_IMPLEMENTED`).

### Changed
- The per-channel scalar routing is now one table + one router (`readQty`),
  covering BASIC flat / BASIC window / STD in a single decision point.
- `RbAmpSnapshot::channels` is documented as `1..RBAMP_MAX_CHANNELS`.
- `errorString(RB_ERR_CHECKSUM)` now reads "Value check failed — checksum /
  signature / range" (was a leftover "Codegen parity mismatch"); the code
  also carries the module's value-check verdict on a command (rejected image,
  payload CRC, epoch out of range, empty history ring).
- `reset()` on an STD module waits quietly (up to 3 s) for the module to
  answer again before restarting the period path: the NACKs of an expected
  reboot no longer reach the log or the retry diagnostics.
- **Command completion on STD is the module's command state, not a sleep.**
  Every command (`saveGains()`, `saveUserConfig()`, `stdCalThdFloor()`,
  `stdCommand()`, the clock / history / identify calls and all OTA steps)
  now waits until the module is idle, writes the opcode, polls the
  command-state register until the module reports the command finished and
  only then reads the verdict — the generated settle no longer sleeps (it
  only caps the first poll), timeouts are at least 1 s (saves 2 s, a page
  5 s, commit 3 s or more). A save inside the module's 5 s store rate limit
  is retried once after 5 s. A command write the module did not acknowledge
  is only repeated when the module provably did not take it (command state
  idle and the completed-command counter unchanged) — never blindly, so a
  command cannot run twice or be dropped as an overrun. Modules without a
  command-state register (map < 0x35) keep the settle-based path.
- `commitAddressChange()` on STD waits for SAVE_CONFIG to finish and succeed
  before RESET, and switches the instance only once the module answers at
  the new address; `reset()` waits for any running command first.
- `stdOtaWritePage()` requires the session to still be open after the page
  (`RB_ERR_NOT_READY` / `RB_ERR_IO` otherwise, with the module's reason in
  the log) — a page the module dropped no longer passes silently.
- Multi-byte blocks (energy counters, a channel block, totals, a latch bank,
  a history record) are read in one transaction where the Wire buffer
  allows — the module serves such a read from one coherent snapshot, so an
  i64 counter can no longer tear across a publish; on a 32-byte buffer a
  split block is re-read until two passes agree.
- `stdReadTotals()` reports 0 for the block's energy accumulators on modules
  with life-time counters (the module does not keep them there; energy is
  `stdReadEnergy()`).
- OTA rejection reason 15 is named (BOARD) in the log; the generated register
  header follows the spec (STATUS bit 3 is reserved; the Modbus function /
  alias leaves that the firmware does not implement are gone).

### Fixed
- **Latch energy no longer freezes after a module reset (STD).** The latch
  arm mask lives in module RAM and is cleared by a warm reset, power cycle or
  firmware activation; a host that armed it only once then reads empty
  snapshots forever. `readPeriodSnapshot()` and `stdReadLatch()` now detect an
  empty snapshot on a disarmed bank, log a warning, re-arm and primer-latch
  that bank, and drop that one period (the outage carries no data; the master
  clock is re-anchored so nothing is estimated over it). Each armed bank
  reports the reset once on its own next read. The library also restarts its
  period path after its own `reset()` / address change, and drops a period
  during which the module's frame counter went backwards (a reboot inside the
  period that a re-armed bank would otherwise hide). Same host-side contract
  across all rbAmp libraries.

### Behaviour on an STD instance
- Every BASIC-only call (`factoryReset` — no STD counterpart, General-Call
  and label/event helpers, `readCapability`, `readCommitSeq`,
  `readPeriodLatchMs`) fails with `RB_ERR_NOT_IMPLEMENTED` — the 8-bit
  primitives refuse structurally, so a wrong-register transaction can never
  reach an STD module.
- No BASIC API change: `RbAmpSnapshot` is unchanged; `RbAmpPeriodSnapshot`
  only gained fields at its end (`avg_p_total`, `wh_net`,
  `module_restarted`); the new STD structs are additive.

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
