#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// PTP Client base module
/// Provides abstract interface for reading PTP hardware clock time

#include "statusbar/dsp/dsp.hpp"
#include "statusbar/realtime/realtime_clock.hpp"
#include "statusbar/status/status.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace statusbar::ptpclient {

using statusbar::failure;
using statusbar::Status;
using statusbar::StatusValue;
using statusbar::success;

//
// Error Handling
//

/// PTP client error codes
enum class PtpError
{
    device_not_found = 1,
    device_open_failed,
    device_not_open,
    clock_gettime_failed,
    invalid_device_path,
    permission_denied,
    not_supported,
    bridge_not_running,
    bridge_not_healthy,
    bridge_epoch_changed,
    invalid_period
};

/// Error category for PTP errors
class PtpErrorCategory : public std::error_category
{
  public:
    [[nodiscard]] auto name() const noexcept -> char const* override { return "statusbar.ptpclient"; }

    [[nodiscard]] auto message(int ev) const -> std::string override;
};

/// Get the singleton PTP error category
[[nodiscard]] inline auto ptp_error_category() noexcept -> std::error_category const&
{
    static PtpErrorCategory const instance;
    return instance;
}

/// Make error_code from PtpError
/// @param e The PTP error code to convert
[[nodiscard]] inline auto make_error_code(PtpError e) noexcept -> std::error_code
{
    return {static_cast<int>(e), ptp_error_category()};
}

//
// Time Mapping Types (for PTP <-> CLOCK_MONOTONIC_RAW conversion)
//

/// Time mapping state between the PTP clock and CLOCK_MONOTONIC_RAW, the
/// local timing reference. The mapping is: ptp_ns = rate * raw_ns + offset_ns.
/// The names below that say "monotonic" (monotonic_ns, ptp_to_monotonic_precise)
/// date from before the reference was RAW and mean the RAW clock.
struct TimeMapping
{
    double rate = 1.0;             ///< PTP rate relative to CLOCK_MONOTONIC_RAW (slope)
    int64_t offset_ns = 0;         ///< Offset in nanoseconds (intercept) - int64_t for precision
    uint64_t epoch = 0;            ///< Incremented on discontinuities/resets
    bool healthy = false;          ///< True if mapping quality is acceptable
    int64_t rms_residual_ns = 0;   ///< RMS of fit residuals in nanoseconds
    int64_t worst_bracket_ns = 0;  ///< Worst sample bracket time in nanoseconds
    int sample_count = 0;          ///< Number of samples in current window
};

/// Configuration parameters for PTP time bridge sampling
struct BridgeSamplingParams
{
    int sample_hz = 500;                    ///< Sampling frequency (200-1000 typical)
    int window_size = 256;                  ///< Points used for regression
    int64_t max_bracket_ns = 30'000;        ///< Reject sample if bracket > this (30us default)
    int64_t step_threshold_ns = 200'000;    ///< Residual > this = step/discontinuity (200us)
    int64_t degrade_threshold_ns = 80'000;  ///< RMS > this = degraded quality (80us)
    double max_rate_ppm = 200.0;            ///< Max rate deviation from 1.0 in ppm
    /// Final spin before a sleep's deadline (ns, 0 to disable): must cover the worst OS wake
    /// latency on the timer's core; ~15-20us suffices on an isolated core (docs/LINUXPTP.md).
    /// PtpTimer::wake_headroom() reports the margin actually left.
    int64_t wake_refine_spin_ns = 50'000;
    int min_samples_for_healthy = 16;       ///< Minimum samples before declaring healthy

    /// How fast the PHC line may go wrong while the PHC cannot be read, in
    /// parts per billion, unless the fits themselves showed the rate moving
    /// more: the deployment's oscillator and temperature figure.
    uint32_t holdover_drift_ppb = realtime::DEFAULT_HOLDOVER_DRIFT_PPB;

    /// An error of the PHC link that no fit shows, in nanoseconds, added to
    /// the bound at every fit: what lies between the true time and a PHC
    /// reading that this bridge cannot measure (the PHC's own offset from
    /// its master, PHY latency asymmetry, the granularity of the hardware
    /// timestamp). The figure of this machine's hardware; 0 claims there is
    /// none, and a negative value is taken as 0.
    int64_t fixed_error_ns = 0;

    /// Builder pattern for configuration
    [[nodiscard]] auto with_sample_hz(int hz) noexcept -> BridgeSamplingParams&
    {
        sample_hz = hz;
        return *this;
    }
    [[nodiscard]] auto with_window_size(int size) noexcept -> BridgeSamplingParams&
    {
        window_size = size;
        return *this;
    }
    [[nodiscard]] auto with_max_bracket_ns(int64_t ns) noexcept -> BridgeSamplingParams&
    {
        max_bracket_ns = ns;
        return *this;
    }
    [[nodiscard]] auto with_step_threshold_ns(int64_t ns) noexcept -> BridgeSamplingParams&
    {
        step_threshold_ns = ns;
        return *this;
    }
    [[nodiscard]] auto with_degrade_threshold_ns(int64_t ns) noexcept -> BridgeSamplingParams&
    {
        degrade_threshold_ns = ns;
        return *this;
    }
    [[nodiscard]] auto with_max_rate_ppm(double ppm) noexcept -> BridgeSamplingParams&
    {
        max_rate_ppm = ppm;
        return *this;
    }
    [[nodiscard]] auto with_wake_refine_spin_ns(int64_t ns) noexcept -> BridgeSamplingParams&
    {
        wake_refine_spin_ns = ns;
        return *this;
    }
    [[nodiscard]] auto with_min_samples_for_healthy(int n) noexcept -> BridgeSamplingParams&
    {
        min_samples_for_healthy = n;
        return *this;
    }
    [[nodiscard]] auto with_holdover_drift_ppb(uint32_t ppb) noexcept -> BridgeSamplingParams&
    {
        holdover_drift_ppb = ppb;
        return *this;
    }
    [[nodiscard]] auto with_fixed_error_ns(int64_t ns) noexcept -> BridgeSamplingParams&
    {
        fixed_error_ns = ns;
        return *this;
    }
};

//
// Time Bridge Math (pure functions, no hardware dependencies)
//

/// A single time sample point for regression
struct TimeSample
{
    int64_t monotonic_ns;  ///< Monotonic clock time (midpoint of bracket)
    int64_t ptp_ns;        ///< PTP clock time
    int64_t bracket_ns;    ///< Bracket width (R2 - R1)
};

/// Result of least-squares linear regression
struct LinearFitResult
{
    double slope = 1.0;        ///< Slope (rate)
    int64_t intercept_ns = 0;  ///< Intercept (offset) in nanoseconds - int64_t for precision
    double rms_residual = 0;   ///< RMS of residuals, against the best-fit line
    double max_residual = 0;   ///< Maximum absolute residual, against the best-fit line
    bool valid = false;        ///< Whether fit was successful
    /// The best-fit slope lay outside the allowed window and `slope` is the
    /// window's edge: the published line is not the line of the samples,
    /// and runs away from them at the difference.
    bool clamped = false;
    /// The furthest any sample lies from the PUBLISHED line, as the
    /// conversions compute it (the slope rounded to a part per million,
    /// the integer intercept), in whole nanoseconds. This, not
    /// max_residual, is how far a conversion can be from what was read.
    int64_t published_miss_ns = 0;
};

/// Fixed-point slope representation for high-precision conversions
/// The slope is represented as numerator/denominator to avoid floating point.
/// For typical PTP/monotonic clock relationships, slope ≈ 1.0 ± 50ppm.
struct FixedPointSlope
{
    int64_t numerator{1'000'000};    ///< Slope numerator (e.g., 999989 for 0.999989)
    int64_t denominator{1'000'000};  ///< Slope denominator (typically 1000000 for ppm precision)

    /// Create from a double slope value
    /// Uses 1e6 denominator for ~1 ppm precision (sufficient for typical clock drift)
    [[nodiscard]] static constexpr auto from_double(double slope) noexcept -> FixedPointSlope
    {
        // Use 1e6 denominator for ppm-level precision
        constexpr int64_t denom = 1'000'000;
        int64_t const num = dsp::lround(slope * static_cast<double>(denom));
        return FixedPointSlope{.numerator = num, .denominator = denom};
    }

    /// Convert back to double (for compatibility)
    [[nodiscard]] constexpr auto to_double() const noexcept -> double
    {
        return static_cast<double>(numerator) / static_cast<double>(denominator);
    }
};

//
// The fit
//
// An absolute nanosecond count never becomes floating point here. A PTP time
// on the TAI epoch is near 1.8e18 ns, where a double steps by 256 ns, and a
// sum of a window of them steps by tens of microseconds. So every sample is
// first taken, in integers, as a pair of deltas from the window's first
// sample. A window spans seconds, and a delta of seconds is exact in a
// double. Only the intercept needs the absolute values back, and it is put
// together in integers.
//

[[nodiscard]] inline auto monotonic_to_ptp_precise(int64_t monotonic_ns, FixedPointSlope slope, int64_t offset_ns) noexcept
    -> int64_t;

namespace detail {

/// A sample as its distance from the window's mean point, in nanoseconds.
struct CentredSample
{
    double m;
    double p;
};

/// Where a window is centred: its first sample, kept as the integers it
/// came as, and the mean of the samples' deltas from it.
struct FitCentre
{
    int64_t first_m{0};
    int64_t first_p{0};
    double mean_dm{0.0};
    double mean_dp{0.0};

    [[nodiscard]] constexpr auto centred(TimeSample const& sample) const noexcept -> CentredSample
    {
        return {
            .m = static_cast<double>(sample.monotonic_ns - first_m) - mean_dm,
            .p = static_cast<double>(sample.ptp_ns - first_p) - mean_dp};
    }
};

/// Centre a window of at least one sample.
template <typename Container>
[[nodiscard]] auto fit_centre(Container const& samples) noexcept -> FitCentre
{
    FitCentre centre{.first_m = samples[0].monotonic_ns, .first_p = samples[0].ptp_ns};
    for (size_t i = 0; i < samples.size(); ++i) {
        centre.mean_dm += static_cast<double>(samples[i].monotonic_ns - centre.first_m);
        centre.mean_dp += static_cast<double>(samples[i].ptp_ns - centre.first_p);
    }
    auto const n = static_cast<double>(samples.size());
    centre.mean_dm /= n;
    centre.mean_dp /= n;
    return centre;
}

/// The least-squares slope, sum((m - mean_m)(p - mean_p)) / sum((m - mean_m)^2),
/// or nothing when the monotonic values do not spread.
template <typename Container>
[[nodiscard]] auto fit_slope(Container const& samples, FitCentre const& centre) noexcept -> std::optional<double>
{
    double sum_mm = 0.0;
    double sum_mp = 0.0;
    for (size_t i = 0; i < samples.size(); ++i) {
        CentredSample const c = centre.centred(samples[i]);
        sum_mm += c.m * c.m;
        sum_mp += c.m * c.p;
    }
    if (std::abs(sum_mm) < 1e-9) {
        return std::nullopt;
    }
    return sum_mp / sum_mm;
}

/// The intercept of the published line `ptp = slope * mono + intercept`,
/// which passes through the window's mean point:
///   intercept = (first_p + mean_dp) - slope * (first_m + mean_dm)
/// The part from the absolute values is integer arithmetic; the part from
/// the means is small. It uses the published slope, so that the pair is a
/// self-consistent conversion line.
[[nodiscard]] inline auto fit_intercept(FitCentre const& centre, FixedPointSlope slope) noexcept -> int64_t
{
    __int128 const slope_first_m = (static_cast<__int128>(centre.first_m) * slope.numerator) / slope.denominator;
    double const from_means = centre.mean_dp - (slope.to_double() * centre.mean_dm);
    return static_cast<int64_t>(static_cast<__int128>(centre.first_p) - slope_first_m) + std::llround(from_means);
}

/// How far the samples lie from the best-fit line.
struct FitResiduals
{
    double rms{0.0};
    double max{0.0};
};

template <typename Container>
[[nodiscard]] auto fit_residuals(Container const& samples, FitCentre const& centre, double slope_fit) noexcept -> FitResiduals
{
    double rss = 0.0;
    double max_abs = 0.0;
    for (size_t i = 0; i < samples.size(); ++i) {
        CentredSample const c = centre.centred(samples[i]);
        double const residual = c.p - (slope_fit * c.m);
        rss += residual * residual;
        max_abs = std::max(max_abs, std::abs(residual));
    }
    return {.rms = std::sqrt(rss / static_cast<double>(samples.size())), .max = max_abs};
}

/// The furthest any sample lies from the published line, converted the
/// way every reader converts.
template <typename Container>
[[nodiscard]] auto published_miss_ns(Container const& samples, FixedPointSlope slope, int64_t intercept_ns) noexcept -> int64_t
{
    int64_t worst = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        int64_t const miss = monotonic_to_ptp_precise(samples[i].monotonic_ns, slope, intercept_ns) - samples[i].ptp_ns;
        worst = std::max(worst, miss < 0 ? -miss : miss);
    }
    return worst;
}

/// The result for a window whose best-fit slope is @p slope_fit.
template <typename Container>
[[nodiscard]] auto fit_result(Container const& samples, FitCentre const& centre, double slope_fit, double max_rate_ppm) noexcept
    -> LinearFitResult
{
    // The PUBLISHED slope is clamped to a plausible window only as a safety bound
    // on scheduling (a degenerate fit must not be able to throw a wake an
    // arbitrary distance into the future). Callers that legitimately expect a
    // large slope (e.g. the RAW<->MONOTONIC tracking fit during a phc2sys slew)
    // pass a wide max_rate_ppm so the real slope passes through unclamped.
    double const max_ppm = std::max(10.0, max_rate_ppm);
    double const slope_pub = std::clamp(slope_fit, 1.0 - (max_ppm * 1e-6), 1.0 + (max_ppm * 1e-6));
    // Residuals are judged against the UNCLAMPED best-fit slope: a clean but
    // steep line (CLOCK_MONOTONIC slewed by phc2sys while the bridge tracks
    // it) is a good fit and must read as healthy, not be pinned to
    // +/-max_rate_ppm and then measured against the wrong line. What the
    // published line itself misses by is told beside them.
    FitResiduals const residuals = fit_residuals(samples, centre, slope_fit);
    auto const published = FixedPointSlope::from_double(slope_pub);
    int64_t const intercept_ns = fit_intercept(centre, published);
    return {
        .slope = slope_pub,
        .intercept_ns = intercept_ns,
        .rms_residual = residuals.rms,
        .max_residual = residuals.max,
        .valid = true,
        .clamped = slope_pub != slope_fit,
        .published_miss_ns = published_miss_ns(samples, published, intercept_ns)};
}

}  // namespace detail

/// Perform least-squares linear regression: ptp = slope * monotonic + intercept
/// @param samples Array of time samples
/// @param count Number of samples
/// @param max_rate_ppm Maximum allowed rate deviation from 1.0 in ppm (clamped)
/// @return Fit result with slope, intercept, and residual statistics
[[nodiscard]] auto fit_time_mapping(TimeSample const* samples, size_t count, double max_rate_ppm) noexcept -> LinearFitResult;

/// Fit linear mapping from an indexable container of TimeSample
/// @tparam Container Type with operator[] and size() returning TimeSample
/// @param samples Container of time samples (e.g., SampleWindow)
/// @param max_rate_ppm Maximum allowed rate deviation from 1.0 in ppm (clamped)
/// @return Fit result with slope, intercept, and residual statistics
template <typename Container>
[[nodiscard]] auto fit_time_mapping(Container const& samples, double max_rate_ppm) noexcept -> LinearFitResult
{
    if (samples.size() < 2) {
        return {};
    }
    auto const centre = detail::fit_centre(samples);
    auto const slope_fit = detail::fit_slope(samples, centre);
    if (!slope_fit) {
        return {};
    }
    return detail::fit_result(samples, centre, *slope_fit, max_rate_ppm);
}

//
// High-Precision Time Conversion using __int128
//
// These functions avoid floating point precision loss when dealing with large
// absolute timestamps (~10^18 ns). The slope is represented as a fixed-point
// ratio (numerator/denominator) to maintain full integer precision.
//
// For a slope of 0.999989 (11 ppm off), we use:
//   slope_num = 999989, slope_denom = 1000000
// This gives us exact arithmetic without floating point rounding errors.

/// Convert PTP time to monotonic time using __int128 for full precision
/// @param ptp_ns PTP time in nanoseconds
/// @param slope Fixed-point slope (rate)
/// @param offset_ns Mapping offset in nanoseconds
/// @return Monotonic time in nanoseconds
[[nodiscard]] inline auto ptp_to_monotonic_precise(int64_t ptp_ns, FixedPointSlope slope, int64_t offset_ns) noexcept
{
    // ptp = (slope_num/slope_denom) * mono + offset
    // mono = (ptp - offset) * slope_denom / slope_num
    __int128 const diff = static_cast<__int128>(ptp_ns) - static_cast<__int128>(offset_ns);
    __int128 const scaled = diff * static_cast<__int128>(slope.denominator);
    __int128 const result = scaled / static_cast<__int128>(slope.numerator);
    return static_cast<int64_t>(result);
}

/// Convert monotonic time to PTP time using __int128 for full precision
/// @param monotonic_ns Monotonic time in nanoseconds
/// @param slope Fixed-point slope (rate)
/// @param offset_ns Mapping offset in nanoseconds
/// @return PTP time in nanoseconds
[[nodiscard]] inline auto monotonic_to_ptp_precise(int64_t monotonic_ns, FixedPointSlope slope, int64_t offset_ns) noexcept
    -> int64_t
{
    // ptp = (slope_num/slope_denom) * mono + offset
    __int128 const mono128 = static_cast<__int128>(monotonic_ns);
    __int128 const scaled = mono128 * static_cast<__int128>(slope.numerator);
    __int128 const divided = scaled / static_cast<__int128>(slope.denominator);
    __int128 const result = divided + static_cast<__int128>(offset_ns);
    return static_cast<int64_t>(result);
}

/// Convert timespec to nanoseconds
/// @param tv_sec Seconds component of the timespec
/// @param tv_nsec Nanoseconds component of the timespec
[[nodiscard]] constexpr auto timespec_to_ns(int64_t tv_sec, int64_t tv_nsec) noexcept
{
    return (tv_sec * 1'000'000'000LL) + tv_nsec;
}

/// Convert nanoseconds to timespec components
/// @param ns Nanoseconds
/// @param out_sec Output: seconds
/// @param out_nsec Output: nanoseconds (0-999999999)
constexpr void ns_to_timespec(int64_t ns, int64_t& out_sec, int64_t& out_nsec) noexcept
{
    out_sec = ns / 1'000'000'000LL;
    out_nsec = ns % 1'000'000'000LL;
    if (out_nsec < 0) {
        out_nsec += 1'000'000'000LL;
        out_sec -= 1;
    }
}

//
// PTP Client Base Class
//

/// Abstract base class for PTP clock access
/// Provides interface for reading hardware PTP clock time
class PtpClientBase
{
  public:
    virtual ~PtpClientBase() noexcept = default;

    // No copy
    PtpClientBase(PtpClientBase const&) = delete;
    auto operator=(PtpClientBase const&) -> PtpClientBase& = delete;

    // Move allowed
    PtpClientBase(PtpClientBase&&) noexcept = default;
    auto operator=(PtpClientBase&&) noexcept -> PtpClientBase& = default;

    /// Open the PTP device
    /// @param device_path Path to the PTP device (e.g., "/dev/ptp0")
    /// @return Status indicating success or failure
    [[nodiscard]] virtual auto open(std::string_view device_path) noexcept -> Status = 0;

    /// Close the PTP device
    virtual void close() noexcept = 0;

    /// Check if the device is open
    [[nodiscard]] virtual auto is_open() const noexcept -> bool = 0;

    /// Get the current PTP time
    /// @return Nanoseconds since epoch, or error
    [[nodiscard]] virtual auto get_time_ns() const noexcept -> StatusValue<int64_t> = 0;

    /// Get the device path
    [[nodiscard]] virtual auto device_path() const noexcept -> std::string_view = 0;

  protected:
    PtpClientBase() noexcept = default;
};

/// Default PTP device path
inline constexpr std::string_view default_ptp_device = "/dev/ptp0";

//
// System Clock PTP Client (Fallback)
//

/// System clock based PTP client for platforms without hardware PTP support
/// Uses std::chrono::steady_clock as a monotonic time source.
/// This provides the same API as hardware PTP clients but uses system time,
/// making code portable across platforms. The bridge will show rate=1.0
/// and minimal offset since both source and monotonic use the same clock.
class SystemClockPtpClient : public PtpClientBase
{
  public:
    SystemClockPtpClient() = default;

    /// Construct with optional device name (for display purposes only)
    /// @param name Device name used for display identification
    explicit SystemClockPtpClient(std::string_view name)
        : device_name_{name}
        , open_{true}
    {}

    ~SystemClockPtpClient() noexcept override = default;

    // Non-copyable, non-movable
    SystemClockPtpClient(SystemClockPtpClient const&) = delete;
    auto operator=(SystemClockPtpClient const&) -> SystemClockPtpClient& = delete;
    SystemClockPtpClient(SystemClockPtpClient&&) = delete;
    auto operator=(SystemClockPtpClient&&) -> SystemClockPtpClient& = delete;

    [[nodiscard]] auto open(std::string_view device_path) noexcept -> Status override
    {
        device_name_ = std::string(device_path.empty() ? "system_clock" : device_path);
        open_ = true;
        return success();
    }

    void close() noexcept override { open_ = false; }

    [[nodiscard]] auto is_open() const noexcept -> bool override { return open_; }

    [[nodiscard]] auto get_time_ns() const noexcept -> StatusValue<int64_t> override
    {
        if (!open_) {
            return failure(make_error_code(PtpError::device_not_open));
        }

        auto const now = std::chrono::steady_clock::now();
        auto const ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

        return success(ns);
    }

    [[nodiscard]] auto device_path() const noexcept -> std::string_view override { return device_name_; }

  private:
    std::string device_name_{"system_clock"};
    bool open_{false};
};

/// Driver name for system clock fallback
inline constexpr std::string_view DRIVER_SYSTEM_CLOCK = "system";

}  // namespace statusbar::ptpclient

// Make PtpError work with std::error_code
template <>
struct std::is_error_code_enum<statusbar::ptpclient::PtpError> : std::true_type
{};
