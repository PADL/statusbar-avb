#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF_FLEXRAY message. Split from
/// avtp_acf_flexray.hpp so consumers that only need the data structures do not
/// pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_flexray.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfFlexrayMessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfFlexrayMessageView const& view) -> OutputIt
{
    out = std::format_to(
        out,
        "ACF_FLEXRAY pad={} mtv={:d} fr_bus_id={} chan={} str={:d} syn={:d} pre={:d} nfi={:d} fr_frame_id=0x{:x} cycle={}",
        view.fixed.pad(),
        view.fixed.mtv(),
        view.fixed.fr_bus_id(),
        view.fixed.chan(),
        view.fixed.str(),
        view.fixed.syn(),
        view.fixed.pre(),
        view.fixed.nfi(),
        view.fixed.fr_frame_id(),
        view.fixed.cycle());
    if (view.fixed.mtv()) {
        out = std::format_to(out, " message_timestamp={}", view.fixed.get_message_timestamp());
    }
    return std::format_to(out, " payload={} octets", view.payload.size());
}

}  // namespace statusbar::avtp
