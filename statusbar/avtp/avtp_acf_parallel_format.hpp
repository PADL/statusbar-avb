#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF_PARALLEL message. Split from
/// avtp_acf_parallel.hpp so consumers that only need the data structures do not
/// pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_parallel.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfParallelMessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfParallelMessageView const& view) -> OutputIt
{
    out = std::format_to(out, "ACF_PARALLEL bit_width={}", view.fixed.get_bit_width());
    return std::format_to(out, " payload={} octets", view.payload.size());
}

}  // namespace statusbar::avtp
