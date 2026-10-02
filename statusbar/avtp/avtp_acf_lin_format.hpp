#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF_LIN message. Split from
/// avtp_acf_lin.hpp so consumers that only need the data structures do not
/// pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_lin.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfLinMessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfLinMessageView const& view) -> OutputIt
{
    out = std::format_to(
        out,
        "ACF_LIN pad={} mtv={:d} lin_bus_id={} lin_identifier=0x{:x}",
        view.fixed.pad(),
        view.fixed.mtv(),
        view.fixed.lin_bus_id(),
        view.fixed.get_lin_identifier());
    if (view.fixed.mtv()) {
        out = std::format_to(out, " message_timestamp={}", view.fixed.get_message_timestamp());
    }
    return std::format_to(out, " payload={} octets", view.payload.size());
}

}  // namespace statusbar::avtp
