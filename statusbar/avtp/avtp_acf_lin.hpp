#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_LIN - LIN message - IEEE 1722-2025 Clause 9.4.5
/// Carries a LIN frame: the bus (5-bit lin_bus_id), the one-octet LIN
/// identifier, the acquisition timestamp and 0 to 8 octets of data (0 to 2
/// quadlets). ACF_LIN_V2 (avtp_acf_lin_v2.hpp) is the same message with an
/// 11-bit bus id.
///
/// Wire format (12-byte fixed part + payload quadlets), Figure 70:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Byte 2:      pad[7:6] | mtv[5] | lin_bus_id[4:0]
///   Byte 3:      lin_identifier
///   Bytes 4-11:  message_timestamp (ns, valid when mtv)
///   Bytes 12+:   payload, zero-padded to the quadlet (pad octets)

#include "statusbar/avtp/avtp_acf.hpp"
#include "statusbar/buffer/buffer_traits.hpp"
#include "statusbar/ieee/ieee_base.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace statusbar::avtp {

using ieee::octet_t;
using ieee::octlet_t;

/// LIN message fixed part - IEEE 1722-2025 Clause 9.4.5, Figure 70
struct AcfLinMessage
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 12;
    /// Payload bounds in quadlets (9.4.5)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 0;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = 2;

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Byte 2: pad[7:6] | mtv[5] | lin_bus_id[4:0]
    octet_t pad_mtv_bus;

    // Byte 3: lin_identifier
    octet_t lin_identifier;

    // Bytes 4-11: message_timestamp (ns, valid when mtv)
    octlet_t message_timestamp;

    // Accessors

    /// Get pad: padding octets at the end of the payload (0 to 3)
    [[nodiscard]] constexpr auto pad() const noexcept -> uint8_t { return pad_mtv_bus.get_bits<uint8_t>(0xC0U, 6); }

    /// Set pad
    constexpr void set_pad(uint8_t const value) noexcept { pad_mtv_bus.set_bits(0xC0U, 6, value); }

    /// Get mtv: message_timestamp valid
    [[nodiscard]] constexpr auto mtv() const noexcept -> bool { return pad_mtv_bus.has_flag(0x20U); }

    /// Set mtv
    constexpr void set_mtv(bool const value) noexcept { pad_mtv_bus.set_flag(0x20U, value); }

    /// Get lin_bus_id: LIN bus identifier, 5 bits (application specific, Annex L)
    [[nodiscard]] constexpr auto lin_bus_id() const noexcept -> uint8_t { return pad_mtv_bus.get_bits<uint8_t>(0x1FU, 0); }

    /// Set lin_bus_id
    constexpr void set_lin_bus_id(uint8_t const value) noexcept { pad_mtv_bus.set_bits(0x1FU, 0, value); }

    /// Get lin_identifier: LIN message identifier
    [[nodiscard]] constexpr auto get_lin_identifier() const noexcept -> uint8_t { return lin_identifier.get(); }

    /// Set lin_identifier
    constexpr void set_lin_identifier(uint8_t const value) noexcept { lin_identifier = value; }

    /// Get message_timestamp: acquisition time in ns (9.4.1.5)
    [[nodiscard]] constexpr auto get_message_timestamp() const noexcept -> uint64_t { return message_timestamp.get(); }

    /// Set message_timestamp
    constexpr void set_message_timestamp(uint64_t const value) noexcept { message_timestamp = value; }

    // Initialization

    /// Initialize as the smallest ACF_LIN message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::lin, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        pad_mtv_bus = 0U;
        lin_identifier = 0U;
        message_timestamp = 0U;
    }

    // Validation

    /// Typed ACF_LIN with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::lin) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        return octets >= LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) &&
            octets <= LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS);
    }

    auto operator<=>(AcfLinMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfLinMessage) == AcfLinMessage::LENGTH, "AcfLinMessage must be exactly 12 bytes");
static_assert(alignof(AcfLinMessage) <= 4, "AcfLinMessage alignment must not exceed 4 bytes");
static_assert(offsetof(AcfLinMessage, pad_mtv_bus) == 2, "pad_mtv_bus must be at offset 2");
static_assert(offsetof(AcfLinMessage, lin_identifier) == 3, "lin_identifier must be at offset 3");
static_assert(offsetof(AcfLinMessage, message_timestamp) == 4, "message_timestamp must be at offset 4");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfLinMessage> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_LIN message: fixed part plus payload (pad removed)
using AcfLinMessageView = AcfTypedMessageView<AcfLinMessage>;

/// Parse an ACF_LIN message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_lin_parse(std::span<uint8_t const> const data) noexcept -> std::optional<AcfLinMessageView>
{
    return acf_parse_typed<AcfLinMessage>(data);
}

/// Build an ACF_LIN message into @p out from @p fixed and the original @p payload.
/// Returns the octets written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_lin_build(
    std::span<uint8_t> const out, AcfLinMessage const& fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    return acf_build_typed<AcfLinMessage>(out, fixed, payload);
}

}  // namespace statusbar::avtp
