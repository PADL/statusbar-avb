#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_CHECKSUM - Checksum message - IEEE 1722-2025 Clause 9.4.20
/// Optional 16-bit validation of the immediately preceding ACF message in
/// the same frame: the ones-complement of the ones-complement sum (the UDP
/// method, RFC 768) over that message from its acf_msg_type through the end
/// of its acf_msg_payload. Unlike UDP, a checksum field of zero is a real
/// value and is validated.
///
/// Wire format (one quadlet):
///   Bytes 0-1: acf_msg_type (0x76) | acf_msg_length (1)
///   Bytes 2-3: checksum

#include "statusbar/avtp/avtp_acf.hpp"
#include "statusbar/buffer/buffer_traits.hpp"
#include "statusbar/ieee/ieee_base.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace statusbar::avtp {

/// Checksum message - IEEE 1722-2025 Clause 9.4.20, Figure 94
struct AcfChecksumMessage
{
    /// Length on the wire: exactly one quadlet
    static constexpr size_t LENGTH = 4;
    static constexpr uint16_t MSG_LENGTH_QUADLETS = 1;

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Bytes 2-3: checksum
    doublet_t checksum;

    /// Get the checksum field
    [[nodiscard]] constexpr auto get_checksum() const noexcept -> uint16_t { return checksum.get(); }

    /// Set the checksum field
    constexpr void set_checksum(uint16_t const value) noexcept { checksum = value; }

    /// Initialize with @p value as the checksum
    constexpr void init(uint16_t const value) noexcept
    {
        header.init(AcfMsgType::checksum, MSG_LENGTH_QUADLETS);
        checksum = value;
    }

    /// Valid when typed ACF_CHECKSUM with the one-quadlet length
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        return header.msg_type() == AcfMsgType::checksum && header.msg_length() == MSG_LENGTH_QUADLETS;
    }

    auto operator<=>(AcfChecksumMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(AcfChecksumMessage) == AcfChecksumMessage::LENGTH, "AcfChecksumMessage must be exactly 4 bytes");
static_assert(offsetof(AcfChecksumMessage, checksum) == 2, "checksum must be at offset 2");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfChecksumMessage> : std::true_type
{};

namespace statusbar::avtp {

/// The checksum of @p preceding_message (its whole span, header included)
[[nodiscard]] auto acf_checksum_compute(std::span<uint8_t const> preceding_message) noexcept -> uint16_t;

/// Parse a Checksum message at the start of @p data (the message's own
/// span, as yielded by the walker)
[[nodiscard]] auto acf_checksum_parse(std::span<uint8_t const> data) noexcept -> std::optional<AcfChecksumMessage>;

/// Write a Checksum message for @p preceding_message into @p out (at least
/// 4 octets); returns false when @p out is too small
[[nodiscard]] auto acf_checksum_build(std::span<uint8_t> out, std::span<uint8_t const> preceding_message) noexcept -> bool;

/// True when @p message carries the checksum of @p preceding_message
[[nodiscard]] auto acf_checksum_verify(AcfChecksumMessage const& message, std::span<uint8_t const> preceding_message) noexcept
    -> bool;

}  // namespace statusbar::avtp
