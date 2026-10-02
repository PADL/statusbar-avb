#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF_GPC message. Split from
/// avtp_acf_gpc.hpp so consumers that only need the data structures do not
/// pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_gpc.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfGpcMessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfGpcMessageView const& view) -> OutputIt
{
    out = std::format_to(out, "ACF_GPC gpc_msg_id=0x{:012x}", view.fixed.get_gpc_msg_id().to_uint64());
    return std::format_to(out, " payload={} octets", view.payload.size());
}

}  // namespace statusbar::avtp
