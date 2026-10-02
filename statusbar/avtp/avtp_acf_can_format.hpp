#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF_CAN message. Split from
/// avtp_acf_can.hpp so consumers that only need the data structures do not
/// pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_can.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfCanMessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfCanMessageView const& view) -> OutputIt
{
    out = std::format_to(
        out,
        "ACF_CAN pad={} mtv={:d} rtr={:d} eff={:d} brs={:d} fdf={:d} esi={:d} can_bus_id={} can_identifier=0x{:x}",
        view.fixed.pad(),
        view.fixed.mtv(),
        view.fixed.rtr(),
        view.fixed.eff(),
        view.fixed.brs(),
        view.fixed.fdf(),
        view.fixed.esi(),
        view.fixed.can_bus_id(),
        view.fixed.can_identifier());
    if (view.fixed.mtv()) {
        out = std::format_to(out, " message_timestamp={}", view.fixed.get_message_timestamp());
    }
    return std::format_to(out, " payload={} octets", view.payload.size());
}

}  // namespace statusbar::avtp
