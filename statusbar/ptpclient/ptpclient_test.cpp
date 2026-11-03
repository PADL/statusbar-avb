// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ptpclient/ptpclient.hpp"

#include "statusbar/status/status.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <system_error>

using namespace statusbar::ptpclient;

//
// Static assertions for constexpr verification
//

// timespec_to_ns() - constexpr verification
static_assert(timespec_to_ns(0, 0) == 0);
static_assert(timespec_to_ns(1, 0) == 1'000'000'000LL);
static_assert(timespec_to_ns(0, 1) == 1);
static_assert(timespec_to_ns(1, 500'000'000) == 1'500'000'000LL);
static_assert(timespec_to_ns(86400, 123456789) == 86400LL * 1'000'000'000LL + 123456789LL);

// ns_to_timespec() - constexpr verification via lambda
static_assert([]() constexpr {
    int64_t sec = 0, nsec = 0;
    ns_to_timespec(0, sec, nsec);
    return sec == 0 && nsec == 0;
}());

static_assert([]() constexpr {
    int64_t sec = 0, nsec = 0;
    ns_to_timespec(1'500'000'000LL, sec, nsec);
    return sec == 1 && nsec == 500'000'000;
}());

static_assert([]() constexpr {
    int64_t sec = 0, nsec = 0;
    ns_to_timespec(-500'000'000LL, sec, nsec);
    return sec == -1 && nsec == 500'000'000;
}());

// timespec roundtrip - constexpr verification
static_assert([]() constexpr {
    int64_t const original = 123456789012345LL;
    int64_t sec = 0, nsec = 0;
    ns_to_timespec(original, sec, nsec);
    return timespec_to_ns(sec, nsec) == original;
}());

// ptp_to_monotonic() - constexpr verification
static_assert(ptp_to_monotonic(1'000'000'000LL, 1.0, 0.0) == 1'000'000'000LL);
static_assert(ptp_to_monotonic(1'000'000'100LL, 1.0, 100.0) == 1'000'000'000LL);

// monotonic_to_ptp() - constexpr verification
static_assert(monotonic_to_ptp(1'000'000'000LL, 1.0, 0.0) == 1'000'000'000LL);
static_assert(monotonic_to_ptp(1'000'000'000LL, 1.0, 100.0) == 1'000'000'100LL);

// FixedPointSlope - constexpr verification
static_assert([]() constexpr {
    auto slope = FixedPointSlope::from_double(1.0);
    return slope.numerator == 1'000'000 && slope.denominator == 1'000'000;
}());

static_assert([]() constexpr {
    auto slope = FixedPointSlope::from_double(1.000001);
    // 1.000001 * 1000000 = 1000001
    return slope.numerator == 1'000'001 && slope.denominator == 1'000'000;
}());

static_assert([]() constexpr {
    FixedPointSlope slope{999'989, 1'000'000};  // ~-11 ppm
    double d = slope.to_double();
    // Can't compare doubles directly in constexpr, but we can check it compiles
    return d > 0.99 && d < 1.01;
}());

// Default constants
static_assert(default_ptp_device == "/dev/ptp0");
static_assert(DRIVER_SYSTEM_CLOCK == "system");

//
// TimeSpec Conversion Tests
//

TEST(ptpclient_timespec, timespec_to_ns_positive)
{
    int64_t const ns = timespec_to_ns(1, 500'000'000);
    EXPECT_EQ(ns, 1'500'000'000LL);
}

TEST(ptpclient_timespec, timespec_to_ns_zero)
{
    int64_t const ns = timespec_to_ns(0, 0);
    EXPECT_EQ(ns, 0LL);
}

TEST(ptpclient_timespec, timespec_to_ns_large)
{
    // 1 day in seconds + 123456789 nanoseconds
    int64_t const ns = timespec_to_ns(86400, 123456789);
    EXPECT_EQ(ns, 86400LL * 1'000'000'000LL + 123456789LL);
}

TEST(ptpclient_timespec, ns_to_timespec_positive)
{
    int64_t sec, nsec;
    ns_to_timespec(1'500'000'000LL, sec, nsec);
    EXPECT_EQ(sec, 1);
    EXPECT_EQ(nsec, 500'000'000);
}

TEST(ptpclient_timespec, ns_to_timespec_zero)
{
    int64_t sec, nsec;
    ns_to_timespec(0, sec, nsec);
    EXPECT_EQ(sec, 0);
    EXPECT_EQ(nsec, 0);
}

TEST(ptpclient_timespec, ns_to_timespec_negative)
{
    int64_t sec, nsec;
    ns_to_timespec(-500'000'000LL, sec, nsec);
    EXPECT_EQ(sec, -1);
    EXPECT_EQ(nsec, 500'000'000);
}

TEST(ptpclient_timespec, ns_to_timespec_roundtrip)
{
    int64_t const original = 123456789012345LL;
    int64_t sec, nsec;
    ns_to_timespec(original, sec, nsec);
    int64_t const result = timespec_to_ns(sec, nsec);
    EXPECT_EQ(result, original);
}

//
// Time Conversion Tests (ptp <-> monotonic)
//

TEST(ptpclient_convert, ptp_to_monotonic_unity)
{
    // With slope=1.0 and intercept=0.0, ptp == monotonic
    int64_t const mono = ptp_to_monotonic(1'000'000'000LL, 1.0, 0.0);
    EXPECT_EQ(mono, 1'000'000'000LL);
}

TEST(ptpclient_convert, monotonic_to_ptp_unity)
{
    // With slope=1.0 and intercept=0.0, monotonic == ptp
    int64_t const ptp = monotonic_to_ptp(1'000'000'000LL, 1.0, 0.0);
    EXPECT_EQ(ptp, 1'000'000'000LL);
}

TEST(ptpclient_convert, ptp_to_monotonic_with_offset)
{
    // ptp = 1.0 * mono + 100, so mono = ptp - 100
    int64_t const mono = ptp_to_monotonic(1'000'000'100LL, 1.0, 100.0);
    EXPECT_EQ(mono, 1'000'000'000LL);
}

TEST(ptpclient_convert, monotonic_to_ptp_with_offset)
{
    // ptp = 1.0 * mono + 100
    int64_t const ptp = monotonic_to_ptp(1'000'000'000LL, 1.0, 100.0);
    EXPECT_EQ(ptp, 1'000'000'100LL);
}

TEST(ptpclient_convert, ptp_to_monotonic_with_rate)
{
    // ptp = 1.001 * mono + 0 (100 ppm fast)
    // mono = ptp / 1.001
    double const slope = 1.001;
    int64_t const ptp_ns = 1'001'000'000LL;
    int64_t const mono = ptp_to_monotonic(ptp_ns, slope, 0.0);
    // Expected: 1001000000 / 1.001 ≈ 1000000000
    EXPECT_TRUE(std::abs(mono - 1'000'000'000LL) < 1000);  // Within 1us
}

TEST(ptpclient_convert, monotonic_to_ptp_with_rate)
{
    // ptp = 1.001 * mono + 0 (100 ppm fast)
    double const slope = 1.001;
    int64_t const mono_ns = 1'000'000'000LL;
    int64_t const ptp = monotonic_to_ptp(mono_ns, slope, 0.0);
    // Expected: 1000000000 * 1.001 = 1001000000
    EXPECT_EQ(ptp, 1'001'000'000LL);
}

TEST(ptpclient_convert, roundtrip_conversion)
{
    double const slope = 1.0001;  // 100 ppm
    double const intercept = 12345.0;
    int64_t const original_mono = 1'000'000'000'000LL;

    int64_t const ptp = monotonic_to_ptp(original_mono, slope, intercept);
    int64_t const back_to_mono = ptp_to_monotonic(ptp, slope, intercept);

    // Should be within 1ns due to rounding
    EXPECT_TRUE(std::abs(back_to_mono - original_mono) <= 1);
}

//
// Linear Fit Tests
//

TEST(ptpclient_fit, fit_with_no_samples)
{
    auto result = fit_time_mapping(nullptr, 0, 200.0);
    EXPECT_FALSE(result.valid);
}

TEST(ptpclient_fit, fit_with_one_sample)
{
    TimeSample samples[] = {{1'000'000'000LL, 1'000'000'000LL, 1000}};
    auto result = fit_time_mapping(samples, 1, 200.0);
    EXPECT_FALSE(result.valid);
}

TEST(ptpclient_fit, fit_with_perfect_unity)
{
    // Perfect unity relationship: ptp = mono
    TimeSample samples[] = {
        {1'000'000'000LL, 1'000'000'000LL, 1000},
        {2'000'000'000LL, 2'000'000'000LL, 1000},
        {3'000'000'000LL, 3'000'000'000LL, 1000},
        {4'000'000'000LL, 4'000'000'000LL, 1000},
    };

    auto result = fit_time_mapping(samples, 4, 200.0);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(std::abs(result.slope - 1.0) < 1e-9);
    EXPECT_TRUE(std::abs(result.intercept_ns) < 1);
    EXPECT_TRUE(result.rms_residual < 1.0);
}

TEST(ptpclient_fit, fit_with_offset)
{
    // ptp = mono + 1000000 (1ms offset)
    int64_t const offset = 1'000'000LL;
    TimeSample samples[] = {
        {1'000'000'000LL, 1'000'000'000LL + offset, 1000},
        {2'000'000'000LL, 2'000'000'000LL + offset, 1000},
        {3'000'000'000LL, 3'000'000'000LL + offset, 1000},
        {4'000'000'000LL, 4'000'000'000LL + offset, 1000},
    };

    auto result = fit_time_mapping(samples, 4, 200.0);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(std::abs(result.slope - 1.0) < 1e-9);
    EXPECT_TRUE(std::abs(result.intercept_ns - offset) < 1);
    EXPECT_TRUE(result.rms_residual < 1.0);
}

TEST(ptpclient_fit, fit_with_rate)
{
    // ptp = 1.0001 * mono (100 ppm fast)
    double const rate = 1.0001;
    TimeSample samples[] = {
        {1'000'000'000LL, static_cast<int64_t>(1'000'000'000.0 * rate), 1000},
        {2'000'000'000LL, static_cast<int64_t>(2'000'000'000.0 * rate), 1000},
        {3'000'000'000LL, static_cast<int64_t>(3'000'000'000.0 * rate), 1000},
        {4'000'000'000LL, static_cast<int64_t>(4'000'000'000.0 * rate), 1000},
    };

    auto result = fit_time_mapping(samples, 4, 200.0);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(std::abs(result.slope - rate) < 1e-6);
    EXPECT_TRUE(std::abs(result.intercept_ns) < 1000);  // Near zero intercept
}

TEST(ptpclient_fit, fit_clamps_rate_high)
{
    // Try to fit with extreme rate (1000 ppm), should be clamped to max_rate_ppm
    double const extreme_rate = 1.001;  // 1000 ppm
    TimeSample samples[] = {
        {1'000'000'000LL, static_cast<int64_t>(1'000'000'000.0 * extreme_rate), 1000},
        {2'000'000'000LL, static_cast<int64_t>(2'000'000'000.0 * extreme_rate), 1000},
        {3'000'000'000LL, static_cast<int64_t>(3'000'000'000.0 * extreme_rate), 1000},
        {4'000'000'000LL, static_cast<int64_t>(4'000'000'000.0 * extreme_rate), 1000},
    };

    auto result = fit_time_mapping(samples, 4, 200.0);  // Max 200 ppm

    EXPECT_TRUE(result.valid);
    // Rate should be clamped to 1.0002 (200 ppm)
    EXPECT_TRUE(result.slope <= 1.0002 + 1e-9);
    EXPECT_TRUE(result.slope >= 1.0 - 1e-9);
}

TEST(ptpclient_fit, fit_clamps_rate_low)
{
    // Try to fit with extreme rate (1000 ppm slow), should be clamped
    double const extreme_rate = 0.999;  // 1000 ppm slow
    TimeSample samples[] = {
        {1'000'000'000LL, static_cast<int64_t>(1'000'000'000.0 * extreme_rate), 1000},
        {2'000'000'000LL, static_cast<int64_t>(2'000'000'000.0 * extreme_rate), 1000},
        {3'000'000'000LL, static_cast<int64_t>(3'000'000'000.0 * extreme_rate), 1000},
        {4'000'000'000LL, static_cast<int64_t>(4'000'000'000.0 * extreme_rate), 1000},
    };

    auto result = fit_time_mapping(samples, 4, 200.0);  // Max 200 ppm

    EXPECT_TRUE(result.valid);
    // Rate should be clamped to 0.9998 (200 ppm slow)
    EXPECT_TRUE(result.slope >= 0.9998 - 1e-9);
    EXPECT_TRUE(result.slope <= 1.0 + 1e-9);
}

TEST(ptpclient_fit, fit_with_noise)
{
    // Data with some noise (simulating real samples)
    // Noise is ±100ns from ideal line ptp = mono
    TimeSample samples[] = {
        {1'000'000'000LL, 1'000'000'100LL, 1000},  // +100ns noise
        {2'000'000'000LL, 1'999'999'900LL, 1000},  // -100ns noise
        {3'000'000'000LL, 3'000'000'050LL, 1000},  // +50ns noise
        {4'000'000'000LL, 3'999'999'950LL, 1000},  // -50ns noise
        {5'000'000'000LL, 5'000'000'000LL, 1000},  // No noise
        {6'000'000'000LL, 6'000'000'000LL, 1000},  // No noise
        {7'000'000'000LL, 7'000'000'100LL, 1000},  // +100ns noise
        {8'000'000'000LL, 7'999'999'900LL, 1000},  // -100ns noise
    };

    auto result = fit_time_mapping(samples, 8, 200.0);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(std::abs(result.slope - 1.0) < 1e-6);
    EXPECT_TRUE(result.rms_residual < 150.0);   // RMS should be reasonable
    EXPECT_TRUE(result.max_residual <= 150.0);  // Max residual includes fitting error
}

TEST(ptpclient_fit, fit_residual_statistics)
{
    // Create data with known residual pattern
    // ptp = mono + residual where residuals are [-100, +100, -50, +50]
    TimeSample samples[] = {
        {1'000'000'000LL, 999'999'900LL, 1000},    // -100
        {2'000'000'000LL, 2'000'000'100LL, 1000},  // +100
        {3'000'000'000LL, 2'999'999'950LL, 1000},  // -50
        {4'000'000'000LL, 4'000'000'050LL, 1000},  // +50
    };

    auto result = fit_time_mapping(samples, 4, 200.0);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.max_residual >= 50.0);
    EXPECT_TRUE(result.max_residual <= 150.0);
}

TEST(ptpclient_fit, fit_with_large_offset)
{
    // Simulate real-world scenario with large offset between clocks
    // Uses exact integer arithmetic to avoid double precision issues in test data
    // - Monotonic time: 67 seconds
    // - PTP time: monotonic + 1735 billion seconds (year 2025 from 1970)
    //
    // Key insight: We construct ptp = mono + offset + noise where noise is exact
    int64_t const base_mono = 67'000'000'000LL;          // 67 seconds in ns
    int64_t const offset = 1'735'000'000'000'000'000LL;  // Large offset

    // Construct samples with no jitter (perfectly synchronized clocks)
    TimeSample samples[] = {
        {base_mono + 0LL, base_mono + offset + 0LL, 1000},
        {base_mono + 2'000'000LL, base_mono + offset + 2'000'000LL, 1000},
        {base_mono + 4'000'000LL, base_mono + offset + 4'000'000LL, 1000},
        {base_mono + 6'000'000LL, base_mono + offset + 6'000'000LL, 1000},
        {base_mono + 8'000'000LL, base_mono + offset + 8'000'000LL, 1000},
        {base_mono + 10'000'000LL, base_mono + offset + 10'000'000LL, 1000},
        {base_mono + 12'000'000LL, base_mono + offset + 12'000'000LL, 1000},
        {base_mono + 14'000'000LL, base_mono + offset + 14'000'000LL, 1000},
    };

    auto result = fit_time_mapping(samples, 8, 200.0);

    EXPECT_TRUE(result.valid);
    // Slope should be 1.0 (perfect linear relationship with no jitter)
    // Allow some tolerance due to double precision
    EXPECT_TRUE(std::abs(result.slope - 1.0) < 1e-4);
    // RMS should be very small for perfect data (some double precision error)
    EXPECT_TRUE(result.rms_residual < 1000.0);
}

TEST(ptpclient_fit, fit_with_small_offset)
{
    // Simulate PTP clock that counts from boot/NIC init (small offset)
    // Monotonic: 67 seconds, PTP: 67.1 seconds (100ms offset)
    int64_t const base_mono = 67'000'000'000LL;  // 67 seconds
    int64_t const offset = 100'000'000LL;        // 100ms offset

    TimeSample samples[] = {
        {base_mono + 0LL, base_mono + offset + 0LL, 1000},
        {base_mono + 2'000'000LL, base_mono + offset + 2'000'000LL, 1000},
        {base_mono + 4'000'000LL, base_mono + offset + 4'000'000LL, 1000},
        {base_mono + 6'000'000LL, base_mono + offset + 6'000'000LL, 1000},
        {base_mono + 8'000'000LL, base_mono + offset + 8'000'000LL, 1000},
        {base_mono + 10'000'000LL, base_mono + offset + 10'000'000LL, 1000},
        {base_mono + 12'000'000LL, base_mono + offset + 12'000'000LL, 1000},
        {base_mono + 14'000'000LL, base_mono + offset + 14'000'000LL, 1000},
    };

    auto result = fit_time_mapping(samples, 8, 200.0);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(std::abs(result.slope - 1.0) < 1e-6);
    EXPECT_TRUE(result.rms_residual < 100.0);
    // With small offset, intercept should be very accurate
    EXPECT_TRUE(std::abs(result.intercept_ns - offset) < 10);
}

//
// BridgeSamplingParams Builder Tests
//

TEST(ptpclient_params, default_values)
{
    BridgeSamplingParams params;
    EXPECT_EQ(params.sample_hz, 500);
    EXPECT_EQ(params.window_size, 256);
    EXPECT_EQ(params.max_bracket_ns, 30'000);
    EXPECT_EQ(params.step_threshold_ns, 200'000);
    EXPECT_EQ(params.degrade_threshold_ns, 80'000);
    EXPECT_TRUE(std::abs(params.max_rate_ppm - 200.0) < 0.001);
    EXPECT_EQ(params.wake_refine_spin_ns, 50'000);
    EXPECT_EQ(params.min_samples_for_healthy, 16);
}

TEST(ptpclient_params, builder_chaining)
{
    BridgeSamplingParams params;
    auto& result = params.with_sample_hz(1000)
                       .with_window_size(512)
                       .with_max_bracket_ns(20'000)
                       .with_step_threshold_ns(100'000)
                       .with_degrade_threshold_ns(50'000)
                       .with_max_rate_ppm(100.0)
                       .with_wake_refine_spin_ns(25'000)
                       .with_min_samples_for_healthy(32);

    // Verify chaining returns same object
    EXPECT_EQ(&result, &params);
    EXPECT_EQ(params.sample_hz, 1000);
    EXPECT_EQ(params.window_size, 512);
    EXPECT_EQ(params.max_bracket_ns, 20'000);
    EXPECT_EQ(params.step_threshold_ns, 100'000);
    EXPECT_EQ(params.degrade_threshold_ns, 50'000);
    EXPECT_TRUE(std::abs(params.max_rate_ppm - 100.0) < 0.001);
    EXPECT_EQ(params.wake_refine_spin_ns, 25'000);
    EXPECT_EQ(params.min_samples_for_healthy, 32);
}

//
// TimeMapping Struct Tests
//

TEST(ptpclient_mapping, default_values)
{
    TimeMapping mapping;
    EXPECT_TRUE(std::abs(mapping.rate - 1.0) < 1e-9);
    EXPECT_TRUE(std::abs(mapping.offset_ns) < 1e-9);
    EXPECT_EQ(mapping.epoch, 0U);
    EXPECT_FALSE(mapping.healthy);
    EXPECT_EQ(mapping.rms_residual_ns, 0);
    EXPECT_EQ(mapping.worst_bracket_ns, 0);
    EXPECT_EQ(mapping.sample_count, 0);
}

//
// TimeConvertResult Struct Tests
//

TEST(ptpclient_result, default_values)
{
    TimeConvertResult result;
    EXPECT_EQ(result.time_ns, 0);
    EXPECT_EQ(result.epoch, 0U);
    EXPECT_FALSE(result.healthy);
}

//
// TimeSample Struct Tests
//

TEST(ptpclient_sample, struct_layout)
{
    TimeSample sample{1'000'000'000LL, 1'000'000'100LL, 5000};
    EXPECT_EQ(sample.monotonic_ns, 1'000'000'000LL);
    EXPECT_EQ(sample.ptp_ns, 1'000'000'100LL);
    EXPECT_EQ(sample.bracket_ns, 5000);
}

//
// LinearFitResult Struct Tests
//

TEST(ptpclient_fitresult, default_values)
{
    LinearFitResult result;
    EXPECT_TRUE(std::abs(result.slope - 1.0) < 1e-9);
    EXPECT_EQ(result.intercept_ns, 0);
    EXPECT_TRUE(std::abs(result.rms_residual) < 1e-9);
    EXPECT_TRUE(std::abs(result.max_residual) < 1e-9);
    EXPECT_FALSE(result.valid);
}

//
// PtpError Tests
//

TEST(ptpclient_error, error_code_construction)
{
    auto ec = make_error_code(PtpError::device_not_found);
    EXPECT_EQ(ec.value(), static_cast<int>(PtpError::device_not_found));
    EXPECT_FALSE(ec.message().empty());
}

TEST(ptpclient_error, all_error_codes_have_messages)
{
    std::array<PtpError, 10> errors = {
        PtpError::device_not_found,
        PtpError::device_open_failed,
        PtpError::device_not_open,
        PtpError::clock_gettime_failed,
        PtpError::invalid_device_path,
        PtpError::permission_denied,
        PtpError::not_supported,
        PtpError::bridge_not_running,
        PtpError::bridge_not_healthy,
        PtpError::bridge_epoch_changed,
    };

    for (auto err : errors) {
        auto ec = make_error_code(err);
        EXPECT_FALSE(ec.message().empty());
        EXPECT_NE(ec.message(), "Unknown PTP error");
    }
}

//
// Tests: PtpTimer - statistics
//

TEST(ptpclient_timer, reset_stats_starts_the_statistics_afresh)
{
    PtpTimeBridge bridge;
    auto timer = make_ptp_timer(bridge, 125'000, [](StatusValue<TimerWakeInfo> const&) {}, 0, false, -1);
    EXPECT_FALSE(timer.is_running());
    EXPECT_EQ(timer.stats().error.count, int64_t{0});
    timer.reset_stats();  // not running: nothing to reset, and nothing breaks
    EXPECT_EQ(timer.stats().error.count, int64_t{0});
    EXPECT_EQ(timer.wake_headroom().count, int64_t{0});
    EXPECT_TRUE(timer.missed_cycles() == 0 && timer.recovery_count() == 0);
}

//
// Main test runner
//

TEST_MAIN(statusbar_ptpclient, ptpclient_test)