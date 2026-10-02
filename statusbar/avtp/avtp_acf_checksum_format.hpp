#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF Checksum message. Split from
/// avtp_acf_checksum.hpp so consumers that only need the data structures
/// do not pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_checksum.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfChecksumMessage to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfChecksumMessage const& message) -> OutputIt
{
    return std::format_to(out, "ACF_CHECKSUM checksum=0x{:04x}", message.get_checksum());
}

}  // namespace statusbar::avtp
