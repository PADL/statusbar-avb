// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ptpclient/ptpclient_base.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

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
    return fit_time_mapping(std::span<TimeSample const>{samples, count}, max_rate_ppm);
}

}  // namespace statusbar::ptpclient
