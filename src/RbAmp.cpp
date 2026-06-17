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
      variant_(RbAmpVariant::Unknown),
      capability_(0),
      last_error_(RB_OK),
      log_(nullptr),
      prev_latch_ms_(0),
      current_latch_ms_(0),
      have_prev_latch_(false),
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
     *    Stamp both prev_latch_ms_ and current_latch_ms_ right after the bus
     *    write (before the settle) so the first user-visible
     *    readPeriodSnapshot() reports master_dt_ms ≈ 0 (skipped by energy.tick
     *    first-tick guard) and subsequent cycles start the (current - prev)
     *    delta from a clean baseline. */
    if (!writeCmd(CMD_LATCH_PERIOD)) {
        setError(RB_ERR_IO);
        return false;
    }
    prev_latch_ms_ = millis();
    current_latch_ms_ = prev_latch_ms_;
    have_prev_latch_ = true;
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
 * @brief Detect variant from REG_HW_VARIANT (0x55) — fall back to hint + u_rms.
 *
 * Per truth-doc §1.2 (root-ack 2026-06-14): @c REG_HW_VARIANT (0x55) returns
 * one of 0x01..0x06 = UI1/UI2/UI3/I1/I2/I3 on v1.2+ firmware. Each SKU encodes
 * both channel count AND voltage-hardware presence in a single byte:
 *   0x01 UI1 → 1ch + U      0x04 I1 → 1ch + no U
 *   0x02 UI2 → 2ch + U      0x05 I2 → 2ch + no U
 *   0x03 UI3 → 3ch + U      0x06 I3 → 3ch + no U
 *
 * v1.0/v1.1 firmware returns 0x00 on this address (it's the legacy
 * REG_CS_INTERVAL_L slot — see §3, unmapped reads return 0x00 not NACK). On
 * 0x00 we fall back to the constructor @c hint (channels_ already set) and
 * probe voltage hardware via U_RMS threshold (mains read >> 1 V; unwired
 * front-end reads near zero — reliable even on the 0x00-unmapped firmware).
 */
void RbAmp::detectVariant() noexcept {
    /* REG_HW_VARIANT (0x55) — canonical SKU byte from auto-gen header
     * (bridge bfa7708 / V2 schema). v1.0/v1.1 fw returns 0x00 here (legacy
     * CS_INTERVAL_L slot); v1.2+ fw returns 0x01..0x06 per truth-doc §1.2. */
    uint8_t variant = 0;
    const bool variant_ok = readU8(REG_HW_VARIANT, variant);

    /* Cache CAPABILITY (0x57, u16 LE) for begin-time feature detect — best
     * effort (absent on v1.0/v1.1 → 0). */
    uint16_t cap = 0;
    if (readU16LE(v2::REG_CAPABILITY, cap)) {
        capability_ = cap;
    }

    if (variant_ok && variant >= 0x01 && variant <= 0x06) {
        /* v1.2+ path — variant byte is authoritative. */
        variant_ = static_cast<RbAmpVariant>(variant);
        switch (variant) {
            case 0x01: topology_ = RbAmpTopology::Single;     channels_ = 1; has_voltage_hw_ = true;  break;
            case 0x02: topology_ = RbAmpTopology::SplitPhase; channels_ = 2; has_voltage_hw_ = true;  break;
            case 0x03: topology_ = RbAmpTopology::ThreePhase; channels_ = 3; has_voltage_hw_ = true;  break;
            case 0x04: topology_ = RbAmpTopology::Single;     channels_ = 1; has_voltage_hw_ = false; break;
            case 0x05: topology_ = RbAmpTopology::SplitPhase; channels_ = 2; has_voltage_hw_ = false; break;
            case 0x06: topology_ = RbAmpTopology::ThreePhase; channels_ = 3; has_voltage_hw_ = false; break;
        }
        if (log_) {
            log_->print(F("[rbamp] variant=0x"));
            log_->print(variant, HEX);
            log_->print(F(" ("));
            log_->print(channels_);
            log_->print(F("ch, voltage_hw="));
            log_->print(has_voltage_hw_ ? F("yes") : F("no"));
            log_->println(F(")"));
        }
        return;
    }

    /* v1.0/v1.1 fallback — keep constructor-hint topology, probe U_RMS for
     * voltage hardware. channels_ already set in the initializer list. */
    float u_rms = 0.0f;
    has_voltage_hw_ = (readFloatLE(REG_V03_U_RMS, u_rms, 500.0f) && u_rms > 1.0f);

    if (log_) {
        log_->print(F("[rbamp] variant=0x00 (legacy fw), hint="));
        log_->print(channels_);
        log_->print(F("ch, voltage_hw="));
        log_->println(has_voltage_hw_ ? F("yes") : F("no"));
    }
}

/* ============================================================================
 * Real-time reads
 * ============================================================================ */

float RbAmp::readRtFloat(uint8_t base, uint8_t ch, uint8_t stride, float max_abs) noexcept {
    if (ch >= channels_) {
        setError(RB_ERR_PARAM);
        return NAN;
    }
    const uint8_t reg = static_cast<uint8_t>(base + ch * stride);
    float v = NAN;
    if (!readFloatLE(reg, v, max_abs)) {
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
    if (!readFloatLE(REG_V03_U_RMS, v, 500.0f)) {
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
    if (!readFloatLE(REG_V03_U_PEAK, v, 800.0f)) {
        setError(RB_ERR_IO);
        return NAN;
    }
    setError(RB_OK);
    return v;
}

float RbAmp::readCurrent(uint8_t ch) noexcept {
    return readRtFloat(REG_V03_I0_RMS, ch, 4, 150.0f);
}

float RbAmp::readCurrentPeak(uint8_t ch) noexcept {
    return readRtFloat(REG_V03_I0_PEAK, ch, 4, 250.0f);
}

float RbAmp::readPower(uint8_t ch) noexcept {
    return readRtFloat(REG_V03_P0_REAL, ch, 4, 30000.0f);
}

float RbAmp::readPowerFactor(uint8_t ch) noexcept {
    return readRtFloat(REG_V03_PF0, ch, 4, 1.5f);
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

bool RbAmp::foldField(bool read_ok, float& field, uint8_t& mask, uint8_t bit) noexcept {
    if (read_ok) return true;
    if (last_error_ == RB_ERR_NON_PHYSICAL) {
        field = NAN;       /* read OK but value implausible — non-fatal */
        mask |= bit;
        return true;
    }
    return false;          /* transport failure — propagate */
}

bool RbAmp::readAll(RbAmpSnapshot& out) noexcept {
    out.topology = topology_;
    out.channels = channels_;
    out.has_voltage_hw = has_voltage_hw_;
    out.implausible = 0;

    /* Per-field: a sanity-reject sets the field NaN + flags implausible and the
     * snapshot stays usable; only a transport failure aborts (matches the
     * esp-idf reference _fold_field — keeps good fields when one is over-range). */
    if (!foldField(readFloatLE(REG_V03_U_RMS, out.voltage, 500.0f),
                   out.voltage, out.implausible, RBAMP_FIELD_VOLTAGE)) { setError(RB_ERR_IO); return false; }
    if (!foldField(readFloatLE(REG_V03_U_PEAK, out.voltage_peak, 800.0f),
                   out.voltage_peak, out.implausible, RBAMP_FIELD_VOLTAGE)) { setError(RB_ERR_IO); return false; }

    for (uint8_t ch = 0; ch < 3; ++ch) {
        if (ch < channels_) {
            const uint8_t off = static_cast<uint8_t>(ch * 4);
            if (!foldField(readFloatLE(static_cast<uint8_t>(REG_V03_I0_RMS + off), out.current[ch], 150.0f),
                           out.current[ch], out.implausible, RBAMP_FIELD_CURRENT)) { setError(RB_ERR_IO); return false; }
            if (!foldField(readFloatLE(static_cast<uint8_t>(REG_V03_I0_PEAK + off), out.current_peak[ch], 250.0f),
                           out.current_peak[ch], out.implausible, RBAMP_FIELD_CURRENT)) { setError(RB_ERR_IO); return false; }
            if (!foldField(readFloatLE(static_cast<uint8_t>(REG_V03_P0_REAL + off), out.power[ch], 30000.0f),
                           out.power[ch], out.implausible, RBAMP_FIELD_POWER)) { setError(RB_ERR_IO); return false; }
            if (!foldField(readFloatLE(static_cast<uint8_t>(REG_V03_PF0 + off), out.power_factor[ch], 1.5f),
                           out.power_factor[ch], out.implausible, RBAMP_FIELD_PF)) { setError(RB_ERR_IO); return false; }
        } else {
            out.current[ch] = out.current_peak[ch] = 0.0f;
            out.power[ch] = out.power_factor[ch] = 0.0f;
        }
    }

    /* Frequency — u8 (no sanity filter): transport failure only. */
    uint8_t f = 0;
    if (!readU8(REG_AC_FREQ, f)) { setError(RB_ERR_IO); return false; }
    out.frequency = static_cast<float>(f);

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
    /* Stamp ONLY current_latch_ms_ — prev_latch_ms_ stays put until the
     * subsequent readPeriodSnapshot() either commits (OK or STALE both roll
     * prev forward). This separates "when did I latch" from "what dt does
     * the next energy tick see" — fixes truth-doc §16.2 #1 multi-module
     * energy-≈-0 bug where a single timestamp collapsed dt to the settle
     * window (~50 ms) in the canonical fleet pattern. */
    current_latch_ms_ = millis();
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
    if (!readFloatLE(reg, v, 30000.0f)) {
        setError(RB_ERR_IO);
        return NAN;
    }
    setError(RB_OK);
    return v;
}

float RbAmp::readPeriodMaxPower() noexcept {
    float v = NAN;
    if (!readFloatLE(REG_V03_PERIOD_MAX_P_F0, v, 30000.0f)) {
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

    /* 1. Latch (skip if caller already used latchPeriod() or broadcastLatch).
     *    In the non-skip path we stamp current_latch_ms_ here too — the
     *    multi-module pattern (caller did latchPeriod(m) per device) routes
     *    through skip_latch=true and already stamped current at latch time. */
    if (!skip_latch) {
        if (!writeCmd(CMD_LATCH_PERIOD)) {
            setError(RB_ERR_IO);
            return false;
        }
        current_latch_ms_ = millis();
    }

    /* 2. Compute master wall-clock dt from prev_latch_ms_ → current_latch_ms_
     *    (NOT from prev to now — the settle window must not inflate dt). */
    if (have_prev_latch_) {
        out.master_dt_ms = current_latch_ms_ - prev_latch_ms_;
    }

    /* 3. Settle 50 ms (per SPEC) before reading the latched block */
    delay(settle_ms);

    /* 4. Verify the snapshot is fresh — discard if stale (empty accumulator).
     *    HOLD the anchor on stale: do NOT advance prev_latch_ms_. The firmware
     *    PRESERVES its period accumulator across an empty/stale latch (OI-3
     *    RESOLVED, truth-doc §16.1), so the next VALID snapshot's avg_p covers
     *    the full master interval since the last CONSUMED read. Advancing the
     *    anchor here would shorten dt and UNDERCOUNT Wh. Energy integrates over
     *    master wall-clock, never the chip latch_ms (0xEC under-counts ~26% via
     *    SysTick starvation — esp-idf E.6/F10, L9). Matches the esp-idf
     *    reference rbamp_read_period_snapshot stale path. */
    if (!isPeriodValid()) {
        setError(RB_ERR_STALE);
        if (log_) log_->println(F("[rbamp] period STALE — discarded, anchor held"));
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

    /* 6. Commit master timestamp & integrate into Wh accumulator.
     *    prev rolls to current — next dt will measure (next_latch - this_latch). */
    prev_latch_ms_ = current_latch_ms_;
    have_prev_latch_ = true;
    energy_.tick(out, channels_);

    setError(RB_OK);
    return true;
}

/* ============================================================================
 * Configuration (SPEC §10, §11)
 * ============================================================================ */

bool RbAmp::setSensorClass(RbAmpSensorClass cls) noexcept {
    /* RbAmpSensorClass values match the wire-level encoding 0..3 — no remap.
     * Client-side enum guard (mirrors the esp-idf reference): the firmware
     * SILENTLY ACCEPTS an out-of-range SENSOR_CLASS (esp-idf F-VALIDATION), so
     * this fast-fail is the ONLY defense against a bad value reaching flash. */
    if (static_cast<uint8_t>(cls) > static_cast<uint8_t>(RbAmpSensorClass::BuiltinCT)) {
        setError(RB_ERR_PARAM);
        return false;
    }
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

bool RbAmp::ctModelValid(RbAmpSensorClass cls, uint8_t code) noexcept {
    /* Per-class accepted CT SKUs (A1) — non-contiguous, mirrors the esp-idf
     * reference _ct_model_valid. Codes 5 (SCT-013-100) and 7 (-060) are
     * recognised SKUs but uncharacterised → rejected. Firmware is the ultimate
     * authority; this is a client fast-fail to avoid burning a flash cycle. */
    switch (cls) {
        case RbAmpSensorClass::Sct013:
            return code == 1 || code == 2 || code == 3 || code == 4 || code == 6;
        case RbAmpSensorClass::WiredCT:
            return code == 1 || code == 2 || code == 3;
        case RbAmpSensorClass::BuiltinCT:
        default:
            return false;  /* per-unit factory cal, no presets */
    }
}

bool RbAmp::setCTModel(uint8_t code) noexcept {
    if (code < 1 || code > 7) {
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
        if (!ctModelValid(static_cast<RbAmpSensorClass>(cls), code)) {
            setError(RB_ERR_PARAM);
            return false;
        }
    }
    /* v1.3 A1: REG_CT_MODEL (0x05) is PURE STAGING — a bare write no longer
     * applies the preset; binding happens exclusively via CMD_SET_CT_MODEL_CHn.
     * The single-arg form targets channel 0 (the only channel on UI1/I1). */
    if (!writeReg(REG_CT_MODEL, code)) {
        setError(RB_ERR_IO);
        return false;
    }
    if (!writeCmd(CMD_SET_CT_MODEL_CH0)) {
        setError(RB_ERR_IO);
        return false;
    }
    delay(SETTLE_MS_SET_CT_MODEL_CH0);
    return saveGains();
}

bool RbAmp::setCTModel(uint8_t channel, uint8_t code) noexcept {
    if (channel >= 3 || code < 1 || code > 7) {
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
    if (!ctModelValid(static_cast<RbAmpSensorClass>(cls), code)) {  /* A1 fast-fail */
        setError(RB_ERR_PARAM);
        return false;
    }
    /* v1.3 A1 sequence (order-independent — REG_CT_MODEL is pure staging):
     *  1. Write REG_CT_MODEL = code  (stages the preset; no channel binds yet).
     *  2. Write REG_COMMAND = CMD_SET_CT_MODEL_CH<channel>  — binds the staged
     *     code to THIS channel only (5 ms settle for in-RAM preset lookup).
     *  3. CMD_SAVE_GAINS (700 ms) to persist NF/GAIN mirrors to flash.
     * For multiple channels prefer configureChannels() (batched single save). */
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

bool RbAmp::readCTModelCh(uint8_t channel, uint8_t& out) noexcept {
    if (channel > 2) {
        setError(RB_ERR_PARAM);
        return false;
    }
    /* Mirror registers: CH0=0x51, CH1=0x52, CH2=0x53 (v2). A/B torn-read. */
    const uint8_t reg = static_cast<uint8_t>(v2::REG_CT_MODEL_CH0 + channel);
    if (!readU8AB(reg, out)) {
        if (lastError() == RB_OK) setError(RB_ERR_IO);
        return false;
    }
    setError(RB_OK);
    return true;
}

int8_t RbAmp::verifyCtBind(uint8_t channel, uint8_t expected) noexcept {
    /* Up to 3 mirror reads: a clean read decides (match → RB_OK, mismatch →
     * RB_ERR_PARAM stable-wrong); an A/B-torn read is retried, not concluded. */
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        uint8_t applied = 0;
        if (readCTModelCh(channel, applied)) {
            return (applied == expected) ? RB_OK : RB_ERR_PARAM;
        }
    }
    return RB_ERR_NON_PHYSICAL;  /* stayed torn — trust the accepted CMD */
}

bool RbAmp::configureChannels(RbAmpSensorClass cls, const uint8_t* models, uint8_t n) noexcept {
    if (models == nullptr) {
        setError(RB_ERR_PARAM);
        return false;
    }
    /* 1. Apply sensor class (one SAVE inside setSensorClass; also resets the
     *    device-side CT_MODEL so channel binds start clean). */
    if (!setSensorClass(cls)) {
        return false;  /* error already set */
    }
    if (n > channels_) n = channels_;

    /* Per-channel opcodes need v1.2+ firmware. */
    uint8_t fw = 0;
    if (!readU8(REG_VERSION, fw)) {
        setError(RB_ERR_IO);
        return false;
    }

    /* 2. Bind each requested channel ASCENDING — order-independent on v1.3
     *    firmware. A models[ch]==0 leaves that channel as-is.
     *    Validate (A1) before each wire write; abort the batch on a bad code. */
    bool any_bound = false;
    for (uint8_t ch = 0; ch < n; ++ch) {
        const uint8_t code = models[ch];
        if (code == 0) continue;
        if (code > 7 || !ctModelValid(cls, code)) {
            setError(RB_ERR_PARAM);
            return false;
        }
        if (fw < 0x03) {
            setError(RB_ERR_VERSION);
            return false;
        }
        if (!writeReg(REG_CT_MODEL, code)) {
            setError(RB_ERR_IO);
            return false;
        }
        const uint8_t cmd = static_cast<uint8_t>(CMD_SET_CT_MODEL_CH0 + ch);
        if (!writeCmd(cmd)) {
            setError(RB_ERR_IO);
            return false;
        }
        delay(5);  /* in-RAM preset lookup; flash save batched below */

        /* Mirror-verify the bind (catches a silently-dropped CMD). Stable-wrong
         * → re-bind once; still wrong → fail. Torn mirror → trust the CMD. */
        int8_t v = verifyCtBind(ch, code);
        if (v == RB_ERR_PARAM) {
            if (!writeReg(REG_CT_MODEL, code) ||
                !writeCmd(cmd)) {
                setError(RB_ERR_IO);
                return false;
            }
            delay(5);
            if (verifyCtBind(ch, code) == RB_ERR_PARAM) {
                setError(RB_ERR_PARAM);  /* ch did not bind */
                if (log_) {
                    log_->print(F("[rbamp] configureChannels: ch"));
                    log_->print(ch);
                    log_->println(F(" did not bind"));
                }
                return false;
            }
        }
        any_bound = true;
    }

    /* 3. ONE terminal SAVE for all binds (a UI3 = 2 flash cycles total, not 4). */
    if (any_bound) {
        return saveGains();
    }
    setError(RB_OK);
    return true;
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

bool RbAmp::saveUserConfig() noexcept {
    if (!writeCmd(CMD_SAVE_USER_CONFIG)) {
        setError(RB_ERR_IO);
        return false;
    }
    delay(SETTLE_MS_SAVE_USER_CONFIG);  /* 700 ms — flash erase + write */
    setError(RB_OK);
    return true;
}

bool RbAmp::prepareAddressChange(uint8_t new_addr) noexcept {
    if (new_addr < 0x08 || new_addr > 0x77 || new_addr == addr_) {
        setError(RB_ERR_PARAM);
        return false;
    }
    /* v1.3: address change is PRODUCTION-OK via the two-phase magic commit
     * (truth-doc §6.1) — NOT develop-gated. No wire I/O here; just arm and
     * record the timestamp (5 s window enforced in commitAddressChange). */
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
    addr_change_armed_ = false;  /* consume the arm eagerly */

    /* Two-phase magic commit (truth-doc §6.1, the ONLY supported path; the
     * legacy "write 0x30 -> SAVE_GAINS -> reset" path is deleted):
     *  1. write candidate addr -> REG_I2C_ADDRESS (0x30) — lands in RAM
     *  2. arm: write 0xA5 -> REG_ADDR_COMMIT_MAGIC (0x31)
     *  3. CMD_COMMIT_ADDR (0x30 opcode) — persists to flash
     *  4. CMD_RESET — new address applied after reboot */
    if (!writeReg(REG_I2C_ADDRESS, pending_addr_)) {
        setError(RB_ERR_IO);
        return false;
    }
    if (!writeReg(REG_ADDR_COMMIT_MAGIC, 0xA5)) {
        setError(RB_ERR_IO);
        return false;
    }
    if (!writeCmd(CMD_COMMIT_ADDR)) {
        setError(RB_ERR_IO);
        return false;
    }
    delay(SETTLE_MS_COMMIT_ADDR);
    /* RESET-write failure is non-fatal: the device adopts the committed address
     * on its next power cycle regardless. Rebind preemptively either way. */
    writeCmd(CMD_RESET);
    delay(SETTLE_MS_RESET);

    /* From now on the device responds at the new address. */
    addr_ = pending_addr_;
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
 * A/B torn-read helpers — 2-of-3 majority for correctness-critical values
 * ============================================================================ */

bool RbAmp::readU8AB(uint8_t reg, uint8_t& out) noexcept {
    uint8_t a = 0, b = 0;
    if (!readU8(reg, a)) return false;
    if (!readU8(reg, b)) return false;
    if (a == b) { out = a; setError(RB_OK); return true; }
    uint8_t c = 0;
    if (!readU8(reg, c)) return false;
    if (c == a || c == b) { out = c; setError(RB_OK); return true; }  /* 2-of-3 */
    sanity_reject_count_++;
    setError(RB_ERR_NON_PHYSICAL);
    return false;
}

bool RbAmp::readU16AB(uint8_t reg, uint16_t& out) noexcept {
    uint16_t a = 0, b = 0;
    if (!readU16LE(reg, a)) return false;
    if (!readU16LE(reg, b)) return false;
    if (a == b) { out = a; setError(RB_OK); return true; }
    uint16_t c = 0;
    if (!readU16LE(reg, c)) return false;
    if (c == a || c == b) { out = c; setError(RB_OK); return true; }  /* 2-of-3 */
    sanity_reject_count_++;
    setError(RB_ERR_NON_PHYSICAL);
    return false;
}

/* ============================================================================
 * Identity / capability (v1.3)
 * ============================================================================ */

RbAmpVariant RbAmp::readVariant() noexcept {
    uint8_t v = 0;
    if (!readU8(REG_HW_VARIANT, v)) {
        setError(RB_ERR_IO);
        return RbAmpVariant::Unknown;
    }
    setError(RB_OK);
    if (v >= 0x01 && v <= 0x06) {
        variant_ = static_cast<RbAmpVariant>(v);
        return variant_;
    }
    return RbAmpVariant::Unknown;  /* 0x00 on v1.0/v1.1 fw (unmapped) */
}

bool RbAmp::readCapability(uint16_t& out) noexcept {
    if (!readU16LE(v2::REG_CAPABILITY, out)) {
        setError(RB_ERR_IO);
        return false;
    }
    capability_ = out;
    setError(RB_OK);
    return true;
}

uint8_t RbAmp::readProductId() noexcept {
    uint8_t v = 0;
    if (!readU8(v2::REG_PRODUCT_ID, v)) {
        setError(RB_ERR_IO);
        return 0;
    }
    setError(RB_OK);
    return v;
}

bool RbAmp::readUid(uint8_t out[12]) noexcept {
    /* 12 single-byte reads — the firmware ACKs per byte; the "burst" wording in
     * the register map is the on-chip source layout, not the I2C access. */
    for (uint8_t i = 0; i < 12; ++i) {
        if (!readU8(static_cast<uint8_t>(v2::REG_UID + i), out[i])) {
            setError(RB_ERR_IO);
            return false;
        }
    }
    setError(RB_OK);
    return true;
}

/* ============================================================================
 * Error / event channel (v1.3)
 * ============================================================================ */

uint8_t RbAmp::readLastError() noexcept {
    uint8_t v = 0;
    if (!readU8(REG_ERROR, v)) {
        setError(RB_ERR_IO);
        return 0xFF;
    }
    setError(RB_OK);
    return v;
}

bool RbAmp::readEventFlags(uint8_t& out) noexcept {
    if (!readU8(v2::REG_EVENT_FLAGS, out)) {
        setError(RB_ERR_IO);
        return false;
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::clearEventFlags(uint8_t mask) noexcept {
    /* Write-1-to-clear: writing the mask back clears those sticky bits. */
    if (!writeReg(v2::REG_EVENT_FLAGS, mask)) {
        setError(RB_ERR_IO);
        return false;
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::hasError(bool& out) noexcept {
    uint8_t flags = 0;
    if (!readEventFlags(flags)) return false;  /* error already set */
    out = (flags & v2::EVENT_ERROR) != 0;
    return true;
}

bool RbAmp::clearError() noexcept {
    if (!writeCmd(v2::CMD_CLEAR_ERROR)) {
        setError(RB_ERR_IO);
        return false;
    }
    if (v2::SETTLE_MS_CLEAR_ERROR) delay(v2::SETTLE_MS_CLEAR_ERROR);
    setError(RB_OK);
    return true;
}

/* ============================================================================
 * Fleet / multi-module (single-device side)
 * ============================================================================ */

bool RbAmp::enableGc(bool enable) noexcept {
    uint8_t cfg = 0;
    if (!readU8(REG_FLEET_CONFIG, cfg)) {
        setError(RB_ERR_IO);
        return false;
    }
    const uint8_t next = enable ? (cfg | 0x01) : (cfg & ~0x01);
    if (!writeReg(REG_FLEET_CONFIG, next)) {
        setError(RB_ERR_IO);
        return false;
    }
    /* Persist (production-OK) then reset — the GC ISR is wired only at boot. */
    if (!writeCmd(CMD_SAVE_USER_CONFIG)) {
        setError(RB_ERR_IO);
        return false;
    }
    delay(SETTLE_MS_SAVE_USER_CONFIG);
    return reset();  /* CMD_RESET + 100 ms; sets RB_OK */
}

bool RbAmp::readFleetConfig(uint8_t& out) noexcept {
    if (!readU8(REG_FLEET_CONFIG, out)) {
        setError(RB_ERR_IO);
        return false;
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::setGroupId(uint8_t group) noexcept {
    if (!writeReg(REG_GROUP_ID, group)) {
        setError(RB_ERR_IO);
        return false;
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::readGroupId(uint8_t& out) noexcept {
    if (!readU8(REG_GROUP_ID, out)) {
        setError(RB_ERR_IO);
        return false;
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::readGcTick(uint16_t& out) noexcept {
    /* A/B torn-read protected — fleet-sync witness must not accept a torn read. */
    if (!readU16AB(v2::REG_GC_TICK, out)) {
        if (lastError() == RB_OK) setError(RB_ERR_IO);
        return false;
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::readLabel(char out[9]) noexcept {
    for (uint8_t i = 0; i < 8; ++i) {
        uint8_t b = 0;
        if (!readU8(static_cast<uint8_t>(v2::REG_LABEL + i), b)) {
            setError(RB_ERR_IO);
            return false;
        }
        out[i] = static_cast<char>(b);
    }
    out[8] = '\0';
    setError(RB_OK);
    return true;
}

bool RbAmp::writeLabel(const char* label) noexcept {
    if (label == nullptr) {
        setError(RB_ERR_PARAM);
        return false;
    }
    bool past_nul = false;
    for (uint8_t i = 0; i < 8; ++i) {
        char c = 0;
        if (!past_nul) {
            c = label[i];
            if (c == '\0') past_nul = true;  /* zero-fill the remaining bytes */
        }
        if (!writeReg(static_cast<uint8_t>(v2::REG_LABEL + i), static_cast<uint8_t>(c))) {
            setError(RB_ERR_IO);
            return false;
        }
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::readActiveAddress(uint8_t& out) noexcept {
    /* A/B protected — a torn address read on a shared bus is corrupting. */
    if (!readU8AB(REG_I2C_ADDRESS, out)) {
        if (lastError() == RB_OK) setError(RB_ERR_IO);
        return false;
    }
    setError(RB_OK);
    return true;
}

/* ============================================================================
 * Static / multi-module
 * ============================================================================ */

bool RbAmp::broadcastLatch(TwoWire& bus) noexcept {
    /* v1.3: General-Call latch is implemented in firmware (truth-doc §5), opt-in
     * per device via enableGc(). All-call form (group 0x00, tick 0x0000). On
     * v1.0/v1.1 firmware GC is disabled and the frame is harmlessly ignored —
     * callers should fall back to per-device sequential latchPeriod() there. */
    return broadcastLatchGroup(bus, 0x00, 0x0000);
}

bool RbAmp::broadcastLatchGroup(TwoWire& bus, uint8_t group, uint16_t tick) noexcept {
    /* Wire frame to General-Call address 0x00: A5 27 group tick_lo tick_hi.
     * 0xA5 = magic, 0x27 = CMD_LATCH_PERIOD opcode. Only modules with
     * FLEET_CONFIG bit0 set AND matching GROUP_ID (or group 0x00) latch. */
    bus.beginTransmission(static_cast<uint8_t>(0x00));
    bus.write(static_cast<uint8_t>(0xA5));
    bus.write(static_cast<uint8_t>(CMD_LATCH_PERIOD));
    bus.write(group);
    bus.write(static_cast<uint8_t>(tick & 0xFF));
    bus.write(static_cast<uint8_t>(tick >> 8));
    /* Static method: no per-instance error state — report via return value. */
    return bus.endTransmission() == 0;
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

/* SPEC §B.5: the ESP-IDF v5 i2c_master driver (which Arduino-ESP32's Wire wraps)
 * intermittently NACKs rbAmp transactions at ~20 % rate at 100 kHz; an N-attempt
 * retry with a short gap drops residual error to <0.8 %. Non-ESP32 platforms
 * don't show the NACK pattern, so retry is gated to 1 attempt to avoid useless
 * latency. Pre-define RBAMP_NACK_RETRY_ATTEMPTS to override (e.g. 5 for soak).
 *
 * Both reads AND writes use this discipline (L6, esp-idf reference): a silently-
 * NACKed config write (e.g. CMD_SET_CT_MODEL_CHn) is MORE dangerous than a
 * dropped read — nothing comes back wrong, the op simply never lands, and a
 * later REG_ERROR read reflects the PRIOR op's OK, so a write-then-verify can't
 * see it. On a contended multi-module bus this bound one of two channels
 * non-deterministically until write-retry parity was added.
 *
 * @warning (L8) Arduino-ESP32 Wire wraps the same IDF i2c_master driver, which
 *          can spin INFINITELY on i2c_ll_is_bus_busy below this library on a
 *          MARGINAL bus (weak pull-ups, long traces, EMI under load). The
 *          xfer-timeout does NOT bound that pre-send wait and the library cannot
 *          interrupt a same-task blocking Wire call — the retry/gap below never
 *          runs because the first transfer never returns. Mitigation is a
 *          three-layer discipline, NOT this retry loop: (1) proper external
 *          ~4.7 kΩ pull-ups, (2) no debugger/NRST attached in production,
 *          (3) an app-level task-WDT on the polling task that reboots cleanly
 *          out of a wedge. Severity is a full-duration-soak measurement. */
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

bool RbAmp::writeReg(uint8_t reg, uint8_t val) noexcept {
    /* Write NACK-retry parity with readU8 (L6). A dropped write is invisible
     * to a follow-up status read, so retry it the same N times + gap. */
    constexpr uint8_t kAttempts = RBAMP_NACK_RETRY_ATTEMPTS;
    for (uint8_t attempt = 0; attempt < kAttempts; ++attempt) {
        bus_.beginTransmission(addr_);
        bus_.write(reg);
        bus_.write(val);
        if (bus_.endTransmission() == 0) {
            return true;
        }
        if (attempt + 1 < kAttempts) {
            delay(RBAMP_NACK_RETRY_GAP_MS);
        }
    }
    retry_exhaustion_count_++;
    return false;
}

bool RbAmp::writeCmd(uint8_t cmd) noexcept {
    return writeReg(REG_COMMAND, cmd);
}

bool RbAmp::readU8(uint8_t reg, uint8_t& out) noexcept {
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

bool RbAmp::readFloatLE(uint8_t reg, float& out, float max_abs) noexcept {
    uint8_t b[4];
    for (uint8_t i = 0; i < 4; ++i) {
        if (!readU8(static_cast<uint8_t>(reg + i), b[i])) {
            return false;
        }
    }
    /* memcpy is the portable bit-reinterpret pattern; avoids strict-aliasing UB */
    memcpy(&out, b, 4);
    /* SPEC §B.5 — sanity filter against IDF i2c_master buffer-leak ghost values
     * (e.g. 0x3C2FFB3F = 1.962 V) that survive retry. Per-quantity ceiling
     * passed by caller: U≤500, I≤150, P/Q≤30000, PF≤1.5. Catches NaN / Inf /
     * exotic-bit-pattern + over-range. No lower bound — brownout / disconnect
     * / off-grid / negative real power (export) pass through. */
    if (!isfinite(out) || fabsf(out) > max_abs) {
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
