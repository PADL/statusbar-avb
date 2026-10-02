// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_checksum.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/checksum/checksum_ones_complement.hpp"

namespace statusbar::avtp {

using statusbar::make_const_span;
using statusbar::span_copy;
using statusbar::span_load;

auto acf_checksum_compute(std::span<uint8_t const> const preceding_message) noexcept -> uint16_t
{
    return checksum::internet_checksum16(preceding_message);
}

auto acf_checksum_parse(std::span<uint8_t const> const data) noexcept -> std::optional<AcfChecksumMessage>
{
    if (data.size() < AcfChecksumMessage::LENGTH) {
        return std::nullopt;
    }
    AcfChecksumMessage message{};
    span_load(message, data);
    if (!message.is_valid()) {
        return std::nullopt;
    }
    return message;
}

auto acf_checksum_build(std::span<uint8_t> const out, std::span<uint8_t const> const preceding_message) noexcept -> bool
{
    if (out.size() < AcfChecksumMessage::LENGTH) {
        return false;
    }
    AcfChecksumMessage message{};
    message.init(acf_checksum_compute(preceding_message));
    span_copy(out.first(AcfChecksumMessage::LENGTH), make_const_span(message));
    return true;
}

auto acf_checksum_verify(AcfChecksumMessage const& message, std::span<uint8_t const> const preceding_message) noexcept -> bool
{
    return message.get_checksum() == acf_checksum_compute(preceding_message);
}

}  // namespace statusbar::avtp
