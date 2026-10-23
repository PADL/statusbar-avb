#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AECP Milan Vendor Unique (MVU) PDUs - Milan v1.2 Sections 5.4.3-5.4.4: an AECP
/// VENDOR_UNIQUE PDU (IEEE 1722.1-2021 Clause 9.2.1.5) with protocol_id 00-1B-C5-0A-C1-00,
/// in the v1.2 layouts. See docs/MVU.md for the wire format and the v1.3 differences.

#include "statusbar/atdecc/atdecc_aecp.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>

namespace statusbar::atdecc::mvu {

using ieee::doublet_t;
using ieee::octet_t;
using ieee::quadlet_t;

// ---------------------------------------------------------------------------
// MVU Constants
// ---------------------------------------------------------------------------

/// MVU protocol_id: Avnu's OUI-36 00-1B-C5-0A-C with the 12-bit protocol
/// identifier 0x100 (Section 5.4.3.2.1)
inline constexpr std::array<uint8_t, 6> MVU_PROTOCOL_ID{0x00, 0x1B, 0xC5, 0x0A, 0xC1, 0x00};

/// MVU command types (Table 5.18). 0x0005-0x7FFF are reserved in v1.2.
constexpr uint16_t MVU_COMMAND_GET_MILAN_INFO = 0x0000;
constexpr uint16_t MVU_COMMAND_SET_SYSTEM_UNIQUE_ID = 0x0001;
constexpr uint16_t MVU_COMMAND_GET_SYSTEM_UNIQUE_ID = 0x0002;
constexpr uint16_t MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO = 0x0003;
constexpr uint16_t MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO = 0x0004;

/// MVU status codes (Table 5.19): 2-31 are reserved in v1.2, so there is no
/// PAYLOAD_TOO_SHORT (that is v1.3).
constexpr uint8_t MVU_STATUS_SUCCESS = 0;
constexpr uint8_t MVU_STATUS_NOT_IMPLEMENTED = 1;

/// MVU command timeout; an entity answers within 240 ms (Section 5.4.3.4)
constexpr uint32_t MVU_TIMEOUT_MS = 250;

/// GET_MILAN_INFO protocol_version (Section 5.4.4.1)
constexpr uint32_t MVU_PROTOCOL_VERSION = 1;

/// GET_MILAN_INFO features_flags (Table 5.20). Milan numbers these bits from
/// the MSB (bit 31 = 0x00000001), so they are written as masks.
constexpr uint32_t MVU_FEATURES_REDUNDANCY = 0x00000001;
constexpr uint32_t MVU_FEATURES_TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING = 0x00000002;

/// SET_MEDIA_CLOCK_REFERENCE_INFO flags (Table 5.21), MSB-numbered as above
constexpr uint8_t MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID = 0x01;
constexpr uint8_t MVU_MCR_FLAGS_MEDIA_CLOCK_DOMAIN_NAME_VALID = 0x02;

/// The shortest AVTPDU in an MVU frame: a 64-octet Ethernet frame (the minimum,
/// FCS included) less the 14-octet header and the 4-octet FCS. Shorter MVU
/// PDUs are padded to this; the padding is not in control_data_length.
constexpr size_t MVU_MIN_PDU_LENGTH = 64 - 14 - 4;

/// GET_MILAN_INFO certification_version: four 8-bit numbers, MSB first, read
/// as a dotted version (Section 5.4.4.1). 0 means not certified.
[[nodiscard]] constexpr auto milan_certification_version(
    uint8_t const major, uint8_t const minor, uint8_t const revision, uint8_t const build) noexcept -> uint32_t
{
    return (uint32_t{major} << 24U) | (uint32_t{minor} << 16U) | (uint32_t{revision} << 8U) | uint32_t{build};
}

// ---------------------------------------------------------------------------
// MVU PDU header (fixed part after AECP common)
// ---------------------------------------------------------------------------

/// MVU AECPDU: AecpDuCommon (22 bytes) + protocol_id (6) + r/command_type (2)
/// = 30 bytes fixed, then command_specific_data.
struct MvuDu
{
    static constexpr size_t LENGTH = 30;

    /// control_data_length of an MVU PDU with no command_specific_data
    static constexpr uint16_t BASE_CONTROL_DATA_LENGTH = LENGTH - AecpDuCommon::HEADER_LENGTH;

    static constexpr uint16_t COMMAND_TYPE_MASK = 0x7FFF;
    static constexpr uint16_t R_MASK = 0x8000;

    // AECP common header (22 bytes)
    AecpDuCommon common{};

    /// Bytes 22-27: protocol_id
    std::array<uint8_t, 6> protocol_id{};

    /// Bytes 28-29: r[15] | command_type[14:0]
    doublet_t r_command_type{0};

    [[nodiscard]] constexpr auto command_type() const noexcept -> uint16_t
    {
        return static_cast<uint16_t>(r_command_type.get() & COMMAND_TYPE_MASK);
    }

    /// Set command_type with r = 0 (r is reserved in v1.2, Section 5.4.3.2.2)
    constexpr void set_command_type(uint16_t const type) noexcept
    {
        r_command_type = static_cast<uint16_t>(type & COMMAND_TYPE_MASK);
    }

    [[nodiscard]] constexpr auto is_mvu_protocol() const noexcept -> bool { return protocol_id == MVU_PROTOCOL_ID; }

    constexpr void set_mvu_protocol() noexcept { protocol_id = MVU_PROTOCOL_ID; }

    /// Initialize as an MVU command carrying @p data_length octets of
    /// command_specific_data
    constexpr void init_command(uint16_t const type, uint16_t const data_length) noexcept
    {
        common.init_command(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND, static_cast<uint16_t>(BASE_CONTROL_DATA_LENGTH + data_length));
        set_mvu_protocol();
        set_command_type(type);
    }

    /// Initialize as an MVU response carrying @p data_length octets of
    /// command_specific_data
    constexpr void init_response(uint16_t const type, uint8_t const status, uint16_t const data_length) noexcept
    {
        common.init_response(
            AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE, status, static_cast<uint16_t>(BASE_CONTROL_DATA_LENGTH + data_length));
        set_mvu_protocol();
        set_command_type(type);
    }

    auto operator<=>(MvuDu const&) const noexcept = default;
};

static_assert(sizeof(MvuDu) == 30, "MvuDu must be exactly 30 bytes");
static_assert(offsetof(MvuDu, common) == 0, "common must be at offset 0");
static_assert(offsetof(MvuDu, protocol_id) == 22, "protocol_id must be at offset 22");
static_assert(offsetof(MvuDu, r_command_type) == 28, "r_command_type must be at offset 28");

/// True when @p pdu is an AECP VENDOR_UNIQUE command carrying the MVU
/// protocol_id, with room (received and declared) for protocol_id and
/// command_type. Says nothing about the target or the command_specific_data.
[[nodiscard]] inline auto is_mvu_command(std::span<uint8_t const> const pdu) noexcept -> bool
{
    if (pdu.size() < MvuDu::LENGTH) {
        return false;
    }
    MvuDu du{};
    span_load(du, pdu.first(MvuDu::LENGTH));
    return du.common.subtype.get() == AvtpSubtype::aecp && du.common.message_type() == AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND &&
        du.common.control_data_length() >= MvuDu::BASE_CONTROL_DATA_LENGTH && du.is_mvu_protocol();
}

// ---------------------------------------------------------------------------
// Command payloads (command_specific_data, after MvuDu) - Section 5.4.4
// ---------------------------------------------------------------------------

/// GET_MILAN_INFO command and GET_SYSTEM_UNIQUE_ID command (Figure 5.3)
struct MvuGetMilanInfoCommand
{
    static constexpr size_t LENGTH = 2;

    /// Bytes 30-31: reserved (0; ignored by the receiver)
    doublet_t reserved{0};

    auto operator<=>(MvuGetMilanInfoCommand const&) const noexcept = default;
};

static_assert(sizeof(MvuGetMilanInfoCommand) == 2, "MvuGetMilanInfoCommand must be 2 bytes");

/// GET_MILAN_INFO response (Figure 5.4). v1.2 has no specification_version.
struct MvuGetMilanInfoResponse
{
    static constexpr size_t LENGTH = 14;

    /// Bytes 30-31: reserved
    doublet_t reserved{0};

    /// Bytes 32-35: protocol_version (MVU_PROTOCOL_VERSION)
    quadlet_t protocol_version{0};

    /// Bytes 36-39: features_flags (MVU_FEATURES_*)
    quadlet_t features_flags{0};

    /// Bytes 40-43: certification_version (milan_certification_version, 0 if none)
    quadlet_t certification_version{0};

    auto operator<=>(MvuGetMilanInfoResponse const&) const noexcept = default;
};

static_assert(sizeof(MvuGetMilanInfoResponse) == 14, "MvuGetMilanInfoResponse must be 14 bytes");

/// SET_SYSTEM_UNIQUE_ID command and response, GET_SYSTEM_UNIQUE_ID response
/// (Figure 5.5). The v1.2 text calls system_unique_id "64 bit" but Figure 5.5
/// draws one quadlet, and GenAVB's v1.2 layout is a u32: the figure is followed.
struct MvuSystemUniqueIdPayload
{
    static constexpr size_t LENGTH = 6;

    /// Bytes 30-31: reserved
    doublet_t reserved{0};

    /// Bytes 32-35: system_unique_id (default 0; SET to 0 is invalid)
    quadlet_t system_unique_id{0};

    auto operator<=>(MvuSystemUniqueIdPayload const&) const noexcept = default;
};

static_assert(sizeof(MvuSystemUniqueIdPayload) == 6, "MvuSystemUniqueIdPayload must be 6 bytes");

/// GET_MEDIA_CLOCK_REFERENCE_INFO command (Figure 5.7)
struct MvuGetMediaClockReferenceInfoCommand
{
    static constexpr size_t LENGTH = 2;

    /// Bytes 30-31: CLOCK_DOMAIN descriptor index
    doublet_t clock_domain_index{0};

    auto operator<=>(MvuGetMediaClockReferenceInfoCommand const&) const noexcept = default;
};

static_assert(sizeof(MvuGetMediaClockReferenceInfoCommand) == 2, "MvuGetMediaClockReferenceInfoCommand must be 2 bytes");

/// SET_MEDIA_CLOCK_REFERENCE_INFO command and response,
/// GET_MEDIA_CLOCK_REFERENCE_INFO response (Figure 5.6)
struct MvuMediaClockReferenceInfoPayload
{
    static constexpr size_t LENGTH = 74;

    /// Bytes 30-31: CLOCK_DOMAIN descriptor index
    doublet_t clock_domain_index{0};

    /// Byte 32: flags (MVU_MCR_FLAGS_*)
    octet_t flags{0};

    /// Byte 33: reserved
    octet_t reserved1{0};

    /// Byte 34: default_mcr_prio (ignored in a command)
    octet_t default_mcr_prio{0};

    /// Byte 35: user_mcr_prio (default: default_mcr_prio)
    octet_t user_mcr_prio{0};

    /// Bytes 36-39: reserved
    quadlet_t reserved2{0};

    /// Bytes 40-103: media_clock_domain_name (UTF-8, zero padded, no trailing
    /// NUL when 64 bytes long; default "DEFAULT")
    std::array<char, 64> media_clock_domain_name{};

    auto operator<=>(MvuMediaClockReferenceInfoPayload const&) const noexcept = default;
};

static_assert(sizeof(MvuMediaClockReferenceInfoPayload) == 74, "MvuMediaClockReferenceInfoPayload must be 74 bytes");

}  // namespace statusbar::atdecc::mvu

// Serialization traits
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::mvu::MvuDu> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::mvu::MvuGetMilanInfoCommand> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::mvu::MvuGetMilanInfoResponse> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::mvu::MvuSystemUniqueIdPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::mvu::MvuGetMediaClockReferenceInfoCommand>
    : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::mvu::MvuMediaClockReferenceInfoPayload>
    : std::true_type
{};

namespace statusbar::atdecc::mvu {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::atdecc::mvu
