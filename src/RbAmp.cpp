/**
 * @file    RbAmp.cpp
 * @brief   Implementation of the Arduino client class for rbAmp.
 * @author  rbAmp team
 * @date    2026
 *
 * @details
 * All wire-level protocol invariants from @c libs/spec/SPEC.md are enforced
 * here:
 *  - BASIC line: one I2C address phase per byte (SPEC §6).
 *  - STD line (v1.6.0+): 16-bit big-endian register pointer + auto-increment
 *    burst reads (RbAmpStdRegisters.h); line detected once in begin() and
 *    routed internally, the 8-bit primitives refuse on an STD instance.
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
      std_(false),
      gate_refused_(false),
      std_version_(0),
      std_caps_(0),
      std_status_(0),
      n_voltage_(0),
      n_current_(0),
      std_variant_(0),
      std_active_bank_(0),
      std_ota_bank_size_(0),
      std_frame_cnt_(0),
      std_have_frame_cnt_(false),
      std_fw_ver_(0),
      std_period_src_(RbAmpStdPeriodSource::None),
      std_lt_anchor_(),
      std_lt_anchored_(false),
      std_last_avg_total_(0.0f),
      std_have_last_avg_(false),
      drdy_line_(-1),
      drdy_seen_edges_(0),
      drdy_last_seq_(0),
      drdy_have_seq_(false),
      drdy_pending_flags_(0),
      begin_event_flags_(0),
      sct_reset_seen_(false),
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

    /* 0. Product line. The STD identity probe goes FIRST — see stdDetect() for
     *    why the order is not negotiable. A re-run of begin() re-detects. */
    std_ = false;
    begin_event_flags_ = 0;
    bool is_std = false;
    if (!stdDetect(is_std)) {
        return false;   /* error already set (transport / unsupported STD map) */
    }
    if (is_std) {
        return stdBegin();
    }

    /* 1. BASIC line — probe REG_VERSION to confirm the device is alive at addr_ */
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

    /* 2b. SCT-013 rating correction (BASIC fw <= 1.4.19): read model + applied
     *     gain per channel; may restart the module once (see sctRatingCompute).
     *     Not fatal — an unreadable channel keeps scale 1 with status Unknown. */
    {
        bool reset_issued = false;
        (void)sctRatingCompute(reset_issued);
    }

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

    /* Event flags as found (RESET_OCCURRED tells the caller the module
     * rebooted before we met it); nothing is cleared here. */
    {
        uint8_t f = 0;
        if (readU8(v2::REG_EVENT_FLAGS, f) && f != 0xFF) begin_event_flags_ = f;
    }
    sct_reset_seen_ = (begin_event_flags_ & v2::EVENT_RESET_OCCURRED) != 0;  /* already acted on in 2b */

    setError(RB_OK);
    return true;
}

bool RbAmp::basicPrimePeriod() noexcept {
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
    if (std_) {
        uint8_t id[2] = {0, 0};
        if (!stdRead(rbamp_std::REG_ID, id, 2)) {
            return false;   /* RB_ERR_NACK set */
        }
        if (id[0] != 'S' || id[1] != 'T') {
            setError(RB_ERR_VERSION);
            return false;
        }
        setError(RB_OK);
        return true;
    }
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
        if (std_) {
            /* STD: the host path comes up before the sensor path, so "ready"
             * means the sensor frames are flowing (STATUS.sensor_ok). */
            if (stdReadU8(rbamp_std::REG_STATUS, status) &&
                (status & rbamp_std::ST_SENSOR_OK) != 0) {
                setError(RB_OK);
                return true;
            }
        } else if (readU8(REG_STATUS, status) && (status & 0x01) != 0) {
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
    if (std_) {
        /* STD: the register-map version byte (0x30+). */
        if (!stdReadU8(rbamp_std::REG_VERSION, v)) {
            return 0;
        }
        setError(RB_OK);
        return v;
    }
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

    if (variant_ok && variant >= 0x01 && variant <= 0x08) {
        /* v1.2+ path — variant byte gives SKU + voltage presence; the I-channel
         * COUNT is authoritative in REG_TOPOLOGY (0x24). BUILD_VARIANT codes
         * (truth-doc §1.2): 1=UI1 2=UI2 3=UI3 4=I1 5=I2 6=I3 7=UI5 8=UI7.
         * Voltage hardware present for {1,2,3,7,8}, absent for {4,5,6}. */
        variant_ = static_cast<RbAmpVariant>(variant);
        has_voltage_hw_ = (variant == 1 || variant == 2 || variant == 3 ||
                           variant == 7 || variant == 8);

        uint8_t ch_from_variant;
        switch (variant) {
            case 0x01: case 0x04: ch_from_variant = 1; break;
            case 0x02: case 0x05: ch_from_variant = 2; break;
            case 0x03: case 0x06: ch_from_variant = 3; break;
            case 0x07:            ch_from_variant = 5; break;
            case 0x08:            ch_from_variant = 7; break;
            default:              ch_from_variant = 1; break;
        }

        /* REG_TOPOLOGY (0x24) = I-channel count {1,2,3,5,7}, authoritative when
         * readable; fall back to the variant-derived count otherwise. */
        uint8_t topo = 0;
        if (readU8(v2::REG_TOPOLOGY, topo) &&
            (topo == 1 || topo == 2 || topo == 3 || topo == 5 || topo == 7)) {
            channels_ = topo;
        } else {
            channels_ = ch_from_variant;
        }
        /* Every per-channel array in the snapshots is RBAMP_MAX_CHANNELS wide
         * and the fill loops run to channels_: never let the count exceed the
         * arrays (only reachable with a smaller RBAMP_MAX_CHANNELS override on
         * a UI5/UI7). Channels beyond the cap are not exposed (RB_ERR_PARAM). */
        if (channels_ > RBAMP_MAX_CHANNELS) {
            if (log_) {
                log_->print(F("[rbamp] module has ")); log_->print(channels_);
                log_->print(F(" current channels; this library exposes the first "));
                log_->println(RBAMP_MAX_CHANNELS);
            }
            channels_ = RBAMP_MAX_CHANNELS;
        }

        switch (channels_) {
            case 1: topology_ = RbAmpTopology::Single;     break;
            case 2: topology_ = RbAmpTopology::SplitPhase; break;
            case 3: topology_ = RbAmpTopology::ThreePhase; break;
            case 5: topology_ = RbAmpTopology::Five;       break;
            case 7: topology_ = RbAmpTopology::Seven;      break;
            default: topology_ = RbAmpTopology::Single;    break;
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

float RbAmp::readChannelWindow(uint8_t ch, uint8_t field, float max_abs) noexcept {
    if (ch >= channels_) {
        setError(RB_ERR_PARAM);
        return NAN;
    }
    /* Rule #1: re-select before EVERY read — the data buffer at 0x3C..0x3F is a
     * snapshot of the value at select time; a read without a fresh select
     * returns the previous snapshot. The selector is a single DUT-wide resource,
     * so select+read is one atomic section per call. */
    const uint8_t sel = static_cast<uint8_t>((field << 4) | (ch & 0x0F));
    if (!writeReg(v2::REG_CHANNEL_SELECT, sel)) {
        setError(RB_ERR_IO);
        return NAN;
    }
    float v = NAN;
    if (!readFloatLE(v2::REG_CHANNEL_DATA, v, max_abs)) {
        setError(RB_ERR_IO);
        return NAN;
    }
    setError(RB_OK);
    return v;
}

bool RbAmp::foldChannelWindow(uint8_t ch, uint8_t field, float& out_field,
                              uint8_t& mask, uint8_t bit, float max_abs) noexcept {
    /* Re-select (rule #1), then fold the data read exactly like the flat path. */
    const uint8_t sel = static_cast<uint8_t>((field << 4) | (ch & 0x0F));
    if (!writeReg(v2::REG_CHANNEL_SELECT, sel)) {
        return false;   /* transport — caller aborts */
    }
    return foldField(readFloatLE(v2::REG_CHANNEL_DATA, out_field, max_abs),
                     out_field, mask, bit);
}

bool RbAmp::writeChannelModel(uint8_t ch, uint8_t code) noexcept {
    /* Per-channel CT model via the window, field 15 (CT_MODEL). Select then write
     * the four data bytes LSB-first — the firmware does NOT support a burst, so
     * each byte is its own transaction and the preset is applied on the 0x3F
     * write. Only the low byte carries the model code. Caller has already
     * validated ch < channels_ and fw >= 0x0A. */
    const uint8_t sel = static_cast<uint8_t>((v2::CHFIELD_CT_MODEL << 4) | (ch & 0x0F));
    if (!writeReg(v2::REG_CHANNEL_SELECT, sel) ||
        !writeReg(static_cast<uint8_t>(v2::REG_CHANNEL_DATA + 0), code) ||
        !writeReg(static_cast<uint8_t>(v2::REG_CHANNEL_DATA + 1), 0) ||
        !writeReg(static_cast<uint8_t>(v2::REG_CHANNEL_DATA + 2), 0) ||
        !writeReg(static_cast<uint8_t>(v2::REG_CHANNEL_DATA + 3), 0)) {
        setError(RB_ERR_IO);
        return false;
    }
    return true;
}

/* Per-channel scalar quantities and where each lives on each line. ONE table +
 * ONE router (readQty) so a new quantity is a table row, and no public
 * accessor can reintroduce its own routing (L8, PUBLIC_API_TRUTH channel-access
 * canon). BASIC: ch0-2 flat register (flat_base + ch*4), ch3+ channel window
 * (chfield); a quantity the BASIC line lacks has chfield 0xFF. STD: fixed-point
 * field at rbamp_std::CURRENT(ch) + std_off, scaled to SI. */
namespace {
enum : uint8_t { Q_I_RMS, Q_I_PEAK, Q_P, Q_PF, Q_Q, Q_S };
struct QtyRow {
    uint8_t  chfield;    /* BASIC window field, 0xFF = not on BASIC */
    uint8_t  flat_base;  /* BASIC ch0 flat register */
    uint16_t std_off;    /* STD offset within the CURRENT block */
    bool     std_i16;    /* STD field width: i16 (else i32) */
    float    std_scale;  /* STD wire unit -> SI */
    float    max_abs;    /* plausibility bound */
};
const QtyRow kQty[] = {
    /* Q_I_RMS  */ { v2::CHFIELD_I_RMS,  REG_V03_I0_RMS,  rbamp_std::CURRENT_I_RMS,  false, 1e-6f,           150.0f   },
    /* Q_I_PEAK */ { v2::CHFIELD_I_PEAK, REG_V03_I0_PEAK, rbamp_std::CURRENT_I_PEAK, false, 1e-6f,           250.0f   },
    /* Q_P      */ { v2::CHFIELD_P_REAL, REG_V03_P0_REAL, rbamp_std::CURRENT_P,      false, 1e-3f,           30000.0f },
    /* Q_PF     */ { v2::CHFIELD_PF,     REG_V03_PF0,     rbamp_std::CURRENT_PF,     true,  1.0f / 1024.0f,  1.5f     },
    /* Q_Q      */ { 0xFF,               0,               rbamp_std::CURRENT_Q,      false, 1e-3f,           30000.0f },
    /* Q_S      */ { 0xFF,               0,               rbamp_std::CURRENT_S,      false, 1e-3f,           30000.0f },
};
} /* namespace */

float RbAmp::readQty(uint8_t q, uint8_t ch) noexcept {
    if (ch >= channels_) {
        setError(RB_ERR_PARAM);
        return NAN;
    }
    const QtyRow& r = kQty[q];
    if (std_) {
        float v = NAN;
        if (!stdReadScaled(static_cast<uint16_t>(rbamp_std::CURRENT(ch) + r.std_off),
                           r.std_i16, r.std_scale, r.max_abs, v)) {
            return NAN;
        }
        setError(RB_OK);
        return v;
    }
    if (r.chfield == 0xFF) {
        setError(RB_ERR_NOT_IMPLEMENTED);   /* quantity not measured on the BASIC line */
        return NAN;
    }
    /* SCT-013 rating correction (BASIC fw <= 1.4.19): I and P scale with the
     * gain, PF does not; the plausibility bound is compared in true units. */
    const float s = (q == Q_PF) ? 1.0f : sct_[ch].scale;
    const float v = (ch >= 3) ? readChannelWindow(ch, r.chfield, r.max_abs / s)
                              : readRtFloat(r.flat_base, ch, 4, r.max_abs / s);
    return isnan(v) ? v : v * s;
}

float RbAmp::readVoltage(uint8_t phase) noexcept {
    float v = NAN;
    if (std_) {
        if (phase >= n_voltage_) {
            setError(RB_ERR_PARAM);
            return NAN;
        }
        if (!stdReadScaled(rbamp_std::VOLTAGE(phase) + rbamp_std::VOLTAGE_U_RMS,
                           false, 1e-3f, 500.0f, v)) {
            return NAN;
        }
        setError(RB_OK);
        return v;
    }
    if (phase != 0) {
        setError(RB_ERR_PARAM);
        return NAN;
    }
    if (!readFloatLE(REG_V03_U_RMS, v, 500.0f)) {
        setError(RB_ERR_IO);
        return NAN;
    }
    setError(RB_OK);
    return v;
}

float RbAmp::readVoltagePeak(uint8_t phase) noexcept {
    float v = NAN;
    if (std_) {
        if (phase >= n_voltage_) {
            setError(RB_ERR_PARAM);
            return NAN;
        }
        if (!stdReadScaled(rbamp_std::VOLTAGE(phase) + rbamp_std::VOLTAGE_U_PEAK,
                           false, 1e-3f, 800.0f, v)) {
            return NAN;
        }
        setError(RB_OK);
        return v;
    }
    if (phase != 0) {
        setError(RB_ERR_PARAM);
        return NAN;
    }
    if (!readFloatLE(REG_V03_U_PEAK, v, 800.0f)) {
        setError(RB_ERR_IO);
        return NAN;
    }
    setError(RB_OK);
    return v;
}

float RbAmp::readCurrent(uint8_t ch) noexcept      { return readQty(Q_I_RMS, ch); }
float RbAmp::readCurrentPeak(uint8_t ch) noexcept  { return readQty(Q_I_PEAK, ch); }
float RbAmp::readPower(uint8_t ch) noexcept        { return readQty(Q_P, ch); }
float RbAmp::readPowerFactor(uint8_t ch) noexcept  { return readQty(Q_PF, ch); }
float RbAmp::readReactivePower(uint8_t ch) noexcept { return readQty(Q_Q, ch); }
float RbAmp::readApparentPower(uint8_t ch) noexcept { return readQty(Q_S, ch); }

float RbAmp::readFrequency() noexcept {
    if (std_) {
        uint16_t chz = 0;   /* centi-hertz */
        if (!stdReadU16(rbamp_std::REG_FREQ, chz)) {
            return NAN;
        }
        setError(RB_OK);
        return static_cast<float>(chz) / 100.0f;
    }
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
    if (std_) {
        return stdReadAll(out);
    }

    /* Per-field: a sanity-reject sets the field NaN + flags implausible and the
     * snapshot stays usable; only a transport failure aborts (matches the
     * esp-idf reference _fold_field — keeps good fields when one is over-range). */
    if (!foldField(readFloatLE(REG_V03_U_RMS, out.voltage, 500.0f),
                   out.voltage, out.implausible, RBAMP_FIELD_VOLTAGE)) { setError(RB_ERR_IO); return false; }
    if (!foldField(readFloatLE(REG_V03_U_PEAK, out.voltage_peak, 800.0f),
                   out.voltage_peak, out.implausible, RBAMP_FIELD_VOLTAGE)) { setError(RB_ERR_IO); return false; }

    for (uint8_t ch = 0; ch < RBAMP_MAX_CHANNELS; ++ch) {
        if (ch >= channels_) {
            out.current[ch] = out.current_peak[ch] = 0.0f;
            out.power[ch] = out.power_factor[ch] = 0.0f;
            continue;
        }
        /* SCT-013 rating correction: bounds in true units, I / P scaled after
         * the fold (NaN stays NaN), PF untouched. */
        const float s = sct_[ch].scale;
        if (ch < 3) {
            /* Flat block ch0-2 — one contiguous set of reads. */
            const uint8_t off = static_cast<uint8_t>(ch * 4);
            if (!foldField(readFloatLE(static_cast<uint8_t>(REG_V03_I0_RMS + off), out.current[ch], 150.0f / s),
                           out.current[ch], out.implausible, RBAMP_FIELD_CURRENT)) { setError(RB_ERR_IO); return false; }
            if (!foldField(readFloatLE(static_cast<uint8_t>(REG_V03_I0_PEAK + off), out.current_peak[ch], 250.0f / s),
                           out.current_peak[ch], out.implausible, RBAMP_FIELD_CURRENT)) { setError(RB_ERR_IO); return false; }
            if (!foldField(readFloatLE(static_cast<uint8_t>(REG_V03_P0_REAL + off), out.power[ch], 30000.0f / s),
                           out.power[ch], out.implausible, RBAMP_FIELD_POWER)) { setError(RB_ERR_IO); return false; }
            if (!foldField(readFloatLE(static_cast<uint8_t>(REG_V03_PF0 + off), out.power_factor[ch], 1.5f),
                           out.power_factor[ch], out.implausible, RBAMP_FIELD_PF)) { setError(RB_ERR_IO); return false; }
        } else {
            /* ch3+ (senior SKUs) — channel access window, re-select per field. */
            if (!foldChannelWindow(ch, v2::CHFIELD_I_RMS,  out.current[ch],      out.implausible, RBAMP_FIELD_CURRENT, 150.0f / s))   { setError(RB_ERR_IO); return false; }
            if (!foldChannelWindow(ch, v2::CHFIELD_I_PEAK, out.current_peak[ch], out.implausible, RBAMP_FIELD_CURRENT, 250.0f / s))   { setError(RB_ERR_IO); return false; }
            if (!foldChannelWindow(ch, v2::CHFIELD_P_REAL, out.power[ch],        out.implausible, RBAMP_FIELD_POWER,   30000.0f / s)) { setError(RB_ERR_IO); return false; }
            if (!foldChannelWindow(ch, v2::CHFIELD_PF,     out.power_factor[ch], out.implausible, RBAMP_FIELD_PF,      1.5f))     { setError(RB_ERR_IO); return false; }
        }
        if (s != 1.0f) {
            if (!isnan(out.current[ch]))      out.current[ch]      *= s;
            if (!isnan(out.current_peak[ch])) out.current_peak[ch] *= s;
            if (!isnan(out.power[ch]))        out.power[ch]        *= s;
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
    if (std_) {
        if (std_period_src_ == RbAmpStdPeriodSource::Counters) {
            /* Counter modules have nothing to latch: the period is read on demand. */
            setError(RB_OK);
            return true;
        }
        if (std_period_src_ != RbAmpStdPeriodSource::Latch) {
            setError(RB_ERR_NOT_IMPLEMENTED);
            return false;
        }
        if (!stdLatchBank(0)) {
            return false;   /* error already set */
        }
    } else if (!writeCmd(CMD_LATCH_PERIOD)) {
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
    if (std_) {
        if (std_period_src_ == RbAmpStdPeriodSource::Counters) {
            setError(RB_OK);
            return std_lt_anchored_;     /* counters: valid once anchored */
        }
        if (std_period_src_ != RbAmpStdPeriodSource::Latch) {
            setError(RB_ERR_NOT_IMPLEMENTED);
            return false;
        }
        /* Bank 0 has a snapshot. Sticky on the module — not a freshness flag. */
        if (!stdReadU8(rbamp_std::REG_PD_VALID, v)) {
            return false;
        }
        return (v & 0x01) != 0;
    }
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
    if (std_) {
        if (std_period_src_ != RbAmpStdPeriodSource::Latch) {
            /* Counters are totals only: a per-channel average exists just on
             * a single-channel module, and only once a period was consumed. */
            if (std_period_src_ != RbAmpStdPeriodSource::Counters || channels_ != 1) {
                setError(RB_ERR_NOT_IMPLEMENTED);
                return NAN;
            }
            if (!std_have_last_avg_) { setError(RB_ERR_NOT_READY); return NAN; }
            setError(RB_OK);
            return std_last_avg_total_;
        }
        RbAmpStdLatch snap;
        if (!stdReadLatchBank(0, snap)) {
            return NAN;
        }
        setError(RB_OK);
        return snap.p_avg_ch[ch];
    }
    /* ch3+ (senior SKUs): the latched period average is exposed via the channel
     * window field 13 (PERIOD_AVG_P), latched atomically with the flat ch0-2
     * trio by CMD_LATCH_PERIOD. */
    const float s = sct_[ch].scale;   /* SCT-013 rating correction */
    if (ch >= 3) {
        const float w = readChannelWindow(ch, v2::CHFIELD_PERIOD_AVG_P, 30000.0f / s);
        return isnan(w) ? w : w * s;
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
    if (!readFloatLE(reg, v, 30000.0f / s)) {
        setError(RB_ERR_IO);
        return NAN;
    }
    setError(RB_OK);
    return v * s;
}

float RbAmp::readPeriodMaxPower() noexcept {
    float v = NAN;
    if (std_) {
        if (std_period_src_ != RbAmpStdPeriodSource::Latch) {
            setError(RB_ERR_NOT_IMPLEMENTED);   /* no period envelope on the counter path */
            return NAN;
        }
        if (!stdReadScaled(rbamp_std::LATCH(0) + rbamp_std::LATCH_MAX_P,
                           false, 1e-3f, 30000.0f, v)) {
            return NAN;
        }
        setError(RB_OK);
        return v;
    }
    const float s = sct_[0].scale;   /* SCT-013 rating correction (channel 0 envelope) */
    if (!readFloatLE(REG_V03_PERIOD_MAX_P_F0, v, 30000.0f / s)) {
        setError(RB_ERR_IO);
        return NAN;
    }
    setError(RB_OK);
    return v * s;
}

uint32_t RbAmp::readPeriodLatchMs() noexcept {
    uint32_t v = 0;
    if (std_) {
        /* No module-side clock on this line — master_dt_ms is the only period. */
        setError(RB_ERR_NOT_IMPLEMENTED);
        return 0;
    }
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
    for (uint8_t i = 0; i < RBAMP_MAX_CHANNELS; ++i) {
        out.avg_p[i] = 0.0f;
    }
    out.avg_p_total      = 0.0f;
    out.wh_net           = 0.0;
    out.module_restarted = false;
    if (std_) {
        switch (std_period_src_) {
        case RbAmpStdPeriodSource::Latch:    return stdReadPeriodSnapshot(out, settle_ms, skip_latch);
        case RbAmpStdPeriodSource::Counters: return stdReadPeriodCounters(out);
        default:                             setError(RB_ERR_NOT_IMPLEMENTED); return false;
        }
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
    {
        float t = 0.0f;
        for (uint8_t ch = 0; ch < channels_; ++ch) t += out.avg_p[ch];
        out.avg_p_total = t;
        out.wh_net      = static_cast<double>(t) * out.master_dt_ms / 3.6e6;
    }

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
    /* Read before write: the value the module already holds is a no-op (the
     * firmware clears the CT models on ANY class write), a factory-paired
     * class is not rewritten. */
    uint8_t cur = 0;
    if (!readSensorClass(cur)) {
        return false;   /* error already set */
    }
    switch (sensorClassWritePolicy(cur, static_cast<uint8_t>(cls))) {
        case WritePolicy::Noop:
            setError(RB_OK);
            return true;
        case WritePolicy::Paired:
            if (log_) {
                log_->print(F("[rbamp] setSensorClass refused: the module is calibrated with its fitted sensor (class "));
                log_->print(cur);
                log_->println(F(")"));
            }
            setError(RB_ERR_MODE);
            return false;
        default:
            break;
    }
    if (std_) {
        /* STD: CFG_CLASS is metadata on this line (no preset table); persisted
         * by CMD_SAVE_CONFIG, whose verdict saveUserConfig() checks. */
        if (!stdWriteU8(rbamp_std::REG_CFG_CLASS, static_cast<uint8_t>(cls))) {
            return false;
        }
        return saveUserConfig();
    }
    if (!writeReg(REG_SENSOR_CLASS, static_cast<uint8_t>(cls))) {
        setError(RB_ERR_IO);
        return false;
    }
    /* SENSOR_CLASS is user_config — persist via CMD_SAVE_USER_CONFIG (ungated).
     * CMD_SAVE_GAINS is factory/develop-gated and a silent no-op in production. */
    if (!saveUserConfig()) {
        return false;  /* error already set */
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::ctModelValid(RbAmpSensorClass cls, uint8_t code) noexcept {
    /* v1.5.0: the accepted-set is no longer a hand-maintained whitelist — it
     * drifted (WIRED_CT was absent entirely). This is now a KNOWN-CODE check
     * against the generated registry (RbAmpSensorModels.h ← sensor_models.yaml).
     * A registry hit does NOT mean the connected module accepts the code:
     * acceptance is runtime truth — the module returns ERR_PARAM for a code
     * without a preset row and keeps the channel's previous model. This fast-
     * fail only rejects codes that are not in the registry at all (typos). */
    return rbamp_sensor_model_lookup(static_cast<uint8_t>(cls), code) != nullptr;
}

bool RbAmp::readSensorClass(uint8_t& out) noexcept {
    if (std_) {
        if (!stdReadU8(rbamp_std::REG_CFG_CLASS, out)) return false;
        setError(RB_OK);
        return true;
    }
    if (!readU8(REG_SENSOR_CLASS, out)) return false;   /* RB_ERR_NACK set */
    setError(RB_OK);
    return true;
}

bool RbAmp::sensorClassPaired(uint8_t cls) noexcept {
    return cls == RBAMP_SENSOR_CLASS_WIRED_CT || cls == RBAMP_SENSOR_CLASS_BUILTIN_CT;
}

RbAmp::WritePolicy RbAmp::sensorClassWritePolicy(uint8_t cur, uint8_t want) noexcept {
    if (cur == want) return WritePolicy::Noop;
    if (sensorClassPaired(cur)) return WritePolicy::Paired;
    return WritePolicy::Proceed;
}

RbAmp::WritePolicy RbAmp::ctModelWritePolicy(uint8_t cls, uint8_t held, uint8_t want,
                                             bool force) noexcept {
    if (force) return WritePolicy::Proceed;
    if (held == want) return WritePolicy::Noop;
    if (sensorClassPaired(cls)) return WritePolicy::Paired;
    return WritePolicy::Proceed;
}

bool RbAmp::ctModelGuard(uint8_t channel, uint8_t code, bool force, bool& skip) noexcept {
    skip = false;
    uint8_t cls = 0, held = 0;
    if (!readSensorClass(cls)) {
        return false;   /* error already set */
    }
    if (!readCTModelCh(channel, held)) {
        held = 0xFF;    /* unreadable mirror: never mistaken for "same" */
    }
    switch (ctModelWritePolicy(cls, held, code, force)) {
        case WritePolicy::Noop:
            skip = true;
            setError(RB_OK);
            return true;
        case WritePolicy::Paired:
            if (log_) {
                log_->print(F("[rbamp] setCTModel ch"));
                log_->print(channel);
                log_->print(F(" refused: the module is calibrated with its fitted sensor (class "));
                log_->print(cls);
                log_->println(F("); use setCTModelChForce in service"));
            }
            setError(RB_ERR_MODE);
            return false;
        default:
            setError(RB_OK);
            return true;
    }
}

bool RbAmp::setCTModel(uint8_t code) noexcept {
    if (code < 1) {  /* 0 = unset; upper bound is registry-checked below (v1.5.0) */
        setError(RB_ERR_PARAM);
        return false;
    }
    if (std_) {
        return setCTModel(0, code);
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
    /* Read before write: the model channel 0 already holds is not re-applied
     * (the module would replace the unit's calibration with the generic
     * preset); a factory-paired module is not reconfigured. */
    bool skip = false;
    if (!ctModelGuard(0, code, false, skip)) {
        return false;   /* error already set */
    }
    if (skip) {
        return true;
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
    return saveUserConfig() && sctAfterModelWrite();  /* CT_MODEL is user_config — SAVE_GAINS is dev-gated (silent no-op in production) */
}

bool RbAmp::setCTModel(uint8_t channel, uint8_t code) noexcept {
    return setCtModelImpl(channel, code, false);
}

bool RbAmp::setCTModelChForce(uint8_t channel, uint8_t code) noexcept {
    return setCtModelImpl(channel, code, true);
}

bool RbAmp::setCtModelImpl(uint8_t channel, uint8_t code, bool force) noexcept {
    /* v1.5.0: validate against the module's actual channel count, not a hardcoded
     * 3 — senior SKUs (UI5/UI7) bind ch3+ via the window (field 15). */
    if (channel >= channels_ || code < 1) {
        setError(RB_ERR_PARAM);
        return false;
    }
    bool skip = false;
    if (std_) {
        /* STD: reference-only metadata (sets no gain) + readback + SAVE_CONFIG.
         * Same read-before-write guard as BASIC (parity across the lines). */
        if (!ctModelGuard(channel, code, force, skip)) {
            return false;  /* error already set */
        }
        if (skip) {
            return true;
        }
        if (!stdApplyCtModel(channel, code)) {
            return false;  /* error already set */
        }
        return saveUserConfig();
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
    if (!ctModelValid(static_cast<RbAmpSensorClass>(cls), code)) {  /* known-code fast-fail */
        setError(RB_ERR_PARAM);
        return false;
    }
    /* Read before write: the model the channel already holds is not re-applied
     * (the module would replace the unit's calibration with the generic
     * preset); a factory-paired module is not reconfigured unless forced. */
    if (!ctModelGuard(channel, code, force, skip)) {
        return false;   /* error already set */
    }
    if (skip) {
        return true;
    }

    if (channel >= 3) {
        /* Senior channel: bind via the channel window, field 15 (CT_MODEL). The
         * flat CMD_SET_CT_MODEL_CHn opcodes only cover ch0-2. Needs v1.4.18+
         * (fw ver 0x0A) where field 15 is signed on silicon. */
        if (fw < 0x0A) {
            setError(RB_ERR_VERSION);
            return false;
        }
        if (!writeChannelModel(channel, code)) {
            return false;  /* error already set (RB_ERR_IO) */
        }
        /* Acceptance is runtime: an unknown (class,model) leaves the channel's
         * previous model in place. Read the applied model back and surface a
         * mismatch as RB_ERR_PARAM (readback-confirmed per test rule). */
        uint8_t applied = 0;
        if (!readCTModelCh(channel, applied)) {
            return false;  /* error already set */
        }
        if (applied != code) {
            setError(RB_ERR_PARAM);
            return false;
        }
        return saveUserConfig() && sctAfterModelWrite();  /* CT_MODEL is user_config — SAVE_GAINS is dev-gated (silent no-op in production) */
    }

    /* v1.3 A1 sequence (ch0-2, order-independent — REG_CT_MODEL is pure staging):
     *  1. Write REG_CT_MODEL = code  (stages the preset; no channel binds yet).
     *  2. Write REG_COMMAND = CMD_SET_CT_MODEL_CH<channel>  — binds the staged
     *     code to THIS channel only (5 ms settle for in-RAM preset lookup).
     *  3. CMD_SAVE_USER_CONFIG (700 ms) to persist the model (user_config).
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
    return saveUserConfig() && sctAfterModelWrite();  /* CT_MODEL is user_config — SAVE_GAINS is dev-gated (silent no-op in production) */
}

/* ============================================================================
 * SCT-013 rating correction (BASIC modules without CAP_SCT_RATING_SELF, i.e.
 * fw <= 1.4.19) — libs/spec/SCT_RATING_SWITCH.md
 *
 * Every BASIC SCT batch is calibrated on the SCT-013-030 (model 3). On these
 * images a model switch is handled like this: in the boot the model is written
 * the module applies the raw preset gain (unit trim lost, seed noise floor);
 * after any restart it applies the factory gain G_cal again while still
 * showing model X. Gain / noise floor are readable in user mode (writes are
 * develop-gated), so the library corrects relative to the gain the module
 * actually applies: s = preset(X) × c(X) × k / G_a with k = G_a / preset(3).
 * ============================================================================ */

namespace {
constexpr uint8_t kSctCalModel         = 3;        /* every batch is calibrated on the -030 */
constexpr float   kSctPresetCal        = 0.6349f;  /* preset gain of the calibration model */
constexpr uint16_t kCapSctRatingSelf   = (1u << 12); /* CAPABILITY bit 12 CAP_SCT_RATING_SELF (fw 1.4.20+): the module corrects itself.
                                                     * Local until RbAmpRegistersV2.h is regenerated with the bit. */
constexpr uint8_t kSctChfieldNf        = 4;        /* channel window field: noise floor (u16) */
constexpr uint8_t kSctChfieldGain      = 5;        /* channel window field: gain (float) */
constexpr float   kSctGainMaxAbs       = 100.0f;   /* plausibility bound for a gain read */
constexpr float   kSctTrimTolerance    = 0.05f;    /* |G_a / preset(3) - 1| beyond this: unexpected factory gain */

struct SctPreset { uint8_t model; float gain; float corr; };
/* Firmware preset gains (op-amp revision) and the rating correction c(X)
 * from the spec (SCT_RATING_SWITCH.md): only the -100 preset carries one. */
const SctPreset kSctPresets[] = {
    { 1, 0.1058f, 1.0f   },   /* -005 */
    { 2, 0.2116f, 1.0f   },   /* -010 */
    { 3, 0.6349f, 1.0f   },   /* -030 (calibration model) */
    { 4, 1.0582f, 1.0f   },   /* -050 */
    { 5, 2.1622f, 0.962f },   /* -100 */
    { 6, 0.4233f, 1.0f   },   /* -020 */
    { 7, 1.2698f, 1.0f   },   /* -060 */
};
const SctPreset* sctPreset(uint8_t model) {
    for (const SctPreset& p : kSctPresets) if (p.model == model) return &p;
    return nullptr;
}
const char* sctStatusName(RbAmpSctRatingStatus st) {
    switch (st) {
        case RbAmpSctRatingStatus::Native:         return "native";
        case RbAmpSctRatingStatus::Corrected:      return "corrected";
        case RbAmpSctRatingStatus::Uncalibrated:   return "uncalibrated";
        case RbAmpSctRatingStatus::UnexpectedGain: return "unexpected gain";
        case RbAmpSctRatingStatus::Unknown:        return "unknown";
        default:                                   return "n/a";
    }
}
} /* namespace */

float RbAmp::sctPresetGain(uint8_t model) noexcept {
    const SctPreset* p = sctPreset(model);
    return p ? p->gain : 0.0f;
}

void RbAmp::sctRatingClear() noexcept {
    for (uint8_t ch = 0; ch < RBAMP_MAX_CHANNELS; ++ch) sct_[ch] = RbAmpSctRating();
}

float RbAmp::sctMaxScale() const noexcept {
    float s = 1.0f;
    for (uint8_t ch = 0; ch < channels_ && ch < RBAMP_MAX_CHANNELS; ++ch) {
        if (sct_[ch].scale > s) s = sct_[ch].scale;
    }
    return s;
}

/* BASIC v1.2+ without CAP_SCT_RATING_SELF (the module corrects itself from
 * fw 1.4.20, announced by CAPABILITY bit 12 — never by the VERSION byte),
 * sensor class SCT-013: the only case the library corrects. CAPABILITY is
 * the copy cached by begin(). */
bool RbAmp::sctCorrectionApplies(bool& applies) noexcept {
    applies = false;
    if (std_) return true;
    uint8_t fw = 0;
    if (!readU8(REG_VERSION, fw)) { setError(RB_ERR_IO); return false; }
    if (fw < 0x03) return true;                            /* no sensor class / model mirrors before v1.2 */
    if (capability_ & kCapSctRatingSelf) return true;     /* the module applies the corrected gain itself */
    uint8_t cls = 0;
    if (!readSensorClass(cls)) return false;   /* error already set */
    applies = (cls == static_cast<uint8_t>(RbAmpSensorClass::Sct013));
    return true;
}

bool RbAmp::sctReadChannel(uint8_t ch, uint8_t& model, float& gain, uint16_t& nf) noexcept {
    if (!readCTModelCh(ch, model)) return false;
    if (ch < 3) {
        if (!readFloatLE(static_cast<uint8_t>(REG_V03_I0_GAIN + ch * 4), gain, kSctGainMaxAbs)) { setError(RB_ERR_IO); return false; }
        if (!readU16LE(static_cast<uint8_t>(REG_V03_I0_NOISE_FLOOR_L + ch * 2), nf))           { setError(RB_ERR_IO); return false; }
        return true;
    }
    gain = readChannelWindow(ch, kSctChfieldGain, kSctGainMaxAbs);
    if (isnan(gain)) return false;   /* error already set */
    const uint8_t sel = static_cast<uint8_t>((kSctChfieldNf << 4) | (ch & 0x0F));
    if (!writeReg(v2::REG_CHANNEL_SELECT, sel) || !readU16LE(v2::REG_CHANNEL_DATA, nf)) { setError(RB_ERR_IO); return false; }
    return true;
}

/* CMD_RESET, wait for the module to answer again with the same UID, then
 * restart the period path (the primer latch anchors the master clock). */
bool RbAmp::sctRestartModule() noexcept {
    uint8_t uid[12];
    const bool have_uid = readUid(uid);
    if (!writeCmd(CMD_RESET)) { setError(RB_ERR_IO); return false; }
    delay(SETTLE_MS_RESET);
    const uint32_t start = millis();
    bool back = false;
    while ((millis() - start) < 1500u) {
        if (probe()) { back = true; break; }
        delay(50);
    }
    if (!back) {
        if (log_) log_->println(F("[rbamp] SCT: module did not come back after CMD_RESET"));
        setError(RB_ERR_TIMEOUT);
        return false;
    }
    if (have_uid) {
        uint8_t now[12];
        if (!readUid(now)) return false;
        if (memcmp(uid, now, sizeof uid) != 0) {
            if (log_) log_->println(F("[rbamp] SCT: a different module answered after CMD_RESET"));
            setError(RB_ERR_VERSION);
            return false;
        }
    }
    return basicPrimePeriod();
}

bool RbAmp::sctAfterModelWrite() noexcept {
    bool applies = false;
    if (!sctCorrectionApplies(applies)) return false;
    if (!applies) { setError(RB_OK); return true; }
    /* The model just written made the module apply the raw preset: restart it
     * so it applies the calibrated gain (flash state), then re-read. */
    if (!sctRestartModule()) return false;
    bool reset_issued = true;
    return sctRatingCompute(reset_issued);
}

bool RbAmp::sctRatingCompute(bool& reset_issued) noexcept {
    sctRatingClear();
    bool applies = false;
    if (!sctCorrectionApplies(applies)) {
        for (uint8_t ch = 0; ch < RBAMP_MAX_CHANNELS; ++ch) sct_[ch].status = RbAmpSctRatingStatus::Unknown;
        return false;   /* error already set */
    }
    if (!applies) { setError(RB_OK); return true; }

    /* Read model / gain / noise floor per channel. A channel that shows a
     * model other than the calibration one AND the raw preset gain was
     * switched in this boot (the unit trim is unknown): restart the module
     * once so it applies the calibrated gain, then read again. */
    for (;;) {
        bool need_reset = false, all_read = true;
        for (uint8_t ch = 0; ch < channels_ && ch < RBAMP_MAX_CHANNELS; ++ch) {
            RbAmpSctRating& r = sct_[ch];
            if (!sctReadChannel(ch, r.model, r.applied_gain, r.noise_floor)) {
                r = RbAmpSctRating();
                r.status = RbAmpSctRatingStatus::Unknown;
                all_read = false;
                continue;
            }
            const SctPreset* p = sctPreset(r.model);
            if (p && r.model != kSctCalModel && r.applied_gain == p->gain) need_reset = true;
        }
        if (!all_read) {
            if (log_) log_->println(F("[rbamp] SCT: channel model / gain not readable — no correction applied"));
            setError(RB_ERR_IO);
            return false;
        }
        if (need_reset && !reset_issued) {
            reset_issued = true;
            if (log_) log_->println(F("[rbamp] SCT: a channel applies the raw preset gain (model switched in this boot) — restarting the module to load the calibrated gain"));
            if (!sctRestartModule()) return false;
            continue;
        }
        break;
    }

    for (uint8_t ch = 0; ch < channels_ && ch < RBAMP_MAX_CHANNELS; ++ch) {
        RbAmpSctRating& r = sct_[ch];
        const SctPreset* p = sctPreset(r.model);
        r.scale = 1.0f;
        if (!p) {
            r.status = RbAmpSctRatingStatus::NotApplicable;      /* no model / not an SCT-013 code */
        } else if (r.model == kSctCalModel) {
            r.status = RbAmpSctRatingStatus::Native;
        } else if (r.applied_gain == p->gain) {
            /* Still the raw preset after a restart: no unit trim on this
             * module (k = 1) — only the rating correction c(X) is known. */
            r.status = RbAmpSctRatingStatus::Uncalibrated;
            r.scale  = p->corr;
        } else {
            const float k = r.applied_gain / kSctPresetCal;
            if (fabsf(k - 1.0f) > kSctTrimTolerance) {
                r.status = RbAmpSctRatingStatus::UnexpectedGain;  /* do not guess */
            } else {
                r.status = RbAmpSctRatingStatus::Corrected;
                r.scale = p->gain * p->corr * k / r.applied_gain;
            }
        }
        if (log_ && r.status != RbAmpSctRatingStatus::NotApplicable) {
            log_->print(F("[rbamp] SCT ch")); log_->print(ch);
            log_->print(F(": model ")); log_->print(r.model);
            log_->print(F(", applied gain ")); log_->print(r.applied_gain, 4);
            log_->print(F(", noise floor ")); log_->print(r.noise_floor);
            log_->print(F(" -> scale x")); log_->print(r.scale, 4);
            log_->print(F(" (")); log_->print(sctStatusName(r.status)); log_->println(F(")"));
            if (r.status == RbAmpSctRatingStatus::Uncalibrated)
                log_->println(F("[rbamp] SCT: warning — module applies the raw preset gain after a restart (no unit trim); nominal correction only"));
            if (r.status == RbAmpSctRatingStatus::UnexpectedGain)
                log_->println(F("[rbamp] SCT: warning — unexpected factory gain (more than 5 % from the calibration preset); readings left uncorrected"));
        }
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::sctRatingRefresh() noexcept {
    bool reset_issued = false;
    return sctRatingCompute(reset_issued);
}

bool RbAmp::sctRating(uint8_t channel, RbAmpSctRating& out) noexcept {
    if (channel >= channels_ || channel >= RBAMP_MAX_CHANNELS) {
        setError(RB_ERR_PARAM);
        return false;
    }
    out = sct_[channel];
    setError(RB_OK);
    return true;
}

bool RbAmp::readCTModelCh(uint8_t channel, uint8_t& out) noexcept {
    if (channel >= channels_) {
        setError(RB_ERR_PARAM);
        return false;
    }
    if (std_) {
        /* STD: the per-channel map entry (CHMAP_MODEL) is the applied reference. */
        if (!stdReadU8(rbamp_std::CHMAP_MODEL(channel), out)) {
            return false;
        }
        setError(RB_OK);
        return true;
    }
    if (channel >= 3) {
        /* Senior channels: the applied model lives in the window (field 15);
         * the flat mirrors 0x51-0x53 only cover ch0-2. Re-select then read the
         * low byte of the u32 window (model code). */
        const uint8_t sel = static_cast<uint8_t>((v2::CHFIELD_CT_MODEL << 4) | (channel & 0x0F));
        if (!writeReg(v2::REG_CHANNEL_SELECT, sel)) {
            setError(RB_ERR_IO);
            return false;
        }
        if (!readU8AB(v2::REG_CHANNEL_DATA, out)) {
            if (lastError() == RB_OK) setError(RB_ERR_IO);
            return false;
        }
        setError(RB_OK);
        return true;
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
    if (n > channels_) n = channels_;
    /* 0. A factory-paired class takes no model binds through this path (the
     *    service path is setCTModelChForce): refuse BEFORE the class write so
     *    nothing is half-applied. */
    if (sensorClassPaired(static_cast<uint8_t>(cls))) {
        for (uint8_t ch = 0; ch < n; ++ch) {
            if (models[ch] != 0) {
                if (log_) log_->println(F("[rbamp] configureChannels refused: class 2/3 modules are calibrated with their fitted sensor"));
                setError(RB_ERR_MODE);
                return false;
            }
        }
    }
    /* 1. Apply sensor class (one SAVE inside setSensorClass; also resets the
     *    device-side CT_MODEL so channel binds start clean). An unchanged
     *    class is a no-op there (read-before-write), a paired module refuses. */
    if (!setSensorClass(cls)) {
        return false;  /* error already set */
    }

    if (std_) {
        /* STD: per-channel reference entries (no gain preset), each readback-
         * verified, then ONE terminal SAVE_CONFIG. */
        bool any = false;
        for (uint8_t ch = 0; ch < n; ++ch) {
            if (models[ch] == 0) continue;
            if (!ctModelValid(cls, models[ch])) {  /* known-code fast-fail (registry) */
                setError(RB_ERR_PARAM);
                return false;
            }
            bool skip = false;
            if (!ctModelGuard(ch, models[ch], false, skip)) {
                return false;  /* error already set */
            }
            if (skip) continue;  /* already holds this model — no write */
            if (!stdApplyCtModel(ch, models[ch])) {
                if (log_) {
                    log_->print(F("[rbamp] configureChannels: ch"));
                    log_->print(ch);
                    log_->println(F(" did not apply"));
                }
                return false;  /* error already set */
            }
            any = true;
        }
        if (any) return saveUserConfig();
        setError(RB_OK);
        return true;
    }

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
        if (!ctModelValid(cls, code)) {  /* known-code fast-fail (registry) */
            setError(RB_ERR_PARAM);
            return false;
        }
        if (fw < 0x03) {
            setError(RB_ERR_VERSION);
            return false;
        }
        /* Same read-before-write guard as the single setter: a model the
         * channel already holds is not re-applied (that would replace the
         * unit's calibration with the generic preset). */
        bool skip = false;
        if (!ctModelGuard(ch, code, false, skip)) {
            return false;  /* error already set */
        }
        if (skip) continue;

        if (ch >= 3) {
            /* Senior channel: bind via the window (field 15) — no CMD opcode
             * exists beyond ch2. Needs v1.4.18+ (fw ver 0x0A). Applies
             * immediately; the single terminal SAVE below persists it. */
            if (fw < 0x0A) {
                setError(RB_ERR_VERSION);
                return false;
            }
            if (!writeChannelModel(ch, code)) {
                return false;  /* RB_ERR_IO already set */
            }
            delay(5);
            /* Window bind is immediate + module-authoritative: a mismatch means
             * the module rejected the (class,model) — re-writing would not help. */
            if (verifyCtBind(ch, code) == RB_ERR_PARAM) {
                setError(RB_ERR_PARAM);
                if (log_) {
                    log_->print(F("[rbamp] configureChannels: ch"));
                    log_->print(ch);
                    log_->println(F(" rejected (unknown model)"));
                }
                return false;
            }
            any_bound = true;
            continue;
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
        return saveUserConfig() && sctAfterModelWrite();  /* CT_MODEL is user_config — SAVE_GAINS is dev-gated (silent no-op in production) */
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::saveGains() noexcept {
    if (std_) {
        /* STD: CMD_SAVE_CAL persists the CAL block (gains, noise floors, THD
         * floors); completion = command state idle, then the verdict. */
        return stdSaveSync(rbamp_std::CMD_SAVE_CAL, rbamp_std::SETTLE_MS_SAVE_CAL);
    }
    if (!writeCmd(CMD_SAVE_GAINS)) {
        setError(RB_ERR_IO);
        return false;
    }
    delay(SETTLE_MS_SAVE_GAINS);  /* 700 ms — flash erase + write */
    setError(RB_OK);
    return true;
}

bool RbAmp::saveUserConfig() noexcept {
    if (std_) {
        /* STD: CMD_SAVE_CONFIG persists the CFG block (class / ct_model / tier /
         * i2c_addr; RMW, calibration preserved). An invalid field is reported
         * as ERR_PARAM once the module is idle again (RB_ERR_PARAM). */
        return stdSaveSync(rbamp_std::CMD_SAVE_CONFIG, rbamp_std::SETTLE_MS_SAVE_CONFIG);
    }
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
     * (truth-doc §6.1) — NOT factory-gated. No wire I/O here; just arm and
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

    if (std_) {
        /* STD: CFG_I2C_ADDR → CMD_SAVE_CONFIG (map 0x34+ rejects an out-of-range
         * address with ERR_PARAM, surfaced by saveUserConfig(); the module keeps
         * its address, so no rebind) → CMD_RESET (address applies on reset). */
        if (!stdWriteU8(rbamp_std::REG_CFG_I2C_ADDR, pending_addr_)) {
            if (log_) log_->println(F("[rbamp] address change: CFG_I2C_ADDR write failed; device unchanged"));
            return false;
        }
        if (!saveUserConfig()) {   /* waits for the save to finish; verdict 0 = persisted */
            if (log_) log_->println(F("[rbamp] address change: SAVE_CONFIG failed; device unchanged"));
            return false;
        }
        if (!stdCmd(rbamp_std::CMD_RESET)) {
            if (log_) log_->println(F("[rbamp] address change: RESET write failed — new address applies on next power cycle"));
            return false;
        }
        delay(rbamp_std::SETTLE_MS_RESET);
        /* Switch only once the module answers at the new address. */
        if (!stdWaitReappear(pending_addr_, 3000)) {
            if (stdWaitReappear(addr_, 500)) {
                if (log_) log_->println(F("[rbamp] address change: module came back on its OLD address — not switched"));
                setError(RB_ERR_NOT_READY);
                return false;
            }
            if (log_) log_->println(F("[rbamp] address change: module not answering yet — switching to the new address"));
        }
        addr_ = pending_addr_;
        /* The module reset behind the address change cleared its RAM arm mask —
         * restart the period path at the new address (E15). */
        return stdPrimePeriod();
    }

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
    if (std_) {
        /* Never while a command (a save, an OTA step) is still running. */
        if (!stdWaitIdleImpl(2000, rbamp_std::CMD_RESET)) return false;
        if (!stdCmd(rbamp_std::CMD_RESET)) {
            return false;
        }
        delay(rbamp_std::SETTLE_MS_RESET);
        /* The module is off the bus while it reboots (longer than the map
         * settle on the counter family): wait for its identity block to
         * answer again; stdPrimePeriod() reports it if it never does. */
        stdWaitReappear(addr_, 3000);
        /* The reset cleared the module's RAM arm mask — restart the period
         * path so latch energy keeps counting (E15). */
        return stdPrimePeriod();
    }
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
    if (std_) {
        if (!stdReadU8(rbamp_std::REG_VARIANT, v)) {
            return RbAmpVariant::Unknown;
        }
        std_variant_ = v;
        variant_ = stdVariantMap(v);
        setError(RB_OK);
        return variant_;
    }
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
    if (std_) {
        /* The STD map has no product-id register; the identity magic 'ST'
         * already proved an rbAmp sensor. Report the sensor family so a
         * product-matched fleet scan adopts STD modules too. */
        setError(RB_OK);
        return 0x01;
    }
    if (!readU8(v2::REG_PRODUCT_ID, v)) {
        setError(RB_ERR_IO);
        return 0;
    }
    setError(RB_OK);
    return v;
}

bool RbAmp::readUid(uint8_t out[12]) noexcept {
    if (std_) {
        if (!stdRead(rbamp_std::REG_UID, out, rbamp_std::REG_UID_SIZE)) {
            return false;
        }
        setError(RB_OK);
        return true;
    }
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
    if (std_) {
        if (!stdReadU8(rbamp_std::REG_ERROR, v)) {
            return 0xFF;
        }
        setError(RB_OK);
        return v;
    }
    if (!readU8(REG_ERROR, v)) {
        setError(RB_ERR_IO);
        return 0xFF;
    }
    setError(RB_OK);
    return v;
}

/* STD event block exists from map 0x36 (image 0x75 implements it; earlier
 * 0x36 images read the registers as 0, which is harmless). Same bit layout
 * as BASIC v1.3, so one API serves both lines. */
bool RbAmp::readEventFlags(uint8_t& out) noexcept {
    if (std_) {
        if (!stdGate36()) return false;
        if (!stdReadU8(rbamp_std::REG_EVENT_FLAGS, out)) return false;
        setError(RB_OK);
        return true;
    }
    if (!readU8(v2::REG_EVENT_FLAGS, out)) {
        setError(RB_ERR_IO);
        return false;
    }
    if ((out & v2::EVENT_RESET_OCCURRED) != 0 && !sct_reset_seen_) {
        /* The module restarted: it now applies the flash state (model as
         * saved, calibrated gain) — re-read the rating correction once. */
        sct_reset_seen_ = true;
        bool reset_issued = false;
        (void)sctRatingCompute(reset_issued);
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::clearEventFlags(uint8_t mask) noexcept {
    /* Write-1-to-clear: writing the mask back clears those sticky bits. */
    if (std_) {
        if (!stdGate36()) return false;
        if (!stdWriteU8(rbamp_std::REG_EVENT_FLAGS, mask)) return false;
        setError(RB_OK);
        return true;
    }
    if (!writeReg(v2::REG_EVENT_FLAGS, mask)) {
        setError(RB_ERR_IO);
        return false;
    }
    if (mask & v2::EVENT_RESET_OCCURRED) sct_reset_seen_ = false;   /* the next restart is new */
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
    if (std_) {
        /* STD: the persisted CFG_I2C_ADDR (applies on reset). */
        if (!stdReadU8(rbamp_std::REG_CFG_I2C_ADDR, out)) return false;
        setError(RB_OK);
        return true;
    }
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

bool RbAmp::readCommitSeq(uint8_t& out) noexcept {
    /* Digest SEQ byte at REG_DIGEST+1 (0x71): increments once per published
     * measurement window (100 ms on v1.2+, 200 ms before), wraps 255->0.
     * Change detection only — there is no CRC. */
    if (!readU8(static_cast<uint8_t>(v2::REG_DIGEST + 1), out)) {
        setError(RB_ERR_IO);
        return false;
    }
    setError(RB_OK);
    return true;
}

const char* RbAmp::errorString(int8_t code) noexcept {
    switch (code) {
        case RB_OK:                 return "OK";
        case RB_ERR_IO:             return "I2C transport failure";
        case RB_ERR_NACK:           return "NACK — device absent or busy";
        case RB_ERR_TIMEOUT:        return "Bus timeout";
        case RB_ERR_NOT_READY:      return "Device not ready";
        case RB_ERR_STALE:          return "Period snapshot stale";
        case RB_ERR_PARAM:          return "Bad parameter";
        case RB_ERR_MODE:           return "Not allowed in the module's current mode (factory mode / service path required)";
        case RB_ERR_CHECKSUM:       return "Value check failed — checksum / signature / range";
        case RB_ERR_VERSION:        return "Unsupported firmware version";
        case RB_ERR_NOT_IMPLEMENTED: return "Not implemented on this firmware";
        case RB_ERR_NON_PHYSICAL:    return "Non-physical value (NaN/Inf/out-of-bounds)";
        default:                return "Unknown error";
    }
}

void RbAmp::setError(int8_t err) noexcept {
    /* An 8-bit primitive refused on an STD instance (gate_refused_): the
     * caller's follow-up transport-class verdict keeps the real reason. */
    if (gate_refused_) {
        gate_refused_ = false;
        if (err != RB_OK) err = RB_ERR_NOT_IMPLEMENTED;
    }
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
    if (std_) {   /* BASIC-only path on an STD module — refuse, never misdeliver */
        setError(RB_ERR_NOT_IMPLEMENTED);
        gate_refused_ = true;
        return false;
    }
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
    if (std_) {   /* BASIC-only path on an STD module — refuse, never misdeliver */
        setError(RB_ERR_NOT_IMPLEMENTED);
        gate_refused_ = true;
        return false;
    }
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

/* ============================================================================
 * STD line transport — 16-bit big-endian register pointer, auto-increment
 *
 * A write is [ptr_hi, ptr_lo, data...]; a read is [ptr_hi, ptr_lo] then a
 * repeated-start read of n bytes. Multi-byte fields are little-endian and the
 * module publishes them byte-wise with no multi-register atomic snapshot, so
 * a burst is no less coherent than N single-byte reads and costs one address
 * phase instead of N. Same NACK-retry discipline as the 8-bit primitives, for
 * writes as well as reads (L6).
 * ============================================================================ */

/* Largest burst per transaction. The module serves every read from a 128-byte
 * coherent snapshot taken when the pointer is set, so a block read in ONE
 * transaction of at most 128 bytes cannot tear (an i64 counter arrives whole).
 * The chunk follows the core's Wire buffer: ESP32 (128) reads a block whole;
 * the classic 32-byte AVR buffer splits it (2 pointer bytes + 30 data on a
 * write, 32 on a read) and stdReadCoherent() re-reads a split block until two
 * passes agree. RBAMP_STD_BURST_MAX overrides both limits. */
#if defined(RBAMP_STD_BURST_MAX)
#  define RBAMP_STD_READ_MAX   (RBAMP_STD_BURST_MAX)
#  define RBAMP_STD_WRITE_MAX  (RBAMP_STD_BURST_MAX)
#else
#  if defined(I2C_BUFFER_LENGTH)
#    define RBAMP_STD_WIRE_BUF  (I2C_BUFFER_LENGTH)
#  elif defined(BUFFER_LENGTH)
#    define RBAMP_STD_WIRE_BUF  (BUFFER_LENGTH)
#  else
#    define RBAMP_STD_WIRE_BUF  32
#  endif
#  define RBAMP_STD_READ_MAX   ((RBAMP_STD_WIRE_BUF) > 128 ? 128u : static_cast<unsigned>(RBAMP_STD_WIRE_BUF))
#  define RBAMP_STD_WRITE_MAX  (RBAMP_STD_READ_MAX - 2u)   /* 2 pointer bytes share the buffer */
#endif

/* Command timing (firmware contract): at least 1 s for any command, saves
 * included; BUSY is polled every 10 ms. */
/* Image versions (REG_FW_VER) from which a module publishes a feature that
 * has no capability bit: voltage angles + phase sequence (0x7A); starting
 * current and reactive µvarh counters (0x7B). */
static constexpr uint32_t kStdFwAngles  = 0x0001007Au;
static constexpr uint32_t kStdFwStartUa = 0x0001007Bu;
static constexpr uint32_t kStdCmdMinTimeoutMs = 1000;
static constexpr uint32_t kStdSaveTimeoutMs   = 2000;
static constexpr uint32_t kStdCmdFirstPollMs  = 20;
static constexpr uint32_t kStdOtaPageTimeoutMs    = 5000;    /* one page slot is 250 ms; flash stalls on top */
static constexpr uint32_t kStdOtaConfirmTimeoutMs = 20000;   /* trial image confirms after ~8 s of metering */

namespace {
inline uint16_t leU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline int16_t leI16(const uint8_t* p) { return static_cast<int16_t>(leU16(p)); }
inline uint32_t leU32(const uint8_t* p) {
    return  static_cast<uint32_t>(p[0])        | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline int32_t leI32(const uint8_t* p) { return static_cast<int32_t>(leU32(p)); }
inline int64_t leI64(const uint8_t* p) {
    uint64_t v = 0;
    for (uint8_t i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return static_cast<int64_t>(v);
}
} /* namespace */

bool RbAmp::stdTxn(const uint8_t* wbuf, uint8_t wlen, uint8_t* rbuf, uint8_t rlen) noexcept {
    return stdTxnN(wbuf, wlen, rbuf, rlen, RBAMP_NACK_RETRY_ATTEMPTS);
}

bool RbAmp::stdTxnN(const uint8_t* wbuf, uint8_t wlen, uint8_t* rbuf, uint8_t rlen,
                    uint8_t attempts) noexcept {
    const uint8_t kAttempts = attempts ? attempts : 1u;
    for (uint8_t attempt = 0; attempt < kAttempts; ++attempt) {
        bus_.beginTransmission(addr_);
        bus_.write(wbuf, wlen);
        if (rlen == 0) {
            if (bus_.endTransmission() == 0) {
                return true;
            }
        } else if (bus_.endTransmission(false) == 0) {   /* repeated start */
            if (bus_.requestFrom(addr_, rlen) >= rlen) {
                uint8_t i = 0;
                for (; i < rlen; ++i) {
                    const int b = bus_.read();
                    if (b < 0) break;
                    rbuf[i] = static_cast<uint8_t>(b);
                }
                if (i == rlen) {
                    return true;
                }
            }
        }
        if (attempt + 1 < kAttempts) {
            delay(RBAMP_NACK_RETRY_GAP_MS);
        }
    }
    retry_exhaustion_count_++;
    return false;
}

bool RbAmp::stdRead(uint16_t reg, uint8_t* buf, size_t n) noexcept {
    while (n) {
        const uint8_t k = (n > RBAMP_STD_READ_MAX) ? static_cast<uint8_t>(RBAMP_STD_READ_MAX)
                                                   : static_cast<uint8_t>(n);
        const uint8_t ptr[2] = { static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg) };
        if (!stdTxn(ptr, 2, buf, k)) {
            setError(RB_ERR_NACK);
            return false;
        }
        reg = static_cast<uint16_t>(reg + k);
        buf += k;
        n   -= k;
    }
    return true;
}

/* A block that must be read as one coherent snapshot (i64 counters, a channel
 * block, a history record). Within one transaction the module guarantees it;
 * when the Wire buffer forces a split, two passes must agree (a publish between
 * the chunks changes the second pass). */
bool RbAmp::stdReadCoherent(uint16_t reg, uint8_t* buf, size_t n) noexcept {
    if (!stdRead(reg, buf, n)) return false;
    if (n <= RBAMP_STD_READ_MAX || n > 128u) return true;
    uint8_t again[128];
    for (uint8_t pass = 0; pass < 3; ++pass) {
        if (!stdRead(reg, again, n)) return false;
        if (memcmp(again, buf, n) == 0) return true;
        memcpy(buf, again, n);
    }
    return true;   /* three disagreeing passes: the last one is as good as any */
}

bool RbAmp::stdWrite(uint16_t reg, const uint8_t* data, size_t n) noexcept {
    uint8_t buf[2 + RBAMP_STD_WRITE_MAX];
    while (n) {
        const uint8_t k = (n > RBAMP_STD_WRITE_MAX) ? static_cast<uint8_t>(RBAMP_STD_WRITE_MAX)
                                                    : static_cast<uint8_t>(n);
        buf[0] = static_cast<uint8_t>(reg >> 8);
        buf[1] = static_cast<uint8_t>(reg);
        memcpy(&buf[2], data, k);
        if (!stdTxn(buf, static_cast<uint8_t>(2 + k), nullptr, 0)) {
            setError(RB_ERR_NACK);
            return false;
        }
        reg  = static_cast<uint16_t>(reg + k);
        data += k;
        n    -= k;
    }
    return true;
}

bool RbAmp::stdReadU8(uint16_t reg, uint8_t& out) noexcept {
    return stdRead(reg, &out, 1);
}

bool RbAmp::stdReadU16(uint16_t reg, uint16_t& out) noexcept {
    uint8_t b[2];
    if (!stdRead(reg, b, 2)) return false;
    out = leU16(b);
    return true;
}

bool RbAmp::stdReadU32(uint16_t reg, uint32_t& out) noexcept {
    uint8_t b[4];
    if (!stdRead(reg, b, 4)) return false;
    out = leU32(b);
    return true;
}

bool RbAmp::stdWriteU8(uint16_t reg, uint8_t val) noexcept {
    return stdWrite(reg, &val, 1);
}

/* COMMAND write. A command the module accepted must never be written again
 * (a second copy is dropped as OVERRUN, or runs twice once the first is done),
 * so a NACKed write is not resent blindly: on map 0x35+ the command-state
 * register tells whether the module took it — BUSY set, or the completed-
 * command counter moved — and only a write the module provably did not see is
 * tried again. Before map 0x35 there is nothing to ask; the plain retry applies. */
bool RbAmp::stdCmd(uint8_t opcode) noexcept {
    if (std_version_ < 0x35) return stdWriteU8(rbamp_std::REG_COMMAND, opcode);
    uint16_t seq0 = 0;
    const bool have_seq = stdReadU16(rbamp_std::REG_CMD_SEQ, seq0);
    const uint8_t frame[3] = { static_cast<uint8_t>(rbamp_std::REG_COMMAND >> 8),
                               static_cast<uint8_t>(rbamp_std::REG_COMMAND), opcode };
    for (uint8_t attempt = 0; attempt < RBAMP_NACK_RETRY_ATTEMPTS; ++attempt) {
        if (stdTxnN(frame, sizeof frame, nullptr, 0, 1)) return true;
        delay(RBAMP_NACK_RETRY_GAP_MS);
        uint8_t  st   = 0;
        uint16_t seq1 = 0;
        if (stdReadU8(rbamp_std::REG_CMD_STATE, st) && (st & 0x01u)) return true;             /* taken, running */
        if (have_seq && stdReadU16(rbamp_std::REG_CMD_SEQ, seq1) && seq1 != seq0) return true; /* taken, finished */
    }
    setError(RB_ERR_NACK);
    return false;
}

/* Synchronous command verdict (map 0x34+): the module clears ERROR to OK when
 * it dispatches a command, so reading ERROR after the command's settle is the
 * verdict for THAT command. Commands run in the module's main loop, not in the
 * I2C interrupt, so this must be read AFTER the settle, never straight after
 * the opcode write. On maps < 0x34 the register is sticky and says nothing
 * about this command. */
bool RbAmp::stdCheckCmdError() noexcept {
    if (std_version_ < 0x34) return true;
    uint8_t e = 0;
    if (!stdReadU8(rbamp_std::REG_ERROR, e)) return false;
    switch (e) {
        case rbamp_std::DEV_ERR_OK:
            return true;
        case rbamp_std::DEV_ERR_PARAM:
            if (log_) log_->println(F("[rbamp] module rejected command/config: ERR_PARAM"));
            setError(RB_ERR_PARAM);
            return false;
        case rbamp_std::DEV_ERR_UNKNOWN_CMD:
            if (log_) log_->println(F("[rbamp] module does not know this command"));
            setError(RB_ERR_NOT_IMPLEMENTED);
            return false;
        case rbamp_std::DEV_ERR_VALUE_CRC:
            setError(RB_ERR_CHECKSUM);       /* value check failed (payload CRC, epoch range, empty ring) */
            return false;
        case rbamp_std::DEV_ERR_PENDING:
            setError(RB_ERR_TIMEOUT);        /* still running — the caller re-reads after the module is idle */
            return false;
        case rbamp_std::DEV_ERR_NVM_RATE_LIMIT:
        case rbamp_std::DEV_ERR_OP_RETRY:
            if (log_) { log_->print(F("[rbamp] module busy / rate-limited: ERROR=0x")); log_->println(e, HEX); }
            setError(RB_ERR_NOT_READY);
            return false;
        case rbamp_std::DEV_ERR_NO_KEY:
        case rbamp_std::DEV_ERR_KEY_GATE:
        case rbamp_std::DEV_ERR_LEGAL_GATE:
            if (log_) { log_->print(F("[rbamp] command not allowed (gated): ERROR=0x")); log_->println(e, HEX); }
            setError(RB_ERR_MODE);
            return false;
        case rbamp_std::DEV_ERR_NVM_IO:
            if (log_) log_->println(F("[rbamp] module storage I/O error"));
            setError(RB_ERR_IO);
            return false;
        default:
            if (log_) { log_->print(F("[rbamp] module reports ERROR=0x")); log_->println(e, HEX); }
            setError(RB_ERR_NOT_READY);
            return false;
    }
}

void RbAmp::stdSane(float& v, float max_abs, uint8_t& mask, uint8_t bit) noexcept {
    if (!isfinite(v) || fabsf(v) > max_abs) {
        sanity_reject_count_++;
        v = NAN;
        mask |= bit;
    }
}

bool RbAmp::stdReadScaled(uint16_t addr, bool is_i16, float scale,
                          float max_abs, float& out) noexcept {
    uint8_t b[4];
    if (!stdRead(addr, b, is_i16 ? 2u : 4u)) {
        setError(RB_ERR_IO);
        return false;
    }
    const float v = is_i16 ? static_cast<float>(leI16(b)) * scale
                           : static_cast<float>(leI32(b)) * scale;
    if (!isfinite(v) || fabsf(v) > max_abs) {
        sanity_reject_count_++;
        setError(RB_ERR_NON_PHYSICAL);
        return false;
    }
    out = v;
    return true;
}

/* Size of the CURRENT / TOTALS block we decode: up to and including Q1. */
static constexpr size_t kStdIpBlockLen = rbamp_std::CURRENT_Q1 + 4u;

bool RbAmp::stdReadIpBlock(uint16_t base, RbAmpStdChannel& out) noexcept {
    uint8_t b[kStdIpBlockLen];
    if (!stdReadCoherent(base, b, sizeof b)) {
        setError(RB_ERR_IO);
        return false;
    }
    memset(&out, 0, sizeof out);
    out.i_rms  = static_cast<float>(leI32(b + rbamp_std::CURRENT_I_RMS))  * 1e-6f;
    out.p      = static_cast<float>(leI32(b + rbamp_std::CURRENT_P))      * 1e-3f;
    out.q      = static_cast<float>(leI32(b + rbamp_std::CURRENT_Q))      * 1e-3f;
    out.s      = static_cast<float>(leI32(b + rbamp_std::CURRENT_S))      * 1e-3f;
    out.pf     = static_cast<float>(leI16(b + rbamp_std::CURRENT_PF))     / 1024.0f;
    out.i_peak = static_cast<float>(leI32(b + rbamp_std::CURRENT_I_PEAK)) * 1e-6f;
    out.e_import_mws = leI64(b + rbamp_std::CURRENT_E_POS);
    out.e_export_mws = leI64(b + rbamp_std::CURRENT_E_NEG);
    out.thd_i  = static_cast<float>(leU16(b + rbamp_std::CURRENT_THD_I))  / 100.0f;
    out.pf1    = static_cast<float>(leI16(b + rbamp_std::CURRENT_PF1))    / 1024.0f;
    out.q1     = static_cast<float>(leI32(b + rbamp_std::CURRENT_Q1))     * 1e-3f;
    stdSane(out.i_rms,  150.0f,   out.implausible, RBAMP_FIELD_CURRENT);
    stdSane(out.i_peak, 250.0f,   out.implausible, RBAMP_FIELD_CURRENT);
    stdSane(out.p,      30000.0f, out.implausible, RBAMP_FIELD_POWER);
    stdSane(out.q,      30000.0f, out.implausible, RBAMP_FIELD_POWER);
    stdSane(out.s,      30000.0f, out.implausible, RBAMP_FIELD_POWER);
    stdSane(out.q1,     30000.0f, out.implausible, RBAMP_FIELD_POWER);
    stdSane(out.pf,     1.5f,     out.implausible, RBAMP_FIELD_PF);
    stdSane(out.pf1,    1.5f,     out.implausible, RBAMP_FIELD_PF);
    setError(RB_OK);
    return true;
}

/* STD identity probe — one burst of the identity block 0x0000-0x0009.
 *
 * Issued FIRST in begin(), before any 8-bit transaction, because the lines
 * cannot be told apart from the BASIC side: an STD module takes a lone
 * pointer byte as the pointer HIGH byte and answers from 0xNN00 with a
 * plausible non-zero value, so the BASIC VERSION probe would pass. This probe
 * is benign on a BASIC module: the pointer pair lands as "reg 0x00 <- 0x00",
 * a write to the read-only STATUS byte that the BASIC firmware drops without
 * raising REG_ERROR, and the 10-byte read returns its control block, which can
 * never spell the 'S','T' magic (bench-verified negative control, esp-idf
 * v1.6.0 HIL). */
bool RbAmp::stdDetect(bool& is_std) noexcept {
    uint8_t id[10];
    is_std = false;
    if (!stdRead(rbamp_std::REG_ID, id, sizeof id)) {
        return false;   /* RB_ERR_NACK — device absent / bus dead */
    }
    if (id[0] != 'S' || id[1] != 'T') {
        return true;    /* BASIC line */
    }
    const uint8_t  ver  = id[rbamp_std::REG_VERSION];
    const uint16_t caps = leU16(&id[rbamp_std::REG_CAPABILITIES]);
    if (ver < 0x30 || (caps & rbamp_std::CAP_ADDR16) == 0) {
        if (log_) {
            log_->print(F("[rbamp] STD magic but map version 0x")); log_->print(ver, HEX);
            log_->print(F(" / caps 0x")); log_->print(caps, HEX);
            log_->println(F(" not supported"));
        }
        setError(RB_ERR_VERSION);
        return false;
    }
    if (ver > rbamp_std::MAP_VERSION && log_) {
        log_->print(F("[rbamp] STD map version 0x")); log_->print(ver, HEX);
        log_->print(F(" is newer than this library (0x"));
        log_->print(rbamp_std::MAP_VERSION, HEX); log_->println(F(")"));
    }
    std_         = true;
    std_version_ = ver;
    std_caps_    = caps;
    std_status_  = id[rbamp_std::REG_STATUS];
    n_voltage_   = id[rbamp_std::REG_N_VOLTAGE];
    n_current_   = id[rbamp_std::REG_N_CURRENT];
    std_variant_ = id[rbamp_std::REG_VARIANT];
    is_std = true;
    return true;
}

RbAmpVariant RbAmp::stdVariantMap(uint8_t v) noexcept {
    switch (v) {
        case rbamp_std::VARIANT_UI1:  return RbAmpVariant::UI1;
        case rbamp_std::VARIANT_UI2:  return RbAmpVariant::UI2;
        case rbamp_std::VARIANT_UI3:  return RbAmpVariant::UI3;
        case rbamp_std::VARIANT_UI5:  return RbAmpVariant::UI5;
        case rbamp_std::VARIANT_UI7:  return RbAmpVariant::UI7;
        case rbamp_std::VARIANT_I1:   return RbAmpVariant::I1;
        case rbamp_std::VARIANT_I2:   return RbAmpVariant::I2;
        case rbamp_std::VARIANT_I3:   return RbAmpVariant::I3;
        case rbamp_std::VARIANT_U3I3: return RbAmpVariant::U3I3;
        case rbamp_std::VARIANT_U3I6: return RbAmpVariant::U3I6;
        default:                      return RbAmpVariant::Unknown;
    }
}

/* Finish begin() for an STD module: adopt discovery counts, arm latch bank 0,
 * primer latch. */
bool RbAmp::stdBegin() noexcept {
    /* Channel count is discovery truth on this line. Registers of channels at
     * or beyond N_CURRENT are unspecified on the module, so the constructor
     * hint cannot "over-detect harmlessly" the way it can on BASIC: N_CURRENT
     * wins and a differing hint only earns a log line. */
    uint8_t ch = n_current_;
    if (ch == 0) {
        if (log_) log_->println(F("[rbamp] STD module reports 0 current channels"));
        setError(RB_ERR_VERSION);
        return false;
    }
    if (ch > RBAMP_MAX_CHANNELS) {
        if (log_) {
            log_->print(F("[rbamp] STD module has ")); log_->print(ch);
            log_->print(F(" current channels; this library exposes the first "));
            log_->println(RBAMP_MAX_CHANNELS);
        }
        ch = RBAMP_MAX_CHANNELS;
    }
    if (log_ && channels_ != ch) {
        log_->print(F("[rbamp] constructor hint ")); log_->print(channels_);
        log_->print(F("ch, STD module reports ")); log_->print(ch);
        log_->println(F("ch — using the module's count"));
    }
    channels_        = ch;
    topology_        = static_cast<RbAmpTopology>(ch);  /* may be a count outside the BASIC enum (4/6) */
    has_voltage_hw_  = n_voltage_ > 0;
    variant_         = stdVariantMap(std_variant_);
    capability_      = 0;                               /* BASIC bitmap; STD caps via stdInfo() */
    std_active_bank_   = 0;
    std_ota_bank_size_ = 0;
    if (std_caps_ & rbamp_std::CAP_OTA) {
        (void)stdReadU32(rbamp_std::REG_OTA_ACTIVE, std_active_bank_);
        /* Bank size is device-reported (E10): image + header must fit it, and
         * the header page index derives from it — never hardcoded. */
        uint32_t bs = 0;
        if (stdReadU32(rbamp_std::REG_OTA_BANK_SIZE, bs) && bs != 0 && bs != 0xFFFFFFFFu
                && (bs % rbamp_std::OTA_PAGE_SIZE) == 0 && bs >= 2u * rbamp_std::OTA_PAGE_SIZE) {
            std_ota_bank_size_ = bs;
        }
    }
    /* Period / energy mechanism is a capability, not a line property: the
     * first STD modules had latch banks, the current firmware family keeps
     * persisted counters instead (the latch command is unknown there and the
     * bank region unmapped). Decide once, here, so no latch traffic ever
     * reaches a module without banks (L15). */
    std_period_src_ = (std_caps_ & rbamp_std::CAP_MULTI_LATCH)    ? RbAmpStdPeriodSource::Latch
                    : (std_caps_ & rbamp_std::CAP_ONDEVICE_ENERGY) ? RbAmpStdPeriodSource::Counters
                    : RbAmpStdPeriodSource::None;
    std_fw_ver_ = 0;
    if (std_version_ >= 0x35) {
        uint32_t fv = 0;
        if (stdReadU32(rbamp_std::REG_FW_VER, fv) && fv != 0xFFFFFFFFu) std_fw_ver_ = fv;
    }
    /* Event flags as found at begin (RESET_OCCURRED tells the caller the
     * module rebooted before we met it); nothing is cleared here. */
    if (std_version_ >= 0x36) {
        uint8_t f = 0;
        if (stdReadU8(rbamp_std::REG_EVENT_FLAGS, f) && f != 0xFF) begin_event_flags_ = f;
    }
    if (log_) {
        log_->print(F("[rbamp] STD module: map=0x")); log_->print(std_version_, HEX);
        log_->print(F(" variant=0x")); log_->print(std_variant_, HEX);
        log_->print(F(" V=")); log_->print(n_voltage_);
        log_->print(F(" I=")); log_->print(n_current_);
        log_->print(F(" caps=0x")); log_->print(std_caps_, HEX);
        log_->print(F(" status=0x")); log_->print(std_status_, HEX);
        log_->print(F(" image=0x")); log_->print(std_fw_ver_, HEX);
        log_->print(F(" period="));
        log_->println(std_period_src_ == RbAmpStdPeriodSource::Latch    ? F("latch-banks") :
                      std_period_src_ == RbAmpStdPeriodSource::Counters ? F("counters") : F("none"));
        if (std_period_src_ == RbAmpStdPeriodSource::None) {
            log_->println(F("[rbamp] WARN module has neither latch banks nor on-device energy counters — period / energy calls will fail"));
        }
    }

    return stdPrimePeriod();
}

/* (Re)start the library's period path on bank 0: arm it (read-modify-write so
 * a user's other banks stay armed), issue the same primer latch as BASIC so the
 * next snapshot covers a full period, and anchor the master clock. The arm
 * mask is RAM on the module — ANY module reset (power, watchdog, OTA, a reset
 * command from another master) clears it, and a latch of an unarmed bank
 * yields an empty snapshot forever, so arming in begin() alone is not enough
 * (E15, found by the esphome HIL). */
bool RbAmp::stdPrimePeriod() noexcept {
    switch (std_period_src_) {
    case RbAmpStdPeriodSource::Latch: {
        uint8_t arm = 0;
        if (!stdReadU8(rbamp_std::REG_PERIOD_ARM, arm)) return false;
        if ((arm & 0x01u) == 0) {
            if (!stdWriteU8(rbamp_std::REG_PERIOD_ARM, static_cast<uint8_t>(arm | 0x01u))) return false;
        }
        if (!stdLatchBank(0)) return false;
        prev_latch_ms_    = millis();
        current_latch_ms_ = prev_latch_ms_;
        have_prev_latch_  = true;
        std_have_frame_cnt_ = stdReadU32(rbamp_std::REG_FRAME_CNT, std_frame_cnt_);
        setError(RB_OK);
        return true;
    }
    case RbAmpStdPeriodSource::Counters:
        /* Persisted counters need no re-priming after a reset the library
         * issued: the anchor stays valid across it (the next period just
         * reports module_restarted). Only a fresh instance takes an anchor. */
        if (std_lt_anchored_) { setError(RB_OK); return true; }
        return stdLtAnchor();
    default:
        setError(RB_OK);   /* no period path on this module; each call refuses on its own */
        return true;
    }
}

bool RbAmp::stdRebootedSinceAnchor() noexcept {
    /* FRAME_CNT counts sensor frames since the module booted (~7000/s) and
     * only wraps after days, so a value BEHIND the one cached at the anchor
     * (modular compare) means the module rebooted inside this period. Such a
     * period holds a partial accumulation that the arm check cannot flag when
     * the arm came back by itself (firmware auto-arm, another master). */
    uint32_t cnt = 0;
    if (!stdReadU32(rbamp_std::REG_FRAME_CNT, cnt)) return false;
    const bool rebooted = std_have_frame_cnt_ &&
                          static_cast<int32_t>(cnt - std_frame_cnt_) < 0;
    std_frame_cnt_      = cnt;
    std_have_frame_cnt_ = true;
    return rebooted;
}

bool RbAmp::stdReadAll(RbAmpSnapshot& out) noexcept {
    /* One burst per block: voltage (RMS + peak), then each populated current
     * channel (I_RMS .. I_PEAK), then frequency. Sanity-reject folds to NaN +
     * implausible bit; only a transport failure aborts (same contract as the
     * BASIC readAll). */
    if (n_voltage_ > 0) {
        uint8_t b[rbamp_std::VOLTAGE_U_PEAK + 4u];
        if (!stdRead(rbamp_std::VOLTAGE(0), b, sizeof b)) { setError(RB_ERR_IO); return false; }
        out.voltage      = static_cast<float>(leI32(b + rbamp_std::VOLTAGE_U_RMS))  * 1e-3f;
        out.voltage_peak = static_cast<float>(leI32(b + rbamp_std::VOLTAGE_U_PEAK)) * 1e-3f;
        stdSane(out.voltage,      500.0f, out.implausible, RBAMP_FIELD_VOLTAGE);
        stdSane(out.voltage_peak, 800.0f, out.implausible, RBAMP_FIELD_VOLTAGE);
    } else {
        out.voltage = out.voltage_peak = 0.0f;   /* current-only module */
    }

    for (uint8_t ch = 0; ch < RBAMP_MAX_CHANNELS; ++ch) {
        if (ch >= channels_) {
            out.current[ch] = out.current_peak[ch] = 0.0f;
            out.power[ch] = out.power_factor[ch] = 0.0f;
            continue;
        }
        uint8_t b[rbamp_std::CURRENT_I_PEAK + 4u];
        if (!stdRead(rbamp_std::CURRENT(ch), b, sizeof b)) { setError(RB_ERR_IO); return false; }
        out.current[ch]      = static_cast<float>(leI32(b + rbamp_std::CURRENT_I_RMS))  * 1e-6f;
        out.power[ch]        = static_cast<float>(leI32(b + rbamp_std::CURRENT_P))      * 1e-3f;
        out.power_factor[ch] = static_cast<float>(leI16(b + rbamp_std::CURRENT_PF))     / 1024.0f;
        out.current_peak[ch] = static_cast<float>(leI32(b + rbamp_std::CURRENT_I_PEAK)) * 1e-6f;
        stdSane(out.current[ch],      150.0f,   out.implausible, RBAMP_FIELD_CURRENT);
        stdSane(out.current_peak[ch], 250.0f,   out.implausible, RBAMP_FIELD_CURRENT);
        stdSane(out.power[ch],        30000.0f, out.implausible, RBAMP_FIELD_POWER);
        stdSane(out.power_factor[ch], 1.5f,     out.implausible, RBAMP_FIELD_PF);
    }

    uint16_t chz = 0;
    if (!stdReadU16(rbamp_std::REG_FREQ, chz)) { setError(RB_ERR_IO); return false; }
    out.frequency = static_cast<float>(chz) / 100.0f;

    setError(RB_OK);
    return true;
}

/* ============================================================================
 * STD line — latch banks
 * ============================================================================ */

/* Minimum wait between a latch request and reading the bank: the module
 * services the latch at its next measurement-window boundary (~200-260 ms),
 * and its valid mask is sticky (set at the first latch ever, never cleared),
 * so elapsed time is the only freshness guarantee. Generated (E7). */
static constexpr uint16_t kStdLatchSettleMinMs = rbamp_std::SETTLE_MS_LATCH_PERIOD;

bool RbAmp::stdLatchBank(uint8_t bank) noexcept {
    /* Target register first, opcode second — two transactions, because the
     * module dispatches the opcode from its main loop and would race a burst
     * that carried the bank byte after the opcode. */
    if (!stdWriteU8(rbamp_std::REG_LATCH_BANK, bank)) return false;
    return stdCmd(rbamp_std::CMD_LATCH_PERIOD);
}

bool RbAmp::stdRearmIfLost(uint8_t bank) noexcept {
    /* E15 (family-wide, bench-found): PERIOD_ARM is module RAM — a warm reset,
     * power cycle or firmware activation clears it, after which every latch
     * snapshots an un-armed bank (nsamp = 0, win_count = 0) forever and the
     * latch energy silently freezes. An empty snapshot therefore checks the
     * arm bit; a lost bit is restored from the instance's remembered mask. The
     * outage interval carries no data — the caller skips it, never estimates. */
    uint8_t arm = 0;
    if (!stdReadU8(rbamp_std::REG_PERIOD_ARM, arm)) return false;
    const uint8_t bit = static_cast<uint8_t>(1u << bank);
    if (arm & bit) return false;                     /* armed — just no window yet */
    if (log_) {
        log_->print(F("[rbamp] latch bank ")); log_->print(bank);
        log_->println(F(" lost its arm (module reset detected) — re-armed and primer-latched; drop this period"));
    }
    /* Only THIS bank's bit: other banks are re-armed by their own next read,
     * so each of them reports the reset once (bench finding, E15 rerun). */
    (void)stdWriteU8(rbamp_std::REG_PERIOD_ARM, static_cast<uint8_t>(arm | bit));
    (void)stdLatchBank(bank);                        /* primer: the next period is whole */
    return true;
}

bool RbAmp::stdReadLatchBank(uint8_t bank, RbAmpStdLatch& out) noexcept {
    memset(&out, 0, sizeof out);
    out.bank     = bank;
    out.channels = channels_;

    uint8_t pd = 0;
    if (!stdReadU8(rbamp_std::REG_PD_VALID, pd)) return false;
    out.valid = ((pd >> bank) & 1u) != 0;
    if (!out.valid) {
        setError(RB_ERR_STALE);
        return false;
    }

    const uint16_t base = rbamp_std::LATCH(bank);
    const size_t   len  = rbamp_std::LATCH_CHE_BASE
                        + static_cast<size_t>(channels_) * rbamp_std::LATCH_CHE_STRIDE;
    uint8_t b[rbamp_std::LATCH_CHE_BASE + RBAMP_MAX_CHANNELS * rbamp_std::LATCH_CHE_STRIDE];
    if (!stdReadCoherent(base, b, len)) return false;

    out.p_avg = static_cast<float>(leI32(b + rbamp_std::LATCH_P)) * 1e-3f;
    out.q_avg = static_cast<float>(leI32(b + rbamp_std::LATCH_Q)) * 1e-3f;
    out.s_avg = static_cast<float>(leI32(b + rbamp_std::LATCH_S)) * 1e-3f;
    out.e_import_mws = leI64(b + rbamp_std::LATCH_E_POS);
    out.e_export_mws = leI64(b + rbamp_std::LATCH_E_NEG);
    out.nsamp     = leU32(b + rbamp_std::LATCH_NSAMP);
    out.win_count = leU32(b + rbamp_std::LATCH_WIN_CNT);
    out.max_p = static_cast<float>(leI32(b + rbamp_std::LATCH_MAX_P)) * 1e-3f;
    out.max_i = static_cast<float>(leI32(b + rbamp_std::LATCH_MAX_I)) * 1e-6f;
    out.max_u = static_cast<float>(leI32(b + rbamp_std::LATCH_MAX_U)) * 1e-3f;
    out.min_u = static_cast<float>(leI32(b + rbamp_std::LATCH_MIN_U)) * 1e-3f;
    for (uint8_t c = 0; c < channels_; ++c) {
        const uint8_t* p = b + rbamp_std::LATCH_CHE(c);
        out.e_import_ch_mws[c] = leI64(p + rbamp_std::LATCH_CHE_E_POS);
        out.e_export_ch_mws[c] = leI64(p + rbamp_std::LATCH_CHE_E_NEG);
        /* Contract: per-channel average P over the period = (E_pos + E_neg) / NSAMP [mW]. */
        out.p_avg_ch[c] = out.nsamp
            ? static_cast<float>(static_cast<double>(out.e_import_ch_mws[c] + out.e_export_ch_mws[c])
                                 / static_cast<double>(out.nsamp)) * 1e-3f
            : 0.0f;
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::stdReadPeriodSnapshot(RbAmpPeriodSnapshot& out, uint16_t settle_ms,
                                  bool skip_latch) noexcept {
    /* 1. Latch bank 0 (skip if the caller already used latchPeriod()). The
     *    latch is serviced at the module's next window boundary; a shorter
     *    settle would read the PREVIOUS snapshot as fresh. */
    if (!skip_latch) {
        if (!stdLatchBank(0)) return false;
        current_latch_ms_ = millis();
        if (settle_ms < kStdLatchSettleMinMs) settle_ms = kStdLatchSettleMinMs;
    }

    /* 2. Master-clock window since the previous consumed read — the module has
     *    no clock at all on this line, so this is the only dt there is. */
    if (have_prev_latch_) {
        out.master_dt_ms = current_latch_ms_ - prev_latch_ms_;
    }

    delay(settle_ms);

    if (!skip_latch && !stdCheckCmdError()) {   /* map 0x34+: LATCH_PERIOD accepted */
        return false;
    }

    /* 3. A module reboot inside the period (E15, frame counter went
     *    backwards): the bank holds a partial period — drop it, restart the
     *    period path, never integrate the outage. */
    if (stdRebootedSinceAnchor()) {
        if (log_) log_->println(F("[rbamp] module rebooted inside the period (frame counter went back) — period dropped, re-primed"));
        (void)stdPrimePeriod();
        setError(RB_ERR_STALE);
        return false;
    }

    /* 4. Read bank 0. Anchor HELD on stale/empty (same contract as BASIC). */
    RbAmpStdLatch snap;
    if (!stdReadLatchBank(0, snap)) {
        if (last_error_ == RB_ERR_STALE && log_) {
            log_->println(F("[rbamp] period STALE — bank 0 never latched, anchor held"));
        }
        return false;
    }
    if (snap.win_count == 0) {
        if (stdRearmIfLost(0)) {
            /* Module reset since the last period: the bank accumulates again
             * from the primer latch, so re-anchor the master clock here — the
             * next valid snapshot integrates only its own period, not the outage. */
            prev_latch_ms_    = millis();
            current_latch_ms_ = prev_latch_ms_;
            have_prev_latch_  = true;
        } else if (log_) {
            log_->println(F("[rbamp] period EMPTY — no window elapsed yet (keep bit 0 set in stdArmBanks())"));
        }
        setError(RB_ERR_STALE);
        return false;
    }

    for (uint8_t ch = 0; ch < channels_; ++ch) {
        out.avg_p[ch] = snap.p_avg_ch[ch];
    }
    out.max_p    = snap.max_p;
    out.latch_ms = 0;          /* no module-side clock on this line */
    out.valid    = true;
    out.avg_p_total = snap.p_avg;
    out.wh_net      = static_cast<double>(snap.p_avg) * out.master_dt_ms / 3.6e6;

    /* 5. Commit master timestamp & integrate into the Wh accumulator. */
    prev_latch_ms_   = current_latch_ms_;
    have_prev_latch_ = true;
    energy_.tick(out, channels_);
    setError(RB_OK);
    return true;
}

/* ============================================================================
 * STD line — public block API
 * ============================================================================ */

bool RbAmp::stdInfo(RbAmpStdInfo& out) noexcept {
    if (!std_) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    out.map_version  = std_version_;
    out.capabilities = std_caps_;
    out.status       = std_status_;
    out.n_voltage    = n_voltage_;
    out.n_current    = n_current_;
    out.variant      = std_variant_;
    out.active_bank  = std_active_bank_;
    out.ota_bank_size = std_ota_bank_size_;
    out.period_source = std_period_src_;
    setError(RB_OK);
    return true;
}

bool RbAmp::stdReadChannel(uint8_t ch, RbAmpStdChannel& out) noexcept {
    if (!std_) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    if (ch >= channels_) { setError(RB_ERR_PARAM); return false; }
    return stdReadIpBlock(rbamp_std::CURRENT(ch), out);
}

bool RbAmp::stdReadTotals(uint8_t k, RbAmpStdChannel& out) noexcept {
    if (!std_) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    /* k=0 grand total is always populated; per-phase totals (1..3) are
     * published from map 0x36 (before that they read 0): on a single-phase
     * module k=1 equals channel 0 and k=2/3 read 0. */
    if (k != 0 && std_version_ < 0x36) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    if (k >= rbamp_std::TOTALS_MAX) { setError(RB_ERR_PARAM); return false; }
    if (!stdReadIpBlock(rbamp_std::TOTALS(k), out)) return false;
    /* The counter family never writes the TOTALS energy accumulators — its
     * energy is the life-time counter block (stdReadEnergy()). Report 0 rather
     * than a stale/unmapped word that looks like a reading. */
    if (std_period_src_ != RbAmpStdPeriodSource::Latch) {
        out.e_import_mws = 0;
        out.e_export_mws = 0;
    }
    return true;
}

bool RbAmp::stdReadVoltage(uint8_t v, RbAmpStdVoltage& out) noexcept {
    if (!std_) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    if (v >= n_voltage_) { setError(RB_ERR_PARAM); return false; }
    uint8_t b[rbamp_std::VOLTAGE_ANGLE + 2u];
    if (!stdRead(rbamp_std::VOLTAGE(v), b, sizeof b)) { setError(RB_ERR_IO); return false; }
    memset(&out, 0, sizeof out);
    out.u_rms  = static_cast<float>(leI32(b + rbamp_std::VOLTAGE_U_RMS))  * 1e-3f;
    out.u_peak = static_cast<float>(leI32(b + rbamp_std::VOLTAGE_U_PEAK)) * 1e-3f;
    out.thd_u  = static_cast<float>(leU16(b + rbamp_std::VOLTAGE_THD_U))  / 100.0f;
    /* Phase angle: i16 centi-degrees vs voltage 0, 0x8000 = no angle. Images
     * before 0x7A leave the bytes 0 — not an angle, so the version gates it. */
    const uint16_t raw = leU16(b + rbamp_std::VOLTAGE_ANGLE);
    out.angle_valid = std_fw_ver_ >= kStdFwAngles && raw != 0x8000u;
    out.angle_deg   = out.angle_valid ? static_cast<float>(static_cast<int16_t>(raw)) / 100.0f : 0.0f;
    stdSane(out.u_rms,  500.0f, out.implausible, RBAMP_FIELD_VOLTAGE);
    stdSane(out.u_peak, 800.0f, out.implausible, RBAMP_FIELD_VOLTAGE);
    setError(RB_OK);
    return true;
}

bool RbAmp::stdReadPhaseSequence(RbAmpStdPhaseSeq& out) noexcept {
    out = RbAmpStdPhaseSeq::NotAvailable;
    if (!std_) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    if (std_fw_ver_ < kStdFwAngles) { setError(RB_OK); return true; }   /* not published on this image */
    uint8_t v = 0;
    if (!stdReadU8(rbamp_std::REG_PHASE_SEQ, v)) return false;
    if (v <= static_cast<uint8_t>(RbAmpStdPhaseSeq::Abnormal)) out = static_cast<RbAmpStdPhaseSeq>(v);
    setError(RB_OK);
    return true;
}

bool RbAmp::stdReadHealth(RbAmpStdHealth& out) noexcept {
    if (!std_) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    memset(&out, 0, sizeof out);
    if (!stdReadU8(rbamp_std::REG_STATUS, out.status)) return false;
    uint8_t b[8];   /* FRAME_CNT + CRC_ERR are adjacent u32s */
    if (!stdRead(rbamp_std::REG_FRAME_CNT, b, sizeof b)) return false;
    out.frame_count = leU32(b);
    out.crc_errors  = leU32(b + 4);
    if (std_version_ < 0x35) { setError(RB_OK); return true; }   /* the rest is not mapped below 0x35 */
    out.time_valid = (out.status & rbamp_std::ST_TIME_VALID) != 0;
    out.nvm_fault  = (out.status & rbamp_std::ST_NVM_FAULT)  != 0;
    out.maint      = (out.status & rbamp_std::ST_MAINT)      != 0;
    static_assert(rbamp_std::REG_LTG_STATUS == rbamp_std::REG_LTA_STATUS + 3, "NVM status bytes moved");
    uint8_t n[4];   /* LTA_STATUS, LTA_SRC, CAL_SRC, LTG_STATUS are adjacent (LTG reads 0 below 0x36) */
    if (!stdRead(rbamp_std::REG_LTA_STATUS, n, sizeof n)) return false;
    out.lta_status   = n[0];
    out.lta_src      = n[1];
    out.cal_src      = n[2];
    out.group_status = (std_version_ >= 0x36) ? n[3] : 0;
    if (!stdReadU16(rbamp_std::REG_LEGAL_NOEN, out.legal_noen)) return false;
    /* RING_DROP .. LOST_FRAMES sit in one vendor-diag run: one burst. */
    uint8_t d[rbamp_std::REG_LOST_FRAMES + 4 - rbamp_std::REG_RING_DROP];
    if (!stdRead(rbamp_std::REG_RING_DROP, d, sizeof d)) return false;
    out.ring_drop   = leU16(d);
    out.lost_frames = leU32(d + (rbamp_std::REG_LOST_FRAMES - rbamp_std::REG_RING_DROP));
    if (!stdReadU32(rbamp_std::ENERGY_BASE + rbamp_std::ENERGY_UPTIME, out.uptime_s)) return false;
    if (std_fw_ver_ >= 0x00010076u) {   /* RESET_CAUSE u8 + pad + BOOT_COUNT u16 in one small burst */
        static_assert(rbamp_std::REG_BOOT_COUNT == rbamp_std::REG_RESET_CAUSE + 2, "RESET_CAUSE / BOOT_COUNT moved");
        uint8_t r[4];
        if (!stdRead(rbamp_std::REG_RESET_CAUSE, r, sizeof r)) return false;
        out.reset_cause = r[0];
        out.boot_count  = leU16(r + 2);
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::stdArmBanks(uint8_t mask) noexcept {
    if (!std_ || std_period_src_ != RbAmpStdPeriodSource::Latch) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    if ((mask & 0x01u) == 0 && log_) {
        log_->println(F("[rbamp] arm mask clears bank 0 — readPeriodSnapshot() will report EMPTY"));
    }
    if (!stdWriteU8(rbamp_std::REG_PERIOD_ARM, mask)) return false;
    setError(RB_OK);
    return true;
}

bool RbAmp::stdLatch(uint8_t bank) noexcept {
    if (!std_ || std_period_src_ != RbAmpStdPeriodSource::Latch) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    if (bank >= rbamp_std::LATCH_MAX) { setError(RB_ERR_PARAM); return false; }
    if (!stdLatchBank(bank)) return false;
    setError(RB_OK);
    return true;
}

bool RbAmp::stdReadLatch(uint8_t bank, RbAmpStdLatch& out) noexcept {
    if (!std_ || std_period_src_ != RbAmpStdPeriodSource::Latch) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    if (bank >= rbamp_std::LATCH_MAX) { setError(RB_ERR_PARAM); return false; }
    if (!stdReadLatchBank(bank, out)) return false;
    if (out.win_count == 0 && stdRearmIfLost(bank)) {
        setError(RB_ERR_STALE);   /* re-armed; this snapshot is the outage, not a period */
        return false;
    }
    return true;
}

bool RbAmp::stdLatchWh(const RbAmpStdLatch& snap, uint32_t dt_ms, uint8_t ch,
                       double& wh_import, double& wh_export) noexcept {
    if (ch != RBAMP_STD_TOTAL && ch >= snap.channels) return false;
    if (snap.nsamp == 0) return false;
    const int64_t e_pos = (ch == RBAMP_STD_TOTAL) ? snap.e_import_mws : snap.e_import_ch_mws[ch];
    const int64_t e_neg = (ch == RBAMP_STD_TOTAL) ? snap.e_export_mws : snap.e_export_ch_mws[ch];
    /* mW·samples → Wh: /1000 (mW→W) /3600 (s→h) × seconds-per-sample (dt/nsamp). */
    const double per_sample_h = (static_cast<double>(dt_ms) / 1000.0)
                              / static_cast<double>(snap.nsamp) / 3600.0;
    wh_import = static_cast<double>(e_pos) / 1000.0 * per_sample_h;
    wh_export = static_cast<double>(e_neg) / 1000.0 * per_sample_h;
    return true;
}

/* ============================================================================
 * STD line — configuration / calibration commands
 * ============================================================================ */

/* STD: the CT model is a REFERENCE (SKU metadata) — it sets no gain; the
 * module's calibration lives in the CAL block. The per-channel map entry
 * CHMAP_MODEL(c) is what readCTModelCh() reads back; the module persists only
 * the single CFG_CT_MODEL byte and re-seeds CHMAP_MODEL(0) from it at boot, so
 * channel 0 also writes CFG_CT_MODEL (channels 1+ hold their entry until reboot
 * on current module firmware — per-channel persist is a pending module batch).
 * Precondition: CFG_CLASS != 0 (map default 0 = unset). No save here. */
bool RbAmp::stdApplyCtModel(uint8_t channel, uint8_t code) noexcept {
    /* From map 0x35 the capability bits are honest: a module that does not
     * persist a per-channel CT model does not take the write either — refuse
     * before any traffic instead of failing the readback afterwards. */
    if (std_version_ >= 0x35 && !(std_caps_ & rbamp_std::CAP_CHMAP_PERSIST)) {
        setError(RB_ERR_NOT_IMPLEMENTED);
        return false;
    }
    uint8_t cls = 0;
    if (!stdReadU8(rbamp_std::REG_CFG_CLASS, cls)) return false;
    if (cls == rbamp_std::SENSOR_UNSET) {
        if (log_) log_->println(F("[rbamp] setCTModel refused: sensor class is UNSET (call setSensorClass first)"));
        setError(RB_ERR_PARAM);
        return false;
    }
    if (!ctModelValid(static_cast<RbAmpSensorClass>(cls), code)) {  /* known-code fast-fail */
        setError(RB_ERR_PARAM);
        return false;
    }
    if (!stdWriteU8(rbamp_std::CHMAP_MODEL(channel), code)) return false;
    if (channel == 0 && !stdWriteU8(rbamp_std::REG_CFG_CT_MODEL, code)) return false;
    /* Readback-confirm (every module write is verified). */
    uint8_t applied = 0;
    if (!stdReadU8(rbamp_std::CHMAP_MODEL(channel), applied)) return false;
    if (applied != code) {
        setError(RB_ERR_PARAM);
        return false;
    }
    return true;
}

bool RbAmp::stdReadConfig(RbAmpStdConfig& out) noexcept {
    if (!std_) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    uint8_t b[rbamp_std::REG_CFG_HARM - rbamp_std::REG_CFG_CLASS + 1u];   /* 0x0070..0x0076 */
    if (!stdRead(rbamp_std::REG_CFG_CLASS, b, sizeof b)) return false;
    /* Offsets relative to CFG_CLASS: CT_MODEL +1, TIER +2, I2C_ADDR +3,
     * RATE +4 (u16), HARM +6. */
    out.sensor_class = b[0];
    out.ct_model     = b[1];
    out.tier         = b[2];
    out.i2c_addr     = b[3];
    out.rate_ms      = leU16(b + 4);
    out.harmonics    = (b[6] & 0x01u) != 0;
    setError(RB_OK);
    return true;
}

bool RbAmp::stdSetHarmonics(bool enable) noexcept {
    if (!std_) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    if ((std_caps_ & rbamp_std::CAP_THD) == 0) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    /* RAM-only switch (not persisted); bit 0 = base-harmonics extraction. */
    const uint8_t want = enable ? 0x01u : 0x00u;
    if (!stdWriteU8(rbamp_std::REG_CFG_HARM, want)) return false;
    uint8_t rb = 0xFF;
    if (!stdReadU8(rbamp_std::REG_CFG_HARM, rb)) return false;
    if ((rb & 0x01u) != want) {
        setError(RB_ERR_PARAM);
        return false;
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::stdStartCurrentGate() noexcept {
    if (!std_ || (std_caps_ & rbamp_std::CAP_ENERGY_UWH) == 0 || std_fw_ver_ < kStdFwStartUa) {
        setError(RB_ERR_NOT_IMPLEMENTED);
        return false;
    }
    return true;
}

bool RbAmp::stdReadEnergyStartCurrent(uint16_t& ua) noexcept {
    ua = 0;
    if (!stdStartCurrentGate()) return false;
    if (!stdReadU16(rbamp_std::REG_CFG_START_UA, ua)) return false;
    setError(RB_OK);
    return true;
}

bool RbAmp::stdSetEnergyStartCurrent(uint16_t ua) noexcept {
    if (!stdStartCurrentGate()) return false;
    /* 0 = off, else 10..2540 µA in 10 µA steps — refused here, no bus write. */
    if (ua != 0 && (ua < 10u || ua > 2540u || (ua % 10u) != 0)) { setError(RB_ERR_PARAM); return false; }
    if (!stdWriteU16(rbamp_std::REG_CFG_START_UA, ua)) return false;
    uint16_t rb = 0xFFFFu;
    if (!stdReadU16(rbamp_std::REG_CFG_START_UA, rb)) return false;
    if (rb != ua) { setError(RB_ERR_PARAM); return false; }   /* module did not take it */
    setError(RB_OK);
    return true;
}

bool RbAmp::stdCalThdFloor() noexcept {
    if (!std_) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    if ((std_caps_ & rbamp_std::CAP_THD) == 0) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    return stdSaveSync(rbamp_std::CMD_CAL_THD_FLOOR, rbamp_std::SETTLE_MS_CAL_THD_FLOOR);
}

/* ============================================================================
 * STD line — firmware update (two-phase OTA over I2C)
 *
 * BEGIN erases the inactive bank (synchronous on the module; OTA_STATUS
 * becomes READY afterwards) → pages of 256 B via OTA_IDX + OTA_PAYLOAD +
 * BLOCK_WR → COMMIT verifies CRC + anti-downgrade and ARMS the swap without
 * rebooting (OTA_STATUS 2 ok / 3 err / 4 downgrade) → ACTIVATE reboots into
 * the armed bank when the host chooses. A bad new image is rolled back by the
 * module's bootloader; calibration lives outside the banks and survives.
 * ============================================================================ */

bool RbAmp::stdWriteU16(uint16_t reg, uint16_t val) noexcept {
    const uint8_t b[2] = { static_cast<uint8_t>(val), static_cast<uint8_t>(val >> 8) };
    return stdWrite(reg, b, 2);
}

bool RbAmp::stdWriteU32(uint16_t reg, uint32_t val) noexcept {
    const uint8_t b[4] = { static_cast<uint8_t>(val), static_cast<uint8_t>(val >> 8),
                           static_cast<uint8_t>(val >> 16), static_cast<uint8_t>(val >> 24) };
    return stdWrite(reg, b, 4);
}

bool RbAmp::stdOtaGate() noexcept {
    if (!std_ || std_version_ < 0x33 || (std_caps_ & rbamp_std::CAP_OTA) == 0) {
        setError(RB_ERR_NOT_IMPLEMENTED);   /* two-phase layout from map 0x33 + OTA capability */
        return false;
    }
    return true;
}

/* Poll OTA_STATUS until it is one of `accept` (bit per value). While the
 * module erases / programs flash its I2C path stalls (the slave handler runs
 * from the flash being written), so a NACK or clock-stretch failure during the
 * wait means BUSY, not "gone": keep polling until the deadline (bench finding,
 * HIL-4). Only the deadline reports a failure. */
bool RbAmp::stdOtaWait(uint32_t timeout_ms, uint8_t accept, uint8_t& last) noexcept {
    const uint32_t start = millis();
    uint8_t st = 0xFF;
    do {
        if (!stdReadU8(rbamp_std::REG_OTA_STATUS, st)) {
            st = 0xFF;             /* busy in flash — retry after the gap */
        } else if (st < 8 && (accept & (1u << st))) {
            last = st;
            setError(RB_OK);
            return true;
        }
        delay(50);
    } while ((millis() - start) < timeout_ms);
    last = st;
    setError(RB_ERR_TIMEOUT);
    return false;
}

bool RbAmp::stdOtaStatus(uint8_t& out) noexcept {
    if (!stdOtaGate()) return false;
    if (!stdReadU8(rbamp_std::REG_OTA_STATUS, out)) return false;
    setError(RB_OK);
    return true;
}

bool RbAmp::stdOtaBegin(uint32_t timeout_ms) noexcept {
    if (!stdOtaGate()) return false;
    if (timeout_ms == 0) timeout_ms = 5000;
    if (!stdWaitIdleImpl(timeout_ms, rbamp_std::CMD_OTA_BEGIN)) return false;
    if (!stdCmd(rbamp_std::CMD_OTA_BEGIN)) return false;
    /* The erase blocks the module's main loop (~1.2 s) and stalls its I2C
     * path. With a command-state register (map 0x35+) BUSY covers the erase;
     * before that OTA_STATUS reads 1 for BOTH "busy" and "ready", so a poll
     * that starts mid-erase passes early — sleep the command's settle first,
     * then poll for READY (bench finding, HIL-4). */
    if (std_version_ < 0x35) delay(rbamp_std::SETTLE_MS_OTA_BEGIN);
    if (!stdOtaWaitCmdDone(timeout_ms, rbamp_std::CMD_OTA_BEGIN)) return false;
    uint8_t st = 0;
    if (!stdOtaWait(timeout_ms, static_cast<uint8_t>(1u << rbamp_std::OTA_READY), st)) {
        if (log_) { log_->print(F("[rbamp] ota begin: status=")); log_->println(st); }
        return false;
    }
    if (!stdCheckCmdError()) return false;
    setError(RB_OK);
    return true;
}

bool RbAmp::stdOtaWritePage(uint32_t page_idx, const uint8_t* page) noexcept {
    if (!stdOtaGate()) return false;
    if (page == nullptr) { setError(RB_ERR_PARAM); return false; }
    if (!stdWriteU32(rbamp_std::REG_OTA_IDX, page_idx)) return false;
    if (!stdWrite(rbamp_std::REG_OTA_PAYLOAD, page, rbamp_std::REG_OTA_PAYLOAD_SIZE)) return false;
    if (std_version_ >= 0x35) {
        /* Optional end-to-end check (OTA extension block, map 0x35+): the
         * module compares its received CRC-32 with this and refuses the page
         * with ERROR 0xFC on a mismatch — then we resend. */
        if (!stdWriteU32(rbamp_std::REG_OTA_PCRC_EXP,
                         stdCrc32(page, rbamp_std::REG_OTA_PAYLOAD_SIZE))) return false;
    }
    if (!stdWaitIdleImpl(kStdOtaPageTimeoutMs, rbamp_std::CMD_OTA_BLOCK_WR)) return false;
    if (!stdCmd(rbamp_std::CMD_OTA_BLOCK_WR)) return false;
    /* Page program stalls the slave (its I2C handler runs from the flash being
     * written). Map 0x35+: BUSY covers the program slot (one page per 250 ms
     * on the counter family). Before that the generated settle (30 ms, E11)
     * covers the stall before the verdict read. */
    if (std_version_ < 0x35) delay(rbamp_std::SETTLE_MS_OTA_BLOCK_WR);
    if (!stdOtaWaitCmdDone(kStdOtaPageTimeoutMs, rbamp_std::CMD_OTA_BLOCK_WR)) return false;
    uint8_t e = 0;
    if (!stdReadU8(rbamp_std::REG_ERROR, e)) return false;
    if (std_version_ >= 0x34 && e == rbamp_std::DEV_ERR_VALUE_CRC) {
        /* Payload did not arrive intact — the caller resends this page. */
        if (log_) { log_->print(F("[rbamp] ota page ")); log_->print(page_idx); log_->println(F(": payload CRC mismatch — resend")); }
        setError(RB_ERR_CHECKSUM);
        return false;
    }
    if (!stdCheckCmdError()) return false;
    /* The session must still be open: anything but READY after a page means
     * the module dropped it (0 = no session, 3 = failed; ERROR may still read
     * 0) and later pages are ignored — the caller restarts from BEGIN. */
    uint8_t st = 0;
    if (!stdReadU8(rbamp_std::REG_OTA_STATUS, st)) return false;
    if (st != rbamp_std::OTA_READY) {
        if (log_) { log_->print(F("[rbamp] ota page ")); log_->print(page_idx);
                    log_->print(F(": session not open after the write (status=")); log_->print(st); log_->println(F(") — restart from begin")); }
        stdOtaLogDiag();
        setError(st == rbamp_std::OTA_ERR ? RB_ERR_IO : RB_ERR_NOT_READY);
        return false;
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::stdOtaWaitCmdDone(uint32_t timeout_ms, uint8_t opcode) noexcept {
    /* Map 0x35+: completion is the command state's BUSY bit, nothing else
     * (ERROR may read 0 mid-run on images before 0x79; OTA_STATUS never
     * reports "busy" there). */
    if (std_version_ >= 0x35) return stdWaitIdleImpl(timeout_ms, opcode);
    const uint32_t start = millis();
    do {
        uint8_t e = 0xFF, st = 0xFF;
        const bool ok = stdReadU8(rbamp_std::REG_ERROR, e) && stdReadU8(rbamp_std::REG_OTA_STATUS, st);
        if (ok && e != rbamp_std::DEV_ERR_PENDING && st != rbamp_std::OTA_BUSY) {
            setError(RB_OK);
            return true;
        }
        delay(50);   /* pending / busy / flash stall — poll again */
    } while ((millis() - start) < timeout_ms);
    if (log_) log_->println(F("[rbamp] ota: command still busy at timeout"));
    setError(RB_ERR_TIMEOUT);
    return false;
}

/* OTA_DIAG reason names — log text only; the API returns the raw byte. */
static const char* stdOtaReasonName(uint8_t r) {
    static const char* const kName[] = {
        "none", "RANGE", "ERASE", "ERASE_VERIFY", "RESEND", "PROG", "VERIFY", "HDR",
        "SECTOR", "CRC", "MAC", "DOWNGRADE", "SWAP", "SEAL", "SIG", "BOARD",
    };
    return (r < sizeof kName / sizeof kName[0]) ? kName[r] : "?";
}

void RbAmp::stdOtaLogDiag() noexcept {
    uint8_t reason = 0, hal = 0;
    if (log_ && stdOtaDiag(reason, hal)) {
        log_->print(F("[rbamp] ota diag: reason=")); log_->print(reason);
        log_->print(F(" (")); log_->print(stdOtaReasonName(reason));
        log_->print(F(") status=0x")); log_->println(hal, HEX);
    }
}

bool RbAmp::stdOtaCommit(uint32_t timeout_ms) noexcept {
    if (!stdOtaGate()) return false;
    if (timeout_ms == 0) timeout_ms = 15000;   /* a signed bank image is verified whole (~2 s; contract: >= 3 s) */
    if (timeout_ms < 3000) timeout_ms = 3000;
    if (!stdWaitIdleImpl(timeout_ms, rbamp_std::CMD_OTA_COMMIT)) return false;
    if (!stdCmd(rbamp_std::CMD_OTA_COMMIT)) return false;
    if (std_version_ < 0x35) delay(rbamp_std::SETTLE_MS_OTA_COMMIT);
    if (!stdOtaWaitCmdDone(timeout_ms, rbamp_std::CMD_OTA_COMMIT)) return false;
    uint8_t st = 0;
    if (!stdOtaWait(timeout_ms, static_cast<uint8_t>((1u << rbamp_std::OTA_OK) | (1u << rbamp_std::OTA_ERR)
                                                     | (1u << rbamp_std::OTA_DOWNGRADE)), st)) {
        if (log_) { log_->print(F("[rbamp] ota commit: status=")); log_->println(st); }
        return false;
    }
    switch (st) {
        case rbamp_std::OTA_OK:
            if (!stdCheckCmdError()) return false;   /* armed */
            setError(RB_OK);
            return true;
        case rbamp_std::OTA_DOWNGRADE:
            if (log_) log_->println(F("[rbamp] ota commit: image version below the running one — rejected"));
            setError(RB_ERR_VERSION);
            return false;
        default:
            if (log_) log_->println(F("[rbamp] ota commit: image rejected (bad header / checksum / authentication)"));
            stdOtaLogDiag();
            setError(RB_ERR_CHECKSUM);
            return false;
    }
}

bool RbAmp::stdOtaActivate() noexcept {
    if (!stdOtaGate()) return false;
    uint8_t st = 0;
    if (!stdReadU8(rbamp_std::REG_OTA_STATUS, st)) return false;
    if (st != rbamp_std::OTA_OK) {
        if (log_) { log_->print(F("[rbamp] ota activate: nothing armed (status=")); log_->print(st); log_->println(F(") — commit first")); }
        setError(RB_ERR_NOT_READY);
        return false;
    }
    const uint32_t before = std_active_bank_;
    uint32_t staged = 0;
    if (std_version_ >= 0x35 && !stdReadU32(rbamp_std::REG_FW_STAGED_VERSION, staged)) return false;
    if (!stdWaitIdleImpl(kStdCmdMinTimeoutMs, rbamp_std::CMD_OTA_ACTIVATE)) return false;
    if (!stdCmd(rbamp_std::CMD_OTA_ACTIVATE)) return false;
    /* The module resets into the armed bank and re-enumerates on the bus after
     * REBOOT_MS; then rediscover it (new firmware may report a newer map) and
     * confirm the running bank changed. */
    delay(rbamp_std::REBOOT_MS_OTA_ACTIVATE);
    bool is_std = false, ok = false;
    for (uint8_t attempt = 0; attempt < 12; ++attempt) {   /* up to ~6 s past REBOOT_MS */
        ok = stdDetect(is_std);
        if (ok && is_std) break;
        delay(500);
    }
    if (!ok) return false;               /* error already set */
    if (!is_std) { setError(RB_ERR_VERSION); return false; }
    if (!stdBegin()) return false;
    if (std_active_bank_ == before) {
        if (log_) { log_->print(F("[rbamp] ota activate: still running bank 0x")); log_->print(before, HEX); log_->println(F(" (bootloader rollback?)")); }
        setError(RB_ERR_NOT_READY);
        return false;
    }
    if (log_) { log_->print(F("[rbamp] ota activate: running bank 0x")); log_->print(std_active_bank_, HEX);
                log_->print(F(" map=0x")); log_->println(std_version_, HEX); }
    if (std_version_ >= 0x35) {
        /* Trial boot: the new image confirms itself after ~8 s of healthy
         * metering; any reset before that rolls back. Stay on the bus (no
         * reset from here) until the swap flag drops, then check the image
         * version really is the one that was staged. */
        const uint32_t t0 = millis();
        uint32_t pending = 0;
        for (;;) {
            if (!stdReadU32(rbamp_std::REG_SWAP_PENDING, pending)) pending = 0xFFFFFFFFu;
            if (pending == 0 || pending == 0xFFFFFFFFu) break;   /* confirmed, or the register is not mapped */
            if (millis() - t0 >= kStdOtaConfirmTimeoutMs) {
                if (log_) log_->println(F("[rbamp] ota activate: new image did not confirm itself (swap still pending)"));
                setError(RB_ERR_NOT_READY);
                return false;
            }
            delay(500);
        }
        uint32_t running = 0;
        if (staged != 0 && staged != 0xFFFFFFFFu && stdReadU32(rbamp_std::REG_FW_VER, running)
                && running != 0xFFFFFFFFu && running != staged) {
            if (log_) { log_->print(F("[rbamp] ota activate: running image 0x")); log_->print(running, HEX);
                        log_->print(F(" is not the staged 0x")); log_->print(staged, HEX); log_->println(F(" (rolled back)")); }
            setError(RB_ERR_VERSION);
            return false;
        }
    }
    setError(RB_OK);
    return true;
}

uint32_t RbAmp::stdCrc32Update(uint32_t crc, const uint8_t* data, size_t len) noexcept {
    /* Reflected CRC-32 with the map's parameters (zlib-compatible); crc is the
     * running register (pre-XOROUT). */
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (rbamp_std::OTA_CRC_POLY & (0u - (crc & 1u)));
        }
    }
    return crc;
}

uint32_t RbAmp::stdCrc32(const uint8_t* data, size_t len) noexcept {
    return stdCrc32Update(rbamp_std::OTA_CRC_INIT, data, len) ^ rbamp_std::OTA_CRC_XOROUT;
}

bool RbAmp::stdOtaWriteImage(const uint8_t* image, size_t len, uint32_t version,
                             RbAmpOtaProgress cb, void* ctx) noexcept {
    (void)image; (void)len; (void)version; (void)cb; (void)ctx;
    if (log_) log_->println(F("[rbamp] ota: host-built images are not accepted — use stdOtaWritePreparedImage()"));
    setError(RB_ERR_NOT_IMPLEMENTED);
    return false;
}

bool RbAmp::stdOtaWriteImage(Stream& image, size_t len, uint32_t version,
                             RbAmpOtaProgress cb, void* ctx) noexcept {
    (void)image; (void)len; (void)version; (void)cb; (void)ctx;
    if (log_) log_->println(F("[rbamp] ota: host-built images are not accepted — use stdOtaWritePreparedImage()"));
    setError(RB_ERR_NOT_IMPLEMENTED);
    return false;
}

bool RbAmp::stdOtaFetchPage(const uint8_t* buf, Stream* stream, size_t off, size_t n,
                            uint8_t* page) noexcept {
    if (buf != nullptr) {
        memcpy(page, buf + off, n);
        return true;
    }
    /* Stream source: read exactly n bytes (retry short reads, bounded). */
    size_t got = 0;
    uint32_t t0 = millis();
    while (got < n) {
        const size_t r = stream->readBytes(reinterpret_cast<char*>(page + got), n - got);
        if (r > 0) { got += r; t0 = millis(); }
        else if ((millis() - t0) > 5000u) break;
    }
    if (got != n) {
        if (log_) { log_->print(F("[rbamp] ota: short image read at offset ")); log_->println(static_cast<uint32_t>(off)); }
        setError(RB_ERR_IO);
        return false;
    }
    return true;
}

/* ============================================================================
 * STD line — prepared (signed) bank images
 * ============================================================================ */

bool RbAmp::stdOtaWritePreparedImage(const uint8_t* image, size_t len,
                                     RbAmpOtaProgress cb, void* ctx) noexcept {
    if (image == nullptr) { setError(RB_ERR_PARAM); return false; }
    return stdOtaWritePreparedImpl(image, nullptr, len, cb, ctx);
}

bool RbAmp::stdOtaWritePreparedImage(Stream& image, size_t len,
                                     RbAmpOtaProgress cb, void* ctx) noexcept {
    return stdOtaWritePreparedImpl(nullptr, &image, len, cb, ctx);
}

bool RbAmp::stdOtaWritePreparedImage(const uint8_t* bank_a, const uint8_t* bank_b, size_t len,
                                     RbAmpOtaProgress cb, void* ctx) noexcept {
    if (bank_a == nullptr || bank_b == nullptr) { setError(RB_ERR_PARAM); return false; }
    uint8_t running = 0xFF;
    if (!stdOtaActiveBank(running)) return false;
    /* The image goes into the bank that is NOT running. */
    return stdOtaWritePreparedImpl(running == 0 ? bank_b : bank_a, nullptr, len, cb, ctx);
}

bool RbAmp::stdOtaWritePreparedImpl(const uint8_t* buf, Stream* stream, size_t len,
                                    RbAmpOtaProgress cb, void* ctx) noexcept {
    if (!stdOtaGate()) return false;
    /* A prepared image IS the whole bank: its size must match the module. */
    uint32_t bank = 0;
    if (!stdReadU32(rbamp_std::REG_OTA_BANK_SIZE, bank)) return false;
    const uint32_t page_sz = rbamp_std::OTA_PAGE_SIZE;
    if (bank == 0 || bank == 0xFFFFFFFFu || (bank % page_sz) != 0 || bank < 2u * page_sz) {
        if (log_) { log_->print(F("[rbamp] ota: module reports no usable bank size (0x")); log_->print(bank, HEX); log_->println(')'); }
        setError(RB_ERR_NOT_IMPLEMENTED);
        return false;
    }
    std_ota_bank_size_ = bank;
    if (len != bank) {
        if (log_) { log_->print(F("[rbamp] ota: prepared image is ")); log_->print(static_cast<uint32_t>(len));
                    log_->print(F(" B, the module's bank is 0x")); log_->print(bank, HEX); log_->println(F(" — refused")); }
        setError(RB_ERR_PARAM);
        return false;
    }
    const uint32_t pages = bank / page_sz;

    if (!stdOtaBegin(0)) return false;

    uint8_t page[rbamp_std::OTA_PAGE_SIZE];
    for (uint32_t idx = 0; idx < pages; ++idx) {
        if (!stdOtaFetchPage(buf, stream, static_cast<size_t>(idx) * page_sz, page_sz, page)) return false;
        /* Page 0 always goes (and goes first); other all-0xFF pages are what
         * the erased bank already holds. The image's own header page is the
         * last one and is streamed like any other — nothing is built here. */
        bool blank = (idx != 0);
        for (uint32_t i = 0; i < page_sz && blank; ++i) blank = (page[i] == 0xFF);
        if (!blank) {
            if (!stdOtaWritePage(idx, page)) {
                if (last_error_ != RB_ERR_CHECKSUM) return false;
                if (!stdOtaWritePage(idx, page)) return false;   /* one resend on a payload CRC miss */
            }
        }
        if (cb) cb(idx + 1, pages, ctx);
    }
    return stdOtaCommit(0);
}

bool RbAmp::stdOtaActiveBank(uint8_t& out) noexcept {
    if (!stdOtaGate()) return false;
    uint8_t v = 0xFF;
    if (!stdReadU8(rbamp_std::REG_FW_ACTIVE_BANK, v)) return false;
    if (v > 1) {   /* register not implemented on this module (reads unmapped) */
        setError(RB_ERR_NOT_IMPLEMENTED);
        return false;
    }
    out = v;
    setError(RB_OK);
    return true;
}

bool RbAmp::stdOtaStagedVersion(uint32_t& out) noexcept {
    if (!stdOtaGate()) return false;
    if (!stdReadU32(rbamp_std::REG_FW_STAGED_VERSION, out)) return false;
    setError(RB_OK);
    return true;
}

bool RbAmp::stdOtaDiag(uint8_t& reason, uint8_t& hal_status) noexcept {
    if (!stdOtaGate()) return false;
    if (std_version_ < 0x35) {   /* OTA extension block exists from map 0x35 */
        setError(RB_ERR_NOT_IMPLEMENTED);
        return false;
    }
    uint8_t b[2];
    if (!stdRead(rbamp_std::REG_OTA_DIAG, b, 2)) return false;
    reason     = (b[0] == 0xFF) ? 0 : b[0];   /* 0xFF = unmapped on this module */
    hal_status = b[1];
    setError(RB_OK);
    return true;
}

bool RbAmp::stdOtaAbort() noexcept {
    if (!stdOtaGate()) return false;
    if (!stdWaitIdleImpl(kStdCmdMinTimeoutMs, rbamp_std::CMD_FW_ABORT)) return false;
    if (!stdCmd(rbamp_std::CMD_FW_ABORT)) return false;
    if (std_version_ < 0x35) delay(rbamp_std::SETTLE_MS_FW_ABORT);
    if (!stdOtaWaitCmdDone(5000, rbamp_std::CMD_FW_ABORT)) return false;
    if (!stdCheckCmdError()) return false;
    setError(RB_OK);
    return true;
}

/* ============================================================================
 * Energy total with the module's own answer for "unknown"
 * ============================================================================ */

double RbAmp::energyWh(uint8_t ch) const noexcept {
    /* Counter-path multi-channel modules below map 0x36 publish no
     * per-channel energy: say so with NaN rather than a zero that reads as
     * "nothing consumed". */
    if (std_ && std_period_src_ == RbAmpStdPeriodSource::Counters && std_version_ < 0x36
            && channels_ > 1 && ch != RBAMP_ENERGY_CH_TOTAL && ch < channels_) {
        return NAN;
    }
    return energy_.wh(ch);
}

/* ============================================================================
 * STD line — map 0x35+ plumbing: gates, command completion, verdicts
 * ============================================================================ */

namespace {
inline uint64_t leU48(const uint8_t* p) {
    return static_cast<uint64_t>(leU32(p)) | (static_cast<uint64_t>(leU16(p + 4)) << 32);
}
/* One raw STD read / write on any address without an instance (bus scan,
 * commissioning): one try, no retry layer — a NACK is the answer. */
bool rawStdRead(TwoWire& bus, uint8_t addr, uint16_t reg, uint8_t* buf, uint8_t n) {
    bus.beginTransmission(addr);
    bus.write(static_cast<uint8_t>(reg >> 8));
    bus.write(static_cast<uint8_t>(reg));
    if (bus.endTransmission(false) != 0) return false;
    if (bus.requestFrom(addr, n) < n) return false;
    for (uint8_t i = 0; i < n; ++i) {
        const int b = bus.read();
        if (b < 0) return false;
        buf[i] = static_cast<uint8_t>(b);
    }
    return true;
}
bool rawStdWrite(TwoWire& bus, uint8_t addr, const uint8_t* frame, uint8_t n) {
    bus.beginTransmission(addr);
    bus.write(frame, n);
    return bus.endTransmission() == 0;
}
} /* namespace */

bool RbAmp::stdGate35() noexcept {
    if (!std_ || std_version_ < 0x35) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    return true;
}

bool RbAmp::stdGate36() noexcept {
    if (!std_ || std_version_ < 0x36) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    return true;
}

/* Poll the command-state register until BUSY clears (map 0x35+; a no-op
 * before). OVERRUN means a command arrived while the module was busy and was
 * dropped — logged, since the first command was not clobbered. Transport
 * errors during the poll are tolerated until the deadline. */
bool RbAmp::stdWaitIdleImpl(uint32_t timeout_ms, uint8_t opcode) noexcept {
    if (std_version_ < 0x35) return true;
    const uint32_t t0 = millis();
    bool overrun_logged = false;
    for (;;) {
        uint8_t st = 0;
        if (stdReadU8(rbamp_std::REG_CMD_STATE, st)) {
            if ((st & 0x02u) && !overrun_logged && log_) {
                log_->print(F("[rbamp] command 0x")); log_->print(opcode, HEX);
                log_->println(F(": module reports OVERRUN (a command written while busy was dropped)"));
                overrun_logged = true;
            }
            if ((st & 0x01u) == 0) return true;
        }
        if (millis() - t0 >= timeout_ms) {
            if (log_) {
                log_->print(F("[rbamp] command 0x")); log_->print(opcode, HEX);
                log_->print(F(": still busy after ")); log_->print(timeout_ms); log_->println(F(" ms"));
            }
            setError(RB_ERR_TIMEOUT);
            return false;
        }
        delay(10);
    }
}

/* Synchronous command — the one completion rule for every STD command.
 * Map 0x35+: wait until the module is idle (a COMMAND written while BUSY is
 * dropped), write the opcode, poll the command state until BUSY clears (the
 * module raises BUSY when it accepts the write; from image 0x79 ERROR holds
 * PENDING for the whole run, earlier images may show 0 meanwhile — so ERROR is
 * only read after BUSY cleared), then take the verdict. PENDING right after
 * BUSY cleared is re-read once. No fixed sleep: the generated settle only caps
 * the first poll. Before map 0x35 there is no command state, and the settle is
 * the completion signal. */
bool RbAmp::stdCmdSync(uint8_t opcode, uint32_t settle_ms, uint32_t timeout_ms) noexcept {
    if (std_version_ < 0x35) {
        if (!stdCmd(opcode)) return false;
        delay(settle_ms);
        if (!stdCheckCmdError()) return false;
        setError(RB_OK);
        return true;
    }
    if (timeout_ms < kStdCmdMinTimeoutMs) timeout_ms = kStdCmdMinTimeoutMs;
    if (!stdWaitIdleImpl(timeout_ms, opcode)) return false;
    uint16_t seq0 = 0;
    const bool have_seq = stdReadU16(rbamp_std::REG_CMD_SEQ, seq0);
    if (!stdCmd(opcode)) return false;
    delay(settle_ms < kStdCmdFirstPollMs ? settle_ms : kStdCmdFirstPollMs);
    if (!stdWaitIdleImpl(timeout_ms, opcode)) return false;
    if (!stdCheckCmdError()) {
        if (last_error_ != RB_ERR_TIMEOUT) return false;
        delay(10);
        if (!stdCheckCmdError()) return false;
    }
    uint16_t seq1 = 0;
    if (have_seq && stdReadU16(rbamp_std::REG_CMD_SEQ, seq1) && seq1 == seq0 && log_) {
        log_->print(F("[rbamp] command 0x")); log_->print(opcode, HEX);
        log_->println(F(": completed-command counter did not move (module did not count it)"));
    }
    setError(RB_OK);
    return true;
}

/* A persisting command (SAVE_CAL / SAVE_CONFIG): the module writes its store
 * at most once per 5 s and answers NVM_RATE_LIMIT in between — wait that out
 * once rather than fail a commissioning sequence. */
bool RbAmp::stdSaveSync(uint8_t opcode, uint32_t settle_ms) noexcept {
    if (stdCmdSync(opcode, settle_ms, kStdSaveTimeoutMs)) return true;
    if (last_error_ != RB_ERR_NOT_READY) return false;
    uint8_t e = 0;
    if (!stdReadU8(rbamp_std::REG_ERROR, e) || e != rbamp_std::DEV_ERR_NVM_RATE_LIMIT) {
        setError(RB_ERR_NOT_READY);
        return false;
    }
    if (log_) log_->println(F("[rbamp] save rate-limited by the module — retrying in 5 s"));
    delay(5000);
    return stdCmdSync(opcode, settle_ms, kStdSaveTimeoutMs);
}

bool RbAmp::stdCommand(uint8_t opcode, uint32_t settle_ms, uint32_t timeout_ms) noexcept {
    if (!std_) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    if (timeout_ms == 0) timeout_ms = kStdCmdMinTimeoutMs;
    return stdCmdSync(opcode, settle_ms, timeout_ms);
}

/* Quiet wait for the identity block to answer at @p addr (after a reset or an
 * address change): the NACKs meanwhile are expected, so neither the log nor
 * the retry diagnostics see them. */
bool RbAmp::stdWaitReappear(uint8_t addr, uint32_t timeout_ms) noexcept {
    const uint32_t t0 = millis();
    for (;;) {
        uint8_t id[2] = {0, 0};
        if (rawStdRead(bus_, addr, rbamp_std::REG_ID, id, sizeof id) && id[0] == 'S' && id[1] == 'T') return true;
        if (millis() - t0 >= timeout_ms) return false;
        delay(100);
    }
}

bool RbAmp::stdWaitIdle(uint32_t timeout_ms) noexcept {
    if (!std_) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    if (!stdWaitIdleImpl(timeout_ms ? timeout_ms : 1000u, 0)) return false;
    setError(RB_OK);
    return true;
}

bool RbAmp::stdLocate() noexcept {
    if (!stdGate35()) return false;
    return stdCmdSync(rbamp_std::CMD_IDENTIFY, rbamp_std::SETTLE_MS_IDENTIFY, 1000);
}

bool RbAmp::stdHistFlush() noexcept {
    if (!stdGate35()) return false;
    return stdCmdSync(rbamp_std::CMD_HIST_FLUSH, rbamp_std::SETTLE_MS_HIST_FLUSH, 2000);
}

/* ============================================================================
 * STD line — life-time energy counters and the counter period path
 * ============================================================================ */

/* Modules without latch banks keep persisted life-time counters instead
 * (monotonic, survive power loss). The ENERGY fields and LT_GEN sit in one
 * burst; UPTIME is read separately. */
bool RbAmp::stdReadEnergyRaw(RbAmpStdEnergy& out) noexcept {
    if (!(std_caps_ & rbamp_std::CAP_ONDEVICE_ENERGY)) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    static_assert(rbamp_std::REG_LT_GEN >= rbamp_std::ENERGY_BASE + rbamp_std::ENERGY_E_REA_EXP + 8
                  && rbamp_std::REG_LT_GEN + 4 <= rbamp_std::ENERGY_BASE + rbamp_std::ENERGY_UPTIME,
                  "ENERGY block / LT_GEN layout changed - split the burst");
    static_assert(rbamp_std::ENERGY_E_ACT_NET_MWH + 8 <= rbamp_std::REG_LT_GEN - rbamp_std::ENERGY_BASE,
                  "mWh fields must sit inside the ENERGY burst");
    uint8_t b[rbamp_std::REG_LT_GEN + 4 - rbamp_std::ENERGY_BASE];
    if (!stdReadCoherent(rbamp_std::ENERGY_BASE, b, sizeof b)) return false;   /* one transaction = one module snapshot */
    uint32_t up = 0;
    if (!stdReadU32(rbamp_std::ENERGY_BASE + rbamp_std::ENERGY_UPTIME, up)) return false;
    if (up == 0xFFFFFFFFu) { setError(RB_ERR_NOT_READY); return false; }   /* unprogrammed / unmapped block */
    out.act_import_wh   = leI64(b + rbamp_std::ENERGY_E_ACT_IMP);
    out.act_export_wh   = leI64(b + rbamp_std::ENERGY_E_ACT_EXP);
    out.act_net_wh      = leI64(b + rbamp_std::ENERGY_E_ACT_NET);
    out.rea_import_varh = leI64(b + rbamp_std::ENERGY_E_REA_IMP);
    out.rea_export_varh = leI64(b + rbamp_std::ENERGY_E_REA_EXP);
    out.generation      = leU32(b + (rbamp_std::REG_LT_GEN - rbamp_std::ENERGY_BASE));
    out.uptime_s        = up;
    out.channels        = 0;
    out.group_status    = 0;
    out.act_import_mwh  = 0;
    out.act_export_mwh  = 0;
    out.act_net_mwh     = 0;
    out.fine            = false;
    if (std_version_ >= 0x36) {
        /* Map 0x36: mWh totals in the same block, per-channel counters in their own. */
        out.act_import_mwh = leI64(b + rbamp_std::ENERGY_E_ACT_IMP_MWH);
        out.act_export_mwh = leI64(b + rbamp_std::ENERGY_E_ACT_EXP_MWH);
        out.act_net_mwh    = leI64(b + rbamp_std::ENERGY_E_ACT_NET_MWH);
        uint8_t n = channels_;
        if (n > rbamp_std::GROUP_ENERGY_MWH_MAX) n = rbamp_std::GROUP_ENERGY_MWH_MAX;
        uint8_t g[RBAMP_MAX_CHANNELS * rbamp_std::GROUP_ENERGY_MWH_STRIDE];
        if (!stdReadCoherent(rbamp_std::GROUP_ENERGY_MWH_BASE, g, static_cast<size_t>(n) * rbamp_std::GROUP_ENERGY_MWH_STRIDE)) return false;
        for (uint8_t c = 0; c < n; ++c) {
            const uint8_t* q = g + static_cast<size_t>(c) * rbamp_std::GROUP_ENERGY_MWH_STRIDE;
            out.ch_import_mwh[c] = leI64(q + rbamp_std::GROUP_ENERGY_MWH_E_IMP);
            out.ch_export_mwh[c] = leI64(q + rbamp_std::GROUP_ENERGY_MWH_E_EXP);
        }
        out.channels = n;
        if (!stdReadU8(rbamp_std::REG_LTG_STATUS, out.group_status)) return false;
    }
    /* Micro-resolution fields are always filled: derived from the finest coarse
     * counters first (mWh from map 0x36, whole Wh before; varh), then replaced
     * by the module's own µWh / µvarh counters when it publishes them. */
    if (std_version_ >= 0x36) {
        out.act_import_uwh = out.act_import_mwh * 1000;
        out.act_export_uwh = out.act_export_mwh * 1000;
    } else {
        out.act_import_uwh = out.act_import_wh * 1000000;
        out.act_export_uwh = out.act_export_wh * 1000000;
    }
    out.act_net_uwh      = out.act_import_uwh - out.act_export_uwh;
    out.rea_import_uvarh = out.rea_import_varh * 1000000;
    out.rea_export_uvarh = out.rea_export_varh * 1000000;
    for (uint8_t c = 0; c < RBAMP_MAX_CHANNELS; ++c) {
        out.ch_import_uwh[c] = out.ch_import_mwh[c] * 1000;
        out.ch_export_uwh[c] = out.ch_export_mwh[c] * 1000;
    }
    if ((std_caps_ & rbamp_std::CAP_ENERGY_UWH) == 0) return true;

    /* One transaction for the µ block (ENERGY +0xD0..+0xF7): IMP / EXP / NET
     * µWh, then reactive IMP / EXP µvarh (published from image 0x7B; on 0x7A
     * those bytes are not counters, keep the derived values). */
    static_assert(rbamp_std::ENERGY_E_ACT_EXP_UWH == rbamp_std::ENERGY_E_ACT_IMP_UWH + 8
                  && rbamp_std::ENERGY_E_ACT_NET_UWH == rbamp_std::ENERGY_E_ACT_IMP_UWH + 16
                  && rbamp_std::ENERGY_E_REA_IMP_UVARH == rbamp_std::ENERGY_E_ACT_IMP_UWH + 24
                  && rbamp_std::ENERGY_E_REA_EXP_UVARH == rbamp_std::ENERGY_E_ACT_IMP_UWH + 32,
                  "micro-resolution ENERGY fields moved - split the burst");
    uint8_t u[40];
    if (!stdReadCoherent(rbamp_std::ENERGY_BASE + rbamp_std::ENERGY_E_ACT_IMP_UWH, u, sizeof u)) return false;
    out.act_import_uwh = leI64(u + 0);
    out.act_export_uwh = leI64(u + 8);
    out.act_net_uwh    = leI64(u + 16);
    if (std_fw_ver_ >= kStdFwStartUa) {
        out.rea_import_uvarh = leI64(u + 24);
        out.rea_export_uvarh = leI64(u + 32);
    }
    /* Per-channel µWh block (0x0C80 + c·16): one transaction as well. */
    uint8_t n = channels_;
    if (n > rbamp_std::GROUP_ENERGY_UWH_MAX) n = rbamp_std::GROUP_ENERGY_UWH_MAX;
    static_assert(rbamp_std::GROUP_ENERGY_UWH_MAX * rbamp_std::GROUP_ENERGY_UWH_STRIDE <= 128, "per-channel uWh block must fit one 128-B snapshot read");
    uint8_t g[RBAMP_MAX_CHANNELS * rbamp_std::GROUP_ENERGY_UWH_STRIDE];
    if (!stdReadCoherent(rbamp_std::GROUP_ENERGY_UWH_BASE, g, static_cast<size_t>(n) * rbamp_std::GROUP_ENERGY_UWH_STRIDE)) return false;
    for (uint8_t c = 0; c < n; ++c) {
        const uint8_t* q = g + static_cast<size_t>(c) * rbamp_std::GROUP_ENERGY_UWH_STRIDE;
        out.ch_import_uwh[c] = leI64(q + rbamp_std::GROUP_ENERGY_UWH_E_IMP);
        out.ch_export_uwh[c] = leI64(q + rbamp_std::GROUP_ENERGY_UWH_E_EXP);
    }
    out.fine = true;
    return true;
}

/* Take the counter-path anchor: the reading every later period is measured from. */
bool RbAmp::stdLtAnchor() noexcept {
    memset(&std_lt_anchor_, 0, sizeof std_lt_anchor_);
    if (!stdReadEnergyRaw(std_lt_anchor_)) return false;
    std_lt_anchored_  = true;
    prev_latch_ms_    = millis();
    current_latch_ms_ = prev_latch_ms_;
    have_prev_latch_  = true;
    setError(RB_OK);
    return true;
}

bool RbAmp::stdReadEnergy(RbAmpStdEnergy& out) noexcept {
    if (!std_) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    memset(&out, 0, sizeof out);
    if (!stdReadEnergyRaw(out)) return false;
    setError(RB_OK);
    return true;
}

bool RbAmp::stdEnergyDelta(const RbAmpStdEnergy& prev, const RbAmpStdEnergy& now,
                           RbAmpStdEnergy* delta, bool* module_restarted) noexcept {
    if (module_restarted) *module_restarted = now.uptime_s < prev.uptime_s;
    if (now.generation != prev.generation) return false;
    /* Power-cut resume rule: a counter may come back a little below the last
     * reading; that is lost energy, not a discontinuity — clamp to 0. */
#define RBAMP_DELTA0(f) (((now.f) > (prev.f)) ? (now.f) - (prev.f) : 0)
    if (delta) {
        memset(delta, 0, sizeof *delta);
        delta->act_import_wh   = RBAMP_DELTA0(act_import_wh);
        delta->act_export_wh   = RBAMP_DELTA0(act_export_wh);
        delta->act_net_wh      = delta->act_import_wh - delta->act_export_wh;
        delta->rea_import_varh = RBAMP_DELTA0(rea_import_varh);
        delta->rea_export_varh = RBAMP_DELTA0(rea_export_varh);
        delta->act_import_mwh  = RBAMP_DELTA0(act_import_mwh);
        delta->act_export_mwh  = RBAMP_DELTA0(act_export_mwh);
        delta->act_net_mwh     = delta->act_import_mwh - delta->act_export_mwh;
        delta->act_import_uwh  = RBAMP_DELTA0(act_import_uwh);
        delta->act_export_uwh  = RBAMP_DELTA0(act_export_uwh);
        delta->act_net_uwh     = delta->act_import_uwh - delta->act_export_uwh;
        delta->rea_import_uvarh = RBAMP_DELTA0(rea_import_uvarh);
        delta->rea_export_uvarh = RBAMP_DELTA0(rea_export_uvarh);
        delta->fine            = now.fine && prev.fine;
        delta->generation      = now.generation;
        delta->uptime_s        = now.uptime_s;
        delta->group_status    = now.group_status;
        /* Per-channel: both readings must carry the same channels. A store
         * status of "restarted at 0" is NOT a reason to drop them: the
         * restart itself shows as a generation change or an uptime drop, and
         * a counter below the previous reading is clamped like the totals. */
        if (now.channels && now.channels == prev.channels) {
            delta->channels = now.channels;
            for (uint8_t c = 0; c < now.channels && c < RBAMP_MAX_CHANNELS; ++c) {
                delta->ch_import_mwh[c] = RBAMP_DELTA0(ch_import_mwh[c]);
                delta->ch_export_mwh[c] = RBAMP_DELTA0(ch_export_mwh[c]);
                delta->ch_import_uwh[c] = RBAMP_DELTA0(ch_import_uwh[c]);
                delta->ch_export_uwh[c] = RBAMP_DELTA0(ch_export_uwh[c]);
            }
        }
    }
#undef RBAMP_DELTA0
    return true;
}

/* STD arm of readPeriodSnapshot() for modules with persisted counters and no
 * latch banks: a period is the difference of two counter readings on the
 * master's schedule. Whole-Wh resolution below map 0x36, so the caller's
 * period should be minutes, not seconds. */
bool RbAmp::stdReadPeriodCounters(RbAmpPeriodSnapshot& out) noexcept {
    if (!std_lt_anchored_) {
        if (!stdLtAnchor()) return false;
        setError(RB_ERR_STALE);   /* first read only anchors */
        return false;
    }
    const uint32_t now_ms = millis();
    const uint32_t dt_ms  = now_ms - prev_latch_ms_;

    RbAmpStdEnergy cur, d;
    bool restarted = false;
    memset(&cur, 0, sizeof cur);
    if (!stdReadEnergyRaw(cur)) return false;
    if (!stdEnergyDelta(std_lt_anchor_, cur, &d, &restarted)) {
        if (log_) {
            log_->print(F("[rbamp] period: life-time counters discontinuous (generation "));
            log_->print(std_lt_anchor_.generation); log_->print(F(" -> ")); log_->print(cur.generation);
            log_->println(F(") — re-anchored, this period is skipped"));
        }
        std_lt_anchor_ = cur;
        prev_latch_ms_ = now_ms;
        setError(RB_ERR_STALE);
        return false;
    }
    if (restarted && log_) {
        log_->print(F("[rbamp] period: module restarted (uptime now ")); log_->print(cur.uptime_s);
        log_->println(F(" s) — counters persisted, period kept; re-apply its clock and RAM settings"));
    }
    if ((cur.act_import_wh < std_lt_anchor_.act_import_wh || cur.act_import_uwh < std_lt_anchor_.act_import_uwh) && log_) {
        log_->println(F("[rbamp] period: a counter resumed below the previous reading (power cut?) — that part is lost, the difference is clamped to 0"));
    }
    /* The µWh fields carry the finest counters the module has (its own from
     * CAP_ENERGY_UWH, else mWh from map 0x36, else whole Wh); per-channel
     * counters exist from map 0x36. */
    const bool   mwh    = std_version_ >= 0x36;
    const double net_wh = static_cast<double>(d.act_net_uwh) / 1e6;
    const float  avg    = dt_ms ? static_cast<float>(net_wh * 3.6e6 / static_cast<double>(dt_ms)) : 0.0f;
    for (uint8_t ch = 0; ch < RBAMP_MAX_CHANNELS; ++ch) {
        if (ch >= channels_)        { out.avg_p[ch] = 0.0f; continue; }
        if (channels_ == 1)         { out.avg_p[ch] = avg;  continue; }   /* one channel = the total */
        if (mwh && d.channels > ch) {
            const double ch_wh = static_cast<double>(d.ch_import_uwh[ch] - d.ch_export_uwh[ch]) / 1e6;
            out.avg_p[ch] = dt_ms ? static_cast<float>(ch_wh * 3.6e6 / static_cast<double>(dt_ms)) : 0.0f;
        } else {
            out.avg_p[ch] = NAN;    /* no usable per-channel counters for this period */
        }
    }
    if (mwh && channels_ > 1 && d.channels == 0 && log_) {
        log_->println(F("[rbamp] period: per-channel counters unavailable for this period (channel count changed)"));
    }
    out.max_p            = NAN;     /* no period envelope on the counter path */
    out.latch_ms         = 0;
    out.master_dt_ms     = dt_ms;
    out.valid            = true;
    out.avg_p_total      = avg;
    out.wh_net           = net_wh;
    out.module_restarted = restarted;

    energy_.addWh(RBAMP_ENERGY_CH_TOTAL, net_wh);
    if (channels_ == 1) {
        energy_.addWh(0, net_wh);
    } else if (mwh && d.channels) {
        for (uint8_t ch = 0; ch < d.channels && ch < RBAMP_MAX_CHANNELS; ++ch) {
            energy_.addWh(ch, static_cast<double>(d.ch_import_uwh[ch] - d.ch_export_uwh[ch]) / 1e6);
        }
    }
    std_lt_anchor_      = cur;
    prev_latch_ms_      = now_ms;
    current_latch_ms_   = now_ms;
    have_prev_latch_    = true;
    std_last_avg_total_ = avg;
    std_have_last_avg_  = true;
    setError(RB_OK);
    return true;
}

/* ============================================================================
 * STD line — identity, board id, clock, address by UID (map 0x35+ / 0x36+)
 * ============================================================================ */

bool RbAmp::stdIdentify(RbAmpStdIdentity& out) noexcept {
    if (!std_) { setError(RB_ERR_NOT_IMPLEMENTED); return false; }
    memset(&out, 0, sizeof out);
    out.i2c_addr     = addr_;
    out.map_version  = std_version_;
    out.capabilities = std_caps_;
    out.variant      = std_variant_;
    out.n_voltage    = n_voltage_;
    out.n_current    = n_current_;
    if (!readUid(out.uid)) return false;
    if (std_version_ >= 0x35) {
        if (!stdReadU32(rbamp_std::REG_FW_VER, out.fw_version)) return false;
        if (out.fw_version == 0xFFFFFFFFu) out.fw_version = 0;
    }
    if (std_version_ >= 0x36) {
        if (!stdReadU32(rbamp_std::REG_BOARD_ID, out.board_id)) return false;
    }
    setError(RB_OK);
    return true;
}

int8_t RbAmp::stdProbe(TwoWire& bus, uint8_t addr, RbAmpStdIdentity& out) noexcept {
    memset(&out, 0, sizeof out);
    if (addr < 0x08 || addr > 0x77) return RB_ERR_PARAM;
    out.i2c_addr = addr;
    uint8_t b[rbamp_std::REG_VARIANT + 1];   /* ID .. VARIANT in one burst */
    if (!rawStdRead(bus, addr, rbamp_std::REG_ID, b, sizeof b)) return RB_ERR_NACK;   /* nothing acknowledges there */
    if (b[0] != 'S' || b[1] != 'T') return RB_ERR_VERSION;                            /* something else lives at this address */
    out.map_version  = b[rbamp_std::REG_VERSION];
    out.capabilities = leU16(b + rbamp_std::REG_CAPABILITIES);
    out.n_voltage    = b[rbamp_std::REG_N_VOLTAGE];
    out.n_current    = b[rbamp_std::REG_N_CURRENT];
    out.variant      = b[rbamp_std::REG_VARIANT];
    if (!rawStdRead(bus, addr, rbamp_std::REG_UID, out.uid, rbamp_std::REG_UID_SIZE)) return RB_ERR_IO;
    uint8_t v[4];
    if (out.map_version >= 0x35) {
        if (!rawStdRead(bus, addr, rbamp_std::REG_FW_VER, v, sizeof v)) return RB_ERR_IO;
        out.fw_version = (leU32(v) == 0xFFFFFFFFu) ? 0 : leU32(v);
    }
    if (out.map_version >= 0x36) {
        if (!rawStdRead(bus, addr, rbamp_std::REG_BOARD_ID, v, sizeof v)) return RB_ERR_IO;
        out.board_id = leU32(v);
    }
    return RB_OK;
}

bool RbAmp::stdReadImageVersion(uint32_t& out) noexcept {
    if (!stdGate35()) return false;
    if (!stdReadU32(rbamp_std::REG_FW_VER, out)) return false;
    if (out == 0xFFFFFFFFu) out = 0;
    setError(RB_OK);
    return true;
}

bool RbAmp::stdReadBoardId(uint32_t& out) noexcept {
    if (!stdGate36()) return false;
    if (!stdReadU32(rbamp_std::REG_BOARD_ID, out)) return false;
    setError(RB_OK);
    return true;
}

bool RbAmp::stdOtaBoardOk(uint32_t image_board_word) noexcept {
    uint32_t mine = 0;
    if (!stdReadBoardId(mine)) return false;
    if (mine != image_board_word) {
        if (log_) {
            log_->print(F("[rbamp] ota: image is for board 0x")); log_->print(image_board_word, HEX);
            log_->print(F(", this module is 0x")); log_->print(mine, HEX); log_->println(F(" — not sent"));
        }
        setError(RB_ERR_PARAM);
        return false;
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::stdSetTime(uint32_t unix_epoch) noexcept {
    if (!stdGate35()) return false;
    if (!stdWriteU32(rbamp_std::REG_TIME_SET, unix_epoch)) return false;
    if (!stdCmdSync(rbamp_std::CMD_SET_TIME, rbamp_std::SETTLE_MS_SET_TIME, 1000)) {
        if (last_error_ == RB_ERR_CHECKSUM) {
            /* The module reuses the value-check code for "epoch before 2020". */
            if (log_) { log_->print(F("[rbamp] set_time: module refused epoch ")); log_->println(unix_epoch); }
            setError(RB_ERR_PARAM);
        }
        return false;
    }
    /* Some images publish TIME_VALID at their next publish pass (one
     * measurement window), later ones at the verdict: re-read tolerantly for
     * up to 300 ms (covers the 200 ms window of images up to 0x7B). */
    uint8_t st = 0;
    const uint32_t t0 = millis();
    do {
        if (!stdReadU8(rbamp_std::REG_STATUS, st)) return false;
        if (st & rbamp_std::ST_TIME_VALID) { setError(RB_OK); return true; }
        delay(20);
    } while (millis() - t0 < 300);
    if (log_) { log_->print(F("[rbamp] set_time: accepted but TIME_VALID still clear (status 0x")); log_->print(st, HEX); log_->println(')'); }
    setError(RB_ERR_NOT_READY);
    return false;
}

bool RbAmp::stdReadTime(uint32_t& unix_epoch, bool& valid) noexcept {
    if (!stdGate35()) return false;
    uint8_t st = 0;
    if (!stdReadU32(rbamp_std::ENERGY_BASE + rbamp_std::ENERGY_RTC_EPOCH, unix_epoch)) return false;
    if (!stdReadU8(rbamp_std::REG_STATUS, st)) return false;
    valid = (st & rbamp_std::ST_TIME_VALID) != 0;
    setError(RB_OK);
    return true;
}

int8_t RbAmp::stdSetAddressByUid(TwoWire& bus, uint8_t shared_addr, const uint8_t uid[12],
                                 uint8_t new_addr, uint32_t restart_ms) noexcept {
    if (shared_addr < 0x08 || shared_addr > 0x77 || new_addr < 0x08 || new_addr > 0x77) return RB_ERR_PARAM;
    static_assert(rbamp_std::REG_ADDR_UID_I2C == rbamp_std::REG_ADDR_UID + rbamp_std::REG_ADDR_UID_SIZE
                  && rbamp_std::REG_ADDR_UID_UNIT == rbamp_std::REG_ADDR_UID_I2C + 1,
                  "ADDR_UID / ADDR_UID_I2C / ADDR_UID_UNIT must be contiguous");
    /* Write-only on the shared address: UID + new I2C address + unit 0 (keep)
     * in one burst, then the command. No read here — several modules may
     * answer and their data would collide. No retry layer either: a NACK on a
     * shared address means nobody is there. */
    uint8_t w[2 + rbamp_std::REG_ADDR_UID_SIZE + 2];
    w[0] = static_cast<uint8_t>(rbamp_std::REG_ADDR_UID >> 8);
    w[1] = static_cast<uint8_t>(rbamp_std::REG_ADDR_UID);
    memcpy(w + 2, uid, rbamp_std::REG_ADDR_UID_SIZE);
    w[2 + rbamp_std::REG_ADDR_UID_SIZE]     = new_addr;
    w[2 + rbamp_std::REG_ADDR_UID_SIZE + 1] = 0;
    if (!rawStdWrite(bus, shared_addr, w, sizeof w)) return RB_ERR_NACK;
    const uint8_t c[3] = { static_cast<uint8_t>(rbamp_std::REG_COMMAND >> 8), static_cast<uint8_t>(rbamp_std::REG_COMMAND),
                           static_cast<uint8_t>(rbamp_std::CMD_ADDR_BY_UID) };
    if (!rawStdWrite(bus, shared_addr, c, sizeof c)) return RB_ERR_NACK;
    delay(rbamp_std::SETTLE_MS_ADDR_BY_UID + (restart_ms ? restart_ms : 1500u));

    /* Probe the new address: identity block first, then the UID must match. */
    uint8_t id[rbamp_std::REG_ID_SIZE] = {0, 0};
    uint8_t got[rbamp_std::REG_UID_SIZE] = {0};
    bool ok = false;
    for (uint8_t i = 0; i < 5 && !ok; ++i) {
        ok = rawStdRead(bus, new_addr, rbamp_std::REG_ID, id, sizeof id);
        if (!ok) delay(200);
    }
    if (ok) ok = rawStdRead(bus, new_addr, rbamp_std::REG_UID, got, sizeof got);
    if (!ok) return RB_ERR_NOT_READY;                                              /* nothing answers after the restart */
    if (id[0] != 'S' || id[1] != 'T' || memcmp(got, uid, sizeof got) != 0) return RB_ERR_VERSION;   /* not the one addressed */
    return RB_OK;
}

/* ============================================================================
 * STD line — history rings and the event log (map 0x35+)
 * ============================================================================ */

/* Select the ring on the module (period byte; the back index when a fetch follows). */
bool RbAmp::stdHistSelect(RbAmpStdHistPeriod period, uint16_t back, bool with_back) noexcept {
    if (static_cast<uint8_t>(period) > static_cast<uint8_t>(RbAmpStdHistPeriod::Events)) { setError(RB_ERR_PARAM); return false; }
    if (!stdWriteU8(rbamp_std::REG_HIST_PERIOD, static_cast<uint8_t>(period))) return false;
    if (!with_back) return true;
    const uint8_t b[2] = { static_cast<uint8_t>(back), static_cast<uint8_t>(back >> 8) };
    return stdWrite(rbamp_std::REG_HIST_BACK, b, sizeof b);
}

bool RbAmp::stdHistInfo(RbAmpStdHistPeriod period, RbAmpStdHistInfo& out) noexcept {
    if (!stdGate35()) return false;
    memset(&out, 0, sizeof out);
    /* The module fills COUNT / CAP / RECSZ / OLDEST / NEWEST for the selected
     * ring only when it serves a HIST_READ — selecting the period alone
     * leaves them 0. Fetch the newest record as the selector; "no record"
     * verdicts are fine here. */
    if (!stdHistSelect(period, 0, true)) return false;
    if (!stdCmdSync(rbamp_std::CMD_HIST_READ, rbamp_std::SETTLE_MS_HIST_READ, 1000)
            && last_error_ != RB_ERR_PARAM && last_error_ != RB_ERR_CHECKSUM) {
        return false;
    }
    static_assert(rbamp_std::REG_HIST_NEWEST == rbamp_std::REG_HIST_COUNT + 10, "HIST block layout changed");
    uint8_t b[14];   /* COUNT u16 · CAP u16 · RECSZ u16 · OLDEST u32 · NEWEST u32 */
    if (!stdRead(rbamp_std::REG_HIST_COUNT, b, sizeof b)) return false;
    out.count       = leU16(b);
    out.capacity    = leU16(b + 2);
    out.record_size = leU16(b + 4);
    out.oldest_ts   = leU32(b + 6);
    out.newest_ts   = leU32(b + 10);
    setError(RB_OK);
    return true;
}

bool RbAmp::stdHistReadRaw(RbAmpStdHistPeriod period, uint16_t back,
                           uint8_t rec[rbamp_std::REG_HIST_REC_SIZE], uint8_t* flags) noexcept {
    if (!stdGate35()) return false;
    if (rec == nullptr) { setError(RB_ERR_PARAM); return false; }
    if (!stdHistSelect(period, back, true)) return false;
    if (!stdCmdSync(rbamp_std::CMD_HIST_READ, rbamp_std::SETTLE_MS_HIST_READ, 1000)) {
        /* No such record: the module answers PARAM (index beyond the ring) or
         * the value-check code (empty ring) — both mean "not there". */
        if (last_error_ == RB_ERR_PARAM || last_error_ == RB_ERR_CHECKSUM) setError(RB_ERR_NOT_READY);
        return false;
    }
    uint8_t f = 0;
    if (!stdReadU8(rbamp_std::REG_HIST_FLAGS, f)) return false;
    if (flags) *flags = f;
    if ((f & 0x01u) == 0) { setError(RB_ERR_NOT_READY); return false; }
    if (!stdReadCoherent(rbamp_std::REG_HIST_REC, rec, rbamp_std::REG_HIST_REC_SIZE)) return false;
    setError(RB_OK);
    return true;
}

/* Energy-period record: fixed part per the generated HIST_REC offsets, then
 * per phase imp u32 · exp u32 (0.01 Wh, this period); the phase count follows
 * the module's record size (56 B + 8 B per phase). */
bool RbAmp::stdHistRead(RbAmpStdHistPeriod period, uint16_t back, RbAmpStdHistRecord& out) noexcept {
    static_assert(rbamp_std::HIST_REC_PH_EXP_CWH == rbamp_std::HIST_REC_PH_IMP_CWH + 4 * RBAMP_STD_HIST_PHASES,
                  "HIST_REC per-phase layout changed");
    constexpr uint16_t kFixedLen = rbamp_std::HIST_REC_PH_IMP_CWH;
    memset(&out, 0, sizeof out);
    if (period == RbAmpStdHistPeriod::Events) { setError(RB_ERR_PARAM); return false; }   /* stdEventRead() */
    uint8_t rec[rbamp_std::REG_HIST_REC_SIZE];
    uint8_t f = 0;
    if (!stdHistReadRaw(period, back, rec, &f)) return false;
    uint16_t recsz = 0;
    if (!stdReadU16(rbamp_std::REG_HIST_RECSZ, recsz)) return false;
    if (recsz < kFixedLen || recsz > rbamp_std::REG_HIST_REC_SIZE) { setError(RB_ERR_NON_PHYSICAL); return false; }
    out.ts       = leU32(rec + rbamp_std::HIST_REC_TS);
    out.flags    = leU16(rec + rbamp_std::HIST_REC_FLAGS);
    out.n        = leU16(rec + rbamp_std::HIST_REC_N);
    out.from_ram = (f & 0x02u) != 0;
    out.v_rms    = static_cast<float>(leU16(rec + rbamp_std::HIST_REC_V_DV)) * 0.1f;
    out.i_total  = static_cast<float>(leU32(rec + rbamp_std::HIST_REC_I_MA)) * 1e-3f;
    out.p_avg    = static_cast<float>(leI32(rec + rbamp_std::HIST_REC_P_W));
    out.q_avg    = static_cast<float>(leI32(rec + rbamp_std::HIST_REC_Q_VAR));
    out.s_avg    = static_cast<float>(leI32(rec + rbamp_std::HIST_REC_S_VA));
    out.pf       = static_cast<float>(leI16(rec + rbamp_std::HIST_REC_PF)) / 1024.0f;
    out.p_min    = static_cast<float>(leI32(rec + rbamp_std::HIST_REC_MIN_P_W));
    out.p_max    = static_cast<float>(leI32(rec + rbamp_std::HIST_REC_MAX_P_W));
    out.i_min    = static_cast<float>(leU32(rec + rbamp_std::HIST_REC_MIN_I_MA)) * 1e-3f;
    out.i_max    = static_cast<float>(leU32(rec + rbamp_std::HIST_REC_MAX_I_MA)) * 1e-3f;
    out.e_import_wh = leU48(rec + rbamp_std::HIST_REC_E_IMP_WH);
    out.e_export_wh = leU48(rec + rbamp_std::HIST_REC_E_EXP_WH);
    uint8_t nph = static_cast<uint8_t>((recsz - kFixedLen) / 8u);
    if (nph > RBAMP_STD_HIST_PHASES) nph = RBAMP_STD_HIST_PHASES;
    out.phases = nph;
    for (uint8_t ph = 0; ph < nph; ++ph) {
        out.ph_import_wh[ph] = static_cast<double>(leU32(rec + kFixedLen + 4u * ph)) / 100.0;
        out.ph_export_wh[ph] = static_cast<double>(leU32(rec + kFixedLen + 4u * nph + 4u * ph)) / 100.0;
    }
    setError(RB_OK);
    return true;
}

/* Event record (packed, LE, 32 B): ts u32 · uptime u32 · type u8 · flags u8 ·
 * boot u16 · a_before u32 · a_after u32 · e_before u48 · e_after u48. */
bool RbAmp::stdEventRead(uint16_t back, RbAmpStdEvent& out) noexcept {
    memset(&out, 0, sizeof out);
    uint8_t rec[rbamp_std::REG_HIST_REC_SIZE];
    if (!stdHistReadRaw(RbAmpStdHistPeriod::Events, back, rec, nullptr)) return false;
    out.ts          = leU32(rec + rbamp_std::EVENT_REC_TS);
    out.uptime_s    = leU32(rec + rbamp_std::EVENT_REC_UPTIME_S);
    out.type        = rec[rbamp_std::EVENT_REC_TYPE];
    out.flags       = rec[rbamp_std::EVENT_REC_FLAGS];
    out.boot        = leU16(rec + rbamp_std::EVENT_REC_BOOT);
    out.a_before    = leU32(rec + rbamp_std::EVENT_REC_A_BEFORE);
    out.a_after     = leU32(rec + rbamp_std::EVENT_REC_A_AFTER);
    out.e_before_wh = leU48(rec + rbamp_std::EVENT_REC_E_BEFORE);
    out.e_after_wh  = leU48(rec + rbamp_std::EVENT_REC_E_AFTER);
    setError(RB_OK);
    return true;
}

/* ============================================================================
 * Data-ready (DRDY) — both product lines
 *
 * One interrupt per GPIO; the ISR only counts the edge and stamps it with
 * micros() (no bus traffic, no instance access). Every instance attached to
 * that GPIO compares the line's edge count with the count it last consumed.
 * On a wired-OR line a pulse may be another module's: the wait confirms it
 * was ours by the module's own sequence. STD modules before image 0x00010075
 * toggle the pin differently: treated as "no line" (polling) so a stale
 * level never looks like data.
 * ============================================================================ */

#ifndef RBAMP_DRDY_MAX_LINES
#define RBAMP_DRDY_MAX_LINES 4
#endif
#ifndef RBAMP_DRDY_MAX_SUBS
#define RBAMP_DRDY_MAX_SUBS 8
#endif
#if defined(ARDUINO_ARCH_ESP32)
#  if defined(ARDUINO_ISR_ATTR)
#    define RBAMP_ISR_ATTR ARDUINO_ISR_ATTR
#  else
#    define RBAMP_ISR_ATTR IRAM_ATTR
#  endif
#else
#  define RBAMP_ISR_ATTR
#endif

namespace {
constexpr uint32_t kDrdyAlarmUs      = 5000;   /* still LOW this long after the edge = event pending (alarm level) */
constexpr uint32_t kDrdyPulseGuardUs = 100;    /* the data-ready pulse itself is <= ~50 us LOW: look only after it */
constexpr uint32_t kStdDrdyMinFw     = 0x00010075u;

struct DrdyLine {
    int               pin;            /* -1 = free slot */
    volatile uint32_t edges;
    volatile uint32_t last_edge_us;
    RbAmp*            subs[RBAMP_DRDY_MAX_SUBS];
};
DrdyLine s_drdy_lines[RBAMP_DRDY_MAX_LINES] = {
    { -1, 0, 0, {} }, { -1, 0, 0, {} }, { -1, 0, 0, {} }, { -1, 0, 0, {} },
};
static_assert(RBAMP_DRDY_MAX_LINES == 4, "one ISR trampoline per line slot — extend the table below");

/* Minimal ISRs: counter + timestamp. attachInterrupt() takes a plain function
 * pointer on every core, so one trampoline per line slot. */
void RBAMP_ISR_ATTR drdyIsr0() { s_drdy_lines[0].edges = s_drdy_lines[0].edges + 1u; s_drdy_lines[0].last_edge_us = micros(); }
void RBAMP_ISR_ATTR drdyIsr1() { s_drdy_lines[1].edges = s_drdy_lines[1].edges + 1u; s_drdy_lines[1].last_edge_us = micros(); }
void RBAMP_ISR_ATTR drdyIsr2() { s_drdy_lines[2].edges = s_drdy_lines[2].edges + 1u; s_drdy_lines[2].last_edge_us = micros(); }
void RBAMP_ISR_ATTR drdyIsr3() { s_drdy_lines[3].edges = s_drdy_lines[3].edges + 1u; s_drdy_lines[3].last_edge_us = micros(); }
void (*const kDrdyIsr[RBAMP_DRDY_MAX_LINES])() = { drdyIsr0, drdyIsr1, drdyIsr2, drdyIsr3 };

/* Atomic copy of the ISR-written pair (32-bit stores are not atomic on 8-bit cores). */
inline void drdySnapshot(const DrdyLine& line, uint32_t& edges, uint32_t& edge_us) {
    noInterrupts();
    edges   = line.edges;
    edge_us = line.last_edge_us;
    interrupts();
}
} /* namespace */

/* A module can hold the line (alarm class): BASIC always, STD from map 0x36. */
bool RbAmp::drdyCanAlarm() const noexcept { return !std_ || std_version_ >= 0x36; }

void RbAmp::drdyDetach() noexcept {
    if (drdy_line_ < 0) return;
    DrdyLine& line = s_drdy_lines[drdy_line_];
    bool empty = true;
    noInterrupts();
    for (uint8_t i = 0; i < RBAMP_DRDY_MAX_SUBS; ++i) {
        if (line.subs[i] == this) line.subs[i] = nullptr;
        if (line.subs[i]) empty = false;
    }
    interrupts();
    drdy_line_ = -1;
    if (empty) {
        detachInterrupt(digitalPinToInterrupt(line.pin));
        line.pin = -1;
    }
}

bool RbAmp::setDrdyPin(int pin) noexcept {
    drdyDetach();
    if (pin < 0) { setError(RB_OK); return true; }
    if (digitalPinToInterrupt(pin) < 0) { setError(RB_ERR_PARAM); return false; }   /* no interrupt on this pin */
    if (std_ && std_fw_ver_ && std_fw_ver_ < kStdDrdyMinFw) {
        if (log_) {
            log_->print(F("[rbamp] drdy: module image 0x")); log_->print(std_fw_ver_, HEX);
            log_->println(F(" drives DRDY differently — polling instead"));
        }
        setError(RB_ERR_NOT_IMPLEMENTED);
        return false;
    }
    /* Find the line (existing pin or a free slot). */
    int8_t idx = -1, freeslot = -1;
    for (int8_t i = 0; i < RBAMP_DRDY_MAX_LINES; ++i) {
        if (s_drdy_lines[i].pin == pin) { idx = i; break; }
        if (s_drdy_lines[i].pin < 0 && freeslot < 0) freeslot = i;
    }
    const bool is_new = (idx < 0);
    if (is_new) {
        if (freeslot < 0) {
            if (log_) log_->println(F("[rbamp] drdy: no free line slot"));
            setError(RB_ERR_PARAM);
            return false;
        }
        idx = freeslot;
        DrdyLine& line = s_drdy_lines[idx];
        for (uint8_t i = 0; i < RBAMP_DRDY_MAX_SUBS; ++i) line.subs[i] = nullptr;
        line.edges        = 0;
        line.last_edge_us = 0;
        line.pin          = pin;
        pinMode(pin, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(pin), kDrdyIsr[idx], FALLING);
    }
    DrdyLine& line = s_drdy_lines[idx];
    bool placed = false;
    noInterrupts();
    for (uint8_t i = 0; i < RBAMP_DRDY_MAX_SUBS && !placed; ++i) {
        if (!line.subs[i]) { line.subs[i] = this; placed = true; }
    }
    interrupts();
    if (!placed) {
        if (is_new) { detachInterrupt(digitalPinToInterrupt(pin)); line.pin = -1; }
        if (log_) log_->println(F("[rbamp] drdy: line already has the maximum number of modules"));
        setError(RB_ERR_PARAM);
        return false;
    }
    drdy_line_ = idx;
    drdy_seen_edges_ = line.edges;   /* start clean */
    if (log_) {
        log_->print(F("[rbamp] drdy: 0x")); log_->print(addr_, HEX);
        log_->print(F(" listens on pin ")); log_->print(pin);
        log_->println(drdyCanAlarm() ? F(" (alarm level enabled)") : F(""));
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::readDataSeq(uint16_t& out) noexcept {
    if (std_) {
        if (!stdGate36()) return false;   /* no publish counter before map 0x36 */
        if (!stdReadU16(rbamp_std::REG_WIN_SEQ, out)) return false;
        setError(RB_OK);
        return true;
    }
    uint8_t s8 = 0;
    if (!readCommitSeq(s8)) return false;
    out = s8;
    return true;
}

/* Fresh if the sequence moved since the last consumed one; fills seq/missed. */
bool RbAmp::drdyCheckSeq(RbAmpDrdyEvent& ev, bool& fresh) noexcept {
    uint16_t seq = 0;
    if (!readDataSeq(seq)) return false;
    fresh = !drdy_have_seq_ || seq != drdy_last_seq_;
    if (fresh) {
        const uint16_t step = drdy_have_seq_
            ? (std_ ? static_cast<uint16_t>(seq - drdy_last_seq_)
                    : static_cast<uint16_t>(static_cast<uint8_t>(seq - drdy_last_seq_)))
            : 1u;
        ev.seq    = seq;
        ev.missed = step ? static_cast<uint16_t>(step - 1u) : 0u;
        drdy_last_seq_ = seq;
        drdy_have_seq_ = true;
    }
    return true;
}

/* The line is held LOW: some module on it has a masked event. A held line
 * hides every other module's pulse, so clear the flags of ALL modules on it;
 * this instance keeps its own, the others are parked on their instances. */
void RbAmp::drdyServiceAlarm(RbAmpDrdyEvent& ev) noexcept {
    if (drdy_line_ < 0) return;
    DrdyLine& line = s_drdy_lines[drdy_line_];
    for (uint8_t i = 0; i < RBAMP_DRDY_MAX_SUBS; ++i) {
        RbAmp* d = line.subs[i];
        if (!d || !d->drdyCanAlarm()) continue;
        uint8_t flags = 0;
        if (!d->readEventFlags(flags) || !flags) continue;
        (void)d->clearEventFlags(flags);
        if (d == this) {
            ev.alarm        = true;
            ev.event_flags |= flags;
        } else {
            d->drdy_pending_flags_ |= flags;
        }
    }
}

bool RbAmp::waitDataReady(uint32_t timeout_ms, RbAmpDrdyEvent* ev) noexcept {
    RbAmpDrdyEvent local;
    if (!ev) ev = &local;
    memset(ev, 0, sizeof *ev);
    if (std_ && !stdGate36()) return false;
    const uint32_t t0 = millis();

    if (drdy_line_ < 0) {
        /* Polling fallback. */
        for (;;) {
            if (drdy_pending_flags_) {
                ev->alarm = true; ev->event_flags |= drdy_pending_flags_; drdy_pending_flags_ = 0;
            }
            bool fresh = false;
            if (!drdyCheckSeq(*ev, fresh)) return false;
            if (fresh || ev->alarm) { ev->fresh = fresh; setError(RB_OK); return true; }
            if (millis() - t0 >= timeout_ms) { setError(RB_ERR_TIMEOUT); return false; }
            delay(RBAMP_DRDY_POLL_MS);
        }
    }

    DrdyLine& line = s_drdy_lines[drdy_line_];
    bool line_has_alarm = false;
    for (uint8_t i = 0; i < RBAMP_DRDY_MAX_SUBS; ++i) {
        if (line.subs[i] && line.subs[i]->drdyCanAlarm()) { line_has_alarm = true; break; }
    }
    for (;;) {
        /* Wait for an edge we have not consumed yet. */
        uint32_t edges = 0, edge_us = 0;
        for (;;) {
            drdySnapshot(line, edges, edge_us);
            if (edges != drdy_seen_edges_) break;
            if (millis() - t0 >= timeout_ms) { setError(RB_ERR_TIMEOUT); return false; }
            delay(1);
        }
        drdy_seen_edges_ = edges;

        /* Flags another instance already consumed for this module. */
        if (drdy_pending_flags_) {
            ev->alarm        = true;
            ev->event_flags |= drdy_pending_flags_;
            drdy_pending_flags_ = 0;
        }
        /* Alarm level: the line stays LOW while a masked event is pending. */
        if (line_has_alarm) {
            /* First look just past the pulse (busy-wait of at most ~100 us):
             * HIGH again = plain data-ready, no latency added. Still LOW = a
             * candidate alarm — confirm a hard 5 ms after the EDGE, measured
             * with micros() against the ISR's stamp, never "slept a tick"
             * (L16: the sample must sit well beyond the 10-50 us pulse). */
            const uint32_t since = micros() - edge_us;
            if (since < kDrdyPulseGuardUs) delayMicroseconds(kDrdyPulseGuardUs - since);
            if (digitalRead(line.pin) == LOW) {
                while (micros() - edge_us < kDrdyAlarmUs) yield();
                if (digitalRead(line.pin) == LOW) drdyServiceAlarm(*ev);
            }
        }
        bool fresh = false;
        if (!drdyCheckSeq(*ev, fresh)) return false;
        if (fresh || ev->alarm) {
            ev->fresh      = fresh;
            ev->by_edge    = true;
            ev->latency_us = micros() - edge_us;
            setError(RB_OK);
            return true;
        }
        /* Not ours (another module on the line pulsed): keep waiting. */
    }
}

bool RbAmp::readAllReady(uint32_t timeout_ms, RbAmpSnapshot& out, RbAmpDrdyEvent* ev) noexcept {
    if (!waitDataReady(timeout_ms, ev)) return false;
    return readAll(out);
}

/* ============================================================================
 * Events — mask and thresholds (both product lines)
 * ============================================================================ */

bool RbAmp::readEventMask(uint8_t& out) noexcept {
    if (std_) {
        if (!stdGate36()) return false;
        if (!stdReadU8(rbamp_std::REG_EVENT_MASK, out)) return false;
        setError(RB_OK);
        return true;
    }
    if (!readU8(v2::REG_EVENT_MASK, out)) { setError(RB_ERR_IO); return false; }
    setError(RB_OK);
    return true;
}

bool RbAmp::setEventMask(uint8_t mask) noexcept {
    if (std_) {
        if (!stdGate36()) return false;
        if (!stdWriteU8(rbamp_std::REG_EVENT_MASK, mask)) return false;
    } else if (!writeReg(v2::REG_EVENT_MASK, mask)) {
        setError(RB_ERR_IO);
        return false;
    }
    uint8_t rb = 0;
    if (!readEventMask(rb)) return false;
    if (rb != mask) {
        if (log_) { log_->print(F("[rbamp] event mask readback 0x")); log_->print(rb, HEX); log_->print(F(" != 0x")); log_->println(mask, HEX); }
        setError(RB_ERR_PARAM);
        return false;
    }
    setError(RB_OK);
    return true;
}

bool RbAmp::setEventThresholds(float i_amps, float p_watts) noexcept {
    if (!std_) {
        /* BASIC v1.3: THRESH_I_HI u16 LE in 0.01 A (max over channels),
         * THRESH_P_HI u16 LE in W (SIGNED sum of channels); 0xFFFF = off. */
        /* SCT-013 rating correction: the module compares its own (uncorrected)
         * values, so a true-amps threshold is divided by the scale. Exact when
         * every channel shares one scale; with mixed sensors the largest scale
         * is used, so the event can only come early, never be missed. */
        const float s = sctMaxScale();
        i_amps  /= s;
        p_watts /= s;
        const uint16_t i_raw = (!(i_amps > 0.0f) || i_amps >= 655.34f) ? 0xFFFFu : static_cast<uint16_t>(i_amps * 100.0f + 0.5f);
        const uint16_t p_raw = (!(p_watts > 0.0f) || p_watts >= 65534.5f) ? 0xFFFFu : static_cast<uint16_t>(p_watts + 0.5f);
        if (!writeReg(v2::REG_THRESH_I_HI, static_cast<uint8_t>(i_raw))
                || !writeReg(static_cast<uint8_t>(v2::REG_THRESH_I_HI + 1), static_cast<uint8_t>(i_raw >> 8))
                || !writeReg(v2::REG_THRESH_P_HI, static_cast<uint8_t>(p_raw))
                || !writeReg(static_cast<uint8_t>(v2::REG_THRESH_P_HI + 1), static_cast<uint8_t>(p_raw >> 8))) {
            setError(RB_ERR_IO);
            return false;
        }
        setError(RB_OK);
        return true;
    }
    if (!stdGate36()) return false;
    const uint32_t i_ua = (!(i_amps > 0.0f) || i_amps > 4000.0f) ? 0xFFFFFFFFu : static_cast<uint32_t>(i_amps * 1e6f);
    const uint32_t p_mw = (!(p_watts > 0.0f) || p_watts > 4000000.0f) ? 0xFFFFFFFFu : static_cast<uint32_t>(p_watts * 1e3f);
    if (!stdWriteU32(rbamp_std::REG_THRESH_I, i_ua)) return false;
    if (!stdWriteU32(rbamp_std::REG_THRESH_P, p_mw)) return false;
    setError(RB_OK);
    return true;
}
