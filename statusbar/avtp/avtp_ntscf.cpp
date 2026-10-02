// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_ntscf.hpp"

#include "statusbar/buffer/span_utils.hpp"

namespace statusbar::avtp {

using statusbar::span_load;

auto ntscf_parse_header(std::span<uint8_t const> const packet) noexcept -> std::optional<NtscfPdu>
{
    if (packet.size() < NtscfPdu::HEADER_LENGTH) {
        return std::nullopt;
    }

    NtscfPdu pdu{};
    span_load(pdu, packet);

    if (!pdu.is_valid()) {
        return std::nullopt;
    }

    return pdu;
}

auto ntscf_get_acf_payload(std::span<uint8_t const> const packet) noexcept -> std::span<uint8_t const>
{
    if (packet.size() <= NtscfPdu::HEADER_LENGTH) {
        return {};
    }
    size_t const available = packet.size() - NtscfPdu::HEADER_LENGTH;
    // Bound to the declared length, not the raw buffer extent (which on the wire
    // includes Ethernet min-frame padding). Clamp so we never over-read.
    auto const pdu = ntscf_parse_header(packet);
    size_t const declared = pdu.has_value() ? pdu->ntscf_data_length() : available;
    return packet.subspan(NtscfPdu::HEADER_LENGTH, declared < available ? declared : available);
}

}  // namespace statusbar::avtp
