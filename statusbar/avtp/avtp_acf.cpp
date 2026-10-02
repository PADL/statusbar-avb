// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf.hpp"

#include "statusbar/avtp/avtp_acf_checksum.hpp"
#include "statusbar/avtp/avtp_acf_crc.hpp"
#include "statusbar/buffer/span_utils.hpp"

namespace statusbar::avtp {

using statusbar::make_const_span;
using statusbar::span_copy;
using statusbar::span_load;

auto acf_msg_type_name(uint8_t const msg_type) noexcept -> std::string_view
{
    switch (msg_type) {
        case AcfMsgType::flexray:
            return "ACF_FLEXRAY";
        case AcfMsgType::can:
            return "ACF_CAN";
        case AcfMsgType::can_brief:
            return "ACF_CAN_BRIEF";
        case AcfMsgType::lin:
            return "ACF_LIN";
        case AcfMsgType::most:
            return "ACF_MOST";
        case AcfMsgType::gpc:
            return "ACF_GPC";
        case AcfMsgType::serial:
            return "ACF_SERIAL";
        case AcfMsgType::parallel:
            return "ACF_PARALLEL";
        case AcfMsgType::sensor:
            return "ACF_SENSOR";
        case AcfMsgType::sensor_brief:
            return "ACF_SENSOR_BRIEF";
        case AcfMsgType::aecp:
            return "ACF_AECP";
        case AcfMsgType::ancillary:
            return "ACF_ANCILLARY";
        case AcfMsgType::gisf:
            return "ACF_GISF";
        case AcfMsgType::byte_bus:
            return "ACF_BYTE_BUS";
        case AcfMsgType::byte_bus_brief:
            return "ACF_BYTE_BUS_BRIEF";
        case AcfMsgType::i2c:
            return "ACF_I2C";
        case AcfMsgType::i2c_brief:
            return "ACF_I2C_BRIEF";
        case AcfMsgType::can_xl:
            return "ACF_CAN_XL";
        case AcfMsgType::can_xl_brief:
            return "ACF_CAN_XL_BRIEF";
        case AcfMsgType::can_v2:
            return "ACF_CAN_V2";
        case AcfMsgType::can_brief_v2:
            return "ACF_CAN_BRIEF_V2";
        case AcfMsgType::lin_v2:
            return "ACF_LIN_V2";
        case AcfMsgType::checksum:
            return "ACF_CHECKSUM";
        case AcfMsgType::crc:
            return "ACF_CRC";
        default:
            break;
    }
    if (msg_type >= AcfMsgType::user_first && msg_type <= AcfMsgType::user_last) {
        return "ACF_USER";
    }
    return "ACF_RESERVED";
}

auto acf_trailer_status_name(AcfTrailerStatus const status) noexcept -> std::string_view
{
    switch (status) {
        case AcfTrailerStatus::none:
            return "none";
        case AcfTrailerStatus::checksum_ok:
            return "checksum_ok";
        case AcfTrailerStatus::checksum_bad:
            return "checksum_bad";
        case AcfTrailerStatus::crc_ok:
            return "crc_ok";
        case AcfTrailerStatus::crc_bad:
            return "crc_bad";
        case AcfTrailerStatus::crc_unsupported:
            return "crc_unsupported";
        case AcfTrailerStatus::trailer_malformed:
            return "trailer_malformed";
    }
    return "unknown";
}

auto acf_parse_message(std::span<uint8_t const> const data) noexcept -> std::optional<AcfMessageView>
{
    if (data.size() < AcfMessageHeader::LENGTH) {
        return std::nullopt;
    }
    AcfMessageHeader header{};
    span_load(header, data);
    if (!header.is_valid()) {
        return std::nullopt;
    }
    auto const octets = header.msg_length_octets();
    if (octets > data.size()) {
        return std::nullopt;
    }
    return AcfMessageView{.header = header, .message = data.first(octets)};
}

auto acf_store_header(std::span<uint8_t> const out, uint8_t const msg_type, uint16_t const msg_length_quadlets) noexcept -> bool
{
    if (out.size() < AcfMessageHeader::LENGTH || msg_length_quadlets < 1U || msg_length_quadlets > ACF_MSG_LENGTH_MAX_QUADLETS ||
        msg_type > AcfMsgType::user_last) {
        return false;
    }
    AcfMessageHeader header{};
    header.init(msg_type, msg_length_quadlets);
    span_copy(out.first(AcfMessageHeader::LENGTH), make_const_span(header));
    return true;
}

auto AcfMessageWalker::next() noexcept -> std::optional<AcfMessageView>
{
    if (remaining_.empty() || malformed_) {
        return std::nullopt;
    }
    auto const view = acf_parse_message(remaining_);
    if (!view.has_value()) {
        malformed_ = true;
        return std::nullopt;
    }
    remaining_ = remaining_.subspan(view->message.size());
    return view;
}

auto AcfMessageWalker::next_verified() noexcept -> std::optional<AcfVerifiedMessage>
{
    auto const message = next();
    if (!message.has_value()) {
        return std::nullopt;
    }
    AcfVerifiedMessage result{.message = *message, .trailer = AcfTrailerStatus::none};
    if (remaining_.empty()) {
        return result;
    }
    // Peek at the following message; only a well-formed ACF_CHECKSUM /
    // ACF_CRC is a trailer. Anything else - including a malformed length,
    // which the next call reports - stays for the next call.
    auto const following = acf_parse_message(remaining_);
    if (!following.has_value()) {
        return result;
    }
    if (following->msg_type() == AcfMsgType::checksum) {
        remaining_ = remaining_.subspan(following->message.size());
        auto const trailer = acf_checksum_parse(following->message);
        if (!trailer.has_value()) {
            result.trailer = AcfTrailerStatus::trailer_malformed;
        } else {
            result.trailer =
                acf_checksum_verify(*trailer, message->message) ? AcfTrailerStatus::checksum_ok : AcfTrailerStatus::checksum_bad;
        }
    } else if (following->msg_type() == AcfMsgType::crc) {
        remaining_ = remaining_.subspan(following->message.size());
        auto const trailer = acf_crc_parse(following->message);
        if (!trailer.has_value()) {
            result.trailer = AcfTrailerStatus::trailer_malformed;
        } else {
            auto const verdict = acf_crc_verify(*trailer, message->message);
            if (!verdict.has_value()) {
                result.trailer = AcfTrailerStatus::crc_unsupported;
            } else {
                result.trailer = *verdict ? AcfTrailerStatus::crc_ok : AcfTrailerStatus::crc_bad;
            }
        }
    }
    return result;
}

}  // namespace statusbar::avtp
