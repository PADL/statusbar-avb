// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_am824_v1.hpp"

#include "statusbar/buffer/span_utils.hpp"

namespace statusbar::avtp {

using statusbar::span_load;

auto am824_v1_parse_header(std::span<uint8_t const> const packet) noexcept -> std::optional<Am824V1Pdu>
{
    if (packet.size() < Am824V1Pdu::HEADER_LENGTH) {
        return std::nullopt;
    }

    Am824V1Pdu pdu{};
    span_load(pdu, packet);

    if (!pdu.is_valid()) {
        return std::nullopt;
    }

    return pdu;
}

auto am824_v1_get_audio_payload(std::span<uint8_t const> const packet) noexcept -> std::span<uint8_t const>
{
    if (packet.size() <= Am824V1Pdu::HEADER_LENGTH) {
        return {};
    }
    size_t const available = packet.size() - Am824V1Pdu::HEADER_LENGTH;
    // Bound to the declared length, not the raw buffer extent (which on the wire
    // includes Ethernet min-frame padding). Clamp so we never over-read.
    auto const pdu = am824_v1_parse_header(packet);
    size_t const declared = pdu.has_value() ? pdu->stream_data_length() : available;
    return packet.subspan(Am824V1Pdu::HEADER_LENGTH, declared < available ? declared : available);
}

}  // namespace statusbar::avtp
