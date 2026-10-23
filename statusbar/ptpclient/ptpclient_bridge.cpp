// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Implementation of PtpTimeBridgeBase and PtpTimeBridge
/// Separated from module to prevent atomic operations from being inlined across module boundaries

#include "statusbar/ptpclient/ptpclient_bridge.hpp"

#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/realtime/realtime_base.hpp"
#include "statusbar/status/catch_or_status.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
    // A seqlock load: nothing is consumed, and any thread may read (the RT
    // timer, the ClockAdapter, a status or telemetry caller).
    return regression_buffer_.load();
}

auto PtpTimeBridgeBase::regression_buffer_overruns() const noexcept -> uint64_t
{
    // A seqlock has no unconsumed-overwrite concept: every reader always observes
    // the latest published value, so there are no overruns to report.
    return 0;
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
    return from_raw_ns(realtime::read_monotonic_raw_ns());
}

auto PtpTimeBridge::to_raw_ns(int64_t ptp_ns) const noexcept -> int64_t
{
    auto const ro = regression_buffer_consume();
    // Use precise conversion with __int128 arithmetic
    auto const slope = FixedPointSlope::from_double(ro.rate);
    return ptp_to_monotonic_precise(ptp_ns, slope, ro.offset_ns);
}

auto PtpTimeBridge::from_raw_ns(int64_t raw_ns) const noexcept -> int64_t
{
    auto const ro = regression_buffer_consume();
    // Use precise conversion with __int128 arithmetic
    auto const slope = FixedPointSlope::from_double(ro.rate);
    return monotonic_to_ptp_precise(raw_ns, slope, ro.offset_ns);
}

auto PtpTimeBridge::to_monotonic(realtime::GptpClock0::time_point const ptp) const noexcept -> realtime::MonotonicClock::time_point
{
    return raw_line_.to_monotonic(realtime::MonotonicRawClock::from_ns(to_raw_ns(realtime::GptpClock0::to_ns(ptp))));
}

auto PtpTimeBridge::from_monotonic(realtime::MonotonicClock::time_point const mono) const noexcept
    -> realtime::GptpClock0::time_point
{
    return realtime::GptpClock0::from_ns(from_raw_ns(realtime::MonotonicRawClock::to_ns(raw_line_.from_monotonic(mono))));
}

auto PtpTimeBridge::prepare_sleep(realtime::GptpClock0::time_point const ptp_deadline) noexcept -> StatusValue<SleepPlan>
{
    if (!is_running_atomic()) {
        return failure(PtpError::bridge_not_running);
    }

    auto const before = get_mapping();
    if (!before.healthy) {
        return failure(PtpError::bridge_not_healthy);
    }

    // 1. PTP deadline -> RAW deadline via the primary (PHC <-> RAW) mapping
    //    captured in `before`: ptp = rate * raw + offset => raw = (ptp - offset) / rate.
    auto const raw_deadline = realtime::MonotonicRawClock::from_ns(ptp_to_monotonic_precise(
        realtime::GptpClock0::to_ns(ptp_deadline),
        FixedPointSlope::from_double(before.rate),
        static_cast<int64_t>(before.offset_ns)));

    // 2. RAW deadline -> coarse CLOCK_MONOTONIC deadline through the
    //    RAW<->MONOTONIC line, for clock_nanosleep; its rate sizes the guard.
    return success(SleepPlan{
        .raw_deadline = raw_deadline,
        .mono_deadline = raw_line_.to_monotonic(raw_deadline),
        .mono_per_raw_slope = raw_line_.consume().rate,
        .before = before});
}

auto PtpTimeBridge::early_wake_monotonic(SleepPlan const& plan, realtime::MonotonicRawClock::time_point const raw_now)
    const noexcept -> realtime::MonotonicClock::time_point
{
    // clock_nanosleep can only sleep on CLOCK_MONOTONIC, whose rate is slewed by
    // phc2sys/NTP. So we wake EARLY on CLOCK_MONOTONIC, then spin on the
    // slew-immune CLOCK_MONOTONIC_RAW to the exact deadline. The early-wake guard
    // must cover (a) OS wakeup latency [base] and (b) the MONOTONIC-vs-RAW
    // divergence accrued over the sleep [slew term] so we never oversleep past
    // the RAW deadline. When phc2sys is calm the slew term is ~0 (minimal spin);
    // when it is railed at ±10% the term is ~10% of the sleep (a small spin).
    int64_t const sleep_dur = (plan.raw_deadline - raw_now).count();
    int64_t const base_guard = std::max<int64_t>(params_.wake_refine_spin_ns, 0);
    int64_t const slew_guard =
        sleep_dur > 0 ? static_cast<int64_t>(std::fabs(plan.mono_per_raw_slope - 1.0) * static_cast<double>(sleep_dur)) : 0;
    return plan.mono_deadline - realtime::Nanoseconds{base_guard + slew_guard};
}

auto PtpTimeBridge::finalize_wake(realtime::MonotonicRawClock::time_point const wake_raw, TimeMapping const& before) noexcept
    -> StatusValue<realtime::GptpClock0::time_point>
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
    return success(
        realtime::GptpClock0::from_ns(monotonic_to_ptp_precise(realtime::MonotonicRawClock::to_ns(wake_raw), slope, offset_ns)));
}

void PtpTimeBridge::reset_epoch() noexcept
{
    // Health first, so the bound of the old timeline is withdrawn before
    // the new epoch can be seen. quality() also checks the epoch each fit
    // was published with, which holds whatever the order.
    set_healthy(false);
    set_ever_healthy(false);
    increment_epoch();
    set_reset_requested(true);
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
    realtime::set_current_thread_name("ptp-sampler");
    // The sampler brackets each /dev/ptp0 read between two CLOCK_MONOTONIC reads;
    // a preemption inside that bracket inflates it past max_bracket_ns (sample
    // rejected) or the residual past the degrade/step thresholds. The sampler
    // runs SCHED_OTHER, unpinned: the GM sync interval is 125ms, so sampling
    // does not need a fast, jitter-free cadence, and an outlier bracket is just
    // rejected. (RT pinning was tried and backed out — it was unnecessary once
    // the media timer no longer hard-exits on a transient unhealthy bridge.)
    // The primary PHC<->RAW window (authoritative) and, over the same span,
    // the tracker that keeps raw_line_ (RAW<->MONOTONIC, for the sleeps).
    SamplerState state = new_sampler_state();
    auto const period = std::chrono::nanoseconds(1'000'000'000LL / std::max(1, params_.sample_hz));
    auto next = std::chrono::steady_clock::now();

    while (is_running_atomic()) {
        next += period;
        (void)observe(state, take_reading());
        std::this_thread::sleep_until(next);
    }
}

auto PtpTimeBridge::begin_told_sampling(BridgeSamplingParams const& params) -> SamplerState
{
    params_ = params;
    set_reset_requested(false);
    set_running(true);
    return new_sampler_state();
}

auto PtpTimeBridge::new_sampler_state() -> SamplerState
{
    SamplerState state{raw_line_, static_cast<size_t>(params_.window_size)};
    state.epoch = load_epoch();
    return state;
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
    // telemetry() needs none of the regression line: it reads the standalone
    // atomics. (It once had to keep off get_mapping(), when the line went
    // through a single-consumer buffer that only the timer thread could
    // read. The line is a seqlocked value now and any thread may read it.)
    auto const b = bracket_stats_.snapshot();
    return {
        .healthy = is_healthy_atomic(),
        .epoch = load_epoch(),
        .sample_count = load_sample_count(),
        .rms_residual_ns = load_rms_residual_ns(),
        .worst_bracket_ns = load_worst_bracket_ns(),
        .bracket_min_ns = b.has_samples() ? b.min_ns : 0,
        .bracket_max_ns = b.has_samples() ? b.max_ns : 0,
        .bracket_mean_ns = static_cast<int64_t>(b.average_ns()),
        .bracket_count = static_cast<uint64_t>(b.count),
        .reject_count = reject_count_.load(std::memory_order_relaxed),
        .step_count = step_count_.load(std::memory_order_relaxed),
        .last_step_residual_ns = last_step_residual_ns_.load(std::memory_order_relaxed),
        .regression_overruns = regression_buffer_overruns()};
}

auto PtpTimeBridge::sleep_until_ptp(
    realtime::GptpClock0::time_point const ptp_deadline,
    std::atomic<bool> const* keep_going,
    realtime::Nanoseconds* const spin_headroom) noexcept -> StatusValue<WakeTimes>
{
    auto const plan = prepare_sleep(ptp_deadline);
    if (!plan) {
        return forward_failure(plan);
    }
    // A caller already told to stop does not sleep at all; the spin below
    // reads the flag too, for a stop that arrives during the sleep. Here
    // first because a host whose coarse sleep overshoots the deadline would
    // otherwise hand a stopped caller a wake time it no longer wants.
    if (told_to_stop(keep_going)) {
        return failure(PtpError::bridge_not_running);
    }
    auto const headroom = sleep_early(*plan);
    if (!headroom) {
        return forward_failure(headroom);
    }
    return finish_sleep(*plan, keep_going, *headroom, spin_headroom);
}

auto PtpTimeBridge::told_to_stop(std::atomic<bool> const* keep_going) noexcept -> bool
{
    return keep_going != nullptr && !keep_going->load(std::memory_order_relaxed);
}

auto PtpTimeBridge::spin_headroom(
    SleepPlan const& plan,
    realtime::MonotonicClock::time_point const mono_wake,
    realtime::MonotonicRawClock::time_point const raw_after_sleep) const noexcept -> realtime::Nanoseconds
{
    // The early wake is the window plus the slew term ahead of the deadline.
    // What is left to spin, less the slew term, is what is left of the window.
    realtime::Nanoseconds const window{std::max<int64_t>(params_.wake_refine_spin_ns, 0)};
    realtime::Nanoseconds const slew_term = (plan.mono_deadline - mono_wake) - window;
    return (plan.raw_deadline - raw_after_sleep) - slew_term;
}

auto PtpTimeBridge::sleep_early(SleepPlan const& plan) const noexcept -> StatusValue<std::optional<realtime::Nanoseconds>>
{
    // One kernel sleep primitive on every platform, on the clock the line
    // maps to. (The macOS branch used to sleep on std::chrono::steady_clock,
    // which is CLOCK_MONOTONIC_RAW there, against a deadline on a different
    // counter: harmless on a desktop, a full-period spin on a laptop that had
    // ever slept.)
    auto const mono_wake = early_wake_monotonic(plan, realtime::read_monotonic_raw());
    if (mono_wake <= realtime::read_monotonic()) {
        return success(std::optional<realtime::Nanoseconds>{});
    }
    // EINTR is a signal received: an error, so the caller can check its
    // shutdown flag.
    int const rc = realtime::sleep_until_monotonic(mono_wake);
    if (rc != 0) {
        return failure(std::error_code(rc, std::system_category()));
    }
    return success(std::optional{spin_headroom(plan, mono_wake, realtime::read_monotonic_raw())});
}

auto PtpTimeBridge::spin_to(
    realtime::MonotonicRawClock::time_point const raw_deadline, std::atomic<bool> const* keep_going) const noexcept -> bool
{
    // Precise finish: spin on CLOCK_MONOTONIC_RAW (a vDSO read) to the exact
    // RAW deadline, bailing on shutdown. A yield is a system call and may
    // not come back for a while, so it is rationed and kept away from the
    // deadline; every other turn is only the CPU's pause hint.
    auto last_yield = realtime::read_monotonic_raw();
    for (auto now = last_yield; now < raw_deadline; now = realtime::read_monotonic_raw()) {
        if (!is_running_atomic() || told_to_stop(keep_going)) {
            return false;
        }
        if (now - last_yield > SPIN_YIELD_INTERVAL && raw_deadline - now > SPIN_NO_YIELD) {
            ::sched_yield();
            last_yield = now;
        } else {
            realtime::cpu_relax();
        }
    }
    return true;
}

auto PtpTimeBridge::finish_sleep(
    SleepPlan const& plan,
    std::atomic<bool> const* keep_going,
    std::optional<realtime::Nanoseconds> const headroom,
    realtime::Nanoseconds* const spin_headroom) noexcept -> StatusValue<WakeTimes>
{
    if (!spin_to(plan.raw_deadline, keep_going)) {
        return failure(PtpError::bridge_not_running);
    }
    auto wake = wake_times(plan.before);
    if (wake && headroom && spin_headroom != nullptr) {
        *spin_headroom = *headroom;
    }
    return wake;
}

auto PtpTimeBridge::wake_times(TimeMapping const& before) noexcept -> StatusValue<WakeTimes>
{
    auto const wake_raw = realtime::read_monotonic_raw();
    auto const wake_ptp = finalize_wake(wake_raw, before);
    if (!wake_ptp) {
        return forward_failure(wake_ptp);
    }
    return success(WakeTimes{.ptp = *wake_ptp, .raw = wake_raw});
}

auto PtpTimeBridge::take_reading() -> Reading
{
    // Bracket the (expensive) PHC read with CLOCK_MONOTONIC_RAW, the slew-immune
    // timing reference. Immediately after, take a RAW-bracketed CLOCK_MONOTONIC
    // read for the (slewed) RAW<->MONOTONIC line the sleeps go through.
    int64_t const raw_before_phc = realtime::read_monotonic_raw_ns();
    auto phc = ptp_reader_();
    int64_t const raw_after_phc = realtime::read_monotonic_raw_ns();
    int64_t const mono = realtime::read_monotonic_ns();
    return Reading{
        .raw_before_phc = raw_before_phc,
        .phc = std::move(phc),
        .raw_after_phc = raw_after_phc,
        .mono = mono,
        .raw_after_mono = realtime::read_monotonic_raw_ns()};
}

auto PtpTimeBridge::min_samples_for_fit() const noexcept -> size_t
{
    return static_cast<size_t>(std::max(2, std::min(params_.min_samples_for_healthy, params_.window_size / 4)));
}

auto PtpTimeBridge::accept_phc_sample(TimingSampleWindow& phc_window, Reading const& reading, int64_t& worst_bracket) -> bool
{
    int64_t const bracket = std::abs(reading.raw_after_phc - reading.raw_before_phc);
    worst_bracket = std::max(worst_bracket, bracket);
    // Telemetry: the full PHC-read bracket distribution (contention signal).
    bracket_stats_.update(bracket);
    if (bracket > params_.max_bracket_ns) {
        reject_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Primary PHC <-> RAW sample (x = RAW midpoint of the PHC bracket, y = PHC).
    phc_window.push(
        {.monotonic_ns = (reading.raw_before_phc + reading.raw_after_phc) / 2, .ptp_ns = *reading.phc, .bracket_ns = bracket});
    return true;
}

auto PtpTimeBridge::observe(SamplerState& state, Reading const& reading) -> bool
{
    if (exchange_reset_requested(false)) {
        state.phc_window.clear();
        state.raw_tracker.clear();
        state.worst_bracket = 0;
        // The request is taken before the epoch is read: a reset that
        // comes after this read asks again, and the window is cleared again.
        state.epoch = load_epoch();
        rate_checkpoint_ = {};
    }
    // The RAW<->MONOTONIC line does not depend on the PHC read: the tracker
    // is told the pair at the RAW midpoint of the MONOTONIC bracket whatever
    // the PHC did, so the sleeps have a fresh line even while unhealthy. The
    // bracket goes with it: half of it is how well the pair is known.
    (void)state.raw_tracker.push(
        realtime::MonotonicRawClock::from_ns((reading.raw_after_phc + reading.raw_after_mono) / 2),
        realtime::MonotonicClock::from_ns(reading.mono),
        realtime::Nanoseconds{reading.raw_after_mono - reading.raw_after_phc});
    if (!reading.phc) {
        return false;
    }
    if (accept_phc_sample(state.phc_window, reading, state.worst_bracket) && state.phc_window.size() >= min_samples_for_fit()) {
        update_mapping(state);
    }
    return true;
}

void PtpTimeBridge::update_mapping(SamplerState const& state)
{
    auto const fit = fit_time_mapping(state.phc_window, params_.max_rate_ppm);
    if (!fit.valid) {
        return;
    }

    // Detect step/discontinuity - but only after we've established a healthy mapping
    // During initial convergence, large residuals are expected as the fit stabilizes.
    // Step detection is meant to catch sudden clock jumps after we have a good mapping.
    if (was_ever_healthy() && fit.max_residual > static_cast<double>(params_.step_threshold_ns)) {
        // Step detected: a new epoch, no bound and no health until both are
        // re-established on a cleared window.
        reset_epoch();
        store_rms_residual_ns(static_cast<int64_t>(fit.rms_residual));
        store_worst_bracket_ns(state.worst_bracket);
        // Telemetry: this is the failure mode that hard-stalls the media timer.
        step_count_.fetch_add(1, std::memory_order_relaxed);
        last_step_residual_ns_.store(static_cast<int64_t>(fit.max_residual), std::memory_order_relaxed);
        return;
    }
    publish_fit(fit, state);
}

void PtpTimeBridge::publish_fit(LinearFitResult const& fit, SamplerState const& state)
{
    // Publish the primary (rate, offset_ns) pair atomically. The regression
    // line `gptp = rate * raw + offset_ns` must be observed as a consistent
    // pair: a torn read (new slope with old intercept, or vice versa)
    // produces an error of Δslope * raw_now, which on a system that's been
    // up tens of hours (raw_now ≈ 2*10^14 ns) means a 1-ppm slope wobble
    // would translate to a ~200 ms gptp glitch.
    regression_buffer_publish(fit.slope, fit.intercept_ns);
    // The quality goes out before the health: quality() claims a bound
    // only once the bridge has been healthy, and by then this one is in.
    publish_fit_quality(fit, state);
    store_rms_residual_ns(static_cast<int64_t>(fit.rms_residual));
    store_worst_bracket_ns(state.worst_bracket);
    store_sample_count(static_cast<int>(state.phc_window.size()));

    // Health is driven by the PRIMARY (PHC <-> RAW) fit residual. RAW is immune
    // to phc2sys slewing, so this fit's slope is always ~the crystal offset and
    // the residual reflects real PHC-read jitter — not a phc2sys frequency rail.
    if (fit.rms_residual <= static_cast<double>(params_.degrade_threshold_ns)) {
        set_healthy(true);
        set_ever_healthy(true);
    } else {
        set_healthy(false);
    }
}

void PtpTimeBridge::publish_fit_quality(LinearFitResult const& fit, SamplerState const& state)
{
    // A sample is known to half its read bracket, and the published line
    // to the furthest it lies from any sample, measured the way a reader
    // converts: together, the worst case over what the line was fitted to.
    // A line held at the edge of the rate window is not the samples' line
    // and no bound is claimed for it. On top goes the error of this
    // machine's hardware that no fit shows (params_.fixed_error_ns).
    TimingSampleWindow const& phc_window = state.phc_window;
    int64_t widest_bracket = 0;
    for (size_t i = 0; i < phc_window.size(); ++i) {
        widest_bracket = std::max(widest_bracket, phc_window[i].bracket_ns);
    }
    realtime::Nanoseconds const bound{
        fit.published_miss_ns + ((widest_bracket + 1) / 2) + std::max<int64_t>(params_.fixed_error_ns, 0)};
    fit_quality_.store(
        {.synced_at = realtime::MonotonicRawClock::from_ns(phc_window[phc_window.size() - 1].monotonic_ns),
         .bound_at_sync = fit.clamped ? realtime::UNBOUNDED_ACCURACY : bound,
         .epoch = state.epoch,
         .drift_bound_ppb = std::max(params_.holdover_drift_ppb, rate_movement_ppb(fit.slope)) + PUBLISHED_RATE_ROUNDING_PPB,
         .time_source = realtime::TimeSource::ptp});
}

auto PtpTimeBridge::rate_movement_ppb(double const slope) noexcept -> uint32_t
{
    if (!rate_checkpoint_.taken) {
        rate_checkpoint_ = {.slope = slope, .fits_since = 0, .previous_movement_ppb = 0, .taken = true};
    }
    double const moved = std::fabs(slope - rate_checkpoint_.slope) * 1e9;
    // Capped far above any real clock and far below where the sum in
    // publish_fit_quality could wrap.
    uint32_t const since_checkpoint = moved >= 1e9 ? uint32_t{1'000'000'000} : static_cast<uint32_t>(std::ceil(moved));
    uint32_t const movement = std::max(since_checkpoint, rate_checkpoint_.previous_movement_ppb);
    if (++rate_checkpoint_.fits_since >= params_.window_size) {
        rate_checkpoint_ = {.slope = slope, .fits_since = 0, .previous_movement_ppb = since_checkpoint, .taken = true};
    }
    return movement;
}

}  // namespace statusbar::ptpclient
