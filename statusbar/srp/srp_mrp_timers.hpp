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
// Timer durations come from a TimerConfig: IEEE 802.1Q-2018 Clause 10.7.11
// (Table 10-7) by default; a profile such as Milan sets its own.
//

#include "statusbar/sm/sm.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>

namespace statusbar::srp::mrp {

using sm::Clock;
using sm::TimePoint;

/// MRP timer durations; the defaults are mrp.c's, within IEEE 802.1Q-2018 Clause 10.7.4.
struct TimerConfig
{
    /// JoinTime (Clause 10.7.4.1): mrp.c's 100 ms, three join fires in 300 ms.
    std::chrono::milliseconds join_time{100};
    /// LeaveTime (Clause 10.7.4.2), 600-1000 ms.
    std::chrono::milliseconds leave_time{1000};
    /// The leavealltimer is drawn uniformly from [leave_all_time_min, leave_all_time_max):
    /// mrp.c's nominal LeaveAllTime (10 s) times [0.5, 1.5).
    std::chrono::milliseconds leave_all_time_min{5000};
    std::chrono::milliseconds leave_all_time_max{15000};
    /// The periodictimer (Clause 10.7.4.4).
    std::chrono::milliseconds periodic_time{1000};

    /// Every duration positive and the leavealltimer's range not empty.
    [[nodiscard]] constexpr auto valid() const noexcept -> bool
    {
        return join_time.count() > 0 && leave_time.count() > 0 && periodic_time.count() > 0 && leave_all_time_min.count() > 0 &&
            leave_all_time_max > leave_all_time_min;
    }

    friend auto operator==(TimerConfig const&, TimerConfig const&) -> bool = default;
};

/// Per-port MRP timer scheduler. Not thread-safe; a participant is
/// assumed to be driven from a single event loop thread.
class TimerScheduler
{
  public:
    /// Construct with @p config's durations. Pass a nonzero seed for deterministic tests; the
    /// default 0 seeds from std::random_device.
    explicit TimerScheduler(uint64_t seed = 0, TimerConfig const& config = {});

    /// The durations the timers are armed with.
    [[nodiscard]] auto config() const noexcept -> TimerConfig const& { return config_; }

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
    /// Uniform random interval in [leave_all_time_min, leave_all_time_max).
    [[nodiscard]] auto randomized_leaveall_duration() -> std::chrono::milliseconds;

    TimerConfig config_;
    std::optional<TimePoint> join_{};
    std::optional<TimePoint> leave_{};
    std::optional<TimePoint> leaveall_{};
    std::optional<TimePoint> periodic_{};
    std::mt19937_64 rng_;
};

}  // namespace statusbar::srp::mrp
