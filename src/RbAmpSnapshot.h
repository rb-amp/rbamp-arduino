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
    UI5     = 7,   /**< 5 current + voltage + power (senior SKU, F040). */
    UI7     = 8,   /**< 7 current + voltage + power (senior SKU, F040). */
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
    Sct013     = 1,   /**< SCT-013-series clamp-on CT (only shipping value as of v1.2). */
    WiredCT    = 2,   /**< Reserved — generic wired CT. */
    BuiltinCT  = 3,   /**< Reserved — on-board built-in CT. */
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
 *          An unknown or reserved code (e.g. Sct013_100 / Sct013_060) is
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
    Sct013_100  = RBAMP_CT_SCT013_100,       /**< SCT-013-100 — 100 A. reserved → ERR_PARAM. */
    Sct013_020  = RBAMP_CT_SCT013_020,       /**< SCT-013-020 — 20 A clamp. */
    Sct013_060  = RBAMP_CT_SCT013_060,       /**< SCT-013-060 — 60 A. reserved → ERR_PARAM. */
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
 * Refreshed every 200 ms by the device firmware; reads are non-blocking
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
    uint8_t channels;       /**< 1..3 — number of valid channels in arrays. */
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
};

#endif /* RBAMP_SNAPSHOT_H */
