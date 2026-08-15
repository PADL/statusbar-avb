#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// MAAP - Multicast Address Allocation Protocol - IEEE 1722 Annex B
/// Modernized C++23 implementation based on jdksatdecc-c

#include "statusbar/avtp/avtp_types.hpp"
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
#include <type_traits>

namespace statusbar::avtp {

using ieee::doublet_t;
using ieee::Eui48;
using ieee::octet_t;
using statusbar::BufferError;
using statusbar::failure;
using statusbar::StatusValue;
using statusbar::success;
using tsn::StreamId;

//
// MAAP Constants - IEEE 1722 Annex B
//
/// MAAP Message Types - Annex B.2.2
constexpr uint8_t MAAP_MESSAGE_TYPE_PROBE = 1;
constexpr uint8_t MAAP_MESSAGE_TYPE_DEFEND = 2;
constexpr uint8_t MAAP_MESSAGE_TYPE_ANNOUNCE = 3;

/// The MAAP protocol version this implementation emits (IEEE 1722-2025 B.2.3:
/// "The current version of MAAP is one (1)").
constexpr uint8_t MAAP_VERSION = 1;

/// Get human-readable name for MAAP message type
/// @param type The MAAP message type value (1-3)
[[nodiscard]] auto maap_message_type_name(uint8_t type) noexcept -> std::string_view;

//
// MAAP Timing Constants - IEEE 1722 Annex B.3.3
//
/// Number of probe retransmissions before claiming an address
constexpr uint32_t MAAP_PROBE_RETRANSMITS = 3;

/// Base probe interval in microseconds (500ms)
constexpr uint64_t MAAP_PROBE_INTERVAL_BASE_US = 500000;

/// Probe interval variation in microseconds (100ms)
constexpr uint64_t MAAP_PROBE_INTERVAL_VARIATION_US = 100000;

/// Base announce interval in microseconds (30s)
constexpr uint64_t MAAP_ANNOUNCE_INTERVAL_BASE_US = 30000000;

/// Announce interval variation in microseconds (2s)
constexpr uint64_t MAAP_ANNOUNCE_INTERVAL_VARIATION_US = 2000000;

//
// MAAP Address Pools - IEEE 1722 Table B.4
//
/// Dynamic allocation pool start address: 91:e0:f0:00:00:00
constexpr Eui48 MAAP_DYNAMIC_POOL_START{0x91, 0xe0, 0xf0, 0x00, 0x00, 0x00};

/// Dynamic allocation pool end address: 91:e0:f0:00:fd:ff
constexpr Eui48 MAAP_DYNAMIC_POOL_END{0x91, 0xe0, 0xf0, 0x00, 0xfd, 0xff};

/// Local allocation pool start address: 91:e0:f0:00:fe:00
constexpr Eui48 MAAP_LOCAL_POOL_START{0x91, 0xe0, 0xf0, 0x00, 0xfe, 0x00};

/// Local allocation pool end address: 91:e0:f0:00:fe:ff
constexpr Eui48 MAAP_LOCAL_POOL_END{0x91, 0xe0, 0xf0, 0x00, 0xfe, 0xff};

//
// MAAPPDU - IEEE 1722 Annex B.2
// Wire format: 12 byte common control header + 16 byte MAAP-specific = 28 bytes
//
/// MAAP Protocol Data Unit
/// Packed structure matching IEEE 1722 wire format
struct MaapDu
{
    /// Total length of MAAPPDU on wire
    static constexpr size_t LENGTH = 28;

    /// Common control header length
    static constexpr size_t HEADER_LENGTH = 12;

    /// MAAP-specific data length (after header)
    static constexpr size_t DATA_LENGTH = 16;

    // Common AVTPDU Control Header (12 bytes) - IEEE 1722 Clause 5.3

    /// Byte 0: subtype[7:0]
    octet_t subtype;

    /// Byte 1: sv[7] | version[6:4] | message_type[3:0]
    octet_t sv_version_msgtype;

    /// Bytes 2-3: maap_version[15:11] | maap_data_length[10:0]
    doublet_t maap_version_datalen;

    /// Bytes 4-11: Stream ID
    StreamId stream_id_;

    // MAAP-specific fields (16 bytes) - IEEE 1722 Annex B.2

    /// Bytes 12-17: Requested start address
    Eui48 requested_start_address;

    /// Bytes 18-19: Requested count
    doublet_t requested_count;

    /// Bytes 20-25: Conflict start address
    Eui48 conflict_start_address;

    /// Bytes 26-27: Conflict count
    doublet_t conflict_count;

    // Accessors for packed header fields

    /// Get the stream valid (sv) bit
    [[nodiscard]] constexpr auto sv() const noexcept -> bool { return (sv_version_msgtype.get() & 0x80) != 0; }

    /// Get the version field
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return (sv_version_msgtype.get() >> 4) & 0x07; }

    /// Get the message type
    [[nodiscard]] constexpr auto message_type() const noexcept -> uint8_t { return sv_version_msgtype.get() & 0x0F; }

    /// Set the message type
    /// @param msg_type The MAAP message type value (1-3)
    constexpr void set_message_type(uint8_t msg_type) noexcept
    {
        sv_version_msgtype = static_cast<uint8_t>((sv_version_msgtype.get() & 0xF0) | (msg_type & 0x0F));
    }

    /// Get the MAAP version field
    [[nodiscard]] constexpr auto maap_version() const noexcept -> uint8_t
    {
        return maap_version_datalen.get_bits<uint8_t>(0xF800, 11);
    }

    /// Set the MAAP version field
    /// @param ver The MAAP version value (5 bits)
    constexpr void set_maap_version(uint8_t ver) noexcept { maap_version_datalen.set_bits(0xF800, 11, ver); }

    /// Get the MAAP data length (11-bit field, should be 16 for MAAP)
    [[nodiscard]] constexpr auto maap_data_length() const noexcept -> uint16_t
    {
        return maap_version_datalen.get_bits<uint16_t>(0x07FF);
    }

    /// Set the MAAP data length
    /// @param len The MAAP data length in bytes (11-bit field)
    constexpr void set_maap_data_length(uint16_t len) noexcept { maap_version_datalen.set_bits(0x07FF, 0, len); }

    /// Get the stream ID
    [[nodiscard]] constexpr auto stream_id() const noexcept -> StreamId { return stream_id_; }

    /// Set the stream ID
    /// @param sid The stream ID to set
    constexpr void set_stream_id(StreamId const& sid) noexcept { stream_id_ = sid; }

    // Message type helpers

    /// Check if this is a Probe message
    [[nodiscard]] constexpr auto is_probe() const noexcept -> bool { return message_type() == MAAP_MESSAGE_TYPE_PROBE; }

    /// Check if this is a Defend message
    [[nodiscard]] constexpr auto is_defend() const noexcept -> bool { return message_type() == MAAP_MESSAGE_TYPE_DEFEND; }

    /// Check if this is an Announce message
    [[nodiscard]] constexpr auto is_announce() const noexcept -> bool { return message_type() == MAAP_MESSAGE_TYPE_ANNOUNCE; }

    // Initialization

    /// Initialize as a Probe message
    /// @param sid The stream ID
    /// @param start_addr Requested start multicast address
    /// @param count Number of addresses requested
    constexpr void init_probe(StreamId const& sid, Eui48 const& start_addr, uint16_t count) noexcept
    {
        subtype = AvtpSubtype::maap;
        sv_version_msgtype = MAAP_MESSAGE_TYPE_PROBE;
        set_maap_version(MAAP_VERSION);
        set_maap_data_length(DATA_LENGTH);
        set_stream_id(sid);
        requested_start_address = start_addr;
        requested_count = count;
        conflict_start_address = Eui48{};
        conflict_count = 0;
    }

    /// Initialize as a Defend message
    /// @param sid The stream ID
    /// @param req_start Requested start multicast address
    /// @param req_count Number of addresses requested
    /// @param conf_start Conflict start multicast address
    /// @param conf_count Number of conflicting addresses
    constexpr void init_defend(
        StreamId const& sid, Eui48 const& req_start, uint16_t req_count, Eui48 const& conf_start, uint16_t conf_count) noexcept
    {
        subtype = AvtpSubtype::maap;
        sv_version_msgtype = MAAP_MESSAGE_TYPE_DEFEND;
        set_maap_version(MAAP_VERSION);
        set_maap_data_length(DATA_LENGTH);
        set_stream_id(sid);
        requested_start_address = req_start;
        requested_count = req_count;
        conflict_start_address = conf_start;
        conflict_count = conf_count;
    }

    /// Initialize as an Announce message
    /// @param sid The stream ID
    /// @param start_addr Claimed start multicast address
    /// @param count Number of addresses claimed
    constexpr void init_announce(StreamId const& sid, Eui48 const& start_addr, uint16_t count) noexcept
    {
        subtype = AvtpSubtype::maap;
        sv_version_msgtype = MAAP_MESSAGE_TYPE_ANNOUNCE;
        set_maap_version(MAAP_VERSION);
        set_maap_data_length(DATA_LENGTH);
        set_stream_id(sid);
        requested_start_address = start_addr;
        requested_count = count;
        conflict_start_address = Eui48{};
        conflict_count = 0;
    }

    // Validation

    /// Check if this is a valid MAAP packet
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        // Check subtype
        if (subtype != AvtpSubtype::maap) {
            return false;
        }
        // Check message type is valid (1-3). IEEE 1722-2025 B.2.2: a MAAP AVTPDU
        // with a reserved message_type shall be ignored. This is the ONLY
        // receiver-side ignore rule the standard mandates.
        // NOTE: control_data_length is NOT validated here. The standard requires a
        // SENDER to set it to 16 (DATA_LENGTH), but the MAAP fields sit at fixed
        // offsets regardless of its value, and B.2.2 only mandates ignoring on a
        // reserved message_type -- so rejecting on control_data_length would be
        // stricter than the standard and could drop a parseable PDU from a
        // slightly-nonconformant sender. Parse it instead (Postel's law).
        uint8_t const msg_type = message_type();
        return msg_type >= 1 && msg_type <= 3;
    }

    auto operator<=>(MaapDu const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(MaapDu) == 28, "MaapDu must be exactly 28 bytes");
static_assert(alignof(MaapDu) <= 4, "MaapDu alignment must not exceed 4 bytes");
static_assert(offsetof(MaapDu, subtype) == 0, "subtype must be at offset 0");
static_assert(offsetof(MaapDu, sv_version_msgtype) == 1, "sv_version_msgtype must be at offset 1");
static_assert(offsetof(MaapDu, maap_version_datalen) == 2, "maap_version_datalen must be at offset 2");
static_assert(offsetof(MaapDu, stream_id_) == 4, "stream_id_ must be at offset 4");
static_assert(offsetof(MaapDu, requested_start_address) == 12, "requested_start_address must be at offset 12");
static_assert(offsetof(MaapDu, requested_count) == 18, "requested_count must be at offset 18");
static_assert(offsetof(MaapDu, conflict_start_address) == 20, "conflict_start_address must be at offset 20");
static_assert(offsetof(MaapDu, conflict_count) == 26, "conflict_count must be at offset 26");

}  // namespace statusbar::avtp

// Serialization traits - MaapDu is a packed wire format struct
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::MaapDu> : std::true_type
{};

// Export using declarations for ADL to find the template functions
namespace statusbar::avtp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::avtp
