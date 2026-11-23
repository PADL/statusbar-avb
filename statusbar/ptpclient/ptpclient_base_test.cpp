// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for ptpclient_base: PtpErrorCategory::message and fit_time_mapping

#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <system_error>

using namespace statusbar::ptpclient;

//
// Tests: PtpErrorCategory::message
//

TEST(ptp_error_category, device_not_found)
{
    auto ec = make_error_code(PtpError::device_not_found);
    EXPECT_EQ(ec.message(), std::string("PTP device not found"));
}

TEST(ptp_error_category, device_open_failed)
{
    auto ec = make_error_code(PtpError::device_open_failed);
    EXPECT_EQ(ec.message(), std::string("Failed to open PTP device"));
}

TEST(ptp_error_category, device_not_open)
{
    auto ec = make_error_code(PtpError::device_not_open);
    EXPECT_EQ(ec.message(), std::string("PTP device not open"));
}

TEST(ptp_error_category, permission_denied)
{
    auto ec = make_error_code(PtpError::permission_denied);
    EXPECT_EQ(ec.message(), std::string("Permission denied accessing PTP device"));
}

TEST(ptp_error_category, bridge_not_running)
{
    auto ec = make_error_code(PtpError::bridge_not_running);
    EXPECT_EQ(ec.message(), std::string("PTP time bridge not running"));
}

TEST(ptp_error_category, bridge_not_healthy)
{
    auto ec = make_error_code(PtpError::bridge_not_healthy);
    EXPECT_EQ(ec.message(), std::string("PTP time bridge mapping not healthy"));
}

TEST(ptp_error_category, unknown_error)
{
    std::error_code ec{999, ptp_error_category()};
    EXPECT_EQ(ec.message(), std::string("Unknown PTP error"));
}

TEST(ptp_error_category, category_name)
{
    EXPECT_EQ(std::string(ptp_error_category().name()), std::string("statusbar.ptpclient"));
}

//
// Tests: fit_time_mapping
//

TEST(ptp_fit_time, insufficient_samples)
{
    // 0 or 1 samples should return invalid result
    auto result0 = fit_time_mapping(nullptr, 0, 200.0);
    EXPECT_FALSE(result0.valid);

    TimeSample single{.monotonic_ns = 1000, .ptp_ns = 2000, .bracket_ns = 10};
    auto result1 = fit_time_mapping(&single, 1, 200.0);
    EXPECT_FALSE(result1.valid);
}

TEST(ptp_fit_time, perfect_linear_unity_slope)
{
    // PTP = monotonic + offset (slope = 1.0, offset = 1000)
    constexpr int64_t offset = 1000;
    std::array<TimeSample, 10> samples{};
    for (int i = 0; i < 10; ++i) {
        int64_t mono = static_cast<int64_t>(i) * 1'000'000LL;
        samples[i] = {.monotonic_ns = mono, .ptp_ns = mono + offset, .bracket_ns = 10};
    }

    auto result = fit_time_mapping(samples.data(), samples.size(), 200.0);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(std::fabs(result.slope - 1.0) < 1e-6);
    // Residuals should be near zero for perfect linear data
    EXPECT_TRUE(result.rms_residual < 1.0);
}

TEST(ptp_fit_time, slope_with_rate_offset)
{
    // PTP runs slightly faster: slope = 1.0001 (100 ppm fast)
    constexpr double rate = 1.0001;
    constexpr int64_t base_offset = 500'000;
    std::array<TimeSample, 20> samples{};
    for (int i = 0; i < 20; ++i) {
        int64_t mono = static_cast<int64_t>(i) * 1'000'000LL;
        int64_t ptp = static_cast<int64_t>(static_cast<double>(mono) * rate) + base_offset;
        samples[i] = {.monotonic_ns = mono, .ptp_ns = ptp, .bracket_ns = 10};
    }

    auto result = fit_time_mapping(samples.data(), samples.size(), 200.0);

    EXPECT_TRUE(result.valid);
    // Slope should be close to 1.0001
    EXPECT_TRUE(std::fabs(result.slope - rate) < 1e-5);
    EXPECT_TRUE(result.rms_residual < 10.0);
}

TEST(ptp_fit_time, slope_clamped_to_max_ppm)
{
    // Extreme slope (500 ppm) should be clamped to max_rate_ppm (200 ppm)
    constexpr double extreme_rate = 1.0005;  // 500 ppm
    std::array<TimeSample, 10> samples{};
    for (int i = 0; i < 10; ++i) {
        int64_t mono = static_cast<int64_t>(i) * 1'000'000LL;
        int64_t ptp = static_cast<int64_t>(static_cast<double>(mono) * extreme_rate);
        samples[i] = {.monotonic_ns = mono, .ptp_ns = ptp, .bracket_ns = 10};
    }

    auto result = fit_time_mapping(samples.data(), samples.size(), 200.0);

    EXPECT_TRUE(result.valid);
    // Slope should be clamped: 1.0 + 200ppm = 1.0002
    EXPECT_TRUE(result.slope <= 1.0 + 200.0e-6 + 1e-9);
}

TEST(ptp_fit_time, large_timestamps)
{
    // Simulate realistic timestamps (~1.7e18 ns, about 2 years since epoch)
    constexpr int64_t base_mono = 1'700'000'000'000'000'000LL;
    constexpr int64_t base_ptp = 1'700'000'000'500'000'000LL;
    std::array<TimeSample, 10> samples{};
    for (int i = 0; i < 10; ++i) {
        int64_t mono = base_mono + (static_cast<int64_t>(i) * 2'000'000LL);  // 2ms apart
        int64_t ptp = base_ptp + (static_cast<int64_t>(i) * 2'000'000LL);
        samples[i] = {.monotonic_ns = mono, .ptp_ns = ptp, .bracket_ns = 100};
    }

    auto result = fit_time_mapping(samples.data(), samples.size(), 200.0);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(std::fabs(result.slope - 1.0) < 1e-6);
}

// An absolute nanosecond count must never reach a double: on the TAI epoch a
// double steps by 256 ns, and a sum of a window of them by tens of
// microseconds. Readings that lie exactly on a line are fitted exactly,
// wherever on the time line they are.
TEST(ptp_fit_time, a_tai_scale_epoch_is_fitted_as_exactly_as_one_near_zero)
{
    constexpr int64_t EPOCHS_NS[] = {1'000'000'000'000LL, 9'000'000'000'000'000LL, 1'795'000'000'000'000'000LL};
    for (int64_t const epoch_ns : EPOCHS_NS) {
        // 70 hours of uptime; a PHC 30 ppm fast, read every 2 ms: 60 ns more per sample.
        std::array<TimeSample, 256> samples{};
        for (size_t i = 0; i < samples.size(); ++i) {
            auto const n = static_cast<int64_t>(i);
            samples[i] = {
                .monotonic_ns = 252'000'000'000'000LL + (n * 2'000'000), .ptp_ns = epoch_ns + (n * 2'000'060), .bracket_ns = 400};
        }
        auto const fit = fit_time_mapping(samples, 200.0);
        EXPECT_TRUE(fit.valid);
        EXPECT_TRUE(std::fabs(fit.slope - 1.00003) < 1e-9);
        // Nothing lies off the line...
        EXPECT_TRUE(fit.max_residual < 0.01);
        EXPECT_TRUE(fit.rms_residual < 0.01);
        // ...and the published line gives back every sample to the nanosecond.
        auto const slope = FixedPointSlope::from_double(fit.slope);
        for (auto const& sample : samples) {
            EXPECT_TRUE(std::abs(monotonic_to_ptp_precise(sample.monotonic_ns, slope, fit.intercept_ns) - sample.ptp_ns) <= 1);
            EXPECT_TRUE(std::abs(ptp_to_monotonic_precise(sample.ptp_ns, slope, fit.intercept_ns) - sample.monotonic_ns) <= 1);
        }
    }
}

// The residual is what the samples really miss the line by, at any epoch:
// one sample 700 ns off a line of 256 reads as 700 ns less its own pull.
TEST(ptp_fit_time, a_residual_at_a_tai_scale_epoch_is_the_real_one)
{
    std::array<TimeSample, 256> samples{};
    for (size_t i = 0; i < samples.size(); ++i) {
        auto const n = static_cast<int64_t>(i);
        samples[i] = {
            .monotonic_ns = 252'000'000'000'000LL + (n * 2'000'000),
            .ptp_ns = 1'795'000'000'000'000'000LL + (n * 2'000'000) + (i == 128 ? 700 : 0),
            .bracket_ns = 400};
    }
    auto const fit = fit_time_mapping(samples, 200.0);
    EXPECT_TRUE(fit.max_residual > 690.0 && fit.max_residual < 700.0);
    EXPECT_TRUE(fit.rms_residual < 50.0);
}

TEST(ptp_fit_time, identical_monotonic_values)
{
    // Degenerate case: all monotonic values the same (zero variance)
    std::array<TimeSample, 5> samples{};
    for (int i = 0; i < 5; ++i) {
        samples[i] = {.monotonic_ns = 1000, .ptp_ns = static_cast<int64_t>(2000 + i), .bracket_ns = 10};
    }

    auto result = fit_time_mapping(samples.data(), samples.size(), 200.0);

    // Should return invalid (sum_mm_centered is zero)
    EXPECT_FALSE(result.valid);
}

TEST(ptp_fit_time, residuals_reported)
{
    // Add some noise to verify residuals are computed
    std::array<TimeSample, 10> samples{};
    for (int i = 0; i < 10; ++i) {
        int64_t mono = static_cast<int64_t>(i) * 1'000'000LL;
        int64_t noise = (i % 2 == 0) ? 100 : -100;
        samples[i] = {.monotonic_ns = mono, .ptp_ns = mono + 5000 + noise, .bracket_ns = 10};
    }

    auto result = fit_time_mapping(samples.data(), samples.size(), 200.0);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.rms_residual > 0.0);
    EXPECT_TRUE(result.max_residual > 0.0);
    EXPECT_TRUE(result.max_residual >= result.rms_residual);
}

TEST(ptp_fit_time, steep_clean_line_has_low_residual)
{
    // A clean line with a STEEP slope (+10%), as happens when CLOCK_MONOTONIC is
    // slewed by phc2sys while the bridge tracks it. Fix 1: the residual must be
    // measured against the UNCLAMPED best-fit slope, so a clean steep line reads
    // as ~zero residual (healthy) — it must NOT be pinned to ±max_rate_ppm and
    // then measured against the wrong line (which would manufacture a huge
    // residual and a false "unhealthy").
    constexpr double steep_rate = 1.10;  // +10% (100000 ppm)
    std::array<TimeSample, 32> samples{};
    for (int i = 0; i < 32; ++i) {
        int64_t const mono = static_cast<int64_t>(i) * 1'000'000LL;
        int64_t const ptp = static_cast<int64_t>(static_cast<double>(mono) * steep_rate);
        samples[i] = {.monotonic_ns = mono, .ptp_ns = ptp, .bracket_ns = 10};
    }

    // Narrow clamp (200 ppm): the published slope is clamped, but because the
    // residual is computed against the unclamped slope it stays ~0.
    auto narrow = fit_time_mapping(samples.data(), samples.size(), 200.0);
    EXPECT_TRUE(narrow.valid);
    EXPECT_TRUE(narrow.rms_residual < 1.0);              // clean line -> ~0 residual
    EXPECT_TRUE(narrow.slope <= 1.0 + 200.0e-6 + 1e-9);  // published slope still clamped

    // Wide clamp (±20%): the real +10% slope passes through unclamped.
    auto wide = fit_time_mapping(samples.data(), samples.size(), 200'000.0);
    EXPECT_TRUE(wide.valid);
    EXPECT_TRUE(wide.rms_residual < 1.0);
    EXPECT_TRUE(std::fabs(wide.slope - steep_rate) < 1e-3);  // unclamped -> real slope
}

//
// Main test runner
//

TEST_MAIN(statusbar_ptpclient, ptpclient_base_test)
