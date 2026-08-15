#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ATDECC Enumeration and Control Protocol (AECP) - IEEE 1722.1 Clause 9
/// Modernized C++23 implementation based on jdksatdecc-c

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>

namespace statusbar::atdecc {

using ieee::doublet_t;
using ieee::Eui64;
using ieee::octet_t;
using statusbar::BufferError;
using statusbar::failure;
using statusbar::StatusValue;
using statusbar::success;

//
// AECP Constants - IEEE 1722.1 Clause 9
//
namespace AvtpSubtype = avtp::AvtpSubtype;

/// Maximum control data length for AECP - Clause 9.2.1.1.7
constexpr uint16_t AECP_MAX_CONTROL_DATA_LENGTH = 524;

/// AECP Message Types - Clause 9.2.1.1.5
constexpr uint8_t AECP_MESSAGE_TYPE_AEM_COMMAND = 0;
constexpr uint8_t AECP_MESSAGE_TYPE_AEM_RESPONSE = 1;
constexpr uint8_t AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND = 2;
constexpr uint8_t AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE = 3;
constexpr uint8_t AECP_MESSAGE_TYPE_AVC_COMMAND = 4;
constexpr uint8_t AECP_MESSAGE_TYPE_AVC_RESPONSE = 5;
constexpr uint8_t AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND = 6;
constexpr uint8_t AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE = 7;
constexpr uint8_t AECP_MESSAGE_TYPE_HDCP_APM_COMMAND = 8;
constexpr uint8_t AECP_MESSAGE_TYPE_HDCP_APM_RESPONSE = 9;
constexpr uint8_t AECP_MESSAGE_TYPE_EXTENDED_COMMAND = 14;
constexpr uint8_t AECP_MESSAGE_TYPE_EXTENDED_RESPONSE = 15;

/// Get human-readable name for AECP message type
/// @param type AECP message type code
[[nodiscard]] auto aecp_message_type_name(uint8_t type) noexcept -> std::string_view;

//
// AECP Status Codes - Clause 9.2.1.1.6
//
constexpr uint8_t AECP_STATUS_SUCCESS = 0;
constexpr uint8_t AECP_STATUS_NOT_IMPLEMENTED = 1;

/// Get human-readable name for AECP status code
/// @param status AECP status code
[[nodiscard]] auto aecp_status_name(uint8_t status) noexcept -> std::string_view;

//
// AECPDU Common Header - Clause 9.2.1.1
// Wire format: 12 byte common control header + 10 byte AECP common = 22 bytes
//
/// ATDECC Enumeration and Control Protocol Data Unit - Common Header
/// This represents the common header for all AECP messages.
/// Packed structure matching IEEE 1722.1 wire format
struct AecpDuCommon
{
    /// Length of common AECP header on wire
    static constexpr size_t LENGTH = 22;

    /// Common control header length
    static constexpr size_t HEADER_LENGTH = 12;

    /// AECP-specific common data length (controller_entity_id + sequence_id)
    static constexpr size_t COMMON_DATA_LENGTH = 10;

    // Common AVTPDU Control Header (12 bytes) - IEEE 1722 Clause 5.3

    /// Byte 0: subtype[7:0]
    octet_t subtype{0};

    /// Byte 1: sv[7] | version[6:4] | message_type[3:0]
    octet_t sv_version_msgtype{0};

    /// Bytes 2-3: status[15:11] | control_data_length[10:0]
    doublet_t status_cdl{0};

    /// Bytes 4-11: Target Entity ID (8 bytes)
    Eui64 target_entity_id{};

    // AECP Common fields (10 bytes) - IEEE 1722.1 Clause 9.2.1.1

    /// Bytes 12-19: Controller Entity ID
    Eui64 controller_entity_id{};

    /// Bytes 20-21: Sequence ID
    doublet_t sequence_id{0};

    // Accessors for packed header fields

    /// Get the stream valid (sv) bit
    [[nodiscard]] constexpr auto sv() const noexcept { return (sv_version_msgtype.get() & 0x80) != 0; }

    /// Get the version field (should be 0 for IEEE 1722.1-2013)
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return (sv_version_msgtype.get() >> 4) & 0x07; }

    /// Get the message type
    [[nodiscard]] constexpr auto message_type() const noexcept -> uint8_t { return sv_version_msgtype.get() & 0x0F; }

    /// Set the message type
    /// @param msg_type AECP message type code
    constexpr void set_message_type(uint8_t const msg_type) noexcept
    {
        sv_version_msgtype = static_cast<uint8_t>((sv_version_msgtype.get() & 0xF0) | (msg_type & 0x0F));
    }

    /// Get the status field
    [[nodiscard]] constexpr auto status() const noexcept -> uint8_t { return status_cdl.get_bits<uint8_t>(0xF800, 11); }

    /// Set the status field
    /// @param stat AECP status code
    constexpr void set_status(uint8_t const stat) noexcept { status_cdl.set_bits(0xF800, 11, stat); }

    /// Get the control data length (11-bit field)
    [[nodiscard]] constexpr auto control_data_length() const noexcept -> uint16_t { return status_cdl.get_bits<uint16_t>(0x07FF); }

    /// Set the control data length
    /// @param len Control data length value (11-bit)
    constexpr void set_control_data_length(uint16_t const len) noexcept { status_cdl.set_bits(0x07FF, 0, len); }

    // Message type helpers

    /// Check if this is a command message (even message types)
    [[nodiscard]] constexpr auto is_command() const noexcept { return (message_type() & 0x01) == 0; }

    /// Check if this is a response message (odd message types)
    [[nodiscard]] constexpr auto is_response() const noexcept { return (message_type() & 0x01) != 0; }

    /// Check if this is an AEM message
    [[nodiscard]] constexpr auto is_aem() const noexcept
    {
        return message_type() == AECP_MESSAGE_TYPE_AEM_COMMAND || message_type() == AECP_MESSAGE_TYPE_AEM_RESPONSE;
    }

    /// Check if this is an Address Access message
    [[nodiscard]] constexpr auto is_address_access() const noexcept
    {
        return message_type() == AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND ||
            message_type() == AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE;
    }

    /// Check if this is an AVC message
    [[nodiscard]] constexpr auto is_avc() const noexcept
    {
        return message_type() == AECP_MESSAGE_TYPE_AVC_COMMAND || message_type() == AECP_MESSAGE_TYPE_AVC_RESPONSE;
    }

    /// Check if this is a Vendor Unique message
    [[nodiscard]] constexpr auto is_vendor_unique() const noexcept
    {
        return message_type() == AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND ||
            message_type() == AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE;
    }

    /// Check if this is an HDCP APM message
    [[nodiscard]] constexpr auto is_hdcp_apm() const noexcept
    {
        return message_type() == AECP_MESSAGE_TYPE_HDCP_APM_COMMAND || message_type() == AECP_MESSAGE_TYPE_HDCP_APM_RESPONSE;
    }

    /// Check if this is an Extended message
    [[nodiscard]] constexpr auto is_extended() const noexcept
    {
        return message_type() == AECP_MESSAGE_TYPE_EXTENDED_COMMAND || message_type() == AECP_MESSAGE_TYPE_EXTENDED_RESPONSE;
    }

    // Initialization

    /// Initialize header fields for a command
    /// @param msg_type AECP message type code
    /// @param data_length Control data length for the message
    constexpr void init_command(uint8_t const msg_type, uint16_t const data_length) noexcept
    {
        subtype = AvtpSubtype::aecp;
        set_message_type(msg_type);
        set_status(AECP_STATUS_SUCCESS);
        set_control_data_length(data_length);
    }

    /// Initialize header fields for a response
    /// @param msg_type AECP message type code
    /// @param stat AECP status code
    /// @param data_length Control data length for the message
    constexpr void init_response(uint8_t const msg_type, uint8_t const stat, uint16_t const data_length) noexcept
    {
        subtype = AvtpSubtype::aecp;
        set_message_type(msg_type);
        set_status(stat);
        set_control_data_length(data_length);
    }

    auto operator<=>(AecpDuCommon const& rhs) const noexcept -> std::strong_ordering = default;

    // Validation

    /// Check if this is a valid AECP PDU common header
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        // Check subtype is AECP (0xFB)
        if (subtype.get() != AvtpSubtype::aecp) {
            return false;
        }
        // Check control data length is at least COMMON_DATA_LENGTH (10 bytes)
        if (control_data_length() < COMMON_DATA_LENGTH) {
            return false;
        }
        // Check control data length doesn't exceed maximum
        if (control_data_length() > AECP_MAX_CONTROL_DATA_LENGTH) {
            return false;
        }
        // Check message type is valid (0-9 or 14-15)
        uint8_t const mt = message_type();
        return mt <= 9 || mt >= 14;
    }
};

// Compile-time layout verification
static_assert(sizeof(AecpDuCommon) == 22, "AecpDuCommon must be exactly 22 bytes");
static_assert(alignof(AecpDuCommon) <= 4, "AecpDuCommon alignment must not exceed 4 bytes");
static_assert(offsetof(AecpDuCommon, subtype) == 0, "subtype must be at offset 0");
static_assert(offsetof(AecpDuCommon, sv_version_msgtype) == 1, "sv_version_msgtype must be at offset 1");
static_assert(offsetof(AecpDuCommon, status_cdl) == 2, "status_cdl must be at offset 2");
static_assert(offsetof(AecpDuCommon, target_entity_id) == 4, "target_entity_id must be at offset 4");
static_assert(offsetof(AecpDuCommon, controller_entity_id) == 12, "controller_entity_id must be at offset 12");
static_assert(offsetof(AecpDuCommon, sequence_id) == 20, "sequence_id must be at offset 20");

}  // namespace statusbar::atdecc

// Serialization traits - AecpDuCommon is a packed wire format struct
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::AecpDuCommon> : std::true_type
{};

// Export using declarations for ADL to find the template functions
namespace statusbar::atdecc {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::atdecc
