// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ptpclient/ptpclient_base.hpp"

#include <cmath>
#include <cstdint>

namespace statusbar::ptpclient {

auto PtpErrorCategory::message(int ev) const -> std::string
{
    switch (static_cast<PtpError>(ev)) {
        case PtpError::device_not_found:
            return "PTP device not found";
        case PtpError::device_open_failed:
            return "Failed to open PTP device";
        case PtpError::device_not_open:
            return "PTP device not open";
        case PtpError::clock_gettime_failed:
            return "Failed to get PTP clock time";
        case PtpError::invalid_device_path:
            return "Invalid PTP device path";
        case PtpError::permission_denied:
            return "Permission denied accessing PTP device";
        case PtpError::not_supported:
            return "PTP not supported on this platform";
        case PtpError::bridge_not_running:
            return "PTP time bridge not running";
        case PtpError::bridge_not_healthy:
            return "PTP time bridge mapping not healthy";
        case PtpError::bridge_epoch_changed:
            return "PTP time bridge epoch changed during operation";
        case PtpError::invalid_period:
            return "PTP timer period must be positive";
        default:
            return "Unknown PTP error";
    }
}

auto fit_time_mapping(TimeSample const* samples, size_t count, double max_rate_ppm) noexcept -> LinearFitResult
{
    LinearFitResult result;

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
    // Using centered coordinates: m' = m - mean_m, p' = p - mean_p
    // This avoids precision loss from squaring large timestamps
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

    // Best-fit slope (UNCLAMPED) — see the header template overload for the
    // rationale: fit quality is judged against this line so a clean-but-steep
    // slope (CLOCK_MONOTONIC slewed by phc2sys) reads as healthy. The clamp below
    // bounds only the PUBLISHED slope, not the residual.
    // slope = sum((m - mean_m)(p - mean_p)) / sum((m - mean_m)^2)
    double const slope_fit = sum_mp_centered / sum_mm_centered;

    double const max_ppm = std::max(10.0, max_rate_ppm);
    double const lo = 1.0 - (max_ppm * 1e-6);
    double const hi = 1.0 + (max_ppm * 1e-6);
    double slope_pub = slope_fit;
    if (slope_pub < lo) {
        slope_pub = lo;
    } else if (slope_pub > hi) {
        slope_pub = hi;
    }

    // intercept = mean_p - slope_pub * mean_m  (matches the published slope)
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
        // residual = p' - slope_fit * m' (intercept cancels out in centered coords)
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

}  // namespace statusbar::ptpclient
