// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_aef.hpp"

#include "statusbar/buffer/span_utils.hpp"

#include <algorithm>
#include <string_view>

namespace statusbar::avtp {

using statusbar::span_load;

auto aef_enc_mode_name(uint8_t const enc) noexcept -> std::string_view
{
    switch (enc) {
        case 0x00:
            return "AES-SIV";
        case 0x01:
            return "AES-GCM-SIV";
        default:
            return "Reserved";
    }
}

auto aef_continuous_parse_header(std::span<uint8_t const> const packet) noexcept -> std::optional<AefContinuousPdu>
{
    if (packet.size() < AefContinuousPdu::HEADER_LENGTH) {
        return std::nullopt;
    }

    AefContinuousPdu pdu{};
    span_load(pdu, packet);

    if (!pdu.is_valid()) {
        return std::nullopt;
    }

    return pdu;
}

auto aef_discrete_parse_header(std::span<uint8_t const> const packet) noexcept -> std::optional<AefDiscretePdu>
{
    if (packet.size() < AefDiscretePdu::HEADER_LENGTH) {
        return std::nullopt;
    }

    AefDiscretePdu pdu{};
    span_load(pdu, packet);

    if (!pdu.is_valid()) {
        return std::nullopt;
    }

    return pdu;
}

auto aef_continuous_get_encrypted_payload(std::span<uint8_t const> const packet) noexcept -> std::span<uint8_t const>
{
    if (packet.size() <= AefContinuousPdu::HEADER_LENGTH) {
        return {};
    }
    size_t const available = packet.size() - AefContinuousPdu::HEADER_LENGTH;
    // Bound to the declared stream_data_length, not the raw buffer extent (which on
    // the wire carries Ethernet min-frame padding). Clamp so we never over-read.
    auto const pdu = aef_continuous_parse_header(packet);
    size_t const declared = pdu.has_value() ? pdu->get_stream_data_length() : available;
    return packet.subspan(AefContinuousPdu::HEADER_LENGTH, std::min(declared, available));
}

auto aef_discrete_get_encrypted_payload(std::span<uint8_t const> const packet) noexcept -> std::span<uint8_t const>
{
    if (packet.size() <= AefDiscretePdu::HEADER_LENGTH) {
        return {};
    }
    size_t const available = packet.size() - AefDiscretePdu::HEADER_LENGTH;
    // Bound to the declared control_data_length (discrete AEF's payload length), not
    // the raw buffer extent. Clamp so we never over-read.
    auto const pdu = aef_discrete_parse_header(packet);
    size_t const declared = pdu.has_value() ? pdu->control_data_length() : available;
    return packet.subspan(AefDiscretePdu::HEADER_LENGTH, std::min(declared, available));
}

}  // namespace statusbar::avtp
