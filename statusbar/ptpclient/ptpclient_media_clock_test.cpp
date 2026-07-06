// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for MediaClockGenerator: deterministic, GPS-rate-pinned, jitter-free
// AVTP presentation timestamps from a free-running sample counter.

#include "statusbar/ptpclient/ptpclient_media_clock.hpp"

#include "statusbar/test/test.hpp"

#include <cmath>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

using namespace statusbar::ptpclient;

namespace {
constexpr double k_nominal_96k = 1e9 / 96000.0;                 // 10416.6667 ns/sample
constexpr std::uint64_t k_base = 1'780'000'000'000'000'000ULL;  // GPS-like gPTP epoch
constexpr std::uint64_t k_wake_ns = 125'000;                    // 12 samples @ 96 kHz (gPTP)
}  // namespace

// The defining property: a JITTERY wake input must yield SMOOTH, near-linear
// presentation timestamps -- that is the whole point (an audio interface recovers its
// media clock from these and warbles if they jitter).
TEST(media_clock_generator, deterministic_timestamps_reject_wake_jitter)
{
    constexpr double r = 1.000045;  // +45 ppm GPS ratio
    MediaClockGenerator mcg{MediaClockGenerator::Config{.sample_rate_hz = 96000.0, .presentation_offset_ns = 1'000'000}};

    std::mt19937 rng{777};
    std::normal_distribution<double> jitter{0.0, 60.0};  // 60 ns RMS wake jitter

    std::vector<std::pair<double, double>> pts;  // (sample_index, ts - base)
    for (int k = 0; k < 4000; ++k) {
        auto const gptp = k_base + static_cast<std::uint64_t>(std::llround((static_cast<double>(k) * k_wake_ns) + jitter(rng)));
        auto const e = mcg.advance(gptp, r, 12);
        if (k >= 500 && e.samples > 0) {  // post-warmup
            auto const ts = mcg.timestamp_for(e.first_index);
            pts.emplace_back(static_cast<double>(e.first_index), static_cast<double>(ts - k_base));
        }
    }

    // Least-squares line ts = a + b*index; the residual RMS is the OUTPUT jitter.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    auto const n = static_cast<double>(pts.size());
    for (auto const& [x, y] : pts) {
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    double const b = ((n * sxy) - (sx * sy)) / ((n * sxx) - (sx * sx));
    double const a = (sy - (b * sx)) / n;
    double ss = 0;
    for (auto const& [x, y] : pts) {
        double const resid = y - (a + (b * x));
        ss += resid * resid;
    }
    double const resid_rms = std::sqrt(ss / n);

    EXPECT_TRUE(resid_rms < 15.0);                           // 60 ns in -> < 15 ns out
    EXPECT_TRUE(std::fabs(b - (k_nominal_96k * r)) < 0.01);  // slope == nominal * r
}

// The rate must be pinned to the GPS ratio, not the local PHC rate.
TEST(media_clock_generator, rate_pinned_to_gps_ratio)
{
    constexpr double r = 1.000100;  // +100 ppm
    MediaClockGenerator mcg{};
    for (int k = 0; k < 50; ++k) {
        mcg.advance(k_base + (static_cast<std::uint64_t>(k) * k_wake_ns), r, 12);
    }
    EXPECT_TRUE(std::fabs(mcg.ns_per_sample() - (k_nominal_96k * r)) < 1e-9);
    EXPECT_TRUE(std::fabs(mcg.ratio() - r) < 1e-12);
}

// Production must be paced to the GPS rate, and the newest sample's presentation
// stays ~one presentation-offset (1 ms) ahead of the wake -- so the offset does
// not slowly drain even though the timer ticks at the PHC rate.
TEST(media_clock_generator, gps_rate_pacing_and_offset)
{
    constexpr double r = 1.000045;
    constexpr double slope = k_nominal_96k * r;
    MediaClockGenerator mcg{MediaClockGenerator::Config{.sample_rate_hz = 96000.0, .presentation_offset_ns = 1'000'000}};

    constexpr int ticks = 8000;  // ~1 s of gPTP
    std::uint64_t total = 0;
    for (int k = 0; k < ticks; ++k) {
        auto const e = mcg.advance(k_base + (static_cast<std::uint64_t>(k) * k_wake_ns), r, 12);
        total += e.samples;
    }
    // Total tracks GPS samples up to the LAST wake: (ticks-1)*wake / slope.
    double const expected = (static_cast<double>(ticks - 1) * k_wake_ns) / slope;
    EXPECT_TRUE(std::fabs(static_cast<double>(total) - expected) < 5.0);

    // Newest emitted sample presents ~1 ms (one offset) ahead of the last wake.
    auto const gptp_last = k_base + (static_cast<std::uint64_t>(ticks - 1) * k_wake_ns);
    auto const ts_latest = mcg.timestamp_for(mcg.samples_emitted() - 1);
    auto const ahead = static_cast<std::int64_t>(ts_latest - gptp_last);
    bool const offset_ok = ahead > 950'000 && ahead < 1'050'000;  // ~1 ms +/- a packet
    EXPECT_TRUE(offset_ok);
}

// At a fixed anchor, timestamp_for() is exactly linear in the sample index
// (the per-sample step is the GPS-pinned slope, +/- 1 ns rounding).
TEST(media_clock_generator, timestamp_is_linear_in_index)
{
    constexpr double r = 1.000045;
    MediaClockGenerator mcg{};
    for (int k = 0; k < 200; ++k) {
        mcg.advance(k_base + (static_cast<std::uint64_t>(k) * k_wake_ns), r, 12);
    }
    auto const i0 = mcg.samples_emitted();
    auto const t0 = mcg.timestamp_for(i0);
    auto const t1 = mcg.timestamp_for(i0 + 1);
    auto const t1000 = mcg.timestamp_for(i0 + 1000);
    EXPECT_TRUE(std::llabs(static_cast<std::int64_t>(t1 - t0) - std::llround(k_nominal_96k * r)) <= 1);
    EXPECT_TRUE(std::llabs(static_cast<std::int64_t>(t1000 - t0) - std::llround(1000.0 * k_nominal_96k * r)) <= 1);
}

// The presentation phase is carried as integer ns + a [0,1) float fractional, so
// it must stay accurate over a LONG run (the float never holds the growing
// magnitude). After ~25 s of ticks the newest sample is still ~one offset ahead
// of the wake and the per-packet step is still exactly the slope.
TEST(media_clock_generator, long_run_float_accumulator_stays_accurate)
{
    constexpr double r = 1.000037;
    constexpr double slope = k_nominal_96k * r;
    MediaClockGenerator mcg{MediaClockGenerator::Config{.sample_rate_hz = 96000.0, .presentation_offset_ns = 1'000'000}};

    constexpr int ticks = 200'000;  // ~25 s of gPTP wakes
    for (int k = 0; k < ticks; ++k) {
        mcg.advance(k_base + (static_cast<std::uint64_t>(k) * k_wake_ns), r, 12);
    }

    // Pacing still holds the newest sample ~1 ms ahead of the last wake (no slow
    // float drift draining the offset).
    auto const gptp_last = k_base + (static_cast<std::uint64_t>(ticks - 1) * k_wake_ns);
    auto const ts_latest = mcg.timestamp_for(mcg.samples_emitted() - 1);
    auto const ahead = static_cast<std::int64_t>(ts_latest - gptp_last);
    EXPECT_TRUE(ahead > 950'000 && ahead < 1'050'000);

    // One packet's step is still exactly the slope after the long run.
    auto const i0 = mcg.samples_emitted();
    auto const step = static_cast<std::int64_t>(mcg.timestamp_for(i0 + 12) - mcg.timestamp_for(i0));
    EXPECT_TRUE(std::llabs(step - std::llround(12.0 * slope)) <= 1);
}

//
// Main test runner
//

// A gPTP timeline step (GM reboot / epoch reset) must re-anchor, not slow-catch-up.
// Forward jump: without re-anchoring the phase crawls at +1 sample/tick and emits
// dead-epoch timestamps for seconds; here the very next packet is on the new epoch.
TEST(media_clock_generator, reanchors_on_forward_gptp_step)
{
    constexpr std::uint64_t offset = 1'000'000;
    MediaClockGenerator mcg{MediaClockGenerator::Config{.sample_rate_hz = 96000.0, .presentation_offset_ns = offset}};
    for (int k = 0; k < 100; ++k) {
        mcg.advance(k_base + (static_cast<std::uint64_t>(k) * k_wake_ns), 1.0, 12);
    }
    EXPECT_EQ(mcg.step_count(), std::uint64_t{0});  // normal ticks never trip the step detector

    // GM reboot bumped the epoch by 10 s beyond the expected next wake.
    std::uint64_t const jumped = k_base + (100ULL * k_wake_ns) + 10'000'000'000ULL;
    auto const e = mcg.advance(jumped, 1.0, 12);
    EXPECT_EQ(mcg.step_count(), std::uint64_t{1});
    EXPECT_EQ(e.samples, std::uint32_t{12});                       // emits nominal, not a clamped catch-up
    EXPECT_EQ(mcg.timestamp_for(e.first_index), jumped + offset);  // on the NEW epoch
}

// Backward jump: the buggy path underflowed and clamped want to 0 -> stream stalled.
TEST(media_clock_generator, reanchors_on_backward_gptp_step)
{
    constexpr std::uint64_t offset = 1'000'000;
    MediaClockGenerator mcg{MediaClockGenerator::Config{.sample_rate_hz = 96000.0, .presentation_offset_ns = offset}};
    for (int k = 0; k < 100; ++k) {
        mcg.advance(k_base + (static_cast<std::uint64_t>(k) * k_wake_ns), 1.0, 12);
    }
    // The GM timeline reset to a much smaller epoch (5 s earlier).
    std::uint64_t const n_before = mcg.samples_emitted();
    std::uint64_t const reset = k_base - 5'000'000'000ULL;
    auto const e = mcg.advance(reset, 1.0, 12);
    EXPECT_EQ(mcg.step_count(), std::uint64_t{1});
    EXPECT_EQ(e.samples, std::uint32_t{12});  // keeps emitting -- the bug stalled at 0
    EXPECT_EQ(mcg.timestamp_for(e.first_index), reset + offset);
    // first_index stays monotonic across the re-anchor (n_ is not reset to 0).
    EXPECT_EQ(e.first_index, n_before);
}

TEST_MAIN(statusbar_ptpclient, ptpclient_media_clock_test)
