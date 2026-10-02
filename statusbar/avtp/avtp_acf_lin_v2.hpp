#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_LIN_V2 - LIN message version 2 - IEEE 1722-2025 Clause 9.4.5
/// The LIN message with an 11-bit lin_bus_id: the identifier moves to an added
/// quadlet after the timestamp (9.4.5, NOTE). Otherwise identical to ACF_LIN
/// (avtp_acf_lin.hpp).
///
/// Wire format (16-byte fixed part + payload quadlets), Figure 71:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Bytes 2-3:   pad[15:14] | mtv[13] | rsv[12:11] | lin_bus_id[10:0]
///   Bytes 4-11:  message_timestamp (ns, valid when mtv)
///   Bytes 12-15: reserved[31:8] | lin_identifier[7:0]
///   Bytes 16+:   payload, zero-padded to the quadlet (pad octets)

#include "statusbar/avtp/avtp_acf.hpp"
#include "statusbar/buffer/buffer_traits.hpp"
#include "statusbar/ieee/ieee_base.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace statusbar::avtp {

using ieee::doublet_t;
using ieee::octlet_t;
using ieee::quadlet_t;

/// LIN message version 2 fixed part - IEEE 1722-2025 Clause 9.4.5, Figure 71
struct AcfLinV2Message
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 16;
    /// Payload bounds in quadlets (9.4.5)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 0;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = 2;

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Bytes 2-3: pad[15:14] | mtv[13] | rsv[12:11] | lin_bus_id[10:0]
    doublet_t pad_mtv_rsv_bus;

    // Bytes 4-11: message_timestamp (ns, valid when mtv)
    octlet_t message_timestamp;

    // Bytes 12-15: reserved[31:8] | lin_identifier[7:0]
    quadlet_t reserved_identifier;

    // Accessors

    /// Get pad: padding octets at the end of the payload (0 to 3)
    [[nodiscard]] constexpr auto pad() const noexcept -> uint8_t { return pad_mtv_rsv_bus.get_bits<uint8_t>(0xC000U, 14); }

    /// Set pad
    constexpr void set_pad(uint8_t const value) noexcept { pad_mtv_rsv_bus.set_bits(0xC000U, 14, value); }

    /// Get mtv: message_timestamp valid
    [[nodiscard]] constexpr auto mtv() const noexcept -> bool { return pad_mtv_rsv_bus.has_flag(0x2000U); }

    /// Set mtv
    constexpr void set_mtv(bool const value) noexcept { pad_mtv_rsv_bus.set_flag(0x2000U, value); }

    /// Get lin_bus_id: LIN bus identifier, 11 bits (application specific, Annex L)
    [[nodiscard]] constexpr auto lin_bus_id() const noexcept -> uint16_t { return pad_mtv_rsv_bus.get_bits<uint16_t>(0x07FFU, 0); }

    /// Set lin_bus_id
    constexpr void set_lin_bus_id(uint16_t const value) noexcept { pad_mtv_rsv_bus.set_bits(0x07FFU, 0, value); }

    /// Get message_timestamp: acquisition time in ns (9.4.1.5)
    [[nodiscard]] constexpr auto get_message_timestamp() const noexcept -> uint64_t { return message_timestamp.get(); }

    /// Set message_timestamp
    constexpr void set_message_timestamp(uint64_t const value) noexcept { message_timestamp = value; }

    /// Get lin_identifier: LIN message identifier
    [[nodiscard]] constexpr auto lin_identifier() const noexcept -> uint8_t
    {
        return reserved_identifier.get_bits<uint8_t>(0x000000FFU, 0);
    }

    /// Set lin_identifier
    constexpr void set_lin_identifier(uint8_t const value) noexcept { reserved_identifier.set_bits(0x000000FFU, 0, value); }

    // Initialization

    /// Initialize as the smallest ACF_LIN_V2 message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::lin_v2, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        pad_mtv_rsv_bus = 0U;
        message_timestamp = 0U;
        reserved_identifier = 0U;
    }

    // Validation

    /// Typed ACF_LIN_V2 with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::lin_v2) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        return octets >= LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) &&
            octets <= LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS);
    }

    auto operator<=>(AcfLinV2Message const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfLinV2Message) == AcfLinV2Message::LENGTH, "AcfLinV2Message must be exactly 16 bytes");
static_assert(alignof(AcfLinV2Message) <= 4, "AcfLinV2Message alignment must not exceed 4 bytes");
static_assert(offsetof(AcfLinV2Message, pad_mtv_rsv_bus) == 2, "pad_mtv_rsv_bus must be at offset 2");
static_assert(offsetof(AcfLinV2Message, message_timestamp) == 4, "message_timestamp must be at offset 4");
static_assert(offsetof(AcfLinV2Message, reserved_identifier) == 12, "reserved_identifier must be at offset 12");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfLinV2Message> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_LIN_V2 message: fixed part plus payload (pad removed)
using AcfLinV2MessageView = AcfTypedMessageView<AcfLinV2Message>;

/// Parse an ACF_LIN_V2 message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_lin_v2_parse(std::span<uint8_t const> const data) noexcept -> std::optional<AcfLinV2MessageView>
{
    return acf_parse_typed<AcfLinV2Message>(data);
}

/// Build an ACF_LIN_V2 message into @p out from @p fixed and the original @p payload.
/// Returns the octets written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_lin_v2_build(
    std::span<uint8_t> const out, AcfLinV2Message const& fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    return acf_build_typed<AcfLinV2Message>(out, fixed, payload);
}

}  // namespace statusbar::avtp
