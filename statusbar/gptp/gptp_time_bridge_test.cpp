// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/gptp/gptp_time_bridge.hpp"

#include "statusbar/gptp/gptp_time_bridge_test_support.hpp"
#include "statusbar/test/test.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>

using namespace statusbar;
using statusbar::gptp::GptpTimeBridge;
using namespace statusbar::gptp::test_support;

namespace {

constexpr int64_t SYNC_INTERVAL_NS = 125'000'000;  // 8 Syncs a second
constexpr int64_t MASTER_EPOCH_NS = 1'700'000'000'000'000'000;
constexpr int64_t APP_EPOCH_NS = 50'000'000'000;

/// The app clock of a machine whose crystal runs @p app_ppm fast of master: the
/// app instant at which master reads @p master_ns.
auto app_at(int64_t master_ns, double app_ppm) -> int64_t
{
    double const elapsed = static_cast<double>(master_ns - MASTER_EPOCH_NS);
    return APP_EPOCH_NS + static_cast<int64_t>(elapsed * (1.0 + app_ppm * 1e-6));
}

/// Feed @p count Sync pairings at the gPTP rate to @p bridge; the app stamp is
/// the true arrival plus @p jitter_ns(i), the user-space read's lateness. Returns
/// the master time of the last anchor.
template <typename Jitter>
auto run_syncs(GptpTimeBridge& bridge, int count, double app_ppm, Jitter jitter_ns) -> int64_t
{
    int64_t master = MASTER_EPOCH_NS;
    for (int i = 0; i < count; ++i) {
        master += SYNC_INTERVAL_NS;
        anchor(bridge, app_at(master, app_ppm) + jitter_ns(i), master);
    }
    return master;
}

auto abs_ns(int64_t v) -> int64_t
{
    return v < 0 ? -v : v;
}

}  // namespace

// The rated conversion between anchors extrapolates with the master rate against
// the bridge's own app clock. An app clock 30 ppm fast of master used to be
// converted with the servo's master/PHC ratio of 1.0: at the next Sync that is a
// 3.75 us sawtooth every anchor. With the rate fitted over the anchors the
// conversion one Sync interval past the last anchor is within a few ns.
TEST(gptp_time_bridge, rated_conversion_follows_the_app_clock_rate_not_the_servo_ratio)
{
    GptpTimeBridge bridge{};
    constexpr double app_ppm = 30.0;
    int64_t const last_master = run_syncs(bridge, 20 * 8, app_ppm, [](int) { return int64_t{0}; });

    int64_t const next_master = last_master + SYNC_INTERVAL_NS;
    int64_t const true_app = app_at(next_master, app_ppm);
    int64_t const error_from = from_gptp_rated_ns(bridge, next_master) - true_app;
    int64_t const error_to = to_gptp_rated_ns(bridge, true_app) - next_master;
    EXPECT_TRUE(abs_ns(error_from) <= 10);
    EXPECT_TRUE(abs_ns(error_to) <= 10);
    // The unrated conversion is the one that still sawtooths, by ppm x interval.
    int64_t const unrated = from_gptp_ns(bridge, next_master) - true_app;
    EXPECT_TRUE(abs_ns(unrated) > 3'000);
    // 30 ppm is 30'000'000 ppt.
    EXPECT_TRUE(abs_ns(bridge.rate_offset_ppt() - (-30'000'000)) < 100'000);
}

// The app stamp is a user-space read, late by a scheduling delay that varies by
// tens of us. Fitted over one Sync interval that would be hundreds of ppm of
// rate noise; over the anchor window it stays within a few ppm.
TEST(gptp_time_bridge, app_stamp_jitter_does_not_reach_the_rate)
{
    GptpTimeBridge bridge{};
    constexpr double app_ppm = 30.0;
    // Deterministic lateness in [0, 40) us.
    auto jitter = [](int i) { return static_cast<int64_t>((static_cast<uint64_t>(i) * 2654435761ULL) % 40'000); };
    (void)run_syncs(bridge, 20 * 8, app_ppm, jitter);

    int64_t const ppm_error = abs_ns(bridge.rate_offset_ppt() - (-30'000'000)) / 1'000'000;
    EXPECT_TRUE(ppm_error <= 3);
}

// Fewer anchors than the shortest baseline: the rate stands at its last value
// (none yet, so 1.0) rather than being fitted over a noisy short interval; the
// offset still follows the latest anchor.
TEST(gptp_time_bridge, the_rate_waits_for_a_baseline)
{
    GptpTimeBridge bridge{};
    int64_t const last_master = run_syncs(bridge, 4, 30.0, [](int) { return int64_t{0}; });
    EXPECT_EQ(bridge.rate_offset_ppt(), 0);
    EXPECT_EQ(bridge.last_anchor_gptp_ns(), last_master);
    EXPECT_EQ(from_gptp_ns(bridge, last_master), app_at(last_master, 30.0));
}

// A grandmaster change steps master time. The window restarts at that anchor
// and the fitted rate is held, not blown up by the step; the offset follows the
// new master at once.
TEST(gptp_time_bridge, a_master_step_restarts_the_window_and_holds_the_rate)
{
    GptpTimeBridge bridge{};
    constexpr double app_ppm = 30.0;
    int64_t const last_master = run_syncs(bridge, 20 * 8, app_ppm, [](int) { return int64_t{0}; });
    int64_t const rate_before = bridge.rate_offset_ppt();

    constexpr int64_t step_ns = 2'000'000'000;  // the new grandmaster is 2 s ahead
    int64_t const stepped_master = last_master + SYNC_INTERVAL_NS + step_ns;
    int64_t const app_now = app_at(last_master + SYNC_INTERVAL_NS, app_ppm);
    anchor(bridge, app_now, stepped_master);

    EXPECT_EQ(bridge.rate_offset_ppt(), rate_before);
    EXPECT_EQ(from_gptp_ns(bridge, stepped_master), app_now);
}

//
// Quality: how well the line is known, and coasting when Syncs stop
//

namespace {

using statusbar::realtime::MonotonicRawClock;
using statusbar::realtime::Nanoseconds;

/// Deterministic user-space lateness of the app stamp, in [0, 40) us.
auto stamp_jitter(int i) -> int64_t
{
    return static_cast<int64_t>((static_cast<uint64_t>(i) * 2654435761ULL) % 40'000);
}

}  // namespace

TEST(gptp_time_bridge_quality, no_bound_is_claimed_before_the_rate_is_fitted)
{
    GptpTimeBridge bridge{};
    EXPECT_FALSE(bridge.quality().is_bounded());
    // Half a second of Syncs: an offset, but no baseline for a rate yet.
    (void)run_syncs(bridge, 4, 30.0, [](int) { return int64_t{0}; });
    EXPECT_FALSE(bridge.quality().is_bounded());
    EXPECT_TRUE(bridge.quality().time_source == realtime::TimeSource::ptp);
}

TEST(gptp_time_bridge_quality, the_bound_is_the_furthest_an_anchor_landed_from_the_line)
{
    GptpTimeBridge bridge{};
    constexpr double app_ppm = 30.0;
    int64_t const last_master = run_syncs(bridge, 20 * 8, app_ppm, stamp_jitter);

    auto const quality = bridge.quality();
    EXPECT_TRUE(quality.is_bounded());
    // Anchored on the app (RAW) instant of the last Sync.
    EXPECT_EQ(MonotonicRawClock::to_ns(quality.synced_at), bridge.last_anchor_app_ns());
    // Stamps up to 40 us late: the line misses an anchor by no more than
    // that, and by a good part of it.
    EXPECT_TRUE(quality.bound_at_sync > Nanoseconds{10'000});
    EXPECT_TRUE(quality.bound_at_sync <= Nanoseconds{45'000});
    EXPECT_EQ(quality.epoch, uint64_t{0});
    // And it does bound the truth: the line one Sync on is within it.
    int64_t const next_master = last_master + SYNC_INTERVAL_NS;
    int64_t const error = to_gptp_rated_ns(bridge, app_at(next_master, app_ppm)) - next_master;
    EXPECT_TRUE(abs_ns(error) <= quality.uncertainty(MonotonicRawClock::from_ns(app_at(next_master, app_ppm))).count());
}

TEST(gptp_time_bridge_quality, when_syncs_stop_the_line_coasts_and_the_uncertainty_grows)
{
    GptpTimeBridge bridge{};
    constexpr double app_ppm = 30.0;
    int64_t const last_master = run_syncs(bridge, 20 * 8, app_ppm, [](int) { return int64_t{0}; });
    auto const at_loss = bridge.quality();
    EXPECT_TRUE(at_loss.is_bounded());
    EXPECT_EQ(at_loss.drift_bound_ppb, realtime::DEFAULT_HOLDOVER_DRIFT_PPB);

    // The master is gone. Nothing is told to the bridge; it is only asked.
    // Three missed Syncs later, and thirty seconds later:
    auto const raw_after = [&](int64_t master_elapsed_ns) {
        return MonotonicRawClock::from_ns(app_at(last_master + master_elapsed_ns, app_ppm));
    };
    int64_t const after_three = at_loss.uncertainty(raw_after(3 * SYNC_INTERVAL_NS)).count();
    int64_t const after_thirty_s = at_loss.uncertainty(raw_after(30'000'000'000)).count();
    EXPECT_TRUE(bridge.quality() == at_loss);
    // 15 ppm for 375 ms is 5.6 us; for 30 s, 450 us.
    EXPECT_TRUE(after_three >= 5'600 && after_three <= 5'700 + at_loss.bound_at_sync.count());
    EXPECT_TRUE(after_thirty_s >= 450'000 && after_thirty_s <= 451'000 + at_loss.bound_at_sync.count());

    // The line itself still converts, with the rate and offset it had: a
    // crystal that really held its 30 ppm is converted to within a few ns
    // thirty seconds on, far inside the bound.
    int64_t const master_later = last_master + 30'000'000'000;
    EXPECT_TRUE(abs_ns(to_gptp_rated_ns(bridge, app_at(master_later, app_ppm)) - master_later) <= 50);

    // An application that tolerates 100 us rides out the three Syncs and
    // six seconds more; one that tolerates a millisecond, the half minute.
    EXPECT_TRUE(at_loss.within(std::chrono::microseconds{100}, raw_after(6'000'000'000)));
    EXPECT_FALSE(at_loss.within(std::chrono::microseconds{100}, raw_after(30'000'000'000)));
    EXPECT_TRUE(at_loss.within(std::chrono::milliseconds{1}, raw_after(30'000'000'000)));
}

TEST(gptp_time_bridge_quality, the_deployment_sets_the_drift_it_coasts_at)
{
    GptpTimeBridge bridge{};
    bridge.holdover_drift_ppb = 1'000;  // a good oscillator in a steady room
    (void)run_syncs(bridge, 20 * 8, 30.0, [](int) { return int64_t{0}; });
    EXPECT_EQ(bridge.quality().drift_bound_ppb, uint32_t{1'000});
}

TEST(gptp_time_bridge_quality, a_rate_seen_moving_raises_the_drift_bound)
{
    GptpTimeBridge bridge{};
    bridge.holdover_drift_ppb = 1'000;
    // Ten seconds at 30 ppm, then the crystal warms and runs at 34 ppm.
    int64_t master = MASTER_EPOCH_NS;
    int64_t app = APP_EPOCH_NS;
    for (int i = 0; i < 20 * 8; ++i) {
        master += SYNC_INTERVAL_NS;
        app += SYNC_INTERVAL_NS + (SYNC_INTERVAL_NS * (i < 10 * 8 ? 30 : 34) / 1'000'000);
        anchor(bridge, app, master);
    }
    // The fitted rate moved by a few ppm inside the window: the held rate
    // may be that far out, which is more than the configured 1 ppm.
    EXPECT_TRUE(bridge.quality().drift_bound_ppb > uint32_t{1'000});
    EXPECT_TRUE(bridge.quality().drift_bound_ppb < uint32_t{6'000});
}

TEST(gptp_time_bridge_quality, a_master_step_is_a_new_timeline)
{
    GptpTimeBridge bridge{};
    constexpr double app_ppm = 30.0;
    int64_t const last_master = run_syncs(bridge, 20 * 8, app_ppm, [](int) { return int64_t{0}; });
    EXPECT_TRUE(bridge.quality().is_bounded());

    constexpr int64_t step_ns = 2'000'000'000;
    int64_t master = last_master + SYNC_INTERVAL_NS;
    anchor(bridge, app_at(master, app_ppm), master + step_ns);
    // The epoch changed, and one anchor of the new timeline proves nothing yet.
    EXPECT_EQ(bridge.quality().epoch, uint64_t{1});
    EXPECT_FALSE(bridge.quality().is_bounded());

    // The next Sync of the new timeline agrees with the line: bounded again.
    master += SYNC_INTERVAL_NS;
    anchor(bridge, app_at(master, app_ppm), master + step_ns);
    EXPECT_TRUE(bridge.quality().is_bounded());
    EXPECT_EQ(bridge.quality().epoch, uint64_t{1});
    EXPECT_TRUE(bridge.quality().bound_at_sync <= Nanoseconds{50});
}

TEST(gptp_time_bridge_quality, the_owner_can_declare_a_new_timeline)
{
    GptpTimeBridge bridge{};
    int64_t master = run_syncs(bridge, 20 * 8, 30.0, [](int) { return int64_t{0}; });
    int64_t const rate_before = bridge.rate_offset_ppt();

    bridge.time_source = realtime::TimeSource::gnss;  // the new grandmaster is a GPS receiver
    bridge.restart_timeline();
    EXPECT_EQ(bridge.quality().epoch, uint64_t{1});
    EXPECT_FALSE(bridge.quality().is_bounded());
    EXPECT_TRUE(bridge.quality().time_source == realtime::TimeSource::gnss);
    EXPECT_EQ(bridge.rate_offset_ppt(), rate_before);

    for (int i = 0; i < 2; ++i) {
        master += SYNC_INTERVAL_NS;
        anchor(bridge, app_at(master, 30.0), master);
    }
    EXPECT_TRUE(bridge.quality().is_bounded());
    EXPECT_TRUE(bridge.quality().time_source == realtime::TimeSource::gnss);
}

TEST_MAIN(statusbar_gptp, gptp_time_bridge_test)
