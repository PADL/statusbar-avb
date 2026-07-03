#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// PTP Client base module
/// Provides abstract interface for reading PTP hardware clock time

#include "statusbar/dsp/dsp.hpp"
#include "statusbar/status/status.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
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
// Time Mapping Types (for PTP <-> Monotonic conversion)
//

/// Time mapping state between PTP clock and local monotonic clock
/// The mapping is: ptp_ns = rate * monotonic_ns + offset_ns
struct TimeMapping
{
    double rate = 1.0;             ///< PTP rate relative to monotonic (slope)
    int64_t offset_ns = 0;         ///< Offset in nanoseconds (intercept) - int64_t for precision
    uint64_t epoch = 0;            ///< Incremented on discontinuities/resets
    bool healthy = false;          ///< True if mapping quality is acceptable
    int64_t rms_residual_ns = 0;   ///< RMS of fit residuals in nanoseconds
    int64_t worst_bracket_ns = 0;  ///< Worst sample bracket time in nanoseconds
    int sample_count = 0;          ///< Number of samples in current window
};

/// Result of time conversion between PTP and monotonic clocks
struct TimeConvertResult
{
    int64_t time_ns = 0;   ///< Converted time in nanoseconds
    uint64_t epoch = 0;    ///< Epoch at time of conversion
    bool healthy = false;  ///< Whether mapping was healthy
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
    int64_t wake_refine_spin_ns = 50'000;   ///< Final spin refinement for sleep (50us, 0 to disable)
    int min_samples_for_healthy = 16;       ///< Minimum samples before declaring healthy

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
    double rms_residual = 0;   ///< RMS of residuals
    double max_residual = 0;   ///< Maximum absolute residual
    bool valid = false;        ///< Whether fit was successful
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

/// Perform least-squares linear regression: ptp = slope * monotonic + intercept
/// @param samples Array of time samples
/// @param count Number of samples
/// @param max_rate_ppm Maximum allowed rate deviation from 1.0 in ppm (clamped)
/// @return Fit result with slope, intercept, and residual statistics
///
/// Uses centered coordinates to avoid numerical precision issues with large
/// nanosecond timestamps (e.g., 10^14 ns values would overflow when squared).
[[nodiscard]] auto fit_time_mapping(TimeSample const* samples, size_t count, double max_rate_ppm) noexcept -> LinearFitResult;

/// Fit linear mapping from an indexable container of TimeSample
/// @tparam Container Type with operator[] and size() returning TimeSample
/// @param samples Container of time samples (e.g., SampleWindow)
/// @param max_rate_ppm Maximum allowed rate deviation from 1.0 in ppm (clamped)
/// @return Fit result with slope, intercept, and residual statistics
template <typename Container>
[[nodiscard]] auto fit_time_mapping(Container const& samples, double max_rate_ppm) noexcept -> LinearFitResult
{
    LinearFitResult result;

    size_t const count = samples.size();
    if (count < 2) {
        return result;
    }

    double const n = static_cast<double>(count);

    // First pass: compute means for centering (numerical stability)
    double sum_m = 0, sum_p = 0;
    for (size_t i = 0; i < count; ++i) {
        sum_m += static_cast<double>(samples[i].monotonic_ns);
        sum_p += static_cast<double>(samples[i].ptp_ns);
    }
    double const mean_m = sum_m / n;
    double const mean_p = sum_p / n;

    // Second pass: compute centered sums for least squares
    double sum_mm_centered = 0, sum_mp_centered = 0;
    for (size_t i = 0; i < count; ++i) {
        double const m_centered = static_cast<double>(samples[i].monotonic_ns) - mean_m;
        double const p_centered = static_cast<double>(samples[i].ptp_ns) - mean_p;
        sum_mm_centered += m_centered * m_centered;
        sum_mp_centered += m_centered * p_centered;
    }

    if (sum_mm_centered == 0.0 || std::abs(sum_mm_centered) < 1e-9) {
        return result;
    }

    // Best-fit slope (UNCLAMPED). Fit quality (residual, below) is judged against
    // this line: a clean but steep slope — e.g. CLOCK_MONOTONIC being slewed by
    // phc2sys while the bridge tracks it — is a *good* fit and must read as
    // healthy, not be pinned to ±max_rate_ppm and then measured against the wrong
    // line (which would manufacture a huge residual and a false "unhealthy").
    // slope = sum((m - mean_m)(p - mean_p)) / sum((m - mean_m)^2)
    double const slope_fit = sum_mp_centered / sum_mm_centered;

    // The PUBLISHED slope is clamped to a plausible window only as a safety bound
    // on scheduling (a degenerate fit must not be able to throw a wake an
    // arbitrary distance into the future). The clamp does NOT affect the residual
    // / health computed below. Callers that legitimately expect a large slope
    // (e.g. the RAW<->MONOTONIC tracking fit during a phc2sys slew) pass a wide
    // max_rate_ppm so the real slope passes through unclamped.
    double const max_ppm = std::max(10.0, max_rate_ppm);
    double const lo = 1.0 - (max_ppm * 1e-6);
    double const hi = 1.0 + (max_ppm * 1e-6);
    double slope_pub = slope_fit;
    if (slope_pub < lo) {
        slope_pub = lo;
    } else if (slope_pub > hi) {
        slope_pub = hi;
    }

    // intercept = mean_p - slope_pub * mean_m  (uses the published slope so the
    // (slope_pub, intercept) pair is a self-consistent conversion line).
    // Use __int128 for precision with large timestamps (~10^18 ns)
    auto const slope_fp = FixedPointSlope::from_double(slope_pub);
    __int128 const mean_m_int = static_cast<__int128>(std::llround(mean_m));
    __int128 const mean_p_int = static_cast<__int128>(std::llround(mean_p));
    __int128 const slope_mean_m = (mean_m_int * slope_fp.numerator) / slope_fp.denominator;
    int64_t const intercept_ns = static_cast<int64_t>(mean_p_int - slope_mean_m);

    // Residuals against the UNCLAMPED best-fit slope = true fit quality.
    double rss = 0;
    double max_abs = 0;
    for (size_t i = 0; i < count; ++i) {
        double const m_centered = static_cast<double>(samples[i].monotonic_ns) - mean_m;
        double const p_centered = static_cast<double>(samples[i].ptp_ns) - mean_p;
        double const residual = p_centered - (slope_fit * m_centered);
        rss += residual * residual;
        double const abs_resid = std::abs(residual);
        if (abs_resid > max_abs) {
            max_abs = abs_resid;
        }
    }

    result.slope = slope_pub;
    result.intercept_ns = intercept_ns;
    result.rms_residual = std::sqrt(rss / n);
    result.max_residual = max_abs;
    result.valid = true;

    return result;
}

/// Convert PTP time to monotonic time using a linear mapping
/// @param ptp_ns PTP time in nanoseconds
/// @param slope Mapping slope (rate)
/// @param intercept Mapping intercept (offset)
/// @return Monotonic time in nanoseconds
[[nodiscard]] constexpr auto ptp_to_monotonic(int64_t ptp_ns, double slope, double intercept) noexcept
{
    // ptp = slope * mono + intercept
    // mono = (ptp - intercept) / slope
    return dsp::lround((static_cast<double>(ptp_ns) - intercept) / slope);
}

/// Convert monotonic time to PTP time using a linear mapping
/// @param monotonic_ns Monotonic time in nanoseconds
/// @param slope Mapping slope (rate)
/// @param intercept Mapping intercept (offset)
/// @return PTP time in nanoseconds
[[nodiscard]] constexpr auto monotonic_to_ptp(int64_t monotonic_ns, double slope, double intercept) noexcept
{
    // ptp = slope * mono + intercept
    return dsp::lround((slope * static_cast<double>(monotonic_ns)) + intercept);
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
