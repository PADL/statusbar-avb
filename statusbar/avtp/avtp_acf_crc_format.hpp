#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF CRC message. Split from
/// avtp_acf_crc.hpp so consumers that only need the data structures do not
/// pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_crc.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfCrcMessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfCrcMessageView const& view) -> OutputIt
{
    out = std::format_to(out, "ACF_CRC crc_type={}", acf_crc_type_name(view.fixed.crc_type()));
    auto const value = view.crc32();
    if (value.has_value()) {
        return std::format_to(out, " crc_data=0x{:08x}", *value);
    }
    return std::format_to(out, " crc_data={} octets", view.crc_data.size());
}

}  // namespace statusbar::avtp
