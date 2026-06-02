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

/**
 * @brief Variant topology — populated by RbAmp::begin() via NACK probe.
 *
 * Reflects how many independent current channels the device firmware exposes.
 * Voltage hardware presence is a separate flag (RbAmpSnapshot::has_voltage_hw).
 *
 * @see SPEC §8 — Variant auto-detection.
 */
enum class RbAmpTopology : uint8_t {
    Single      = 1,   /**< 1 current channel (UI1 / I1). */
    SplitPhase  = 2,   /**< 2 current channels (UI2 / I2). */
    ThreePhase  = 3,   /**< 3 current channels (UI3 / I3). */
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
 * @brief Typed enumeration of SCT-013 models — replaces the magic-int
 *        @c code argument to setCTModel().
 *
 * Both signed forms remain available:
 *  - @c setCTModel(uint8_t code) — legacy 1..5 form
 *  - @c setCTModel(RbAmpCTModel model) — typed form (recommended)
 *
 * The numeric values match the wire-level @c REG_CT_MODEL encoding so
 * the typed overload is a thin static_cast wrapper with no runtime cost.
 *
 * @see SPEC §10, registers.yaml REG_CT_MODEL.
 */
enum class RbAmpCTModel : uint8_t {
    Unset       = 0,   /**< Default after factory reset; setCTModel() refused on v1.2+. */
    Sct013_005  = 1,   /**< SCT-013-005A — 5 A clamp. */
    Sct013_010  = 2,   /**< SCT-013-010 — 10 A clamp. */
    Sct013_030  = 3,   /**< SCT-013-030 — 30 A clamp. */
    Sct013_050  = 4,   /**< SCT-013-050 — 50 A clamp. */
    Sct013_100  = 5,   /**< SCT-013-100 — 100 A clamp. */
};

/**
 * @brief Real-time metering snapshot — one full read of the V03 RT register block.
 *
 * Populated by RbAmp::readAll(). All floats are in SI units.
 * Refreshed every 200 ms by the device firmware; reads are non-blocking
 * (a few milliseconds at 100 kHz I2C due to no-auto-increment protocol).
 *
 * Channel indices 0/1/2 map to I0/I1/I2 on the device. Indices beyond
 * @c channels are zeroed.
 */
struct RbAmpSnapshot {
    float voltage;          /**< RMS voltage at REG_V03_U_RMS (0x86) in V. */
    float voltage_peak;     /**< Peak voltage at REG_V03_U_PEAK (0x8A) in V. */
    float current[3];       /**< RMS current per channel in A. */
    float current_peak[3];  /**< Peak current per channel in A. */
    float power[3];         /**< Real power per channel in W (signed; negative = export). */
    float power_factor[3];  /**< Power factor per channel, dimensionless (-1..+1). */
    float frequency;        /**< Mains frequency at REG_AC_FREQ (0x20) in Hz. */
    RbAmpTopology topology; /**< Variant detected at begin(). */
    uint8_t channels;       /**< 1..3 — number of valid channels in arrays. */
    bool has_voltage_hw;    /**< true if device has voltage sensing hardware. */
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
    float avg_p[3];      /**< Average real power per channel over the latched period (W). */
    float max_p;         /**< Peak real power on channel 0 during the period (W). */
    uint32_t latch_ms;   /**< Device-reported period duration (diagnostic, do not use for energy). */
    uint32_t master_dt_ms; /**< Master's wall-clock dt since previous successful latch (ms). */
    bool valid;          /**< true if REG_V03_PERIOD_VALID bit0==1 at read time. */
};

#endif /* RBAMP_SNAPSHOT_H */
