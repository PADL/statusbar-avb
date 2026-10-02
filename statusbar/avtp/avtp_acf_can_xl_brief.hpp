#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_CAN_XL_BRIEF - Abbreviated CAN XL message - IEEE 1722-2025 Clause 9.4.19
/// The CAN XL message (avtp_acf_can_xl.hpp) without the two-quadlet
/// message_timestamp. mtv is transmitted as zero and ignored on receipt
/// (9.4.19.2); the payload is at least one quadlet.
///
/// Wire format (16-byte fixed part + payload quadlets), Figure 93:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Bytes 2-3:   pad[15:14] | mtv[13] | rsv[12:11] | can_bus_id[10:0]
///   Byte 4:      vcid (virtual CAN network id)
///   Byte 5:      sdt (service data unit type)
///   Bytes 6-7:   reserved[15:13] | rrs[12] | sec[11] | priority_id[10:0]
///   Bytes 8-11:  acceptance_field
///   Byte 12:     reserved
///   Byte 13:     transaction_num
///   Bytes 14-15: reserved[15:13] | ms[12] | segment_num[11:0]
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
using ieee::octet_t;
using ieee::quadlet_t;

/// Abbreviated CAN XL message fixed part - IEEE 1722-2025 Clause 9.4.19, Figure 93
struct AcfCanXlBriefMessage
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 16;
    /// Payload bounds in quadlets (9.4.19)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 1;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = ACF_MSG_LENGTH_MAX_QUADLETS - (LENGTH / ACF_QUADLET_OCTETS);

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Bytes 2-3: pad[15:14] | mtv[13] | rsv[12:11] | can_bus_id[10:0]
    doublet_t pad_mtv_rsv_bus;

    // Byte 4: vcid (virtual CAN network id)
    octet_t vcid;

    // Byte 5: sdt (service data unit type)
    octet_t sdt;

    // Bytes 6-7: reserved[15:13] | rrs[12] | sec[11] | priority_id[10:0]
    doublet_t flags_priority;

    // Bytes 8-11: acceptance_field
    quadlet_t acceptance_field;

    // Byte 12: reserved
    octet_t reserved1;

    // Byte 13: transaction_num
    octet_t transaction_num;

    // Bytes 14-15: reserved[15:13] | ms[12] | segment_num[11:0]
    doublet_t ms_segment;

    // Accessors

    /// Get pad: padding octets at the end of the payload (0 to 3; zero when ms is set)
    [[nodiscard]] constexpr auto pad() const noexcept -> uint8_t { return pad_mtv_rsv_bus.get_bits<uint8_t>(0xC000U, 14); }

    /// Set pad
    constexpr void set_pad(uint8_t const value) noexcept { pad_mtv_rsv_bus.set_bits(0xC000U, 14, value); }

    /// Get mtv: message_timestamp valid - always zero on a brief message (9.4.19.2)
    [[nodiscard]] constexpr auto mtv() const noexcept -> bool { return pad_mtv_rsv_bus.has_flag(0x2000U); }

    /// Set mtv
    constexpr void set_mtv(bool const value) noexcept { pad_mtv_rsv_bus.set_flag(0x2000U, value); }

    /// Get can_bus_id: CAN XL bus identifier, 11 bits (application specific, Annex L)
    [[nodiscard]] constexpr auto can_bus_id() const noexcept -> uint16_t { return pad_mtv_rsv_bus.get_bits<uint16_t>(0x07FFU, 0); }

    /// Set can_bus_id
    constexpr void set_can_bus_id(uint16_t const value) noexcept { pad_mtv_rsv_bus.set_bits(0x07FFU, 0, value); }

    /// Get vcid: virtual CAN network identifier
    [[nodiscard]] constexpr auto get_vcid() const noexcept -> uint8_t { return vcid.get(); }

    /// Set vcid
    constexpr void set_vcid(uint8_t const value) noexcept { vcid = value; }

    /// Get sdt: service data unit type
    [[nodiscard]] constexpr auto get_sdt() const noexcept -> uint8_t { return sdt.get(); }

    /// Set sdt
    constexpr void set_sdt(uint8_t const value) noexcept { sdt = value; }

    /// Get rrs: remote request substitution
    [[nodiscard]] constexpr auto rrs() const noexcept -> bool { return flags_priority.has_flag(0x1000U); }

    /// Set rrs
    constexpr void set_rrs(bool const value) noexcept { flags_priority.set_flag(0x1000U, value); }

    /// Get sec: simple extended content
    [[nodiscard]] constexpr auto sec() const noexcept -> bool { return flags_priority.has_flag(0x0800U); }

    /// Set sec
    constexpr void set_sec(bool const value) noexcept { flags_priority.set_flag(0x0800U, value); }

    /// Get priority_id: priority identifier (11 bits)
    [[nodiscard]] constexpr auto priority_id() const noexcept -> uint16_t { return flags_priority.get_bits<uint16_t>(0x07FFU, 0); }

    /// Set priority_id
    constexpr void set_priority_id(uint16_t const value) noexcept { flags_priority.set_bits(0x07FFU, 0, value); }

    /// Get acceptance_field: acceptance field
    [[nodiscard]] constexpr auto get_acceptance_field() const noexcept -> uint32_t { return acceptance_field.get(); }

    /// Set acceptance_field
    constexpr void set_acceptance_field(uint32_t const value) noexcept { acceptance_field = value; }

    /// Get transaction_num: sequence of CAN XL messages; same for every segment of one frame
    [[nodiscard]] constexpr auto get_transaction_num() const noexcept -> uint8_t { return transaction_num.get(); }

    /// Set transaction_num
    constexpr void set_transaction_num(uint8_t const value) noexcept { transaction_num = value; }

    /// Get ms: more segments follow in later AVTPDUs
    [[nodiscard]] constexpr auto ms() const noexcept -> bool { return ms_segment.has_flag(0x1000U); }

    /// Set ms
    constexpr void set_ms(bool const value) noexcept { ms_segment.set_flag(0x1000U, value); }

    /// Get segment_num: 0 = unsegmented, else the 1-based segment number
    [[nodiscard]] constexpr auto segment_num() const noexcept -> uint16_t { return ms_segment.get_bits<uint16_t>(0x0FFFU, 0); }

    /// Set segment_num
    constexpr void set_segment_num(uint16_t const value) noexcept { ms_segment.set_bits(0x0FFFU, 0, value); }

    // Initialization

    /// Initialize as the smallest ACF_CAN_XL_BRIEF message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::can_xl_brief, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        pad_mtv_rsv_bus = 0U;
        vcid = 0U;
        sdt = 0U;
        flags_priority = 0U;
        acceptance_field = 0U;
        reserved1 = 0U;
        transaction_num = 0U;
        ms_segment = 0U;
    }

    // Validation

    /// Typed ACF_CAN_XL_BRIEF with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::can_xl_brief) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        return octets >= LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) &&
            octets <= LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS);
    }

    auto operator<=>(AcfCanXlBriefMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfCanXlBriefMessage) == AcfCanXlBriefMessage::LENGTH, "AcfCanXlBriefMessage must be exactly 16 bytes");
static_assert(alignof(AcfCanXlBriefMessage) <= 4, "AcfCanXlBriefMessage alignment must not exceed 4 bytes");
static_assert(offsetof(AcfCanXlBriefMessage, pad_mtv_rsv_bus) == 2, "pad_mtv_rsv_bus must be at offset 2");
static_assert(offsetof(AcfCanXlBriefMessage, vcid) == 4, "vcid must be at offset 4");
static_assert(offsetof(AcfCanXlBriefMessage, sdt) == 5, "sdt must be at offset 5");
static_assert(offsetof(AcfCanXlBriefMessage, flags_priority) == 6, "flags_priority must be at offset 6");
static_assert(offsetof(AcfCanXlBriefMessage, acceptance_field) == 8, "acceptance_field must be at offset 8");
static_assert(offsetof(AcfCanXlBriefMessage, reserved1) == 12, "reserved1 must be at offset 12");
static_assert(offsetof(AcfCanXlBriefMessage, transaction_num) == 13, "transaction_num must be at offset 13");
static_assert(offsetof(AcfCanXlBriefMessage, ms_segment) == 14, "ms_segment must be at offset 14");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfCanXlBriefMessage> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_CAN_XL_BRIEF message: fixed part plus payload (pad removed)
using AcfCanXlBriefMessageView = AcfTypedMessageView<AcfCanXlBriefMessage>;

/// Parse an ACF_CAN_XL_BRIEF message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_can_xl_brief_parse(std::span<uint8_t const> const data) noexcept
    -> std::optional<AcfCanXlBriefMessageView>
{
    return acf_parse_typed<AcfCanXlBriefMessage>(data);
}

/// Build an ACF_CAN_XL_BRIEF message into @p out from @p fixed and the original @p payload;
/// mtv is forced to zero (brief messages carry no timestamp). Returns the octets
/// written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_can_xl_brief_build(
    std::span<uint8_t> const out, AcfCanXlBriefMessage fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    fixed.set_mtv(false);
    return acf_build_typed<AcfCanXlBriefMessage>(out, fixed, payload);
}

}  // namespace statusbar::avtp
