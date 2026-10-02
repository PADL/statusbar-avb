#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF_MOST message. Split from
/// avtp_acf_most.hpp so consumers that only need the data structures do not
/// pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_most.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfMostMessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfMostMessageView const& view) -> OutputIt
{
    out = std::format_to(
        out,
        "ACF_MOST pad={} mtv={:d} most_net_id={} device_id=0x{:x} fblock_id=0x{:x} inst_id=0x{:x} func_id=0x{:x} op_type=0x{:x}",
        view.fixed.pad(),
        view.fixed.mtv(),
        view.fixed.most_net_id(),
        view.fixed.get_device_id(),
        view.fixed.get_fblock_id(),
        view.fixed.get_inst_id(),
        view.fixed.func_id(),
        view.fixed.op_type());
    if (view.fixed.mtv()) {
        out = std::format_to(out, " message_timestamp={}", view.fixed.get_message_timestamp());
    }
    return std::format_to(out, " payload={} octets", view.payload.size());
}

}  // namespace statusbar::avtp
