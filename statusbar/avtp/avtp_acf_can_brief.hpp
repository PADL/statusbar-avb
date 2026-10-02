#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_CAN_BRIEF - Abbreviated CAN/CAN FD message - IEEE 1722-2025 Clause 9.4.4
/// The CAN/CAN FD message (avtp_acf_can.hpp) without the two-quadlet
/// message_timestamp, for CAN data that needs no acquisition time. mtv is
/// transmitted as zero and ignored on receipt (9.4.4.1).
///
/// Wire format (8-byte fixed part + payload quadlets), Figure 68:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Byte 2:      pad[7:6] | mtv[5] | rtr[4] | eff[3] | brs[2] | fdf[1] | esi[0]
///   Byte 3:      rsv[7:5] | can_bus_id[4:0]
///   Bytes 4-7:   rsv[31:29] | can_identifier[28:0]
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

using ieee::octet_t;
using ieee::quadlet_t;

/// Abbreviated CAN/CAN FD message fixed part - IEEE 1722-2025 Clause 9.4.4, Figure 68
struct AcfCanBriefMessage
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 8;
    /// Payload bounds in quadlets (9.4.4)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 0;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = 16;

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Byte 2: pad[7:6] | mtv[5] | rtr[4] | eff[3] | brs[2] | fdf[1] | esi[0]
    octet_t pad_mtv_flags;

    // Byte 3: rsv[7:5] | can_bus_id[4:0]
    octet_t rsv_bus;

    // Bytes 4-7: rsv[31:29] | can_identifier[28:0]
    quadlet_t identifier;

    // Accessors

    /// Get pad: padding octets at the end of the payload (0 to 3)
    [[nodiscard]] constexpr auto pad() const noexcept -> uint8_t { return pad_mtv_flags.get_bits<uint8_t>(0xC0U, 6); }

    /// Set pad
    constexpr void set_pad(uint8_t const value) noexcept { pad_mtv_flags.set_bits(0xC0U, 6, value); }

    /// Get mtv: message_timestamp valid - always zero on a brief message (9.4.4.1)
    [[nodiscard]] constexpr auto mtv() const noexcept -> bool { return pad_mtv_flags.has_flag(0x20U); }

    /// Set mtv
    constexpr void set_mtv(bool const value) noexcept { pad_mtv_flags.set_flag(0x20U, value); }

    /// Get rtr: remote transmission request (CAN CC only)
    [[nodiscard]] constexpr auto rtr() const noexcept -> bool { return pad_mtv_flags.has_flag(0x10U); }

    /// Set rtr
    constexpr void set_rtr(bool const value) noexcept { pad_mtv_flags.set_flag(0x10U, value); }

    /// Get eff: extended frame format: 29-bit identifier when set, 11-bit when clear
    [[nodiscard]] constexpr auto eff() const noexcept -> bool { return pad_mtv_flags.has_flag(0x08U); }

    /// Set eff
    constexpr void set_eff(bool const value) noexcept { pad_mtv_flags.set_flag(0x08U, value); }

    /// Get brs: bit rate switch (CAN FD only)
    [[nodiscard]] constexpr auto brs() const noexcept -> bool { return pad_mtv_flags.has_flag(0x04U); }

    /// Set brs
    constexpr void set_brs(bool const value) noexcept { pad_mtv_flags.set_flag(0x04U, value); }

    /// Get fdf: CAN FD format: payload may be 0-8, 12, 16, 20, 24, 32, 48 or 64 octets
    [[nodiscard]] constexpr auto fdf() const noexcept -> bool { return pad_mtv_flags.has_flag(0x02U); }

    /// Set fdf
    constexpr void set_fdf(bool const value) noexcept { pad_mtv_flags.set_flag(0x02U, value); }

    /// Get esi: error state indicator (CAN FD only)
    [[nodiscard]] constexpr auto esi() const noexcept -> bool { return pad_mtv_flags.has_flag(0x01U); }

    /// Set esi
    constexpr void set_esi(bool const value) noexcept { pad_mtv_flags.set_flag(0x01U, value); }

    /// Get can_bus_id: CAN bus identifier, 5 bits (application specific, Annex L)
    [[nodiscard]] constexpr auto can_bus_id() const noexcept -> uint8_t { return rsv_bus.get_bits<uint8_t>(0x1FU, 0); }

    /// Set can_bus_id
    constexpr void set_can_bus_id(uint8_t const value) noexcept { rsv_bus.set_bits(0x1FU, 0, value); }

    /// Get can_identifier: CAN identifier, 11 or 29 bits per eff, right-aligned
    [[nodiscard]] constexpr auto can_identifier() const noexcept -> uint32_t
    {
        return identifier.get_bits<uint32_t>(0x1FFFFFFFU, 0);
    }

    /// Set can_identifier
    constexpr void set_can_identifier(uint32_t const value) noexcept { identifier.set_bits(0x1FFFFFFFU, 0, value); }

    // Initialization

    /// Initialize as the smallest ACF_CAN_BRIEF message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::can_brief, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        pad_mtv_flags = 0U;
        rsv_bus = 0U;
        identifier = 0U;
    }

    // Validation

    /// Typed ACF_CAN_BRIEF with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::can_brief) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        return octets >= LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) &&
            octets <= LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS);
    }

    auto operator<=>(AcfCanBriefMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfCanBriefMessage) == AcfCanBriefMessage::LENGTH, "AcfCanBriefMessage must be exactly 8 bytes");
static_assert(alignof(AcfCanBriefMessage) <= 4, "AcfCanBriefMessage alignment must not exceed 4 bytes");
static_assert(offsetof(AcfCanBriefMessage, pad_mtv_flags) == 2, "pad_mtv_flags must be at offset 2");
static_assert(offsetof(AcfCanBriefMessage, rsv_bus) == 3, "rsv_bus must be at offset 3");
static_assert(offsetof(AcfCanBriefMessage, identifier) == 4, "identifier must be at offset 4");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfCanBriefMessage> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_CAN_BRIEF message: fixed part plus payload (pad removed)
using AcfCanBriefMessageView = AcfTypedMessageView<AcfCanBriefMessage>;

/// Parse an ACF_CAN_BRIEF message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_can_brief_parse(std::span<uint8_t const> const data) noexcept -> std::optional<AcfCanBriefMessageView>
{
    return acf_parse_typed<AcfCanBriefMessage>(data);
}

/// Build an ACF_CAN_BRIEF message into @p out from @p fixed and the original @p payload;
/// mtv is forced to zero (brief messages carry no timestamp). Returns the octets
/// written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_can_brief_build(
    std::span<uint8_t> const out, AcfCanBriefMessage fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    fixed.set_mtv(false);
    return acf_build_typed<AcfCanBriefMessage>(out, fixed, payload);
}

}  // namespace statusbar::avtp
