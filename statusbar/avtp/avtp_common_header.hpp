#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVTP common header accessors — work on raw byte spans regardless of subtype.
///
/// All version-0 AVTP PDUs (IEEE 1722-2016 Section 4.4) share a common
/// 12-byte prefix:
///
///   Offset  Field
///   0       subtype
///   1       sv[7] version[6:4] mr[3] r[2] gv/fs[1] tv/tu[0]
///   2       sequence_num
///   3       subtype-specific (reserved_tu, crf_type, etc.)
///   4-11    stream_id
///
/// Stream-type subtypes (AAF, AM824, etc.) additionally have:
///   12-15   avtp_timestamp

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <span>

namespace statusbar::avtp {

/// Minimum length for the common AVTP header (bytes 0-11).
inline constexpr size_t AVTP_COMMON_HEADER_LENGTH = 12;

/// Minimum length to access avtp_timestamp (bytes 0-15, stream subtypes only).
inline constexpr size_t AVTP_STREAM_HEADER_MIN_LENGTH = 16;

/// Get the subtype field (byte 0).
[[nodiscard]] constexpr auto avtp_get_subtype(std::span<uint8_t const> payload) noexcept -> std::optional<uint8_t>
{
    if (payload.empty()) {
        return std::nullopt;
    }
    return payload[0];
}

/// Get the stream-valid (sv) bit (byte 1, bit 7).
[[nodiscard]] constexpr auto avtp_get_sv(std::span<uint8_t const> payload) noexcept -> std::optional<bool>
{
    if (payload.size() < 2) {
        return std::nullopt;
    }
    return (payload[1] & 0x80) != 0;
}

/// Get the version field (byte 1, bits 6:4).
[[nodiscard]] constexpr auto avtp_get_version(std::span<uint8_t const> payload) noexcept -> std::optional<uint8_t>
{
    if (payload.size() < 2) {
        return std::nullopt;
    }
    return static_cast<uint8_t>((payload[1] >> 4) & 0x07);
}

/// Get the media-clock-restart (mr) bit (byte 1, bit 3).
[[nodiscard]] constexpr auto avtp_get_mr(std::span<uint8_t const> payload) noexcept -> std::optional<bool>
{
    if (payload.size() < 2) {
        return std::nullopt;
    }
    return (payload[1] & 0x08) != 0;
}

/// Get the timestamp-valid (tv) bit (byte 1, bit 0).
[[nodiscard]] constexpr auto avtp_get_tv(std::span<uint8_t const> payload) noexcept -> std::optional<bool>
{
    if (payload.size() < 2) {
        return std::nullopt;
    }
    return (payload[1] & 0x01) != 0;
}

/// Get the sequence number (byte 2).
[[nodiscard]] constexpr auto avtp_get_sequence_num(std::span<uint8_t const> payload) noexcept -> std::optional<uint8_t>
{
    if (payload.size() < 3) {
        return std::nullopt;
    }
    return payload[2];
}

/// Get the stream ID (bytes 4-11). Returns nullopt if payload is too short.
[[nodiscard]] inline auto avtp_get_stream_id(std::span<uint8_t const> payload) noexcept -> std::optional<tsn::StreamId>
{
    if (payload.size() < AVTP_COMMON_HEADER_LENGTH) {
        return std::nullopt;
    }
    tsn::StreamId sid{};
    span_load(sid, payload.subspan(4, sizeof(sid)));
    return sid;
}

/// Get the AVTP timestamp (bytes 12-15, big-endian).
/// Only valid for stream-type subtypes (AAF, AM824, etc.), not CRF or control types.
[[nodiscard]] constexpr auto avtp_get_timestamp(std::span<uint8_t const> payload) noexcept -> std::optional<uint32_t>
{
    if (payload.size() < AVTP_STREAM_HEADER_MIN_LENGTH) {
        return std::nullopt;
    }
    std::array<uint8_t, 4> const be{payload[12], payload[13], payload[14], payload[15]};
    return std::bit_cast<ieee::quadlet_t>(be).get();
}

/// Check if a payload's stream_id matches the expected value.
/// Returns false if the payload is too short.
[[nodiscard]] inline auto avtp_match_stream_id(std::span<uint8_t const> payload, tsn::StreamId const& expected) noexcept -> bool
{
    auto const sid = avtp_get_stream_id(payload);
    return sid.has_value() && *sid == expected;
}

}  // namespace statusbar::avtp
