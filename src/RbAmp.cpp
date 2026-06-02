/**
 * @file    RbAmp.cpp
 * @brief   Implementation of the Arduino client class for rbAmp.
 * @author  rbAmp team
 * @date    2026
 *
 * @details
 * All wire-level protocol invariants from @c libs/spec/SPEC.md are enforced
 * here:
 *  - One I2C address phase per byte (SPEC §6).
 *  - 50 ms settle after CMD_LATCH_PERIOD (SPEC §7).
 *  - 700 ms settle after CMD_SAVE_GAINS (SPEC §11).
 *  - REG_V03_PERIOD_VALID checked before consuming a snapshot.
 *  - Two-step address change with 5 s arm window (SPEC §10).
 *
 * @see RbAmp.h for the public API.
 */
#include "RbAmp.h"

#include <string.h>  /* memcpy */
#include <math.h>    /* NAN */

using namespace rbamp;

/* ============================================================================
 * Construction
 * ============================================================================ */

RbAmp::RbAmp(TwoWire& bus, uint8_t addr, RbAmpTopology hint) noexcept
    : bus_(bus),
      addr_(addr),
      topology_(hint),
      channels_(hint == RbAmpTopology::Single      ? 1 :
                hint == RbAmpTopology::SplitPhase  ? 2 : 3),
      has_voltage_hw_(false),
      last_error_(RB_OK),
      log_(nullptr),
      last_latch_ms_(0),
      have_last_latch_(false),
      pending_addr_(0),
      pending_addr_armed_ms_(0),
      addr_change_armed_(false),
      energy_(),
      retry_exhaustion_count_(0),
      sanity_reject_count_(0) {}

/* ============================================================================
 * Lifecycle (SPEC §12)
 * ============================================================================ */

bool RbAmp::begin() noexcept {
#if defined(RBAMP_ESP32_CLAMP_HZ) && (defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM))
    /* Opt-in: clamp the I²C bus clock on ESP32 before any wire traffic.
     * Motivation: the Arduino-ESP32 i2c_master driver has an upstream
     * buffer-leak ghost at higher clock rates (SPEC §B.5). User opts in
     * by defining @c RBAMP_ESP32_CLAMP_HZ before @c #include <RbAmp.h>:
     *
     *     #define RBAMP_ESP32_CLAMP_HZ 50000
     *     #include <RbAmp.h>
     *
     * Library does not override the user's choice on non-ESP32 platforms
     * or when the macro is not defined. */
    bus_.setClock(RBAMP_ESP32_CLAMP_HZ);
#endif

    /* 1. Probe REG_VERSION to confirm the device is alive at addr_ */
    uint8_t version = 0;
    if (!readU8(REG_VERSION, version)) {
        setError(RB_ERR_NACK);
        return false;
    }
    if (version == 0 || version == 0xFF) {
        setError(RB_ERR_VERSION);
        return false;
    }

    /* 2. Auto-detect variant (channels + voltage hardware) */
    detectVariant();

    /* 3. Primer LATCH — discard first accumulator window so subsequent reads
     *    start from a clean baseline. Per SPEC §7 begin() flow.
     *
     *    Stamp last_latch_ms_ RIGHT AFTER the bus write (before the settle)
     *    so the first user-visible readPeriodSnapshot() reports a
     *    master_dt_ms consistent with subsequent cycles — both sides of the
     *    diff are captured immediately after their respective LATCH writes. */
    if (!writeCmd(CMD_LATCH_PERIOD)) {
        setError(RB_ERR_IO);
        return false;
    }
    last_latch_ms_ = millis();
    have_last_latch_ = true;
    delay(SETTLE_MS_LATCH_PERIOD);

    setError(RB_OK);
    return true;
}

bool RbAmp::probe() noexcept {
    uint8_t version = 0;
    if (!readU8(REG_VERSION, version)) {
        setError(RB_ERR_NACK);
        return false;
    }
    if (version == 0 || version == 0xFF) {
        setError(RB_ERR_VERSION);
        return false;
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::waitReady(uint32_t timeout_ms) noexcept {
    /* Poll REG_STATUS (0x00) bit0=READY — non-destructive, sticky.
     * NOT REG_V03_STATUS (0xCE) which is "cleared on read" and races with
     * the firmware commit-thread (audit P4.5).
     *
     * Use unsigned subtraction (start + delta) — rollover-safe across the
     * 32-bit millis() wrap at ~49.7 days. */
    const uint32_t start = millis();
    while ((millis() - start) < timeout_ms) {
        uint8_t status = 0;
        if (readU8(REG_STATUS, status) && (status & 0x01) != 0) {
            setError(RB_OK);
            return true;
        }
        delay(10);
    }
    setError(RB_ERR_TIMEOUT);
    return false;
}

uint8_t RbAmp::firmwareVersion() noexcept {
    uint8_t v = 0;
    if (!readU8(REG_VERSION, v)) {
        setError(RB_ERR_NACK);
        return 0;
    }
    setError(RB_OK);
    return v;
}

uint8_t RbAmp::rawTopology() noexcept {
    /* Direct wire read of REG_TOPOLOGY (0x24). Goes through readU8 so it
     * benefits from the same retry + sanity machinery as every other read.
     * Returns 0xFF on I2C failure (distinguishable from 0x00 unmapped). */
    uint8_t v = 0;
    if (!readU8(REG_TOPOLOGY, v)) {
        return 0xFF;
    }
    return v;
}

/**
 * @brief Apply constructor topology hint + best-effort voltage-hardware probe.
 *
 * SPEC §8 (v1.0): the v1 rbAmp firmware ACKs every in-range register read and
 * returns 0x00 for unmapped registers (src/modules/i2c_driver.c:184,262,285).
 * NACK-probe variant detection is therefore broken at the wire level —
 * `0x00 0x00 0x00 0x00` decodes to a valid `+0.0f`, indistinguishable from a
 * real "channel exists with zero current" reading.
 *
 * The library keeps topology/channels from the constructor @c hint
 * (default @c ThreePhase). Unused channels harmlessly read 0.0 A on this
 * firmware. A future REG_TOPOLOGY byte (v1.1) will enable real auto-detect.
 *
 * Voltage-hardware probe uses an absolute-magnitude threshold on
 * REG_V03_U_RMS: mains rails always read >> 1 V, an unwired voltage front-end
 * reads near zero. This is a reliable signal even on the 0x00-unmapped
 * firmware (because zero is the no-hardware case).
 */
void RbAmp::detectVariant() noexcept {
    /* topology_ and channels_ already initialised from the constructor hint. */

    float u_rms = 0.0f;
    has_voltage_hw_ = (readFloatLE(REG_V03_U_RMS, u_rms) && u_rms > 1.0f);

    if (log_) {
        log_->print(F("[rbamp] topology hint="));
        log_->print(channels_);
        log_->print(F("ch, voltage_hw="));
        log_->println(has_voltage_hw_ ? F("yes") : F("no"));
    }
}

/* ============================================================================
 * Real-time reads
 * ============================================================================ */

float RbAmp::readRtFloat(uint8_t base, uint8_t ch, uint8_t stride) noexcept {
    if (ch >= channels_) {
        setError(RB_ERR_PARAM);
        return NAN;
    }
    const uint8_t reg = static_cast<uint8_t>(base + ch * stride);
    float v = NAN;
    if (!readFloatLE(reg, v)) {
        setError(RB_ERR_IO);
        return NAN;
    }
    setError(RB_OK);
    return v;
}

float RbAmp::readVoltage(uint8_t phase) noexcept {
    if (phase != 0) {
        setError(RB_ERR_PARAM);
        return NAN;
    }
    float v = NAN;
    if (!readFloatLE(REG_V03_U_RMS, v)) {
        setError(RB_ERR_IO);
        return NAN;
    }
    setError(RB_OK);
    return v;
}

float RbAmp::readVoltagePeak(uint8_t phase) noexcept {
    if (phase != 0) {
        setError(RB_ERR_PARAM);
        return NAN;
    }
    float v = NAN;
    if (!readFloatLE(REG_V03_U_PEAK, v)) {
        setError(RB_ERR_IO);
        return NAN;
    }
    setError(RB_OK);
    return v;
}

float RbAmp::readCurrent(uint8_t ch) noexcept {
    return readRtFloat(REG_V03_I0_RMS, ch);
}

float RbAmp::readCurrentPeak(uint8_t ch) noexcept {
    return readRtFloat(REG_V03_I0_PEAK, ch);
}

float RbAmp::readPower(uint8_t ch) noexcept {
    return readRtFloat(REG_V03_P0_REAL, ch);
}

float RbAmp::readPowerFactor(uint8_t ch) noexcept {
    return readRtFloat(REG_V03_PF0, ch);
}

float RbAmp::readFrequency() noexcept {
    uint8_t f = 0;
    if (!readU8(REG_AC_FREQ, f)) {
        setError(RB_ERR_IO);
        return NAN;
    }
    setError(RB_OK);
    return static_cast<float>(f);
}

bool RbAmp::readAll(RbAmpSnapshot& out) noexcept {
    out.topology = topology_;
    out.channels = channels_;
    out.has_voltage_hw = has_voltage_hw_;
    out.voltage      = readVoltage();
    out.voltage_peak = readVoltagePeak();
    for (uint8_t ch = 0; ch < 3; ++ch) {
        if (ch < channels_) {
            out.current[ch]       = readCurrent(ch);
            out.current_peak[ch]  = readCurrentPeak(ch);
            out.power[ch]         = readPower(ch);
            out.power_factor[ch]  = readPowerFactor(ch);
        } else {
            out.current[ch] = out.current_peak[ch] = 0.0f;
            out.power[ch] = out.power_factor[ch] = 0.0f;
        }
    }
    out.frequency = readFrequency();

    /* Any NaN signals failure — last_error_ retains the most recent. */
    if (isnan(out.voltage) || isnan(out.frequency)) {
        return false;
    }
    for (uint8_t ch = 0; ch < channels_; ++ch) {
        if (isnan(out.current[ch]) || isnan(out.power[ch])) {
            return false;
        }
    }
    setError(RB_OK);
    return true;
}

/* ============================================================================
 * Period metering (SPEC §7)
 * ============================================================================ */

bool RbAmp::latchPeriod() noexcept {
    if (!writeCmd(CMD_LATCH_PERIOD)) {
        setError(RB_ERR_IO);
        return false;
    }
    /* Stamp master_t for energy integration on next readPeriodSnapshot(skip_latch=true).
     * Without this, dt would be measured from the previous auto-latch and Wh
     * would be double-counted on the next successful snapshot (audit P4.5). */
    last_latch_ms_ = millis();
    have_last_latch_ = true;
    setError(RB_OK);
    return true;
}

bool RbAmp::isPeriodValid() noexcept {
    uint8_t v = 0;
    if (!readU8(REG_V03_PERIOD_VALID, v)) {
        setError(RB_ERR_IO);
        return false;
    }
    return (v & 0x01) != 0;
}

float RbAmp::readPeriodAvgPower(uint8_t ch) noexcept {
    if (ch >= channels_) {
        setError(RB_ERR_PARAM);
        return NAN;
    }
    /* Per SPEC: ch0 at 0xDC, ch1 at 0xC2, ch2 at 0xC6 — non-contiguous! */
    uint8_t reg;
    switch (ch) {
        case 0: reg = REG_V03_PERIOD_AVG_P_F0; break;
        case 1: reg = REG_V03_PERIOD_AVG_P_F1; break;
        case 2: reg = REG_V03_PERIOD_AVG_P_F2; break;
        default:
            setError(RB_ERR_PARAM);
            return NAN;
    }
    float v = NAN;
    if (!readFloatLE(reg, v)) {
        setError(RB_ERR_IO);
        return NAN;
    }
    setError(RB_OK);
    return v;
}

float RbAmp::readPeriodMaxPower() noexcept {
    float v = NAN;
    if (!readFloatLE(REG_V03_PERIOD_MAX_P_F0, v)) {
        setError(RB_ERR_IO);
        return NAN;
    }
    setError(RB_OK);
    return v;
}

uint32_t RbAmp::readPeriodLatchMs() noexcept {
    uint32_t v = 0;
    if (!readU32LE(REG_V03_PERIOD_LATCH_MS, v)) {
        setError(RB_ERR_IO);
        return 0;
    }
    setError(RB_OK);
    return v;
}

bool RbAmp::readPeriodSnapshot(RbAmpPeriodSnapshot& out,
                               uint16_t settle_ms,
                               bool skip_latch) noexcept {
    out.valid = false;
    out.max_p = 0.0f;
    out.latch_ms = 0;
    out.master_dt_ms = 0;
    for (uint8_t i = 0; i < 3; ++i) {
        out.avg_p[i] = 0.0f;
    }

    /* 1. Latch (skip if caller already used broadcastLatch) */
    if (!skip_latch) {
        if (!writeCmd(CMD_LATCH_PERIOD)) {
            setError(RB_ERR_IO);
            return false;
        }
    }

    /* 2. Capture master wall-clock for dt_s computation */
    const uint32_t now_ms = millis();
    if (have_last_latch_) {
        out.master_dt_ms = now_ms - last_latch_ms_;
    }

    /* 3. Settle 50 ms (per SPEC) before reading the latched block */
    delay(settle_ms);

    /* 4. Verify the snapshot is fresh — discard if stale (race condition).
     *    Commit master_t even on stale: otherwise the next successful snapshot
     *    integrates avg_p over 2× period and double-counts Wh
     *    (audit P4.5 cross-cutting #3). */
    if (!isPeriodValid()) {
        last_latch_ms_ = now_ms;
        have_last_latch_ = true;
        setError(RB_ERR_STALE);
        if (log_) log_->println(F("[rbamp] period STALE — discarded"));
        return false;
    }

    /* 5. Read avg_p for every populated channel */
    for (uint8_t ch = 0; ch < channels_; ++ch) {
        out.avg_p[ch] = readPeriodAvgPower(ch);
        if (isnan(out.avg_p[ch])) {
            setError(RB_ERR_IO);
            return false;
        }
    }
    out.max_p = readPeriodMaxPower();
    out.latch_ms = readPeriodLatchMs();
    out.valid = true;

    /* 6. Commit master timestamp & integrate into Wh accumulator */
    last_latch_ms_ = now_ms;
    have_last_latch_ = true;
    energy_.tick(out, channels_);

    setError(RB_OK);
    return true;
}

/* ============================================================================
 * Configuration (SPEC §10, §11)
 * ============================================================================ */

bool RbAmp::setSensorClass(RbAmpSensorClass cls) noexcept {
    /* RbAmpSensorClass values match the wire-level encoding 0..3 — no remap. */
    if (!writeReg(REG_SENSOR_CLASS, static_cast<uint8_t>(cls))) {
        setError(RB_ERR_IO);
        return false;
    }
    if (!writeCmd(CMD_SAVE_GAINS)) {
        setError(RB_ERR_IO);
        return false;
    }
    delay(SETTLE_MS_SAVE_GAINS);
    setError(RB_OK);
    return true;
}

bool RbAmp::setCTModel(uint8_t code) noexcept {
    if (code < 1 || code > 5) {
        setError(RB_ERR_PARAM);
        return false;
    }
    /* v1.2+ firmware (REG_VERSION >= 0x03) gates REG_CT_MODEL on REG_SENSOR_CLASS
     * != 0. Mirror the guard library-side so users get RB_ERR_PARAM without
     * burning a flash-erase cycle on a write that would silently fail. On
     * v1.0/v1.1 firmware (REG_VERSION < 0x03) the device-side callback has
     * no guard — preserve backward compat by skipping the check. Operator
     * decision: explicit (no auto-fallback to Sct013) — caller must call
     * setSensorClass() first or read RB_ERR_PARAM. */
    uint8_t fw = 0;
    if (readU8(REG_VERSION, fw) && fw >= 0x03) {
        uint8_t cls = 0;
        if (!readU8(REG_SENSOR_CLASS, cls)) {
            setError(RB_ERR_IO);
            return false;
        }
        if (cls == 0) {
            setError(RB_ERR_PARAM);
            return false;
        }
    }
    if (!writeReg(REG_CT_MODEL, code)) {
        setError(RB_ERR_IO);
        return false;
    }
    return saveGains();
}

bool RbAmp::setCTModel(uint8_t channel, uint8_t code) noexcept {
    if (channel >= 3 || code < 1 || code > 5) {
        setError(RB_ERR_PARAM);
        return false;
    }
    /* Per-channel form requires v1.2+ firmware — CMD_SET_CT_MODEL_CHn opcodes
     * (0x28/0x29/0x2A) do not exist on v1.0/v1.1. On older firmware, callers
     * should use the legacy single-arg setCTModel(code) which writes channel 0
     * via the direct REG_CT_MODEL path. */
    uint8_t fw = 0;
    if (!readU8(REG_VERSION, fw)) {
        setError(RB_ERR_IO);
        return false;
    }
    if (fw < 0x03) {
        setError(RB_ERR_VERSION);
        return false;
    }
    /* v1.2 strictness — same RB_ERR_PARAM guard as single-arg setCTModel. */
    uint8_t cls = 0;
    if (!readU8(REG_SENSOR_CLASS, cls)) {
        setError(RB_ERR_IO);
        return false;
    }
    if (cls == 0) {
        setError(RB_ERR_PARAM);
        return false;
    }
    /* Sequence per inc/modules/i2c_callbacks.c::cmd_set_ct_model_apply:
     *  1. Write REG_CT_MODEL = code  (firmware side-effect: cb_ct_model_write
     *     unconditionally applies the preset to channel 0 — this is the
     *     v1.1 backward-compat direct-write path that v1.2 retains. To set
     *     ch1/ch2 without leaving ch0 clobbered, the caller must apply the
     *     CT model from highest channel down to ch0 — see Doxygen on
     *     setCTModel(channel, code) in RbAmp.h for the canonical example.)
     *  2. Write REG_COMMAND = CMD_SET_CT_MODEL_CH<channel>  (5 ms settle for
     *     in-RAM preset lookup; flash save deferred to step 3).
     *  3. CMD_SAVE_GAINS (700 ms) to persist NF/GAIN mirrors to flash. */
    if (!writeReg(REG_CT_MODEL, code)) {
        setError(RB_ERR_IO);
        return false;
    }
    const uint8_t cmd = static_cast<uint8_t>(CMD_SET_CT_MODEL_CH0 + channel);
    if (!writeCmd(cmd)) {
        setError(RB_ERR_IO);
        return false;
    }
    delay(5);  /* per commands.yaml settle_ms */
    return saveGains();
}

bool RbAmp::saveGains() noexcept {
    if (!writeCmd(CMD_SAVE_GAINS)) {
        setError(RB_ERR_IO);
        return false;
    }
    delay(SETTLE_MS_SAVE_GAINS);  /* 700 ms — flash erase + write */
    setError(RB_OK);
    return true;
}

bool RbAmp::prepareAddressChange(uint8_t new_addr) noexcept {
    if (new_addr < 0x08 || new_addr > 0x77 || new_addr == addr_) {
        setError(RB_ERR_PARAM);
        return false;
    }
    uint8_t mode = 0;
    if (!readU8(REG_MODE, mode)) {
        setError(RB_ERR_IO);
        return false;
    }
    if (mode != 1) {
        setError(RB_ERR_MODE);
        if (log_) log_->println(F("[rbamp] address change refused: MODE != develop"));
        return false;
    }
    pending_addr_ = new_addr;
    pending_addr_armed_ms_ = millis();
    addr_change_armed_ = true;
    setError(RB_OK);
    return true;
}

bool RbAmp::commitAddressChange() noexcept {
    if (!addr_change_armed_) {
        setError(RB_ERR_PARAM);
        return false;
    }
    /* Rollover-safe deadline check via unsigned subtraction. */
    if ((uint32_t)(millis() - pending_addr_armed_ms_) > 5000u) {
        addr_change_armed_ = false;
        setError(RB_ERR_TIMEOUT);
        if (log_) log_->println(F("[rbamp] address change arm expired (>5s)"));
        return false;
    }
    if (!writeReg(REG_I2C_ADDRESS, pending_addr_)) {
        setError(RB_ERR_IO);
        return false;
    }
    if (!writeCmd(CMD_SAVE_GAINS)) {
        setError(RB_ERR_IO);
        return false;
    }
    delay(SETTLE_MS_SAVE_GAINS);
    if (!writeCmd(CMD_RESET)) {
        setError(RB_ERR_IO);
        return false;
    }
    delay(SETTLE_MS_RESET);

    /* From now on the device responds at the new address */
    addr_ = pending_addr_;
    addr_change_armed_ = false;
    setError(RB_OK);
    return true;
}

bool RbAmp::factoryReset() noexcept {
    if (!writeCmd(CMD_FACTORY_RESET)) {
        setError(RB_ERR_IO);
        return false;
    }
    delay(SETTLE_MS_FACTORY_RESET);
    setError(RB_OK);
    return true;
}

bool RbAmp::reset() noexcept {
    if (!writeCmd(CMD_RESET)) {
        setError(RB_ERR_IO);
        return false;
    }
    delay(SETTLE_MS_RESET);
    setError(RB_OK);
    return true;
}

/* ============================================================================
 * Static / multi-module
 * ============================================================================ */

bool RbAmp::broadcastLatch(TwoWire& bus) noexcept {
    /* General-Call DISABLED in v1 rbAmp firmware (SPEC §9, v1.0).
     * src/modules/i2c_driver.c:110 sets I2C_GENERALCALL_DISABLE — peripheral
     * does not respond to address 0x00 at all. Reserved for v2 when firmware
     * adds the GC ISR handler.
     *
     * To avoid waiting on a wire transaction that nobody is listening to, we
     * return RB_ERR_NOT_IMPLEMENTED immediately. Callers should fall back to
     * per-device sequential latchPeriod() with skew compensation. */
    (void)bus;
    /* Note: static method, cannot call setError(); document via return value. */
    return false;
}

/* ============================================================================
 * Diagnostics
 * ============================================================================ */

const char* RbAmp::errorString(int8_t code) noexcept {
    switch (code) {
        case RB_OK:                 return "OK";
        case RB_ERR_IO:             return "I2C transport failure";
        case RB_ERR_NACK:           return "NACK — device absent or busy";
        case RB_ERR_TIMEOUT:        return "Bus timeout";
        case RB_ERR_NOT_READY:      return "Device not ready";
        case RB_ERR_STALE:          return "Period snapshot stale";
        case RB_ERR_PARAM:          return "Bad parameter";
        case RB_ERR_MODE:           return "Operation requires develop mode";
        case RB_ERR_CHECKSUM:       return "Codegen parity mismatch";
        case RB_ERR_VERSION:        return "Unsupported firmware version";
        case RB_ERR_NOT_IMPLEMENTED: return "Not implemented (RESERVED FOR v2)";
        case RB_ERR_NON_PHYSICAL:    return "Non-physical value (NaN/Inf/out-of-bounds)";
        default:                return "Unknown error";
    }
}

void RbAmp::setError(int8_t err) noexcept {
    last_error_ = err;
    if (log_ && err != RB_OK) {
        log_->print(F("[rbamp] err "));
        log_->print(err);
        log_->print(F(" — "));
        log_->println(errorString(err));
    }
}

/* ============================================================================
 * Low-level I/O — one byte per I2C address phase (SPEC §6, no auto-increment)
 * ============================================================================ */

bool RbAmp::writeReg(uint8_t reg, uint8_t val) noexcept {
    bus_.beginTransmission(addr_);
    bus_.write(reg);
    bus_.write(val);
    return bus_.endTransmission() == 0;
}

bool RbAmp::writeCmd(uint8_t cmd) noexcept {
    return writeReg(REG_COMMAND, cmd);
}

bool RbAmp::readU8(uint8_t reg, uint8_t& out) noexcept {
    /* SPEC §B.5: ESP-IDF v5 i2c_master driver intermittently NACKs reads from
     * the rbAmp slave
     * at ~20 % rate at 100 kHz; 3-attempt retry with 5 ms gap drops residual
     * error rate to <0.8 %. Non-ESP32 platforms don't show the NACK pattern,
     * so retry is gated to avoid useless latency. Firmware fix in v1.1 will
     * remove the need for retry.
     *
     * Dense-workload tuning (SPEC §B.5): the default 3 attempts can be
     * exhausted on ≥10-byte/cycle workloads. Pre-define
     * RBAMP_NACK_RETRY_ATTEMPTS to override (e.g. set to 5 for soak tests). */
#ifndef RBAMP_NACK_RETRY_ATTEMPTS
#  if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
#    define RBAMP_NACK_RETRY_ATTEMPTS 3
#  else
#    define RBAMP_NACK_RETRY_ATTEMPTS 1
#  endif
#endif
#ifndef RBAMP_NACK_RETRY_GAP_MS
#  define RBAMP_NACK_RETRY_GAP_MS 5
#endif
    constexpr uint8_t kAttempts = RBAMP_NACK_RETRY_ATTEMPTS;
    for (uint8_t attempt = 0; attempt < kAttempts; ++attempt) {
        bus_.beginTransmission(addr_);
        bus_.write(reg);
        if (bus_.endTransmission() == 0) {
            /* requestFrom returns uint8_t on AVR but size_t on STM32duino
             * and others; use >= 1 for cross-core safety (audit P4.5). */
            if (bus_.requestFrom(addr_, static_cast<uint8_t>(1)) >= 1) {
                const int b = bus_.read();
                if (b >= 0) {
                    out = static_cast<uint8_t>(b);
                    return true;
                }
            }
        }
        if (attempt + 1 < kAttempts) {
            delay(RBAMP_NACK_RETRY_GAP_MS);  /* let the rbAmp slave flush ADDR-phase state before retrying */
        }
    }
    /* Retry exhausted — instrument for soak diagnostics. */
    retry_exhaustion_count_++;
    setError(RB_ERR_NACK);
    return false;
}

bool RbAmp::readU16LE(uint8_t reg, uint16_t& out) noexcept {
    uint8_t lo = 0, hi = 0;
    if (!readU8(reg, lo) || !readU8(static_cast<uint8_t>(reg + 1), hi)) {
        return false;
    }
    out = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
    return true;
}

bool RbAmp::readU32LE(uint8_t reg, uint32_t& out) noexcept {
    uint8_t b[4];
    for (uint8_t i = 0; i < 4; ++i) {
        if (!readU8(static_cast<uint8_t>(reg + i), b[i])) {
            return false;
        }
    }
    out =  static_cast<uint32_t>(b[0])
        | (static_cast<uint32_t>(b[1]) << 8)
        | (static_cast<uint32_t>(b[2]) << 16)
        | (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

bool RbAmp::readFloatLE(uint8_t reg, float& out) noexcept {
    uint8_t b[4];
    for (uint8_t i = 0; i < 4; ++i) {
        if (!readU8(static_cast<uint8_t>(reg + i), b[i])) {
            return false;
        }
    }
    /* memcpy is the portable bit-reinterpret pattern; avoids strict-aliasing UB */
    memcpy(&out, b, 4);
    /* SPEC §B.5 — sanity filter against IDF i2c_master buffer-leak ghost values
     * (e.g. 0x3C2FFB3F = 1.962 V) that survive retry. Loose bounds only: no
     * physical lower bounds, so brownout / disconnect / off-grid states pass
     * through. Catches NaN / Inf / exotic-bit-pattern only. */
    if (!isfinite(out) || fabsf(out) > 10000.0f) {
        sanity_reject_count_++;
        setError(RB_ERR_NON_PHYSICAL);
        return false;
    }
    return true;
}

bool RbAmp::registerAcks(uint8_t reg) noexcept {
    bus_.beginTransmission(addr_);
    bus_.write(reg);
    /* endTransmission(true) = send STOP; returns 0 on full ACK chain */
    if (bus_.endTransmission() != 0) {
        return false;
    }
    /* Confirm slave also ACKs a read setup at that register.
     * Use >= 1 — see readU8() comment about cross-core requestFrom semantics. */
    return bus_.requestFrom(addr_, static_cast<uint8_t>(1)) >= 1;
}
