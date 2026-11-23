// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for MediaClockRateTracker (avb_entity_media_rate.hpp): the
// entity-level glue that turns (gPTP-master, GPS CLOCK_REALTIME) samples into
// r = switch/GPS plus the gPTP->GPS-TAI mapping, with the GPS-sample and
// telemetry rate-limits. (The Kalman filter and TAI translator it wraps have
// their own ptpclient tests; this covers the offset/dt/r and rate-limit glue.)

#include "statusbar/avb_entity/avb_entity_media_rate.hpp"

#include "statusbar/test/test.hpp"

#include <chrono>
#include <cstdint>

using statusbar::avb_entity::MediaClockRateTracker;
namespace ptpclient = statusbar::ptpclient;

namespace {
constexpr uint64_t QUARTER_S = 250'000'000ULL;
constexpr uint64_t FIVE_S = 5'000'000'000ULL;

using statusbar::avb_entity::SystemClockVerdict;
/// Disciplined and freshly rated: followed, and its TAI vouched for.
constexpr SystemClockVerdict USABLE{
    .follow = {.usable = true, .timeline_changed = false, .accuracy = std::chrono::microseconds{1500}}, .tai_usable = true};
/// Disciplined, but the system's figure has grown since the last update:
/// still followed for its rate, its TAI not vouched for.
constexpr SystemClockVerdict FOLLOWED_ONLY{
    .follow = {.usable = true, .timeline_changed = false, .accuracy = std::chrono::milliseconds{25}}, .tai_usable = false};
/// Never synchronized, or left without a server for most of a day.
constexpr SystemClockVerdict OUTSIDE{
    .follow = {.usable = false, .timeline_changed = false, .accuracy = std::chrono::seconds{3}}, .tai_usable = false};
constexpr SystemClockVerdict STEPPED{
    .follow = {.usable = true, .timeline_changed = true, .accuracy = std::chrono::microseconds{1500}}, .tai_usable = true};
}  // namespace

TEST(media_rate, defaults_to_unity_and_no_tai)
{
    MediaClockRateTracker t;
    EXPECT_TRUE(t.r() == 1.0);
    EXPECT_FALSE(t.has_tai_sample());
}

TEST(media_rate, should_sample_rate_limits_to_quarter_second)
{
    MediaClockRateTracker t;
    EXPECT_TRUE(t.should_sample(1000));  // nothing sampled yet
    (void)t.add_sample(1000, 900);
    EXPECT_FALSE(t.should_sample(1000 + (QUARTER_S - 1)));  // < 0.25 s since last
    EXPECT_TRUE(t.should_sample(1000 + QUARTER_S));         // >= 0.25 s
}

TEST(media_rate, should_log_rate_limits_to_five_seconds)
{
    MediaClockRateTracker t;
    EXPECT_TRUE(t.should_log(7000));  // never logged yet
    t.mark_logged(7000);
    EXPECT_FALSE(t.should_log(7000 + (FIVE_S - 1)));
    EXPECT_TRUE(t.should_log(7000 + FIVE_S));
}

TEST(media_rate, tai_available_after_a_sample)
{
    MediaClockRateTracker t;
    EXPECT_FALSE(t.has_tai_sample());
    (void)t.add_sample(1'000'000'000, 1'000'000'000);
    (void)t.add_sample(1'000'000'000 + QUARTER_S, 1'000'000'000 + QUARTER_S);
    EXPECT_TRUE(t.has_tai_sample());
}

TEST(media_rate, tracks_positive_switch_vs_gps_rate)
{
    MediaClockRateTracker t;
    // gPTP (switch) runs ~50 ppm FASTER than GPS: the gptp-gps offset grows
    // 12500 ns each 0.25 s sample (= 50 us/s = 50 ppm), so r should settle just
    // above 1.0. We assert direction + a sane band, not the exact Kalman value.
    ptpclient::RatioEstimate est{};
    for (uint64_t i = 0; i < 60; ++i) {
        uint64_t const gptp = i * QUARTER_S;
        uint64_t const gps = gptp - (i * 12'500ULL);  // offset = +12500 ns/sample
        est = t.add_sample(gptp, gps);
    }
    EXPECT_TRUE(est.valid);
    EXPECT_TRUE(t.r() > 1.0);
    EXPECT_TRUE(t.r() < 1.001);  // ~1.00005 expected
}

TEST(media_rate, configure_resets_running_state)
{
    MediaClockRateTracker t;
    for (uint64_t i = 0; i < 30; ++i) {
        (void)t.add_sample(i * QUARTER_S, (i * QUARTER_S) - (i * 12'500ULL));
    }
    EXPECT_TRUE(t.r() != 1.0);

    t.configure(ptpclient::KalmanRatioTracker::Config{.meas_noise_ns = 1000.0, .jerk_psd = 1e-3});
    EXPECT_TRUE(t.r() == 1.0);
    EXPECT_TRUE(t.should_sample(1));  // last-sample timestamp cleared
}

// The verdict on the system clock decides whether a pair is followed. The
// verdicts here are made up, as the readings are.

TEST(media_rate, a_pair_from_a_usable_system_clock_is_fed)
{
    MediaClockRateTracker t;
    EXPECT_FALSE(t.tai_usable());  // nothing judged yet
    auto const est = t.observe(1'000'000'000, 1'000'000'000, USABLE);
    EXPECT_TRUE(est.has_value());
    EXPECT_TRUE(t.tai_usable());
    EXPECT_TRUE(t.has_tai_sample());
    EXPECT_TRUE(t.tai_snapshot().tai_usable);
}

TEST(media_rate, outside_the_limit_the_ratio_holds_and_the_snapshot_says_so)
{
    MediaClockRateTracker t;
    for (uint64_t i = 0; i < 60; ++i) {
        (void)t.observe(i * QUARTER_S, (i * QUARTER_S) - (i * 12'500ULL), USABLE);
    }
    double const r_followed = t.r();
    EXPECT_TRUE(r_followed > 1.0);

    // The time server is lost for long enough that the clock leaves the limit.
    // The system clock now wanders the other way; none of it is followed.
    for (uint64_t i = 60; i < 120; ++i) {
        auto const est = t.observe(i * QUARTER_S, (i * QUARTER_S) + (i * 50'000ULL), OUTSIDE);
        EXPECT_FALSE(est.has_value());
    }
    EXPECT_TRUE(t.r() == r_followed);
    EXPECT_FALSE(t.tai_usable());
    EXPECT_FALSE(t.tai_snapshot().tai_usable);
    EXPECT_TRUE(t.has_tai_sample());  // the mapping it had is kept
}

TEST(media_rate, a_clock_good_enough_for_its_rate_is_followed_though_its_tai_is_not_vouched_for)
{
    // What an entity sees for up to a poll interval after it starts: the
    // system's figure has grown past the tight limit since the daemon's last
    // update, but the clock is disciplined and its rate is as good as ever.
    MediaClockRateTracker t;
    for (uint64_t i = 0; i < 60; ++i) {
        auto const est = t.observe(i * QUARTER_S, (i * QUARTER_S) - (i * 12'500ULL), FOLLOWED_ONLY);
        EXPECT_TRUE(est.has_value());
    }
    EXPECT_TRUE(t.r() > 1.0);
    EXPECT_TRUE(t.has_tai_sample());
    EXPECT_FALSE(t.tai_usable());
    EXPECT_FALSE(t.tai_snapshot().tai_usable);

    // The daemon updates: the same clock, now vouched for.
    (void)t.observe(60 * QUARTER_S, (60 * QUARTER_S) - (60 * 12'500ULL), USABLE);
    EXPECT_TRUE(t.tai_snapshot().tai_usable);
}

TEST(media_rate, a_pair_not_fed_still_holds_off_the_next_read)
{
    MediaClockRateTracker t;
    (void)t.observe(1000, 900, OUTSIDE);
    EXPECT_FALSE(t.should_sample(1000 + (QUARTER_S - 1)));
    EXPECT_TRUE(t.should_sample(1000 + QUARTER_S));
}

TEST(media_rate, a_media_clock_pinned_to_gptp_judges_the_system_clock_without_following_it)
{
    MediaClockRateTracker t;
    for (uint64_t i = 0; i < 60; ++i) {
        auto const est = t.observe(i * QUARTER_S, (i * QUARTER_S) - (i * 12'500ULL), USABLE, /*follow=*/false);
        EXPECT_FALSE(est.has_value());
    }
    EXPECT_TRUE(t.r() == 1.0);
    EXPECT_FALSE(t.has_tai_sample());
    EXPECT_TRUE(t.tai_usable());  // the tunnel's timestamps may still say so
}

TEST(media_rate, a_step_of_the_system_clock_restarts_the_filters_and_keeps_the_ratio)
{
    MediaClockRateTracker t;
    for (uint64_t i = 0; i < 60; ++i) {
        (void)t.observe(i * QUARTER_S, (i * QUARTER_S) - (i * 12'500ULL), USABLE);
    }
    double const r_before = t.r();
    EXPECT_TRUE(r_before > 1.0);

    // The system clock is stepped by 3 s: a new timeline. One pair on it is
    // not an estimate yet, so r stays where it was instead of returning to 1.
    constexpr uint64_t STEP_NS = 3'000'000'000ULL;
    uint64_t const gptp = 60 * QUARTER_S;
    auto const first = t.observe(gptp, gptp + STEP_NS, STEPPED);
    EXPECT_TRUE(first.has_value());
    EXPECT_FALSE(first->valid);
    EXPECT_TRUE(t.r() == r_before);

    // The mapping follows the new timeline at once: TAI is the stepped clock plus 37 s.
    EXPECT_EQ(t.tai_ns(static_cast<std::int64_t>(gptp)), static_cast<std::int64_t>(gptp + STEP_NS) + 37'000'000'000LL);
}

// Test runner

TEST_MAIN(statusbar_avb_entity, avb_entity_media_rate_test)
