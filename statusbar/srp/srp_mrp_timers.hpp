#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// Per-port MRP timer scheduler.
//
// Tracks four deadline-based timers — Join, Leave, LeaveAll, Periodic —
// and exposes next_deadline() for integration with any event loop. Three
// are the port's own; the Leave slot is the EARLIEST of the per-Registrar
// leavetimers (IEEE 802.1Q-2022 10.7.4.2 runs one per Registrar), which
// live on the attribute records and re-arm this slot as they expire.
// Monotonic std::chrono::steady_clock based; no OS timers, no threads.
//
// The participant is responsible for driving tick() at or after the
// deadlines it learns from next_deadline(). Expired timers are
// reported and then cleared; the caller decides whether to rearm.
//
// Timer durations follow OpenAvnu mrpd defaults (mrp.h:140-143), which
// match IEEE 802.1Q-2014 Clause 10.7.4 within the permitted ranges.
//

#include "statusbar/sm/sm.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>

namespace statusbar::srp::mrp {

using sm::Clock;
using sm::TimePoint;

/// Per-port MRP timer scheduler. Not thread-safe; a participant is
/// assumed to be driven from a single event loop thread.
class TimerScheduler
{
  public:
    /// JoinTime — mrp.h:140. The spec allows up to 3 join fires in
    /// 300 ms (10.7.4.1); mrp.c uses 100 ms to give 3 fires in 300 ms.
    static constexpr auto JOIN_TIME = std::chrono::milliseconds(100);

    /// LeaveTime — mrp.h:141, spec 10.7.4.3.
    static constexpr auto LEAVE_TIME = std::chrono::milliseconds(1000);

    /// LeaveAllTime (nominal) — mrp.h:142, spec 10.7.4.2.
    /// The actual interval is randomized in
    /// [LEAVE_ALL_TIME_NOMINAL / 2, LEAVE_ALL_TIME_NOMINAL * 3 / 2).
    static constexpr auto LEAVE_ALL_TIME_NOMINAL = std::chrono::milliseconds(10000);

    /// PeriodicTransmissionTime — mrp.h:143, spec 10.7.5.23.
    static constexpr auto PERIODIC_TIME = std::chrono::milliseconds(1000);

    /// Construct. Pass a nonzero seed for deterministic tests; the
    /// default 0 seeds from std::random_device.
    explicit TimerScheduler(uint64_t seed = 0);

    // --- Idempotent starts --------------------------------------------
    // If the timer is already running, these are a no-op (deadline is
    // preserved). Mirrors mrp.c's start-if-not-running pattern.
    void start_join(TimePoint now) noexcept;
    void start_leaveall(TimePoint now);
    void start_periodic(TimePoint now) noexcept;

    // --- The Registrars' leavetimers ------------------------------------
    // A Registrar's leavetimer started at @p now, or one with a known
    // @p deadline: the slot keeps the earliest, so tick() reports when the
    // first Registrar is due and the participant looks at each record's
    // own deadline to see which.
    void start_leave(TimePoint now) noexcept;
    void start_leave_at(TimePoint deadline) noexcept;

    // --- Force rearm --------------------------------------------------
    // Unconditionally (re)sets the deadline, even if already running.
    void arm_join(TimePoint now) noexcept;
    void arm_leave(TimePoint now) noexcept;
    void arm_leaveall(TimePoint now);
    void arm_periodic(TimePoint now) noexcept;

    // --- Stops --------------------------------------------------------
    void stop_join() noexcept { join_.reset(); }
    void stop_leave() noexcept { leave_.reset(); }
    void stop_leaveall() noexcept { leaveall_.reset(); }
    void stop_periodic() noexcept { periodic_.reset(); }

    // --- Queries ------------------------------------------------------
    [[nodiscard]] auto join_running() const noexcept -> bool { return join_.has_value(); }
    [[nodiscard]] auto leave_running() const noexcept -> bool { return leave_.has_value(); }
    [[nodiscard]] auto leaveall_running() const noexcept -> bool { return leaveall_.has_value(); }
    [[nodiscard]] auto periodic_running() const noexcept -> bool { return periodic_.has_value(); }

    /// Earliest deadline across all running timers.
    /// Returns TimePoint::max() if none are running (caller can wait
    /// indefinitely for external events).
    [[nodiscard]] auto next_deadline() const noexcept -> TimePoint;

    /// Report of which timers expired during the most recent tick().
    struct Expired
    {
        bool join{false};
        bool leave{false};
        bool leaveall{false};
        bool periodic{false};

        [[nodiscard]] auto any() const noexcept -> bool { return join || leave || leaveall || periodic; }
    };

    /// Check all running timers against `now`. Any timer whose
    /// deadline has been reached is reported AND cleared (i.e. becomes
    /// not-running). The caller must arm or restart as appropriate
    /// based on FSM outputs after dispatching the expiry events.
    [[nodiscard]] auto tick(TimePoint now) noexcept -> Expired;

  private:
    /// Uniform random interval in [LEAVE_ALL_TIME_NOMINAL/2, 3*LEAVE_ALL_TIME_NOMINAL/2).
    /// See mrp.c:412-428 for the reference formula.
    auto randomized_leaveall_duration() -> std::chrono::milliseconds;

    std::optional<TimePoint> join_{};
    std::optional<TimePoint> leave_{};
    std::optional<TimePoint> leaveall_{};
    std::optional<TimePoint> periodic_{};
    std::mt19937_64 rng_;
};

}  // namespace statusbar::srp::mrp
