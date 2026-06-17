/**
 * @file    RbAmpFleet.h
 * @brief   Multi-module manager for a bus of rbAmp sensors (v1.3).
 * @author  rbAmp team
 * @date    2026
 *
 * @details
 * RbAmpFleet is a thin manager layered over single-device RbAmp calls. It
 * owns an array of RbAmp device handles (max @c RBAMP_FLEET_MAX_MODULES) and
 * provides bus scan, batched polling, General-Call period synchronisation,
 * fleet-wide aggregation, and provisioning/address-assignment helpers.
 *
 * Every fleet operation delegates to the per-device RbAmp API — the manager
 * adds no new wire protocol, only orchestration. Failure isolation is a core
 * property: one unreachable module never aborts a batch (poll/aggregate/sync
 * skip the dead module and continue).
 *
 * @warning Wedged-bus hazard (L8): Arduino-ESP32's Wire wraps the same ESP-IDF
 *          i2c_master driver that can spin indefinitely on a marginal bus
 *          (weak pull-ups, long traces, EMI under load) BELOW this library —
 *          pollAll()/totalPower()/etc. can block and cannot be interrupted from
 *          the same task. The proven mitigation is a three-layer discipline,
 *          NOT a library timeout: (1) proper external ~4.7 kΩ pull-ups,
 *          (2) no debugger/NRST attached in production, (3) an app-level
 *          task-WDT on the polling task that reboots cleanly out of a wedge
 *          (feed it only after each fleet call returns). Severity is a
 *          full-duration-soak measurement, not a one-window guess.
 */
#ifndef RBAMP_FLEET_H
#define RBAMP_FLEET_H

#include <Arduino.h>
#include <Wire.h>
#include <stdint.h>
#include <stddef.h>

#include "RbAmp.h"
#include "RbAmpSnapshot.h"

/** Maximum modules a single fleet can track (members + excluded each capped). */
#ifndef RBAMP_FLEET_MAX_MODULES
#define RBAMP_FLEET_MAX_MODULES 16
#endif

/**
 * @brief Per-module result of checkSync() — General-Call latch verification.
 */
struct RbAmpFleetSync {
    uint8_t  addr;       /**< Module 7-bit address. */
    uint16_t gc_tick;    /**< REG_GC_TICK read back (0xFFFF = never received). */
    bool     in_sync;    /**< gc_tick == expected_tick. */
    bool     reachable;  /**< The GC_TICK read succeeded. */
};

/**
 * @brief Per-module result of pollAll() — one RT snapshot read attempt.
 */
struct RbAmpFleetPoll {
    uint8_t addr;      /**< Module 7-bit address. */
    bool    ok;        /**< readAll() succeeded for this module. */
    uint8_t channels;  /**< Channel count (mirrors the module's variant). */
};

/**
 * @class RbAmpFleet
 * @brief Manager for N rbAmp modules sharing one I2C bus.
 *
 * Construct with the shared @c Wire bus, then either scan() the bus for
 * modules or add() externally-owned RbAmp handles. The fleet owns (and frees)
 * handles it creates itself (scan/assignAddress); add()-ed handles stay
 * caller-owned.
 */
class RbAmpFleet {
public:
    /**
     * @brief Construct an empty fleet bound to a bus. No wire traffic.
     * @param[in] bus Shared Wire bus (caller must call bus.begin() first).
     */
    explicit RbAmpFleet(TwoWire& bus) noexcept;

    /** @brief Frees every fleet-owned member handle. add()-ed handles untouched. */
    ~RbAmpFleet() noexcept;

    /* Non-copyable — owns heap RbAmp instances. */
    RbAmpFleet(const RbAmpFleet&) = delete;
    RbAmpFleet& operator=(const RbAmpFleet&) = delete;

    /* ---- Membership / query ---- */

    /** @return Number of tracked members. */
    size_t count() const noexcept { return count_; }

    /** @return Member handle at @p idx, or @c nullptr if out of range. */
    RbAmp* get(size_t idx) const noexcept;

    /** @return Member handle whose live address == @p addr, or @c nullptr. */
    RbAmp* find(uint8_t addr) const noexcept;

    /**
     * @brief Adopt a caller-owned RbAmp handle (not freed on destroy).
     * @param[in] dev Begun RbAmp handle.
     * @return @c true on success; @c false if the address is already tracked,
     *         the fleet is full, or @p dev is null (see lastError()).
     */
    bool add(RbAmp* dev) noexcept;

    /** @return Number of addresses dropped as conflicts during scan(). */
    size_t excludedCount() const noexcept { return n_excluded_; }

    /** @return Excluded address at @p idx, or -1 if out of range. */
    int excludedAddr(size_t idx) const noexcept;

    /* ---- Scan ---- */

    /**
     * @brief Probe addresses 0x08..0x77, adopt every healthy rbAmp found.
     *
     * For each ACKing address runs a best-effort collision check (L7). A
     * conflict on a HEALTHY bus excludes that address (see excludedAddr()); a
     * conflict that also corrupts the known-empty canary address means the bus
     * itself is compromised (SDA stuck) — the fleet is emptied and the call
     * fails with RB_ERR_PARAM. Stops at RBAMP_FLEET_MAX_MODULES.
     *
     * @param[in]  match_product If true, only adopt modules whose PRODUCT_ID
     *                           reads 0x01 (rbAmp sensor).
     * @param[out] added         Number of modules newly adopted.
     * @return @c true on a healthy scan (even if 0 added); @c false if the bus
     *         was found compromised.
     */
    bool scan(bool match_product, size_t& added) noexcept;

    /* ---- Poll ---- */

    /**
     * @brief Read an RT snapshot from every member into caller arrays.
     *
     * One NACKing module sets its status.ok=false + zeroes its snapshot and the
     * loop continues — never aborts the batch.
     *
     * @param[out] snapshots Array of at least count() snapshots.
     * @param[out] status    Array of at least count() poll results.
     * @param[in]  cap        Capacity of both arrays.
     * @param[out] n_ok       Number of modules read successfully.
     * @return @c true on success; @c false if @p cap < count() or null args.
     */
    bool pollAll(RbAmpSnapshot* snapshots, RbAmpFleetPoll* status,
                 size_t cap, size_t& n_ok) noexcept;

    /* ---- General-Call fleet sync ---- */

    /**
     * @brief Enable GC latch reception on every member (persisted + reset).
     *
     * Per module: optionally setGroupId(@p group) (if non-zero), then
     * enableGc(true). Blocks ~1 s per module (save + reset). A per-module
     * failure is skipped (loop continues).
     *
     * @param[in]  group    Group id to assign (0 = leave group unchanged).
     * @param[out] ok_count Number of modules enabled successfully.
     * @return @c true (best-effort; check @p ok_count vs count()).
     */
    bool enableGcAll(uint8_t group, size_t& ok_count) noexcept;

    /**
     * @brief Broadcast a GC latch (group + tick), then settle.
     *
     * Thin wrapper over RbAmp::broadcastLatchGroup() + a delay. Follow with
     * checkSync() to verify which modules latched.
     *
     * @param[in] group     Group filter (0x00 = all-call).
     * @param[in] tick      16-bit window/tick stored in each latching module.
     * @param[in] settle_ms Delay after the broadcast before reads.
     * @return @c true if the frame was transmitted.
     */
    bool gcLatch(uint8_t group, uint16_t tick, uint16_t settle_ms) noexcept;

    /**
     * @brief Verify per-module GC sync by reading REG_GC_TICK on each member.
     *
     * @param[in]  expected_tick Tick value the last gcLatch() broadcast.
     * @param[out] status        Array of at least count() sync results.
     * @param[in]  cap           Capacity of @p status.
     * @param[out] n_missed      Modules unreachable OR out-of-sync.
     * @return @c true on success; @c false if @p cap < count() or null args.
     */
    bool checkSync(uint16_t expected_tick, RbAmpFleetSync* status,
                   size_t cap, size_t& n_missed) noexcept;

    /* ---- Aggregation ---- */

    /**
     * @brief Sum real power across every member × channel (W).
     * @param[out] out_w Total real power; a NACKing channel contributes 0.
     * @return @c true on success.
     */
    bool totalPower(float& out_w) noexcept;

    /**
     * @brief Sum the per-device Wh accumulators across the fleet.
     * @param[out] out_wh Total energy in Wh.
     * @return @c true on success.
     */
    bool totalEnergyWh(double& out_wh) noexcept;

    /**
     * @brief Poll the durable error flag (EVENT_ERROR) of every member.
     * @param[out] error_mask Bit i set if member index i has latched an error.
     * @param[out] n_errors   Number of members flagging an error.
     * @return @c true on success.
     */
    bool pollErrors(uint32_t& error_mask, size_t& n_errors) noexcept;

    /* ---- Provisioning / addressing ---- */

    /**
     * @brief Move a tracked module to a new address (two-phase commit).
     *
     * Refuses if @p new_addr is out of range, the source shows a collision, or
     * @p new_addr already ACKs on the bus. Verifies the move with a probe-retry
     * and that the old address vacates.
     *
     * @param[in] dev      A member handle (typically from get()/find()).
     * @param[in] new_addr Target 7-bit address.
     * @return @c true on confirmed move.
     */
    bool assignAddress(RbAmp* dev, uint8_t new_addr) noexcept;

    /**
     * @brief Non-destructive collision probe of one address.
     * @param[in]  addr      Address to check.
     * @param[out] collision @c true if a contention signature was detected.
     * @return @c true if the probe completed (collision out-param is the result).
     */
    bool checkConflict(uint8_t addr, bool& collision) noexcept;

    /**
     * @brief Provision a factory-fresh module (at 0x50) to a desired address.
     *
     * Standalone (no fleet membership). Probes the factory address 0x50,
     * collision-checks it, ensures @p desired_addr is free, performs the
     * two-phase commit, verifies the move (probe-retry + 0x50 vacated), and
     * optionally persists user config (repairs a fresh module's
     * FLASH_PARAMS_BAD). The caller owns @p out_dev (RbAmp::del via delete, or
     * hand to add()).
     *
     * @param[in]  bus          Shared Wire bus.
     * @param[in]  desired_addr Target address (0x08..0x77, != 0x50).
     * @param[in]  save_config  Persist user config after the move.
     * @param[out] out_dev      Newly-created handle bound to the new address.
     * @return @c true on confirmed provisioning.
     */
    static bool provision(TwoWire& bus, uint8_t desired_addr,
                          bool save_config, RbAmp*& out_dev) noexcept;

    /** @brief Last error code from any fleet operation (RB_OK / RB_ERR_*). */
    int8_t lastError() const noexcept { return last_error_; }

private:
    /* Best-effort two-signal collision detection (L7): (1) identity
     * cross-consistency — a U-variant SKU must carry the voltage capability
     * bit, an I-variant must not; the open-drain wired-AND of two different
     * variants yields an incoherent identity. (2) 8× live I0_RMS read —
     * conflict if >=2 reads are non-finite OR >=4 distinct finite values.
     * Blind to two IDLE IDENTICAL modules (clean stable AND) — the one-virgin-
     * at-a-time provisioning discipline is the real guard, this is the net.
     * Static — operates only on the device, reused by the static provision(). */
    static bool suspectCollision(RbAmp* dev) noexcept;

    int  append(RbAmp* dev, bool owned) noexcept;   /**< -1 if full. */
    void setError(int8_t e) noexcept { last_error_ = e; }
    static bool probeAddr(TwoWire& bus, uint8_t addr) noexcept;
    static bool probeRetry(TwoWire& bus, uint8_t addr) noexcept;  /**< 4× / 150 ms. */

    TwoWire& bus_;
    RbAmp*   dev_[RBAMP_FLEET_MAX_MODULES];
    bool     owned_[RBAMP_FLEET_MAX_MODULES];
    size_t   count_;
    uint8_t  excluded_[RBAMP_FLEET_MAX_MODULES];
    size_t   n_excluded_;
    int8_t   last_error_;
};

#endif /* RBAMP_FLEET_H */
