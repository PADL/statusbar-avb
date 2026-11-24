// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// ClockGate: the session's clocks against its configured limits. Every time
// is made up: a link is "synchronized" at an instant the test names and the
// gate is asked about later instants, so nothing here waits.

#include "statusbar/udptun/udptun_clock_gate.hpp"

#include "statusbar/test/test.hpp"

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>

using namespace statusbar;
using namespace statusbar::udptun;
using namespace std::chrono_literals;
using realtime::ClockQuality;
using realtime::MonotonicRawClock;
using realtime::Nanoseconds;

namespace {

constexpr auto raw(Nanoseconds const since_start) -> MonotonicRawClock::time_point
{
    return MonotonicRawClock::time_point{since_start};
}

/// A master link last synchronized at @p synced to 2 us, wandering at the
/// default holdover figure of 15 ppm afterwards.
constexpr auto link_synced_at(Nanoseconds const synced, uint64_t const epoch = 0) -> ClockQuality
{
    return {
        .synced_at = raw(synced),
        .bound_at_sync = 2us,
        .epoch = epoch,
        .drift_bound_ppb = realtime::DEFAULT_HOLDOVER_DRIFT_PPB,
        .time_source = realtime::TimeSource::ptp};
}

/// A system clock rated at @p bound by its time daemon at @p synced.
constexpr auto utc_rated(Nanoseconds const bound, Nanoseconds const synced) -> ClockQuality
{
    return {
        .synced_at = raw(synced),
        .bound_at_sync = bound,
        .epoch = 0,
        .drift_bound_ppb = realtime::DEFAULT_HOLDOVER_DRIFT_PPB,
        .time_source = realtime::TimeSource::gnss};
}

auto line_of(ClockReport const& report) -> std::string
{
    std::ostringstream out;
    print_clock_report(out, report);
    return out.str();
}

}  // namespace

TEST(udptun_clock_gate, three_missed_syncs_do_not_stop_a_session)
{
    ClockGate gate{ClockGate::Config{.has_link = true}};
    EXPECT_FALSE(gate.usable());  // nothing judged yet
    EXPECT_TRUE(gate.judge(link_synced_at(10s), ClockQuality{}, raw(10s)));
    // Syncs come eight times a second; three in a row lost is 375 ms without one.
    EXPECT_TRUE(gate.judge(link_synced_at(10s), ClockQuality{}, raw(10s + 375ms)));
    auto const report = gate.report();
    EXPECT_EQ(report.link_accuracy_ns, 2'000 + 5'625);  // 2 us + 15 ppm of 375 ms
    EXPECT_EQ(report.link_age_ns, 375'000'000);
    EXPECT_EQ(report.link_excursions, 0U);
}

TEST(udptun_clock_gate, a_link_that_coasts_past_the_limit_stops_the_session_and_sync_resumes_it)
{
    ClockGate gate{ClockGate::Config{.has_link = true}};
    EXPECT_TRUE(gate.judge(link_synced_at(0s), ClockQuality{}, raw(0s)));
    EXPECT_TRUE(gate.judge(link_synced_at(0s), ClockQuality{}, raw(6s)));   // 92 us
    EXPECT_FALSE(gate.judge(link_synced_at(0s), ClockQuality{}, raw(7s)));  // 107 us
    EXPECT_FALSE(gate.judge(link_synced_at(0s), ClockQuality{}, raw(9s)));
    EXPECT_TRUE(gate.judge(link_synced_at(10s), ClockQuality{}, raw(10s)));
    auto const report = gate.report();
    EXPECT_EQ(report.link_excursions, 1U);
    EXPECT_EQ(report.link_outside_ns, 3'000'000'000);
    EXPECT_EQ(report.link_limit_ns, DEFAULT_MAX_CLOCK_ERROR_NS);
}

TEST(udptun_clock_gate, a_link_never_synchronized_is_not_usable)
{
    ClockGate gate{ClockGate::Config{.has_link = true}};
    EXPECT_FALSE(gate.judge(ClockQuality{}, ClockQuality{}, raw(30s)));
    EXPECT_EQ(gate.report().link_accuracy_ns, ClockReport::UNBOUNDED);
}

TEST(udptun_clock_gate, a_new_grandmaster_is_a_change_of_timeline)
{
    ClockGate gate{ClockGate::Config{.has_link = true}};
    (void)gate.judge(link_synced_at(0s, 1), ClockQuality{}, raw(0s));
    EXPECT_FALSE(gate.link_timeline_changed());
    (void)gate.judge(link_synced_at(1s, 2), ClockQuality{}, raw(1s));
    EXPECT_TRUE(gate.link_timeline_changed());
    (void)gate.judge(link_synced_at(2s, 2), ClockQuality{}, raw(2s));
    EXPECT_FALSE(gate.link_timeline_changed());
    EXPECT_EQ(gate.report().link_timeline_changes, 1U);
}

TEST(udptun_clock_gate, without_a_master_link_there_is_nothing_to_wait_for)
{
    ClockGate gate{ClockGate::Config{.has_link = false, .tracks_utc = true}};
    // The system clock has never reported itself synchronized, and no limit
    // on it is configured: the bound is shown, the session runs.
    EXPECT_TRUE(gate.judge(ClockQuality{}, ClockQuality{}, raw(1s)));
    EXPECT_EQ(gate.report().utc_accuracy_ns, ClockReport::UNBOUNDED);
    EXPECT_FALSE(gate.report().has_link);
}

TEST(udptun_clock_gate, the_system_clock_is_enforced_only_when_a_limit_is_configured)
{
    ClockGate reporting{ClockGate::Config{.has_link = true, .tracks_utc = true}};
    ClockGate enforcing{ClockGate::Config{.max_utc_error_ns = 5'000'000, .has_link = true, .tracks_utc = true}};
    auto const utc = utc_rated(8ms, 0s);
    EXPECT_TRUE(reporting.judge(link_synced_at(0s), utc, raw(0s)));
    EXPECT_FALSE(enforcing.judge(link_synced_at(0s), utc, raw(0s)));
    EXPECT_EQ(reporting.report().utc_accuracy_ns, 8'000'000);
    EXPECT_EQ(reporting.report().utc_limit_ns, 0);
    EXPECT_EQ(enforcing.report().utc_limit_ns, 5'000'000);

    EXPECT_TRUE(enforcing.judge(link_synced_at(1s), utc_rated(1500us, 1s), raw(1s)));
}

TEST(udptun_clock_gate, the_link_is_fit_to_learn_from_whatever_the_system_clock_does)
{
    // What is learned from the master link (the GPS-TAI mapping) depends on
    // the link alone being inside its limit.
    ClockGate gate{ClockGate::Config{.max_utc_error_ns = 5'000'000, .has_link = true, .tracks_utc = true}};
    EXPECT_FALSE(gate.link_usable());
    EXPECT_FALSE(gate.judge(link_synced_at(0s), utc_rated(8ms, 0s), raw(0s)));
    EXPECT_TRUE(gate.link_usable());
    // Coasted past the limit, it is not; back in sync, it is again.
    (void)gate.judge(link_synced_at(0s), utc_rated(8ms, 0s), raw(10s));
    EXPECT_FALSE(gate.link_usable());
    (void)gate.judge(link_synced_at(11s), utc_rated(8ms, 0s), raw(11s));
    EXPECT_TRUE(gate.link_usable());

    // Without a link there is nothing that could be unfit.
    ClockGate linkless{ClockGate::Config{.has_link = false, .tracks_utc = true}};
    (void)linkless.judge(ClockQuality{}, ClockQuality{}, raw(1s));
    EXPECT_TRUE(linkless.link_usable());
}

TEST(udptun_clock_gate, a_negative_link_limit_takes_any_bounded_link)
{
    ClockGate gate{ClockGate::Config{.max_link_error_ns = -1, .has_link = true}};
    EXPECT_FALSE(gate.judge(ClockQuality{}, ClockQuality{}, raw(0s)));
    EXPECT_TRUE(gate.judge(link_synced_at(0s), ClockQuality{}, raw(0s) + std::chrono::hours{24}));
    EXPECT_EQ(gate.report().link_limit_ns, ClockReport::UNBOUNDED);
}

TEST(udptun_clock_gate, the_report_line_names_each_clock_and_rounds_a_bound_up)
{
    ClockGate gate{ClockGate::Config{.has_link = true, .tracks_utc = true}};
    (void)gate.judge(link_synced_at(0s), utc_rated(1470us, 0s), raw(375ms));
    EXPECT_TRUE(
        line_of(gate.report()) ==
        "  clock: link +/-7.7 us (limit +/-100.0 us, synced 0.3 s ago) excursions=0 outside=0.0 s timeline_changes=0"
        " utc +/-1.5 ms\n");

    (void)gate.judge(link_synced_at(0s), utc_rated(1470us, 0s), raw(20s));
    auto const stopped = line_of(gate.report());
    EXPECT_TRUE(stopped.find("link +/-302.0 us") != std::string::npos);
    EXPECT_TRUE(stopped.find("excursions=1") != std::string::npos);
    EXPECT_TRUE(stopped.ends_with("NOT USABLE: nothing measured or sent\n"));
}

TEST(udptun_clock_gate, the_report_line_says_when_the_tai_filter_had_to_correct_itself)
{
    ClockGate gate{ClockGate::Config{.has_link = true, .tracks_utc = true}};
    (void)gate.judge(link_synced_at(0s), utc_rated(1500us, 0s), raw(0s));
    auto report = gate.report();
    EXPECT_TRUE(line_of(report).find("self-corrections") == std::string::npos);
    report.tai_self_corrections = 2;
    EXPECT_TRUE(line_of(report).find("tai filter self-corrections 2") != std::string::npos);
}

TEST(udptun_clock_gate, a_session_that_judges_no_clock_prints_no_line)
{
    EXPECT_TRUE(line_of(ClockGate{}.report()).empty());
    EXPECT_TRUE(line_of(ClockReport{.has_link = true}).find("link unbounded") != std::string::npos);
}

TEST_MAIN(statusbar_udptun, udptun_clock_gate_test)
