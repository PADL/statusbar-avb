#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_CRC - CRC message - IEEE 1722-2025 Clause 9.4.21
/// Optional 32-bit (or wider) validation of the immediately preceding ACF
/// message in the same frame, over that message from its acf_msg_type
/// through the end of its acf_msg_payload. crc_type (Table 30) selects the
/// polynomial; CRC_ETH and CRC_32P4 carry one quadlet of crc_data.
///
/// Wire format (two quadlets for the 32-bit types):
///   Bytes 0-1: acf_msg_type (0x77) | acf_msg_length (2)
///   Bytes 2-3: reserved[15:4] | crc_type[3:0]
///   Bytes 4-7: crc_data (CRC_ETH / CRC_32P4; 1 to n quadlets in general)

#include "statusbar/avtp/avtp_acf.hpp"
#include "statusbar/buffer/buffer_traits.hpp"
#include "statusbar/ieee/ieee_base.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace statusbar::avtp {

/// crc_type values - IEEE 1722-2025 Table 30
enum class AcfCrcType : uint8_t
{
    crc_eth = 0x0U,   ///< Ethernet's 32-bit CRC (IEEE 802.3 FCS)
    crc_32p4 = 0x1U,  ///< AUTOSAR's 32-bit CRC32P4
    // 0x2 - 0xE: reserved
    crc_user = 0xFU,  ///< user-defined
};

/// Name of an AcfCrcType value
[[nodiscard]] auto acf_crc_type_name(AcfCrcType type) noexcept -> std::string_view;

/// The fixed part of a CRC message - IEEE 1722-2025 Clause 9.4.21, Figure 95
struct AcfCrcMessage
{
    /// Length of the fixed part on the wire (crc_data follows)
    static constexpr size_t LENGTH = 4;
    /// acf_msg_length of a message carrying a 32-bit CRC
    static constexpr uint16_t MSG_LENGTH_QUADLETS_32 = 2;

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Bytes 2-3: reserved[15:4] | crc_type[3:0]
    doublet_t reserved_crc_type;

    /// Get crc_type
    [[nodiscard]] constexpr auto crc_type() const noexcept -> AcfCrcType
    {
        return static_cast<AcfCrcType>(reserved_crc_type.get_bits<uint8_t>(0x000FU, 0));
    }

    /// Set crc_type
    constexpr void set_crc_type(AcfCrcType const type) noexcept
    {
        reserved_crc_type.set_bits(0x000FU, 0, static_cast<uint8_t>(type));
    }

    /// Initialize for a 32-bit CRC of @p type (crc_data stored separately)
    constexpr void init(AcfCrcType const type) noexcept
    {
        header.init(AcfMsgType::crc, MSG_LENGTH_QUADLETS_32);
        reserved_crc_type = 0U;
        set_crc_type(type);
    }

    /// Valid when typed ACF_CRC with at least one crc_data quadlet
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        return header.msg_type() == AcfMsgType::crc && header.msg_length() >= 2U;
    }

    auto operator<=>(AcfCrcMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(AcfCrcMessage) == AcfCrcMessage::LENGTH, "AcfCrcMessage must be exactly 4 bytes");
static_assert(offsetof(AcfCrcMessage, reserved_crc_type) == 2, "reserved_crc_type must be at offset 2");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfCrcMessage> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed CRC message: the fixed part and its crc_data quadlets
struct AcfCrcMessageView
{
    AcfCrcMessage fixed{};
    std::span<uint8_t const> crc_data;  ///< acf_msg_length - 1 quadlets

    /// The 32-bit crc_data value when exactly one quadlet is carried
    [[nodiscard]] auto crc32() const noexcept -> std::optional<uint32_t>;
};

/// Compute the CRC of @p type over @p preceding_message; nullopt for a
/// crc_type this implementation cannot compute (reserved or user)
[[nodiscard]] auto acf_crc_compute(AcfCrcType type, std::span<uint8_t const> preceding_message) noexcept -> std::optional<uint32_t>;

/// Parse a CRC message at the start of @p data (the message's own span, as
/// yielded by the walker)
[[nodiscard]] auto acf_crc_parse(std::span<uint8_t const> data) noexcept -> std::optional<AcfCrcMessageView>;

/// Write a 32-bit CRC message of @p type for @p preceding_message into
/// @p out (at least 8 octets); false when @p out is too small or @p type
/// cannot be computed
[[nodiscard]] auto acf_crc_build(std::span<uint8_t> out, AcfCrcType type, std::span<uint8_t const> preceding_message) noexcept
    -> bool;

/// Whether @p view carries the CRC of @p preceding_message: true/false for
/// a computable 32-bit type, nullopt when the type is unsupported or the
/// crc_data width does not match
[[nodiscard]] auto acf_crc_verify(AcfCrcMessageView const& view, std::span<uint8_t const> preceding_message) noexcept
    -> std::optional<bool>;

}  // namespace statusbar::avtp
