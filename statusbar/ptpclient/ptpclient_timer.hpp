#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// PTP Timer module
/// Provides PtpTimer class for periodic wake-ups synchronized to PTP time

#include "statusbar/itc/itc_telemetry_counter.hpp"
#include "statusbar/ptpclient/ptpclient_base.hpp"
#include "statusbar/ptpclient/ptpclient_bridge.hpp"
#include "statusbar/realtime/realtime.hpp"
#include "statusbar/realtime/realtime_clock.hpp"
#include "statusbar/status/catch_or_status.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <atomic>
#include <climits>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace statusbar::ptpclient {

using statusbar::failure;
using statusbar::Status;
using statusbar::StatusValue;
using statusbar::success;

//
// PtpTimerBase - Non-template base class for cross-thread state + telemetry
//

/// Base class that holds the atomic running flag and the recovery /
/// missed-cycle TelemetryCounters with out-of-line implementations,
/// to prevent inlining issues across module boundaries (LLVM Issue #172241)
class PtpTimerBase
{
  public:
    PtpTimerBase() noexcept;

  protected:
    // Running state
    [[nodiscard]] auto is_running_atomic() const noexcept -> bool;
    auto exchange_running(bool value) noexcept -> bool;
    auto set_running(bool value) noexcept -> void;

    // Recovery count
    [[nodiscard]] auto load_recovery_count() const noexcept -> int64_t;
    auto increment_recovery_count() noexcept -> void;
    auto reset_recovery_count() noexcept -> void;

    // Missed cycles
    [[nodiscard]] auto load_missed_cycles() const noexcept -> int64_t;
    auto add_missed_cycles(int64_t count) noexcept -> void;
    auto reset_missed_cycles() noexcept -> void;

    // For passing to monitor threads that need to stop when timer stops
    [[nodiscard]] auto running_flag_ptr() noexcept -> std::atomic<bool>* { return &running_; }

  private:
    std::atomic<bool> running_;
    statusbar::itc::TelemetryCounter<int64_t> recovery_count_{};
    statusbar::itc::TelemetryCounter<int64_t> missed_cycles_{};
};

//
// Timer Wake Info
//

/// Information passed to callback on each timer wake: the wake told on each
/// clock the callback may care about, so nothing it calls reads a clock.
struct TimerWakeInfo
{
    realtime::GptpClock0::time_point scheduled_time;  ///< Target wake time, on the PHC's clock
    realtime::GptpClock0::time_point actual_time;     ///< Actual wake time, on the PHC's clock
    realtime::Nanoseconds error;                      ///< actual - scheduled (positive = late, negative = early)
    int64_t wake_count;                               ///< Total successful wakes so far
    /// The wake instant on CLOCK_MONOTONIC_RAW: the one reading actual_time
    /// was converted from. Typed, so it can only be told to something that
    /// takes the RAW clock (an RX drain, which tells it to the port).
    realtime::MonotonicRawClock::time_point actual_raw{};

    /// What was left of the spin window (ptp.spin_ns) when the kernel sleep
    /// before this wake returned: the window less how late the kernel woke
    /// the thread. Near the whole window on a quiet core; at or below zero
    /// the sleep overran the window and the wake was late by that much.
    /// Empty when the wake did not sleep (its deadline was already inside
    /// the window, or behind).
    std::optional<realtime::Nanoseconds> spin_headroom{};

    /// The same three as bare nanosecond counts, for a callback that prints
    /// or records them.
    [[nodiscard]] constexpr auto scheduled_time_ns() const noexcept -> int64_t
    {
        return realtime::GptpClock0::to_ns(scheduled_time);
    }
    [[nodiscard]] constexpr auto actual_time_ns() const noexcept -> int64_t { return realtime::GptpClock0::to_ns(actual_time); }
    [[nodiscard]] constexpr auto error_ns() const noexcept -> int64_t { return error.count(); }
};

//
// PTP Timer
//

/// Periodic timer synchronized to PTP time
///
/// @tparam Callback Callable type invoked on each wake with StatusValue<TimerWakeInfo> const&
///                  Can be a lambda, function pointer, or any callable object.
///
/// Usage:
///   PtpTimeBridge bridge;
///   auto guard = bridge.start_sampling(ptp_client, params);
///   // ... wait for healthy ...
///
///   auto timer = make_ptp_timer(bridge, 1'000'000,  // 1ms period
///       [](StatusValue<TimerWakeInfo> const& info) {
///           if (info) {
///               // Handle wake at info->scheduled_time
///           } else {
///               // Handle error
///           }
///       });
///   timer.start();
///   // ... do other work ...
///   timer.stop();
template <typename Callback>
class PtpTimer : public PtpTimerBase
{
    static_assert(
        std::is_invocable_v<Callback, StatusValue<TimerWakeInfo> const&>,
        "Callback must be invocable with StatusValue<TimerWakeInfo> const&");

  public:
    /// Construct a PTP timer
    /// @param bridge Reference to PTP time bridge (must remain valid while timer is running)
    /// @param period_ns Wake period in nanoseconds
    /// @param callback Function called on each wake with StatusValue<TimerWakeInfo>
    /// @param compensation_ns Compensation offset in nanoseconds (negative = wake earlier)
    /// @param enable_realtime Enable realtime thread priority (SCHED_FIFO) for timer thread
    /// @param cpu_affinity CPU to pin timer thread to (-1 to disable)
    /// @param threshold Wake-jitter threshold in ns. Currently unused (the ftrace
    ///        tripwire that consumed it was removed); retained for API stability.
    PtpTimer(
        PtpTimeBridge& bridge,
        int64_t period_ns,
        Callback callback,
        int64_t compensation_ns = 0,
        bool enable_realtime = true,
        int cpu_affinity = 3,
        int64_t threshold = 50'000) noexcept(std::is_nothrow_move_constructible_v<Callback>)
        : bridge_{bridge}
        , period_ns_{period_ns}
        , compensation_ns_{compensation_ns}
        , enable_realtime_{enable_realtime}
        , cpu_affinity_{cpu_affinity}
        , threshold_{threshold}
        , callback_{std::move(callback)}
    {}

    ~PtpTimer() noexcept { stop(); }

    // No copy/move (has thread and references)
    PtpTimer(PtpTimer const&) = delete;
    auto operator=(PtpTimer const&) -> PtpTimer& = delete;
    PtpTimer(PtpTimer&&) = delete;
    auto operator=(PtpTimer&&) -> PtpTimer& = delete;

    /// Prefault `bytes` of the timer thread's stack before its loop, so the
    /// first deep calls in it fault on nothing (realtime::prefault_stack()).
    /// 0, the default, prefaults nothing. Set it before start().
    auto set_stack_prefault(size_t const bytes) noexcept -> void { stack_prefault_bytes_ = bytes; }

    /// Stack bytes prefaulted before the loop; 0 for none.
    [[nodiscard]] auto stack_prefault_bytes() const noexcept -> size_t { return stack_prefault_bytes_; }

    /// Lock the process's memory once more from the timer thread, before its
    /// loop (mlockall(MCL_CURRENT)). A process that locks its memory at
    /// start-up does so before this thread exists, and MCL_CURRENT takes in
    /// only what is mapped at that moment: the timer thread's own stack and
    /// thread-local storage, made when the thread is, are then left unlocked,
    /// and the kernel may migrate their pages (memory compaction) or swap them
    /// out. A touch of a page that is being migrated waits for the migration,
    /// which on a small host under memory pressure was seen to take 0.2 to
    /// 2.6 ms in the middle of a callback. Off by default; set it before
    /// start() wherever the process locks its memory.
    auto set_lock_memory(bool const lock) noexcept -> void { lock_memory_ = lock; }

    /// Whether the timer thread locks the process's memory before its loop.
    [[nodiscard]] auto locks_memory() const noexcept -> bool { return lock_memory_; }

    /// Start the timer
    /// @return Success or error if timer is already running
    [[nodiscard]] auto start() -> Status
    {
        // A non-positive period would divide by zero in the catch-up path
        // (periods_behind = ... / period_ns_). Refuse rather than SIGFPE.
        if (period_ns_ <= 0) {
            return failure(PtpError::invalid_period);
        }
        if (exchange_running(true)) {
            // Already running
            return failure(PtpError::bridge_not_running);  // Reuse error code
        }

        stats_.reset();
        headroom_.reset();
        reset_recovery_count();
        reset_missed_cycles();

        timer_thread_ = std::thread([this]() -> void { timer_loop(); });

        return success();
    }

    /// Stop the timer (blocks until thread joins)
    void stop() noexcept
    {
        bool const was_running = exchange_running(false);
        if (was_running && timer_thread_.joinable()) {
            timer_thread_.join();
        }
    }

    /// Check if timer is running
    [[nodiscard]] auto is_running() const noexcept -> bool { return is_running_atomic(); }

    /// Wake statistics (thread-safe copy): error = actual minus scheduled wake (positive = late),
    /// duration = callback time per wake, skipped_counts = periods skipped by a late wake.
    [[nodiscard]] auto stats() const noexcept -> stats::AtomicWakeStats::Snapshot { return stats_.snapshot(); }

    /// Get configured period in nanoseconds
    [[nodiscard]] auto period_ns() const noexcept -> int64_t { return period_ns_; }

    /// Get configured compensation in nanoseconds
    [[nodiscard]] auto compensation_ns() const noexcept -> int64_t { return compensation_ns_; }

    /// Check if realtime priority is enabled
    [[nodiscard]] auto realtime_enabled() const noexcept -> bool { return enable_realtime_; }

    /// Get the number of times the timer transitioned from healthy to unhealthy
    [[nodiscard]] auto recovery_count() const noexcept -> int64_t { return load_recovery_count(); }

    /// Get the total number of missed cycles (periods skipped due to running behind)
    [[nodiscard]] auto missed_cycles() const noexcept -> int64_t { return load_missed_cycles(); }

    /// Wake headroom (thread-safe copy): per wake that slept, what was left of the spin window
    /// (BridgeSamplingParams::wake_refine_spin_ns) when the sleep returned; negative = overran.
    /// The OS wake latency is the window minus the headroom.
    [[nodiscard]] auto wake_headroom() const noexcept -> stats::AtomicTimeStats::Snapshot { return headroom_.snapshot(); }

    /// Name the timer thread (default "media"; Linux keeps 15 characters, the
    /// rest is dropped). Copied; applies from the next start(). Call it
    /// while the timer is stopped.
    void set_thread_name(std::string_view const name) noexcept
    {
        thread_name_ = {};
        name.copy(thread_name_.data(), thread_name_.size() - 1);
    }

  private:
    using PtpTimePoint = realtime::GptpClock0::time_point;

    /// The schedule after a wake at @p actual_wake: the next period, or,
    /// when that is already behind the wake, the first period boundary still
    /// ahead, the skipped ones counted as missed cycles. Returns how many.
    auto advance_schedule(int64_t const actual_wake_ptp, int64_t& next_wake) noexcept -> int64_t
    {
        int64_t periods_behind = 0;
        next_wake += period_ns_;
        if (actual_wake_ptp > next_wake) {
            periods_behind = ((actual_wake_ptp - next_wake) / period_ns_) + 1;
            next_wake += periods_behind * period_ns_;
            add_missed_cycles(periods_behind);
        }
        return periods_behind;
    }

    /// Process a successful wake: update stats, call the callback with the
    /// wake told on both clocks, advance the schedule; the callback's duration
    /// runs on RAW from the wake.
    /// @return wake error in nanoseconds
    auto process_wake(PtpTimeBridge::WakeTimes const& wake, int64_t& next_wake) -> int64_t
    {
        int64_t const error_ns = wake.ptp_ns - next_wake;
        stats_.update(error_ns);
        callback_(success(TimerWakeInfo{
            .scheduled_time_ns = next_wake,
            .actual_time_ns = wake.ptp_ns,
            .error_ns = error_ns,
            .wake_count = stats_.count(),
            .actual_raw = realtime::MonotonicRawClock::from_ns(wake.raw_ns)}));
        int64_t const periods_behind = advance_schedule(wake.ptp_ns, next_wake);
        stats_.update_duration(realtime::read_monotonic_raw_ns() - wake.raw_ns, periods_behind);
        return error_ns;
    }

    void timer_loop()
    {
        run_guarded("PTP timer thread", [this]() { timer_loop_impl(); });
    }

    /// Lock the process's memory from the timer thread (set_lock_memory).
    /// The thread allocates once first: a thread's first allocation is what
    /// makes its allocator arena, a mapping of its own in glibc, and the lock
    /// takes in only the mappings that exist when it is called. Locked here,
    /// the arena stays locked as it grows.
    static void lock_thread_memory() noexcept
    {
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,hicpp-no-malloc)
        void* volatile first_allocation = std::malloc(64);
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,hicpp-no-malloc)
        std::free(first_allocation);
        (void)realtime::lock_memory_ex();
    }

    /// Make the timer thread ready for its loop: stack prefault, the memory
    /// lock that takes in this thread's stack (set_lock_memory), CPU
    /// affinity, SCHED_FIFO priority. The last two log their failure and
    /// the loop runs anyway.
    void prepare_timer_thread()
    {
        realtime::prefault_stack(stack_prefault_bytes_);
        realtime::set_current_thread_name(thread_name_.data());
        // Set realtime affinity if set (log failure but continue)
        if (cpu_affinity_ >= 0) {
            (void)realtime::set_realtime_affinity_logged(cpu_affinity_, "PTP timer");
        }
        // Set realtime priority if enabled (log failure but continue). Priority
        // 49: below kernel IRQ threads (@50) and far below migration/RCU (@99);
        // matches the realtime_timer_config default. Max priority would contend
        // with critical per-CPU kernel threads on the isolated core.
        if (enable_realtime_) {
            (void)realtime::set_realtime_priority_logged(49, "PTP timer");
        }
    }

    void timer_loop_impl()
    {
        prepare_timer_thread();

        // Do NOT block waiting for sync at startup. Enter the loop immediately;
        // while the bridge is unhealthy (no sync yet, or sync lost mid-run) we
        // report "no sync" via the callback and retry, rather than exiting. When
        // sync arrives (or returns) we realign to the next period boundary and
        // resume firing. The timer must keep running and recover on its own
        // whenever time sync comes back, however long it takes.
        PtpTimePoint next_wake{};
        bool aligned = false;

        while (is_running_atomic()) {
            if (!aligned) {
                auto const mapping = bridge_.get_mapping();
                if (!mapping.healthy || !bridge_.is_running()) {
                    // No sync: report and retry without blocking forever. A
                    // bridge that has stopped keeps its last mapping, so it
                    // is asked as well: aligning to it would only fail at the
                    // next sleep, and this path would spin.
                    callback_(failure(PtpError::bridge_not_healthy));
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                // Sync (re)acquired: align to the next period boundary. now_ns()
                // reads the authoritative RAW clock and converts via the bridge's
                // PHC<->RAW mapping.
                next_wake = next_boundary(realtime::GptpClock0::from_ns(bridge_.now_ns()));
                aligned = true;
            }

            int64_t headroom_ns = NO_HEADROOM;
            auto wake_result = bridge_.sleep_until_ptp(next_wake + compensation_ns_, running_flag_ptr(), &headroom_ns);

            if (!wake_result) {
                if (!is_running_atomic()) {
                    break;
                }
                // Lost sync mid-run: count the transition, report it, and drop
                // back to the realignment path (which keeps reporting no-sync).
                increment_recovery_count();
                callback_(failure(wake_result.error()));
                aligned = false;
                continue;
            }

            // Updates wake stats, invokes the callback, and advances the
            // schedule (catching up if we ran behind). A late wake is recorded
            // in the stats but never stops the timer.
            if (headroom_ns != NO_HEADROOM) {
                headroom_.update(headroom_ns);
            }
            (void)process_wake(*wake_result, next_wake);
        }
    }

    PtpTimeBridge& bridge_;
    int64_t period_ns_;
    int64_t compensation_ns_;
    bool enable_realtime_;
    int cpu_affinity_;
    size_t stack_prefault_bytes_{0};      ///< prefaulted before the loop; 0 = none
    bool lock_memory_{false};             ///< mlockall(MCL_CURRENT) from the thread before the loop
    [[maybe_unused]] int64_t threshold_;  ///< retained for API stability; ftrace tripwire that used it was removed

    Callback callback_;
    // Atomics are in PtpTimerBase
    std::thread timer_thread_;
    static constexpr int64_t NO_HEADROOM = INT64_MIN;  ///< the wake did not sleep
    std::array<char, 16> thread_name_{'m', 'e', 'd', 'i', 'a'};
    stats::AtomicWakeStats stats_;
    stats::AtomicTimeStats headroom_;
};

/// Factory function to create a PtpTimer with type deduction
/// @param bridge Reference to PTP time bridge
/// @param period_ns Wake period in nanoseconds
/// @param callback Function called on each wake
/// @param compensation_ns Compensation offset in nanoseconds (negative = wake earlier)
/// @param enable_realtime Enable realtime thread priority
/// @param cpu_affinity CPU to pin timer thread to (-1 to disable)
/// @param threshold Jitter threshold in nanoseconds for wake statistics
template <typename Callback>
[[nodiscard]] auto make_ptp_timer(
    PtpTimeBridge& bridge,
    int64_t period_ns,
    Callback&& callback,
    int64_t compensation_ns = 0,
    bool enable_realtime = true,
    int cpu_affinity = 3,
    int64_t threshold = 50'000) -> PtpTimer<std::decay_t<Callback>>
{
    return PtpTimer<std::decay_t<Callback>>{
        bridge, period_ns, std::forward<Callback>(callback), compensation_ns, enable_realtime, cpu_affinity, threshold};
}

}  // namespace statusbar::ptpclient
