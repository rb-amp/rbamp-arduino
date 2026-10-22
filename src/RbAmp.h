/**
 * @file    RbAmp.h
 * @brief   Arduino client library for the rbAmp I2C sensor/dimmer module —
 *          public API.
 * @author  rbAmp team
 * @date    2026
 * @version 1.6.1
 *
 * @details
 * @par Overview
 * @c RbAmp wraps the rbAmp wire-level I2C protocol defined in
 * @c libs/spec/SPEC.md (the "single source of truth" — every per-platform
 * library conforms to the same specification). The Arduino library is one of
 * five client implementations; users moving between platforms will recognise
 * the method names immediately.
 *
 * @par Quick start
 * @code
 * #include <Wire.h>
 * #include <RbAmp.h>
 *
 * RbAmp dev(Wire, 0x50);
 *
 * void setup() {
 *     Wire.begin();
 *     while (!dev.begin()) { delay(1000); }
 * }
 *
 * void loop() {
 *     RbAmpPeriodSnapshot snap;
 *     if (dev.readPeriodSnapshot(snap)) {
 *         Serial.print("P0 = "); Serial.print(snap.avg_p[0]); Serial.println(" W");
 *         Serial.print("Wh = "); Serial.println(dev.energy().wh(0), 4);
 *     }
 *     delay(60000);
 * }
 * @endcode
 *
 * @par Supported platforms
 *  - Arduino AVR (Uno, Mega, Nano) — full API, no async examples.
 *  - ESP32 / ESP32-S2 / ESP32-S3 / ESP32-C3 (Arduino-ESP32 core) — full API.
 *  - ESP8266 (Arduino-ESP8266 core) — full API.
 *  - STM32 boards (STM32duino core) — full API.
 *  - SAMD / RP2040 — should work via standard Wire, untested.
 *
 * @par Protocol invariants enforced
 *  - BASIC line: no I2C auto-increment — each multi-byte register is read with
 *    one address phase per byte (SPEC §6). Slow but correct.
 *  - STD line (v1.6.0+): 16-bit big-endian register pointer + auto-increment
 *    burst reads (RbAmpStdRegisters.h); the line is detected once in begin()
 *    and routed internally — same method surface for both lines, and the
 *    8-bit primitives refuse on an STD instance (@c RB_ERR_NOT_IMPLEMENTED).
 *  - 50 ms settle after CMD_LATCH_PERIOD before reading the period block.
 *  - 700 ms settle after CMD_SAVE_GAINS.
 *  - REG_V03_PERIOD_VALID (0x07) checked before consuming the snapshot.
 *  - Two-step API for I2C address changes (prepare + commit within 5 s).
 *
 * @see libs/spec/SPEC.md
 * @see libs/spec/registers.yaml
 */
#ifndef RBAMP_H
#define RBAMP_H

/** Polling interval of RbAmp::waitDataReady() when no DRDY line is attached, ms (define before including to override). */
#ifndef RBAMP_DRDY_POLL_MS
#define RBAMP_DRDY_POLL_MS 20
#endif

#include <Arduino.h>
#include <Wire.h>
#include <stdint.h>

#include "RbAmpRegisters.h"    /* auto-generated; do not edit — namespace rbamp:: (V1 bridge) */
#include "RbAmpRegistersV2.h" /* auto-generated; do not edit — namespace rbamp::v2:: (v1.3 surface) */
#include "RbAmpStdRegisters.h" /* auto-generated; do not edit — namespace rbamp_std:: (STD line, 16-bit map) */
#include "RbAmpSnapshot.h"
#include "RbAmpEnergy.h"

/**
 * @class RbAmp
 * @brief Arduino client for one rbAmp slave device.
 *
 * Instances are bound at construction to a @c TwoWire bus reference and an
 * I2C address. Multiple @c RbAmp instances may share the same bus to talk
 * to several rbAmp modules; use the static broadcastLatch() to synchronise
 * their period boundaries.
 *
 * @note Methods that read a single value return @c float and use @c NAN to
 *       signal failure. Call lastError() to retrieve the specific error
 *       code (one of the @c RB_ERR_* constants from RbAmpRegisters.h).
 *       Methods that perform an operation return @c bool (true on success).
 *
 * @see SPEC §12 — Unified API surface.
 */
class RbAmp {
public:
    /**
     * @brief Construct an unbound device handle.
     *
     * No I2C traffic occurs in the constructor. Call begin() after
     * @c Wire.begin() to probe the device and detect its variant.
     *
     * @param[in] bus  Wire bus reference. Caller must call @c bus.begin() before begin().
     * @param[in] addr 7-bit slave address (default 0x50). Range 0x08..0x77.
     * @param[in] hint Topology hint for begin() — used ONLY as fallback when
     *                 the variant byte @c REG_HW_VARIANT (0x55) is not exposed
     *                 by the firmware. Default @c Single keeps the safe path
     *                 on v1.0 (unmapped variant byte → no spurious polls on
     *                 channels 1-2 of a UI1 SKU). Override when you know the
     *                 SKU at compile-time and want a non-Single default for
     *                 pre-v1.2 firmware.
     *
     * @note v1.2+ firmware exposes the canonical @c REG_HW_VARIANT (0x55)
     *       returning 0x01..0x06 (UI1/UI2/UI3/I1/I2/I3 — truth-doc §1.2).
     *       @c begin() reads it and sets topology + voltage-hw directly,
     *       ignoring @c hint on success. On older firmware (returns 0x00),
     *       @c hint is the source of truth.
     */
    explicit RbAmp(TwoWire& bus,
                   uint8_t addr = 0x50,
                   RbAmpTopology hint = RbAmpTopology::Single) noexcept;

    /* ============================================================
     * Lifecycle (SPEC §12)
     * ============================================================ */

    /**
     * @brief Probe the device, detect its product line + variant, run primer LATCH.
     *
     * Performs:
     *  -# STD identity probe FIRST (one 16-bit burst of the identity block):
     *     magic 'ST' + map version + capabilities → STD line, channel counts
     *     from discovery, latch bank 0 armed + primer latch. Return.
     *  -# REG_VERSION (0x03) read — fails if device does not ACK.
     *  -# Variant: use constructor @c hint (cannot probe — see SPEC §8 v1).
     *  -# U_rms read with threshold for @c hasVoltageHw().
     *  -# CMD_LATCH_PERIOD primer write (50 ms settle) — discards the first
     *     snapshot to clean the accumulator.
     *  -# Records master_t_last for subsequent energy integration.
     *
     * Idempotent; safe to call multiple times.
     *
     * @return @c true if probe succeeded; @c false otherwise.
     *         Call lastError() for detail.
     */
    bool begin() noexcept;

    /**
     * @brief Lightweight alive check.
     *
     * Single byte read of REG_VERSION. No side effects.
     *
     * @return @c true if slave ACKs and reports a supported firmware version.
     */
    bool probe() noexcept;

    /**
     * @brief Poll REG_V03_STATUS (0xCE) bit 0 until the device reports valid data.
     *
     * Useful at boot when the device may need one measurement window
     * (100 ms; 200 ms on images before v1.2) to commit its first RT data.
     *
     * @param[in] timeout_ms Maximum wait in ms (default 1000).
     * @return @c true if valid bit observed; @c false on timeout.
     */
    bool waitReady(uint32_t timeout_ms = 1000) noexcept;

    /** @return Firmware version byte (REG_VERSION, 0x03). Returns 0 on failure. */
    uint8_t firmwareVersion() noexcept;

    /** @return Detected topology (cached from begin()). */
    RbAmpTopology topology() const noexcept { return topology_; }

    /** @return Number of valid current channels (1..7; STD line: N_CURRENT from discovery, capped at RBAMP_MAX_CHANNELS). */
    uint8_t channels() const noexcept { return channels_; }

    /**
     * @return Product line detected by begin() (@c RbAmpLine::Basic until it
     *         runs). See @c RbAmpLine for what differs between the lines.
     */
    RbAmpLine line() const noexcept { return std_ ? RbAmpLine::Std : RbAmpLine::Basic; }

    /** @return @c true if voltage sensing hardware was detected. */
    bool hasVoltageHw() const noexcept { return has_voltage_hw_; }

    /** @return Current I2C address (changes after a successful commitAddressChange()). */
    uint8_t address() const noexcept { return addr_; }

    /**
     * @brief Read the wire-level @c REG_TOPOLOGY (0x24) byte directly.
     *
     * Issues one I2C transaction. Distinct from @c topology() which returns
     * the constructor hint (cached, no bus traffic). Use this to detect v1.1
     * firmware: it returns 1/2/3 (SINGLE/SPLIT_PHASE/THREE_PHASE). v1.0
     * firmware returns 0x00 (unmapped → library's retry+sanity path may
     * surface @c RB_ERR_NACK or treat 0 as the actual value — both are
     * disambiguatable by checking @c firmwareVersion() against @c 0x02).
     *
     * @return Raw register byte on success, or @c 0xFF on I2C failure
     *         (distinct from the 0x00 "unmapped on v1.0" reading).
     */
    uint8_t rawTopology() noexcept;

    /* ============================================================
     * Real-time reads (SPEC §12 — RT block, refreshed every measurement
     * window: 100 ms on both lines; 200 ms on BASIC images before v1.2 and
     * on STD images up to 0x7B)
     * ============================================================ */

    /**
     * @brief Read instantaneous RMS voltage (REG_V03_U_RMS, 0x86).
     * @param[in] phase Phase index — 0 on the BASIC line; 0..n_voltage-1 on
     *                  the STD line (see stdInfo()).
     * @return Voltage in V, or @c NAN on failure.
     */
    float readVoltage(uint8_t phase = 0) noexcept;

    /**
     * @brief Read instantaneous peak voltage (REG_V03_U_PEAK, 0x8A).
     * @param[in] phase Phase index — 0 on BASIC; 0..n_voltage-1 on STD.
     * @return Peak voltage in V, or @c NAN on failure.
     */
    float readVoltagePeak(uint8_t phase = 0) noexcept;

    /**
     * @brief Read instantaneous RMS current for one channel.
     *
     * Reads REG_V03_I0_RMS (0x8E), REG_V03_I1_RMS (0x92), or REG_V03_I2_RMS (0x96).
     *
     * @param[in] ch Channel index 0..2. Must be less than channels().
     * @return Current in A, or @c NAN on failure / out-of-range channel.
     */
    float readCurrent(uint8_t ch = 0) noexcept;

    /**
     * @brief Read instantaneous peak current for one channel.
     * @param[in] ch Channel index 0..2.
     * @return Peak current in A, or @c NAN on failure.
     */
    float readCurrentPeak(uint8_t ch = 0) noexcept;

    /**
     * @brief Read instantaneous real power for one channel (signed).
     *
     * Negative values indicate net export on bidirectional installations.
     *
     * @param[in] ch Channel index 0..2.
     * @return Real power in W, or @c NAN on failure.
     */
    float readPower(uint8_t ch = 0) noexcept;

    /**
     * @brief Read instantaneous power factor for one channel (-1..+1).
     * @param[in] ch Channel index 0..2.
     * @return Power factor (dimensionless), or @c NAN on failure.
     */
    float readPowerFactor(uint8_t ch = 0) noexcept;

    /**
     * @brief Read reactive power for one channel (var, signed).
     * @param[in] ch Channel index 0..channels()-1.
     * @return Reactive power in var, or @c NAN on failure. STD line only —
     *         @c RB_ERR_NOT_IMPLEMENTED on BASIC (no reactive register).
     */
    float readReactivePower(uint8_t ch = 0) noexcept;

    /**
     * @brief Read apparent power for one channel (VA).
     * @param[in] ch Channel index 0..channels()-1.
     * @return Apparent power in VA, or @c NAN on failure. STD line only —
     *         @c RB_ERR_NOT_IMPLEMENTED on BASIC (no apparent register).
     */
    float readApparentPower(uint8_t ch = 0) noexcept;

    /**
     * @brief Read mains frequency (REG_AC_FREQ, 0x20).
     * @return 50.0 or 60.0 Hz (BASIC; STD reports 0.01 Hz resolution), or @c NAN on failure.
     */
    float readFrequency() noexcept;

    /**
     * @brief One-shot read of the full RT block into a snapshot struct.
     *
     * Equivalent to calling readVoltage(), readCurrent(0..2), readPower(0..2),
     * readPowerFactor(0..2), readFrequency() in sequence. Unused channels
     * (per channels()) are filled with @c 0.0f.
     *
     * @param[out] out Snapshot to populate.
     * @return @c true on success, @c false if any underlying read failed.
     */
    bool readAll(RbAmpSnapshot& out) noexcept;

    /* ============================================================
     * Period metering (SPEC §7)
     * ============================================================ */

    /**
     * @brief Issue CMD_LATCH_PERIOD (write 0x27 to REG_COMMAND).
     *
     * Does not wait. Caller is responsible for the 50 ms settle plus
     * REG_V03_PERIOD_VALID check before reading 0xDC / 0xC2 / 0xC6 / 0xE0.
     * For most usage, prefer readPeriodSnapshot() which encapsulates the
     * full sequence.
     *
     * @return @c true if the write succeeded.
     */
    bool latchPeriod() noexcept;

    /**
     * @brief Read REG_V03_PERIOD_VALID (0x07) bit 0.
     * @return @c true if the latched snapshot at 0xDC/0xE0/0xEC is fresh.
     */
    bool isPeriodValid() noexcept;

    /**
     * @brief Read REG_V03_PERIOD_AVG_P for one channel (latched float32).
     *
     * Reads REG_V03_PERIOD_AVG_P_F0 (0xDC), F1 (0xC2), or F2 (0xC6) depending
     * on channel. Must be called after latchPeriod() + settle + valid check.
     *
     * @param[in] ch Channel index 0..2.
     * @return Average real power in W, or @c NAN on failure.
     */
    float readPeriodAvgPower(uint8_t ch = 0) noexcept;

    /**
     * @brief Read REG_V03_PERIOD_MAX_P_F0 (0xE0).
     * @return Peak real power on channel 0 during the latched period (W), or @c NAN.
     */
    float readPeriodMaxPower() noexcept;

    /**
     * @brief Read REG_V03_PERIOD_LATCH_MS (0xEC) — diagnostic.
     *
     * Device's view of the period duration. Use the master's own wall-clock
     * for energy integration; this field is diagnostic only.
     *
     * @return Period duration in ms, or 0 on failure.
     */
    uint32_t readPeriodLatchMs() noexcept;

    /**
     * @brief One-shot period snapshot: latch, settle, valid-check, read, integrate.
     *
     * Recommended entry point for period metering. Sequence:
     *  -# Skip the latch if @c skip_latch is true (use after broadcastLatch()).
     *  -# Else write CMD_LATCH_PERIOD.
     *  -# delay(settle_ms) — default 50 ms per SPEC.
     *  -# Read REG_V03_PERIOD_VALID; if bit 0 == 0 the snapshot is stale
     *     (lastError() = RB_ERR_STALE).
     *  -# Read avg_p for each populated channel + max_p + latch_ms.
     *  -# Compute master_dt_ms from millis() since previous successful snapshot.
     *  -# Call energy().tick() to integrate into per-channel Wh totals.
     *
     * @param[out] out         Snapshot to populate.
     * @param[in]  settle_ms   Wait after latch before reading (default 50).
     * @param[in]  skip_latch  If true, assume an external party (e.g.
     *                         broadcastLatch()) has already latched and skip
     *                         the write — only read.
     * @return @c true if @c out.valid == true; @c false if the read failed
     *         or the snapshot was stale.
     */
    bool readPeriodSnapshot(RbAmpPeriodSnapshot& out,
                            uint16_t settle_ms = 50,
                            bool skip_latch = false) noexcept;

    /* ============================================================
     * Energy (master-side, library-owned)
     * ============================================================ */

    /**
     * @brief Access the per-device Wh accumulator.
     *
     * The accumulator is updated automatically by readPeriodSnapshot().
     * Call energy().disable() if you prefer to integrate energy in
     * user code.
     */
    RbAmpEnergy& energy() noexcept { return energy_; }

    /** @brief Const accessor for energy(). */
    const RbAmpEnergy& energy() const noexcept { return energy_; }

    /**
     * @brief Running energy total with the module's own answer for "unknown".
     *
     * Same as energy().wh(ch), except on an STD module that keeps persisted
     * counters but publishes no per-channel split (counter path, map < 0x36,
     * more than one channel): a per-channel request returns @c NaN there
     * rather than a 0 that reads as "nothing consumed". Use
     * @c RBAMP_ENERGY_CH_TOTAL for the all-channel total.
     */
    double energyWh(uint8_t ch = 0) const noexcept;

    /* ============================================================
     * Configuration (SPEC §10, §11)
     * ============================================================ */

    /**
     * @brief Set the current-sensor family and persist to flash.
     *
     * Reads the module's class first (read-before-write):
     *   - the class the module already holds is a no-op — returns @c true with
     *     NO bus write. On the module ANY write of REG_SENSOR_CLASS, even of
     *     the unchanged value, clears every channel's CT model and its
     *     calibration shape, so re-applying a stored configuration on every
     *     reconnect used to destroy the unit's calibration;
     *   - a module whose class is WiredCT or BuiltinCT (shipped and calibrated
     *     together with its sensor) is not rewritten: @c false with
     *     @c RB_ERR_MODE and no bus write. Service re-pairing goes through
     *     setCTModelChForce().
     *
     * Otherwise writes REG_SENSOR_CLASS (0x25), issues CMD_SAVE_USER_CONFIG
     * (0x32) and waits 700 ms for the flash erase. Blocking. (SENSOR_CLASS is
     * user_config; CMD_SAVE_GAINS is factory-gated and a silent no-op
     * in production.)
     *
     * On v1.2+ firmware a set class is a precondition for setCTModel() —
     * otherwise setCTModel() returns false with @c RB_ERR_PARAM. A class
     * change also resets the CT models to 0 device-side, so set the models
     * afterwards.
     *
     * On v1.0/v1.1 firmware the register exists in the firmware register
     * table but has no functional effect — write is harmless and ignored.
     *
     * On the STD line the class is metadata (no preset table): written to the
     * config block and persisted with the module's SAVE_CONFIG command, whose
     * verdict is checked (map 0x34+). The same read-before-write rules apply.
     *
     * @param[in] cls Sensor class.
     * @return @c true on success, or when the module already holds @p cls.
     */
    bool setSensorClass(RbAmpSensorClass cls) noexcept;

    /**
     * @brief Read the module's current sensor class (RbAmpSensorClass value).
     *
     * BASIC: REG_SENSOR_CLASS (0x25). STD: the class field of the config block.
     *
     * @param[out] out Class code (0 = unset).
     * @return @c true on success.
     */
    bool readSensorClass(uint8_t& out) noexcept;

    /**
     * @brief Registry entry of a sensor model — name, descriptor, status and
     *        the current limits @c nominal_a (rated RMS current), @c fs_a (RMS
     *        at clip onset for a sine) and @c peak_a (instantaneous clip
     *        ceiling); 0 = not known for that model.
     *
     * Pure lookup in the generated registry (RbAmpSensorModels.h) — no bus
     * traffic. A registry hit does NOT mean the connected module accepts the
     * code; that is runtime truth (see setCTModel()).
     *
     * @return The entry, or @c nullptr if (class, code) is not in the registry.
     */
    static const rbamp_sensor_model_info_t* sensorModel(uint8_t cls, uint8_t code) noexcept {
        return rbamp_sensor_model_lookup(cls, code);
    }
    static const rbamp_sensor_model_info_t* sensorModel(RbAmpSensorClass cls, uint8_t code) noexcept {
        return sensorModel(static_cast<uint8_t>(cls), code);
    }

    /**
     * @brief Rated RMS current of a sensor model, A (0 = not known).
     *
     * Pure lookup, no bus traffic. Covers the SCT-013 family and the clamp
     * models that carry a rating; legacy toroid / burden codes return 0.
     */
    static float sensorModelRatedA(uint8_t cls, uint8_t code) noexcept {
        const rbamp_sensor_model_info_t* m = rbamp_sensor_model_lookup(cls, code);
        return m ? m->nominal_a : 0.0f;
    }
    static float sensorModelRatedA(RbAmpSensorClass cls, uint8_t code) noexcept {
        return sensorModelRatedA(static_cast<uint8_t>(cls), code);
    }

    /**
     * @brief Set the SCT-013 CT model on channel 0 (legacy single-arg form).
     *
     * Writes REG_CT_MODEL (0x05), binds channel 0, issues CMD_SAVE_USER_CONFIG
     * (0x32), waits 700 ms for flash erase. Blocking. (CT_MODEL is user_config;
     * CMD_SAVE_GAINS is factory-gated and a silent no-op in production.)
     *
     * v1.2+ firmware precondition: setSensorClass() MUST be called first,
     * otherwise this method returns false with @c RB_ERR_PARAM and does not
     * write. On v1.0/v1.1 firmware the precondition is skipped (backward
     * compat — device-side callback has no guard).
     *
     * For multi-channel modules (UI2/UI3/I2/I3) use the per-channel overload
     * @c setCTModel(channel, code) instead — this single-arg form only
     * configures channel 0 (the device-side legacy direct-write path).
     *
     * Read-before-write guard as in @c setCTModel(channel, code): the model
     * channel 0 already holds is not re-applied (no bus write), and a module of
     * class WiredCT / BuiltinCT is not reconfigured (@c RB_ERR_MODE).
     *
     * @param[in] code CT-model code — use a generated @c RBAMP_CT_* define
     *                 (RbAmpSensorModels.h), e.g. @c RBAMP_CT_SCT013_030 or
     *                 @c RBAMP_CT_CLAMP2000_20A. The code's meaning is per-class;
     *                 a code absent from the registry is rejected with
     *                 @c RB_ERR_PARAM, and a code the module does not accept
     *                 (reserved / uncharacterised) is surfaced as the device's
     *                 @c RB_ERR_PARAM.
     * @return @c true on success.
     */
    bool setCTModel(uint8_t code) noexcept;

    /**
     * @brief Typed-enum overload of @c setCTModel(uint8_t code) — SCT-013 only.
     *
     * Backward-compat convenience for the seven SCT-013 codes. As of v1.5.0 the
     * canonical way to name a model — and the only way to name a WIRED_CT model
     * — is the generated @c RBAMP_CT_* define:
     * @code
     * dev.setCTModel(RBAMP_CT_SCT013_030);        // ← canonical (any class)
     * dev.setCTModel(RbAmpCTModel::Sct013_030);   // ← legacy typed (SCT-013)
     * @endcode
     *
     * @param[in] model One of the @c RbAmpCTModel enumerators (Unset rejected
     *                  with @c RB_ERR_PARAM, same as @c code = 0).
     * @return @c true on success.
     */
    bool setCTModel(RbAmpCTModel model) noexcept {
        return setCTModel(static_cast<uint8_t>(model));
    }

    /**
     * @brief Set the CT model on a specific channel (v1.2+ firmware).
     *
     * Channels 0-2 bind via @c CMD_SET_CT_MODEL_CH0/CH1/CH2 (0x28/0x29/0x2A);
     * channels 3+ (senior SKUs UI5/UI7) bind via the channel window field 15
     * (CT_MODEL), which requires v1.4.18+ firmware (fw ver 0x0A). Both paths end
     * with @c CMD_SAVE_USER_CONFIG for flash persistence (700 ms erase; CT_MODEL
     * is user_config — SAVE_GAINS is factory-gated); the ch3+ path
     * additionally reads the applied model back and reports @c RB_ERR_PARAM if
     * the module rejected the code (it keeps its previous model). Blocking
     * ~705 ms per call.
     *
     * @note On firmware 1.4.4+ (VERSION >= 0x05) and on most 0x04 modules the
     * order of per-channel writes does not matter. Very early 1.2 modules
     * overwrote channel 0 as a side effect of a channel 1-2 write; writing the
     * higher channels first, or using @c configureChannels(), is safe on all
     * of them. Verify the result with @c readCTModelCh().
     *
     * Requires @c firmwareVersion() >= 0x03 (v1.2); ch3+ requires >= 0x0A
     * (v1.4.18). Returns @c RB_ERR_VERSION on older firmware. Same
     * @c RB_ERR_PARAM guard as the single-arg form applies (sensor class must be
     * set first, and the code must exist in the registry).
     *
     * Read-before-write guard. Applying a model sets that channel's generic
     * preset, replacing whatever calibration the unit carries for it — and the
     * module does that even when the model does not change. So:
     *   - the model the channel already holds (readCTModelCh()) is a no-op:
     *     @c true with NO bus write and no flash cycle, which makes re-applying
     *     a stored configuration on every reconnect safe;
     *   - a module of class WiredCT or BuiltinCT (calibrated together with its
     *     fitted sensor) returns @c false with @c RB_ERR_MODE and no bus write;
     *     setCTModelChForce() is the service path.
     *
     * On the STD line the model is a REFERENCE only (sensor label — it sets no
     * gain; the module is calibrated per channel): written to the per-channel
     * map entry, read back, and persisted with SAVE_CONFIG. Current module
     * firmware persists channel 0's reference across reboot; channels 1+ hold
     * theirs until the next reboot. No channel-0 clobber side-effect on STD.
     * The same read-before-write rules apply.
     *
     * @param[in] channel 0..channels()-1.
     * @param[in] code    A generated @c RBAMP_CT_* code (see RbAmpSensorModels.h).
     * @return @c true on success, or when the channel already holds @p code.
     */
    bool setCTModel(uint8_t channel, uint8_t code) noexcept;

    /**
     * @brief Service path: set a channel's CT model without the pairing guard.
     *
     * Same as @c setCTModel(channel, code) but writes regardless of the
     * module's class and of the model the channel currently holds. It
     * replaces the paired calibration of that channel with the model's generic
     * preset — use only when re-pairing a module with a new sensor in service,
     * and follow it with a calibration. The registry check (unknown code →
     * @c RB_ERR_PARAM, no write) still applies.
     *
     * @param[in] channel 0..channels()-1.
     * @param[in] code    A generated @c RBAMP_CT_* code.
     * @return @c true on success.
     */
    bool setCTModelChForce(uint8_t channel, uint8_t code) noexcept;

    /**
     * @brief Typed-enum overload of @c setCTModel(channel, code) — SCT-013 only.
     *
     * Equivalent to @c setCTModel(channel, static_cast<uint8_t>(model)).
     * Same channel-order caveat and @c RB_ERR_VERSION gate apply. New code
     * should pass a generated @c RBAMP_CT_* code to the @c uint8_t overload.
     *
     * @param[in] channel 0..channels()-1.
     * @param[in] model   One of the @c RbAmpCTModel enumerators.
     * @return @c true on success.
     */
    bool setCTModel(uint8_t channel, RbAmpCTModel model) noexcept {
        return setCTModel(channel, static_cast<uint8_t>(model));
    }

    /**
     * @brief Configure sensor class + per-channel CT models in one batched call (v1.3).
     *
     * The recommended entry point for multi-channel modules. Sets @p cls
     * (one SAVE), then binds @c models[ch] to each channel @b ascending
     * (order-independent on v1.3 firmware), and finishes with
     * ONE terminal SAVE_GAINS — so a UI3 costs two flash cycles, not four.
     *
     * Each non-zero model is validated against the per-class accepted set (A1)
     * before the wire write — an uncharacterised code (e.g. SCT-013-100) fails
     * fast with @c RB_ERR_PARAM and aborts the batch. A @c models[ch] of 0
     * leaves that channel's existing model untouched. @p n is clamped to
     * channels().
     *
     * Read-before-write (see setSensorClass() and setCTModel(channel, code)):
     * a class or model the module already holds is not rewritten, so
     * re-applying the same configuration costs only reads and no flash cycle.
     * A module of class WiredCT / BuiltinCT is not reconfigured, and a request
     * to bind models under one of those classes is refused up front — in both
     * cases @c false with @c RB_ERR_MODE and nothing written.
     *
     * @param[in] cls    Sensor class to apply first.
     * @param[in] models Per-channel CT codes (index 0..n-1); 0 = skip channel.
     * @param[in] n      Number of entries in @p models.
     * @return @c true if class + every requested bind succeeded.
     */
    bool configureChannels(RbAmpSensorClass cls, const uint8_t* models, uint8_t n) noexcept;

    /**
     * @brief Read the CT model actually APPLIED to a channel (mirror register).
     *
     * Reads REG_CT_MODEL_CH0/CH1/CH2 (0x51/0x52/0x53) — the device's read-back
     * of the bound preset, A/B torn-read protected. Use to confirm a bind took.
     *
     * @param[in]  channel 0..2.
     * @param[out] out     Applied CT code (0 = unset).
     * @return @c true on success.
     */
    bool readCTModelCh(uint8_t channel, uint8_t& out) noexcept;

    /**
     * @brief SCT-013 rating correction (BASIC firmware up to 1.4.19).
     *
     * Those modules are calibrated on the SCT-013-030 and, after a restart,
     * apply that calibrated gain whatever model the channel shows. begin()
     * reads each channel's model and applied gain and sets a per-channel
     * scale = preset(model) × c(model) / preset(030) that readCurrent() /
     * readCurrentPeak() / readPower() / readAll() / readPeriodAvgPower() /
     * readPeriodMaxPower() / readPeriodSnapshot() (and the energy derived
     * from it) apply; power factor and voltage are unchanged. The plausibility
     * bounds and setEventThresholds() are expressed in true amps / watts.
     *
     * A model written in the running boot makes the module apply the raw
     * preset (no unit trim): the setters therefore restart the module after
     * the save (CMD_RESET, wait for the same UID) and re-read; if begin()
     * finds that state (another master switched the model), it restarts the
     * module once. The library re-reads on its own after readEventFlags()
     * reports @c EVENT_RESET_OCCURRED; call sctRatingRefresh() after a
     * restart you detect some other way. A module that reports capability
     * bit 12 (SCT rating self-correction, firmware 1.4.20+) corrects itself:
     * scale 1 and a plain model write without a restart. STD modules and the
     * wired / built-in CT classes are never scaled.
     *
     * @return @c true when the module could be read (scales valid).
     */
    bool sctRatingRefresh() noexcept;

    /**
     * @brief Rating correction state of one channel (as computed by begin(),
     *        the model setters or sctRatingRefresh()).
     * @param[in]  channel 0..channels()-1.
     * @return @c true if @p channel is valid.
     */
    bool sctRating(uint8_t channel, RbAmpSctRating& out) noexcept;

    /** @return The factor applied to that channel's I / P readings (1.0 when nothing is corrected). */
    float sctRatingScale(uint8_t channel) const noexcept {
        return (channel < RBAMP_MAX_CHANNELS) ? sct_[channel].scale : 1.0f;
    }

    /** @return The firmware's preset gain for an SCT-013 model code (op-amp revision), 0 if unknown. */
    static float sctPresetGain(uint8_t model) noexcept;

    /**
     * @brief Bare CMD_SAVE_GAINS — flush in-memory gain registers to flash.
     *
     * Normally invoked internally by setSensorClass(), setCTModel(), and
     * commitAddressChange() — most users never need to call this directly.
     *
     * @warning Bare saveGains() is relevant ONLY if the caller has manually
     *          written to non-public calibration registers (NF, GAIN,
     *          PHASE_*) via raw register access. That is an out-of-warranty
     *          operation and bypasses the SKU-matched preset table — incorrect
     *          values produce wrong current/power readings with no obvious
     *          warning. Standard users configure the module via
     *          setSensorClass() + setCTModel() and never need saveGains().
     *
     *          Each call performs a flash erase + write cycle (~700 ms). Flash
     *          endurance is finite (~10 000 cycles per page) — do not call in
     *          a loop.
     *
     * On the STD line this issues the module's SAVE_CAL command (persists the
     * calibration block: gains, noise floors, THD floors), waits until the
     * module reports the command finished and checks its verdict. The module
     * writes its store at most once per 5 s; a save inside that window is
     * retried once after 5 s.
     *
     * @return @c true on success.
     */
    bool saveGains() noexcept;

    /**
     * @brief Persist user-config to flash — CMD_SAVE_USER_CONFIG (production-OK).
     *
     * Saves the user-config namespace (ct_model / sensor_class / fleet_config /
     * group_id / label). Unlike saveGains(), this is NOT factory-gated — it
     * works on production modules. Also clears a fresh module's first-boot
     * FLASH_PARAMS_BAD (0xFB) error. ~700 ms (flash erase + write).
     *
     * On the STD line this issues SAVE_CONFIG (class / CT reference / tier /
     * I2C address; calibration preserved), waits until the module reports the
     * command finished (no fixed sleep) and checks its verdict — an invalid
     * field (e.g. an out-of-range address) returns @c RB_ERR_PARAM. A save
     * inside the module's 5 s store rate limit is retried once after 5 s.
     *
     * @return @c true on success.
     */
    bool saveUserConfig() noexcept;

    /**
     * @brief Arm an I2C address change (step 1 of 2).
     *
     * Validates the new address range and records the arm timestamp. No wire
     * I/O — the change is staged and committed by commitAddressChange(), which
     * must be called within 5 seconds or the arm expires.
     *
     * @note v1.3: the address change is a PRODUCTION-OK two-phase magic commit
     *       (truth-doc §6.1) — it is NOT factory-gated. Field-swapping a
     *       production spare to a new bus address is supported. (The legacy
     *       factory-mode + SAVE_GAINS path has been removed.)
     *
     * @param[in] new_addr New 7-bit slave address (0x08..0x77, != current).
     * @return @c true if armed.
     * @retval RB_ERR_PARAM via lastError(): address out of range or == current.
     */
    bool prepareAddressChange(uint8_t new_addr) noexcept;

    /**
     * @brief Commit the previously prepared address change (step 2 of 2).
     *
     * Must be called within 5 seconds of prepareAddressChange(). Two-phase
     * magic commit (truth-doc §6.1): writes the candidate to REG_I2C_ADDRESS,
     * arms 0xA5 → REG_ADDR_COMMIT_MAGIC, issues CMD_COMMIT_ADDR (persists to
     * flash), then CMD_RESET. The internal address field is updated so
     * subsequent calls target the new address.
     *
     * @note Production-OK — not factory-gated (v1.3). The RESET write failing
     *       is non-fatal: the device adopts the committed address on its next
     *       power cycle regardless.
     * @note STD line: writes the persisted address field, SAVE_CONFIG (waits
     *       for the save to finish; a rejected address returns @c RB_ERR_PARAM
     *       and the module keeps its current address), then RESET, and
     *       switches this instance only once the module answers at the new
     *       address (up to 3 s). If it comes back on the OLD address instead,
     *       the instance stays there and @c RB_ERR_NOT_READY is returned.
     *
     * @warning After a successful commit the device resets and re-enumerates
     *          at the NEW address. Subsequent calls on this RbAmp instance
     *          target the new address transparently — but any other master
     *          on the bus (Python script, ESP-IDF component instance, debug
     *          probe) still believes the device is at the old address until
     *          its own state is updated.
     *
     * @return @c true on success.
     */
    bool commitAddressChange() noexcept;

    /**
     * @brief Issue CMD_FACTORY_RESET (0xAA) and wait 1500 ms.
     *
     * Erases ALL flash params (CT model, sensor class, calibration gains,
     * I²C address) and reboots the device. Bus unavailable during reset.
     *
     * @warning Destructive operation. After calling this, the module returns
     *          to factory defaults — RbAmpSensorClass becomes Unset,
     *          REG_CT_MODEL becomes 0, and any tuning the operator persisted
     *          via setSensorClass()/setCTModel() is gone. The next user MUST
     *          re-apply setSensorClass() + setCTModel() before metering is
     *          usable again. This is NOT a routine "soft restart" — use
     *          reset() (CMD_RESET, 0x01) for that. Reserve factoryReset() for
     *          known-bad-state recovery or for handing the module to another
     *          user / installation.
     *
     * @return @c true if the write succeeded.
     */
    bool factoryReset() noexcept;

    /**
     * @brief Issue CMD_RESET (0x01) and wait 100 ms.
     *
     * Soft-reboot the device. STD line: waits for any running command first
     * (never resets a module mid-save), then waits quietly (up to 3 s) for
     * the module to answer again and re-arms the period path.
     *
     * @return @c true if the write succeeded.
     */
    bool reset() noexcept;

    /* ============================================================
     * Static / multi-module
     * ============================================================ */

    /**
     * @brief I2C General-Call broadcast LATCH — all-call convenience (v1.3).
     *
     * Delegates to @c broadcastLatchGroup(bus, 0x00, 0x0000): broadcasts the
     * latch frame to every GC-enabled module on the bus (@c REG_FLEET_CONFIG
     * bit0, opt-in per device via @c enableGc()). Group @c 0x00 = all-call, so
     * every enabled module latches regardless of @c REG_GROUP_ID.
     *
     * After the broadcast the master waits its settle window, then calls
     * @c readPeriodSnapshot(snap, settle, skip_latch=true) on each device.
     *
     * On v1.0/v1.1 firmware General-Call is disabled, so the frame is harmlessly
     * ignored — fall back to per-device sequential @c latchPeriod() there (see
     * example 03_MultiModuleBroadcast for the skew-tolerant pattern).
     *
     * @param[in,out] bus Wire bus.
     * @return @c true if the frame was transmitted.
     */
    static bool broadcastLatch(TwoWire& bus) noexcept;

    /**
     * @brief I2C General-Call broadcast LATCH with group filter + tick (v1.3).
     *
     * Transmits the 5-byte frame @c {0xA5, CMD_LATCH_PERIOD(0x27), group,
     * tick_lo, tick_hi} to general-call address @c 0x00. Every rbAmp on the bus
     * with @c REG_FLEET_CONFIG bit0 set (see enableGc()) AND a matching
     * @c REG_GROUP_ID — or @c group == 0x00 (all-call) — latches its period
     * accumulator atomically and stores @p tick in @c REG_GC_TICK (0x59).
     *
     * After the broadcast the master waits its settle window, then calls
     * @c readPeriodSnapshot(snap, settle, skip_latch=true) on each device,
     * and may verify per-module sync by reading @c readGcTick() == @p tick.
     *
     * Latch-only by firmware design: destructive opcodes (SAVE_*, COMMIT_ADDR,
     * FACTORY_RESET) are never honoured over General-Call.
     *
     * @param[in,out] bus   Wire bus.
     * @param[in]     group Group filter (0x00 = all-call).
     * @param[in]     tick  16-bit window/tick counter stored in each module.
     * @return @c true if the frame was transmitted.
     */
    static bool broadcastLatchGroup(TwoWire& bus, uint8_t group, uint16_t tick) noexcept;

    /* ============================================================
     * Identity / capability (v1.3)
     * ============================================================ */

    /**
     * @brief Read the hardware SKU variant (REG_HW_VARIANT, 0x55).
     * @return One of @c RbAmpVariant; @c Unknown on v1.0/v1.1 fw or I2C failure.
     */
    RbAmpVariant readVariant() noexcept;

    /**
     * @brief Read the capability bitmap (REG_CAPABILITY, 0x57, u16 LE).
     *
     * Branch on @c rbamp::v2::CAP_* bits, never on firmware-version heuristics.
     *
     * @param[out] out Capability bitmap.
     * @return @c true on success.
     */
    bool readCapability(uint16_t& out) noexcept;

    /**
     * @brief Read the product family ID (REG_PRODUCT_ID, 0x54).
     * @return 0x01 = rbAmp sensor, 0x02 = rbDimmer; 0 on failure.
     */
    uint8_t readProductId() noexcept;

    /**
     * @brief Read the 96-bit chip UID (REG_UID, 0x5C — 12 bytes).
     * @param[out] out 12-byte buffer.
     * @return @c true on success.
     */
    bool readUid(uint8_t out[12]) noexcept;

    /* ============================================================
     * Error / event channel (v1.3)
     * ============================================================ */

    /**
     * @brief Read REG_ERROR (0x02) — outcome of the last write op.
     *
     * Name matches the cross-platform family (`rbamp_read_last_error`).
     *
     * @return Device error class (0x00 = OK, 0xFA..0xFF error), 0xFF on I2C fail.
     */
    uint8_t readLastError() noexcept;

    /**
     * @brief Read sticky event flags (REG_EVENT_FLAGS, 0x2A).
     * @param[out] out Event bitmap (@c rbamp::v2::EVENT_* bits).
     * @return @c true on success.
     */
    bool readEventFlags(uint8_t& out) noexcept;

    /**
     * @brief Clear sticky event flags by writing back a mask (write-1-to-clear).
     * @param[in] mask Bits to clear.
     * @return @c true on success.
     */
    bool clearEventFlags(uint8_t mask) noexcept;

    /**
     * @brief Durable async error check: (EVENT_FLAGS & EVENT_ERROR bit3) != 0.
     * @param[out] out @c true if the device latched an error since last clear.
     * @return @c true if the flags read succeeded.
     */
    bool hasError(bool& out) noexcept;

    /**
     * @brief Issue CMD_CLEAR_ERROR (v1.3 opcode 0x31) — clears REG_ERROR + bit3.
     * @return @c true if the write succeeded.
     */
    bool clearError() noexcept;

    /* ---- Data-ready (DRDY) and events — both product lines ----
     *
     * The module's DRDY output is open-drain, idle HIGH (host pull-up), and
     * pulses LOW (~10-50 µs) each time a new complete snapshot is published
     * — once per measurement window, 100 ms (STD images up to 0x7B: every
     * 200 ms); several modules may share one line (wired-OR). A module
     * whose masked event is pending holds the line solid LOW instead (alarm
     * level) until the flags are cleared. STD modules carry the event block
     * from map 0x36 (image 0x75+); BASIC modules from v1.3. */

    /**
     * @brief Attach (or detach with @p pin < 0) the DRDY line of this module.
     *
     * Configures the pin as an input with the internal pull-up and a
     * falling-edge interrupt (the ISR only counts the edge and stamps it; no
     * bus traffic there). Up to 4 distinct lines and 8 modules per line. An
     * STD module whose image predates the family DRDY contract refuses
     * (@c RB_ERR_NOT_IMPLEMENTED): waitDataReady() then polls the sequence.
     * Without a line, waitDataReady() polls as well.
     */
    bool setDrdyPin(int pin) noexcept;

    /**
     * @brief Block until this module publishes a new snapshot (or an alarm),
     *        at most @p timeout_ms (0 = one non-blocking check).
     *
     * With a line: waits for the edge, then confirms it was this module's by
     * the sequence (another module's pulse on a shared line is ignored). If
     * the line is still LOW 5 ms after the edge, some module on it holds an
     * alarm: the flags of EVERY alarm-capable module on the line are read and
     * cleared (a held line hides the other modules' pulses); this module's
     * flags come back in @p ev, the others' are parked on their instances and
     * reported by their next wait. Without a line: polls the sequence every
     * @c RBAMP_DRDY_POLL_MS.
     *
     * @return @c true with @p ev filled; @c false with @c RB_ERR_TIMEOUT when
     *         nothing arrived, @c RB_ERR_NOT_IMPLEMENTED on an STD module
     *         below map 0x36 (no publish counter).
     */
    bool waitDataReady(uint32_t timeout_ms, RbAmpDrdyEvent* ev = nullptr) noexcept;

    /**
     * @brief waitDataReady() followed by readAll(): the whole read lands
     *        inside one publish window, so the snapshot is never read twice
     *        and never straddles a publish.
     */
    bool readAllReady(uint32_t timeout_ms, RbAmpSnapshot& out, RbAmpDrdyEvent* ev = nullptr) noexcept;

    /**
     * @brief The module's snapshot sequence: STD publish counter (u16, map
     *        0x36+), BASIC commit sequence (u8). Change detection only.
     */
    bool readDataSeq(uint16_t& out) noexcept;

    /**
     * @brief Event flags as found by begin(), before the library wrote
     *        anything (nothing is cleared there). @c EVENT_RESET_OCCURRED
     *        here means the module restarted before this instance met it.
     */
    uint8_t beginEventFlags() const noexcept { return begin_event_flags_; }

    /** @brief Read which event bits assert the DRDY alarm level (EVENT_MASK). */
    bool readEventMask(uint8_t& out) noexcept;

    /**
     * @brief Set which event bits hold DRDY LOW (readback-verified).
     * @note  RAM on the module: re-apply after a module reset
     *        (@c EVENT_RESET_OCCURRED tells you).
     */
    bool setEventMask(uint8_t mask) noexcept;

    /**
     * @brief Set the current / power thresholds behind @c EVENT_THRESH_I /
     *        @c EVENT_THRESH_P; a value <= 0 disables that threshold.
     *
     * BASIC compares the maximum channel current (0.01 A steps) and the
     * SIGNED sum of channel powers (1 W steps) — export never trips it. STD
     * (map 0x36+) compares any channel current (µA) and the MAGNITUDE of the
     * grand power (mW) — direction-agnostic. Threshold events are
     * level-latched on the module (re-set every window while the condition
     * holds): lift the threshold before expecting a quiet line. RAM on the
     * module.
     */
    bool setEventThresholds(float i_amps, float p_watts) noexcept;

    /* ============================================================
     * Fleet / multi-module (single-device side; see RbAmpFleet for the manager)
     * ============================================================ */

    /**
     * @brief Enable or disable General-Call latch reception, persisted (v1.3).
     *
     * Read-modify-writes @c REG_FLEET_CONFIG (0x27) bit0, issues
     * @c CMD_SAVE_USER_CONFIG (production-OK), then @c CMD_RESET — the GC ISR is
     * wired only at boot, so a reset is mandatory for the change to take effect.
     * Blocking (~1 s: save 700 ms + reset settle).
     *
     * @param[in] enable @c true to receive GC latches.
     * @return @c true on success.
     */
    bool enableGc(bool enable) noexcept;

    /**
     * @brief Read REG_FLEET_CONFIG (0x27).
     * @param[out] out Config byte (bit0 = GC_ENABLE).
     * @return @c true on success.
     */
    bool readFleetConfig(uint8_t& out) noexcept;

    /**
     * @brief Set the GC group filter (REG_GROUP_ID, 0x28). Persist with
     *        @c CMD_SAVE_USER_CONFIG (e.g. via enableGc()) to survive reset.
     * @param[in] group Group id (0x00 = respond to all-call only).
     * @return @c true on success.
     */
    bool setGroupId(uint8_t group) noexcept;

    /**
     * @brief Read the GC group filter (REG_GROUP_ID, 0x28).
     * @param[out] out Group id.
     * @return @c true on success.
     */
    bool readGroupId(uint8_t& out) noexcept;

    /**
     * @brief Read the last accepted GC tick (REG_GC_TICK, 0x59, u16 LE).
     *
     * A/B torn-read protected (correctness-critical fleet-sync witness).
     * @c 0xFFFF means no GC frame has been received since boot.
     *
     * @param[out] out Tick value.
     * @return @c true on success.
     */
    bool readGcTick(uint16_t& out) noexcept;

    /**
     * @brief Read the user location label (REG_LABEL, 0x68 — 8 ASCII bytes).
     * @param[out] out 9-byte buffer (8 chars + NUL terminator).
     * @return @c true on success.
     */
    bool readLabel(char out[9]) noexcept;

    /**
     * @brief Write the user location label (8 bytes, zero-padded past NUL).
     *        Persist with @c CMD_SAVE_USER_CONFIG to survive reset.
     * @param[in] label NUL-terminated string; bytes past 8 are dropped.
     * @return @c true on success.
     */
    bool writeLabel(const char* label) noexcept;

    /**
     * @brief Read the live slave address (REG_I2C_ADDRESS, 0x30), A/B protected.
     *        STD line: the persisted address field (applies on reset).
     * @param[out] out 7-bit address currently held by the device.
     * @return @c true on success.
     */
    bool readActiveAddress(uint8_t& out) noexcept;

    /* ============================================================
     * Diagnostics
     * ============================================================ */

    /**
     * @brief Read the digest commit sequence byte (REG_DIGEST+1, 0x71).
     *
     * The device increments this once per published measurement window —
     * 100 ms on firmware v1.2+, 200 ms before — wrapping 255→0. Sample it
     * before and after a multi-register read to detect that the published
     * values advanced mid-read (values may straddle two windows). There is NO CRC in the digest — this is a change flag only,
     * paired with the per-value @c isfinite()/range filter for integrity.
     *
     * @param[out] out Sequence byte (0..255).
     * @return @c true on success.
     */
    bool readCommitSeq(uint8_t& out) noexcept;

    /**
     * @brief Last error code from any operation.
     * @return One of @c rbamp::RB_OK or @c rbamp::RB_ERR_* (see RbAmpRegisters.h).
     */
    int8_t lastError() const noexcept { return last_error_; }

    /**
     * @brief Human-readable string for an error code.
     * @param[in] code Error code as returned by lastError().
     * @return Static const string, never null.
     */
    static const char* errorString(int8_t code) noexcept;

    /**
     * @brief Instance shortcut for @c errorString(lastError()).
     *
     * Adafruit-style ergonomic forwarder — saves users from typing
     * @c RbAmp::errorString(dev.lastError()) at every diagnostic call site.
     *
     * @return Human-readable string for the most recent error. Never null.
     */
    const char* errorString() const noexcept { return errorString(last_error_); }

    /**
     * @brief Optional log sink for diagnostic output.
     *
     * If set, the library prints brief diagnostic lines (probe results,
     * stale snapshots, mode-gate refusals) to @c stream. Pass @c nullptr to
     * disable. Disabled by default.
     *
     * @param[in] stream Any Arduino @c Stream (Serial, Serial1, ...).
     */
    void setLogStream(Stream* stream) noexcept { log_ = stream; }

    /* ============================================================
     * SPEC §B.5 diagnostic counters (for soak / regression testing)
     * ============================================================ */

    /**
     * @brief Total per-byte retry-loop exhaustions since boot or last reset.
     *
     * Incremented in @c readU8 when all @c RBAMP_NACK_RETRY_ATTEMPTS attempts
     * fail for a single-byte read. A non-zero count indicates wire-level
     * trouble — either a slave-side firmware issue or insufficient
     * @c RBAMP_NACK_RETRY_ATTEMPTS for the workload's per-cycle byte count.
     * SPEC §B.5 documents the dense-workload tuning recipe.
     */
    uint32_t retryExhaustionCount() const noexcept { return retry_exhaustion_count_; }

    /**
     * @brief Total sanity-filter rejections since boot or last reset.
     *
     * Incremented in @c readFloatLE when the assembled float fails the
     * SPEC §B.5 loose-sanity check (`!isfinite || |x| > 10000`). A non-zero
     * count after retry+50 kHz mitigation in place usually means the
     * ESP-IDF i2c_master buffer-leak ghost made it past the retry layer
     * (rare but possible at high read density).
     */
    uint32_t sanityRejectCount() const noexcept { return sanity_reject_count_; }

    /**
     * @brief Zero both diagnostic counters.
     *
     * Use at the start of a soak / regression test window to measure only
     * the events inside that window.
     */
    void resetCounters() noexcept {
        retry_exhaustion_count_ = 0;
        sanity_reject_count_ = 0;
    }

    /* ============================================================
     * STD line — block reads (16-bit family map)
     *
     * Every method here fails with RB_ERR_NOT_IMPLEMENTED on a BASIC-line
     * instance. The scalar API above (readCurrent / readPower / readVoltage /
     * ...) works on both lines; use these when you want a whole channel in one
     * burst, or the quantities only the STD line measures (Q / S / THD /
     * signed energy).
     * ============================================================ */

    /**
     * @brief Cached STD identity from begin() — no bus traffic.
     * @return @c false (RB_ERR_NOT_IMPLEMENTED) on a BASIC instance or before begin().
     */
    bool stdInfo(RbAmpStdInfo& out) noexcept;

    /**
     * @brief Read one current channel block (I / P / Q / S / PF / peak / energy /
     *        harmonics) in a single burst.
     * @param[in] ch 0..channels()-1.
     */
    bool stdReadChannel(uint8_t ch, RbAmpStdChannel& out) noexcept;

    /**
     * @brief Read an aggregate block: @p k = 0 grand total (sum of all current
     *        channels), 1..3 per-phase totals A/B/C (map 0x36+; on a
     *        single-phase module k = 1 equals channel 0, k = 2/3 read 0).
     *        I / P / Q / S / PF / peak only: on modules with life-time
     *        counters the block's energy accumulators are not kept and read 0
     *        here — energy comes from stdReadEnergy().
     */
    bool stdReadTotals(uint8_t k, RbAmpStdChannel& out) noexcept;

    /**
     * @brief Read one voltage channel block (RMS / peak / THD / phase angle).
     *
     * The phase angle (degrees vs voltage 0) is published from image 0x7A;
     * @c angle_valid is @c false on older images, for an input the module
     * cannot measure (below 50 V) and on single-voltage modules for v > 0.
     * @param[in] v 0..n_voltage-1 (see stdInfo()).
     */
    bool stdReadVoltage(uint8_t v, RbAmpStdVoltage& out) noexcept;

    /**
     * @brief Phase sequence of a three-voltage module, derived by the module
     *        from the voltage angles (image 0x7A+).
     *
     * Returns @c true with @c RbAmpStdPhaseSeq::NotAvailable — and no bus
     * traffic — on an image that does not publish it; the module itself
     * reports @c NotAvailable with fewer than three voltage inputs or when one
     * of them is below 50 V.
     */
    bool stdReadPhaseSequence(RbAmpStdPhaseSeq& out) noexcept;

    /**
     * @brief Live module health: status bits + sensor frame / CRC-error counters.
     *
     * Treat readings as valid when @c status has @c rbamp_std::ST_SENSOR_OK and
     * @c frame_count grows between two polls.
     * @note @c ST_CAL_OK on image 0x7A+ comes from a marker written by the
     *       module's calibration save: a unit calibrated on an older image
     *       reads cal_ok = 0 until its calibration is saved once more
     *       (saveGains()). The readings themselves are unaffected.
     */
    bool stdReadHealth(RbAmpStdHealth& out) noexcept;

    /* ---- STD latch banks (8 independent master-timed periods) ----
     * Typical use: arm the banks you need once, then per bank on your own
     * schedule: stdLatch(bank) → wait >= 300 ms → stdReadLatch(bank, snap) →
     * stdLatchWh(snap, dt) with the master-measured dt since the previous
     * latch. Bank 0 is also what readPeriodSnapshot() / energy() use. */

    /**
     * @brief Set the accumulate mask: bit b = bank b accumulates (newly armed
     *        banks start clean; disarmed banks freeze, last snapshot stays).
     * @note  Bit 0 must stay set if you use readPeriodSnapshot().
     * @note  The mask lives in module RAM and is lost on any module reset
     *        (warm reset, power cycle, firmware activation). Each bank is
     *        re-armed when a read of it finds an empty snapshot on a disarmed
     *        bank (see stdReadLatch() / readPeriodSnapshot()) — every armed
     *        bank therefore reports the reset once, on its own next read.
     */
    bool stdArmBanks(uint8_t mask) noexcept;

    /**
     * @brief Request snapshot + reset of @p bank at the module's next window
     *        boundary. Does not wait. Read it >= 300 ms later.
     */
    bool stdLatch(uint8_t bank) noexcept;

    /**
     * @brief Read the last snapshot of @p bank (one burst: totals + per-channel
     *        energy for every populated channel).
     * @return @c false with @c RB_ERR_STALE if the module has never latched
     *         this bank (@c out.valid == false), or if the snapshot is empty
     *         (@c win_count == 0) because the bank had lost its arm after a
     *         module reset — the library re-arms and primer-latches that bank
     *         in that case, so your next latch closes a whole period; drop
     *         this one and restart your own dt anchor (the outage interval
     *         carries no data).
     */
    bool stdReadLatch(uint8_t bank, RbAmpStdLatch& out) noexcept;

    /**
     * @brief Convert a snapshot's energy buckets to Wh over the master-measured
     *        period @p dt_ms.
     *
     * Wh = E[mW·samples] / 1000 / 3600 · (dt / nsamp): the sample count and the
     * master's dt remove any dependence on the module's sample rate.
     *
     * @param[in]  snap       Snapshot from stdReadLatch().
     * @param[in]  dt_ms      Master wall-clock between the previous and this latch of the bank.
     * @param[in]  ch         Channel index, or @c RBAMP_STD_TOTAL for the bank totals.
     * @param[out] wh_import  Import energy, Wh (>= 0).
     * @param[out] wh_export  Export energy, Wh (<= 0).
     * @return @c false if @p ch is out of range or @c nsamp is 0 (nothing accumulated).
     */
    static bool stdLatchWh(const RbAmpStdLatch& snap, uint32_t dt_ms, uint8_t ch,
                           double& wh_import, double& wh_export) noexcept;

    /* ---- STD configuration / calibration ----
     * The scalar config API (setSensorClass / setCTModel / configureChannels /
     * readCTModelCh / saveUserConfig / saveGains / prepare+commitAddressChange /
     * readActiveAddress / reset) works on both lines; on STD every command's
     * verdict is read from the module's synchronous error register after its
     * settle (map 0x34+): RB_ERR_PARAM = rejected, module unchanged. On STD the
     * CT model is a REFERENCE (sensor label) — it sets no gain; the module is
     * calibrated per channel at the factory. factoryReset() has no STD
     * counterpart (RB_ERR_NOT_IMPLEMENTED). */

    /**
     * @brief Read the STD configuration block in one burst (sensor class,
     *        channel-0 CT model reference, tier, persisted I2C address,
     *        report-rate hint, harmonics switch).
     */
    bool stdReadConfig(RbAmpStdConfig& out) noexcept;

    /**
     * @brief Enable / disable base-harmonics extraction (THD, displacement PF,
     *        fundamental reactive power). RAM-only (not persisted); readback-
     *        verified. Requires the module's THD capability
     *        (@c rbamp_std::CAP_THD) — else @c RB_ERR_NOT_IMPLEMENTED.
     */
    bool stdSetHarmonics(bool enable) noexcept;

    /**
     * @brief Starting current for energy counting, µA (image 0x7B+). Below
     *        this channel current the module counts no energy for that channel
     *        (import or export, grand and per channel) — at no load the measured
     *        power is noise of either sign and would otherwise creep into both
     *        counters. Readings (I / P / Q) are not affected. Default 400 µA.
     *
     * Applies at once; readback-verified. Persist with saveUserConfig().
     * @param[in] ua 0 = off, or 10..2540 in steps of 10 µA — anything else is
     *               @c RB_ERR_PARAM with no bus write.
     * @return @c false with @c RB_ERR_NOT_IMPLEMENTED when the module has no
     *         starting current (no micro-resolution energy or an older image).
     */
    bool stdSetEnergyStartCurrent(uint16_t ua) noexcept;

    /** @brief Current starting current for energy counting, µA (0 = off); see stdSetEnergyStartCurrent(). */
    bool stdReadEnergyStartCurrent(uint16_t& ua) noexcept;

    /**
     * @brief Capture the no-load raw THD as the sensor floor and persist it
     *        (@c CMD_CAL_THD_FLOOR, ~700 ms). Run with clean mains and NO load;
     *        afterwards THD readings are de-embedded of the sensor floor.
     *        Requires @c rbamp_std::CAP_THD.
     */
    bool stdCalThdFloor() noexcept;

    /* ---- STD firmware update over I2C (two-phase OTA) ----
     * Sequence: stdOtaBegin() → stdOtaWritePage() ×N (256-byte pages; blank
     * 0xFF pages may be skipped) → stdOtaCommit() (verifies, ARMS the swap, no
     * reboot) → stdOtaActivate() (reboots into the new image when you are
     * ready; the module is off the bus for a few seconds, then confirms the
     * new image itself). stdOtaWritePreparedImage() does begin + pages +
     * commit in one call for an image prepared on the manufacturer's build
     * host — the only kind current modules accept. A bad image is rolled back
     * by the module itself; calibration survives. Every step waits for the
     * module to report the command finished (no fixed sleeps). All fail with
     * RB_ERR_NOT_IMPLEMENTED unless the module reports the OTA capability on
     * map 0x33 or later. */

    /** @brief Read the module's OTA state (@c rbamp_std::OTA_IDLE / READY / OK / ERR / DOWNGRADE). */
    bool stdOtaStatus(uint8_t& out) noexcept;

    /**
     * @brief Erase the inactive bank and wait until the module reports READY.
     * @param[in] timeout_ms 0 = default (5 s; the erase takes ~1.2 s).
     * @return @c false with @c RB_ERR_TIMEOUT if READY was not reached.
     */
    bool stdOtaBegin(uint32_t timeout_ms = 0) noexcept;

    /**
     * @brief Write one 256-byte page at index @p page_idx (offset / 256 within the bank).
     *
     * Waits for the module to finish programming the page (one page slot is
     * 250 ms on modules that program in slots), then requires the session to
     * still be open.
     * @return @c false with @c RB_ERR_CHECKSUM if the payload arrived corrupt
     *         (resend the same page), @c RB_ERR_IO if the module failed the
     *         page, @c RB_ERR_NOT_READY if the session is no longer open —
     *         in both of the latter cases restart from stdOtaBegin() (see
     *         stdOtaDiag() for the reason).
     */
    bool stdOtaWritePage(uint32_t page_idx, const uint8_t* page) noexcept;

    /**
     * @brief Verify the written image and arm the bank swap (no reboot).
     * @param[in] timeout_ms 0 = default (3 s).
     * @return @c true armed · @c RB_ERR_CHECKSUM bad image · @c RB_ERR_VERSION
     *         image older than the running one (rejected) · @c RB_ERR_TIMEOUT.
     */
    bool stdOtaCommit(uint32_t timeout_ms = 0) noexcept;

    /**
     * @brief Reboot into the armed image, wait for the module to come back
     *        (~4 s), rediscover it, confirm the running bank changed, then
     *        wait for the new image to confirm itself (map 0x35+: it does so
     *        after ~8 s of healthy metering; any reset before that rolls it
     *        back) and check the running image version is the staged one.
     *
     * Blocks for the reboot and the confirmation (~12 s in all). The instance
     * stays bound; identity is refreshed (stdInfo() reflects the new firmware).
     * @return @c false with @c RB_ERR_NOT_READY if nothing was armed, if the
     *         module came back on the previous bank (bootloader rollback) or
     *         never confirmed the new image; @c RB_ERR_VERSION if the image
     *         that runs afterwards is not the staged one (rolled back).
     */
    bool stdOtaActivate() noexcept;

    /** @brief CRC-32 (zlib-compatible) — the per-page checksum the module verifies while streaming. */
    static uint32_t stdCrc32(const uint8_t* data, size_t len) noexcept;

    /**
     * @brief Host-built (unsigned) image update — not accepted by current
     *        modules. Returns @c false with @c RB_ERR_NOT_IMPLEMENTED without
     *        touching the module: the commit step verifies the image's own
     *        signed header, which only the manufacturer's build host
     *        produces. Use stdOtaWritePreparedImage(). Kept for source
     *        compatibility.
     */
    bool stdOtaWriteImage(const uint8_t* image, size_t len, uint32_t version,
                          RbAmpOtaProgress cb = nullptr, void* ctx = nullptr) noexcept;

    /** @brief Stream form of stdOtaWriteImage() — same refusal. */
    bool stdOtaWriteImage(Stream& image, size_t len, uint32_t version,
                          RbAmpOtaProgress cb = nullptr, void* ctx = nullptr) noexcept;

    /* ---- Prepared (signed) bank images ----
     * Modules accept only images prepared on the manufacturer's build host: a
     * full bank image (size == the module's reported bank size) that already
     * carries its own header, authentication and encryption. The library
     * streams such an image AS-IS — no host-built header, pages in ascending
     * order (page 0 always sent, blank 0xFF pages skipped, the module's own
     * header page last), each page checked by the module against its CRC-32
     * and resent once on a mismatch — then commits. */

    /**
     * @brief Stream a prepared bank image (begin + pages + commit; no activate).
     * @param[in] image Full bank image for the bank that is NOT running.
     * @param[in] len   Must equal the module's bank size — else @c RB_ERR_PARAM.
     * @return @c true armed · @c RB_ERR_CHECKSUM rejected at commit (see
     *         stdOtaDiag()) · @c RB_ERR_VERSION older than the running image.
     */
    bool stdOtaWritePreparedImage(const uint8_t* image, size_t len,
                                  RbAmpOtaProgress cb = nullptr, void* ctx = nullptr) noexcept;

    /** @brief Stream form of stdOtaWritePreparedImage() (one 256-byte page in RAM). */
    bool stdOtaWritePreparedImage(Stream& image, size_t len,
                                  RbAmpOtaProgress cb = nullptr, void* ctx = nullptr) noexcept;

    /**
     * @brief Convenience: give both per-bank images and let the library pick
     *        the one for the bank that is not running (module reports which
     *        bank it runs). @c RB_ERR_NOT_IMPLEMENTED if the module does not
     *        report its running bank id.
     */
    bool stdOtaWritePreparedImage(const uint8_t* bank_a, const uint8_t* bank_b, size_t len,
                                  RbAmpOtaProgress cb = nullptr, void* ctx = nullptr) noexcept;

    /** @brief Running bank id as the module reports it: 0 = A, 1 = B. */
    bool stdOtaActiveBank(uint8_t& out) noexcept;

    /** @brief Version of the armed (committed, not yet activated) image; 0 when nothing is armed. */
    bool stdOtaStagedVersion(uint32_t& out) noexcept;

    /**
     * @brief Reason the module gave for the last OTA failure (module-family
     *        specific codes; e.g. a bad header, checksum, authentication or
     *        signature) plus the low-level status byte. Read after a commit or
     *        page write failed.
     */
    bool stdOtaDiag(uint8_t& reason, uint8_t& hal_status) noexcept;

    /** @brief Close the update session / disarm an armed image without rebooting. */
    bool stdOtaAbort() noexcept;

    /**
     * @brief Refuse an OTA image built for another board before sending it.
     *
     * Compares the module's board id (map 0x36+) with the image's board word
     * (shipped in the image manifest; it cannot be read from the sealed
     * image). A mismatch returns @c RB_ERR_PARAM — otherwise it would surface
     * only at commit (rejection reason BOARD).
     * @return @c false with @c RB_ERR_NOT_IMPLEMENTED below map 0x36.
     */
    bool stdOtaBoardOk(uint32_t image_board_word) noexcept;

    /* ---- STD life-time energy counters (persisted on the module) ---- */

    /**
     * @brief Read the module's life-time energy counters (grand block, plus
     *        the mWh totals and per-channel counters on map 0x36+, plus the
     *        µWh / µvarh counters on modules that publish them).
     *
     * Each block is read in one transaction (the module serves it from one
     * coherent snapshot). The µWh / µvarh fields are always filled — the
     * module's own counters when @c RbAmpStdEnergy::fine is @c true, else
     * derived from the coarse ones — so callers can use them unconditionally.
     * @return @c false with @c RB_ERR_NOT_IMPLEMENTED when the module keeps no
     *         on-device energy; @c RB_ERR_NOT_READY if the block is unprogrammed.
     */
    bool stdReadEnergy(RbAmpStdEnergy& out) noexcept;

    /**
     * @brief Energy between two readings: @c now - @c prev, each counter
     *        clamped at 0 (a counter may resume slightly below the last
     *        reading after a module restart — lost energy, not a
     *        discontinuity); net is recomputed as import - export of the
     *        clamped deltas (never a delta of the module's net field). The same
     *        rules cover the Wh, mWh, µWh, varh, µvarh and per-channel fields.
     *
     * @param[out] delta            May be @c nullptr. @c fine is @c true only when both readings were fine.
     * @param[out] module_restarted May be @c nullptr; @c true when @c now's uptime is below @c prev's.
     * @return @c false when the generation counter changed between the two
     *         readings (life-time reset / adopt): do not account that interval.
     */
    static bool stdEnergyDelta(const RbAmpStdEnergy& prev, const RbAmpStdEnergy& now,
                               RbAmpStdEnergy* delta, bool* module_restarted) noexcept;

    /* ---- STD identity, clock, commands (map 0x35+ unless noted) ---- */

    /** @brief Identity of this module: cached begin() data + UID + image version (0x35+) + board id (0x36+). */
    bool stdIdentify(RbAmpStdIdentity& out) noexcept;

    /**
     * @brief Identify whatever answers at @p addr without an instance (bus
     *        scan): the STD identity block, UID, image version, board id.
     *        One try per read, no retry layer — a NACK is the answer.
     * @return @c RB_OK; @c RB_ERR_NACK nothing acknowledges there;
     *         @c RB_ERR_VERSION something that is not an STD module;
     *         @c RB_ERR_PARAM address outside 0x08..0x77; @c RB_ERR_IO a later
     *         read failed.
     */
    static int8_t stdProbe(TwoWire& bus, uint8_t addr, RbAmpStdIdentity& out) noexcept;

    /** @brief Running image version (not the register-map version); 0 if the module does not report one. */
    bool stdReadImageVersion(uint32_t& out) noexcept;

    /** @brief Board id of the running image (map 0x36+). */
    bool stdReadBoardId(uint32_t& out) noexcept;

    /**
     * @brief Set the module clock (unix epoch, UTC). The clock is lost at
     *        every module restart — call after begin() and whenever
     *        RbAmpStdHealth::time_valid is false; the module records no
     *        history without it.
     * @return @c false with @c RB_ERR_PARAM if the module refuses the epoch
     *         (before 2020), @c RB_ERR_NOT_READY if it accepted but did not
     *         report the clock valid within 300 ms.
     */
    bool stdSetTime(uint32_t unix_epoch) noexcept;

    /** @brief Module clock and whether it was set since the module's last restart. */
    bool stdReadTime(uint32_t& unix_epoch, bool& valid) noexcept;

    /**
     * @brief Issue any module command and wait for its verdict: waits until
     *        the module is idle (a command written while busy is dropped),
     *        writes the opcode, polls the command state until the module
     *        reports it finished, reads the verdict. @p settle_ms only caps
     *        the first poll (the map settle is the completion signal only on
     *        modules without a command-state register, map < 0x35).
     * @param[in] timeout_ms 0 = 1000; never less than 1000.
     * @return @c false with the verdict mapped to an @c RB_ERR_* code (PARAM,
     *         NOT_IMPLEMENTED = unknown command, CHECKSUM = value check,
     *         MODE = key / legal gate, NOT_READY = rate limit / retry,
     *         TIMEOUT, IO = module storage error).
     */
    bool stdCommand(uint8_t opcode, uint32_t settle_ms, uint32_t timeout_ms = 0) noexcept;

    /** @brief Wait until the module reports no command in progress (@p timeout_ms 0 = 1000). */
    bool stdWaitIdle(uint32_t timeout_ms = 0) noexcept;

    /** @brief Blink the module's LEDs for ~10 s (locate it in a cabinet). */
    bool stdLocate() noexcept;

    /**
     * @brief Give one module among several on a shared address its own
     *        address, selected by UID (commissioning). Write-only on the
     *        shared address (several modules may answer, so nothing is read
     *        there); the addressed module persists the new address and
     *        restarts; the call then probes the new address and checks the UID.
     *        Re-run begin() on an instance bound to it afterwards.
     * @param[in] restart_ms How long the module takes to come back (0 = 1500).
     * @return @c RB_OK; @c RB_ERR_PARAM address out of range; @c RB_ERR_NACK no
     *         module acknowledged on the shared address; @c RB_ERR_NOT_READY
     *         nothing answers at the new address afterwards; @c RB_ERR_VERSION
     *         the module there is not the one addressed (UID differs).
     */
    static int8_t stdSetAddressByUid(TwoWire& bus, uint8_t shared_addr, const uint8_t uid[12],
                                     uint8_t new_addr, uint32_t restart_ms = 0) noexcept;

    /* ---- STD history and event log (map 0x35+) ---- */

    /** @brief Ring summary; records are fetched newest-first by @c back index. */
    bool stdHistInfo(RbAmpStdHistPeriod period, RbAmpStdHistInfo& out) noexcept;

    /**
     * @brief Fetch and decode one energy-period record (@c Min15 / @c Hour;
     *        @p back 0 = newest).
     * @return @c false with @c RB_ERR_NOT_READY when there is no such record
     *         (empty ring or @p back beyond it); @c RB_ERR_PARAM for the
     *         event ring (use stdEventRead()).
     */
    bool stdHistRead(RbAmpStdHistPeriod period, uint16_t back, RbAmpStdHistRecord& out) noexcept;

    /** @brief Fetch one raw record (80 bytes) of any ring; @p flags (optional) = the module's record flags. */
    bool stdHistReadRaw(RbAmpStdHistPeriod period, uint16_t back,
                        uint8_t rec[rbamp_std::REG_HIST_REC_SIZE], uint8_t* flags) noexcept;

    /** @brief Fetch and decode one event-log record (@p back 0 = newest); @c RB_ERR_NOT_READY = no such record. */
    bool stdEventRead(uint16_t back, RbAmpStdEvent& out) noexcept;

    /** @brief Ask the module to write its energy accumulators to its storage now. */
    bool stdHistFlush() noexcept;

private:
    /* --- Low-level I/O (every byte = separate I2C address phase per SPEC §6) --- */
    bool      writeReg(uint8_t reg, uint8_t val) noexcept;
    bool      writeCmd(uint8_t cmd) noexcept;       /**< Helper: writeReg(REG_COMMAND, cmd). */
    bool      readU8(uint8_t reg, uint8_t& out) noexcept;
    bool      readU16LE(uint8_t reg, uint16_t& out) noexcept;
    bool      readU32LE(uint8_t reg, uint32_t& out) noexcept;
    /* readFloatLE accepts an optional per-quantity magnitude ceiling. Defaults
     * to 30000.0 (the max of any physical quantity in the rbAmp public API:
     * P/Q ≤ 30 kW; U ≤ 500 V; I ≤ 150 A; PF ≤ 1.5). Per-quantity helpers below
     * pass tight values so a 11 kW load doesn't trip the global filter (was
     * 10000 — truth-doc §16.2 #2 cross-lib parity fix). */
    bool      readFloatLE(uint8_t reg, float& out,
                          float max_abs = 30000.0f) noexcept;
    bool      registerAcks(uint8_t reg) noexcept;   /**< Used by variant detect. */

    /* A/B torn-read defense for correctness-critical decided values (address,
     * GC_TICK, CT-model mirror). On a shared open-drain bus a torn read can
     * return a corrupt mix; read twice, and on disagreement read a third time
     * and take the 2-of-3 majority. All-three-disagree → sanity_reject_count_++
     * and false. Single-byte NACK-retry runs underneath each sub-read. */
    bool      readU8AB(uint8_t reg, uint8_t& out) noexcept;
    bool      readU16AB(uint8_t reg, uint16_t& out) noexcept;

    /* Known-code check against the generated registry (RbAmpSensorModels.h ←
     * sensor_models.yaml). v1.5.0: replaced the hand-maintained per-class
     * whitelist (which had drifted — WIRED_CT was missing entirely). A registry
     * hit does NOT imply the module accepts the code — acceptance is runtime
     * truth (module returns ERR_PARAM, channel keeps its previous model). This
     * only fast-fails codes absent from the registry (typos). */
    static bool ctModelValid(RbAmpSensorClass cls, uint8_t code) noexcept;

    /* Read-before-write policy for the sensor class / CT model setters. The
     * firmware clears the models on ANY class write and re-applies the generic
     * preset on ANY model write, so an unchanged value must not reach the bus,
     * and a factory-paired module (class 2 / 3) is not reconfigured without
     * the explicit force path. Pure functions — host-tested. */
    enum class WritePolicy : uint8_t { Proceed, Noop, Paired };
    static bool        sensorClassPaired(uint8_t cls) noexcept;
    static WritePolicy sensorClassWritePolicy(uint8_t cur, uint8_t want) noexcept;
    static WritePolicy ctModelWritePolicy(uint8_t cls, uint8_t held, uint8_t want,
                                          bool force) noexcept;
    /* Reads class + the channel's applied model and applies ctModelWritePolicy.
     * skip = nothing to write (RB_OK). false = refused (RB_ERR_MODE) or a
     * transport error (error set). */
    bool      ctModelGuard(uint8_t channel, uint8_t code, bool force, bool& skip) noexcept;
    bool      setCtModelImpl(uint8_t channel, uint8_t code, bool force) noexcept;

    /* Post-bind mirror verify (configureChannels). Returns RB_OK if the applied
     * mirror matches; RB_ERR_PARAM if stable-wrong (caller re-binds once);
     * RB_ERR_NON_PHYSICAL if the mirror read stayed torn (trust the accepted CMD). */
    int8_t    verifyCtBind(uint8_t channel, uint8_t expected) noexcept;

    /* --- Helpers --- */
    void      detectVariant() noexcept;             /**< Called by begin(). */
    void      setError(int8_t err) noexcept;        /**< Updates last_error_ + optional log. */
    float     readRtFloat(uint8_t reg, uint8_t ch, uint8_t stride = 4,
                          float max_abs = 30000.0f) noexcept;
    /* Channel access window (senior SKUs, ch>=3): re-select 0x32=(field<<4)|ch
     * then read the 0x3C..0x3F float32 snapshot. Re-selects on EVERY call
     * (contract rule #1 — the data buffer is the value at select time). */
    float     readChannelWindow(uint8_t ch, uint8_t field, float max_abs) noexcept;
    /* Single routing point for the per-channel scalar quantities (I_RMS /
     * I_PEAK / P / PF / Q / S) — ONE table (kQty in RbAmp.cpp) + ONE router, so
     * a new quantity is a table row and no public accessor can reintroduce its
     * own routing (L8). STD: fixed-point field in the CURRENT block, scaled to
     * SI. BASIC: ch0-2 flat register (flat_base + ch*4), ch3+ channel window; a
     * quantity the BASIC line lacks fails with RB_ERR_NOT_IMPLEMENTED. */
    float     readQty(uint8_t q, uint8_t ch) noexcept;

    /* --- STD line transport: 16-bit big-endian pointer, auto-increment ---
     * A write is [ptr_hi, ptr_lo, data...]; a read is [ptr_hi, ptr_lo] then a
     * repeated-start read of n bytes. Bursts are chunked to the Wire buffer
     * (128 bytes on ESP32 = one coherent module snapshot per transaction; 32
     * on the classic AVR buffer; RBAMP_STD_BURST_MAX overrides) with an
     * explicit pointer per chunk. Same NACK-retry discipline as the 8-bit
     * primitives, for writes as well as reads — except a COMMAND write, which
     * is repeated only when the module provably did not take it. */
    bool      stdTxn(const uint8_t* wbuf, uint8_t wlen, uint8_t* rbuf, uint8_t rlen) noexcept;
    bool      stdTxnN(const uint8_t* wbuf, uint8_t wlen, uint8_t* rbuf, uint8_t rlen,
                      uint8_t attempts) noexcept;
    bool      stdRead(uint16_t reg, uint8_t* buf, size_t n) noexcept;
    bool      stdReadCoherent(uint16_t reg, uint8_t* buf, size_t n) noexcept;
    bool      stdWrite(uint16_t reg, const uint8_t* data, size_t n) noexcept;
    bool      stdReadU8(uint16_t reg, uint8_t& out) noexcept;
    bool      stdReadU16(uint16_t reg, uint16_t& out) noexcept;
    bool      stdReadU32(uint16_t reg, uint32_t& out) noexcept;
    bool      stdWriteU8(uint16_t reg, uint8_t val) noexcept;
    bool      stdWriteU16(uint16_t reg, uint16_t val) noexcept;
    bool      stdStartCurrentGate() noexcept;
    bool      stdCmd(uint8_t opcode) noexcept;
    /* Synchronous command verdict (map 0x34+): read ERROR after the settle. */
    bool      stdCheckCmdError() noexcept;
    /* One scaled fixed-point field (i16 or i32 LE) as float, same plausibility
     * gate as the BASIC float readers (RB_ERR_NON_PHYSICAL on reject). */
    bool      stdReadScaled(uint16_t addr, bool is_i16, float scale,
                            float max_abs, float& out) noexcept;
    void      stdSane(float& v, float max_abs, uint8_t& mask, uint8_t bit) noexcept;
    bool      stdReadIpBlock(uint16_t base, RbAmpStdChannel& out) noexcept;
    /* Identity probe (issued FIRST in begin()) + STD-side begin(). */
    bool      stdDetect(bool& is_std) noexcept;
    bool      stdBegin() noexcept;
    static RbAmpVariant stdVariantMap(uint8_t v) noexcept;
    bool      stdReadAll(RbAmpSnapshot& out) noexcept;
    /* Latch: target bank register first, opcode second (two transactions). */
    bool      stdLatchBank(uint8_t bank) noexcept;
    /* E15: the arm mask is module RAM and is lost on any module reset. On an
     * empty snapshot, check the bank's OWN arm bit; if it is gone, warn, re-arm
     * that bank only, primer-latch it and report true (caller drops the outage
     * period). Per bank, so every armed bank gets its own reset signal. */
    bool      stdRearmIfLost(uint8_t bank) noexcept;
    /* (Re)start the library's period path on bank 0: arm, primer latch, anchor
     * the master clock. begin(), after the library's own resets (reset(),
     * address change), and after a detected module reset. */
    bool      stdPrimePeriod() noexcept;
    /* Module reboot INSIDE a period (arm restored by a newer firmware or another
     * master, so the arm check cannot see it): the sensor frame counter went
     * backwards since the anchor. Reads FRAME_CNT, updates the cache. */
    bool      stdRebootedSinceAnchor() noexcept;
    bool      stdReadLatchBank(uint8_t bank, RbAmpStdLatch& out) noexcept;
    bool      stdReadPeriodSnapshot(RbAmpPeriodSnapshot& out, uint16_t settle_ms,
                                    bool skip_latch) noexcept;
    /* STD CT-model reference write (CHMAP_MODEL(ch) [+ CFG_CT_MODEL for ch0]),
     * class-precondition + registry check + readback; caller saves. */
    bool      stdApplyCtModel(uint8_t channel, uint8_t code) noexcept;
    bool      stdWriteU32(uint16_t reg, uint32_t val) noexcept;
    /* OTA: capability/map gate, status poll (accept = bit per OTA_* value),
     * shared image loop (buffer or Stream source), incremental CRC step. */
    bool      stdOtaGate() noexcept;
    bool      stdOtaWait(uint32_t timeout_ms, uint8_t accept, uint8_t& last) noexcept;
    bool      stdOtaWritePreparedImpl(const uint8_t* buf, Stream* stream, size_t len,
                                      RbAmpOtaProgress cb, void* ctx) noexcept;
    /* Fetch exactly n bytes of page data from the buffer or the Stream. */
    bool      stdOtaFetchPage(const uint8_t* buf, Stream* stream, size_t off, size_t n,
                              uint8_t* page) noexcept;
    /* Wait until the module has finished executing the last OTA command: the
     * command state's BUSY bit on map 0x35+; before that ERROR reads PENDING
     * and/or OTA_STATUS reads BUSY while it runs; a transport error means a
     * flash stall. */
    bool      stdOtaWaitCmdDone(uint32_t timeout_ms, uint8_t opcode) noexcept;
    void      stdOtaLogDiag() noexcept;
    static uint32_t stdCrc32Update(uint32_t crc, const uint8_t* data, size_t len) noexcept;

    /* STD map 0x35+ / 0x36 helpers */
    bool      stdGate35() noexcept;
    bool      stdGate36() noexcept;
    bool      stdWaitIdleImpl(uint32_t timeout_ms, uint8_t opcode) noexcept;
    bool      stdCmdSync(uint8_t opcode, uint32_t settle_ms, uint32_t timeout_ms) noexcept;
    bool      stdSaveSync(uint8_t opcode, uint32_t settle_ms) noexcept;
    bool      stdWaitReappear(uint8_t addr, uint32_t timeout_ms) noexcept;
    bool      stdReadEnergyRaw(RbAmpStdEnergy& out) noexcept;
    bool      stdLtAnchor() noexcept;
    bool      stdReadPeriodCounters(RbAmpPeriodSnapshot& out) noexcept;
    bool      stdHistSelect(RbAmpStdHistPeriod period, uint16_t back, bool with_back) noexcept;

    /* DRDY helpers */
    bool      drdyCanAlarm() const noexcept;
    bool      drdyCheckSeq(RbAmpDrdyEvent& ev, bool& fresh) noexcept;
    void      drdyServiceAlarm(RbAmpDrdyEvent& ev) noexcept;
    void      drdyDetach() noexcept;
    /* Per-channel CT model write via the window (field 15). Select + four
     * single-byte data writes (LSB=code); no burst. Requires fw ver 0x0A.
     * Caller validates channel and firmware version. */
    bool      writeChannelModel(uint8_t ch, uint8_t code) noexcept;
    /* readAll folding: on a sanity-reject (RB_ERR_NON_PHYSICAL) set field=NaN +
     * flag the implausible bit and keep going; on transport failure return false. */
    bool      foldField(bool read_ok, float& field, uint8_t& mask, uint8_t bit) noexcept;
    /* readAll helper for ch>=3: select the window field, read it, and fold with
     * the same implausible-vs-transport semantics as foldField. Returns false
     * only on a transport failure (caller aborts); a sanity-reject folds to
     * NaN+flag and returns true. */
    bool      foldChannelWindow(uint8_t ch, uint8_t field, float& out_field,
                                uint8_t& mask, uint8_t bit, float max_abs) noexcept;
    static uint8_t addressForCurrentReg(uint8_t base, uint8_t ch) noexcept {
        return static_cast<uint8_t>(base + ch * 4);
    }
    /* SCT-013 rating correction (BASIC fw <= 1.4.19) — see sctRatingRefresh(). */
    void      sctRatingClear() noexcept;
    bool      sctReadChannel(uint8_t ch, uint8_t& model, float& gain, uint16_t& nf) noexcept;
    bool      sctRatingCompute(bool& reset_issued) noexcept;
    bool      sctCorrectionApplies(bool& applies) noexcept;
    bool      sctRestartModule() noexcept;
    bool      sctAfterModelWrite() noexcept;
    float     sctMaxScale() const noexcept;
    /* Primer latch + master clock anchor of the BASIC period path (begin(),
     * and again after a module restart issued by the library). */
    bool      basicPrimePeriod() noexcept;

    /* --- State --- */
    TwoWire&       bus_;
    uint8_t        addr_;
    RbAmpTopology  topology_;
    uint8_t        channels_;
    bool           has_voltage_hw_;
    RbAmpVariant   variant_;       /**< Cached REG_HW_VARIANT (0x55), set in begin(). */
    uint16_t       capability_;    /**< Cached REG_CAPABILITY (0x57), set in begin(). */
    int8_t         last_error_;
    Stream*        log_;

    /* Product line — set by begin() from the identity probe. While std_ is
     * true the 8-bit primitives (writeReg / readU8) refuse, so a BASIC-only
     * path can never run an 8-bit transaction on an STD module (fail-safe, not
     * "plausible wrong numbers"). gate_refused_ lets the caller's follow-up
     * setError(RB_ERR_IO) keep the RB_ERR_NOT_IMPLEMENTED verdict. */
    bool           std_;
    bool           gate_refused_;
    uint8_t        std_version_;
    uint16_t       std_caps_;
    uint8_t        std_status_;
    uint8_t        n_voltage_;
    uint8_t        n_current_;
    uint8_t        std_variant_;
    uint32_t       std_active_bank_;
    uint32_t       std_ota_bank_size_;   /**< Device-reported; 0 = no usable OTA bank. */
    uint32_t       std_frame_cnt_;       /**< FRAME_CNT at the last period anchor (reboot-inside-period detect). */
    bool           std_have_frame_cnt_;
    uint32_t       std_fw_ver_;          /**< Running image version (map 0x35+), 0 if unknown. */
    RbAmpStdPeriodSource std_period_src_;/**< Period / energy mechanism chosen from the capability bits. */
    RbAmpStdEnergy std_lt_anchor_;       /**< Counter path: the reading each period is measured from. */
    bool           std_lt_anchored_;
    float          std_last_avg_total_;  /**< Counter path: average power of the last consumed period. */
    bool           std_have_last_avg_;

    /* DRDY / events */
    int8_t         drdy_line_;           /**< Index into the shared line table, -1 = no line (polling). */
    uint32_t       drdy_seen_edges_;     /**< Line edge count already consumed by this instance. */
    uint16_t       drdy_last_seq_;
    bool           drdy_have_seq_;
    uint8_t        drdy_pending_flags_;  /**< Flags another instance consumed for this module on a shared line. */
    uint8_t        begin_event_flags_;   /**< Event flags as found by begin(). */
    RbAmpSctRating sct_[RBAMP_MAX_CHANNELS];  /**< SCT-013 rating correction per channel. */
    bool           sct_reset_seen_;      /**< RESET_OCCURRED already acted on since it was last cleared. */

    /* Period-metering state — master-tracked wall-clock for energy integration.
     *
     * Multi-module pattern requires two timestamps (per truth-doc §16.2 #1):
     *   - prev_latch_ms_     = wall-clock of the last CONSUMED (valid) latch;
     *                          rolled forward ONLY on a valid snapshot, HELD on
     *                          STALE (chip preserves its accumulator — OI-3)
     *   - current_latch_ms_  = wall-clock at the most recent latch issue,
     *                          stamped inside latchPeriod() AND inside
     *                          readPeriodSnapshot() (the non-skip path).
     *
     * dt for energy integration = current - prev. Single-timestamp tracking
     * would collapse to (settle_ms) in the canonical multi-module sequence:
     *   for m: latchPeriod(m)   // each stamps own current+prev (BAD: single var)
     *   sleep(50)
     *   for m: readPeriodSnapshot(m, skip_latch=true)  // reports dt≈50 ms.
     */
    uint32_t       prev_latch_ms_;
    uint32_t       current_latch_ms_;
    bool           have_prev_latch_;

    /* Address-change two-step state */
    uint8_t        pending_addr_;
    uint32_t       pending_addr_armed_ms_;
    bool           addr_change_armed_;

    /* Energy accumulator (per-device) */
    RbAmpEnergy    energy_;

    /* SPEC §B.5 diagnostic counters — incremented in readU8 / readFloatLE.
     * Public accessors via retryExhaustionCount() / sanityRejectCount();
     * zeroed via resetCounters(). */
    uint32_t       retry_exhaustion_count_;
    uint32_t       sanity_reject_count_;
};

#endif /* RBAMP_H */
