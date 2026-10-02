#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF_CAN_XL_BRIEF message. Split from
/// avtp_acf_can_xl_brief.hpp so consumers that only need the data structures do not
/// pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_can_xl_brief.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfCanXlBriefMessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfCanXlBriefMessageView const& view) -> OutputIt
{
    out = std::format_to(
        out,
        "ACF_CAN_XL_BRIEF pad={} mtv={:d} can_bus_id={} vcid=0x{:x} sdt=0x{:x} rrs={:d} sec={:d} priority_id=0x{:x} "
        "acceptance_field=0x{:x} transaction_num=0x{:x} ms={:d} segment_num={}",
        view.fixed.pad(),
        view.fixed.mtv(),
        view.fixed.can_bus_id(),
        view.fixed.get_vcid(),
        view.fixed.get_sdt(),
        view.fixed.rrs(),
        view.fixed.sec(),
        view.fixed.priority_id(),
        view.fixed.get_acceptance_field(),
        view.fixed.get_transaction_num(),
        view.fixed.ms(),
        view.fixed.segment_num());
    return std::format_to(out, " payload={} octets", view.payload.size());
}

}  // namespace statusbar::avtp
