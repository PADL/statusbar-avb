// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_tscf_v1.hpp"

#include "statusbar/buffer/span_utils.hpp"

namespace statusbar::avtp {

using statusbar::span_load;

auto tscf_v1_parse_header(std::span<uint8_t const> const packet) noexcept -> std::optional<TscfV1Pdu>
{
    if (packet.size() < TscfV1Pdu::HEADER_LENGTH) {
        return std::nullopt;
    }

    TscfV1Pdu pdu{};
    span_load(pdu, packet);

    if (!pdu.is_valid()) {
        return std::nullopt;
    }

    return pdu;
}

auto tscf_v1_get_acf_payload(std::span<uint8_t const> const packet) noexcept -> std::span<uint8_t const>
{
    if (packet.size() <= TscfV1Pdu::HEADER_LENGTH) {
        return {};
    }
    size_t const available = packet.size() - TscfV1Pdu::HEADER_LENGTH;
    // Bound to the declared length, not the raw buffer extent (which on the wire
    // includes Ethernet min-frame padding). Clamp so we never over-read.
    auto const pdu = tscf_v1_parse_header(packet);
    size_t const declared = pdu.has_value() ? pdu->get_stream_data_length() : available;
    return packet.subspan(TscfV1Pdu::HEADER_LENGTH, declared < available ? declared : available);
}

}  // namespace statusbar::avtp
