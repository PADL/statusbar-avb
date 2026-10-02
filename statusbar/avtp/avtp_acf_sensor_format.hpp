#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF_SENSOR message. Split from
/// avtp_acf_sensor.hpp so consumers that only need the data structures do not
/// pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_sensor.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfSensorMessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfSensorMessageView const& view) -> OutputIt
{
    out = std::format_to(
        out,
        "ACF_SENSOR mtv={:d} num_sensors={} sz={} sensor_group={}",
        view.fixed.mtv(),
        view.fixed.num_sensors(),
        view.fixed.sz(),
        view.fixed.sensor_group());
    if (view.fixed.mtv()) {
        out = std::format_to(out, " message_timestamp={}", view.fixed.get_message_timestamp());
    }
    return std::format_to(out, " payload={} octets", view.payload.size());
}

}  // namespace statusbar::avtp
