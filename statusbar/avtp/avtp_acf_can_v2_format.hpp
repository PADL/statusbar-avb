#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF_CAN_V2 message. Split from
/// avtp_acf_can_v2.hpp so consumers that only need the data structures do not
/// pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_can_v2.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfCanV2MessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfCanV2MessageView const& view) -> OutputIt
{
    out = std::format_to(
        out,
        "ACF_CAN_V2 pad={} mtv={:d} rtr={:d} eff={:d} can_bus_id={} brs={:d} fdf={:d} esi={:d} can_identifier=0x{:x}",
        view.fixed.pad(),
        view.fixed.mtv(),
        view.fixed.rtr(),
        view.fixed.eff(),
        view.fixed.can_bus_id(),
        view.fixed.brs(),
        view.fixed.fdf(),
        view.fixed.esi(),
        view.fixed.can_identifier());
    if (view.fixed.mtv()) {
        out = std::format_to(out, " message_timestamp={}", view.fixed.get_message_timestamp());
    }
    return std::format_to(out, " payload={} octets", view.payload.size());
}

}  // namespace statusbar::avtp
