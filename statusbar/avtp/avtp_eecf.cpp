// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_eecf.hpp"

#include "statusbar/buffer/span_utils.hpp"

#include <string_view>

namespace statusbar::avtp {

using statusbar::span_load;

auto eecf_enc_mode_name(uint8_t const enc) noexcept -> std::string_view
{
    switch (enc) {
        case 0x00:
            return "ECC1";
        default:
            return "Reserved";
    }
}

auto eecf_parse_header(std::span<uint8_t const> const packet) noexcept -> std::optional<EecfPdu>
{
    if (packet.size() < EecfPdu::HEADER_LENGTH) {
        return std::nullopt;
    }

    EecfPdu pdu{};
    span_load(pdu, packet);

    if (!pdu.is_valid()) {
        return std::nullopt;
    }

    return pdu;
}

auto eecf_get_encrypted_payload(std::span<uint8_t const> const packet) noexcept -> std::span<uint8_t const>
{
    if (packet.size() <= EecfPdu::HEADER_LENGTH) {
        return {};
    }
    return packet.subspan(EecfPdu::HEADER_LENGTH);
}

}  // namespace statusbar::avtp
