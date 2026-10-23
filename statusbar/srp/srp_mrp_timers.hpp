#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// Per-port MRP timer scheduler.
//
// Tracks four deadline-based timers — Join, Leave, LeaveAll, Periodic —
// and exposes next_deadline() for integration with any event loop.
// Monotonic std::chrono::steady_clock based; no OS timers, no threads.
//
// The participant is responsible for driving tick() at or after the
// deadlines it learns from next_deadline(). Expired timers are
// reported and then cleared; the caller decides whether to rearm.
//
// Timer durations are the Milan v1.2 defaults (Section 4.2.7.1.1,
// Table 4.3: joinTime 200 ms [180, 240], LeaveTime 5000 ms [4500, 7500],
// leavealltimer 10-15 s [9.5, 15.5], periodictimer 1000 ms [900, 1500]).
// Each value is also inside the IEEE 802.1Q-2018 Clause 10.7.11 ranges, so
// classic MRP peers interoperate unchanged.
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
    /// JoinTime — Milan v1.2 Table 4.3 default 200 ms (802.1Q 10.7.4.1).
    static constexpr auto JOIN_TIME = std::chrono::milliseconds(200);

    /// LeaveTime: Milan v1.2 Table 4.3 default 5000 ms (IEEE 802.1Q 10.7.4.2), paired with the MSRP
    /// IN/rLv! -> MT rule (Milan v1.2 Section 4.2.7.2.2) so an explicit withdrawal is seen at once.
    static constexpr auto LEAVE_TIME = std::chrono::milliseconds(5000);

    /// LeaveAllTime: Milan v1.2 Table 4.3 "10-15 s" (IEEE 802.1Q 10.7.4.3); each period is drawn
    /// uniformly from [LEAVE_ALL_TIME_MIN, LEAVE_ALL_TIME_MAX).
    static constexpr auto LEAVE_ALL_TIME_MIN = std::chrono::milliseconds(10000);
    static constexpr auto LEAVE_ALL_TIME_MAX = std::chrono::milliseconds(15000);

    /// PeriodicTransmissionTime — Milan v1.2 Table 4.3 default 1000 ms
    /// (802.1Q 10.7.5.23).
    static constexpr auto PERIODIC_TIME = std::chrono::milliseconds(1000);

    /// Construct. Pass a nonzero seed for deterministic tests; the
    /// default 0 seeds from std::random_device.
    explicit TimerScheduler(uint64_t seed = 0);

    // --- Idempotent starts --------------------------------------------
    // If the timer is already running, these are a no-op (deadline is
    // preserved). Mirrors mrp.c's start-if-not-running pattern.
    void start_join(TimePoint now) noexcept;
    void start_leave(TimePoint now) noexcept;
    void start_leaveall(TimePoint now);
    void start_periodic(TimePoint now) noexcept;

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
    /// Uniform random interval in [LEAVE_ALL_TIME_MIN, LEAVE_ALL_TIME_MAX).
    auto randomized_leaveall_duration() -> std::chrono::milliseconds;

    std::optional<TimePoint> join_{};
    std::optional<TimePoint> leave_{};
    std::optional<TimePoint> leaveall_{};
    std::optional<TimePoint> periodic_{};
    std::mt19937_64 rng_;
};

}  // namespace statusbar::srp::mrp
