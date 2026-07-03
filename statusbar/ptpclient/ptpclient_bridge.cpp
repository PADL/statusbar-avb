// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Implementation of PtpTimeBridgeBase and PtpTimeBridge
/// Separated from module to prevent atomic operations from being inlined across module boundaries

#include "statusbar/ptpclient/ptpclient_bridge.hpp"

#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/status/catch_or_status.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <system_error>
#include <thread>
#include <utility>

#if defined(__linux__)
#    include <sched.h>
#endif

namespace statusbar::ptpclient {

//
// PtpTimeBridgeBase implementation
//

PtpTimeBridgeBase::PtpTimeBridgeBase() noexcept
    : running_(false)
    , reset_requested_(false)
    , epoch_(0)
    , healthy_(false)
    , ever_healthy_(false)
    , rms_residual_ns_(0)
    , worst_bracket_ns_(0)
    , sample_count_(0)
{}

auto PtpTimeBridgeBase::regression_buffer_publish(double const rate, int64_t const offset_ns) noexcept -> void
{
    regression_buffer_.store(RateOffset{.rate = rate, .offset_ns = offset_ns});
}

auto PtpTimeBridgeBase::regression_buffer_consume() const noexcept -> RateOffset
{
    // Non-consuming multi-reader load: safe to call from any thread (the RT timer,
    // the ClockAdapter, or a status/telemetry caller), unlike the previous SPSC
    // triple-buffer whose consume() was single-consumer-only.
    return regression_buffer_.load();
}

auto PtpTimeBridgeBase::regression_buffer_overruns() const noexcept -> uint64_t
{
    // A seqlock has no unconsumed-overwrite concept: every reader always observes
    // the latest published value, so there are no overruns to report.
    return 0;
}

auto PtpTimeBridgeBase::mono_raw_publish(double const rate, int64_t const offset_ns) noexcept -> void
{
    mono_raw_buffer_.store(RateOffset{.rate = rate, .offset_ns = offset_ns});
}

auto PtpTimeBridgeBase::mono_raw_consume() const noexcept -> RateOffset
{
    return mono_raw_buffer_.load();
}

// Running state
auto PtpTimeBridgeBase::is_running_atomic() const noexcept -> bool
{
    return running_.load(std::memory_order_relaxed);
}

auto PtpTimeBridgeBase::exchange_running(bool value) noexcept -> bool
{
    return running_.exchange(value, std::memory_order_relaxed);
}

auto PtpTimeBridgeBase::set_running(bool value) noexcept -> void
{
    running_.store(value, std::memory_order_relaxed);
}

// Reset requested
auto PtpTimeBridgeBase::is_reset_requested_atomic() const noexcept -> bool
{
    return reset_requested_.load(std::memory_order_relaxed);
}

auto PtpTimeBridgeBase::exchange_reset_requested(bool value) noexcept -> bool
{
    return reset_requested_.exchange(value, std::memory_order_relaxed);
}

auto PtpTimeBridgeBase::set_reset_requested(bool value) noexcept -> void
{
    reset_requested_.store(value, std::memory_order_relaxed);
}

// Epoch
auto PtpTimeBridgeBase::load_epoch() const noexcept -> uint64_t
{
    return epoch_.load(std::memory_order_relaxed);
}

auto PtpTimeBridgeBase::increment_epoch() noexcept -> void
{
    epoch_.fetch_add(1, std::memory_order_relaxed);
}

// Healthy
auto PtpTimeBridgeBase::is_healthy_atomic() const noexcept -> bool
{
    return healthy_.load(std::memory_order_relaxed);
}

auto PtpTimeBridgeBase::set_healthy(bool value) noexcept -> void
{
    healthy_.store(value, std::memory_order_relaxed);
}

// Ever healthy
auto PtpTimeBridgeBase::was_ever_healthy() const noexcept -> bool
{
    return ever_healthy_.load(std::memory_order_relaxed);
}

auto PtpTimeBridgeBase::set_ever_healthy(bool value) noexcept -> void
{
    ever_healthy_.store(value, std::memory_order_relaxed);
}

// RMS residual
auto PtpTimeBridgeBase::load_rms_residual_ns() const noexcept -> int64_t
{
    return rms_residual_ns_.load(std::memory_order_relaxed);
}

auto PtpTimeBridgeBase::store_rms_residual_ns(int64_t value) noexcept -> void
{
    rms_residual_ns_.store(value, std::memory_order_relaxed);
}

// Worst bracket
auto PtpTimeBridgeBase::load_worst_bracket_ns() const noexcept -> int64_t
{
    return worst_bracket_ns_.load(std::memory_order_relaxed);
}

auto PtpTimeBridgeBase::store_worst_bracket_ns(int64_t value) noexcept -> void
{
    worst_bracket_ns_.store(value, std::memory_order_relaxed);
}

// Sample count
auto PtpTimeBridgeBase::load_sample_count() const noexcept -> int
{
    return sample_count_.load(std::memory_order_relaxed);
}

auto PtpTimeBridgeBase::store_sample_count(int value) noexcept -> void
{
    sample_count_.store(value, std::memory_order_relaxed);
}

//
// SamplingGuard implementation
//

SamplingGuard::~SamplingGuard() noexcept
{
    stop();
}

void SamplingGuard::stop() noexcept
{
    if (bridge_ != nullptr) {
        bridge_->stop_sampling();
        bridge_ = nullptr;
    }
}

//
// PtpTimeBridge implementation
//

auto PtpTimeBridge::start_sampling(PtpClientBase& client, BridgeSamplingParams const& params) -> StatusValue<SamplingGuard>
{
    if (!client.is_open()) {
        return failure(PtpError::device_not_open);
    }

    // Create reader that captures client reference
    auto ptp_reader = [&client]() -> StatusValue<int64_t> { return client.get_time_ns(); };

    return start_sampling_impl(std::move(ptp_reader), params);
}

auto PtpTimeBridge::start_sampling(PtpTimeReader ptp_reader, BridgeSamplingParams const& params) -> StatusValue<SamplingGuard>
{
    return start_sampling_impl(std::move(ptp_reader), params);
}

void PtpTimeBridge::stop_sampling() noexcept
{
    bool const was_running = exchange_running(false);
    if (was_running && sampler_thread_.joinable()) {
        sampler_thread_.join();
    }
}

auto PtpTimeBridge::now_ns() const noexcept -> int64_t
{
    // Authoritative timebase is CLOCK_MONOTONIC_RAW (immune to phc2sys slewing);
    // the primary regression maps PHC = rate * RAW + offset.
    int64_t const raw_ns = read_raw_ns();
    auto const ro = regression_buffer_consume();
    auto const slope = FixedPointSlope::from_double(ro.rate);
    return monotonic_to_ptp_precise(raw_ns, slope, ro.offset_ns);
}

auto PtpTimeBridge::to_monotonic_ns(int64_t ptp_ns) const noexcept -> int64_t
{
    auto const ro = regression_buffer_consume();
    // Use precise conversion with __int128 arithmetic
    auto const slope = FixedPointSlope::from_double(ro.rate);
    return ptp_to_monotonic_precise(ptp_ns, slope, ro.offset_ns);
}

auto PtpTimeBridge::from_monotonic_ns(int64_t mono_ns) const noexcept -> int64_t
{
    auto const ro = regression_buffer_consume();
    // Use precise conversion with __int128 arithmetic
    auto const slope = FixedPointSlope::from_double(ro.rate);
    return monotonic_to_ptp_precise(mono_ns, slope, ro.offset_ns);
}

auto PtpTimeBridge::prepare_sleep(int64_t ptp_deadline_ns) noexcept -> StatusValue<SleepPlan>
{
    if (!is_running_atomic()) {
        return failure(PtpError::bridge_not_running);
    }

    auto const before = get_mapping();
    if (!before.healthy) {
        return failure(PtpError::bridge_not_healthy);
    }

    // 1. PTP deadline -> RAW deadline via the primary (PHC <-> RAW) mapping.
    //    ptp = rate * raw + offset  =>  raw = (ptp - offset) / rate. The
    //    *_precise/ptp_to_monotonic_precise helpers compute exactly this inverse;
    //    here the "monotonic" value they return is the RAW timestamp.
    auto const phc_slope = FixedPointSlope::from_double(before.rate);
    int64_t const phc_offset_ns = static_cast<int64_t>(before.offset_ns);
    int64_t const raw_deadline = ptp_to_monotonic_precise(ptp_deadline_ns, phc_slope, phc_offset_ns);

    // 2. RAW deadline -> coarse CLOCK_MONOTONIC deadline via the secondary
    //    (MONOTONIC = mono_rate * RAW + mono_offset) mapping, for clock_nanosleep.
    auto const mr = mono_raw_consume();
    auto const mono_slope = FixedPointSlope::from_double(mr.rate);
    int64_t const mono_deadline = monotonic_to_ptp_precise(raw_deadline, mono_slope, mr.offset_ns);

    return success(SleepPlan{
        .raw_deadline_ns = raw_deadline, .mono_deadline_ns = mono_deadline, .mono_per_raw_slope = mr.rate, .before = before});
}

auto PtpTimeBridge::finalize_wake(int64_t wake_raw, TimeMapping const& before) noexcept -> StatusValue<int64_t>
{
    auto const after = get_mapping();
    if (after.epoch != before.epoch) {
        return failure(PtpError::bridge_epoch_changed);
    }
    if (!after.healthy) {
        return failure(PtpError::bridge_not_healthy);
    }

    // RAW wake time -> PTP via the primary (PHC <-> RAW) mapping.
    auto const slope = FixedPointSlope::from_double(after.rate);
    int64_t const offset_ns = static_cast<int64_t>(after.offset_ns);
    return success(monotonic_to_ptp_precise(wake_raw, slope, offset_ns));
}

void PtpTimeBridge::reset_epoch() noexcept
{
    increment_epoch();
    set_healthy(false);
    set_ever_healthy(false);
    set_reset_requested(true);
}

auto PtpTimeBridge::read_monotonic_ns() noexcept -> int64_t
{
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return timespec_to_ns(ts.tv_sec, ts.tv_nsec);
}

auto PtpTimeBridge::read_raw_ns() noexcept -> int64_t
{
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return timespec_to_ns(ts.tv_sec, ts.tv_nsec);
}

auto PtpTimeBridge::start_sampling_impl(PtpTimeReader ptp_reader, BridgeSamplingParams const& params) -> StatusValue<SamplingGuard>
{
    if (exchange_running(true)) {
        // Already running
        return failure(PtpError::bridge_not_running);
    }

    params_ = params;
    ptp_reader_ = std::move(ptp_reader);
    set_reset_requested(false);

    sampler_thread_ = std::thread([this]() -> void { sampler_loop(); });

    return success(SamplingGuard{this});
}

void PtpTimeBridge::sampler_loop()
{
    run_guarded("PTP bridge sampler thread", [this]() { sampler_loop_impl(); });
}

void PtpTimeBridge::sampler_loop_impl()
{
    // The sampler brackets each /dev/ptp0 read between two CLOCK_MONOTONIC reads;
    // a preemption inside that bracket inflates it past max_bracket_ns (sample
    // rejected) or the residual past the degrade/step thresholds. The sampler
    // runs SCHED_OTHER, unpinned: the GM sync interval is 125ms, so sampling
    // does not need a fast, jitter-free cadence, and an outlier bracket is just
    // rejected. (RT pinning was tried and backed out — it was unnecessary once
    // the media timer no longer hard-exits on a transient unhealthy bridge.)
    // Two lockstep windows: primary PHC<->RAW (authoritative) and secondary
    // MONOTONIC<->RAW (coarse early-wake scheduling).
    TimingSampleWindow phc_window(static_cast<size_t>(params_.window_size));
    TimingSampleWindow mono_window(static_cast<size_t>(params_.window_size));

    auto const period = std::chrono::nanoseconds(1'000'000'000LL / std::max(1, params_.sample_hz));
    auto next = std::chrono::steady_clock::now();

    int64_t worst_bracket = 0;

    while (is_running_atomic()) {
        next += period;
        collect_sample(phc_window, mono_window, worst_bracket);
        std::this_thread::sleep_until(next);
    }
}

auto PtpTimeBridge::get_mapping() const noexcept -> TimeMapping
{
    TimeMapping m;
    auto const ro = regression_buffer_consume();
    m.rate = ro.rate;
    m.offset_ns = ro.offset_ns;
    m.epoch = load_epoch();
    m.healthy = is_healthy_atomic();
    m.rms_residual_ns = load_rms_residual_ns();
    m.worst_bracket_ns = load_worst_bracket_ns();
    m.sample_count = load_sample_count();
    return m;
}

auto PtpTimeBridge::telemetry() const noexcept -> BridgeTelemetry
{
    // IMPORTANT: do NOT call get_mapping() here. get_mapping() consumes the
    // regression_buffer_ triple buffer, which is strictly SPSC — its only
    // legitimate consumer is the RT timer thread. telemetry() is called from a
    // different thread (e.g. the tool's main loop, once per second); a second
    // consumer races the timer's consume side and can hand the timer a torn
    // (rate, offset) pair, producing a far-future sleep deadline that wedges the
    // media timer (healthy bridge, zero wakes). telemetry() needs none of the
    // regression buffer's contents — read the standalone atomics directly.
    auto const b = bracket_stats_.snapshot();
    BridgeTelemetry t;
    t.healthy = is_healthy_atomic();
    t.epoch = load_epoch();
    t.sample_count = load_sample_count();
    t.rms_residual_ns = load_rms_residual_ns();
    t.worst_bracket_ns = load_worst_bracket_ns();
    t.bracket_min_ns = b.has_samples() ? b.min_ns : 0;
    t.bracket_max_ns = b.has_samples() ? b.max_ns : 0;
    t.bracket_mean_ns = static_cast<int64_t>(b.average_ns());
    t.bracket_count = static_cast<uint64_t>(b.count);
    t.reject_count = reject_count_.load(std::memory_order_relaxed);
    t.step_count = step_count_.load(std::memory_order_relaxed);
    t.last_step_residual_ns = last_step_residual_ns_.load(std::memory_order_relaxed);
    t.regression_overruns = regression_buffer_overruns();
    return t;
}

auto PtpTimeBridge::convert_ptp_to_monotonic(int64_t ptp_ns) const noexcept -> TimeConvertResult
{
    TimeConvertResult result;
    result.epoch = load_epoch();
    result.healthy = is_healthy_atomic();

    auto const ro = regression_buffer_consume();
    auto const slope = FixedPointSlope::from_double(ro.rate);
    result.time_ns = ptp_to_monotonic_precise(ptp_ns, slope, ro.offset_ns);

    return result;
}

auto PtpTimeBridge::convert_monotonic_to_ptp(int64_t monotonic_ns) const noexcept -> TimeConvertResult
{
    TimeConvertResult result;
    result.epoch = load_epoch();
    result.healthy = is_healthy_atomic();

    auto const ro = regression_buffer_consume();
    auto const slope = FixedPointSlope::from_double(ro.rate);
    result.time_ns = monotonic_to_ptp_precise(monotonic_ns, slope, ro.offset_ns);

    return result;
}

auto PtpTimeBridge::sleep_until_ptp(int64_t ptp_deadline_ns) noexcept -> StatusValue<int64_t>
{
    auto prep = prepare_sleep(ptp_deadline_ns);
    if (!prep) {
        return forward_failure(prep);
    }
    auto const plan = *prep;
    int64_t const raw_deadline = plan.raw_deadline_ns;

    // clock_nanosleep can only sleep on CLOCK_MONOTONIC, whose rate is slewed by
    // phc2sys/NTP. So we wake EARLY on CLOCK_MONOTONIC, then spin on the
    // slew-immune CLOCK_MONOTONIC_RAW to the exact deadline. The early-wake guard
    // must cover (a) OS wakeup latency [base] and (b) the MONOTONIC-vs-RAW
    // divergence accrued over the sleep [slew term] so we never oversleep past
    // the RAW deadline. When phc2sys is calm the slew term is ~0 (minimal spin);
    // when it is railed at ±10% the term is ~10% of the sleep (a small spin).
    int64_t const raw_now = read_raw_ns();
    int64_t const sleep_dur = raw_deadline - raw_now;
    int64_t const base_guard = std::max<int64_t>(params_.wake_refine_spin_ns, 0);
    int64_t const slew_guard =
        sleep_dur > 0 ? static_cast<int64_t>(std::fabs(plan.mono_per_raw_slope - 1.0) * static_cast<double>(sleep_dur)) : 0;
    int64_t const mono_wake = plan.mono_deadline_ns - (base_guard + slew_guard);

#if defined(__linux__)
    timespec now_ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &now_ts);
    int64_t const mono_now = timespec_to_ns(now_ts.tv_sec, now_ts.tv_nsec);
    if (mono_wake > mono_now) {
        int64_t sec = 0, nsec = 0;
        ns_to_timespec(mono_wake, sec, nsec);
        timespec ts{};
        ts.tv_sec = static_cast<time_t>(sec);
        ts.tv_nsec = static_cast<long>(nsec);
        int const rc = ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        if (rc == EINTR) {
            // Signal received -- return error so caller can check shutdown flag
            return failure(std::error_code(EINTR, std::system_category()));
        }
        if (rc != 0) {
            return failure(std::error_code(rc, std::system_category()));
        }
    }
#else
    int64_t const mono_now =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (mono_wake > mono_now) {
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point{std::chrono::nanoseconds{mono_wake}});
    }
#endif

    // Precise finish: spin on CLOCK_MONOTONIC_RAW to the exact RAW deadline.
    // sched_yield while still far out to avoid a hard busy-loop; bail on shutdown.
    while (true) {
        int64_t const now_raw = read_raw_ns();
        if (now_raw >= raw_deadline) {
            break;
        }
        if (!is_running_atomic()) {
            return failure(PtpError::bridge_not_running);
        }
        if (raw_deadline - now_raw > 10'000) {
            ::sched_yield();
        }
    }

    int64_t const wake_raw = read_raw_ns();
    return finalize_wake(wake_raw, plan.before);
}

auto PtpTimeBridge::collect_sample(TimingSampleWindow& phc_window, TimingSampleWindow& mono_window, int64_t& worst_bracket) -> bool
{
    if (exchange_reset_requested(false)) {
        phc_window.clear();
        mono_window.clear();
        worst_bracket = 0;
    }

    // Bracket the (expensive) PHC read with CLOCK_MONOTONIC_RAW — the slew-immune
    // authoritative timebase. Immediately after, take a RAW-bracketed
    // CLOCK_MONOTONIC read so we can also track the (slewed) MONOTONIC<->RAW line
    // used for coarse early-wake scheduling.
    int64_t const raw1 = read_raw_ns();
    auto ptp_result = ptp_reader_();
    int64_t const raw2 = read_raw_ns();
    int64_t const mono = read_monotonic_ns();
    int64_t const raw3 = read_raw_ns();

    if (!ptp_result) {
        return false;
    }

    int64_t const ptp = *ptp_result;
    int64_t bracket = raw2 - raw1;
    if (bracket < 0) {
        bracket = -bracket;
    }
    if (bracket > worst_bracket) {
        worst_bracket = bracket;
    }

    // Telemetry: record the full PHC-read bracket distribution (contention signal).
    bracket_stats_.update(bracket);

    if (bracket > params_.max_bracket_ns) {
        reject_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Primary PHC <-> RAW sample (x = RAW midpoint of the PHC bracket, y = PHC).
    int64_t const raw_mid_phc = (raw1 + raw2) / 2;
    phc_window.push({.monotonic_ns = raw_mid_phc, .ptp_ns = ptp, .bracket_ns = bracket});

    // Secondary MONOTONIC <-> RAW sample (x = RAW midpoint of the MONOTONIC read,
    // y = MONOTONIC). Kept in lockstep with the primary window (same size).
    int64_t mono_bracket = raw3 - raw2;
    if (mono_bracket < 0) {
        mono_bracket = -mono_bracket;
    }
    int64_t const raw_mid_mono = (raw2 + raw3) / 2;
    mono_window.push({.monotonic_ns = raw_mid_mono, .ptp_ns = mono, .bracket_ns = mono_bracket});

    size_t const min_samples = static_cast<size_t>(std::max(2, std::min(params_.min_samples_for_healthy, params_.window_size / 4)));

    if (phc_window.size() >= min_samples) {
        update_mapping(phc_window, mono_window, worst_bracket);
    }

    return true;
}

void PtpTimeBridge::update_mapping(
    TimingSampleWindow const& phc_window, TimingSampleWindow const& mono_window, int64_t worst_bracket)
{
    auto fit = fit_time_mapping(phc_window, params_.max_rate_ppm);

    if (!fit.valid) {
        return;
    }

    // Detect step/discontinuity - but only after we've established a healthy mapping
    // During initial convergence, large residuals are expected as the fit stabilizes.
    // Step detection is meant to catch sudden clock jumps after we have a good mapping.
    if (was_ever_healthy()) {
        if (fit.max_residual > static_cast<double>(params_.step_threshold_ns)) {
            // Step detected - reset epoch and clear samples
            increment_epoch();
            set_healthy(false);
            set_ever_healthy(false);  // Must re-establish health
            set_reset_requested(true);
            store_rms_residual_ns(static_cast<int64_t>(fit.rms_residual));
            store_worst_bracket_ns(worst_bracket);
            // Telemetry: this is the failure mode that hard-stalls the media timer.
            step_count_.fetch_add(1, std::memory_order_relaxed);
            last_step_residual_ns_.store(static_cast<int64_t>(fit.max_residual), std::memory_order_relaxed);
            return;
        }
    }

    // Publish the primary (rate, offset_ns) pair atomically via the triple
    // buffer. The regression line `gptp = rate * raw + offset_ns` must be
    // observed as a consistent pair: a torn read (new slope with old intercept,
    // or vice versa) produces an error of Δslope * raw_now, which on a system
    // that's been up tens of hours (raw_now ≈ 2*10^14 ns) means a 1-ppm slope
    // wobble would translate to a ~200 ms gptp glitch.
    regression_buffer_publish(fit.slope, fit.intercept_ns);
    store_rms_residual_ns(static_cast<int64_t>(fit.rms_residual));
    store_worst_bracket_ns(worst_bracket);
    store_sample_count(static_cast<int>(phc_window.size()));

    // Health is driven by the PRIMARY (PHC <-> RAW) fit residual. RAW is immune
    // to phc2sys slewing, so this fit's slope is always ~the crystal offset and
    // the residual reflects real PHC-read jitter — not a phc2sys frequency rail.
    bool const healthy = fit.rms_residual <= static_cast<double>(params_.degrade_threshold_ns);
    set_healthy(healthy);
    if (healthy) {
        set_ever_healthy(true);
    }

    // Secondary MONOTONIC <-> RAW fit, used only to convert a RAW wake deadline
    // into a coarse CLOCK_MONOTONIC deadline for clock_nanosleep. A WIDE rate
    // clamp (±20%) because phc2sys legitimately slews CLOCK_MONOTONIC by up to
    // ~±10%; pinning it to the primary's narrow window would corrupt the coarse
    // deadline. Does not gate health.
    constexpr double MONO_RAW_MAX_RATE_PPM = 200'000.0;  // ±20%
    auto const mono_fit = fit_time_mapping(mono_window, MONO_RAW_MAX_RATE_PPM);
    if (mono_fit.valid) {
        mono_raw_publish(mono_fit.slope, mono_fit.intercept_ns);
    }
}

}  // namespace statusbar::ptpclient
