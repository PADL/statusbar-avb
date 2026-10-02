#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF dispatcher - format_acf_payload() walks the ACF messages of an
/// NTSCF/TSCF acf_payload_data and routes each to its type's format_to()
/// (IEEE 1722-2025 Clause 9.4). Split from avtp_acf_format.hpp so consumers
/// that only need one message type do not pull every type's formatter.

#include "statusbar/avtp/avtp_acf.hpp"
#include "statusbar/avtp/avtp_acf_aecp.hpp"
#include "statusbar/avtp/avtp_acf_aecp_format.hpp"
#include "statusbar/avtp/avtp_acf_ancillary.hpp"
#include "statusbar/avtp/avtp_acf_ancillary_format.hpp"
#include "statusbar/avtp/avtp_acf_byte_bus.hpp"
#include "statusbar/avtp/avtp_acf_byte_bus_brief.hpp"
#include "statusbar/avtp/avtp_acf_byte_bus_brief_format.hpp"
#include "statusbar/avtp/avtp_acf_byte_bus_format.hpp"
#include "statusbar/avtp/avtp_acf_can.hpp"
#include "statusbar/avtp/avtp_acf_can_brief.hpp"
#include "statusbar/avtp/avtp_acf_can_brief_format.hpp"
#include "statusbar/avtp/avtp_acf_can_brief_v2.hpp"
#include "statusbar/avtp/avtp_acf_can_brief_v2_format.hpp"
#include "statusbar/avtp/avtp_acf_can_format.hpp"
#include "statusbar/avtp/avtp_acf_can_v2.hpp"
#include "statusbar/avtp/avtp_acf_can_v2_format.hpp"
#include "statusbar/avtp/avtp_acf_can_xl.hpp"
#include "statusbar/avtp/avtp_acf_can_xl_brief.hpp"
#include "statusbar/avtp/avtp_acf_can_xl_brief_format.hpp"
#include "statusbar/avtp/avtp_acf_can_xl_format.hpp"
#include "statusbar/avtp/avtp_acf_checksum.hpp"
#include "statusbar/avtp/avtp_acf_checksum_format.hpp"
#include "statusbar/avtp/avtp_acf_crc.hpp"
#include "statusbar/avtp/avtp_acf_crc_format.hpp"
#include "statusbar/avtp/avtp_acf_flexray.hpp"
#include "statusbar/avtp/avtp_acf_flexray_format.hpp"
#include "statusbar/avtp/avtp_acf_format.hpp"
#include "statusbar/avtp/avtp_acf_gpc.hpp"
#include "statusbar/avtp/avtp_acf_gpc_format.hpp"
#include "statusbar/avtp/avtp_acf_i2c.hpp"
#include "statusbar/avtp/avtp_acf_i2c_brief.hpp"
#include "statusbar/avtp/avtp_acf_i2c_brief_format.hpp"
#include "statusbar/avtp/avtp_acf_i2c_format.hpp"
#include "statusbar/avtp/avtp_acf_lin.hpp"
#include "statusbar/avtp/avtp_acf_lin_format.hpp"
#include "statusbar/avtp/avtp_acf_lin_v2.hpp"
#include "statusbar/avtp/avtp_acf_lin_v2_format.hpp"
#include "statusbar/avtp/avtp_acf_most.hpp"
#include "statusbar/avtp/avtp_acf_most_format.hpp"
#include "statusbar/avtp/avtp_acf_parallel.hpp"
#include "statusbar/avtp/avtp_acf_parallel_format.hpp"
#include "statusbar/avtp/avtp_acf_sensor.hpp"
#include "statusbar/avtp/avtp_acf_sensor_brief.hpp"
#include "statusbar/avtp/avtp_acf_sensor_brief_format.hpp"
#include "statusbar/avtp/avtp_acf_sensor_format.hpp"
#include "statusbar/avtp/avtp_acf_serial.hpp"
#include "statusbar/avtp/avtp_acf_serial_format.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>

namespace statusbar::avtp {

namespace detail {

/// Octets of an unknown message shown in hex before truncating
constexpr size_t ACF_FORMAT_HEX_LIMIT = 32;

/// Parse @p message as @p Parse's type and format it, or report it invalid
template <typename OutputIt, typename ParseFn, typename FormatFn>
auto format_acf_typed(OutputIt out, AcfMessageView const& message, ParseFn parse, FormatFn format) -> OutputIt
{
    auto const view = parse(message.message);
    if (!view.has_value()) {
        out = format_to(out, message.header);
        return std::format_to(out, " (invalid for its type)");
    }
    return format(out, *view);
}

/// Format one ACF message by its type
template <typename OutputIt>
auto format_acf_message(OutputIt out, AcfMessageView const& message) -> OutputIt
{
    auto const typed = [&](auto parse) {
        return format_acf_typed(out, message, parse, [](OutputIt o, auto const& v) { return format_to(o, v); });
    };
    switch (message.msg_type()) {
        case AcfMsgType::flexray:
            return typed(acf_flexray_parse);
        case AcfMsgType::can:
            return typed(acf_can_parse);
        case AcfMsgType::can_v2:
            return typed(acf_can_v2_parse);
        case AcfMsgType::can_brief:
            return typed(acf_can_brief_parse);
        case AcfMsgType::can_brief_v2:
            return typed(acf_can_brief_v2_parse);
        case AcfMsgType::lin:
            return typed(acf_lin_parse);
        case AcfMsgType::lin_v2:
            return typed(acf_lin_v2_parse);
        case AcfMsgType::most:
            return typed(acf_most_parse);
        case AcfMsgType::gpc:
            return typed(acf_gpc_parse);
        case AcfMsgType::serial:
            return typed(acf_serial_parse);
        case AcfMsgType::parallel:
            return typed(acf_parallel_parse);
        case AcfMsgType::sensor:
            return typed(acf_sensor_parse);
        case AcfMsgType::sensor_brief:
            return typed(acf_sensor_brief_parse);
        case AcfMsgType::aecp:
            return typed(acf_aecp_parse);
        case AcfMsgType::ancillary:
            return typed(acf_ancillary_parse);
        case AcfMsgType::byte_bus:
            return typed(acf_byte_bus_parse);
        case AcfMsgType::byte_bus_brief:
            return typed(acf_byte_bus_brief_parse);
        case AcfMsgType::i2c:
            return typed(acf_i2c_parse);
        case AcfMsgType::i2c_brief:
            return typed(acf_i2c_brief_parse);
        case AcfMsgType::can_xl:
            return typed(acf_can_xl_parse);
        case AcfMsgType::can_xl_brief:
            return typed(acf_can_xl_brief_parse);
        case AcfMsgType::checksum:
            return typed(acf_checksum_parse);
        case AcfMsgType::crc:
            return typed(acf_crc_parse);
        default:
            break;
    }
    // GISF, reserved and user types: the header and the leading octets.
    out = format_to(out, message.header);
    auto const body = message.after_header();
    out = std::format_to(out, " data=");
    for (auto const octet : body.first(body.size() < ACF_FORMAT_HEX_LIMIT ? body.size() : ACF_FORMAT_HEX_LIMIT)) {
        out = std::format_to(out, "{:02x}", octet);
    }
    if (body.size() > ACF_FORMAT_HEX_LIMIT) {
        out = std::format_to(out, "...({} octets)", body.size());
    }
    return out;
}

}  // namespace detail

/// Format every ACF message of @p acf_payload_data (the span from
/// tscf_get_acf_payload / ntscf_get_acf_payload), one line each with the
/// given @p indent. With @p verify_trailers the Checksum/CRC messages are
/// consumed as trailers and their verdict is appended to the message they
/// cover; without it they are listed as messages like any other. A
/// malformed length ends the listing with a note.
template <typename OutputIt>
auto format_acf_payload(
    OutputIt out,
    std::span<uint8_t const> const acf_payload_data,
    bool const verify_trailers = false,
    std::string_view const indent = "       ") -> OutputIt
{
    AcfMessageWalker walker{acf_payload_data};
    while (true) {
        std::optional<AcfVerifiedMessage> message;
        if (verify_trailers) {
            message = walker.next_verified();
        } else if (auto const plain = walker.next(); plain.has_value()) {
            message = AcfVerifiedMessage{.message = *plain, .trailer = AcfTrailerStatus::none};
        }
        if (!message.has_value()) {
            break;
        }
        out = std::format_to(out, "{}", indent);
        out = detail::format_acf_message(out, message->message);
        if (message->trailer != AcfTrailerStatus::none) {
            out = std::format_to(out, " [{}]", acf_trailer_status_name(message->trailer));
        }
        out = std::format_to(out, "\n");
    }
    if (walker.malformed()) {
        out = std::format_to(out, "{}ACF malformed message length ({} octets unparsed)\n", indent, walker.remaining().size());
    }
    return out;
}

}  // namespace statusbar::avtp
