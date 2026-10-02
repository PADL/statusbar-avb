#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF_SENSOR_BRIEF message. Split from
/// avtp_acf_sensor_brief.hpp so consumers that only need the data structures do not
/// pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_sensor_brief.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfSensorBriefMessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfSensorBriefMessageView const& view) -> OutputIt
{
    out = std::format_to(
        out,
        "ACF_SENSOR_BRIEF mtv={:d} num_sensors={} sz={} sensor_group={}",
        view.fixed.mtv(),
        view.fixed.num_sensors(),
        view.fixed.sz(),
        view.fixed.sensor_group());
    return std::format_to(out, " payload={} octets", view.payload.size());
}

}  // namespace statusbar::avtp
