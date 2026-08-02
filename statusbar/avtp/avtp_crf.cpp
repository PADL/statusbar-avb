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
    uint64_t const value = (static_cast<uint64_t>(timestamp_data[offset]) << 56) |
        (static_cast<uint64_t>(timestamp_data[offset + 1]) << 48) | (static_cast<uint64_t>(timestamp_data[offset + 2]) << 40) |
        (static_cast<uint64_t>(timestamp_data[offset + 3]) << 32) | (static_cast<uint64_t>(timestamp_data[offset + 4]) << 24) |
        (static_cast<uint64_t>(timestamp_data[offset + 5]) << 16) | (static_cast<uint64_t>(timestamp_data[offset + 6]) << 8) |
        static_cast<uint64_t>(timestamp_data[offset + 7]);
    return value;
}

auto crf_set_timestamp(std::span<uint8_t> const timestamp_data, size_t const index, uint64_t const timestamp) noexcept -> bool
{
    size_t const offset = index * CrfPdu::TIMESTAMP_SIZE;
    if (offset + CrfPdu::TIMESTAMP_SIZE > timestamp_data.size()) {
        return false;
    }
    // Store as 64-bit big-endian
    timestamp_data[offset] = static_cast<uint8_t>((timestamp >> 56) & 0xFFU);
    timestamp_data[offset + 1] = static_cast<uint8_t>((timestamp >> 48) & 0xFFU);
    timestamp_data[offset + 2] = static_cast<uint8_t>((timestamp >> 40) & 0xFFU);
    timestamp_data[offset + 3] = static_cast<uint8_t>((timestamp >> 32) & 0xFFU);
    timestamp_data[offset + 4] = static_cast<uint8_t>((timestamp >> 24) & 0xFFU);
    timestamp_data[offset + 5] = static_cast<uint8_t>((timestamp >> 16) & 0xFFU);
    timestamp_data[offset + 6] = static_cast<uint8_t>((timestamp >> 8) & 0xFFU);
    timestamp_data[offset + 7] = static_cast<uint8_t>(timestamp & 0xFFU);
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
