#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Detection helpers for identifying AVTP audio stream subtypes (AAF vs.
// IEC-61883/AM824) from a raw payload. Pulled out of avtp_to_wav_tool so
// other clients (live capture, retransmit, analysis) can drive the same
// classification without duplicating the magic-byte checks.

#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_am824.hpp"
#include "statusbar/avtp/avtp_types.hpp"
#include "statusbar/buffer/span_utils.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace statusbar::avtp_tools {

enum class StreamKind : uint8_t
{
    unknown,
    aaf,
    am824_mbla,
};

[[nodiscard]] constexpr auto kind_name(StreamKind k) noexcept -> std::string_view
{
    switch (k) {
        case StreamKind::aaf:
            return "AAF";
        case StreamKind::am824_mbla:
            return "AM824-MBLA";
        default:
            return "?";
    }
}

[[nodiscard]] inline auto payload_is_aaf(std::span<uint8_t const> payload) noexcept -> bool
{
    return payload.size() >= avtp::AafPdu::HEADER_LENGTH && payload[0] == avtp::AvtpSubtype::aaf;
}

[[nodiscard]] inline auto payload_is_am824(std::span<uint8_t const> payload) noexcept -> bool
{
    return payload.size() >= avtp::Am824Pdu::HEADER_LENGTH && payload[0] == avtp::AvtpSubtype::iec_61883_iidc;
}

// Extract the AVTP stream id from an audio payload of the given kind.
// Caller must have already verified the payload is at least the header
// length (via payload_is_aaf / payload_is_am824).
[[nodiscard]] inline auto stream_id_of(std::span<uint8_t const> payload, StreamKind kind) noexcept -> uint64_t
{
    if (kind == StreamKind::aaf) {
        avtp::AafPdu pdu;
        span_load(pdu, payload.subspan(0, avtp::AafPdu::HEADER_LENGTH));
        return pdu.stream_id().to_uint64();
    }
    avtp::Am824Pdu pdu;
    span_load(pdu, payload.subspan(0, avtp::Am824Pdu::HEADER_LENGTH));
    return pdu.stream_id().to_uint64();
}

}  // namespace statusbar::avtp_tools
