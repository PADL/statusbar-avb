#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// JDKS Vendor-Specific Extensions for ATDECC - Publicly usable definitions under JDKS OUI-36
/// Modernized C++23 implementation based on jdksavdecc-c (J.D. Koftinoff Software, Ltd.)
///
/// J.D. Koftinoff Software, Ltd.'s MAC-S (OUI-36) is: 70-B3-D5-ED-C
///
/// JDKS reserves MAC range 70:B3:D5:ED:CF:F0 to 70:B3:D5:ED:CF:F7 for experimental devices.
///
/// Public multicast addresses:
///   71:B3:D5:ED:CF:FF = JDKS_MULTICAST_LOG for multicasting entity log messages
///
/// Public vendor control types:
///   70:B3:D5:ED:C0:00:00:00 = LOG_TEXT for logging string value control
///   70:B3:D5:ED:C0:00:00:01 = IPV4_PARAMETERS for IPv4 ethernet parameters

#include "statusbar/atdecc/atdecc_aecp.hpp"
#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>
#include <type_traits>

namespace statusbar::atdecc::jdks {

namespace AvtpSubtype = avtp::AvtpSubtype;
using ieee::doublet_t;
using ieee::Eui48;
using ieee::Eui64;
using ieee::octet_t;
using ieee::quadlet_t;
using ieee::store_unchecked;

//
// JDKS Log Priority Levels
//
namespace log_priority {
constexpr uint8_t ERROR = 0;
constexpr uint8_t WARNING = 1;
constexpr uint8_t INFO = 2;
constexpr uint8_t DEBUG1 = 3;
constexpr uint8_t DEBUG2 = 4;
constexpr uint8_t DEBUG3 = 5;
constexpr uint8_t CONSOLE = 0xFF;
}  // namespace log_priority

/// Get human-readable name for log priority
/// @param priority Log priority level
[[nodiscard]] auto log_priority_name(uint8_t priority) noexcept -> std::string_view;

//
// JDKS Well-Known Addresses and Control Types
//
/// Special controller entity ID used for JDKS notifications
/// Only for use with controls with value_type set to CONTROL_VENDOR
inline constexpr Eui64 NOTIFICATIONS_CONTROLLER_ENTITY_ID(0x70, 0xB3, 0xD5, 0xFF, 0xFF, 0xED, 0xCF, 0xFF);

/// Multicast MAC address for logging via unsolicited SET_CONTROL
/// of control descriptor with type LOG_TEXT
inline constexpr Eui48 MULTICAST_LOG(0x71, 0xB3, 0xD5, 0xED, 0xCF, 0xFF);

/// Vendor control type for logging string values (AEM CONTROL_VENDOR blob)
inline constexpr Eui64 CONTROL_LOG_TEXT(0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x00);

/// Vendor control type for IPv4 parameters
inline constexpr Eui64 CONTROL_IPV4_PARAMETERS(0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x01);

//
// JDKS Log Control Constants
//
/// Maximum blob size per AVDECC spec
constexpr size_t BLOB_MAX_SIZE = 362;

/// Maximum text length (blob size - 2 bytes for priority and reserved)
constexpr size_t LOG_MAX_TEXT_LEN = 360;

//
// JDKS Log Control Payload - Vendor Blob Format
// This follows the AEM SET_CONTROL command header
//
/// Log control blob header (after AemControlPayloadHeader)
/// Wire format for JDKS log message vendor blob
struct LogBlobHeader
{
    static constexpr size_t LENGTH = 14;  // 8 + 4 + 1 + 1

    /// Bytes 0-7: Vendor EUI-64 (should be CONTROL_LOG_TEXT)
    Eui64 vendor_eui64;

    /// Bytes 8-11: Blob size (text_length + 2)
    quadlet_t blob_size;

    /// Byte 12: Log priority/detail level
    octet_t log_detail;

    /// Byte 13: Reserved
    octet_t reserved;

    // Followed by UTF-8 text bytes (variable length up to LOG_MAX_TEXT_LEN)

    auto operator<=>(LogBlobHeader const&) const noexcept = default;
};

static_assert(sizeof(LogBlobHeader) == 14, "LogBlobHeader must be 14 bytes");
static_assert(offsetof(LogBlobHeader, vendor_eui64) == 0, "vendor_eui64 must be at offset 0");
static_assert(offsetof(LogBlobHeader, blob_size) == 8, "blob_size must be at offset 8");
static_assert(offsetof(LogBlobHeader, log_detail) == 12, "log_detail must be at offset 12");
static_assert(offsetof(LogBlobHeader, reserved) == 13, "reserved must be at offset 13");

//
// JDKS IPv4 Parameters Control - Vendor Blob Format
//
/// IPv4 parameters flags
namespace ipv4_flags {
constexpr uint32_t STATIC_ENABLE = 0x00000001;
constexpr uint32_t LINK_LOCAL_ENABLE = 0x00000002;
constexpr uint32_t DHCP_ENABLE = 0x00000004;
constexpr uint32_t IPV4_ADDRESS_VALID = 0x00000008;
constexpr uint32_t IPV4_NETMASK_VALID = 0x00000010;
constexpr uint32_t IPV4_GATEWAY_VALID = 0x00000020;
constexpr uint32_t IPV4_BROADCAST_VALID = 0x00000040;
constexpr uint32_t DNSSERVER1_VALID = 0x00000080;
constexpr uint32_t DNSSERVER2_VALID = 0x00000100;
}  // namespace ipv4_flags

/// IPv4 parameters blob (after vendor_eui64 + blob_size header)
struct Ipv4ParamsBlob
{
    static constexpr size_t LENGTH = 32;

    /// Bytes 0-1: Interface descriptor type
    doublet_t interface_descriptor_type;

    /// Bytes 2-3: Interface descriptor index
    doublet_t interface_descriptor_index;

    /// Bytes 4-7: Flags (ipv4_flags)
    quadlet_t flags;

    /// Bytes 8-11: IPv4 address
    quadlet_t ipv4_address;

    /// Bytes 12-15: IPv4 netmask
    quadlet_t ipv4_netmask;

    /// Bytes 16-19: IPv4 gateway
    quadlet_t ipv4_gateway;

    /// Bytes 20-23: IPv4 broadcast
    quadlet_t ipv4_broadcast;

    /// Bytes 24-27: DNS server 1
    quadlet_t ipv4_dnsserver1;

    /// Bytes 28-31: DNS server 2
    quadlet_t ipv4_dnsserver2;

    // Flag accessors
    [[nodiscard]] constexpr auto is_static_enabled() const noexcept -> bool { return flags.has_flag(ipv4_flags::STATIC_ENABLE); }
    [[nodiscard]] constexpr auto is_link_local_enabled() const noexcept -> bool
    {
        return flags.has_flag(ipv4_flags::LINK_LOCAL_ENABLE);
    }
    [[nodiscard]] constexpr auto is_dhcp_enabled() const noexcept -> bool { return flags.has_flag(ipv4_flags::DHCP_ENABLE); }
    [[nodiscard]] constexpr auto is_address_valid() const noexcept -> bool
    {
        return flags.has_flag(ipv4_flags::IPV4_ADDRESS_VALID);
    }
    [[nodiscard]] constexpr auto is_netmask_valid() const noexcept -> bool
    {
        return flags.has_flag(ipv4_flags::IPV4_NETMASK_VALID);
    }
    [[nodiscard]] constexpr auto is_gateway_valid() const noexcept -> bool
    {
        return flags.has_flag(ipv4_flags::IPV4_GATEWAY_VALID);
    }
    [[nodiscard]] constexpr auto is_broadcast_valid() const noexcept -> bool
    {
        return flags.has_flag(ipv4_flags::IPV4_BROADCAST_VALID);
    }

    auto operator<=>(Ipv4ParamsBlob const&) const noexcept = default;
};

static_assert(sizeof(Ipv4ParamsBlob) == 32, "Ipv4ParamsBlob must be 32 bytes");

//
// High-Level Log Message Structures
//
/// High-level representation of a JDKS log control message
/// Used for both log response (unsolicited) and console command
struct LogMessage
{
    Eui64 source_entity_id{};      ///< Entity sending the log
    Eui64 target_entity_id{};      ///< Target entity (may be source for responses)
    Eui64 controller_entity_id{};  ///< Controller entity ID
    uint16_t descriptor_index{0};  ///< CONTROL descriptor index
    uint16_t sequence_id{0};       ///< AECP sequence ID
    uint8_t log_detail{0};         ///< Log priority level
    std::string_view text{};       ///< Log text (UTF-8)
};

/// Context for generating JDKS console commands
/// Contains connection-related parameters that stay constant across messages
struct ConsoleCommandContext
{
    Eui48 dest_mac{};              ///< Destination MAC address
    Eui48 src_mac{};               ///< Source MAC address
    Eui64 my_entity_id{};          ///< Controller entity ID (for commands) or source entity ID (for responses)
    Eui64 target_entity_id{};      ///< Target entity ID
    uint16_t descriptor_index{0};  ///< CONTROL descriptor index
};

//
// Packet Generation Functions
//
/// Generate a JDKS Log PDU as an AEM unsolicited SET_CONTROL response
/// @param buffer Buffer to write the frame into (starting at DA)
/// @param ctx Context containing src_mac, my_entity_id, and descriptor_index
/// @param sequence_id Sequence ID (will be incremented on success)
/// @param log_detail Log priority level
/// @param text UTF-8 log text (max LOG_MAX_TEXT_LEN bytes)
/// @return Number of bytes written, or 0 if buffer too small
/// @note dest_mac is always MULTICAST_LOG for log responses
[[nodiscard]] auto generate_log_response(
    std::span<uint8_t> buffer, ConsoleCommandContext const& ctx, uint16_t& sequence_id, uint8_t log_detail, std::string_view text)
    -> size_t;

/// Generate a JDKS console command as an AEM SET_CONTROL command
/// @param buffer Buffer to write the frame into (starting at DA)
/// @param ctx Context containing dest_mac, src_mac, my_entity_id, target_entity_id, descriptor_index
/// @param sequence_id Sequence ID (will be incremented on success)
/// @param log_detail Log priority (should be CONSOLE for console input)
/// @param text UTF-8 console text (max LOG_MAX_TEXT_LEN bytes)
/// @return Number of bytes written, or 0 if buffer too small
[[nodiscard]] auto generate_console_command(
    std::span<uint8_t> buffer, ConsoleCommandContext const& ctx, uint16_t& sequence_id, uint8_t log_detail, std::string_view text)
    -> size_t;

//
// Packet Parsing Functions
//
/// Check if a received frame is a JDKS log message (unsolicited SET_CONTROL response)
/// @param data Raw frame data starting at DA
/// @param out_blob If not null and returns true, filled with parsed blob header
/// @return true if this is a valid JDKS log message
[[nodiscard]] auto is_log_response(std::span<uint8_t const> data, LogBlobHeader* out_blob = nullptr) noexcept -> bool;

/// Check if a received frame is a JDKS console command (SET_CONTROL command)
/// @param data Raw frame data starting at DA
/// @param out_blob If not null and returns true, filled with parsed blob header
/// @return true if this is a valid JDKS console command
[[nodiscard]] auto is_console_command(std::span<uint8_t const> data, LogBlobHeader* out_blob = nullptr) noexcept -> bool;

/// Parse a JDKS log message from raw frame data
/// @param data Raw frame data starting at DA
/// @param msg Output log message structure
/// @return true if parsing succeeded
[[nodiscard]] auto parse_log_message(std::span<uint8_t const> data, LogMessage& msg) noexcept -> bool;

}  // namespace statusbar::atdecc::jdks

//
// Serialization traits
//
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::jdks::LogBlobHeader> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::jdks::Ipv4ParamsBlob> : std::true_type
{};
