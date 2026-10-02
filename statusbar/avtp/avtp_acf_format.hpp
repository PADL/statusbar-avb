#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overloads for the ACF common header, message view and
/// verified message. Split from avtp_acf.hpp so consumers that only need
/// the data structures do not pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfMessageHeader to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfMessageHeader const& header) -> OutputIt
{
    return std::format_to(
        out,
        "ACF msg_type={}(0x{:02x}) msg_length={}q",
        acf_msg_type_name(header.msg_type()),
        header.msg_type(),
        header.msg_length());
}

/// Format an AcfMessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfMessageView const& view) -> OutputIt
{
    out = format_to(out, view.header);
    return std::format_to(out, " octets={}", view.message.size());
}

/// Format an AcfVerifiedMessage to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfVerifiedMessage const& verified) -> OutputIt
{
    out = format_to(out, verified.message);
    return std::format_to(out, " trailer={}", acf_trailer_status_name(verified.trailer));
}

}  // namespace statusbar::avtp
