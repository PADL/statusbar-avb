#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// PTP Client module for statusbar
/// Provides access to hardware PTP clocks for precision time synchronization
///
/// Supported drivers:
/// - "linuxptp": Linux PTP via /dev/ptp* devices (Linux only)
/// - "system": System clock fallback (all platforms) - uses steady_clock
///
/// Usage:
///   // Using factory function (cross-platform):
///   auto client = create_ptp_client("system");  // Works on all platforms
///   if (client) {
///       auto time_ns = (*client)->get_time_ns();
///       if (time_ns) {
///           // Use *time_ns (nanoseconds since epoch)
///       }
///   }
///
///   // On Linux with hardware PTP:
///   auto client = create_ptp_client("linuxptp", "/dev/ptp0");
///
///   // Using PTP Time Bridge:
///   auto client = create_ptp_client("system");
///   PtpTimeBridge bridge;
///   auto guard = bridge.start_sampling(**client);
///   // ... wait for healthy mapping ...
///   if (bridge.is_healthy()) {
///       auto result = bridge.convert_ptp_to_monotonic(ptp_ns);
///       bridge.sleep_until_ptp(deadline_ptp_ns);
///   }
///   // guard destructor stops sampling

#include "statusbar/ptpclient/ptpclient_base.hpp"
#include "statusbar/ptpclient/ptpclient_bridge.hpp"
#include "statusbar/ptpclient/ptpclient_linuxptp.hpp"
#include "statusbar/ptpclient/ptpclient_phc.hpp"
#include "statusbar/ptpclient/ptpclient_phc_servo.hpp"
#include "statusbar/ptpclient/ptpclient_setup.hpp"
#include "statusbar/ptpclient/ptpclient_timer.hpp"
#include "statusbar/status/status.hpp"

#include <memory>
#include <string_view>

namespace statusbar::ptpclient {

using statusbar::StatusValue;

/// Supported PTP driver names
inline constexpr std::string_view DRIVER_LINUXPTP = "linuxptp";

/// Create a PTP client for the specified driver
/// @param driver_name The driver to use (e.g., "linuxptp", "system")
/// @param device_path Path to the PTP device (e.g., "/dev/ptp0") or name for display
/// @return Unique pointer to PtpClientBase, or error if driver not supported
///
/// Supported drivers:
/// - "linuxptp": Linux PTP via /dev/ptp* devices (Linux only)
/// - "system": System clock fallback (all platforms) - uses steady_clock
[[nodiscard]] auto create_ptp_client(std::string_view driver_name, std::string_view device_path = default_ptp_device)
    -> StatusValue<std::unique_ptr<PtpClientBase>>;

}  // namespace statusbar::ptpclient
