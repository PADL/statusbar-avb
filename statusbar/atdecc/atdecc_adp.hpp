#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ATDECC Discovery Protocol (ADP) - IEEE 1722.1 Clause 6
/// Modernized C++23 implementation based on jdksatdecc-c

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"

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
using ieee::quadlet_t;
using statusbar::BufferError;
using statusbar::failure;
using statusbar::StatusValue;
using statusbar::success;
using tsn::ClockIdentity;

//
// ADP Constants - IEEE 1722.1 Clause 6
//
namespace AvtpSubtype = avtp::AvtpSubtype;

/// ADP Message Types - Clause 6.2.1.5
constexpr uint8_t ADP_MESSAGE_TYPE_ENTITY_AVAILABLE = 0;
constexpr uint8_t ADP_MESSAGE_TYPE_ENTITY_DEPARTING = 1;
constexpr uint8_t ADP_MESSAGE_TYPE_ENTITY_DISCOVER = 2;

/// Get human-readable name for ADP message type
/// @param type ADP message type code (0-2)
[[nodiscard]] auto adp_message_type_name(uint8_t type) noexcept -> std::string_view;

//
// Entity Capabilities - Clause 6.2.1.10
//
/// Entity capability flags (32-bit field, bits numbered from MSB)
namespace entity_capabilities {
constexpr uint32_t EFU_MODE = 0x00000001;
constexpr uint32_t ADDRESS_ACCESS_SUPPORTED = 0x00000002;
constexpr uint32_t GATEWAY_ENTITY = 0x00000004;
constexpr uint32_t AEM_SUPPORTED = 0x00000008;
constexpr uint32_t LEGACY_AVC = 0x00000010;
constexpr uint32_t ASSOCIATION_ID_SUPPORTED = 0x00000020;
constexpr uint32_t ASSOCIATION_ID_VALID = 0x00000040;
constexpr uint32_t VENDOR_UNIQUE_SUPPORTED = 0x00000080;
constexpr uint32_t CLASS_A_SUPPORTED = 0x00000100;
constexpr uint32_t CLASS_B_SUPPORTED = 0x00000200;
constexpr uint32_t GPTP_SUPPORTED = 0x00000400;
constexpr uint32_t AEM_AUTHENTICATION_SUPPORTED = 0x00000800;
constexpr uint32_t AEM_AUTHENTICATION_REQUIRED = 0x00001000;
constexpr uint32_t AEM_PERSISTENT_ACQUIRE_SUPPORTED = 0x00002000;
constexpr uint32_t AEM_IDENTIFY_CONTROL_INDEX_VALID = 0x00004000;
constexpr uint32_t AEM_INTERFACE_INDEX_VALID = 0x00008000;
constexpr uint32_t GENERAL_CONTROLLER_IGNORE = 0x00010000;
constexpr uint32_t ENTITY_NOT_READY = 0x00020000;
constexpr uint32_t ACMP_ACQUIRE_WITH_AEM = 0x00040000;
constexpr uint32_t ACMP_AUTHENTICATE_WITH_AEM = 0x00080000;
constexpr uint32_t SUPPORTS_UDPV4_ATDECC = 0x00100000;
constexpr uint32_t SUPPORTS_UDPV4_STREAMING = 0x00200000;
constexpr uint32_t SUPPORTS_UDPV6_ATDECC = 0x00400000;
constexpr uint32_t SUPPORTS_UDPV6_STREAMING = 0x00800000;
constexpr uint32_t MULTIPLE_PTP_INSTANCES = 0x01000000;
constexpr uint32_t AEM_CONFIGURATION_INDEX_VALID = 0x02000000;
}  // namespace entity_capabilities

//
// Talker Capabilities - Clause 6.2.1.12
//
/// Talker capability flags (16-bit field)
namespace talker_capabilities {
constexpr uint16_t IMPLEMENTED = 0x0001;
constexpr uint16_t OTHER_SOURCE = 0x0200;
constexpr uint16_t CONTROL_SOURCE = 0x0400;
constexpr uint16_t MEDIA_CLOCK_SOURCE = 0x0800;
constexpr uint16_t SMPTE_SOURCE = 0x1000;
constexpr uint16_t MIDI_SOURCE = 0x2000;
constexpr uint16_t AUDIO_SOURCE = 0x4000;
constexpr uint16_t VIDEO_SOURCE = 0x8000;
}  // namespace talker_capabilities

//
// Listener Capabilities - Clause 6.2.1.14
//
/// Listener capability flags (16-bit field)
namespace listener_capabilities {
constexpr uint16_t IMPLEMENTED = 0x0001;
constexpr uint16_t OTHER_SINK = 0x0200;
constexpr uint16_t CONTROL_SINK = 0x0400;
constexpr uint16_t MEDIA_CLOCK_SINK = 0x0800;
constexpr uint16_t SMPTE_SINK = 0x1000;
constexpr uint16_t MIDI_SINK = 0x2000;
constexpr uint16_t AUDIO_SINK = 0x4000;
constexpr uint16_t VIDEO_SINK = 0x8000;
}  // namespace listener_capabilities

//
// Controller Capabilities - Clause 6.2.1.15
//
/// Controller capability flags (32-bit field)
namespace controller_capabilities {
constexpr uint32_t IMPLEMENTED = 0x00000001;
}  // namespace controller_capabilities

//
// ADPDU - Clause 6.2.1
// Wire format: 12 byte AVTPDU common control header + 56 byte ADP-specific = 68 bytes
//
/// ATDECC Discovery Protocol Data Unit.
///
/// Packed structure matching the IEEE 1722.1 wire format. The 68-byte layout
/// is identical in IEEE 1722.1-2013 and IEEE 1722.1-2021, so this single
/// struct serializes and deserializes both revisions.
///
/// Field names and offsets follow 1722.1-2021 Clause 6.2.1. The three 2021
/// additions (reserved0 at offset 49, current_configuration_index at 50-51,
/// and the 2021-only entity_capabilities bits) all occupy bytes that a
/// 2013 talker leaves at zero and a 2013 reader ignores, so the 2021
/// layout reads 2013 PDUs losslessly. Whether current_configuration_index
/// is meaningful is controlled by the AEM_CONFIGURATION_INDEX_VALID flag;
/// 2013 talkers leave that flag clear.
struct AdpDu
{
    /// Total length of ADPDU on wire (common header + ADP fields)
    static constexpr size_t LENGTH = 68;

    /// Common control header length
    static constexpr size_t HEADER_LENGTH = 12;

    /// ADP-specific data length (after header)
    static constexpr size_t DATA_LENGTH = LENGTH - HEADER_LENGTH;

    // Common AVTPDU Control Header (12 bytes) - IEEE 1722 Clause 5.3

    /// Byte 0: subtype[7:0]
    octet_t subtype{0};

    /// Byte 1: sv[7] | version[6:4] | message_type[3:0]
    octet_t sv_version_msgtype{0};

    /// Bytes 2-3: valid_time[15:11] | control_data_length[10:0]
    doublet_t valid_time_cdl{0};

    /// Bytes 4-11: Entity ID (8 bytes)
    Eui64 entity_id{};

    // ADP-specific fields (56 bytes) - IEEE 1722.1 Clause 6.2.1

    /// Bytes 12-19: Entity Model ID
    Eui64 entity_model_id{};

    /// Bytes 20-23: Entity Capabilities
    quadlet_t entity_capabilities{0};

    /// Bytes 24-25: Talker Stream Sources count
    doublet_t talker_stream_sources{0};

    /// Bytes 26-27: Talker Capabilities
    doublet_t talker_capabilities{0};

    /// Bytes 28-29: Listener Stream Sinks count
    doublet_t listener_stream_sinks{0};

    /// Bytes 30-31: Listener Capabilities
    doublet_t listener_capabilities{0};

    /// Bytes 32-35: Controller Capabilities
    quadlet_t controller_capabilities{0};

    /// Bytes 36-39: Available Index (incremented on entity state change)
    /// Per Cor1: set to zero for ENTITY_DEPARTING and ENTITY_DISCOVER messages
    quadlet_t available_index{0};

    /// Bytes 40-47: gPTP Grandmaster ID
    ClockIdentity gptp_grandmaster_id{};

    /// Byte 48: gPTP Domain Number
    octet_t gptp_domain_number{0};

    /// Byte 49: Reserved
    octet_t reserved0{0};

    /// Bytes 50-51: Current Configuration Index
    doublet_t current_configuration_index{0};

    /// Bytes 52-53: Identify Control Index
    doublet_t identify_control_index{0};

    /// Bytes 54-55: Interface Index
    doublet_t interface_index{0};

    /// Bytes 56-63: Association ID
    Eui64 association_id{};

    /// Bytes 64-67: Reserved (4 bytes)
    std::array<uint8_t, 4> reserved1{};

    // Accessors for packed header fields

    /// Get the stream valid (sv) bit
    [[nodiscard]] constexpr auto sv() const noexcept { return (sv_version_msgtype.get() & 0x80) != 0; }

    /// Get the version field (0 for both IEEE 1722.1-2013 and IEEE 1722.1-2021)
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return (sv_version_msgtype.get() >> 4) & 0x07; }

    /// Get the message type
    [[nodiscard]] constexpr auto message_type() const noexcept -> uint8_t { return sv_version_msgtype.get() & 0x0F; }

    /// Set the message type
    /// @param msg_type ADP message type code
    constexpr void set_message_type(uint8_t const msg_type) noexcept
    {
        sv_version_msgtype = static_cast<uint8_t>((sv_version_msgtype.get() & 0xF0) | (msg_type & 0x0F));
    }

    /// Get the valid time in 2-second units (0-31)
    [[nodiscard]] constexpr auto valid_time() const noexcept -> uint8_t { return valid_time_cdl.get_bits<uint8_t>(0xF800, 11); }

    /// Set the valid time in 2-second units (0-31)
    /// @param time Valid time value in 2-second units
    constexpr void set_valid_time(uint8_t const time) noexcept { valid_time_cdl.set_bits(0xF800, 11, time); }

    /// Get the control data length (11-bit field, should be 56 for ADP)
    [[nodiscard]] constexpr auto control_data_length() const noexcept -> uint16_t
    {
        return valid_time_cdl.get_bits<uint16_t>(0x07FF);
    }

    /// Set the control data length
    /// @param len Control data length value (11-bit)
    constexpr void set_control_data_length(uint16_t const len) noexcept { valid_time_cdl.set_bits(0x07FF, 0, len); }

    // Capability flag helpers

    /// Check if a specific entity capability is set
    /// @param cap Entity capability flag to check
    [[nodiscard]] constexpr auto has_entity_capability(uint32_t const cap) const noexcept
    {
        return entity_capabilities.has_flag(cap);
    }

    /// Check if a specific talker capability is set
    /// @param cap Talker capability flag to check
    [[nodiscard]] constexpr auto has_talker_capability(uint16_t const cap) const noexcept
    {
        return talker_capabilities.has_flag(cap);
    }

    /// Check if a specific listener capability is set
    /// @param cap Listener capability flag to check
    [[nodiscard]] constexpr auto has_listener_capability(uint16_t const cap) const noexcept
    {
        return listener_capabilities.has_flag(cap);
    }

    /// Check if a specific controller capability is set
    /// @param cap Controller capability flag to check
    [[nodiscard]] constexpr auto has_controller_capability(uint32_t const cap) const noexcept
    {
        return controller_capabilities.has_flag(cap);
    }

    // Message type helpers

    /// Check if this is an Entity Available message
    [[nodiscard]] constexpr auto is_entity_available() const noexcept
    {
        return message_type() == ADP_MESSAGE_TYPE_ENTITY_AVAILABLE;
    }

    /// Check if this is an Entity Departing message
    [[nodiscard]] constexpr auto is_entity_departing() const noexcept
    {
        return message_type() == ADP_MESSAGE_TYPE_ENTITY_DEPARTING;
    }

    /// Check if this is an Entity Discover message
    [[nodiscard]] constexpr auto is_entity_discover() const noexcept { return message_type() == ADP_MESSAGE_TYPE_ENTITY_DISCOVER; }

    // Initialization

    /// Initialize as Entity Available message
    /// @param eid Entity ID for this entity
    /// @param valid_time_2s Valid time in 2-second units (0-31, default 31)
    void init_entity_available(Eui64 const& eid, uint8_t valid_time_2s = 31) noexcept;

    /// Initialize as Entity Departing message
    /// @param eid Entity ID of the departing entity
    void init_entity_departing(Eui64 const& eid) noexcept;

    /// Initialize as Entity Discover message
    void init_entity_discover() noexcept;

    auto operator<=>(AdpDu const& rhs) const noexcept -> std::strong_ordering = default;

    // Validation

    /// Check if this is a valid ADP PDU
    [[nodiscard]] constexpr auto is_valid() const noexcept
    {
        // Check subtype is ADP (0xFA)
        if (subtype.get() != AvtpSubtype::adp) {
            return false;
        }
        // Accept control_data_length >= DATA_LENGTH per IEEE 1722.1 forward-compatibility:
        // newer standard revisions may append additional fields.
        if (control_data_length() < DATA_LENGTH) {
            return false;
        }
        // Check message type is valid (0-2)
        if (message_type() > ADP_MESSAGE_TYPE_ENTITY_DISCOVER) {
            return false;
        }
        return true;
    }
};

// Compile-time layout verification
static_assert(sizeof(AdpDu) == 68, "AdpDu must be exactly 68 bytes");
static_assert(alignof(AdpDu) <= 4, "AdpDu alignment must not exceed 4 bytes");
static_assert(offsetof(AdpDu, subtype) == 0, "subtype must be at offset 0");
static_assert(offsetof(AdpDu, sv_version_msgtype) == 1, "sv_version_msgtype must be at offset 1");
static_assert(offsetof(AdpDu, valid_time_cdl) == 2, "valid_time_cdl must be at offset 2");
static_assert(offsetof(AdpDu, entity_id) == 4, "entity_id must be at offset 4");
static_assert(offsetof(AdpDu, entity_model_id) == 12, "entity_model_id must be at offset 12");
static_assert(offsetof(AdpDu, entity_capabilities) == 20, "entity_capabilities must be at offset 20");
static_assert(offsetof(AdpDu, talker_stream_sources) == 24, "talker_stream_sources must be at offset 24");
static_assert(offsetof(AdpDu, talker_capabilities) == 26, "talker_capabilities must be at offset 26");
static_assert(offsetof(AdpDu, listener_stream_sinks) == 28, "listener_stream_sinks must be at offset 28");
static_assert(offsetof(AdpDu, listener_capabilities) == 30, "listener_capabilities must be at offset 30");
static_assert(offsetof(AdpDu, controller_capabilities) == 32, "controller_capabilities must be at offset 32");
static_assert(offsetof(AdpDu, available_index) == 36, "available_index must be at offset 36");
static_assert(offsetof(AdpDu, gptp_grandmaster_id) == 40, "gptp_grandmaster_id must be at offset 40");
static_assert(offsetof(AdpDu, gptp_domain_number) == 48, "gptp_domain_number must be at offset 48");
static_assert(offsetof(AdpDu, reserved0) == 49, "reserved0 must be at offset 49");
static_assert(offsetof(AdpDu, current_configuration_index) == 50, "current_configuration_index must be at offset 50");
static_assert(offsetof(AdpDu, identify_control_index) == 52, "identify_control_index must be at offset 52");
static_assert(offsetof(AdpDu, interface_index) == 54, "interface_index must be at offset 54");
static_assert(offsetof(AdpDu, association_id) == 56, "association_id must be at offset 56");
static_assert(offsetof(AdpDu, reserved1) == 64, "reserved1 must be at offset 64");

}  // namespace statusbar::atdecc

// Serialization traits - AdpDu is a packed wire format struct
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::AdpDu> : std::true_type
{};

// Export using declarations for ADL to find the template functions
namespace statusbar::atdecc {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::atdecc
