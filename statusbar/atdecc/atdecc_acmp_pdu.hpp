#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACMP Protocol Data Unit wire format structs and state machine support types
/// IEEE 1722.1 Clause 8

#include "statusbar/atdecc/atdecc_acmp_types.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/ip/ip.hpp"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/status.hpp"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>

namespace statusbar::atdecc {

using ieee::doublet_t;
using ieee::Eui48;
using ieee::Eui64;
using ieee::octet_t;
using ip::IPv6Address;
using statusbar::BufferError;
using statusbar::failure;
using statusbar::StatusValue;
using statusbar::success;

//
// ACMPDU - Clause 8.2.1
// Wire format: 12 byte common control header + 44 byte ACMP-specific = 56 bytes
//
/// ATDECC Connection Management Protocol Data Unit (56-byte short form).
///
/// Matches the IEEE 1722.1-2013 wire length (control_data_length = 44) and
/// also the 56-byte prefix of the IEEE 1722.1-2021 extended form. Per
/// 1722.1-2021 Clause 8.2.1 Note 2, a 2021 receiver must still accept the
/// 56-byte short form from 2013-era talkers.
///
/// Field names follow 1722.1-2021: the 2-byte region at offset 54-55 that
/// 1722.1-2013 labels `reserved` is named `connected_listeners_entries`
/// here. A 2013 talker zeros those bytes; a 2021 talker only populates
/// them when the CL_ENTRIES_VALID flag is set, so the naming is lossless
/// in both directions. For the full 2021 extended form with IP addressing
/// fields (control_data_length = 84, total 96 bytes), use AcmpDu2021.
struct AcmpDu
{
    /// Total length of ACMPDU on wire (common header + ACMP fields)
    static constexpr size_t LENGTH = 56;

    /// Common control header length
    static constexpr size_t HEADER_LENGTH = 12;

    /// ACMP-specific data length (after header)
    static constexpr size_t DATA_LENGTH = 44;

    // Common AVTPDU Control Header (12 bytes) - IEEE 1722 Clause 5.3

    /// Byte 0: subtype[7:0]
    octet_t subtype{0};

    /// Byte 1: sv[7] | version[6:4] | message_type[3:0]
    octet_t sv_version_msgtype{0};

    /// Bytes 2-3: status[15:11] | control_data_length[10:0]
    doublet_t status_cdl{0};

    /// Bytes 4-11: Stream ID (8 bytes)
    Eui64 stream_id{};

    // ACMP-specific fields (44 bytes) - IEEE 1722.1 Clause 8.2.1

    /// Bytes 12-19: Controller Entity ID
    Eui64 controller_entity_id{};

    /// Bytes 20-27: Talker Entity ID
    Eui64 talker_entity_id{};

    /// Bytes 28-35: Listener Entity ID
    Eui64 listener_entity_id{};

    /// Bytes 36-37: Talker Unique ID
    doublet_t talker_unique_id{0};

    /// Bytes 38-39: Listener Unique ID
    doublet_t listener_unique_id{0};

    /// Bytes 40-45: Stream Destination MAC Address
    Eui48 stream_dest_mac{};

    /// Bytes 46-47: Connection Count
    doublet_t connection_count{0};

    /// Bytes 48-49: Sequence ID
    doublet_t sequence_id{0};

    /// Bytes 50-51: Flags
    doublet_t flags{0};

    /// Bytes 52-53: Stream VLAN ID
    doublet_t stream_vlan_id{0};

    /// Bytes 54-55: Connected Listeners Entries
    /// Only valid when CL_ENTRIES_VALID flag is set (IEEE 1722.1-2021)
    doublet_t connected_listeners_entries{0};

    // Accessors for packed header fields

    /// Get the stream valid (sv) bit
    [[nodiscard]] constexpr auto sv() const noexcept -> bool { return (sv_version_msgtype.get() & 0x80) != 0; }

    /// Get the version field (0 for both IEEE 1722.1-2013 and IEEE 1722.1-2021)
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return (sv_version_msgtype.get() >> 4) & 0x07; }

    /// Get the message type
    [[nodiscard]] constexpr auto message_type() const noexcept -> uint8_t { return sv_version_msgtype.get() & 0x0F; }

    /// Set the message type
    /// @param msg_type ACMP message type code
    constexpr void set_message_type(uint8_t msg_type) noexcept
    {
        sv_version_msgtype = static_cast<uint8_t>((sv_version_msgtype.get() & 0xF0) | (msg_type & 0x0F));
    }

    /// Get the status field
    [[nodiscard]] constexpr auto status() const noexcept -> uint8_t { return status_cdl.get_bits<uint8_t>(0xF800, 11); }

    /// Set the status field
    /// @param stat ACMP status code
    constexpr void set_status(uint8_t stat) noexcept { status_cdl.set_bits(0xF800, 11, stat); }

    /// Get the control data length (11-bit field; 44 for the 56-byte short form)
    [[nodiscard]] constexpr auto control_data_length() const noexcept -> uint16_t { return status_cdl.get_bits<uint16_t>(0x07FF); }

    /// Set the control data length
    /// @param len Control data length value (11-bit)
    constexpr void set_control_data_length(uint16_t len) noexcept { status_cdl.set_bits(0x07FF, 0, len); }

    // Flag helpers

    /// Check if CLASS_B flag is set
    [[nodiscard]] constexpr auto is_class_b() const noexcept -> bool { return flags.has_flag(acmp_flags::CLASS_B); }

    /// Check if FAST_CONNECT flag is set
    [[nodiscard]] constexpr auto is_fast_connect() const noexcept -> bool { return flags.has_flag(acmp_flags::FAST_CONNECT); }

    /// Check if SAVED_STATE flag is set
    [[nodiscard]] constexpr auto has_saved_state() const noexcept -> bool { return flags.has_flag(acmp_flags::SAVED_STATE); }

    /// Check if STREAMING_WAIT flag is set
    [[nodiscard]] constexpr auto is_streaming_wait() const noexcept -> bool { return flags.has_flag(acmp_flags::STREAMING_WAIT); }

    /// Check if SUPPORTS_ENCRYPTED flag is set
    [[nodiscard]] constexpr auto supports_encrypted() const noexcept -> bool
    {
        return flags.has_flag(acmp_flags::SUPPORTS_ENCRYPTED);
    }

    /// Check if ENCRYPTED_PDU flag is set
    [[nodiscard]] constexpr auto is_encrypted_pdu() const noexcept -> bool { return flags.has_flag(acmp_flags::ENCRYPTED_PDU); }

    /// Check if TALKER_FAILED / SRP_REGISTRATION_FAILED flag is set
    [[nodiscard]] constexpr auto is_talker_failed() const noexcept -> bool { return flags.has_flag(acmp_flags::TALKER_FAILED); }

    /// Check if SRP_REGISTRATION_FAILED flag is set (alias for is_talker_failed)
    [[nodiscard]] constexpr auto is_srp_registration_failed() const noexcept -> bool
    {
        return flags.has_flag(acmp_flags::SRP_REGISTRATION_FAILED);
    }

    /// Check if CL_ENTRIES_VALID flag is set (IEEE 1722.1-2021)
    [[nodiscard]] constexpr auto is_cl_entries_valid() const noexcept -> bool
    {
        return flags.has_flag(acmp_flags::CL_ENTRIES_VALID);
    }

    /// Check if NO_SRP flag is set (IEEE 1722.1-2021)
    [[nodiscard]] constexpr auto is_no_srp() const noexcept -> bool { return flags.has_flag(acmp_flags::NO_SRP); }

    /// Check if UDP flag is set (IEEE 1722.1-2021)
    [[nodiscard]] constexpr auto is_udp() const noexcept -> bool { return flags.has_flag(acmp_flags::UDP); }

    // Message type helpers

    /// Check if this is a command message (even message types)
    [[nodiscard]] constexpr auto is_command() const noexcept -> bool { return (message_type() & 0x01) == 0; }

    /// Check if this is a response message (odd message types)
    [[nodiscard]] constexpr auto is_response() const noexcept -> bool { return (message_type() & 0x01) != 0; }

    // Initialization

    /// Initialize header fields for a command
    /// @param msg_type ACMP message type code
    constexpr void init_command(uint8_t msg_type) noexcept
    {
        subtype = AvtpSubtype::acmp;
        set_message_type(msg_type);
        set_status(ACMP_STATUS_SUCCESS);
        set_control_data_length(DATA_LENGTH);
    }

    /// Initialize header fields for a response
    /// @param msg_type ACMP message type code
    /// @param stat ACMP status code
    constexpr void init_response(uint8_t msg_type, uint8_t stat) noexcept
    {
        subtype = AvtpSubtype::acmp;
        set_message_type(msg_type);
        set_status(stat);
        set_control_data_length(DATA_LENGTH);
    }

    auto operator<=>(AcmpDu const& rhs) const noexcept -> std::strong_ordering = default;

    // Validation

    /// Check if this is a valid ACMP PDU
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        // Check subtype is ACMP (0xFC)
        if (subtype.get() != AvtpSubtype::acmp) {
            return false;
        }
        // Accept control_data_length >= DATA_LENGTH per IEEE 1722.1 forward-compatibility:
        // newer standard revisions may append additional fields.
        if (control_data_length() < DATA_LENGTH) {
            return false;
        }
        // Check message type is valid (0-13)
        if (message_type() > ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE) {
            return false;
        }
        return true;
    }
};

// Compile-time layout verification
static_assert(sizeof(AcmpDu) == 56, "AcmpDu must be exactly 56 bytes");
static_assert(alignof(AcmpDu) <= 4, "AcmpDu alignment must not exceed 4 bytes");
static_assert(offsetof(AcmpDu, subtype) == 0, "subtype must be at offset 0");
static_assert(offsetof(AcmpDu, sv_version_msgtype) == 1, "sv_version_msgtype must be at offset 1");
static_assert(offsetof(AcmpDu, status_cdl) == 2, "status_cdl must be at offset 2");
static_assert(offsetof(AcmpDu, stream_id) == 4, "stream_id must be at offset 4");
static_assert(offsetof(AcmpDu, controller_entity_id) == 12, "controller_entity_id must be at offset 12");
static_assert(offsetof(AcmpDu, talker_entity_id) == 20, "talker_entity_id must be at offset 20");
static_assert(offsetof(AcmpDu, listener_entity_id) == 28, "listener_entity_id must be at offset 28");
static_assert(offsetof(AcmpDu, talker_unique_id) == 36, "talker_unique_id must be at offset 36");
static_assert(offsetof(AcmpDu, listener_unique_id) == 38, "listener_unique_id must be at offset 38");
static_assert(offsetof(AcmpDu, stream_dest_mac) == 40, "stream_dest_mac must be at offset 40");
static_assert(offsetof(AcmpDu, connection_count) == 46, "connection_count must be at offset 46");
static_assert(offsetof(AcmpDu, sequence_id) == 48, "sequence_id must be at offset 48");
static_assert(offsetof(AcmpDu, flags) == 50, "flags must be at offset 50");
static_assert(offsetof(AcmpDu, stream_vlan_id) == 52, "stream_vlan_id must be at offset 52");
static_assert(offsetof(AcmpDu, connected_listeners_entries) == 54, "connected_listeners_entries must be at offset 54");

//
// Extended ACMPDU - IEEE 1722.1-2021 Clause 8.2.1
// Wire format: 12 byte common control header + 84 byte ACMP-specific = 96 bytes
// Adds IP addressing fields for UDP transport support
//
/// Extended ATDECC Connection Management Protocol Data Unit (IEEE 1722.1-2021).
///
/// Full 2021 extended form: control_data_length = 84, total 96 bytes. Adds
/// ip_flags, source/destination ports, and source/destination IPv6 addresses
/// (IPv4 is carried via RFC 4291 IPv4-in-IPv6 encoding) to the 56-byte
/// short form described by AcmpDu. A conformant 2021 talker emits this
/// extended form; a conformant 2021 receiver must still accept the 56-byte
/// short form from 2013 talkers (use is_acmpdu_2013_format() /
/// is_acmpdu_2021_format() to dispatch by control_data_length).
struct AcmpDu2021
{
    /// Total length of extended ACMPDU on wire (common header + ACMP fields)
    static constexpr size_t LENGTH = 96;

    /// Common control header length
    static constexpr size_t HEADER_LENGTH = 12;

    /// ACMP-specific data length (after header) for extended format
    static constexpr size_t DATA_LENGTH = 84;

    // Common AVTPDU Control Header (12 bytes) - IEEE 1722 Clause 5.3

    /// Byte 0: subtype[7:0]
    octet_t subtype{0};

    /// Byte 1: sv[7] | version[6:4] | message_type[3:0]
    octet_t sv_version_msgtype{0};

    /// Bytes 2-3: status[15:11] | control_data_length[10:0]
    doublet_t status_cdl{0};

    /// Bytes 4-11: Stream ID (8 bytes)
    Eui64 stream_id{};

    // ACMP-specific fields (84 bytes) - IEEE 1722.1-2021 Clause 8.2.1

    /// Bytes 12-19: Controller Entity ID
    Eui64 controller_entity_id{};

    /// Bytes 20-27: Talker Entity ID
    Eui64 talker_entity_id{};

    /// Bytes 28-35: Listener Entity ID
    Eui64 listener_entity_id{};

    /// Bytes 36-37: Talker Unique ID
    doublet_t talker_unique_id{0};

    /// Bytes 38-39: Listener Unique ID
    doublet_t listener_unique_id{0};

    /// Bytes 40-45: Stream Destination MAC Address
    Eui48 stream_dest_mac{};

    /// Bytes 46-47: Connection Count
    doublet_t connection_count{0};

    /// Bytes 48-49: Sequence ID
    doublet_t sequence_id{0};

    /// Bytes 50-51: Flags
    doublet_t flags{0};

    /// Bytes 52-53: Stream VLAN ID
    doublet_t stream_vlan_id{0};

    /// Bytes 54-55: Connected Listeners Entries
    doublet_t connected_listeners_entries{0};

    // Extended fields (IEEE 1722.1-2021) - IP addressing support

    /// Bytes 56-57: IP flags — 16-bit field per Clause 8.2.1; no flags defined in 2021, so set to 0.
    doublet_t ip_flags{0};

    /// Bytes 58-59: Reserved (present in the Clause 8.2.1 SVG diagram and required by
    /// control_data_length = 84; omitted from the bullet list in the RST text).
    doublet_t reserved{0};

    /// Bytes 60-61: UDP Source Port
    doublet_t source_port{0};

    /// Bytes 62-63: UDP Destination Port
    doublet_t destination_port{0};

    /// Bytes 64-79: Source IP Address (always IPv6; IPv4 is encoded per RFC 4291 §2.5.5.2)
    IPv6Address source_ip_address{};

    /// Bytes 80-95: Destination IP Address (always IPv6; IPv4 is encoded per RFC 4291 §2.5.5.2)
    IPv6Address destination_ip_address{};

    // Accessors for packed header fields

    /// Get the stream valid (sv) bit
    [[nodiscard]] constexpr auto sv() const noexcept -> bool { return (sv_version_msgtype.get() & 0x80) != 0; }

    /// Get the version field (0 for IEEE 1722.1-2021)
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return (sv_version_msgtype.get() >> 4) & 0x07; }

    /// Get the message type
    [[nodiscard]] constexpr auto message_type() const noexcept -> uint8_t { return sv_version_msgtype.get() & 0x0F; }

    /// Set the message type
    /// @param msg_type ACMP message type code
    constexpr void set_message_type(uint8_t msg_type) noexcept
    {
        sv_version_msgtype = static_cast<uint8_t>((sv_version_msgtype.get() & 0xF0) | (msg_type & 0x0F));
    }

    /// Get the status field
    [[nodiscard]] constexpr auto status() const noexcept -> uint8_t { return status_cdl.get_bits<uint8_t>(0xF800, 11); }

    /// Set the status field
    /// @param stat ACMP status code
    constexpr void set_status(uint8_t stat) noexcept { status_cdl.set_bits(0xF800, 11, stat); }

    /// Get the control data length (11-bit field; 84 for the 2021 extended form)
    [[nodiscard]] constexpr auto control_data_length() const noexcept -> uint16_t { return status_cdl.get_bits<uint16_t>(0x07FF); }

    /// Set the control data length
    /// @param len Control data length value (11-bit)
    constexpr void set_control_data_length(uint16_t len) noexcept { status_cdl.set_bits(0x07FF, 0, len); }

    // Flag helpers

    [[nodiscard]] constexpr auto is_class_b() const noexcept -> bool { return flags.has_flag(acmp_flags::CLASS_B); }
    [[nodiscard]] constexpr auto is_fast_connect() const noexcept -> bool { return flags.has_flag(acmp_flags::FAST_CONNECT); }
    [[nodiscard]] constexpr auto has_saved_state() const noexcept -> bool { return flags.has_flag(acmp_flags::SAVED_STATE); }
    [[nodiscard]] constexpr auto is_streaming_wait() const noexcept -> bool { return flags.has_flag(acmp_flags::STREAMING_WAIT); }
    [[nodiscard]] constexpr auto supports_encrypted() const noexcept -> bool
    {
        return flags.has_flag(acmp_flags::SUPPORTS_ENCRYPTED);
    }
    [[nodiscard]] constexpr auto is_encrypted_pdu() const noexcept -> bool { return flags.has_flag(acmp_flags::ENCRYPTED_PDU); }
    [[nodiscard]] constexpr auto is_talker_failed() const noexcept -> bool { return flags.has_flag(acmp_flags::TALKER_FAILED); }
    [[nodiscard]] constexpr auto is_srp_registration_failed() const noexcept -> bool
    {
        return flags.has_flag(acmp_flags::SRP_REGISTRATION_FAILED);
    }
    [[nodiscard]] constexpr auto is_cl_entries_valid() const noexcept -> bool
    {
        return flags.has_flag(acmp_flags::CL_ENTRIES_VALID);
    }
    [[nodiscard]] constexpr auto is_no_srp() const noexcept -> bool { return flags.has_flag(acmp_flags::NO_SRP); }
    [[nodiscard]] constexpr auto is_udp() const noexcept -> bool { return flags.has_flag(acmp_flags::UDP); }

    // Message type helpers

    [[nodiscard]] constexpr auto is_command() const noexcept -> bool { return (message_type() & 0x01) == 0; }
    [[nodiscard]] constexpr auto is_response() const noexcept -> bool { return (message_type() & 0x01) != 0; }

    // Initialization

    /// Initialize header fields for a command (extended format)
    /// @param msg_type ACMP message type code
    constexpr void init_command(uint8_t msg_type) noexcept
    {
        subtype = AvtpSubtype::acmp;
        set_message_type(msg_type);
        set_status(ACMP_STATUS_SUCCESS);
        set_control_data_length(DATA_LENGTH);
    }

    /// Initialize header fields for a response (extended format)
    /// @param msg_type ACMP message type code
    /// @param stat ACMP status code
    constexpr void init_response(uint8_t msg_type, uint8_t stat) noexcept
    {
        subtype = AvtpSubtype::acmp;
        set_message_type(msg_type);
        set_status(stat);
        set_control_data_length(DATA_LENGTH);
    }

    auto operator<=>(AcmpDu2021 const& rhs) const noexcept -> std::strong_ordering = default;

    // Validation

    /// Check if this is a valid extended ACMP PDU (IEEE 1722.1-2021)
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        // Check subtype is ACMP (0xFC)
        if (subtype.get() != AvtpSubtype::acmp) {
            return false;
        }
        // Accept control_data_length >= DATA_LENGTH per IEEE 1722.1 forward-compatibility:
        // newer standard revisions may append additional fields.
        if (control_data_length() < DATA_LENGTH) {
            return false;
        }
        // Check message type is valid (0-13)
        if (message_type() > ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE) {
            return false;
        }
        return true;
    }
};

// Compile-time layout verification for extended ACMPDU
static_assert(sizeof(AcmpDu2021) == 96, "AcmpDu2021 must be exactly 96 bytes");
static_assert(alignof(AcmpDu2021) <= 4, "AcmpDu2021 alignment must not exceed 4 bytes");
static_assert(offsetof(AcmpDu2021, subtype) == 0, "subtype must be at offset 0");
static_assert(offsetof(AcmpDu2021, sv_version_msgtype) == 1, "sv_version_msgtype must be at offset 1");
static_assert(offsetof(AcmpDu2021, status_cdl) == 2, "status_cdl must be at offset 2");
static_assert(offsetof(AcmpDu2021, stream_id) == 4, "stream_id must be at offset 4");
static_assert(offsetof(AcmpDu2021, controller_entity_id) == 12, "controller_entity_id must be at offset 12");
static_assert(offsetof(AcmpDu2021, talker_entity_id) == 20, "talker_entity_id must be at offset 20");
static_assert(offsetof(AcmpDu2021, listener_entity_id) == 28, "listener_entity_id must be at offset 28");
static_assert(offsetof(AcmpDu2021, talker_unique_id) == 36, "talker_unique_id must be at offset 36");
static_assert(offsetof(AcmpDu2021, listener_unique_id) == 38, "listener_unique_id must be at offset 38");
static_assert(offsetof(AcmpDu2021, stream_dest_mac) == 40, "stream_dest_mac must be at offset 40");
static_assert(offsetof(AcmpDu2021, connection_count) == 46, "connection_count must be at offset 46");
static_assert(offsetof(AcmpDu2021, sequence_id) == 48, "sequence_id must be at offset 48");
static_assert(offsetof(AcmpDu2021, flags) == 50, "flags must be at offset 50");
static_assert(offsetof(AcmpDu2021, stream_vlan_id) == 52, "stream_vlan_id must be at offset 52");
static_assert(offsetof(AcmpDu2021, connected_listeners_entries) == 54, "connected_listeners_entries must be at offset 54");
static_assert(offsetof(AcmpDu2021, ip_flags) == 56, "ip_flags must be at offset 56");
static_assert(offsetof(AcmpDu2021, reserved) == 58, "reserved must be at offset 58");
static_assert(offsetof(AcmpDu2021, source_port) == 60, "source_port must be at offset 60");
static_assert(offsetof(AcmpDu2021, destination_port) == 62, "destination_port must be at offset 62");
static_assert(offsetof(AcmpDu2021, source_ip_address) == 64, "source_ip_address must be at offset 64");
static_assert(offsetof(AcmpDu2021, destination_ip_address) == 80, "destination_ip_address must be at offset 80");

//
// ACMPDU Format Detection
//
/// Check if a control_data_length indicates ACMPDU format (IEEE 1722.1-2013)
/// @param control_data_length Control data length field from the ACMPDU header
[[nodiscard]] constexpr auto is_acmpdu_2013_format(uint16_t control_data_length) noexcept -> bool
{
    return control_data_length == AcmpDu::DATA_LENGTH;
}

/// Check if a control_data_length indicates extended ACMPDU format (IEEE 1722.1-2021)
/// @param control_data_length Control data length field from the ACMPDU header
[[nodiscard]] constexpr auto is_acmpdu_2021_format(uint16_t control_data_length) noexcept -> bool
{
    return control_data_length == AcmpDu2021::DATA_LENGTH;
}

//
// ACMP State Machine Support Types - IEEE 1722.1-2021 Clause 8.2.2
//
/// ACMPCommandResponse - Alias to AcmpDu2021 for state machine use
/// The wire format struct is used directly - accessors handle byte-order conversion
using AcmpCommandResponse = AcmpDu2021;

/// Create an AcmpCommandResponse from an original format AcmpDu
/// Copies common fields and initializes extended fields to zero
/// @param pdu Original format ACMPDU to convert from
[[nodiscard]] auto acmp_command_response_from_pdu(AcmpDu const& pdu) noexcept -> AcmpCommandResponse;

/// Copy common fields from AcmpCommandResponse to an original format AcmpDu
/// @param resp Extended ACMP command/response to convert from
/// @param pdu Original format ACMPDU to populate
auto acmp_command_response_to_pdu(AcmpCommandResponse const& resp, AcmpDu& pdu) noexcept -> void;

/// Serialize an ACMP command/response for Layer-2 transmission, pre-2021 form.
///
/// IEEE 1722.1-2021 grew the ACMPDU to 84/96 bytes (trailing ip_flags/ports/IP
/// fields) and intends that larger PDU to be acceptable even on L2 (the standard's
/// forward-compatibility rule: receivers must tolerate a control_data_length >= the
/// version they know). In practice many shipping devices (e.g. third-party devices) never
/// implemented that and silently drop any ACMPDU longer than the 1722.1-2013
/// 56-byte / control_data_length=44 form. For interoperability we therefore emit
/// the pre-2021 short form on L2. The extended form is only required (and only
/// emitted) when the UDP flag is set, i.e. UDP-encapsulated ACMP.
///
/// @param resp ACMP command/response to serialize
/// @param buffer Output buffer; must be at least AcmpDu2021::LENGTH (96) bytes
/// @return the prefix of @p buffer to place on the wire
[[nodiscard]] auto acmp_serialize_2016(AcmpCommandResponse const& resp, std::span<uint8_t> buffer) noexcept
    -> std::span<uint8_t const>;

/// ListenerStreamInfo - Per-stream state for ATDECC Listener (Clause 8.2.2.1.2)
struct ListenerStreamInfo
{
    Eui64 talker_entity_id{};
    uint16_t talker_unique_id{0};
    bool connected{false};
    Eui64 stream_id{};
    Eui48 stream_dest_mac{};
    Eui64 controller_entity_id{};
    uint16_t flags{0};
    uint16_t stream_vlan_id{0};
    bool pending_connection{false};

    /// Reset all fields to default state
    constexpr void reset() noexcept { *this = ListenerStreamInfo{}; }

    auto operator<=>(ListenerStreamInfo const&) const noexcept = default;
};

/// ListenerPair - Track a connected listener (Clause 8.2.2.1.3)
struct ListenerPair
{
    Eui64 listener_entity_id{};
    uint16_t listener_unique_id{0};

    auto operator<=>(ListenerPair const&) const noexcept = default;
};

/// TalkerStreamInfoDynamic - Per-stream state for ATDECC Talker with inline storage
/// @tparam MaxConnectedListeners Compile-time capacity for connected listeners
template <size_t MaxConnectedListeners = 32>
struct TalkerStreamInfoDynamic
{
    Eui64 stream_id{};
    Eui48 stream_dest_mac{};
    uint16_t stream_vlan_id{0};

    /// Construct with the logical connected-listener limit, clamped to the
    /// compile-time MaxConnectedListeners capacity — a larger request would
    /// otherwise let add_listener() push past the inplace_vector.
    explicit TalkerStreamInfoDynamic(size_t max_connected_listeners = 16)
        : max_connected_listeners_{std::min(max_connected_listeners, MaxConnectedListeners)}
    {}

    /// Get connection count
    [[nodiscard]] auto connection_count() const noexcept -> size_t { return connected_listeners_.size(); }

    /// Get max connected listeners capacity
    [[nodiscard]] auto max_connected_listeners() const noexcept -> size_t { return max_connected_listeners_; }

    /// Add a listener to the connected_listeners
    /// Returns true if added, false if already present or at capacity
    auto add_listener(ListenerPair const& pair) -> bool
    {
        for (auto const& listener : connected_listeners_) {
            if (listener == pair) {
                return false;
            }
        }
        if (connected_listeners_.size() >= max_connected_listeners_) {
            return false;
        }
        connected_listeners_.push_back(pair);
        return true;
    }

    /// Remove a listener from the connected_listeners
    /// Returns true if removed, false if not found
    auto remove_listener(ListenerPair const& pair) noexcept -> bool
    {
        for (auto it = connected_listeners_.begin(); it != connected_listeners_.end(); ++it) {
            if (*it == pair) {
                connected_listeners_.erase(it);
                return true;
            }
        }
        return false;
    }

    /// Check if a listener is in the connected_listeners
    [[nodiscard]] auto contains_listener(ListenerPair const& pair) const noexcept -> bool
    {
        for (auto const& listener : connected_listeners_) {
            if (listener == pair) {
                return true;
            }
        }
        return false;
    }

    /// Get listener at index (for GET_TX_CONNECTION)
    /// Returns nullptr if index out of range
    [[nodiscard]] auto get_listener(size_t index) const noexcept -> ListenerPair const*
    {
        if (index < connected_listeners_.size()) {
            return &connected_listeners_[index];
        }
        return nullptr;
    }

    /// Reset all fields to default state (preserves capacity)
    void reset() noexcept
    {
        stream_id = {};
        stream_dest_mac = {};
        stream_vlan_id = 0;
        connected_listeners_.clear();
    }

  private:
    statusbar::sg14::inplace_vector<ListenerPair, MaxConnectedListeners> connected_listeners_;
    size_t max_connected_listeners_;
};

/// InflightCommand - Track pending commands with timeouts (Clause 8.2.2.1.5)
struct InflightCommand
{
    std::chrono::steady_clock::time_point timeout_time{};
    bool retried{false};
    AcmpCommandResponse command{};
    uint16_t original_sequence_id{0};  // For Listener tracking Controller's sequence_id
    bool valid{false};                 // Explicit validity flag for fixed-size arrays
};

/// ACMPCommandParams - Parameters for Controller to create commands (Clause 8.2.2.1.6)
struct AcmpCommandParams
{
    uint8_t message_type{0};
    Eui64 talker_entity_id{};
    Eui64 listener_entity_id{};
    uint16_t talker_unique_id{0};
    uint16_t listener_unique_id{0};
    uint16_t connection_count{0};
    uint16_t flags{0};
    uint16_t stream_vlan_id{0};
};

}  // namespace statusbar::atdecc

// Serialization traits - AcmpDu and AcmpDuExtended are packed wire format structs
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::AcmpDu> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::AcmpDu2021> : std::true_type
{};

// Export using declarations for ADL to find the template functions
namespace statusbar::atdecc {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::atdecc
