#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// PTP Time Bridge module
/// Provides PtpTimeBridge class for mapping between PTP and monotonic clocks
/// Uses background sampling thread with linear regression for accurate time conversion

#include "statusbar/itc/itc_seqlock_value.hpp"
#include "statusbar/ptpclient/ptpclient_base.hpp"
#include "statusbar/realtime/realtime.hpp"
#include "statusbar/stats/stats.hpp"
#include "statusbar/status/status.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <expected>
#include <functional>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>

namespace statusbar::ptpclient {

using statusbar::failure;
using statusbar::Status;
using statusbar::StatusValue;
using statusbar::success;

// Type alias for time sample window using the generic realtime ring buffer
using TimingSampleWindow = realtime::TimingSampleWindow<TimeSample>;

/// Diagnostic snapshot of the time-bridge sampler. Lets a consumer see WHY the
/// bridge is (un)healthy: PHC-read bracket distribution (contention), how many
/// samples were rejected for an over-long bracket, how many step/discontinuity
/// resets occurred (and the residual that triggered the last one), plus the
/// current fit quality. All fields are atomic loads — cheap to poll each second.
struct BridgeTelemetry
{
    bool healthy{false};
    uint64_t epoch{0};
    int sample_count{0};
    int64_t rms_residual_ns{0};
    int64_t worst_bracket_ns{0};
    // PHC-read bracket distribution over all collected samples (contention signal)
    int64_t bracket_min_ns{0};
    int64_t bracket_max_ns{0};
    int64_t bracket_mean_ns{0};
    uint64_t bracket_count{0};
    uint64_t reject_count{0};          ///< samples dropped for bracket > max_bracket_ns
    uint64_t step_count{0};            ///< step/discontinuity epoch-resets (→ unhealthy)
    int64_t last_step_residual_ns{0};  ///< max_residual that triggered the last step
    uint64_t regression_overruns{0};
};

//
// PtpTimeBridgeBase - Non-template base class for atomic operations
//

/// Base class that holds atomic members with out-of-line implementations
/// to prevent inlining issues across module boundaries (LLVM Issue #172241)
class PtpTimeBridgeBase
{
  public:
    PtpTimeBridgeBase() noexcept;

    /// Result of a seqlock-protected paired load of (rate, offset_ns).
    /// The two values were observed from the same writer publication,
    /// so the regression line `gptp = rate * mono + offset_ns` is
    /// internally consistent. A non-seqlocked load of one without the
    /// other is racy with the sampler thread's update_mapping: a
    /// 1 ppm slope wobble between samples combined with a stale
    /// intercept (or vice versa) yields a translation error of
    /// rate_delta * mono_now, which on a system that's been up for
    /// tens of hours is on the order of 100 ms per ppm.
    struct RateOffset
    {
        double rate;
        int64_t offset_ns;
    };

    /// Diagnostic: publishes that overwrote a still-unconsumed prior
    /// publish in the regression buffer. Non-zero is normal because
    /// the sampler thread publishes at ~500 Hz while the RT-timer
    /// consumer reads at ~8 kHz — the consumer just sees the latest
    /// value, and "missed" prior samples are counted here.
    [[nodiscard]] auto regression_buffer_overruns() const noexcept -> uint64_t;

  protected:
    // Running state
    [[nodiscard]] auto is_running_atomic() const noexcept -> bool;
    auto exchange_running(bool value) noexcept -> bool;
    auto set_running(bool value) noexcept -> void;

    // Reset requested
    [[nodiscard]] auto is_reset_requested_atomic() const noexcept -> bool;
    auto exchange_reset_requested(bool value) noexcept -> bool;
    auto set_reset_requested(bool value) noexcept -> void;

    /// Wait-free publish of the (rate, offset_ns) pair through the
    /// regression_buffer_ triple buffer. Single writer (the sampler
    /// thread); single consumer (the RT thread or main thread during
    /// setup/shutdown).
    auto regression_buffer_publish(double rate, int64_t offset_ns) noexcept -> void;

    /// Wait-free consume of the latest (rate, offset_ns). SPSC: only
    /// one consumer thread at a time may call this — the consumer
    /// position transfers between main (setup/shutdown) and the
    /// wan_timer / RT thread (steady-state operation).
    [[nodiscard]] auto regression_buffer_consume() const noexcept -> RateOffset;

    /// Secondary mapping CLOCK_MONOTONIC = rate * CLOCK_MONOTONIC_RAW + offset_ns.
    /// The primary `regression_buffer_` holds the authoritative PHC<->RAW line
    /// (RAW is immune to phc2sys slewing); this secondary line is used only to
    /// convert a RAW wake deadline into a coarse CLOCK_MONOTONIC deadline for
    /// clock_nanosleep (which cannot sleep on RAW). It legitimately has a large
    /// slope when phc2sys is slewing CLOCK_MONOTONIC. Same SPSC discipline.
    auto mono_raw_publish(double rate, int64_t offset_ns) noexcept -> void;
    [[nodiscard]] auto mono_raw_consume() const noexcept -> RateOffset;

    // Epoch
    [[nodiscard]] auto load_epoch() const noexcept -> uint64_t;
    auto increment_epoch() noexcept -> void;

    // Healthy
    [[nodiscard]] auto is_healthy_atomic() const noexcept -> bool;
    auto set_healthy(bool value) noexcept -> void;

    // Ever healthy
    [[nodiscard]] auto was_ever_healthy() const noexcept -> bool;
    auto set_ever_healthy(bool value) noexcept -> void;

    // RMS residual
    [[nodiscard]] auto load_rms_residual_ns() const noexcept -> int64_t;
    auto store_rms_residual_ns(int64_t value) noexcept -> void;

    // Worst bracket
    [[nodiscard]] auto load_worst_bracket_ns() const noexcept -> int64_t;
    auto store_worst_bracket_ns(int64_t value) noexcept -> void;

    // Sample count
    [[nodiscard]] auto load_sample_count() const noexcept -> int;
    auto store_sample_count(int value) noexcept -> void;

  private:
    // Thread control
    std::atomic<bool> running_;
    std::atomic<bool> reset_requested_;

    // Triple-buffered regression line. Single writer (update_mapping
    // in the sampler thread); single consumer (the RT timer thread,
    // plus the main thread during setup before the RT thread starts
    // and during shutdown after it exits). Initialized with
    // {rate=1.0, offset_ns=0} via the initial-value constructor so
    // the first load() before any regression fit returns sensible defaults.
    // Single writer (the sampler thread) via store(); many readers via load() --
    // now_ns()/get_mapping()/convert_*() are public const and may be called from
    // any thread (RT timer, ClockAdapter, status/telemetry). A seqlock (not the
    // strictly-SPSC AtomicTripleBuffer, whose consume() is single-consumer) is the
    // right primitive: every reader observes a consistent (rate, offset) snapshot.
    statusbar::itc::SeqlockValue<RateOffset> regression_buffer_{RateOffset{.rate = 1.0, .offset_ns = 0}};
    // Secondary CLOCK_MONOTONIC <- CLOCK_MONOTONIC_RAW line (see mono_raw_publish).
    statusbar::itc::SeqlockValue<RateOffset> mono_raw_buffer_{RateOffset{.rate = 1.0, .offset_ns = 0}};
    std::atomic<uint64_t> epoch_;
    std::atomic<bool> healthy_;
    std::atomic<bool> ever_healthy_;
    std::atomic<int64_t> rms_residual_ns_;
    std::atomic<int64_t> worst_bracket_ns_;
    std::atomic<int> sample_count_;
};

//
// RAII Sampling Guard
//

class PtpTimeBridge;  // Forward declaration

/// RAII guard that stops sampling on destruction
class SamplingGuard
{
  public:
    SamplingGuard() noexcept = default;
    /// @param bridge Pointer to the PtpTimeBridge to guard
    explicit SamplingGuard(PtpTimeBridge* bridge) noexcept
        : bridge_{bridge}
    {}

    ~SamplingGuard() noexcept;

    // Move only
    SamplingGuard(SamplingGuard&& other) noexcept
        : bridge_{other.bridge_}
    {
        other.bridge_ = nullptr;
    }

    auto operator=(SamplingGuard&& other) noexcept -> SamplingGuard&
    {
        if (this != &other) {
            stop();
            bridge_ = other.bridge_;
            other.bridge_ = nullptr;
        }
        return *this;
    }

    SamplingGuard(SamplingGuard const&) = delete;
    auto operator=(SamplingGuard const&) -> SamplingGuard& = delete;

    /// Check if guard is active
    [[nodiscard]] auto active() const noexcept -> bool { return bridge_ != nullptr; }

    /// Release ownership without stopping
    auto release() noexcept -> PtpTimeBridge*
    {
        auto* b = bridge_;
        bridge_ = nullptr;
        return b;
    }

    /// Stop sampling explicitly
    void stop() noexcept;

  private:
    PtpTimeBridge* bridge_{nullptr};
};

//
// PTP Time Bridge
//

/// Bridge between PTP hardware clock and local monotonic clock
/// Maintains a continuously-updated linear mapping using background sampling
///
/// Usage:
///   PtpTimeBridge bridge;
///   auto guard = bridge.start_sampling(ptp_client, params);
///   // ... use bridge for conversions ...
///   // guard destructor calls stop_sampling()
class PtpTimeBridge : public PtpTimeBridgeBase
{
  public:
    /// Function type for reading PTP time (abstracted from hardware)
    using PtpTimeReader = std::function<StatusValue<int64_t>()>;

    PtpTimeBridge() noexcept = default;
    ~PtpTimeBridge() noexcept { stop_sampling(); }

    // No copy
    PtpTimeBridge(PtpTimeBridge const&) = delete;
    auto operator=(PtpTimeBridge const&) -> PtpTimeBridge& = delete;

    // No move (has thread)
    PtpTimeBridge(PtpTimeBridge&&) = delete;
    auto operator=(PtpTimeBridge&&) -> PtpTimeBridge& = delete;

    // Sampling Control

    /// Start background sampling with a PtpClientBase
    /// @param client PTP client to read time from (must remain valid while sampling)
    /// @param params Sampling configuration
    /// @return RAII guard that stops sampling on destruction, or error
    [[nodiscard]] auto start_sampling(PtpClientBase& client, BridgeSamplingParams const& params = {}) -> StatusValue<SamplingGuard>;

    /// Start background sampling with custom PTP time reader (for testing)
    /// @param ptp_reader Function to read PTP time
    /// @param params Sampling configuration
    /// @return RAII guard that stops sampling on destruction, or error
    [[nodiscard]] auto start_sampling(PtpTimeReader ptp_reader, BridgeSamplingParams const& params = {})
        -> StatusValue<SamplingGuard>;

    /// Stop background sampling
    void stop_sampling() noexcept;

    /// Check if sampling is running
    [[nodiscard]] auto is_running() const noexcept -> bool { return is_running_atomic(); }

    // Time Mapping State (lock-free reads)

    /// Get current time mapping state
    [[nodiscard]] auto get_mapping() const noexcept -> TimeMapping;

    /// Check if mapping is healthy
    [[nodiscard]] auto is_healthy() const noexcept -> bool { return is_healthy_atomic(); }

    /// Get current epoch (incremented on discontinuities)
    [[nodiscard]] auto epoch() const noexcept -> uint64_t { return load_epoch(); }

    // ClockSource-compatible interface (for use with ClockAdapter)

    /// Get current PTP time in nanoseconds
    /// This reads monotonic time and converts to PTP domain
    /// Uses __int128 precision to avoid floating-point errors with large offsets
    [[nodiscard]] auto now_ns() const noexcept -> int64_t;

    /// Convert PTP time to monotonic time (nanoseconds)
    /// Uses __int128 precision to avoid floating-point errors with large offsets
    /// @param ptp_ns PTP time in nanoseconds to convert
    [[nodiscard]] auto to_monotonic_ns(int64_t ptp_ns) const noexcept -> int64_t;

    /// Convert monotonic time to PTP time (nanoseconds)
    /// Uses __int128 precision to avoid floating-point errors with large offsets
    /// @param mono_ns Monotonic time in nanoseconds to convert
    [[nodiscard]] auto from_monotonic_ns(int64_t mono_ns) const noexcept -> int64_t;

    // Time Conversion (returning TimeConvertResult with epoch/health info)

    /// Convert PTP time to monotonic time
    /// Uses __int128 precision to avoid floating-point errors with large offsets
    /// @param ptp_ns PTP time in nanoseconds to convert
    [[nodiscard]] auto convert_ptp_to_monotonic(int64_t ptp_ns) const noexcept -> TimeConvertResult;

    /// Convert monotonic time to PTP time
    /// Uses __int128 precision to avoid floating-point errors with large offsets
    /// @param monotonic_ns Monotonic time in nanoseconds to convert
    [[nodiscard]] auto convert_monotonic_to_ptp(int64_t monotonic_ns) const noexcept -> TimeConvertResult;

    /// Plan for one bracketed sleep: the PTP deadline expressed in both clock
    /// domains. raw_deadline (CLOCK_MONOTONIC_RAW) is authoritative — we spin to
    /// it. mono_deadline (CLOCK_MONOTONIC) is the coarse early-wake target for
    /// clock_nanosleep. mono_per_raw_slope sizes the early-wake guard.
    struct SleepPlan
    {
        int64_t raw_deadline_ns{0};
        int64_t mono_deadline_ns{0};
        double mono_per_raw_slope{1.0};
        TimeMapping before{};
    };

    /// Prepare for sleep: validate state and convert the PTP deadline into the
    /// RAW and (coarse) MONOTONIC domains.
    /// @return SleepPlan, or error if not running / unhealthy
    [[nodiscard]] auto prepare_sleep(int64_t ptp_deadline_ns) noexcept -> StatusValue<SleepPlan>;

    /// Finalize after sleep: validate epoch and convert the RAW wake time back to
    /// PTP via the primary (PHC<->RAW) mapping.
    /// @return current PTP time, or error if epoch changed or mapping unhealthy
    [[nodiscard]] auto finalize_wake(int64_t wake_raw, TimeMapping const& before) noexcept -> StatusValue<int64_t>;

    /// Sleep until a PTP deadline by converting to monotonic and sleeping
    /// @param ptp_deadline_ns Target PTP time in nanoseconds
    /// @return Current PTP time on success (allows caller to measure wake accuracy), error otherwise
    /// @note On Linux, uses clock_nanosleep for precision. On other platforms, uses std::this_thread::sleep_until.
    [[nodiscard]] auto sleep_until_ptp(int64_t ptp_deadline_ns) noexcept -> StatusValue<int64_t>;

    // Manual Epoch Control

    /// Force a new epoch (clears samples and marks unhealthy)
    void reset_epoch() noexcept;

    /// Diagnostic snapshot of sampler health (bracket distribution, rejects,
    /// step resets, fit quality). Out-of-line (reads atomics across modules).
    [[nodiscard]] auto telemetry() const noexcept -> BridgeTelemetry;

  private:
    /// Read CLOCK_MONOTONIC (slewed by phc2sys/NTP frequency adjustment). Used
    /// only as the coarse early-wake clock for clock_nanosleep.
    [[nodiscard]] static auto read_monotonic_ns() noexcept -> int64_t;

    /// Read CLOCK_MONOTONIC_RAW — the raw hardware counter, immune to phc2sys/NTP
    /// slewing. This is the bridge's authoritative local timebase: the PHC is
    /// fitted against RAW, and wake deadlines are hit by spinning on RAW.
    [[nodiscard]] static auto read_raw_ns() noexcept -> int64_t;

    [[nodiscard]] auto start_sampling_impl(PtpTimeReader ptp_reader, BridgeSamplingParams const& params)
        -> StatusValue<SamplingGuard>;

    /// Take a single sample: bracket the PHC read with CLOCK_MONOTONIC_RAW
    /// (primary PHC<->RAW window) and a RAW-bracketed CLOCK_MONOTONIC read
    /// (secondary MONOTONIC<->RAW window).
    /// @return true if a valid sample was collected, false if skipped
    auto collect_sample(TimingSampleWindow& phc_window, TimingSampleWindow& mono_window, int64_t& worst_bracket) -> bool;

    void sampler_loop();       ///< Thread entry: exception barrier around sampler_loop_impl()
    void sampler_loop_impl();  ///< Actual sampler work loop

    /// Refit and publish both mappings from the two windows (kept in lockstep).
    void update_mapping(TimingSampleWindow const& phc_window, TimingSampleWindow const& mono_window, int64_t worst_bracket);

    // Configuration
    BridgeSamplingParams params_{};
    PtpTimeReader ptp_reader_;

    // Thread (atomics are in base class)
    std::thread sampler_thread_;

    // Telemetry (written by the sampler thread, read by any consumer)
    stats::AtomicTimeStats bracket_stats_;           ///< PHC-read bracket distribution (all samples)
    std::atomic<uint64_t> reject_count_{0};          ///< samples dropped for bracket > max_bracket_ns
    std::atomic<uint64_t> step_count_{0};            ///< step/discontinuity epoch-resets
    std::atomic<int64_t> last_step_residual_ns_{0};  ///< max_residual that triggered the last step
};

}  // namespace statusbar::ptpclient
