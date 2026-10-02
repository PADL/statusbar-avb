#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF_SERIAL message. Split from
/// avtp_acf_serial.hpp so consumers that only need the data structures do not
/// pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_serial.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfSerialMessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfSerialMessageView const& view) -> OutputIt
{
    out = std::format_to(
        out,
        "ACF_SERIAL pad={} dcd={:d} dtr={:d} dsr={:d} rts={:d} cts={:d} ri={:d}",
        view.fixed.pad(),
        view.fixed.dcd(),
        view.fixed.dtr(),
        view.fixed.dsr(),
        view.fixed.rts(),
        view.fixed.cts(),
        view.fixed.ri());
    return std::format_to(out, " payload={} octets", view.payload.size());
}

}  // namespace statusbar::avtp
