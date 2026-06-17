/**
 * @file    RbAmpEnergy.h
 * @brief   Master-side per-channel energy (Wh) accumulator for the rbAmp library.
 * @author  rbAmp team
 * @date    2026
 *
 * @details
 * Owned by the @ref RbAmp class. The user does not normally instantiate this
 * directly — call @c dev.energy() to access the live accumulator after each
 * successful RbAmp::readPeriodSnapshot().
 *
 * Energy is integrated from the period-averaged real power using the master's
 * wall-clock interval (see SPEC §7):
 * @code
 *   E_Wh[ch] += avg_p_W * dt_s / 3600;
 * @endcode
 *
 * @note The @c dt is the MASTER's wall-clock (millis), NOT the chip's
 *       @c REG_V03_PERIOD_LATCH_MS (0xEC). The chip timer under-counts ~25-30 %
 *       under load (SysTick starvation, hardware-confirmed) and is diagnostic
 *       only. Future maintainers / sister-lib ports: do NOT revert energy to the
 *       chip latch_ms — it is a known billing regression. On a stale period the
 *       integration anchor is held (RbAmp::readPeriodSnapshot) so the next valid
 *       window covers the full elapsed interval.
 *
 * On platforms with double-precision floating point (ESP32, ESP8266, STM32
 * Arduino core) the accumulator stores 64-bit @c double — drift is negligible.
 * On classic AVR the toolchain defines @c double as 32-bit (== @c float);
 * users running long-soak energy logging on AVR should reset the accumulator
 * periodically (e.g. every 24 h) to avoid float-precision creep.
 */
#ifndef RBAMP_ENERGY_H
#define RBAMP_ENERGY_H

#include <stdint.h>
#include "RbAmpSnapshot.h"

/**
 * @class RbAmpEnergy
 * @brief Per-channel running-total Wh accumulator.
 *
 * Updates triggered by RbAmp::readPeriodSnapshot(); user code reads totals
 * via wh(ch). Can be disabled at runtime via disable() if the caller wants
 * raw period readings only and will integrate energy elsewhere.
 *
 * Thread-safety: not thread-safe. All operations happen on the main loop.
 *
 * @see SPEC §12 — Energy section.
 */
class RbAmpEnergy {
public:
    /**
     * @brief Construct an empty accumulator.
     *
     * All channels start at 0 Wh. Enabled by default.
     */
    RbAmpEnergy() noexcept;

    /**
     * @brief Integrate a period snapshot into the running totals.
     *
     * Idempotent: a snapshot with @c valid==false is ignored. Called
     * automatically by RbAmp::readPeriodSnapshot() — user code does not need
     * to invoke this directly.
     *
     * @param[in] snap Period snapshot from readPeriodSnapshot().
     * @param[in] channels Number of channels actually populated (1..3).
     */
    void tick(const RbAmpPeriodSnapshot& snap, uint8_t channels) noexcept;

    /**
     * @brief Read the running total Wh for one channel.
     *
     * @param[in] ch Channel index 0..2. Out-of-range returns 0.
     * @return Running total energy in Wh. Signed: negative = net export.
     */
    double wh(uint8_t ch = 0) const noexcept;

    /**
     * @brief Reset one channel's accumulator to zero.
     *
     * @param[in] ch Channel index 0..2.
     */
    void reset(uint8_t ch = 0) noexcept;

    /**
     * @brief Reset all channels' accumulators to zero.
     */
    void resetAll() noexcept;

    /**
     * @brief Disable automatic integration.
     *
     * Subsequent tick() calls become no-ops. wh() continues to return the
     * frozen value at the time of disable. Use enable() to re-arm.
     */
    void disable() noexcept { enabled_ = false; }

    /**
     * @brief Re-enable automatic integration.
     *
     * Counters retain their values; integration resumes on the next tick().
     */
    void enable() noexcept { enabled_ = true; }

    /**
     * @brief Query whether automatic integration is active.
     * @return true if enabled (default), false if disable() was called.
     */
    bool isEnabled() const noexcept { return enabled_; }

    /**
     * @brief Count of tick() calls where dt exceeded the 1 h sanity clamp.
     *
     * Increments when a snapshot arrives with @c master_dt_ms > 3 600 000
     * (loop paused, deep sleep, millis() wrap, debugger break). The Wh delta
     * is dropped to avoid silently injecting a spurious multi-hour spike, but
     * the user previously had no way to detect the drop. Reset via
     * @c resetDroppedDtCount().
     *
     * @return Count of dropped long-dt ticks since construction or last reset.
     */
    uint32_t droppedDtCount() const noexcept { return dropped_dt_count_; }

    /**
     * @brief Zero the dropped-dt diagnostic counter.
     */
    void resetDroppedDtCount() noexcept { dropped_dt_count_ = 0; }

private:
    double   wh_[3];              /**< Per-channel Wh accumulator. */
    bool     enabled_;            /**< Master switch for tick(). */
    uint32_t dropped_dt_count_;   /**< Diagnostic: tick() calls clamped on dt > 1 h. */
};

#endif /* RBAMP_ENERGY_H */
