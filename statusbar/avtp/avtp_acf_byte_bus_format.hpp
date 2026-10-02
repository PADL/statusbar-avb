#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF_BYTE_BUS message. Split from
/// avtp_acf_byte_bus.hpp so consumers that only need the data structures do not
/// pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_byte_bus.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfByteBusMessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfByteBusMessageView const& view) -> OutputIt
{
    out = std::format_to(
        out,
        "ACF_BYTE_BUS pad={} mtv={:d} byte_bus_id={} evt={} hs={:d} cs={:d} transaction_num=0x{:x} op={:d} rsp={:d} err={:d} "
        "ms={:d} read_size_segment_num=0x{:x}",
        view.fixed.pad(),
        view.fixed.mtv(),
        view.fixed.byte_bus_id(),
        view.fixed.evt(),
        view.fixed.hs(),
        view.fixed.cs(),
        view.fixed.transaction_num(),
        view.fixed.op(),
        view.fixed.rsp(),
        view.fixed.err(),
        view.fixed.ms(),
        view.fixed.read_size_segment_num());
    if (view.fixed.mtv()) {
        out = std::format_to(out, " message_timestamp={}", view.fixed.get_message_timestamp());
    }
    return std::format_to(out, " payload={} octets", view.payload.size());
}

}  // namespace statusbar::avtp
