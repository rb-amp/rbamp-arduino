/**
 * @file    RbAmpEnergy.cpp
 * @brief   Implementation of master-side Wh accumulator.
 * @author  rbAmp team
 * @date    2026
 *
 * @see RbAmpEnergy.h, SPEC §7 (period-metering state machine).
 */
#include "RbAmpEnergy.h"

RbAmpEnergy::RbAmpEnergy() noexcept
    : wh_{0.0, 0.0, 0.0}, enabled_(true) {}

void RbAmpEnergy::tick(const RbAmpPeriodSnapshot& snap, uint8_t channels) noexcept {
    if (!enabled_ || !snap.valid) {
        return;
    }
    /* dt in seconds — master's wall-clock, NOT the chip's diagnostic latch_ms. */
    const double dt_s = static_cast<double>(snap.master_dt_ms) / 1000.0;
    if (dt_s <= 0.0) {
        /* First tick after primer-latch on begin(); no integration yet. */
        return;
    }
    /* Sanity clamp: a master_dt > 1 hour almost certainly means the loop was
     * paused (deep sleep, WiFi reconnect storm, debugger break) or the
     * millis() counter wrapped between two snapshots. Integrating avg_p over
     * that gap would silently inject a spurious multi-hour Wh delta. Drop the
     * sample and require the caller to manually fold in expected energy via
     * external accounting if needed. */
    if (dt_s > 3600.0) {
        return;
    }
    const uint8_t n = (channels > 3) ? 3 : channels;
    for (uint8_t ch = 0; ch < n; ++ch) {
        /* E_Wh += avg_p_W * dt_s / 3600  — see SPEC §7. */
        wh_[ch] += static_cast<double>(snap.avg_p[ch]) * dt_s / 3600.0;
    }
}

double RbAmpEnergy::wh(uint8_t ch) const noexcept {
    if (ch > 2) {
        return 0.0;
    }
    return wh_[ch];
}

void RbAmpEnergy::reset(uint8_t ch) noexcept {
    if (ch > 2) {
        return;
    }
    wh_[ch] = 0.0;
}

void RbAmpEnergy::resetAll() noexcept {
    wh_[0] = wh_[1] = wh_[2] = 0.0;
}
