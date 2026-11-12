// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/gptp/gptp_time_bridge.hpp"

#include "statusbar/test/test.hpp"

#include <cstdint>
#include <cstdlib>

using namespace statusbar;
using statusbar::gptp::GptpTimeBridge;

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
        bridge.update_from_anchor(app_at(master, app_ppm) + jitter_ns(i), master);
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
    int64_t const error_from = bridge.from_gptp_rated(next_master) - true_app;
    int64_t const error_to = bridge.to_gptp_rated(true_app) - next_master;
    EXPECT_TRUE(abs_ns(error_from) <= 10);
    EXPECT_TRUE(abs_ns(error_to) <= 10);
    // The unrated conversion is the one that still sawtooths, by ppm x interval.
    int64_t const unrated = bridge.from_gptp(next_master) - true_app;
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
    EXPECT_EQ(bridge.from_gptp(last_master), app_at(last_master, 30.0));
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
    bridge.update_from_anchor(app_now, stepped_master);

    EXPECT_EQ(bridge.rate_offset_ppt(), rate_before);
    EXPECT_EQ(bridge.from_gptp(stepped_master), app_now);
}

TEST_MAIN(statusbar_gptp, gptp_time_bridge_test)
