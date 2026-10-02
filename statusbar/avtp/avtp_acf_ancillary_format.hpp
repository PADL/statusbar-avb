#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF_ANCILLARY message. Split from
/// avtp_acf_ancillary.hpp so consumers that only need the data structures do not
/// pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_ancillary.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfAncillaryMessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfAncillaryMessageView const& view) -> OutputIt
{
    out = std::format_to(
        out,
        "ACF_ANCILLARY pad={} mode={} fp={:d} lp={:d} line_number={} did=0x{:x} sdid_dbn=0x{:x}",
        view.fixed.pad(),
        view.fixed.mode(),
        view.fixed.fp(),
        view.fixed.lp(),
        view.fixed.get_line_number(),
        view.fixed.get_did(),
        view.fixed.get_sdid_dbn());
    return std::format_to(out, " payload={} octets", view.payload.size());
}

}  // namespace statusbar::avtp
