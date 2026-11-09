// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// PulseLock: a sink PHC's lock to a source's 1 PPS from crafted timestamps and a fake local clock;
// ExttsReader's refusals.

#include "statusbar/ptpclient/ptpclient_pulse.hpp"

#include "statusbar/test/test.hpp"

#include <cstdint>

using statusbar::ptpclient::ExttsReader;
using statusbar::ptpclient::PulseLock;

namespace {

constexpr int64_t SECOND_NS = 1'000'000'000;
/// A grandmaster off the PTP timescale: early 1970.
constexpr int64_t EPOCH_NS = 691'752 * SECOND_NS;

/// The rising edge of second @p k as the sink stamps it, @p offset_ns off, read 2 ms later on both clocks.
struct Pulse
{
    int64_t event_ns;
    int64_t reference_ns;
    int64_t local_ns;
};

auto pulse_at(int64_t const k, int64_t const offset_ns) -> Pulse
{
    int64_t const edge = EPOCH_NS + (k * SECOND_NS);
    return {.event_ns = edge + offset_ns, .reference_ns = edge + 2'000'000, .local_ns = (k * SECOND_NS) + 2'000'000};
}

auto feed(PulseLock& lock, Pulse const& p) -> bool
{
    return lock.pulse(p.event_ns, p.reference_ns, p.local_ns);
}

}  // namespace

TEST(ptpclient_pulse, three_pulses_within_the_bound_lock)
{
    PulseLock lock;
    EXPECT_FALSE(feed(lock, pulse_at(1, 40)));
    EXPECT_FALSE(feed(lock, pulse_at(2, -60)));
    EXPECT_FALSE(lock.locked());
    EXPECT_TRUE(feed(lock, pulse_at(3, 25)));
    EXPECT_TRUE(lock.locked());
    EXPECT_EQ(lock.offset_ns(), int64_t{25});
}

TEST(ptpclient_pulse, the_falling_edge_is_ignored)
{
    PulseLock lock;
    for (int64_t k = 1; k <= 3; ++k) {
        (void)feed(lock, pulse_at(k, 0));
        Pulse falling = pulse_at(k, SECOND_NS / 2);
        EXPECT_FALSE(feed(lock, falling));
    }
    EXPECT_TRUE(lock.locked());
    EXPECT_EQ(lock.offset_ns(), int64_t{0});
}

TEST(ptpclient_pulse, a_pulse_off_by_more_than_the_bound_unlocks)
{
    PulseLock lock;
    for (int64_t k = 1; k <= 3; ++k) {
        (void)feed(lock, pulse_at(k, 100));
    }
    EXPECT_TRUE(feed(lock, pulse_at(4, 30'000)));
    EXPECT_FALSE(lock.locked());
    EXPECT_EQ(lock.offset_ns(), int64_t{30'000});
    EXPECT_FALSE(feed(lock, pulse_at(5, 0)));
    EXPECT_FALSE(feed(lock, pulse_at(6, 0)));
    EXPECT_TRUE(feed(lock, pulse_at(7, 0)));
}

// A sink stepped by whole seconds (an adapter reset writing the system time) shows a good sub-second
// offset; the reference clock's second gives it away.
TEST(ptpclient_pulse, a_sink_whole_seconds_off_never_locks)
{
    PulseLock lock;
    for (int64_t k = 1; k <= 5; ++k) {
        Pulse p = pulse_at(k, 10);
        p.event_ns += 37 * SECOND_NS;
        EXPECT_FALSE(feed(lock, p));
    }
    EXPECT_FALSE(lock.locked());
}

TEST(ptpclient_pulse, silence_unlocks_and_only_once)
{
    PulseLock lock;
    for (int64_t k = 1; k <= 3; ++k) {
        (void)feed(lock, pulse_at(k, 0));
    }
    int64_t const last = lock.last_pulse_ns();
    EXPECT_FALSE(lock.expire(last + (2 * SECOND_NS)));
    EXPECT_TRUE(lock.locked());
    EXPECT_TRUE(lock.expire(last + (3 * SECOND_NS)));
    EXPECT_FALSE(lock.locked());
    EXPECT_FALSE(lock.expire(last + (4 * SECOND_NS)));
}

// A silence forgets a good run that had not yet locked: three pulses in a row, not three in all.
TEST(ptpclient_pulse, a_silence_restarts_the_settle)
{
    PulseLock lock;
    (void)feed(lock, pulse_at(1, 0));
    (void)feed(lock, pulse_at(2, 0));
    EXPECT_FALSE(lock.expire(lock.last_pulse_ns() + (3 * SECOND_NS)));
    EXPECT_FALSE(feed(lock, pulse_at(6, 0)));
    EXPECT_FALSE(lock.locked());
}

// A reactor late by most of a second still finds a good pulse good.
TEST(ptpclient_pulse, a_late_read_still_finds_the_second)
{
    PulseLock lock;
    for (int64_t k = 1; k <= 3; ++k) {
        Pulse p = pulse_at(k, 0);
        p.reference_ns += 800'000'000;
        (void)feed(lock, p);
    }
    EXPECT_TRUE(lock.locked());
}

TEST(ptpclient_pulse, a_reader_needs_a_phc)
{
    auto const opened = ExttsReader::open("/nonexistent/ptp99", 0, [](int64_t, int64_t) {});
    EXPECT_FALSE(opened.has_value());
}

TEST_MAIN(statusbar_ptpclient, ptpclient_pulse_test)
