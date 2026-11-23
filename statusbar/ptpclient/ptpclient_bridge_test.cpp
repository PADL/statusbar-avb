// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/realtime/realtime_base.hpp"
#include "statusbar/realtime/realtime_clock.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/test/test.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <system_error>

#include <sys/resource.h>

using namespace statusbar::ptpclient;

//
// How these tests tell the time
//
// The bridge is told every reading (begin_told_sampling / observe): the
// times are made up, no thread runs and nothing waits. The few tests of the
// sampler thread's own start and stop give it a reader that reads no clock,
// and assert nothing that depends on how long anything takes.
//

namespace {

using statusbar::realtime::MonotonicRawClock;
using statusbar::realtime::Nanoseconds;

/// A made-up PHC, a fixed distance ahead of RAW and running `ppm` fast of
/// it, read every 2 ms, beside a made-up CLOCK_MONOTONIC running `mono_ppm`
/// fast of RAW. Each reading takes `bracket_ns` and catches the PHC at the
/// middle of it.
struct ToldPhc
{
    static constexpr int64_t SAMPLE_INTERVAL_NS = 2'000'000;
    static constexpr int64_t PHC_AHEAD_OF_RAW_NS = 1'000'000'000'000;

    int64_t raw_ns{50'000'000'000};                        ///< RAW at the start of the next reading
    int64_t phc_ns{50'000'000'000 + PHC_AHEAD_OF_RAW_NS};  ///< the PHC at that instant
    int64_t mono_ns{57'000'000'000};                       ///< CLOCK_MONOTONIC at that instant
    int64_t ppm{0};
    int64_t mono_ppm{0};
    int64_t bracket_ns{400};
    bool fails{false};

    /// The next reading, its PHC value @p phc_error_ns off the truth.
    auto next(int64_t phc_error_ns = 0) -> PtpTimeBridge::Reading
    {
        int64_t const before = raw_ns;
        int64_t const after = before + bracket_ns;
        int64_t const phc_mid = phc_ns + (bracket_ns / 2) + phc_error_ns;
        int64_t const mono_read = mono_ns + bracket_ns + 100;
        raw_ns += SAMPLE_INTERVAL_NS;
        phc_ns += SAMPLE_INTERVAL_NS + ((SAMPLE_INTERVAL_NS * ppm) / 1'000'000);
        mono_ns += SAMPLE_INTERVAL_NS + ((SAMPLE_INTERVAL_NS * mono_ppm) / 1'000'000);
        return {
            .raw_before_phc = before,
            .phc = fails ? statusbar::StatusValue<int64_t>{statusbar::failure(make_error_code(PtpError::clock_gettime_failed))}
                         : statusbar::StatusValue<int64_t>{phc_mid},
            .raw_after_phc = after,
            .mono = mono_read,
            .raw_after_mono = after + 200};
    }

    /// The RAW midpoint of the reading taken last: where its sample sits.
    [[nodiscard]] auto last_sample_raw_ns() const -> int64_t { return raw_ns - SAMPLE_INTERVAL_NS + (bracket_ns / 2); }
};

/// A small window so a test can fill it several times over: fitted from
/// 8 samples on, 32 at most.
auto told_params() -> BridgeSamplingParams
{
    BridgeSamplingParams params;
    params.window_size = 32;
    params.min_samples_for_healthy = 8;
    return params;
}

auto feed(PtpTimeBridge& bridge, PtpTimeBridge::SamplerState& state, ToldPhc& phc, int readings) -> void
{
    for (int i = 0; i < readings; ++i) {
        (void)bridge.observe(state, phc.next());
    }
}

/// For the three tests that let real code wait on the real clocks: tell the
/// bridge a history that agrees with those clocks as they stand at this
/// call, 64 readings ending now. The mapping is then fixed, so what such a
/// test asserts is exact or one-sided, and never how long the wait took.
auto tell_history_ending_now(PtpTimeBridge& bridge, PtpTimeBridge::SamplerState& state) -> ToldPhc
{
    constexpr int READINGS = 64;
    int64_t const raw_now = statusbar::realtime::read_monotonic_raw_ns();
    int64_t const mono_now = statusbar::realtime::read_monotonic_ns();
    ToldPhc phc;
    phc.raw_ns = raw_now - (READINGS * ToldPhc::SAMPLE_INTERVAL_NS);
    phc.phc_ns = phc.raw_ns + ToldPhc::PHC_AHEAD_OF_RAW_NS;
    phc.mono_ns = phc.raw_ns + (mono_now - raw_now);
    feed(bridge, state, phc, READINGS);
    return phc;
}

/// A PHC reader for the sampler thread's start and stop tests: it counts,
/// and reads no clock.
auto counting_reader() -> PtpTimeBridge::PtpTimeReader
{
    return [count = int64_t{0}]() mutable -> statusbar::StatusValue<int64_t> { return statusbar::success(++count); };
}

}  // namespace

//
// Tests: PtpTimeBridge - Construction and initial state
//

TEST(ptp_bridge_construct, default_state_is_not_running)
{
    PtpTimeBridge bridge;

    EXPECT_FALSE(bridge.is_running());
    EXPECT_FALSE(bridge.is_healthy());
    EXPECT_EQ(bridge.epoch(), 0U);
}

TEST(ptp_bridge_construct, initial_mapping_has_default_values)
{
    PtpTimeBridge bridge;
    auto mapping = bridge.get_mapping();

    EXPECT_TRUE(std::abs(mapping.rate - 1.0) < 1e-9);
    EXPECT_TRUE(std::abs(mapping.offset_ns) < 1e-9);
    EXPECT_EQ(mapping.epoch, 0U);
    EXPECT_FALSE(mapping.healthy);
    EXPECT_EQ(mapping.sample_count, 0);
}

//
// Tests: PtpTimeBridge - start_sampling with PtpTimeReader
//

TEST(ptp_bridge_sampling, start_with_reader_succeeds)
{
    PtpTimeBridge bridge;
    auto result = bridge.start_sampling(counting_reader());

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(bridge.is_running());

    // Stop via guard destruction
    result->stop();
    EXPECT_FALSE(bridge.is_running());
}

TEST(ptp_bridge_sampling, guard_stops_on_destruction)
{
    PtpTimeBridge bridge;

    {
        auto result = bridge.start_sampling(counting_reader());
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(bridge.is_running());
    }  // Guard goes out of scope

    EXPECT_FALSE(bridge.is_running());
}

TEST(ptp_bridge_sampling, stop_sampling_is_idempotent)
{
    PtpTimeBridge bridge;
    auto result = bridge.start_sampling(counting_reader());
    EXPECT_TRUE(result.has_value());

    bridge.stop_sampling();
    EXPECT_FALSE(bridge.is_running());

    // Second stop should be safe
    bridge.stop_sampling();
    EXPECT_FALSE(bridge.is_running());
}

TEST(ptp_bridge_sampling, cannot_start_twice)
{
    PtpTimeBridge bridge;

    auto result1 = bridge.start_sampling(counting_reader());
    EXPECT_TRUE(result1.has_value());

    // Second start should fail
    auto result2 = bridge.start_sampling(counting_reader());
    EXPECT_FALSE(result2.has_value());

    bridge.stop_sampling();
}

//
// Tests: PtpTimeBridge - Convergence to healthy state
//

TEST(ptp_bridge_health, converges_to_healthy_with_good_data)
{
    PtpTimeBridge bridge;
    auto const params = told_params();
    auto state = bridge.begin_told_sampling(params);
    ToldPhc phc;
    feed(bridge, state, phc, 64);

    EXPECT_TRUE(bridge.is_healthy());
    auto mapping = bridge.get_mapping();
    EXPECT_TRUE(mapping.healthy);
    EXPECT_EQ(mapping.sample_count, params.window_size);
    // The readings lie on a line at rate one: the fit is that line.
    EXPECT_TRUE(std::abs(mapping.rate - 1.0) < 1e-9);
    EXPECT_EQ(mapping.offset_ns, ToldPhc::PHC_AHEAD_OF_RAW_NS);
    EXPECT_EQ(mapping.rms_residual_ns, int64_t{0});
    EXPECT_EQ(mapping.worst_bracket_ns, int64_t{400});
}

TEST(ptp_bridge_health, unhealthy_when_reader_fails)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc phc;
    phc.fails = true;
    feed(bridge, state, phc, 64);

    // Not healthy, since every PHC read failed.
    EXPECT_FALSE(bridge.is_healthy());
    EXPECT_EQ(bridge.get_mapping().sample_count, 0);
}

TEST(ptp_bridge_health, a_read_that_took_too_long_is_not_a_sample)
{
    PtpTimeBridge bridge;
    auto const params = told_params();
    auto state = bridge.begin_told_sampling(params);
    ToldPhc phc;
    // Every read preempted for longer than the bracket limit: none is used.
    phc.bracket_ns = params.max_bracket_ns + 2;
    feed(bridge, state, phc, 64);
    EXPECT_FALSE(bridge.is_healthy());
    EXPECT_EQ(bridge.telemetry().reject_count, uint64_t{64});

    phc.bracket_ns = 400;
    feed(bridge, state, phc, 8);
    EXPECT_TRUE(bridge.is_healthy());
    EXPECT_EQ(bridge.telemetry().reject_count, uint64_t{64});
}

//
// Tests: PtpTimeBridge - Time conversion
//

TEST(ptp_bridge_convert, ptp_to_monotonic_with_unity_mapping)
{
    PtpTimeBridge bridge;

    // Default mapping: rate=1.0, offset=0
    auto result = bridge.convert_ptp_to_monotonic(1'000'000'000LL);

    EXPECT_EQ(result.time_ns, 1'000'000'000LL);
    EXPECT_EQ(result.epoch, 0U);
    EXPECT_FALSE(result.healthy);  // Not healthy until samples are collected
}

TEST(ptp_bridge_convert, monotonic_to_ptp_with_unity_mapping)
{
    PtpTimeBridge bridge;

    auto result = bridge.convert_monotonic_to_ptp(1'000'000'000LL);

    EXPECT_EQ(result.time_ns, 1'000'000'000LL);
}

TEST(ptp_bridge_convert, conversion_after_convergence)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc phc;
    feed(bridge, state, phc, 64);
    EXPECT_TRUE(bridge.is_healthy());

    // The instant the next reading would start at, on all three clocks.
    auto const ptp_result = bridge.convert_monotonic_to_ptp(phc.mono_ns);
    EXPECT_EQ(ptp_result.time_ns, phc.phc_ns);
    EXPECT_TRUE(ptp_result.healthy);
    EXPECT_EQ(ptp_result.epoch, bridge.epoch());

    auto const mono_result = bridge.convert_ptp_to_monotonic(ptp_result.time_ns);
    EXPECT_EQ(mono_result.time_ns, phc.mono_ns);
    EXPECT_EQ(bridge.convert_raw_to_ptp(phc.raw_ns).time_ns, phc.phc_ns);
    EXPECT_EQ(bridge.convert_ptp_to_raw(phc.phc_ns).time_ns, phc.raw_ns);
}

//
// Tests: PtpTimeBridge - reset_epoch()
//

TEST(ptp_bridge_epoch, reset_epoch_increments_epoch)
{
    PtpTimeBridge bridge;

    EXPECT_EQ(bridge.epoch(), 0U);

    bridge.reset_epoch();

    EXPECT_EQ(bridge.epoch(), 1U);

    bridge.reset_epoch();

    EXPECT_EQ(bridge.epoch(), 2U);
}

TEST(ptp_bridge_epoch, reset_epoch_marks_unhealthy)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc phc;
    feed(bridge, state, phc, 64);
    EXPECT_TRUE(bridge.is_healthy());

    bridge.reset_epoch();

    EXPECT_FALSE(bridge.is_healthy());
}

//
// Tests: PtpTimeBridge - quality(): how well the PHC line is known
//

static_assert(statusbar::realtime::QualifiedClockSource<PtpTimeBridge>);

TEST(ptp_bridge_quality, no_bound_is_claimed_before_the_first_healthy_fit)
{
    PtpTimeBridge bridge;
    EXPECT_FALSE(bridge.quality().is_bounded());

    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc phc;
    // A PHC that cannot be read teaches nothing.
    phc.fails = true;
    feed(bridge, state, phc, 20);
    EXPECT_FALSE(bridge.quality().is_bounded());

    // Seven readings are one short of a fit; the eighth makes it.
    phc.fails = false;
    feed(bridge, state, phc, 7);
    EXPECT_FALSE(bridge.quality().is_bounded());
    EXPECT_FALSE(bridge.is_healthy());
    feed(bridge, state, phc, 1);
    EXPECT_TRUE(bridge.is_healthy());
    EXPECT_TRUE(bridge.quality().is_bounded());
}

TEST(ptp_bridge_quality, a_fit_is_bounded_by_its_worst_residual_and_half_its_longest_read)
{
    PtpTimeBridge bridge;
    auto const params = told_params();
    auto state = bridge.begin_told_sampling(params);
    ToldPhc phc{.ppm = 30};
    feed(bridge, state, phc, 64);

    // Readings that lie on a line, each taken in 400 ns: known to 200 ns.
    auto quality = bridge.quality();
    EXPECT_TRUE(quality.is_bounded());
    EXPECT_TRUE(quality.bound_at_sync >= Nanoseconds{200} && quality.bound_at_sync <= Nanoseconds{202});
    // As of the newest sample, on this timeline, from a PTP clock.
    EXPECT_EQ(MonotonicRawClock::to_ns(quality.synced_at), phc.last_sample_raw_ns());
    EXPECT_EQ(quality.epoch, bridge.epoch());
    EXPECT_TRUE(quality.time_source == statusbar::realtime::TimeSource::ptp);
    // The configured drift, plus the rounding of the rate the line is published with.
    EXPECT_EQ(quality.drift_bound_ppb, params.holdover_drift_ppb + 500);

    // PHC reads that scatter 3 us either side of the line widen the bound
    // by that. The pattern (+ - - +) has no slope of its own to tilt the fit.
    constexpr int64_t SCATTER_NS[4] = {3'000, -3'000, -3'000, 3'000};
    for (int i = 0; i < 64; ++i) {
        (void)bridge.observe(state, phc.next(SCATTER_NS[i % 4]));
    }
    quality = bridge.quality();
    EXPECT_TRUE(quality.bound_at_sync >= Nanoseconds{3'200} && quality.bound_at_sync <= Nanoseconds{3'210});

    // And slower reads by half their length: 2 us brackets, 800 ns more.
    phc.bracket_ns = 2'000;
    for (int i = 0; i < 64; ++i) {
        (void)bridge.observe(state, phc.next(SCATTER_NS[i % 4]));
    }
    quality = bridge.quality();
    EXPECT_TRUE(quality.bound_at_sync >= Nanoseconds{4'000} && quality.bound_at_sync <= Nanoseconds{4'010});
}

TEST(ptp_bridge_quality, when_the_phc_cannot_be_read_the_line_coasts)
{
    PtpTimeBridge bridge;
    auto const params = told_params();
    auto state = bridge.begin_told_sampling(params);
    ToldPhc phc{.ppm = 30};
    feed(bridge, state, phc, 64);
    auto const at_loss = bridge.quality();
    EXPECT_TRUE(at_loss.is_bounded());

    // Thirty seconds in which the PHC does not answer. The sampler keeps
    // trying; nothing is fitted and nothing is withdrawn.
    phc.fails = true;
    feed(bridge, state, phc, 15'000);
    EXPECT_TRUE(bridge.quality() == at_loss);
    EXPECT_TRUE(bridge.quality().is_bounded());

    // Only the age has grown, and the uncertainty with it: 30 s at 15.5 ppm.
    auto const later = at_loss.synced_at + std::chrono::seconds{30};
    EXPECT_EQ(at_loss.uncertainty(later).count(), at_loss.bound_at_sync.count() + 465'000);
    // The line still converts with the rate and offset it had: a PHC that
    // held its rate is where the line says, far inside the bound.
    EXPECT_TRUE(std::abs(bridge.from_raw_ns(phc.raw_ns) - phc.phc_ns) <= 5);
    EXPECT_TRUE(std::abs(bridge.to_raw_ns(phc.phc_ns) - phc.raw_ns) <= 5);

    // The PHC answers again: the anchor moves on to the new readings.
    phc.fails = false;
    feed(bridge, state, phc, 4);
    EXPECT_EQ(MonotonicRawClock::to_ns(bridge.quality().synced_at), phc.last_sample_raw_ns());
    EXPECT_EQ(bridge.quality().epoch, at_loss.epoch);
}

TEST(ptp_bridge_quality, a_new_epoch_claims_nothing_until_the_fit_is_healthy_again)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc phc;
    feed(bridge, state, phc, 64);
    EXPECT_TRUE(bridge.quality().is_bounded());
    uint64_t const epoch_before = bridge.epoch();

    bridge.reset_epoch();
    EXPECT_EQ(bridge.quality().epoch, epoch_before + 1);
    EXPECT_FALSE(bridge.quality().is_bounded());

    // The window starts again: seven readings are not enough, eight are.
    feed(bridge, state, phc, 7);
    EXPECT_FALSE(bridge.quality().is_bounded());
    feed(bridge, state, phc, 1);
    EXPECT_TRUE(bridge.quality().is_bounded());
    EXPECT_EQ(bridge.quality().epoch, epoch_before + 1);
}

TEST(ptp_bridge_quality, a_step_of_the_phc_is_a_new_epoch)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc phc;
    feed(bridge, state, phc, 64);
    uint64_t const epoch_before = bridge.epoch();

    // The PHC is stepped a second ahead between two readings.
    phc.phc_ns += 1'000'000'000;
    feed(bridge, state, phc, 1);
    EXPECT_EQ(bridge.quality().epoch, epoch_before + 1);
    EXPECT_FALSE(bridge.quality().is_bounded());
    EXPECT_FALSE(bridge.is_healthy());

    // Fitted afresh on the new timeline, and bounded on it.
    feed(bridge, state, phc, 8);
    EXPECT_TRUE(bridge.quality().is_bounded());
    EXPECT_EQ(bridge.quality().epoch, epoch_before + 1);
    EXPECT_TRUE(std::abs(bridge.from_raw_ns(phc.raw_ns) - phc.phc_ns) <= 5);
}

TEST(ptp_bridge_quality, a_rate_seen_moving_raises_the_drift_bound)
{
    PtpTimeBridge bridge;
    auto params = told_params();
    params.holdover_drift_ppb = 1'000;  // a good oscillator in a steady room
    auto state = bridge.begin_told_sampling(params);
    ToldPhc phc{.ppm = 10};
    feed(bridge, state, phc, 96);
    EXPECT_EQ(bridge.quality().drift_bound_ppb, uint32_t{1'000 + 500});

    // The servo pulls the PHC 50 ppm: the rate the line holds was seen to
    // move by that much within a window, so it may be that far out again.
    phc.ppm = 60;
    feed(bridge, state, phc, 48);
    EXPECT_TRUE(bridge.quality().drift_bound_ppb > uint32_t{40'000});
    EXPECT_TRUE(bridge.quality().drift_bound_ppb <= uint32_t{51'000});

    // Steady again for a few windows, the configured figure stands.
    feed(bridge, state, phc, 160);
    EXPECT_EQ(bridge.quality().drift_bound_ppb, uint32_t{1'000 + 500});
}

//
// Tests: SamplingGuard - RAII behavior
//

TEST(ptp_bridge_guard, guard_active_when_valid)
{
    PtpTimeBridge bridge;
    auto result = bridge.start_sampling(counting_reader());
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(result->active());

    result->stop();
    EXPECT_FALSE(result->active());
}

TEST(ptp_bridge_guard, guard_release_transfers_ownership)
{
    PtpTimeBridge bridge;
    auto result = bridge.start_sampling(counting_reader());
    EXPECT_TRUE(result.has_value());

    // Release ownership
    auto* released = result->release();
    EXPECT_EQ(released, &bridge);
    EXPECT_FALSE(result->active());

    // Bridge should still be running
    EXPECT_TRUE(bridge.is_running());

    // Manual cleanup
    bridge.stop_sampling();
}

TEST(ptp_bridge_guard, guard_move_transfers_ownership)
{
    PtpTimeBridge bridge;
    auto result = bridge.start_sampling(counting_reader());
    EXPECT_TRUE(result.has_value());

    SamplingGuard guard2 = std::move(*result);
    EXPECT_TRUE(guard2.active());
    EXPECT_FALSE(result->active());

    guard2.stop();
    EXPECT_FALSE(bridge.is_running());
}

//
// Tests: PtpTimeBridge - sleep_until_ptp
//

TEST(ptp_bridge_sleep, sleep_fails_when_not_running)
{
    PtpTimeBridge bridge;

    auto result = bridge.sleep_until_ptp(1'000'000'000LL);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().value(), static_cast<int>(PtpError::bridge_not_running));
}

TEST(ptp_bridge_sleep, sleep_fails_when_unhealthy)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc phc;
    phc.fails = true;  // never healthy
    feed(bridge, state, phc, 16);

    // Refused in the plan, before any clock is read or anything waits.
    auto result = bridge.sleep_until_ptp(phc.phc_ns + 1'000'000LL);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().value(), static_cast<int>(PtpError::bridge_not_healthy));
    EXPECT_EQ(bridge.prepare_sleep(phc.phc_ns + 1'000'000LL).error().value(), static_cast<int>(PtpError::bridge_not_healthy));
}

TEST(ptp_bridge_sleep, sleep_bails_when_the_caller_stops)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc phc;
    feed(bridge, state, phc, 64);
    EXPECT_TRUE(bridge.is_healthy());

    // A caller that has already been told to stop does not sleep at all:
    // bridge_not_running instead of a wake time, whatever the deadline.
    std::atomic<bool> const keep_going{false};
    auto result = bridge.sleep_until_ptp(phc.phc_ns + 10'000'000LL, &keep_going);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().value(), static_cast<int>(PtpError::bridge_not_running));
}

TEST(ptp_bridge_sleep, the_plan_is_the_deadline_on_both_local_clocks)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc phc;
    feed(bridge, state, phc, 64);

    // 10 ms after the instant the next reading would start at.
    auto const plan = bridge.prepare_sleep(phc.phc_ns + 10'000'000LL);
    EXPECT_TRUE(plan.has_value());
    EXPECT_EQ(plan->raw_deadline_ns, phc.raw_ns + 10'000'000LL);
    EXPECT_EQ(plan->mono_deadline_ns, phc.mono_ns + 10'000'000LL);
    EXPECT_EQ(plan->mono_deadline_ns, bridge.raw_line().to_monotonic_ns(plan->raw_deadline_ns));
    EXPECT_TRUE(std::abs(plan->mono_per_raw_slope - 1.0) < 1e-9);
    EXPECT_EQ(plan->before.epoch, bridge.epoch());
}

TEST(ptp_bridge_sleep, the_coarse_sleep_ends_a_guard_before_the_deadline)
{
    PtpTimeBridge bridge;
    auto params = told_params();
    params.wake_refine_spin_ns = 50'000;
    auto state = bridge.begin_told_sampling(params);
    ToldPhc phc;
    feed(bridge, state, phc, 64);

    // CLOCK_MONOTONIC at rate one against RAW: only the wakeup guard.
    auto const plan = bridge.prepare_sleep(phc.phc_ns + 10'000'000LL);
    EXPECT_TRUE(plan.has_value());
    EXPECT_EQ(bridge.early_wake_monotonic_ns(*plan, phc.raw_ns), plan->mono_deadline_ns - 50'000);
    // Told a RAW time already past the deadline, the slew term is nothing.
    EXPECT_EQ(bridge.early_wake_monotonic_ns(*plan, plan->raw_deadline_ns + 1), plan->mono_deadline_ns - 50'000);
}

TEST(ptp_bridge_sleep, a_slewed_monotonic_clock_widens_the_guard_by_what_the_sleep_could_accrue)
{
    PtpTimeBridge bridge;
    auto params = told_params();
    params.wake_refine_spin_ns = 50'000;
    auto state = bridge.begin_told_sampling(params);
    ToldPhc phc{.mono_ppm = 1'000};  // CLOCK_MONOTONIC slewed 1000 ppm fast
    feed(bridge, state, phc, 64);

    auto const plan = bridge.prepare_sleep(phc.phc_ns + 10'000'000LL);
    EXPECT_TRUE(plan.has_value());
    EXPECT_TRUE(std::abs(plan->mono_per_raw_slope - 1.001) < 1e-6);
    // 10 ms of sleep at 1000 ppm could diverge by 10 us: that much earlier.
    int64_t const guard = plan->mono_deadline_ns - bridge.early_wake_monotonic_ns(*plan, phc.raw_ns);
    EXPECT_TRUE(guard >= 50'000 + 9'950 && guard <= 50'000 + 10'050);
}

TEST(ptp_bridge_sleep, the_wake_is_told_in_ptp_through_the_mapping_it_slept_under)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc phc;
    feed(bridge, state, phc, 64);
    auto const plan = bridge.prepare_sleep(phc.phc_ns + 10'000'000LL);
    EXPECT_TRUE(plan.has_value());

    // Woken 150 ns after the RAW deadline: 150 ns after the PTP one.
    auto const wake = bridge.finalize_wake(plan->raw_deadline_ns + 150, plan->before);
    EXPECT_TRUE(wake.has_value());
    EXPECT_EQ(wake.value_or(0), phc.phc_ns + 10'000'000LL + 150);

    // A new epoch while it slept: the wake belongs to no timeline.
    bridge.reset_epoch();
    auto const stale = bridge.finalize_wake(plan->raw_deadline_ns + 150, plan->before);
    EXPECT_FALSE(stale.has_value());
    EXPECT_EQ(stale.error().value(), static_cast<int>(PtpError::bridge_epoch_changed));
}

// The one test of the sleep itself on the real clocks. It says nothing of
// how long the sleep took, only what no amount of load can change: it does
// not end before its deadline, and the wake it tells is one instant on both
// clocks.
TEST(ptp_bridge_sleep, a_real_sleep_never_ends_before_its_deadline)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc const phc = tell_history_ending_now(bridge, state);

    int64_t const deadline_ptp = phc.phc_ns + 2'000'000LL;
    auto const result = bridge.sleep_until_ptp(deadline_ptp);

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(result->ptp_ns >= deadline_ptp);
    EXPECT_TRUE(result->raw_ns >= bridge.to_raw_ns(deadline_ptp));
    EXPECT_EQ(bridge.from_raw_ns(result->raw_ns), result->ptp_ns);
    EXPECT_TRUE(statusbar::realtime::read_monotonic_raw_ns() >= result->raw_ns);
}

//
// Tests: PtpTimeBridge - get_mapping() consistency
//

TEST(ptp_bridge_mapping, get_mapping_returns_consistent_snapshot)
{
    PtpTimeBridge bridge;

    auto guard = bridge.start_sampling(counting_reader());
    EXPECT_TRUE(guard.has_value());

    // Get multiple mappings and verify they're consistent
    auto m1 = bridge.get_mapping();
    auto m2 = bridge.get_mapping();

    // Epoch should be the same (or m2 >= m1)
    EXPECT_TRUE(m2.epoch >= m1.epoch);

    bridge.stop_sampling();
}

//
// Main test runner
//

//
// Tests: PtpTimeBridge - the reference clock is CLOCK_MONOTONIC_RAW
//

static_assert(statusbar::realtime::ClockSource<PtpTimeBridge>);
static_assert(
    statusbar::realtime::ClockAdapterType<statusbar::realtime::ClockAdapter<statusbar::realtime::GptpClock0, PtpTimeBridge>>);

TEST(ptp_bridge_raw, the_raw_conversions_are_the_primary_line_and_the_mono_line_starts_as_identity)
{
    PtpTimeBridge bridge;

    EXPECT_EQ(bridge.to_raw_ns(1'000'000'000LL), 1'000'000'000LL);
    EXPECT_EQ(bridge.from_raw_ns(1'000'000'000LL), 1'000'000'000LL);
    EXPECT_EQ(bridge.to_monotonic_ns(1'000'000'000LL), 1'000'000'000LL);
    EXPECT_EQ(bridge.from_monotonic_ns(1'000'000'000LL), 1'000'000'000LL);
    EXPECT_EQ(bridge.raw_line().consume().rate, 1.0);
    EXPECT_EQ(bridge.raw_line().consume().offset_ns, 0);
    EXPECT_FALSE(bridge.convert_ptp_to_raw(5).healthy);
    EXPECT_EQ(bridge.convert_raw_to_ptp(5).time_ns, 5);
}

TEST(ptp_bridge_raw, after_sampling_each_conversion_names_the_clock_it_returns)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc phc{.ppm = 30, .mono_ppm = -200};  // three clocks at three rates
    feed(bridge, state, phc, 64);
    EXPECT_TRUE(bridge.is_healthy());

    // The same instant on each clock: to_raw_ns gives the RAW one,
    // to_monotonic_ns the CLOCK_MONOTONIC one, through the line the sampler keeps.
    EXPECT_TRUE(std::abs(bridge.to_raw_ns(phc.phc_ns) - phc.raw_ns) <= 2);
    EXPECT_TRUE(std::abs(bridge.to_monotonic_ns(phc.phc_ns) - phc.mono_ns) <= 2);
    EXPECT_TRUE(std::abs(bridge.raw_line().to_monotonic_ns(phc.raw_ns) - phc.mono_ns) <= 2);

    // Both pairs round-trip, and the two pairs compose the same way.
    EXPECT_TRUE(std::abs(bridge.from_raw_ns(bridge.to_raw_ns(phc.phc_ns)) - phc.phc_ns) <= 2);
    EXPECT_TRUE(std::abs(bridge.from_monotonic_ns(bridge.to_monotonic_ns(phc.phc_ns)) - phc.phc_ns) <= 2);
    EXPECT_EQ(bridge.to_monotonic_ns(phc.phc_ns), bridge.raw_line().to_monotonic_ns(bridge.to_raw_ns(phc.phc_ns)));
}

//
// Tests: PtpTimer - each wake tells its RAW instant
//

// The PtpTimer tests run its real thread, since a thread waking and a stack
// being prefaulted are what they test. They block until the first wake has
// been told, however long the host takes to get there, and assert nothing
// about when that was.
TEST(ptp_timer, each_wake_carries_the_raw_instant_its_ptp_time_was_made_from)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    (void)tell_history_ending_now(bridge, state);
    EXPECT_TRUE(bridge.is_healthy());

    // The callback is told the wake on both clocks; it compares the RAW one
    // with the clock as it stands on entry and with the PTP one through the
    // bridge, and reads nothing else.
    std::atomic<int64_t> raw_skew_ns{INT64_MIN};
    std::atomic<int64_t> ptp_skew_ns{INT64_MAX};
    std::atomic<int64_t> error_skew_ns{INT64_MAX};
    std::atomic<int> wakes{0};
    std::atomic<bool> told{false};
    auto timer = make_ptp_timer(
        bridge,
        1'000'000,
        [&](statusbar::StatusValue<TimerWakeInfo> const& info) {
            if (!info || wakes.fetch_add(1) != 0) {
                return;
            }
            int64_t const raw_ns = statusbar::realtime::MonotonicRawClock::to_ns(info->actual_raw);
            raw_skew_ns.store(statusbar::realtime::read_monotonic_raw_ns() - raw_ns);
            ptp_skew_ns.store(bridge.from_raw_ns(raw_ns) - info->actual_time_ns);
            error_skew_ns.store((info->actual_time_ns - info->scheduled_time_ns) - info->error_ns);
            told.store(true);
            told.notify_all();
        },
        0,
        /*enable_realtime=*/false,
        /*cpu_affinity=*/-1);
    EXPECT_TRUE(timer.start().has_value());
    told.wait(false);
    timer.stop();

    // The RAW instant is one the clock had reached, and the PTP time is that
    // instant through the bridge's mapping, to the nanosecond.
    EXPECT_TRUE(raw_skew_ns.load() >= 0);
    EXPECT_EQ(ptp_skew_ns.load(), int64_t{0});
    EXPECT_EQ(error_skew_ns.load(), int64_t{0});
    bridge.stop_sampling();
}

namespace {

/// Use 64 KiB of stack, one write per page.
[[gnu::noinline]] auto use_stack() noexcept -> unsigned char
{
    constexpr size_t bytes = size_t{64} * 1024;
    unsigned char volatile buf[bytes];
    for (size_t i = 0; i < bytes; i += 4096) {
        buf[i] = static_cast<unsigned char>(i);
    }
    asm volatile("" : : "r"(&buf[0]) : "memory");
    return buf[0];
}

#if defined(__linux__)
/// Minor faults this thread has taken so far.
auto minor_faults() -> long
{
    rusage usage{};
    (void)getrusage(RUSAGE_THREAD, &usage);
    return usage.ru_minflt;
}
#endif

}  // namespace

TEST(ptp_timer, a_stack_prefault_leaves_the_timer_thread_nothing_to_fault_on)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    (void)tell_history_ending_now(bridge, state);
    EXPECT_TRUE(bridge.is_healthy());

    // The first wake uses 64 KiB of stack and counts the minor faults that
    // took: none, once the thread prefaulted 128 KiB before its loop.
    (void)use_stack();  // its code is resident before anything is counted
    std::atomic<long> faults{-1};
    std::atomic<int> wakes{0};
    std::atomic<bool> told{false};
    auto timer = make_ptp_timer(
        bridge,
        1'000'000,
        [&](statusbar::StatusValue<TimerWakeInfo> const& info) {
            if (!info || wakes.fetch_add(1) != 0) {
                return;
            }
#if defined(__linux__)
            (void)minor_faults();  // and so is getrusage's
            long const before = minor_faults();
            (void)use_stack();
            faults.store(minor_faults() - before);
#else
            (void)use_stack();
            faults.store(0);
#endif
            told.store(true);
            told.notify_all();
        },
        0,
        /*enable_realtime=*/false,
        /*cpu_affinity=*/-1);
    EXPECT_EQ(timer.stack_prefault_bytes(), size_t{0});
    timer.set_stack_prefault(size_t{128} * 1024);
    EXPECT_EQ(timer.stack_prefault_bytes(), size_t{128} * 1024);
    EXPECT_TRUE(timer.start().has_value());
    told.wait(false);
    timer.stop();

    EXPECT_EQ(faults.load(), 0);
    bridge.stop_sampling();
}

TEST_MAIN(statusbar_ptpclient, ptpclient_bridge_test)