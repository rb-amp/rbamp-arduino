/**
 * @file    RbAmpFleet.cpp
 * @brief   Implementation of the multi-module rbAmp manager (v1.3).
 * @see     RbAmpFleet.h
 */
#include "RbAmpFleet.h"

#include <string.h>  /* memset */
#include <math.h>    /* isnan, isinf */

using namespace rbamp;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

RbAmpFleet::RbAmpFleet(TwoWire& bus) noexcept
    : bus_(bus), count_(0), n_excluded_(0), last_error_(RB_OK) {
    for (size_t i = 0; i < RBAMP_FLEET_MAX_MODULES; ++i) {
        dev_[i] = nullptr;
        owned_[i] = false;
    }
}

RbAmpFleet::~RbAmpFleet() noexcept {
    for (size_t i = 0; i < count_; ++i) {
        if (owned_[i] && dev_[i] != nullptr) {
            delete dev_[i];
        }
    }
}

/* ============================================================================
 * Membership / query
 * ============================================================================ */

RbAmp* RbAmpFleet::get(size_t idx) const noexcept {
    return (idx < count_) ? dev_[idx] : nullptr;
}

RbAmp* RbAmpFleet::find(uint8_t addr) const noexcept {
    for (size_t i = 0; i < count_; ++i) {
        if (dev_[i] != nullptr && dev_[i]->address() == addr) {
            return dev_[i];
        }
    }
    return nullptr;
}

int RbAmpFleet::excludedAddr(size_t idx) const noexcept {
    return (idx < n_excluded_) ? static_cast<int>(excluded_[idx]) : -1;
}

int RbAmpFleet::append(RbAmp* dev, bool owned) noexcept {
    if (count_ >= RBAMP_FLEET_MAX_MODULES) return -1;
    const int idx = static_cast<int>(count_);
    dev_[idx] = dev;
    owned_[idx] = owned;
    ++count_;
    return idx;
}

bool RbAmpFleet::add(RbAmp* dev) noexcept {
    if (dev == nullptr) {
        setError(RB_ERR_PARAM);
        return false;
    }
    if (find(dev->address()) != nullptr) {
        setError(RB_ERR_PARAM);  /* already tracked */
        return false;
    }
    if (append(dev, false) < 0) {
        setError(RB_ERR_PARAM);  /* fleet full */
        return false;
    }
    setError(RB_OK);
    return true;
}

/* ============================================================================
 * Low-level bus probe
 * ============================================================================ */

bool RbAmpFleet::probeAddr(TwoWire& bus, uint8_t addr) noexcept {
    bus.beginTransmission(addr);
    return bus.endTransmission() == 0;
}

bool RbAmpFleet::probeRetry(TwoWire& bus, uint8_t addr) noexcept {
    for (uint8_t i = 0; i < 4; ++i) {
        if (probeAddr(bus, addr)) return true;
        delay(150);
    }
    return false;
}

/* ============================================================================
 * Collision detection (L7) — best-effort, two-signal
 * ============================================================================ */

bool RbAmpFleet::suspectCollision(RbAmp* dev) noexcept {
    if (dev == nullptr) return false;

    /* Signal 1 — identity cross-consistency. A U-variant SKU (UI1..UI3) must
     * carry the voltage capability bit (CAP_ZC_PHASE_OFFSET); an I-variant
     * (I1..I3) must not. Two different variants wired-AND'd on the open-drain
     * bus yield an incoherent identity (variant says U, ANDed capability says
     * no voltage, or vice-versa). Deterministic — not a glitch. */
    const RbAmpVariant v = dev->readVariant();
    uint16_t cap = 0;
    const bool cap_ok = dev->readCapability(cap);
    if (v != RbAmpVariant::Unknown && cap_ok) {
        const bool is_u = (v == RbAmpVariant::UI1 ||
                           v == RbAmpVariant::UI2 ||
                           v == RbAmpVariant::UI3);
        const bool has_voltage_cap = (cap & v2::CAP_ZC_PHASE_OFFSET) != 0;
        if (is_u != has_voltage_cap) {
            return true;
        }
    }

    /* Signal 2 — 8× live I0_RMS. Two contending slaves clock a corrupt
     * wired-AND: conflict if >=2 reads are non-finite OR >=4 distinct finite
     * values appear. Blind to two IDLE IDENTICAL modules (clean stable AND);
     * the one-virgin-at-a-time provisioning discipline is the real guard. */
    uint8_t nonfinite = 0;
    float seen[8];
    uint8_t n_seen = 0;
    for (uint8_t i = 0; i < 8; ++i) {
        /* Space the samples across distinct RT-refresh windows (~200 ms). Without
         * this the 8 reads return one cached value (n_seen==1) and signal 2 is
         * defeated. ~15 ms × 8 ≈ 120 ms, under one refresh — matches reference. */
        if (i != 0) delay(15);
        const float c = dev->readCurrent(0);
        if (isnan(c) || isinf(c)) {
            ++nonfinite;
            continue;
        }
        bool dup = false;
        for (uint8_t j = 0; j < n_seen; ++j) {
            if (seen[j] == c) { dup = true; break; }
        }
        if (!dup && n_seen < 8) seen[n_seen++] = c;
    }
    if (nonfinite >= 2) return true;
    if (n_seen >= 4) return true;
    return false;
}

/* ============================================================================
 * Scan
 * ============================================================================ */

bool RbAmpFleet::scan(bool match_product, size_t& added) noexcept {
    added = 0;
    bool have_canary = false;
    uint8_t canary = 0;

    for (uint16_t a = 0x08; a <= 0x77; ++a) {
        if (count_ >= RBAMP_FLEET_MAX_MODULES) break;
        const uint8_t addr = static_cast<uint8_t>(a);
        if (find(addr) != nullptr) continue;  /* already tracked */

        if (!probeAddr(bus_, addr)) {
            if (!have_canary) { canary = addr; have_canary = true; }  /* known-empty witness */
            continue;
        }

        /* ACK — create a handle to identity-check and (maybe) adopt. */
        RbAmp* d = new RbAmp(bus_, addr);
        if (d == nullptr) {
            setError(RB_ERR_PARAM);
            return false;
        }
        if (!d->begin()) {        /* version probe failed → not an rbAmp */
            delete d;
            continue;
        }

        if (suspectCollision(d)) {
            delete d;
            /* Tier 2 — if the known-empty canary now ACKs, SDA is stuck: the
             * whole bus is compromised. Empty the fleet and fail loudly. */
            if (have_canary && probeAddr(bus_, canary)) {
                for (size_t i = 0; i < count_; ++i) {
                    if (owned_[i] && dev_[i] != nullptr) delete dev_[i];
                }
                count_ = 0;
                n_excluded_ = 0;
                added = 0;
                setError(RB_ERR_PARAM);
                return false;
            }
            /* Tier 1 — healthy bus, isolated conflict: exclude this address. */
            if (n_excluded_ < RBAMP_FLEET_MAX_MODULES) {
                excluded_[n_excluded_++] = addr;
            }
            continue;
        }

        if (match_product && d->readProductId() != 0x01) {
            delete d;
            continue;
        }

        if (append(d, true) < 0) {
            delete d;
            break;  /* fleet full */
        }
        ++added;
    }

    setError(RB_OK);
    return true;
}

/* ============================================================================
 * Poll
 * ============================================================================ */

bool RbAmpFleet::pollAll(RbAmpSnapshot* snapshots, RbAmpFleetPoll* status,
                         size_t cap, size_t& n_ok) noexcept {
    n_ok = 0;
    if (snapshots == nullptr || status == nullptr) {
        setError(RB_ERR_PARAM);
        return false;
    }
    if (cap < count_) {
        setError(RB_ERR_PARAM);  /* caller arrays too small */
        return false;
    }
    for (size_t i = 0; i < count_; ++i) {
        RbAmp* d = dev_[i];
        status[i].addr = d->address();
        status[i].channels = d->channels();
        const bool ok = d->readAll(snapshots[i]);
        status[i].ok = ok;
        if (ok) {
            ++n_ok;
        } else {
            memset(&snapshots[i], 0, sizeof(RbAmpSnapshot));  /* one dead module never aborts the batch */
        }
    }
    setError(RB_OK);
    return true;
}

/* ============================================================================
 * General-Call fleet sync
 * ============================================================================ */

bool RbAmpFleet::enableGcAll(uint8_t group, size_t& ok_count) noexcept {
    ok_count = 0;
    for (size_t i = 0; i < count_; ++i) {
        RbAmp* d = dev_[i];
        if (group != 0 && !d->setGroupId(group)) continue;
        if (d->enableGc(true)) ++ok_count;
    }
    setError(RB_OK);
    return true;
}

bool RbAmpFleet::gcLatch(uint8_t group, uint16_t tick, uint16_t settle_ms) noexcept {
    const bool ok = RbAmp::broadcastLatchGroup(bus_, group, tick);
    delay(settle_ms);
    setError(ok ? RB_OK : RB_ERR_IO);
    return ok;
}

bool RbAmpFleet::checkSync(uint16_t expected_tick, RbAmpFleetSync* status,
                           size_t cap, size_t& n_missed) noexcept {
    n_missed = 0;
    if (status == nullptr) {
        setError(RB_ERR_PARAM);
        return false;
    }
    if (cap < count_) {
        setError(RB_ERR_PARAM);
        return false;
    }
    for (size_t i = 0; i < count_; ++i) {
        RbAmp* d = dev_[i];
        status[i].addr = d->address();
        uint16_t t = 0xFFFF;
        const bool reach = d->readGcTick(t);
        status[i].reachable = reach;
        status[i].gc_tick = reach ? t : 0xFFFF;
        status[i].in_sync = reach && (t == expected_tick);
        if (!status[i].in_sync) ++n_missed;
    }
    setError(RB_OK);
    return true;
}

/* ============================================================================
 * Aggregation
 * ============================================================================ */

bool RbAmpFleet::totalPower(float& out_w) noexcept {
    float sum = 0.0f;
    for (size_t i = 0; i < count_; ++i) {
        RbAmp* d = dev_[i];
        const uint8_t ch = d->channels();
        for (uint8_t c = 0; c < ch; ++c) {
            const float p = d->readPower(c);
            if (!isnan(p)) sum += p;  /* a NACKing channel contributes 0 */
        }
    }
    out_w = sum;
    setError(RB_OK);
    return true;
}

bool RbAmpFleet::totalEnergyWh(double& out_wh) noexcept {
    double sum = 0.0;
    for (size_t i = 0; i < count_; ++i) {
        RbAmp* d = dev_[i];
        const uint8_t ch = d->channels();
        for (uint8_t c = 0; c < ch; ++c) {
            sum += d->energy().wh(c);
        }
    }
    out_wh = sum;
    setError(RB_OK);
    return true;
}

bool RbAmpFleet::pollErrors(uint32_t& error_mask, size_t& n_errors) noexcept {
    error_mask = 0;
    n_errors = 0;
    for (size_t i = 0; i < count_; ++i) {
        bool err = false;
        if (dev_[i]->hasError(err) && err) {
            if (i < 32) error_mask |= (static_cast<uint32_t>(1) << i);
            ++n_errors;
        }
    }
    setError(RB_OK);
    return true;
}

/* ============================================================================
 * Provisioning / addressing
 * ============================================================================ */

bool RbAmpFleet::assignAddress(RbAmp* dev, uint8_t new_addr) noexcept {
    if (dev == nullptr || new_addr < 0x08 || new_addr > 0x77) {
        setError(RB_ERR_PARAM);
        return false;
    }
    const uint8_t old_addr = dev->address();
    if (new_addr == old_addr) {
        setError(RB_OK);
        return true;  /* no-op */
    }
    if (suspectCollision(dev)) {
        setError(RB_ERR_PARAM);
        return false;
    }
    if (probeAddr(bus_, new_addr)) {
        setError(RB_ERR_PARAM);  /* target already occupied */
        return false;
    }
    if (!dev->prepareAddressChange(new_addr) || !dev->commitAddressChange()) {
        setError(dev->lastError());
        return false;
    }
    if (!probeRetry(bus_, new_addr)) {
        setError(RB_ERR_NON_PHYSICAL);  /* device did not appear at new addr */
        return false;
    }
    if (probeAddr(bus_, old_addr)) {
        setError(RB_ERR_PARAM);  /* old address did not vacate */
        return false;
    }
    setError(RB_OK);
    return true;
}

bool RbAmpFleet::checkConflict(uint8_t addr, bool& collision) noexcept {
    collision = false;
    if (!probeAddr(bus_, addr)) {
        setError(RB_OK);
        return true;  /* NACK → nothing there → no collision */
    }
    RbAmp* d = new RbAmp(bus_, addr);
    if (d == nullptr) {
        setError(RB_ERR_PARAM);
        return false;
    }
    collision = suspectCollision(d);  /* read-only; no begin() primer write */
    delete d;
    setError(RB_OK);
    return true;
}

bool RbAmpFleet::provision(TwoWire& bus, uint8_t desired_addr,
                           bool save_config, RbAmp*& out_dev) noexcept {
    out_dev = nullptr;
    if (desired_addr < 0x08 || desired_addr > 0x77 || desired_addr == 0x50) {
        return false;  /* invalid target (0x50 is the factory address) */
    }
    if (!probeAddr(bus, 0x50)) {
        return false;  /* no fresh module present */
    }
    if (probeAddr(bus, desired_addr)) {
        return false;  /* target already occupied */
    }

    RbAmp* d = new RbAmp(bus, 0x50);
    if (d == nullptr) return false;
    if (!d->begin()) { delete d; return false; }

    /* Collision-check the factory address BEFORE committing — two fresh
     * modules both at 0x50 is the provisioning hazard L7 guards against. */
    if (suspectCollision(d)) { delete d; return false; }

    if (!d->prepareAddressChange(desired_addr) || !d->commitAddressChange()) {
        delete d;
        return false;
    }
    /* Positive confirmation: new addr appears AND 0x50 vacates. */
    if (!probeRetry(bus, desired_addr)) { delete d; return false; }
    if (probeAddr(bus, 0x50))          { delete d; return false; }

    if (save_config) {
        d->saveUserConfig();  /* repairs fresh-module FLASH_PARAMS_BAD; non-fatal */
    }
    out_dev = d;  /* caller owns */
    return true;
}
