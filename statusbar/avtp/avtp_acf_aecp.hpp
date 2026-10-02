#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_AECP - AECP message - IEEE 1722-2025 Clause 9.4.12
/// Carries a whole IEEE Std 1722.1 AECPDU (its subclause 9.2) in the payload,
/// zero-padded to the quadlet. There is no pad field: the AECPDU's own
/// control_data_length (12 + control_data_length octets) says where the padding
/// starts (9.4.12.2, NOTE).
///
/// Wire format (4-byte fixed part + payload quadlets), Figure 81:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Bytes 2-3:   reserved
///   Bytes 4+:   payload, zero-padded to the quadlet (pad octets)

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

/// AECP message fixed part - IEEE 1722-2025 Clause 9.4.12, Figure 81
struct AcfAecpMessage
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 4;
    /// Payload bounds in quadlets (9.4.12)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 0;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = ACF_MSG_LENGTH_MAX_QUADLETS - (LENGTH / ACF_QUADLET_OCTETS);

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Bytes 2-3: reserved
    doublet_t reserved0;

    // Accessors

    /// This message type has no pad field: the payload is taken as carried
    [[nodiscard]] constexpr auto pad() const noexcept -> uint8_t { return 0U; }

    /// No pad field to set (the shared builder calls this; nothing to record)
    constexpr void set_pad(uint8_t const /*value*/) noexcept {}

    // Initialization

    /// Initialize as the smallest ACF_AECP message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::aecp, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        reserved0 = 0U;
    }

    // Validation

    /// Typed ACF_AECP with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::aecp) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        return octets >= LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) &&
            octets <= LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS);
    }

    auto operator<=>(AcfAecpMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfAecpMessage) == AcfAecpMessage::LENGTH, "AcfAecpMessage must be exactly 4 bytes");
static_assert(alignof(AcfAecpMessage) <= 4, "AcfAecpMessage alignment must not exceed 4 bytes");
static_assert(offsetof(AcfAecpMessage, reserved0) == 2, "reserved0 must be at offset 2");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfAecpMessage> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_AECP message: fixed part plus payload (pad removed)
using AcfAecpMessageView = AcfTypedMessageView<AcfAecpMessage>;

/// Parse an ACF_AECP message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_aecp_parse(std::span<uint8_t const> const data) noexcept -> std::optional<AcfAecpMessageView>
{
    return acf_parse_typed<AcfAecpMessage>(data);
}

/// Build an ACF_AECP message into @p out from @p fixed and the original @p payload.
/// Returns the octets written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_aecp_build(
    std::span<uint8_t> const out, AcfAecpMessage const& fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    return acf_build_typed<AcfAecpMessage>(out, fixed, payload);
}

/// Length of the AECPDU common header (IEEE 1722.1 Clause 9.2.1)
constexpr size_t ACF_AECPDU_HEADER_LENGTH = 12;

/// The AECPDU carried by @p view, trimmed of the quadlet padding: 12 +
/// control_data_length octets (the AECPDU's bytes 2-3 are status[15:11] |
/// control_data_length[10:0]). nullopt when the payload is shorter than an
/// AECPDU header or than the length it declares.
[[nodiscard]] inline auto acf_aecp_aecpdu(AcfAecpMessageView const& view) noexcept -> std::optional<std::span<uint8_t const>>
{
    if (view.payload.size() < ACF_AECPDU_HEADER_LENGTH) {
        return std::nullopt;
    }
    doublet_t status_cdl{};
    span_load(status_cdl, view.payload.subspan(2));
    auto const total = ACF_AECPDU_HEADER_LENGTH + status_cdl.get_bits<size_t>(0x07FFU, 0);
    if (total > view.payload.size()) {
        return std::nullopt;
    }
    return view.payload.first(total);
}

}  // namespace statusbar::avtp
