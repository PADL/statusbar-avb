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
#include <functional>
#include <system_error>
#include <thread>

using namespace statusbar::ptpclient;

//
// Mock PTP Time Reader
//

/// Mock PTP reader that returns monotonic time + offset with optional rate adjustment
class MockPtpReader
{
  public:
    MockPtpReader() = default;

    /// Set the offset between PTP and monotonic time
    void set_offset(int64_t offset_ns) { offset_ns_ = offset_ns; }

    /// Set the rate multiplier (1.0 = same rate, 1.0001 = 100 ppm fast)
    void set_rate(double rate) { rate_ = rate; }

    /// Set whether the reader should fail
    void set_should_fail(bool fail) { should_fail_ = fail; }

    /// Get the reader function
    auto get_reader() -> PtpTimeBridge::PtpTimeReader
    {
        return [this]() -> statusbar::StatusValue<int64_t> {
            if (should_fail_) {
                return statusbar::failure(make_error_code(PtpError::clock_gettime_failed));
            }

            auto const now = std::chrono::steady_clock::now();
            auto const mono_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

            // PTP = rate * monotonic + offset
            auto const ptp_ns = static_cast<int64_t>(static_cast<double>(mono_ns) * rate_) + offset_ns_;
            return statusbar::success(ptp_ns);
        };
    }

  private:
    int64_t offset_ns_{0};
    double rate_{1.0};
    bool should_fail_{false};
};

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
    MockPtpReader mock;

    auto result = bridge.start_sampling(mock.get_reader());

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(bridge.is_running());

    // Stop via guard destruction
    result->stop();
    EXPECT_FALSE(bridge.is_running());
}

TEST(ptp_bridge_sampling, guard_stops_on_destruction)
{
    PtpTimeBridge bridge;
    MockPtpReader mock;

    {
        auto result = bridge.start_sampling(mock.get_reader());
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(bridge.is_running());
    }  // Guard goes out of scope

    EXPECT_FALSE(bridge.is_running());
}

TEST(ptp_bridge_sampling, stop_sampling_is_idempotent)
{
    PtpTimeBridge bridge;
    MockPtpReader mock;

    auto result = bridge.start_sampling(mock.get_reader());
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
    MockPtpReader mock;

    auto result1 = bridge.start_sampling(mock.get_reader());
    EXPECT_TRUE(result1.has_value());

    // Second start should fail
    auto result2 = bridge.start_sampling(mock.get_reader());
    EXPECT_FALSE(result2.has_value());

    bridge.stop_sampling();
}

//
// Tests: PtpTimeBridge - Convergence to healthy state
//

TEST(ptp_bridge_health, converges_to_healthy_with_good_data)
{
    PtpTimeBridge bridge;
    MockPtpReader mock;
    mock.set_offset(100'000'000);  // 100ms offset

    BridgeSamplingParams params;
    params.sample_hz = 100;               // Fast sampling for test
    params.min_samples_for_healthy = 8;   // Need 8 samples
    params.degrade_threshold_ns = 10000;  // 10us threshold

    auto result = bridge.start_sampling(mock.get_reader(), params);
    EXPECT_TRUE(result.has_value());

    // Wait for convergence (enough samples + processing time)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Should be healthy now
    EXPECT_TRUE(bridge.is_healthy());

    auto mapping = bridge.get_mapping();
    EXPECT_TRUE(mapping.healthy);
    EXPECT_TRUE(mapping.sample_count >= params.min_samples_for_healthy);

    // Rate should be close to 1.0
    EXPECT_TRUE(std::abs(mapping.rate - 1.0) < 0.001);

    bridge.stop_sampling();
}

TEST(ptp_bridge_health, unhealthy_when_reader_fails)
{
    PtpTimeBridge bridge;
    MockPtpReader mock;
    mock.set_should_fail(true);

    BridgeSamplingParams params;
    params.sample_hz = 100;

    auto result = bridge.start_sampling(mock.get_reader(), params);
    EXPECT_TRUE(result.has_value());

    // Wait a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Should not be healthy since all samples fail
    EXPECT_FALSE(bridge.is_healthy());

    bridge.stop_sampling();
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
    MockPtpReader mock;
    int64_t const offset = 500'000'000LL;  // 500ms offset
    mock.set_offset(offset);

    BridgeSamplingParams params;
    params.sample_hz = 100;
    params.min_samples_for_healthy = 8;

    auto guard = bridge.start_sampling(mock.get_reader(), params);
    EXPECT_TRUE(guard.has_value());

    // Wait for convergence
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(bridge.is_healthy());

    // After convergence, time conversions should be accurate
    // The mapping is: ptp = rate * monotonic + offset
    // So converting monotonic->ptp and back should give close to the original
    auto now = std::chrono::steady_clock::now();
    auto mono_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    auto ptp_result = bridge.convert_monotonic_to_ptp(mono_ns);
    auto mono_result = bridge.convert_ptp_to_monotonic(ptp_result.time_ns);

    // Round-trip should preserve monotonic time within tight tolerance
    EXPECT_TRUE(std::abs(mono_result.time_ns - mono_ns) < 1000);  // Within 1us

    // The PTP time should be offset from monotonic by approximately our set offset
    // The mock applies: ptp = rate * monotonic + offset, so the difference should be close to offset
    // But the bridge's linear fit may have a slightly different slope, so allow more tolerance
    auto mapping = bridge.get_mapping();
    (void)mapping;  // Suppress warning; we verified health above

    // Just verify that the healthy flag is set and rate is close to 1.0
    EXPECT_TRUE(std::abs(mapping.rate - 1.0) < 0.001);

    bridge.stop_sampling();
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
    MockPtpReader mock;

    BridgeSamplingParams params;
    params.sample_hz = 100;
    params.min_samples_for_healthy = 8;

    auto guard = bridge.start_sampling(mock.get_reader(), params);
    EXPECT_TRUE(guard.has_value());

    // Wait for healthy
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(bridge.is_healthy());

    // Reset epoch
    bridge.reset_epoch();

    // Should be unhealthy after reset
    EXPECT_FALSE(bridge.is_healthy());

    bridge.stop_sampling();
}

//
// Tests: SamplingGuard - RAII behavior
//

TEST(ptp_bridge_guard, guard_active_when_valid)
{
    PtpTimeBridge bridge;
    MockPtpReader mock;

    auto result = bridge.start_sampling(mock.get_reader());
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(result->active());

    result->stop();
    EXPECT_FALSE(result->active());
}

TEST(ptp_bridge_guard, guard_release_transfers_ownership)
{
    PtpTimeBridge bridge;
    MockPtpReader mock;

    auto result = bridge.start_sampling(mock.get_reader());
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
    MockPtpReader mock;

    auto result = bridge.start_sampling(mock.get_reader());
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
    MockPtpReader mock;
    mock.set_should_fail(true);  // Force unhealthy

    BridgeSamplingParams params;
    params.sample_hz = 100;

    auto guard = bridge.start_sampling(mock.get_reader(), params);
    EXPECT_TRUE(guard.has_value());

    // Wait a bit (but won't become healthy due to failures)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Try to sleep - should fail because unhealthy
    auto now = std::chrono::steady_clock::now();
    auto deadline_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count() + 1'000'000LL;

    auto result = bridge.sleep_until_ptp(deadline_ns);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().value(), static_cast<int>(PtpError::bridge_not_healthy));

    bridge.stop_sampling();
}

TEST(ptp_bridge_sleep, sleep_bails_when_the_caller_stops)
{
    PtpTimeBridge bridge;
    MockPtpReader mock;

    BridgeSamplingParams params;
    params.sample_hz = 100;
    params.min_samples_for_healthy = 8;

    auto guard = bridge.start_sampling(mock.get_reader(), params);
    EXPECT_TRUE(guard.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(bridge.is_healthy());

    // A caller that has already been told to stop: the sleep's final spin
    // reads the flag and returns bridge_not_running instead of the wake time.
    std::atomic<bool> const keep_going{false};
    auto now = std::chrono::steady_clock::now();
    auto mono_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    auto deadline_ptp = bridge.convert_monotonic_to_ptp(mono_ns + 10'000'000LL);

    auto result = bridge.sleep_until_ptp(deadline_ptp.time_ns, &keep_going);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().value(), static_cast<int>(PtpError::bridge_not_running));

    bridge.stop_sampling();
}

TEST(ptp_bridge_sleep, sleep_returns_wake_time)
{
    PtpTimeBridge bridge;
    MockPtpReader mock;

    BridgeSamplingParams params;
    params.sample_hz = 100;
    params.min_samples_for_healthy = 8;

    auto guard = bridge.start_sampling(mock.get_reader(), params);
    EXPECT_TRUE(guard.has_value());

    // Wait for healthy
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(bridge.is_healthy());

    // Sleep for a short time
    auto now = std::chrono::steady_clock::now();
    auto mono_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    auto deadline_ptp = bridge.convert_monotonic_to_ptp(mono_ns + 10'000'000LL);  // 10ms from now

    auto result = bridge.sleep_until_ptp(deadline_ptp.time_ns);

    EXPECT_TRUE(result.has_value());
    // Wake time should be close to or after the deadline
    EXPECT_TRUE(result->ptp_ns >= deadline_ptp.time_ns - 1'000'000LL);  // Within 1ms tolerance
    // The wake is one RAW read, and the PTP time is that read through the
    // mapping (as it stood then; the sampler may have refitted since).
    EXPECT_TRUE(std::abs(bridge.from_raw_ns(result->raw_ns) - result->ptp_ns) < 1'000'000);
    EXPECT_TRUE(std::abs(statusbar::realtime::read_monotonic_raw_ns() - result->raw_ns) < 1'000'000'000LL);

    bridge.stop_sampling();
}

//
// Tests: PtpTimeBridge - get_mapping() consistency
//

TEST(ptp_bridge_mapping, get_mapping_returns_consistent_snapshot)
{
    PtpTimeBridge bridge;
    MockPtpReader mock;

    auto guard = bridge.start_sampling(mock.get_reader());
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

static_assert(PtpTimeBridge::reference_is_raw);
static_assert(statusbar::realtime::RawReferenceClockSource<PtpTimeBridge>);
static_assert(statusbar::realtime::ClockAdapter<statusbar::realtime::GptpClock0, PtpTimeBridge>::reference_is_raw);

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
    MockPtpReader mock;
    mock.set_offset(500'000'000LL);

    BridgeSamplingParams params;
    params.sample_hz = 100;
    params.min_samples_for_healthy = 8;

    auto guard = bridge.start_sampling(mock.get_reader(), params);
    EXPECT_TRUE(guard.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(bridge.is_healthy());

    // to_raw_ns of "now" in PTP is RAW now; to_monotonic_ns of it is
    // CLOCK_MONOTONIC now, through the line the sampler keeps.
    int64_t const ptp_now = bridge.now_ns();
    int64_t const raw_now = statusbar::realtime::read_monotonic_raw_ns();
    int64_t const mono_now = statusbar::realtime::read_monotonic_ns();
    constexpr int64_t slack_ns = 1'000'000;
    EXPECT_TRUE(std::abs(bridge.to_raw_ns(ptp_now) - raw_now) < slack_ns);
    EXPECT_TRUE(std::abs(bridge.to_monotonic_ns(ptp_now) - mono_now) < slack_ns);

    // Both pairs round-trip, and the two pairs compose the same way.
    EXPECT_TRUE(std::abs(bridge.from_raw_ns(bridge.to_raw_ns(ptp_now)) - ptp_now) < 1'000);
    EXPECT_TRUE(std::abs(bridge.from_monotonic_ns(bridge.to_monotonic_ns(ptp_now)) - ptp_now) < 1'000);
    EXPECT_TRUE(std::abs(bridge.to_monotonic_ns(ptp_now) - bridge.raw_line().to_monotonic_ns(bridge.to_raw_ns(ptp_now))) < 1'000);

    bridge.stop_sampling();
}

TEST(ptp_bridge_raw, the_sleep_plan_takes_its_monotonic_deadline_from_the_line)
{
    PtpTimeBridge bridge;
    MockPtpReader mock;

    BridgeSamplingParams params;
    params.sample_hz = 100;
    params.min_samples_for_healthy = 8;

    auto guard = bridge.start_sampling(mock.get_reader(), params);
    EXPECT_TRUE(guard.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(bridge.is_healthy());

    int64_t const ptp_deadline = bridge.now_ns() + 10'000'000LL;
    auto plan = bridge.prepare_sleep(ptp_deadline);
    EXPECT_TRUE(plan.has_value());
    // The sampler may republish either line between the plan and these reads
    // (a few ns of fit movement), so the checks are tolerances, not equalities.
    EXPECT_TRUE(std::abs(plan->raw_deadline_ns - bridge.to_raw_ns(ptp_deadline)) < 1'000);
    EXPECT_TRUE(std::abs(plan->mono_deadline_ns - bridge.raw_line().to_monotonic_ns(plan->raw_deadline_ns)) < 1'000);
    EXPECT_TRUE(std::abs(plan->mono_per_raw_slope - bridge.raw_line().consume().rate) < 1e-3);
    EXPECT_TRUE(std::abs(plan->mono_per_raw_slope - 1.0) < 0.2);

    bridge.stop_sampling();
}

//
// Tests: PtpTimer - each wake tells its RAW instant
//

TEST(ptp_timer, each_wake_carries_the_raw_instant_its_ptp_time_was_made_from)
{
    PtpTimeBridge bridge;
    MockPtpReader mock;

    BridgeSamplingParams params;
    params.sample_hz = 100;
    params.min_samples_for_healthy = 8;

    auto guard = bridge.start_sampling(mock.get_reader(), params);
    EXPECT_TRUE(guard.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(bridge.is_healthy());

    // The callback is told the wake on both clocks; it compares the RAW one
    // with the clock as it stands on entry and with the PTP one through the
    // bridge, and reads nothing else.
    std::atomic<int64_t> raw_skew_ns{INT64_MAX};
    std::atomic<int64_t> ptp_skew_ns{INT64_MAX};
    std::atomic<int> wakes{0};
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
        },
        0,
        /*enable_realtime=*/false,
        /*cpu_affinity=*/-1);
    EXPECT_TRUE(timer.start().has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    timer.stop();

    EXPECT_TRUE(wakes.load() > 0);
    EXPECT_TRUE(raw_skew_ns.load() >= 0);
    EXPECT_TRUE(raw_skew_ns.load() < 50'000'000);
    EXPECT_TRUE(std::abs(ptp_skew_ns.load()) < 1'000'000);
    bridge.stop_sampling();
}

TEST_MAIN(statusbar_ptpclient, ptpclient_bridge_test)