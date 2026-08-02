// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_escf.hpp"

#include "statusbar/buffer/span_utils.hpp"

#include <string_view>

namespace statusbar::avtp {

using statusbar::span_load;

auto escf_sig_mode_name(uint8_t const sig) noexcept -> std::string_view
{
    switch (sig) {
        case 0x00:
            return "ECC1";
        default:
            return "Reserved";
    }
}

auto escf_parse_header(std::span<uint8_t const> const packet) noexcept -> std::optional<EscfPdu>
{
    if (packet.size() < EscfPdu::HEADER_LENGTH) {
        return std::nullopt;
    }

    EscfPdu pdu{};
    span_load(pdu, packet);

    if (!pdu.is_valid()) {
        return std::nullopt;
    }

    return pdu;
}

auto escf_get_signed_payload(std::span<uint8_t const> const packet) noexcept -> std::span<uint8_t const>
{
    if (packet.size() <= EscfPdu::HEADER_LENGTH) {
        return {};
    }
    return packet.subspan(EscfPdu::HEADER_LENGTH);
}

}  // namespace statusbar::avtp
