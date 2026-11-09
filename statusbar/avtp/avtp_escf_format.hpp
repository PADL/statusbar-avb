#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overloads for AVTP EscfPdu. Split from avtp_escf.hpp
/// so consumers that only need the data structures do not pay the
/// compile-time cost of <format>.

#include "statusbar/avtp/avtp_escf.hpp"
#include "statusbar/ieee/ieee_ethernet_format.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an EscfPdu to an output iterator
/// @param out The output iterator to write formatted text to
/// @param pdu The ESCF PDU to format
template <typename OutputIt>
auto format_to(OutputIt out, EscfPdu const& pdu) -> OutputIt
{
    out = std::format_to(out, "ESCF: sig={}", escf_sig_mode_name(pdu.sig()));
    out = std::format_to(out, " key_id=");
    out = ieee::format_to(out, pdu.key_id());
    out = std::format_to(out, " encapsulated_avtpdu_length={}", pdu.encapsulated_avtpdu_length());
    out = std::format_to(out, " signing_timestamp={}", pdu.signing_timestamp());
    return out;
}

}  // namespace statusbar::avtp
