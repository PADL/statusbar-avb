// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/ptpclient/ptpclient_bridge_test_support.hpp"
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

using namespace statusbar::ptpclient;

//
// How these tests tell the time
//
// The bridge is told every reading (begin_told_sampling / observe): the
// times are made up, no thread runs and nothing waits. The sampler thread's
// own start and stop, the sleep on the real clocks and the PtpTimer thread
// are tried in ptpclient_bridge_functional_test.cpp.
//

using namespace statusbar::ptpclient::test_support;

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

TEST(ptp_bridge_convert, before_any_sample_the_lines_are_identity_and_nothing_is_vouched_for)
{
    PtpTimeBridge bridge;

    EXPECT_TRUE(bridge.to_monotonic(ptp(1'000'000'000LL)) == mono(1'000'000'000LL));
    EXPECT_TRUE(bridge.from_monotonic(mono(1'000'000'000LL)) == ptp(1'000'000'000LL));
    EXPECT_EQ(bridge.epoch(), 0U);
    EXPECT_FALSE(bridge.is_healthy());
    EXPECT_FALSE(bridge.quality().is_bounded());  // the bound a caller judges a conversion by
}

TEST(ptp_bridge_convert, conversion_after_convergence)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc phc;
    feed(bridge, state, phc, 64);
    EXPECT_TRUE(bridge.is_healthy());
    EXPECT_TRUE(bridge.quality().is_bounded());

    // The instant the next reading would start at, on all three clocks.
    EXPECT_TRUE(bridge.from_monotonic(mono(phc.mono_ns)) == ptp(phc.phc_ns));
    EXPECT_TRUE(bridge.to_monotonic(ptp(phc.phc_ns)) == mono(phc.mono_ns));
    EXPECT_EQ(bridge.from_raw_ns(phc.raw_ns), phc.phc_ns);
    EXPECT_EQ(bridge.to_raw_ns(phc.phc_ns), phc.raw_ns);

    // The same through the adapter, which is how a caller holds typed time.
    statusbar::realtime::ClockAdapter<GptpClock0, PtpTimeBridge> const adapter{bridge};
    EXPECT_TRUE(adapter.to_raw(ptp(phc.phc_ns)) == raw(phc.raw_ns));
    EXPECT_TRUE(adapter.from_raw(raw(phc.raw_ns)) == ptp(phc.phc_ns));
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

TEST(ptp_bridge_quality, a_phc_slewed_past_the_rate_window_is_not_vouched_for)
{
    PtpTimeBridge bridge;
    auto const params = told_params();
    auto state = bridge.begin_told_sampling(params);
    // A servo slewing the PHC 300 ppm against RAW, past the 200 ppm the
    // published rate is held within.
    EXPECT_TRUE(params.max_rate_ppm < 300.0);
    ToldPhc phc{.ppm = 300};
    feed(bridge, state, phc, 64);

    // The readings lie on a clean line, so the fit is healthy and a timer
    // on the bridge keeps running. The line that is published is not that
    // line: it is held at the edge of the window and runs away from the
    // PHC at 100 ppm, microseconds across the window. No bound is claimed.
    EXPECT_TRUE(bridge.is_healthy());
    EXPECT_FALSE(bridge.quality().is_bounded());
    EXPECT_TRUE(std::abs(bridge.from_raw_ns(phc.raw_ns) - phc.phc_ns) >= 2'000);

    // The servo settles inside the window: bounded again, on the same epoch.
    uint64_t const epoch = bridge.epoch();
    phc.ppm = 150;
    feed(bridge, state, phc, 96);
    EXPECT_TRUE(bridge.quality().is_bounded());
    EXPECT_EQ(bridge.quality().epoch, epoch);
    EXPECT_TRUE(std::abs(bridge.from_raw_ns(phc.raw_ns) - phc.phc_ns) <= 5);
}

TEST(ptp_bridge_quality, the_bound_is_measured_against_the_line_the_conversions_use)
{
    // What quality() claims must cover what a conversion really does: at
    // every sample of the window the published line is within the bound.
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc phc{.ppm = 97};
    constexpr int64_t SCATTER_NS[4] = {700, -700, -700, 700};
    for (int i = 0; i < 64; ++i) {
        int64_t const truth = phc.phc_ns + (phc.bracket_ns / 2);
        (void)bridge.observe(state, phc.next(SCATTER_NS[i % 4]));
        if (i >= 32) {
            int64_t const converted = bridge.from_raw_ns(phc.last_sample_raw_ns());
            EXPECT_TRUE(std::abs(converted - truth) <= bridge.quality().bound_at_sync.count());
        }
    }
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
// Tests: PtpTimeBridge - sleep_until_ptp
//

TEST(ptp_bridge_sleep, sleep_fails_when_not_running)
{
    PtpTimeBridge bridge;

    auto result = bridge.sleep_until_ptp(ptp(1'000'000'000LL));

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
    auto result = bridge.sleep_until_ptp(ptp(phc.phc_ns + 1'000'000LL));

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().value(), static_cast<int>(PtpError::bridge_not_healthy));
    EXPECT_EQ(bridge.prepare_sleep(ptp(phc.phc_ns + 1'000'000LL)).error().value(), static_cast<int>(PtpError::bridge_not_healthy));
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
    auto result = bridge.sleep_until_ptp(ptp(phc.phc_ns + 10'000'000LL), &keep_going);

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
    auto const plan = bridge.prepare_sleep(ptp(phc.phc_ns + 10'000'000LL));
    EXPECT_TRUE(plan.has_value());
    EXPECT_TRUE(plan->raw_deadline == raw(phc.raw_ns + 10'000'000LL));
    EXPECT_TRUE(plan->mono_deadline == mono(phc.mono_ns + 10'000'000LL));
    EXPECT_TRUE(plan->mono_deadline == bridge.raw_line().to_monotonic(plan->raw_deadline));
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
    auto const plan = bridge.prepare_sleep(ptp(phc.phc_ns + 10'000'000LL));
    EXPECT_TRUE(plan.has_value());
    EXPECT_TRUE(bridge.early_wake_monotonic(*plan, raw(phc.raw_ns)) == plan->mono_deadline - 50us);
    // Told a RAW time already past the deadline, the slew term is nothing.
    EXPECT_TRUE(bridge.early_wake_monotonic(*plan, plan->raw_deadline + 1ns) == plan->mono_deadline - 50us);
}

TEST(ptp_bridge_sleep, a_slewed_monotonic_clock_widens_the_guard_by_what_the_sleep_could_accrue)
{
    PtpTimeBridge bridge;
    auto params = told_params();
    params.wake_refine_spin_ns = 50'000;
    auto state = bridge.begin_told_sampling(params);
    ToldPhc phc{.mono_ppm = 1'000};  // CLOCK_MONOTONIC slewed 1000 ppm fast
    feed(bridge, state, phc, 64);

    auto const plan = bridge.prepare_sleep(ptp(phc.phc_ns + 10'000'000LL));
    EXPECT_TRUE(plan.has_value());
    EXPECT_TRUE(std::abs(plan->mono_per_raw_slope - 1.001) < 1e-6);
    // 10 ms of sleep at 1000 ppm could diverge by 10 us: that much earlier.
    int64_t const guard = (plan->mono_deadline - bridge.early_wake_monotonic(*plan, raw(phc.raw_ns))).count();
    EXPECT_TRUE(guard >= 50'000 + 9'950 && guard <= 50'000 + 10'050);
}

TEST(ptp_bridge_sleep, the_wake_is_told_in_ptp_through_the_mapping_it_slept_under)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc phc;
    feed(bridge, state, phc, 64);
    auto const plan = bridge.prepare_sleep(ptp(phc.phc_ns + 10'000'000LL));
    EXPECT_TRUE(plan.has_value());

    // Woken 150 ns after the RAW deadline: 150 ns after the PTP one.
    auto const wake = bridge.finalize_wake(plan->raw_deadline + 150ns, plan->before);
    EXPECT_TRUE(wake.has_value());
    EXPECT_TRUE(*wake == ptp(phc.phc_ns + 10'000'000LL + 150));

    // A new epoch while it slept: the wake belongs to no timeline.
    bridge.reset_epoch();
    auto const stale = bridge.finalize_wake(plan->raw_deadline + 150ns, plan->before);
    EXPECT_FALSE(stale.has_value());
    EXPECT_EQ(stale.error().value(), static_cast<int>(PtpError::bridge_epoch_changed));
}

TEST(ptp_bridge_sleep, the_headroom_is_the_spin_window_less_how_late_the_kernel_woke_the_thread)
{
    PtpTimeBridge bridge;
    auto params = told_params();
    params.wake_refine_spin_ns = 50'000;
    auto state = bridge.begin_told_sampling(params);
    ToldPhc phc;
    feed(bridge, state, phc, 64);

    auto const plan = bridge.prepare_sleep(ptp(phc.phc_ns + 10'000'000LL));
    EXPECT_TRUE(plan.has_value());
    auto const mono_wake = bridge.early_wake_monotonic(*plan, raw(phc.raw_ns));
    // CLOCK_MONOTONIC at rate one: the early wake is the window before the deadline.
    auto const on_time = plan->raw_deadline - 50us;

    EXPECT_TRUE(bridge.spin_headroom(*plan, mono_wake, on_time) == 50us);          // woken on time: all of it
    EXPECT_TRUE(bridge.spin_headroom(*plan, mono_wake, on_time + 20us) == 30us);   // 20 us late
    EXPECT_TRUE(bridge.spin_headroom(*plan, mono_wake, on_time + 50us) == 0us);    // at the deadline
    EXPECT_TRUE(bridge.spin_headroom(*plan, mono_wake, on_time + 80us) == -30us);  // overran it
}

TEST(ptp_bridge_sleep, the_slew_term_of_the_early_wake_is_not_counted_as_headroom)
{
    PtpTimeBridge bridge;
    auto params = told_params();
    params.wake_refine_spin_ns = 50'000;
    auto state = bridge.begin_told_sampling(params);
    ToldPhc phc{.mono_ppm = 1'000};  // CLOCK_MONOTONIC slewed 1000 ppm fast
    feed(bridge, state, phc, 64);

    auto const plan = bridge.prepare_sleep(ptp(phc.phc_ns + 10'000'000LL));
    EXPECT_TRUE(plan.has_value());
    // The early wake is about 60 us ahead: the window and 10 us for the slew.
    auto const mono_wake = bridge.early_wake_monotonic(*plan, raw(phc.raw_ns));
    auto const raw_at_wake = bridge.raw_line().from_monotonic(mono_wake);

    // Woken on time, the thread has the window, not the window and the slew term.
    EXPECT_TRUE(abs_ns(bridge.spin_headroom(*plan, mono_wake, raw_at_wake) - 50us) <= 100);
    EXPECT_TRUE(abs_ns(bridge.spin_headroom(*plan, mono_wake, raw_at_wake + 20us) - 30us) <= 100);
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
    EXPECT_TRUE(bridge.to_monotonic(ptp(1'000'000'000LL)) == mono(1'000'000'000LL));
    EXPECT_TRUE(bridge.from_monotonic(mono(1'000'000'000LL)) == ptp(1'000'000'000LL));
    EXPECT_EQ(bridge.raw_line().consume().rate, 1.0);
    EXPECT_EQ(bridge.raw_line().consume().offset_ns, 0);
    EXPECT_FALSE(bridge.is_healthy());
    EXPECT_EQ(bridge.from_raw_ns(5), 5);
}

TEST(ptp_bridge_raw, after_sampling_each_conversion_names_the_clock_it_returns)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc phc{.ppm = 30, .mono_ppm = -200};  // three clocks at three rates
    feed(bridge, state, phc, 64);
    EXPECT_TRUE(bridge.is_healthy());

    // The same instant on each clock: to_raw_ns gives the RAW one,
    // to_monotonic the CLOCK_MONOTONIC one, through the line the sampler keeps.
    EXPECT_TRUE(std::abs(bridge.to_raw_ns(phc.phc_ns) - phc.raw_ns) <= 2);
    EXPECT_TRUE(abs_ns(bridge.to_monotonic(ptp(phc.phc_ns)) - mono(phc.mono_ns)) <= 2);
    EXPECT_TRUE(std::abs(bridge.raw_line().to_monotonic_ns(phc.raw_ns) - phc.mono_ns) <= 2);

    // Both pairs round-trip, and the two pairs compose the same way.
    EXPECT_TRUE(std::abs(bridge.from_raw_ns(bridge.to_raw_ns(phc.phc_ns)) - phc.phc_ns) <= 2);
    EXPECT_TRUE(abs_ns(bridge.from_monotonic(bridge.to_monotonic(ptp(phc.phc_ns))) - ptp(phc.phc_ns)) <= 2);
    EXPECT_TRUE(bridge.to_monotonic(ptp(phc.phc_ns)) == bridge.raw_line().to_monotonic(raw(bridge.to_raw_ns(phc.phc_ns))));
}

TEST_MAIN(statusbar_ptpclient, ptpclient_bridge_test)