// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_crc.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/checksum/checksum_crc32.hpp"

namespace statusbar::avtp {

using ieee::quadlet_t;
using statusbar::make_const_span;
using statusbar::span_copy;
using statusbar::span_load;

auto acf_crc_type_name(AcfCrcType const type) noexcept -> std::string_view
{
    switch (type) {
        case AcfCrcType::crc_eth:
            return "CRC_ETH";
        case AcfCrcType::crc_32p4:
            return "CRC_32P4";
        case AcfCrcType::crc_user:
            return "CRC_USER";
        default:
            break;
    }
    return "CRC_RESERVED";
}

auto AcfCrcMessageView::crc32() const noexcept -> std::optional<uint32_t>
{
    if (crc_data.size() != sizeof(quadlet_t)) {
        return std::nullopt;
    }
    quadlet_t value{};
    span_load(value, crc_data);
    return value.get();
}

auto acf_crc_compute(AcfCrcType const type, std::span<uint8_t const> const preceding_message) noexcept -> std::optional<uint32_t>
{
    switch (type) {
        case AcfCrcType::crc_eth:
            return checksum::crc32_ethernet(preceding_message);
        case AcfCrcType::crc_32p4:
            return checksum::crc32_p4(preceding_message);
        default:
            return std::nullopt;
    }
}

auto acf_crc_parse(std::span<uint8_t const> const data) noexcept -> std::optional<AcfCrcMessageView>
{
    if (data.size() < AcfCrcMessage::LENGTH) {
        return std::nullopt;
    }
    AcfCrcMessage fixed{};
    span_load(fixed, data);
    if (!fixed.is_valid()) {
        return std::nullopt;
    }
    auto const total = fixed.header.msg_length_octets();
    if (total > data.size()) {
        return std::nullopt;
    }
    return AcfCrcMessageView{.fixed = fixed, .crc_data = data.subspan(AcfCrcMessage::LENGTH, total - AcfCrcMessage::LENGTH)};
}

auto acf_crc_build(std::span<uint8_t> const out, AcfCrcType const type, std::span<uint8_t const> const preceding_message) noexcept
    -> bool
{
    constexpr size_t MESSAGE_LENGTH = AcfCrcMessage::LENGTH + sizeof(quadlet_t);
    if (out.size() < MESSAGE_LENGTH) {
        return false;
    }
    auto const crc = acf_crc_compute(type, preceding_message);
    if (!crc.has_value()) {
        return false;
    }
    AcfCrcMessage fixed{};
    fixed.init(type);
    span_copy(out.first(AcfCrcMessage::LENGTH), make_const_span(fixed));
    quadlet_t const value{*crc};
    span_copy(out.subspan(AcfCrcMessage::LENGTH, sizeof(quadlet_t)), make_const_span(value));
    return true;
}

auto acf_crc_verify(AcfCrcMessageView const& view, std::span<uint8_t const> const preceding_message) noexcept -> std::optional<bool>
{
    auto const expected = acf_crc_compute(view.fixed.crc_type(), preceding_message);
    auto const actual = view.crc32();
    if (!expected.has_value() || !actual.has_value()) {
        return std::nullopt;
    }
    return *actual == *expected;
}

}  // namespace statusbar::avtp
