#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_FLEXRAY - FlexRay message - IEEE 1722-2025 Clause 9.4.2
/// Carries a FlexRay frame: bus and channel, the frame's
/// startup/sync/preamble/null-frame indicators, its frame id and cycle, and 0
/// to 64 quadlets of frame data (enough for the largest 254-octet FlexRay
/// frame).
///
/// Wire format (16-byte fixed part + payload quadlets), Figure 64:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Byte 2:      pad[7:6] | mtv[5] | fr_bus_id[4:0]
///   Byte 3:      rsv[7:6] | chan[5:4] | str[3] | syn[2] | pre[1] | nfi[0]
///   Bytes 4-11:  message_timestamp (ns, valid when mtv)
///   Bytes 12-15: fr_frame_id[31:21] | reserved[20:6] | cycle[5:0]
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

using ieee::octet_t;
using ieee::octlet_t;
using ieee::quadlet_t;

/// FlexRay message fixed part - IEEE 1722-2025 Clause 9.4.2, Figure 64
struct AcfFlexrayMessage
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 16;
    /// Payload bounds in quadlets (9.4.2)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 0;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = 64;

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Byte 2: pad[7:6] | mtv[5] | fr_bus_id[4:0]
    octet_t pad_mtv_bus;

    // Byte 3: rsv[7:6] | chan[5:4] | str[3] | syn[2] | pre[1] | nfi[0]
    octet_t chan_flags;

    // Bytes 4-11: message_timestamp (ns, valid when mtv)
    octlet_t message_timestamp;

    // Bytes 12-15: fr_frame_id[31:21] | reserved[20:6] | cycle[5:0]
    quadlet_t frame_id_cycle;

    // Accessors

    /// Get pad: padding octets at the end of the payload (0 to 3)
    [[nodiscard]] constexpr auto pad() const noexcept -> uint8_t { return pad_mtv_bus.get_bits<uint8_t>(0xC0U, 6); }

    /// Set pad
    constexpr void set_pad(uint8_t const value) noexcept { pad_mtv_bus.set_bits(0xC0U, 6, value); }

    /// Get mtv: message_timestamp valid
    [[nodiscard]] constexpr auto mtv() const noexcept -> bool { return pad_mtv_bus.has_flag(0x20U); }

    /// Set mtv
    constexpr void set_mtv(bool const value) noexcept { pad_mtv_bus.set_flag(0x20U, value); }

    /// Get fr_bus_id: FlexRay bus identifier (application specific, Annex L)
    [[nodiscard]] constexpr auto fr_bus_id() const noexcept -> uint8_t { return pad_mtv_bus.get_bits<uint8_t>(0x1FU, 0); }

    /// Set fr_bus_id
    constexpr void set_fr_bus_id(uint8_t const value) noexcept { pad_mtv_bus.set_bits(0x1FU, 0, value); }

    /// Get chan: source channel (Table 25: 0 unknown, 1 A, 2 B, 3 both)
    [[nodiscard]] constexpr auto chan() const noexcept -> uint8_t { return chan_flags.get_bits<uint8_t>(0x30U, 4); }

    /// Set chan
    constexpr void set_chan(uint8_t const value) noexcept { chan_flags.set_bits(0x30U, 4, value); }

    /// Get str: startup frame indicator
    [[nodiscard]] constexpr auto str() const noexcept -> bool { return chan_flags.has_flag(0x08U); }

    /// Set str
    constexpr void set_str(bool const value) noexcept { chan_flags.set_flag(0x08U, value); }

    /// Get syn: sync frame indicator
    [[nodiscard]] constexpr auto syn() const noexcept -> bool { return chan_flags.has_flag(0x04U); }

    /// Set syn
    constexpr void set_syn(bool const value) noexcept { chan_flags.set_flag(0x04U, value); }

    /// Get pre: payload preamble indicator
    [[nodiscard]] constexpr auto pre() const noexcept -> bool { return chan_flags.has_flag(0x02U); }

    /// Set pre
    constexpr void set_pre(bool const value) noexcept { chan_flags.set_flag(0x02U, value); }

    /// Get nfi: null frame indicator
    [[nodiscard]] constexpr auto nfi() const noexcept -> bool { return chan_flags.has_flag(0x01U); }

    /// Set nfi
    constexpr void set_nfi(bool const value) noexcept { chan_flags.set_flag(0x01U, value); }

    /// Get message_timestamp: acquisition time in ns (9.4.1.5)
    [[nodiscard]] constexpr auto get_message_timestamp() const noexcept -> uint64_t { return message_timestamp.get(); }

    /// Set message_timestamp
    constexpr void set_message_timestamp(uint64_t const value) noexcept { message_timestamp = value; }

    /// Get fr_frame_id: FlexRay frame identifier (1 to 2047, 0 when not from FlexRay)
    [[nodiscard]] constexpr auto fr_frame_id() const noexcept -> uint16_t
    {
        return frame_id_cycle.get_bits<uint16_t>(0xFFE00000U, 21);
    }

    /// Set fr_frame_id
    constexpr void set_fr_frame_id(uint16_t const value) noexcept { frame_id_cycle.set_bits(0xFFE00000U, 21, value); }

    /// Get cycle: FlexRay cycle count
    [[nodiscard]] constexpr auto cycle() const noexcept -> uint8_t { return frame_id_cycle.get_bits<uint8_t>(0x0000003FU, 0); }

    /// Set cycle
    constexpr void set_cycle(uint8_t const value) noexcept { frame_id_cycle.set_bits(0x0000003FU, 0, value); }

    // Initialization

    /// Initialize as the smallest ACF_FLEXRAY message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::flexray, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        pad_mtv_bus = 0U;
        chan_flags = 0U;
        message_timestamp = 0U;
        frame_id_cycle = 0U;
    }

    // Validation

    /// Typed ACF_FLEXRAY with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::flexray) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        return octets >= LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) &&
            octets <= LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS);
    }

    auto operator<=>(AcfFlexrayMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfFlexrayMessage) == AcfFlexrayMessage::LENGTH, "AcfFlexrayMessage must be exactly 16 bytes");
static_assert(alignof(AcfFlexrayMessage) <= 4, "AcfFlexrayMessage alignment must not exceed 4 bytes");
static_assert(offsetof(AcfFlexrayMessage, pad_mtv_bus) == 2, "pad_mtv_bus must be at offset 2");
static_assert(offsetof(AcfFlexrayMessage, chan_flags) == 3, "chan_flags must be at offset 3");
static_assert(offsetof(AcfFlexrayMessage, message_timestamp) == 4, "message_timestamp must be at offset 4");
static_assert(offsetof(AcfFlexrayMessage, frame_id_cycle) == 12, "frame_id_cycle must be at offset 12");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfFlexrayMessage> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_FLEXRAY message: fixed part plus payload (pad removed)
using AcfFlexrayMessageView = AcfTypedMessageView<AcfFlexrayMessage>;

/// Parse an ACF_FLEXRAY message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_flexray_parse(std::span<uint8_t const> const data) noexcept -> std::optional<AcfFlexrayMessageView>
{
    return acf_parse_typed<AcfFlexrayMessage>(data);
}

/// Build an ACF_FLEXRAY message into @p out from @p fixed and the original @p payload.
/// Returns the octets written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_flexray_build(
    std::span<uint8_t> const out, AcfFlexrayMessage const& fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    return acf_build_typed<AcfFlexrayMessage>(out, fixed, payload);
}

}  // namespace statusbar::avtp
