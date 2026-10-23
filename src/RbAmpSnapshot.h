/**
 * @file    RbAmpSnapshot.h
 * @brief   POD snapshot structs returned by RbAmp::readAll() and
 *          RbAmp::readPeriodSnapshot().
 * @author  rbAmp team
 * @date    2026
 *
 * @details
 * These structs are part of the public API surface defined in
 * @c libs/spec/SPEC.md §12. They are plain C/POD so they bind cleanly to
 * the corresponding STM32 HAL / ESP-IDF / Python equivalents.
 *
 * Unused channels (e.g. channel 1 and 2 on a UI1 SKU) are filled with @c 0.0f.
 * Call RbAmp::channels() to learn how many channels are populated.
 */
#ifndef RBAMP_SNAPSHOT_H
#define RBAMP_SNAPSHOT_H

#include <stdint.h>
#include "RbAmpSensorModels.h"  /* auto-generated CT-model registry (single source of truth) */

/**
 * @brief Maximum current channels across all SKUs (UI7 / I7 = 7).
 *
 * Sizes the per-channel arrays in RbAmpSnapshot / RbAmpPeriodSnapshot and the
 * Wh accumulator. Junior SKUs (1..3 channels) leave the tail filled with 0.
 * Call RbAmp::channels() for the count actually populated.
 */
#ifndef RBAMP_MAX_CHANNELS
#define RBAMP_MAX_CHANNELS 7
#endif

/**
 * @brief Variant topology — populated by RbAmp::begin() from REG_HW_VARIANT (0x55).
 *
 * Reflects how many independent current channels the device firmware exposes.
 * Voltage hardware presence is a separate flag (RbAmpSnapshot::has_voltage_hw)
 * — also derived from the variant byte on v1.2+ (0x01..0x03 = with U,
 * 0x04..0x06 = no U). On v1.0/v1.1 firmware the variant byte reads 0x00 (it
 * lives in the legacy CS_INTERVAL_L slot) and begin() falls back to the
 * constructor hint + a U_RMS threshold for hasVoltageHw().
 *
 * @see truth-doc §1 — Detection / variant identification.
 */
enum class RbAmpTopology : uint8_t {
    Single      = 1,   /**< 1 current channel (UI1 / I1). */
    SplitPhase  = 2,   /**< 2 current channels (UI2 / I2). */
    ThreePhase  = 3,   /**< 3 current channels (UI3 / I3). */
    Five        = 5,   /**< 5 current channels (UI5) — senior SKU. */
    Seven       = 7,   /**< 7 current channels (UI7) — senior SKU. */
};

/**
 * @brief Hardware SKU variant — authoritative value of REG_HW_VARIANT (0x55).
 *
 * Each SKU encodes both the channel count AND voltage-sensing presence in one
 * byte. @c readVariant() returns this; @c begin() uses it to set topology +
 * hasVoltageHw() directly. v1.0/v1.1 firmware returns 0x00 here (the register
 * is unmapped) → @c Unknown, and the library falls back to the constructor
 * topology hint + a U_RMS threshold for voltage detection.
 *
 * @see truth-doc §1.2 — Detection / variant identification.
 */
enum class RbAmpVariant : uint8_t {
    Unknown = 0,   /**< Not identified (v1.0/v1.1 firmware, or non-rbAmp device). */
    UI1     = 1,   /**< 1 current + voltage + power. */
    UI2     = 2,   /**< 2 current + voltage + power. */
    UI3     = 3,   /**< 3 current + voltage + power. */
    I1      = 4,   /**< 1 current only (no voltage / power). */
    I2      = 5,   /**< 2 current only. */
    I3      = 6,   /**< 3 current only. */
    UI5     = 7,   /**< 5 current + voltage + power (senior SKU). */
    UI7     = 8,   /**< 7 current + voltage + power (senior SKU). */
    ThreePh3Ch = 9,   /**< STD line: 3 voltage + 3 current (three-phase meter). */
    ThreePh6Ch = 10,  /**< STD line: 3 voltage + 6 current. */
};

/**
 * @brief Product line the module behind an RbAmp instance speaks — detected
 *        by RbAmp::begin() and cached (RbAmp::line()).
 *
 * One class and one API serve both lines; begin() discovers the line from the
 * module's identity block and every call routes internally.
 *  - @c Basic — 8-bit register pointer, float registers, channel access window
 *    for senior channels (the v1.x contract in RbAmpRegistersV2.h).
 *  - @c Std — 16-bit big-endian register pointer with auto-increment,
 *    fixed-point registers, per-channel blocks at fixed family addresses
 *    (RbAmpStdRegisters.h). Reactive / apparent power, THD, signed energy and
 *    the multi-bank period latch exist only on this line.
 *
 * A call that has no counterpart on the detected line fails with
 * @c RB_ERR_NOT_IMPLEMENTED — never a wrong-register read.
 */
enum class RbAmpLine : uint8_t {
    Basic = 0,   /**< 8-bit map (default until begin() runs). */
    Std   = 1,   /**< 16-bit family map. */
};

/**
 * @brief Which period / energy mechanism an STD module offers — chosen by
 *        RbAmp::begin() from the module's capability bits and reported in
 *        RbAmpStdInfo::period_source.
 *
 * - @c Latch: the module has latch banks (bank 0 serves readPeriodSnapshot()).
 * - @c Counters: no latch banks, but persisted life-time energy counters — a
 *   period is the difference of two counter readings on the master's schedule
 *   (readPeriodSnapshot() / energy() route onto it; the bank calls refuse).
 * - @c None: neither — the period / energy calls fail with RB_ERR_NOT_IMPLEMENTED.
 */
enum class RbAmpStdPeriodSource : uint8_t {
    None     = 0,
    Latch    = 1,
    Counters = 2,
};

/**
 * @brief Current-sensor family — selects which preset table is consulted
 *        by setCTModel().
 *
 * The class is determined by the module's hardware revision (SKU label) and
 * must be set BEFORE setCTModel() on v1.2+ firmware. On v1.0/v1.1 firmware
 * the register exists but is unused — write is harmless.
 *
 * @see SPEC §10 — Sensor configuration.
 */
enum class RbAmpSensorClass : uint8_t {
    Unset      = 0,   /**< Default after factory reset; setCTModel() refused on v1.2+. */
    Sct013     = 1,   /**< SCT-013-series clamp-on CT fitted by the user; the model selects the sensor type. */
    WiredCT    = 2,   /**< Wired CT shipped with the module and calibrated together with it (see setSensorClass()). */
    BuiltinCT  = 3,   /**< Built-in CT, calibrated together with the module (see setSensorClass()). */
};

/** @brief Why a channel's SCT-013 rating scale is what it is — see RbAmp::sctRating(). */
enum class RbAmpSctRatingStatus : uint8_t {
    NotApplicable  = 0,  /**< STD line, sensor class other than SCT-013, firmware that corrects itself (1.4.20+), or no model: scale 1. */
    Native         = 1,  /**< The channel shows the model the batch was calibrated on (030): nothing to correct, scale 1. */
    Corrected      = 2,  /**< BASIC fw <= 1.4.19 with another SCT-013 model: readings are scaled relative to the gain the module applies. */
    Uncalibrated   = 3,  /**< The module applies the raw preset even after a restart (no unit trim): nominal correction only. */
    UnexpectedGain = 4,  /**< The applied gain is more than 5 % from the calibration preset: nothing assumed, scale 1. */
    Unknown        = 5,  /**< Could not be read (transport): scale 1. */
};

/**
 * @brief SCT-013 rating correction of one channel (RbAmp::sctRating()).
 *
 * BASIC modules up to fw 1.4.19 are calibrated on the SCT-013-030 and apply
 * that calibrated gain whatever model the channel shows after a restart. The
 * library therefore scales I / P readings by @c scale = preset(model) ×
 * c(model) / preset(030). PF and voltage are not touched.
 */
struct RbAmpSctRating {
    uint8_t  model        = 0;     /**< CT model code the channel shows. */
    float    applied_gain = 0.0f;  /**< Gain the module applies to this channel. */
    uint16_t noise_floor  = 0;     /**< Noise floor the module applies (ADC counts). */
    float    scale        = 1.0f;  /**< Factor applied to I_rms / I_peak / P / period avg P of this channel. */
    RbAmpSctRatingStatus status = RbAmpSctRatingStatus::NotApplicable;
};

/**
 * @brief Typed convenience enumeration for the SCT-013 class (backward-compat).
 *
 * @note As of v1.5.0 the CT-model list is generated from the registry
 *       (@c libs/spec/sensor_models.yaml → @c RbAmpSensorModels.h). This enum
 *       is a FROZEN backward-compat alias for the seven SCT-013 codes only; its
 *       values are sourced from the generated @c RBAMP_CT_SCT013_* defines so
 *       they cannot drift from the registry. New code — and every WIRED_CT
 *       model — uses the generated @c RBAMP_CT_* defines directly with
 *       @c setCTModel(uint8_t code) / @c setCTModel(channel, code).
 *
 * Both call forms remain available:
 *  - @c setCTModel(uint8_t code) — numeric / generated-define form (canonical)
 *  - @c setCTModel(RbAmpCTModel model) — typed form (SCT-013 only, legacy)
 *
 * @warning Model ACCEPTANCE is runtime truth: the client only checks that the
 *          (class, code) pair exists in the registry, then the module decides.
 *          An unknown or unsupported code is
 *          surfaced as @c RB_ERR_PARAM from the device — the channel keeps its
 *          previous model. See @c rbamp_sensor_model_lookup().
 *
 * @see SPEC §10, truth-doc §7, RbAmpSensorModels.h.
 */
enum class RbAmpCTModel : uint8_t {
    Unset       = RBAMP_SENSOR_CLASS_UNSET,  /**< 0 — default after factory reset. */
    Sct013_005  = RBAMP_CT_SCT013_005,       /**< SCT-013-005A — 5 A clamp. */
    Sct013_010  = RBAMP_CT_SCT013_010,       /**< SCT-013-010 — 10 A clamp. */
    Sct013_030  = RBAMP_CT_SCT013_030,       /**< SCT-013-030 — 30 A clamp. */
    Sct013_050  = RBAMP_CT_SCT013_050,       /**< SCT-013-050 — 50 A clamp. */
    Sct013_100  = RBAMP_CT_SCT013_100,       /**< SCT-013-100 — 100 A clamp. */
    Sct013_020  = RBAMP_CT_SCT013_020,       /**< SCT-013-020 — 20 A clamp. */
    Sct013_060  = RBAMP_CT_SCT013_060,       /**< SCT-013-060 — 60 A clamp. */
};

/** @name Per-field implausible bits (RbAmpSnapshot::implausible). */
/**@{*/
static const uint8_t RBAMP_FIELD_VOLTAGE   = 1u << 0;
static const uint8_t RBAMP_FIELD_CURRENT   = 1u << 1;
static const uint8_t RBAMP_FIELD_POWER     = 1u << 2;
static const uint8_t RBAMP_FIELD_PF        = 1u << 3;
static const uint8_t RBAMP_FIELD_FREQUENCY = 1u << 4;
/**@}*/

/**
 * @brief Real-time metering snapshot — one full read of the V03 RT register block.
 *
 * Populated by RbAmp::readAll(). All floats are in SI units.
 * Refreshed once per measurement window — 100 ms (images before v1.2 used
 * 200 ms); reads are non-blocking
 * (a few milliseconds at 100 kHz I2C due to no-auto-increment protocol).
 *
 * Channel indices 0/1/2 map to I0/I1/I2 on the device. Indices beyond
 * @c channels are zeroed.
 *
 * A field that reads but fails the physical sanity filter is set to @c NAN and
 * its bit flagged in @c implausible — the rest of the snapshot stays usable.
 * Only a transport failure makes readAll() return @c false.
 */
struct RbAmpSnapshot {
    float voltage;          /**< RMS voltage at REG_V03_U_RMS (0x86) in V. */
    float voltage_peak;     /**< Peak voltage at REG_V03_U_PEAK (0x8A) in V. */
    float current[RBAMP_MAX_CHANNELS];       /**< RMS current per channel in A. */
    float current_peak[RBAMP_MAX_CHANNELS];  /**< Peak current per channel in A. */
    float power[RBAMP_MAX_CHANNELS];         /**< Real power per channel in W (signed; negative = export). */
    float power_factor[RBAMP_MAX_CHANNELS];  /**< Power factor per channel, dimensionless (-1..+1). */
    float frequency;        /**< Mains frequency at REG_AC_FREQ (0x20) in Hz. */
    RbAmpTopology topology; /**< Variant detected at begin(). */
    uint8_t channels;       /**< 1..RBAMP_MAX_CHANNELS — number of valid channels in arrays. */
    bool has_voltage_hw;    /**< true if device has voltage sensing hardware. */
    uint8_t implausible;    /**< Bitmask of fields set NaN by the sanity filter (RBAMP_FIELD_*). */
};

/**
 * @brief Period-metering snapshot — output of RbAmp::readPeriodSnapshot().
 *
 * The energy primitive of the rbAmp protocol. Master tracks wall-clock @c master_dt_ms
 * between successful latches and integrates Wh from @c avg_p:
 *
 * @code
 *   E_Wh[ch] += snap.avg_p[ch] * (snap.master_dt_ms / 1000.0) / 3600.0;
 * @endcode
 *
 * The device's view of the period (@c latch_ms) is diagnostic only — the
 * master's own wall-clock is authoritative for energy correctness.
 *
 * @see SPEC §7 — Period-metering state machine.
 */
struct RbAmpPeriodSnapshot {
    float avg_p[RBAMP_MAX_CHANNELS]; /**< Average real power per channel over the latched period (W). */
    float max_p;         /**< Peak real power on channel 0 during the period (W). */
    uint32_t latch_ms;   /**< Device-reported period duration (diagnostic, do not use for energy). */
    uint32_t master_dt_ms; /**< Master's wall-clock dt since previous successful latch (ms). */
    bool valid;          /**< true if REG_V03_PERIOD_VALID bit0==1 at read time. */
    float avg_p_total;   /**< Average real power over the period, all channels (W). */
    double wh_net;       /**< Net energy of this period (import - export, Wh): the module's own counter delta on the STD counter path, avg_p_total x dt elsewhere. */
    bool module_restarted; /**< The module restarted inside this period (its uptime went back); counters persisted, period kept. STD counter path only. */
};

/* ============================================================================
 * STD line — block-read structures (RbAmp::stdRead* / stdReadLatch)
 * ============================================================================ */

/**
 * @brief STD-line identity, cached by RbAmp::begin() (no bus traffic to read).
 *
 * Feature gating on this line goes by @c capabilities (@c rbamp_std::CAP_*)
 * and @c map_version, never by guessing. Channel gating goes by @c n_voltage /
 * @c n_current: registers of channels at or beyond those counts are
 * unspecified on the module and the library refuses to read them.
 */
struct RbAmpStdInfo {
    uint8_t  map_version;  /**< Register-map version (@c rbamp_std::REG_VERSION). */
    uint16_t capabilities; /**< @c rbamp_std::CAP_* bitmap. */
    uint8_t  status;       /**< @c rbamp_std::ST_* bits at begin() time (live: stdReadHealth()). */
    uint8_t  n_voltage;    /**< Populated voltage channels (0..8). */
    uint8_t  n_current;    /**< Populated current channels (1..64; the library exposes up to RBAMP_MAX_CHANNELS). */
    uint8_t  variant;      /**< Raw @c rbamp_std::VARIANT_* byte (also mapped into readVariant()). */
    uint32_t active_bank;  /**< Running firmware bank base (OTA), 0 if the module has no OTA capability. */
    uint32_t ota_bank_size;/**< Flash bank size the module reports (image + header must fit), 0 if none. */
    RbAmpStdPeriodSource period_source; /**< Which period / energy mechanism this module offers (see the enum). */
};

/** @brief Progress callback for RbAmp::stdOtaWriteImage() (pages done / total). */
typedef void (*RbAmpOtaProgress)(uint32_t done, uint32_t total, void* ctx);

/**
 * @brief One STD current channel (or a TOTALS aggregate) — output of
 *        RbAmp::stdReadChannel() / RbAmp::stdReadTotals().
 *
 * Decoded from one burst read of the channel block, so the fields are as
 * coherent as the module publishes them (byte-wise per measurement window —
 * ~100 ms from image 0x7C, ~200 ms on earlier images; for a
 * strictly atomic multi-field snapshot use a latch bank). Fixed-point wire
 * units are converted to SI here. A field that trips a sanity bound is set to
 * @c NAN with the matching @c RBAMP_FIELD_* bit in @c implausible.
 *
 * @note @c thd_i / @c pf1 / @c q1 read 0 unless the module's harmonics
 *       extraction is enabled (off by default on the regular tier).
 * @note @c e_import_mws / @c e_export_mws are the module's raw signed energy
 *       accumulators in mW·samples. The module has no clock, so converting to
 *       Wh needs the sample count and the master's own period timing (latch
 *       path, RbAmp::stdLatchWh()). For monitoring, integrate @c p yourself.
 */
struct RbAmpStdChannel {
    float   i_rms;         /**< RMS current, A. */
    float   i_peak;        /**< Peak current, A. */
    float   p;             /**< Active power, W (signed). */
    float   q;             /**< Reactive power, var (signed). */
    float   s;             /**< Apparent power, VA. */
    float   pf;            /**< Power factor (-1..+1). */
    float   thd_i;         /**< Current THD, % (0 when harmonics are off). */
    float   pf1;           /**< Displacement (fundamental) power factor (0 when harmonics are off). */
    float   q1;            /**< Fundamental reactive power, var (0 when harmonics are off). */
    int64_t e_import_mws;  /**< Import energy accumulator, mW·samples (>= 0). */
    int64_t e_export_mws;  /**< Export energy accumulator, mW·samples (<= 0). */
    uint8_t implausible;   /**< Bitmask of @c RBAMP_FIELD_* classes set to NaN. 0 = all OK. */
};

/** @brief One STD voltage channel — output of RbAmp::stdReadVoltage(). */
struct RbAmpStdVoltage {
    float   u_rms;         /**< RMS voltage, V. */
    float   u_peak;        /**< Peak voltage, V. */
    float   thd_u;         /**< Voltage THD, % (0 when harmonics are off). */
    float   angle_deg;     /**< Fundamental phase angle vs voltage 0, degrees (-180, 180]; 0 when @c angle_valid is false. Forward sequence: v1 ≈ -120, v2 ≈ +120. */
    bool    angle_valid;   /**< @c true when the module publishes an angle for this input (image 0x7A+, input and voltage 0 above 50 V). Single-voltage modules: valid only for v = 0 (always 0°). */
    uint8_t implausible;   /**< @c RBAMP_FIELD_VOLTAGE if a bound tripped. */
};

/**
 * @brief Phase sequence of a three-voltage module (RbAmp::stdReadPhaseSequence()).
 *
 * Derived by the module from the voltage angles (image 0x7A+). @c NotAvailable
 * on modules with fewer than three voltage inputs, when one input is below
 * 50 V, and on images that do not publish it.
 */
enum class RbAmpStdPhaseSeq : uint8_t {
    NotAvailable = 0,  /**< Not determined (fewer than 3 voltage inputs, an input below 50 V, or an older image). */
    Forward      = 1,  /**< L1-L2-L3. */
    Reverse      = 2,  /**< L1-L3-L2. */
    Abnormal     = 3   /**< No 120° pattern (e.g. two inputs on one phase). */
};

/** @brief STD module health — output of RbAmp::stdReadHealth() (live). */
struct RbAmpStdHealth {
    uint8_t  status;       /**< @c rbamp_std::ST_* bits: cal_ok / sensor_ok / mains_ok / (bit 3 reserved) / time_valid / nvm_fault / maint.
                                 cal_ok (bit 0) on image 0x7A+ comes from a marker the module writes with its calibration save: a unit calibrated on an older image reads cal_ok = 0 until its calibration is saved once more. */
    uint32_t frame_count;  /**< Sensor frame counter — must grow between polls (liveness). */
    uint32_t crc_errors;   /**< Sensor frame CRC error counter. */
    /* Map 0x35+ (0 / false below): */
    bool     time_valid;   /**< Module clock set since its last restart (ST_TIME_VALID). Set it with RbAmp::stdSetTime(). */
    bool     nvm_fault;    /**< Energy store unusable (ST_NVM_FAULT). */
    bool     maint;        /**< Maintenance mode (ST_MAINT). */
    uint8_t  lta_status;   /**< Life-time energy store: 0 ok · 1 new · 2 fault · 3 corrupt (service). */
    uint8_t  lta_src;      /**< Where the life-time counters were restored from (module-specific code). */
    uint8_t  cal_src;      /**< 0 built-in defaults · 1 flash · 2 EEPROM · 3 stored cal is for another firmware (defaults in use). */
    uint8_t  group_status; /**< Map 0x36+: per-channel energy store: 0 ok · 1 new · 2 fault · 3 restarted at 0. */
    uint16_t legal_noen;   /**< Operations executed without the legal gate (report mode). */
    uint16_t ring_drop;    /**< Acquisition ring drops since boot — should stay 0. */
    uint32_t lost_frames;  /**< ADC frames that never arrived since boot — should stay flat. */
    uint32_t uptime_s;     /**< Seconds since the module's last restart. */
    /* Module image 0x00010076+ (0 below): */
    uint8_t  reset_cause;  /**< Cause of THIS boot, @c rbamp_std::RST_* bits (PIN accompanies most internal resets — judge by the other bits). */
    uint16_t boot_count;   /**< Persisted boot counter, low 16 bits — a master that missed a restart sees it step. */
};

/** @brief STD configuration block — output of RbAmp::stdReadConfig(). */
struct RbAmpStdConfig {
    uint8_t  sensor_class; /**< RbAmpSensorClass value; 0 = unset. Persisted. */
    uint8_t  ct_model;     /**< CT model reference for channel 0 (label only, sets no gain). Persisted. */
    uint8_t  tier;         /**< 0 = regular, 1 = industrial. Persisted. */
    uint8_t  i2c_addr;     /**< Persisted 7-bit address (applies on reset). */
    uint16_t rate_ms;      /**< Report-rate hint, ms (RAM only). */
    bool     harmonics;    /**< Base-harmonics extraction enabled (RAM only; RbAmp::stdSetHarmonics()). */
};

/** @brief Channel selector for RbAmp::stdLatchWh() meaning "bank totals". */
#define RBAMP_STD_TOTAL 0xFFu

/**
 * @brief One latched period from an STD latch bank — output of
 *        RbAmp::stdReadLatch().
 *
 * The module keeps 8 independent banks. A bank accumulates while its bit is
 * set in the arm mask (RbAmp::stdArmBanks()); RbAmp::stdLatch() snapshots AND
 * resets it at the next measurement-window boundary, so a period is the
 * interval between two latches of the same bank, measured by the MASTER's
 * clock (the module has none). Totals are averages over the period; energy is
 * the module's raw signed accumulator in mW·samples, converted with
 * RbAmp::stdLatchWh(). Per-channel average power is derived as
 * (E_import + E_export) / nsamp per the module contract.
 *
 * @note The module's "valid" flag only says the bank has EVER been latched;
 *       it is not a freshness signal. Read a bank at least one measurement
 *       window (~300 ms) after latching it. @c win_count == 0 means the bank
 *       was not armed or no window elapsed since it was armed.
 */
struct RbAmpStdLatch {
    float    p_avg;        /**< Average active power over the period, W (signed). */
    float    q_avg;        /**< Average reactive power, var (signed). */
    float    s_avg;        /**< Average apparent power, VA. */
    float    max_p;        /**< Peak active power in the period, W. */
    float    max_i;        /**< Peak RMS current, A. */
    float    max_u;        /**< Peak RMS voltage, V. */
    float    min_u;        /**< Minimum RMS voltage, V. */
    int64_t  e_import_mws; /**< Import energy over the period, mW·samples (>= 0). */
    int64_t  e_export_mws; /**< Export energy over the period, mW·samples (<= 0). */
    uint32_t nsamp;        /**< Sensor samples in the period. */
    uint32_t win_count;    /**< Measurement windows in the period (0 = not armed / nothing elapsed). */
    uint8_t  channels;     /**< Entries valid in the per-channel arrays (= RbAmp::channels()). */
    uint8_t  bank;         /**< Bank this snapshot came from (0..7). */
    bool     valid;        /**< Module reports the bank has a snapshot (see note). */
    float    p_avg_ch[RBAMP_MAX_CHANNELS];          /**< Per-channel average active power, W. */
    int64_t  e_import_ch_mws[RBAMP_MAX_CHANNELS];   /**< Per-channel import energy, mW·samples. */
    int64_t  e_export_ch_mws[RBAMP_MAX_CHANNELS];   /**< Per-channel export energy, mW·samples. */
};

/** @brief Channel selector for RbAmpEnergy::wh() / addWh() / reset() meaning "all channels" (the running total). */
#define RBAMP_ENERGY_CH_TOTAL 0xFFu

/**
 * @brief Life-time energy counters of an STD module (RbAmp::stdReadEnergy()).
 *
 * Persisted on the module (survive power loss), monotonic, signed import /
 * export split; the grand block is the energy register. A period's energy is
 * the difference of two readings (RbAmp::stdEnergyDelta()). Below map 0x36
 * the mWh fields and the per-channel arrays are 0 (whole-Wh totals only).
 *
 * The @c *_uwh / @c *_uvarh fields are ALWAYS filled, so a caller can use
 * them on any module: on a module that publishes micro-resolution counters
 * (@c fine == true) they are the module's own counters; otherwise they are
 * derived from the coarse ones (mWh·1000 — or Wh·10⁶ below map 0x36 — and
 * varh·10⁶) and step in those units.
 */
struct RbAmpStdEnergy {
    int64_t  act_import_wh;   /**< Imported active energy, Wh. */
    int64_t  act_export_wh;   /**< Exported active energy, Wh (magnitude). */
    int64_t  act_net_wh;      /**< Import - export, Wh. */
    int64_t  rea_import_varh; /**< Reactive, varh. */
    int64_t  rea_export_varh; /**< Reactive, varh (magnitude). */
    uint32_t generation;      /**< Life-time generation counter — steps when the counters were reset or adopted; never take a delta across a change. */
    uint32_t uptime_s;        /**< Module uptime at the reading, s (a drop = the module restarted). */
    int64_t  act_import_mwh;  /**< Map 0x36+: imported, mWh (0 below). */
    int64_t  act_export_mwh;  /**< Map 0x36+: exported, mWh, magnitude (0 below). */
    int64_t  act_net_mwh;     /**< Map 0x36+: import - export, mWh (0 below). */
    uint8_t  channels;        /**< Map 0x36+: entries valid in the per-channel arrays (0 below). */
    uint8_t  group_status;    /**< Map 0x36+: per-channel store: 0 ok · 1 new · 2 fault · 3 restarted at 0. */
    int64_t  ch_import_mwh[RBAMP_MAX_CHANNELS]; /**< Per-channel imported, mWh (informative; restart at 0 with the generation or on a new memory). */
    int64_t  ch_export_mwh[RBAMP_MAX_CHANNELS]; /**< Per-channel exported, mWh. */
    bool     fine;            /**< @c true: the µWh / µvarh fields are the module's own micro-resolution counters (capability @c rbamp_std::CAP_ENERGY_UWH). @c false: derived from the coarse fields. */
    int64_t  act_import_uwh;  /**< Imported active energy, µWh. */
    int64_t  act_export_uwh;  /**< Exported active energy, µWh (magnitude). */
    int64_t  act_net_uwh;     /**< Import - export, µWh. */
    int64_t  rea_import_uvarh;/**< Reactive import, µvarh (module counter from image 0x7B; derived from varh before). */
    int64_t  rea_export_uvarh;/**< Reactive export, µvarh (magnitude). */
    int64_t  ch_import_uwh[RBAMP_MAX_CHANNELS]; /**< Per-channel imported, µWh. */
    int64_t  ch_export_uwh[RBAMP_MAX_CHANNELS]; /**< Per-channel exported, µWh. */
};

/**
 * @brief STD module identity (RbAmp::stdIdentify() / RbAmp::stdProbe()).
 *
 * The UID is the module's identity in a master's database — addresses can
 * change, the UID cannot. @c fw_version is the running image's own version
 * (not the register-map version); @c board_id identifies the board an OTA
 * image must be built for (RbAmp::stdOtaBoardOk()).
 */
struct RbAmpStdIdentity {
    uint8_t  i2c_addr;     /**< 7-bit address the module answered on. */
    uint8_t  map_version;  /**< Register-map version. */
    uint16_t capabilities; /**< @c rbamp_std::CAP_* bitmap. */
    uint8_t  variant;      /**< Raw @c rbamp_std::VARIANT_* byte. */
    uint8_t  n_voltage;    /**< Voltage channels. */
    uint8_t  n_current;    /**< Current channels. */
    uint8_t  uid[12];      /**< Chip UID. */
    uint32_t fw_version;   /**< Running image version (map 0x35+; 0 if unknown). */
    uint32_t board_id;     /**< Board id (map 0x36+; 0 below). */
};

/** @brief History rings of an STD module (RbAmp::stdHistInfo() / stdHistRead()). */
enum class RbAmpStdHistPeriod : uint8_t {
    Min15  = 0,  /**< 15-minute energy records — RAM only (the last record survives no restart). */
    Hour   = 1,  /**< Hourly energy records — persistent ring (the one to backfill from). */
    Events = 2,  /**< Event log (RbAmp::stdEventRead()). */
};

/** @brief Ring summary (RbAmp::stdHistInfo()). */
struct RbAmpStdHistInfo {
    uint16_t count;        /**< Records in the ring. */
    uint16_t capacity;     /**< Ring capacity. */
    uint16_t record_size;  /**< Bytes per record (80 energy / 32 event). */
    uint32_t oldest_ts;    /**< Unix time of the oldest record. */
    uint32_t newest_ts;    /**< Unix time of the newest record. */
};

/** @brief Phases carried by a history record (per-phase energy fields). */
#define RBAMP_STD_HIST_PHASES 3

/**
 * @brief One energy-period record (RbAmp::stdHistRead()).
 *
 * @c e_import_wh / @c e_export_wh are the module's life-time ABSOLUTE
 * counters at the end of the period; @c ph_import_wh / @c ph_export_wh are
 * THIS period's per-phase energy. Records exist only for periods during which
 * the module clock was valid. Take a period's energy from two consecutive
 * hourly records only when their timestamps differ by exactly one period and
 * neither carries @c rbamp_std::HF_LTA_JUMP.
 */
struct RbAmpStdHistRecord {
    uint32_t ts;           /**< Unix time at the end of the period. */
    uint16_t flags;        /**< @c rbamp_std::HF_* bits (partial / power-up / clock / life-time jump). */
    uint16_t n;            /**< Measurement windows in the period. */
    bool     from_ram;     /**< Served from RAM (not yet persisted). */
    float    v_rms;        /**< Average RMS voltage, V. */
    float    i_total;      /**< Average total RMS current, A. */
    float    p_avg;        /**< Average active power, W. */
    float    q_avg;        /**< Average reactive power, var. */
    float    s_avg;        /**< Average apparent power, VA. */
    float    pf;           /**< Average power factor. */
    float    p_min, p_max; /**< Active power envelope, W. */
    float    i_min, i_max; /**< Current envelope, A. */
    uint64_t e_import_wh;  /**< Life-time imported energy at period end, Wh (absolute). */
    uint64_t e_export_wh;  /**< Life-time exported energy at period end, Wh (absolute). */
    uint8_t  phases;       /**< Entries valid in the per-phase arrays (0..3). */
    double   ph_import_wh[RBAMP_STD_HIST_PHASES]; /**< This period's imported energy per phase, Wh. */
    double   ph_export_wh[RBAMP_STD_HIST_PHASES]; /**< This period's exported energy per phase, Wh. */
};

/** @brief One event-log record (RbAmp::stdEventRead()); @c type = @c rbamp_std::EV_*. */
struct RbAmpStdEvent {
    uint32_t ts;           /**< Unix time (0 if the clock was not set). */
    uint32_t uptime_s;     /**< Module uptime at the event, s. */
    uint8_t  type;         /**< @c rbamp_std::EV_* record type. */
    uint8_t  flags;        /**< @c rbamp_std::EVF_* bits. */
    uint16_t boot;         /**< Boot counter at the event. */
    uint32_t a_before;     /**< Event-specific value before. */
    uint32_t a_after;      /**< Event-specific value after. */
    uint64_t e_before_wh;  /**< Life-time energy before, Wh. */
    uint64_t e_after_wh;   /**< Life-time energy after, Wh. */
};

/**
 * @brief Outcome of RbAmp::waitDataReady() / RbAmp::readAllReady().
 *
 * @c seq is the module's snapshot sequence (STD: the publish counter, BASIC:
 * the commit sequence); @c missed counts snapshots published since the
 * previous call that were not read. @c alarm means the DRDY line was held LOW
 * because a masked event is pending on this module: the flags were read and
 * cleared, and @c event_flags holds them.
 */
struct RbAmpDrdyEvent {
    bool     fresh;        /**< A new snapshot of THIS module is available (always true when the wait succeeds without an alarm). */
    bool     by_edge;      /**< Woken by the DRDY edge (false = found by polling). */
    uint16_t seq;          /**< Snapshot sequence now. */
    uint16_t missed;       /**< Snapshots published since the previous call that were not read (0 = none). */
    bool     alarm;        /**< The line was held LOW (masked event pending on this module). */
    uint8_t  event_flags;  /**< Event flags consumed from this module on an alarm (@c rbamp::v2::EVENT_* bits), else 0. */
    uint32_t latency_us;   /**< Edge to return, µs (0 when polling). */
};

#endif /* RBAMP_SNAPSHOT_H */
