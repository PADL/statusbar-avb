// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// PtpTimeBridge and PtpTimer with real threads and the real clocks: the
// sampler thread's start and stop, one sleep to a deadline, and the timer
// thread's first wake. The bridge's arithmetic is tested on made-up time in
// ptpclient_bridge_test.cpp; here it is the threads and the waits that are
// tried, and nothing is asserted that depends on how long they took.

#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/ptpclient/ptpclient_bridge_test_support.hpp"
#include "statusbar/realtime/realtime_base.hpp"
#include "statusbar/realtime/realtime_clock.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/test/test.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <sys/resource.h>

using namespace statusbar::ptpclient;
using namespace statusbar::ptpclient::test_support;

namespace {

/// For the tests that let real code wait on the real clocks: tell the
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
// Tests: PtpTimeBridge - sleep_until_ptp on the real clocks
//

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
    auto const result = bridge.sleep_until_ptp(ptp(deadline_ptp));

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(result->ptp >= ptp(deadline_ptp));
    EXPECT_TRUE(result->raw >= raw(bridge.to_raw_ns(deadline_ptp)));
    EXPECT_TRUE(ptp(bridge.from_raw_ns(MonotonicRawClock::to_ns(result->raw))) == result->ptp);
    EXPECT_TRUE(statusbar::realtime::read_monotonic_raw() >= result->raw);
}

TEST(ptp_bridge_sleep, a_wake_that_never_slept_tells_no_headroom)
{
    PtpTimeBridge bridge;
    auto state = bridge.begin_told_sampling(told_params());
    ToldPhc const phc = tell_history_ending_now(bridge, state);

    // A deadline already behind: nothing to sleep for, and the spin ends at once.
    constexpr Nanoseconds untouched{-1};
    Nanoseconds headroom = untouched;
    auto const result = bridge.sleep_until_ptp(ptp(phc.phc_ns - 1'000'000LL), nullptr, &headroom);

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(headroom == untouched);
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
            ptp_skew_ns.store(bridge.from_raw_ns(raw_ns) - info->actual_time_ns());
            error_skew_ns.store(((info->actual_time - info->scheduled_time) - info->error).count());
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

TEST_MAIN(statusbar_ptpclient, ptpclient_bridge_functional_test)
