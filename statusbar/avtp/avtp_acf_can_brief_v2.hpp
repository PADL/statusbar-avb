#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_CAN_BRIEF_V2 - Abbreviated CAN/CAN FD message version 2 - IEEE 1722-2025 Clause 9.4.4
/// The CAN/CAN FD message version 2 (avtp_acf_can_v2.hpp, 11-bit can_bus_id)
/// without the two-quadlet message_timestamp. mtv is transmitted as zero and
/// ignored on receipt (9.4.4.1).
///
/// Wire format (8-byte fixed part + payload quadlets), Figure 69:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Bytes 2-3:   pad[15:14] | mtv[13] | rtr[12] | eff[11] | can_bus_id[10:0]
///   Bytes 4-7:   brs[31] | fdf[30] | esi[29] | can_identifier[28:0]
///   Bytes 8+:   payload, zero-padded to the quadlet (pad octets)

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
using ieee::quadlet_t;

/// Abbreviated CAN/CAN FD message version 2 fixed part - IEEE 1722-2025 Clause 9.4.4, Figure 69
struct AcfCanBriefV2Message
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 8;
    /// Payload bounds in quadlets (9.4.4)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 0;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = 16;

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Bytes 2-3: pad[15:14] | mtv[13] | rtr[12] | eff[11] | can_bus_id[10:0]
    doublet_t pad_mtv_flags_bus;

    // Bytes 4-7: brs[31] | fdf[30] | esi[29] | can_identifier[28:0]
    quadlet_t flags_identifier;

    // Accessors

    /// Get pad: padding octets at the end of the payload (0 to 3)
    [[nodiscard]] constexpr auto pad() const noexcept -> uint8_t { return pad_mtv_flags_bus.get_bits<uint8_t>(0xC000U, 14); }

    /// Set pad
    constexpr void set_pad(uint8_t const value) noexcept { pad_mtv_flags_bus.set_bits(0xC000U, 14, value); }

    /// Get mtv: message_timestamp valid - always zero on a brief message (9.4.4.1)
    [[nodiscard]] constexpr auto mtv() const noexcept -> bool { return pad_mtv_flags_bus.has_flag(0x2000U); }

    /// Set mtv
    constexpr void set_mtv(bool const value) noexcept { pad_mtv_flags_bus.set_flag(0x2000U, value); }

    /// Get rtr: remote transmission request (CAN CC only)
    [[nodiscard]] constexpr auto rtr() const noexcept -> bool { return pad_mtv_flags_bus.has_flag(0x1000U); }

    /// Set rtr
    constexpr void set_rtr(bool const value) noexcept { pad_mtv_flags_bus.set_flag(0x1000U, value); }

    /// Get eff: extended frame format: 29-bit identifier when set, 11-bit when clear
    [[nodiscard]] constexpr auto eff() const noexcept -> bool { return pad_mtv_flags_bus.has_flag(0x0800U); }

    /// Set eff
    constexpr void set_eff(bool const value) noexcept { pad_mtv_flags_bus.set_flag(0x0800U, value); }

    /// Get can_bus_id: CAN bus identifier, 11 bits (application specific, Annex L)
    [[nodiscard]] constexpr auto can_bus_id() const noexcept -> uint16_t
    {
        return pad_mtv_flags_bus.get_bits<uint16_t>(0x07FFU, 0);
    }

    /// Set can_bus_id
    constexpr void set_can_bus_id(uint16_t const value) noexcept { pad_mtv_flags_bus.set_bits(0x07FFU, 0, value); }

    /// Get brs: bit rate switch (CAN FD only)
    [[nodiscard]] constexpr auto brs() const noexcept -> bool { return flags_identifier.has_flag(0x80000000U); }

    /// Set brs
    constexpr void set_brs(bool const value) noexcept { flags_identifier.set_flag(0x80000000U, value); }

    /// Get fdf: CAN FD format: payload may be 0-8, 12, 16, 20, 24, 32, 48 or 64 octets
    [[nodiscard]] constexpr auto fdf() const noexcept -> bool { return flags_identifier.has_flag(0x40000000U); }

    /// Set fdf
    constexpr void set_fdf(bool const value) noexcept { flags_identifier.set_flag(0x40000000U, value); }

    /// Get esi: error state indicator (CAN FD only)
    [[nodiscard]] constexpr auto esi() const noexcept -> bool { return flags_identifier.has_flag(0x20000000U); }

    /// Set esi
    constexpr void set_esi(bool const value) noexcept { flags_identifier.set_flag(0x20000000U, value); }

    /// Get can_identifier: CAN identifier, 11 or 29 bits per eff, right-aligned
    [[nodiscard]] constexpr auto can_identifier() const noexcept -> uint32_t
    {
        return flags_identifier.get_bits<uint32_t>(0x1FFFFFFFU, 0);
    }

    /// Set can_identifier
    constexpr void set_can_identifier(uint32_t const value) noexcept { flags_identifier.set_bits(0x1FFFFFFFU, 0, value); }

    // Initialization

    /// Initialize as the smallest ACF_CAN_BRIEF_V2 message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::can_brief_v2, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        pad_mtv_flags_bus = 0U;
        flags_identifier = 0U;
    }

    // Validation

    /// Typed ACF_CAN_BRIEF_V2 with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::can_brief_v2) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        return octets >= LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) &&
            octets <= LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS);
    }

    auto operator<=>(AcfCanBriefV2Message const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfCanBriefV2Message) == AcfCanBriefV2Message::LENGTH, "AcfCanBriefV2Message must be exactly 8 bytes");
static_assert(alignof(AcfCanBriefV2Message) <= 4, "AcfCanBriefV2Message alignment must not exceed 4 bytes");
static_assert(offsetof(AcfCanBriefV2Message, pad_mtv_flags_bus) == 2, "pad_mtv_flags_bus must be at offset 2");
static_assert(offsetof(AcfCanBriefV2Message, flags_identifier) == 4, "flags_identifier must be at offset 4");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfCanBriefV2Message> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_CAN_BRIEF_V2 message: fixed part plus payload (pad removed)
using AcfCanBriefV2MessageView = AcfTypedMessageView<AcfCanBriefV2Message>;

/// Parse an ACF_CAN_BRIEF_V2 message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_can_brief_v2_parse(std::span<uint8_t const> const data) noexcept
    -> std::optional<AcfCanBriefV2MessageView>
{
    return acf_parse_typed<AcfCanBriefV2Message>(data);
}

/// Build an ACF_CAN_BRIEF_V2 message into @p out from @p fixed and the original @p payload;
/// mtv is forced to zero (brief messages carry no timestamp). Returns the octets
/// written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_can_brief_v2_build(
    std::span<uint8_t> const out, AcfCanBriefV2Message fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    fixed.set_mtv(false);
    return acf_build_typed<AcfCanBriefV2Message>(out, fixed, payload);
}

}  // namespace statusbar::avtp
