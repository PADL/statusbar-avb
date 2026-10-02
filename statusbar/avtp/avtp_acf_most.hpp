#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_MOST - MOST message - IEEE 1722-2025 Clause 9.4.6
/// Carries a MOST control message: the network (most_net_id), the
/// source/receiver node address (device_id), the function block and instance,
/// the function and operation type, and the message payload; messages larger
/// than a frame need a higher-level segmentation protocol (9.4.6.11).
///
/// Wire format (20-byte fixed part + payload quadlets), Figure 72:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Byte 2:      pad[7:6] | mtv[5] | most_net_id[4:0]
///   Byte 3:      reserved
///   Bytes 4-11:  message_timestamp (ns, valid when mtv)
///   Bytes 12-13: device_id (MOST node address)
///   Byte 14:     fblock_id
///   Byte 15:     inst_id
///   Bytes 16-17: func_id[15:4] | op_type[3:0]
///   Bytes 18-19: reserved
///   Bytes 20+:   payload, zero-padded to the quadlet (pad octets)

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
using ieee::octet_t;
using ieee::octlet_t;

/// MOST message fixed part - IEEE 1722-2025 Clause 9.4.6, Figure 72
struct AcfMostMessage
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 20;
    /// Payload bounds in quadlets (9.4.6)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 0;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = ACF_MSG_LENGTH_MAX_QUADLETS - (LENGTH / ACF_QUADLET_OCTETS);

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Byte 2: pad[7:6] | mtv[5] | most_net_id[4:0]
    octet_t pad_mtv_net;

    // Byte 3: reserved
    octet_t reserved0;

    // Bytes 4-11: message_timestamp (ns, valid when mtv)
    octlet_t message_timestamp;

    // Bytes 12-13: device_id (MOST node address)
    doublet_t device_id;

    // Byte 14: fblock_id
    octet_t fblock_id;

    // Byte 15: inst_id
    octet_t inst_id;

    // Bytes 16-17: func_id[15:4] | op_type[3:0]
    doublet_t func_op;

    // Bytes 18-19: reserved
    doublet_t reserved1;

    // Accessors

    /// Get pad: padding octets at the end of the payload (0 to 3)
    [[nodiscard]] constexpr auto pad() const noexcept -> uint8_t { return pad_mtv_net.get_bits<uint8_t>(0xC0U, 6); }

    /// Set pad
    constexpr void set_pad(uint8_t const value) noexcept { pad_mtv_net.set_bits(0xC0U, 6, value); }

    /// Get mtv: message_timestamp valid
    [[nodiscard]] constexpr auto mtv() const noexcept -> bool { return pad_mtv_net.has_flag(0x20U); }

    /// Set mtv
    constexpr void set_mtv(bool const value) noexcept { pad_mtv_net.set_flag(0x20U, value); }

    /// Get most_net_id: MOST network identifier (application specific, Annex L)
    [[nodiscard]] constexpr auto most_net_id() const noexcept -> uint8_t { return pad_mtv_net.get_bits<uint8_t>(0x1FU, 0); }

    /// Set most_net_id
    constexpr void set_most_net_id(uint8_t const value) noexcept { pad_mtv_net.set_bits(0x1FU, 0, value); }

    /// Get message_timestamp: acquisition time in ns (9.4.1.5)
    [[nodiscard]] constexpr auto get_message_timestamp() const noexcept -> uint64_t { return message_timestamp.get(); }

    /// Set message_timestamp
    constexpr void set_message_timestamp(uint64_t const value) noexcept { message_timestamp = value; }

    /// Get device_id: node address of the source (or, for an answer, the receiver)
    [[nodiscard]] constexpr auto get_device_id() const noexcept -> uint16_t { return device_id.get(); }

    /// Set device_id
    constexpr void set_device_id(uint16_t const value) noexcept { device_id = value; }

    /// Get fblock_id: MOST function block identifier
    [[nodiscard]] constexpr auto get_fblock_id() const noexcept -> uint8_t { return fblock_id.get(); }

    /// Set fblock_id
    constexpr void set_fblock_id(uint8_t const value) noexcept { fblock_id = value; }

    /// Get inst_id: function block instance
    [[nodiscard]] constexpr auto get_inst_id() const noexcept -> uint8_t { return inst_id.get(); }

    /// Set inst_id
    constexpr void set_inst_id(uint8_t const value) noexcept { inst_id = value; }

    /// Get func_id: MOST function (12 bits)
    [[nodiscard]] constexpr auto func_id() const noexcept -> uint16_t { return func_op.get_bits<uint16_t>(0xFFF0U, 4); }

    /// Set func_id
    constexpr void set_func_id(uint16_t const value) noexcept { func_op.set_bits(0xFFF0U, 4, value); }

    /// Get op_type: MOST operation type (4 bits)
    [[nodiscard]] constexpr auto op_type() const noexcept -> uint8_t { return func_op.get_bits<uint8_t>(0x000FU, 0); }

    /// Set op_type
    constexpr void set_op_type(uint8_t const value) noexcept { func_op.set_bits(0x000FU, 0, value); }

    // Initialization

    /// Initialize as the smallest ACF_MOST message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::most, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        pad_mtv_net = 0U;
        reserved0 = 0U;
        message_timestamp = 0U;
        device_id = 0U;
        fblock_id = 0U;
        inst_id = 0U;
        func_op = 0U;
        reserved1 = 0U;
    }

    // Validation

    /// Typed ACF_MOST with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::most) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        return octets >= LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) &&
            octets <= LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS);
    }

    auto operator<=>(AcfMostMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfMostMessage) == AcfMostMessage::LENGTH, "AcfMostMessage must be exactly 20 bytes");
static_assert(alignof(AcfMostMessage) <= 4, "AcfMostMessage alignment must not exceed 4 bytes");
static_assert(offsetof(AcfMostMessage, pad_mtv_net) == 2, "pad_mtv_net must be at offset 2");
static_assert(offsetof(AcfMostMessage, reserved0) == 3, "reserved0 must be at offset 3");
static_assert(offsetof(AcfMostMessage, message_timestamp) == 4, "message_timestamp must be at offset 4");
static_assert(offsetof(AcfMostMessage, device_id) == 12, "device_id must be at offset 12");
static_assert(offsetof(AcfMostMessage, fblock_id) == 14, "fblock_id must be at offset 14");
static_assert(offsetof(AcfMostMessage, inst_id) == 15, "inst_id must be at offset 15");
static_assert(offsetof(AcfMostMessage, func_op) == 16, "func_op must be at offset 16");
static_assert(offsetof(AcfMostMessage, reserved1) == 18, "reserved1 must be at offset 18");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfMostMessage> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_MOST message: fixed part plus payload (pad removed)
using AcfMostMessageView = AcfTypedMessageView<AcfMostMessage>;

/// Parse an ACF_MOST message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_most_parse(std::span<uint8_t const> const data) noexcept -> std::optional<AcfMostMessageView>
{
    return acf_parse_typed<AcfMostMessage>(data);
}

/// Build an ACF_MOST message into @p out from @p fixed and the original @p payload.
/// Returns the octets written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_most_build(
    std::span<uint8_t> const out, AcfMostMessage const& fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    return acf_build_typed<AcfMostMessage>(out, fixed, payload);
}

}  // namespace statusbar::avtp
