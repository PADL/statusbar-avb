#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_GPC - General Purpose Control (GPC) message - IEEE 1722-2025 Clause 9.4.7
/// A generic message: a 48-bit gpc_msg_id (an EUI-48 from the vendor's own
/// MA-L/MA-M/MA-S block, or the organization's CID followed by its own value)
/// identifies the message type that defines the payload's structure. There is
/// no pad field; any padding convention belongs to the payload's definition
/// (9.4.7.3, NOTE).
///
/// Wire format (8-byte fixed part + payload quadlets), Figure 73:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Bytes 2-7:   gpc_msg_id (48 bits: EUI-48 or CID-based)
///   Bytes 8+:   payload, zero-padded to the quadlet (pad octets)

#include "statusbar/avtp/avtp_acf.hpp"
#include "statusbar/buffer/buffer_traits.hpp"
#include "statusbar/ieee/ieee_base.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace statusbar::avtp {

using ieee::Eui48;

/// General Purpose Control (GPC) message fixed part - IEEE 1722-2025 Clause 9.4.7, Figure 73
struct AcfGpcMessage
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 8;
    /// Payload bounds in quadlets (9.4.7)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 0;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = ACF_MSG_LENGTH_MAX_QUADLETS - (LENGTH / ACF_QUADLET_OCTETS);

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Bytes 2-7: gpc_msg_id (48 bits: EUI-48 or CID-based)
    Eui48 gpc_msg_id;

    // Accessors

    /// Get gpc_msg_id: identifier of the GPC message type
    [[nodiscard]] constexpr auto get_gpc_msg_id() const noexcept -> ieee::Eui48 { return gpc_msg_id; }

    /// Set gpc_msg_id
    constexpr void set_gpc_msg_id(ieee::Eui48 const& value) noexcept { gpc_msg_id = value; }

    /// This message type has no pad field: the payload is taken as carried
    [[nodiscard]] constexpr auto pad() const noexcept -> uint8_t { return 0U; }

    /// No pad field to set (the shared builder calls this; nothing to record)
    constexpr void set_pad(uint8_t const /*value*/) noexcept {}

    // Initialization

    /// Initialize as the smallest ACF_GPC message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::gpc, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        gpc_msg_id = ieee::Eui48{};
    }

    // Validation

    /// Typed ACF_GPC with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::gpc) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        return octets >= LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) &&
            octets <= LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS);
    }

    auto operator<=>(AcfGpcMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfGpcMessage) == AcfGpcMessage::LENGTH, "AcfGpcMessage must be exactly 8 bytes");
static_assert(alignof(AcfGpcMessage) <= 4, "AcfGpcMessage alignment must not exceed 4 bytes");
static_assert(offsetof(AcfGpcMessage, gpc_msg_id) == 2, "gpc_msg_id must be at offset 2");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfGpcMessage> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_GPC message: fixed part plus payload (pad removed)
using AcfGpcMessageView = AcfTypedMessageView<AcfGpcMessage>;

/// Parse an ACF_GPC message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_gpc_parse(std::span<uint8_t const> const data) noexcept -> std::optional<AcfGpcMessageView>
{
    return acf_parse_typed<AcfGpcMessage>(data);
}

/// Build an ACF_GPC message into @p out from @p fixed and the original @p payload.
/// Returns the octets written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_gpc_build(
    std::span<uint8_t> const out, AcfGpcMessage const& fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    return acf_build_typed<AcfGpcMessage>(out, fixed, payload);
}

}  // namespace statusbar::avtp
