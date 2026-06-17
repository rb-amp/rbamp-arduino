/**
 * @file    RbAmp.h
 * @brief   Arduino client library for the rbAmp I2C sensor/dimmer module —
 *          public API.
 * @author  rbAmp team
 * @date    2026
 * @version 1.0.0
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
 *  - No I2C auto-increment: each multi-byte register is read with one address
 *    phase per byte (SPEC §6). Slow but correct.
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

#include <Arduino.h>
#include <Wire.h>
#include <stdint.h>

#include "RbAmpRegisters.h"    /* auto-generated; do not edit — namespace rbamp:: (V1 bridge) */
#include "RbAmpRegistersV2.h" /* auto-generated; do not edit — namespace rbamp::v2:: (v1.3 surface) */
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
     * @brief Probe the device, set topology from constructor hint, run primer LATCH.
     *
     * Performs:
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
     * Useful at boot when the device may need up to 200 ms to commit its
     * first RT measurement window.
     *
     * @param[in] timeout_ms Maximum wait in ms (default 1000).
     * @return @c true if valid bit observed; @c false on timeout.
     */
    bool waitReady(uint32_t timeout_ms = 1000) noexcept;

    /** @return Firmware version byte (REG_VERSION, 0x03). Returns 0 on failure. */
    uint8_t firmwareVersion() noexcept;

    /** @return Detected topology (cached from begin()). */
    RbAmpTopology topology() const noexcept { return topology_; }

    /** @return Number of valid current channels (1..3). */
    uint8_t channels() const noexcept { return channels_; }

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
     * Real-time reads (SPEC §12 — RT block, 200 ms refresh)
     * ============================================================ */

    /**
     * @brief Read instantaneous RMS voltage (REG_V03_U_RMS, 0x86).
     * @param[in] phase Phase index (only 0 supported in v1.0).
     * @return Voltage in V, or @c NAN on failure.
     */
    float readVoltage(uint8_t phase = 0) noexcept;

    /**
     * @brief Read instantaneous peak voltage (REG_V03_U_PEAK, 0x8A).
     * @param[in] phase Phase index (only 0 supported in v1.0).
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
     * @brief Read mains frequency (REG_AC_FREQ, 0x20).
     * @return 50.0 or 60.0 Hz, or @c NAN on failure.
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

    /* ============================================================
     * Configuration (SPEC §10, §11)
     * ============================================================ */

    /**
     * @brief Set the current-sensor family and persist to flash.
     *
     * Writes REG_SENSOR_CLASS (0x25), issues CMD_SAVE_GAINS (0x26), waits 700 ms
     * for flash erase to complete. Blocking.
     *
     * On v1.2+ firmware this must be called BEFORE setCTModel() — otherwise
     * setCTModel() returns false with @c RB_ERR_PARAM. The chosen class also
     * resets @c REG_CT_MODEL to 0 device-side (prevents stale class/model
     * bleed across an installer's two-step sequence).
     *
     * On v1.0/v1.1 firmware the register exists in the firmware register
     * table but has no functional effect — write is harmless and ignored.
     *
     * @param[in] cls RbAmpSensorClass::Sct013 (only shipping value as of v1.2).
     * @return @c true on success.
     */
    bool setSensorClass(RbAmpSensorClass cls) noexcept;

    /**
     * @brief Set the SCT-013 CT model on channel 0 (legacy single-arg form).
     *
     * Writes REG_CT_MODEL (0x05), issues CMD_SAVE_GAINS (0x26), waits 700 ms
     * for flash erase to complete. Blocking.
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
     * @param[in] code 1=SCT_013_005, 2=-010, 3=-030, 4=-050, 5=-100.
     * @return @c true on success.
     */
    bool setCTModel(uint8_t code) noexcept;

    /**
     * @brief Typed-enum overload of @c setCTModel(uint8_t code).
     *
     * Equivalent to @c setCTModel(static_cast<uint8_t>(model)). Prefer this
     * form in user code for type safety and self-documenting call sites:
     * @code
     * dev.setCTModel(RbAmpCTModel::Sct013_030);    // ← clear
     * dev.setCTModel(3);                            // ← legacy, still works
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
     * @brief Set the SCT-013 CT model on a specific channel (v1.2+ firmware).
     *
     * Sequence: writes @c REG_CT_MODEL, issues
     * @c CMD_SET_CT_MODEL_CH0/CH1/CH2 (0x28/0x29/0x2A) per @p channel, waits
     * 5 ms settle for the in-RAM preset-table lookup, then issues
     * @c CMD_SAVE_GAINS for flash persistence (700 ms erase). Blocking ~705 ms
     * per call.
     *
     * @warning Multi-channel call order matters. Writing @c REG_CT_MODEL also
     * triggers the device-side legacy direct-write callback which applies the
     * preset to channel 0 unconditionally. So @c setCTModel(1, code) writes
     * @c code's preset to channel 1 AS INTENDED, but also clobbers channel 0
     * to the same preset as a side-effect. To configure all channels with
     * different models, **call the higher channel indices FIRST**:
     * @code
     * dev.setCTModel(2, 5);   // ch2 = SCT-013-100  (also clobbers ch0 → preset 5)
     * dev.setCTModel(1, 3);   // ch1 = SCT-013-030  (also clobbers ch0 → preset 3)
     * dev.setCTModel(0, 1);   // ch0 = SCT-013-005  (final ch0 preset)
     * @endcode
     * Final state: ch0=preset 1, ch1=preset 3, ch2=preset 5. ✓
     *
     * Requires @c firmwareVersion() >= 0x03 (v1.2). Returns @c RB_ERR_VERSION
     * on older firmware. Same @c RB_ERR_PARAM guard as the single-arg form
     * applies (sensor class must be set first).
     *
     * @param[in] channel 0..2.
     * @param[in] code    1=SCT_013_005, 2=-010, 3=-030, 4=-050, 5=-100.
     * @return @c true on success.
     */
    bool setCTModel(uint8_t channel, uint8_t code) noexcept;

    /**
     * @brief Typed-enum overload of @c setCTModel(channel, code).
     *
     * Equivalent to @c setCTModel(channel, static_cast<uint8_t>(model)).
     * Same descending-order requirement applies; same @c RB_ERR_VERSION
     * gate on firmware < v1.2.
     *
     * @param[in] channel 0..2.
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
     * @return @c true on success.
     */
    bool saveGains() noexcept;

    /**
     * @brief Persist user-config to flash — CMD_SAVE_USER_CONFIG (production-OK).
     *
     * Saves the user-config namespace (ct_model / sensor_class / fleet_config /
     * group_id / label). Unlike saveGains(), this is NOT develop-gated — it
     * works on production modules. Also clears a fresh module's first-boot
     * FLASH_PARAMS_BAD (0xFB) error. ~700 ms (flash erase + write).
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
     *       (truth-doc §6.1) — it is NOT develop-gated. Field-swapping a
     *       production spare to a new bus address is supported. (The legacy
     *       develop-mode + SAVE_GAINS path has been removed.)
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
     * @note Production-OK — not develop-gated (v1.3). The RESET write failing
     *       is non-fatal: the device adopts the committed address on its next
     *       power cycle regardless.
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
     * Soft-reboot the device.
     *
     * @return @c true if the write succeeded.
     */
    bool reset() noexcept;

    /* ============================================================
     * Static / multi-module
     * ============================================================ */

    /**
     * @brief I2C General-Call broadcast LATCH — RESERVED for v2 firmware.
     *
     * @warning v1 rbAmp firmware DISABLES General-Call (SPEC §9), so this
     *          method returns @c false WITHOUT touching the bus. Callers
     *          must fall back to per-device sequential @c latchPeriod()
     *          (see example 03_MultiModuleBroadcast for the recommended
     *          skew-tolerant pattern).
     *
     * Intended behaviour (v2): writes @c [REG_COMMAND=0x01, CMD_LATCH_PERIOD=0x27]
     * to general-call address 0x00, latching every rbAmp on the bus within
     * microseconds. Master then times wall-clock dt and calls
     * @c readPeriodSnapshot(snap, settle_ms, true) on each device.
     *
     * @param[in,out] bus Wire bus (unused in v1 — accepted for API parity).
     * @return Always @c false on v1 firmware; check @c lastError() (per-instance
     *         API only — broadcastLatch is static and cannot set it directly).
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
     * @param[out] out 7-bit address currently held by the device.
     * @return @c true on success.
     */
    bool readActiveAddress(uint8_t& out) noexcept;

    /* ============================================================
     * Diagnostics
     * ============================================================ */

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

    /* Per-class CT-model accepted-set validation (A1). NON-contiguous, per the
     * esp-idf reference _ct_model_valid: SCT013 {1,2,3,4,6}, WIRED_CT {1,2,3},
     * BUILTIN_CT {} (codes 5/7 uncharacterised → reject). Firmware is the
     * ultimate authority; this is a client-side fast-fail only. */
    static bool ctModelValid(RbAmpSensorClass cls, uint8_t code) noexcept;

    /* Post-bind mirror verify (configureChannels). Returns RB_OK if the applied
     * mirror matches; RB_ERR_PARAM if stable-wrong (caller re-binds once);
     * RB_ERR_NON_PHYSICAL if the mirror read stayed torn (trust the accepted CMD). */
    int8_t    verifyCtBind(uint8_t channel, uint8_t expected) noexcept;

    /* --- Helpers --- */
    void      detectVariant() noexcept;             /**< Called by begin(). */
    void      setError(int8_t err) noexcept;        /**< Updates last_error_ + optional log. */
    float     readRtFloat(uint8_t reg, uint8_t ch, uint8_t stride = 4,
                          float max_abs = 30000.0f) noexcept;
    /* readAll folding: on a sanity-reject (RB_ERR_NON_PHYSICAL) set field=NaN +
     * flag the implausible bit and keep going; on transport failure return false. */
    bool      foldField(bool read_ok, float& field, uint8_t& mask, uint8_t bit) noexcept;
    static uint8_t addressForCurrentReg(uint8_t base, uint8_t ch) noexcept {
        return static_cast<uint8_t>(base + ch * 4);
    }

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
