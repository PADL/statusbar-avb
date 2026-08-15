// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_crf.hpp"

#include "statusbar/buffer/span_utils.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace statusbar::avtp {

using statusbar::span_load;

auto crf_parse_header(std::span<uint8_t const> const payload) noexcept -> std::optional<CrfPdu>
{
    if (payload.size() < CrfPdu::HEADER_LENGTH) {
        return std::nullopt;
    }
    CrfPdu pdu;
    span_load(pdu, payload);
    if (!pdu.is_valid()) {
        return std::nullopt;
    }
    return pdu;
}

auto crf_get_timestamp_data(std::span<uint8_t const> const payload) noexcept -> std::span<uint8_t const>
{
    if (payload.size() < CrfPdu::HEADER_LENGTH) {
        return {};
    }
    auto pdu = crf_parse_header(payload);
    if (!pdu) {
        return {};
    }
    size_t const expected_size = CrfPdu::HEADER_LENGTH + pdu->crf_data_length();
    if (payload.size() < expected_size) {
        return {};
    }
    return payload.subspan(CrfPdu::HEADER_LENGTH, pdu->crf_data_length());
}

auto crf_get_timestamp(std::span<uint8_t const> const timestamp_data, size_t const index) noexcept -> std::optional<uint64_t>
{
    size_t const offset = index * CrfPdu::TIMESTAMP_SIZE;
    if (offset + CrfPdu::TIMESTAMP_SIZE > timestamp_data.size()) {
        return std::nullopt;
    }
    // Timestamps are 64-bit big-endian values
    ieee::octlet_t value{};
    span_load(value, timestamp_data.subspan(offset, CrfPdu::TIMESTAMP_SIZE));
    return value.get();
}

auto crf_set_timestamp(std::span<uint8_t> const timestamp_data, size_t const index, uint64_t const timestamp) noexcept -> bool
{
    size_t const offset = index * CrfPdu::TIMESTAMP_SIZE;
    if (offset + CrfPdu::TIMESTAMP_SIZE > timestamp_data.size()) {
        return false;
    }
    // Store as 64-bit big-endian
    ieee::octlet_t const value{timestamp};
    statusbar::span_copy(timestamp_data.subspan(offset, CrfPdu::TIMESTAMP_SIZE), value.span());
    return true;
}

auto crf_type_name(uint8_t const type) noexcept -> std::string_view
{
    switch (static_cast<CrfType>(type)) {
        case CrfType::user:
            return "User";
        case CrfType::audio_sample:
            return "Audio Sample";
        case CrfType::video_frame:
            return "Video Frame";
        case CrfType::video_line:
            return "Video Line";
        case CrfType::machine_cycle:
            return "Machine Cycle";
        default:
            return "Reserved";
    }
}

auto crf_type_name(CrfType const type) noexcept -> std::string_view
{
    return crf_type_name(static_cast<uint8_t>(type));
}

auto crf_pull_name(uint8_t const pull) noexcept -> std::string_view
{
    switch (static_cast<CrfPull>(pull)) {
        case CrfPull::multiply_1_0:
            return "1.0";
        case CrfPull::multiply_1_div_1001:
            return "1/1.001";
        case CrfPull::multiply_1001:
            return "1.001";
        case CrfPull::multiply_24_div_25:
            return "24/25";
        case CrfPull::multiply_25_div_24:
            return "25/24";
        case CrfPull::multiply_1_div_8:
            return "1/8";
        default:
            return "Reserved";
    }
}

auto crf_pull_name(CrfPull const pull) noexcept -> std::string_view
{
    return crf_pull_name(static_cast<uint8_t>(pull));
}

}  // namespace statusbar::avtp
