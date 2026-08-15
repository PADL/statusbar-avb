#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ATDECC Entity Model (AEM) Protocol - IEEE 1722.1 Clause 7 & 9.2.1.2
/// Modernized C++23 implementation based on jdksatdecc-c

#include "statusbar/atdecc/atdecc_aecp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <chrono>
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
namespace AvtpSubtype = statusbar::avtp::AvtpSubtype;

//
// AEM Status Codes - Clause 7.4
//
constexpr uint8_t AEM_STATUS_SUCCESS = 0;
constexpr uint8_t AEM_STATUS_NOT_IMPLEMENTED = 1;
constexpr uint8_t AEM_STATUS_NO_SUCH_DESCRIPTOR = 2;
constexpr uint8_t AEM_STATUS_ENTITY_LOCKED = 3;
constexpr uint8_t AEM_STATUS_ENTITY_ACQUIRED = 4;
constexpr uint8_t AEM_STATUS_NOT_AUTHENTICATED = 5;
constexpr uint8_t AEM_STATUS_AUTHENTICATION_DISABLED = 6;
constexpr uint8_t AEM_STATUS_BAD_ARGUMENTS = 7;
constexpr uint8_t AEM_STATUS_NO_RESOURCES = 8;
constexpr uint8_t AEM_STATUS_IN_PROGRESS = 9;
constexpr uint8_t AEM_STATUS_ENTITY_MISBEHAVING = 10;
constexpr uint8_t AEM_STATUS_NOT_SUPPORTED = 11;
constexpr uint8_t AEM_STATUS_STREAM_IS_RUNNING = 12;

/// Get human-readable name for AEM status code
/// @param status AEM status code
[[nodiscard]] auto aem_status_name(uint8_t status) noexcept -> std::string_view;

//
// AEM Timeouts - Clause 9.2.1.2.5
//
constexpr uint32_t AEM_TIMEOUT_MS = 250;
constexpr uint32_t AEM_IN_PROGRESS_TIMEOUT_MS = 120;
constexpr uint32_t AEM_LOCK_TIMEOUT_MS = 60000;

//
// Maximum descriptor size that fits in a READ_DESCRIPTOR response — IEEE 1722.1 Clause 9.2.2.6
//
// Derivation:
//   AECP_MAX_CONTROL_DATA_LENGTH          = 524 bytes  (Clause 9.2.1.1.7)
//   minus AemDu::AEM_DATA_LENGTH          = 12  bytes  (AEM-specific header after CDL)
//   minus READ_DESCRIPTOR response header = 4   bytes  (configuration_index:2 + reserved:2)
//   = 508 bytes available for the descriptor payload itself.
//
// Every descriptor that can be returned via READ_DESCRIPTOR must fit within this limit,
// including any variable-length trailer (e.g. Configuration descriptor_counts, AudioMap
// mappings, Stream stream_formats, etc.). Descriptor structs report their actual on-wire
// length via wire_size().
constexpr size_t MAX_AEM_DESCRIPTOR_SIZE = 508;

//
// AEM Command Codes - Clause 7.4
//
constexpr uint16_t AEM_COMMAND_ACQUIRE_ENTITY = 0x0000;
constexpr uint16_t AEM_COMMAND_LOCK_ENTITY = 0x0001;
constexpr uint16_t AEM_COMMAND_ENTITY_AVAILABLE = 0x0002;
constexpr uint16_t AEM_COMMAND_CONTROLLER_AVAILABLE = 0x0003;
constexpr uint16_t AEM_COMMAND_READ_DESCRIPTOR = 0x0004;
constexpr uint16_t AEM_COMMAND_WRITE_DESCRIPTOR = 0x0005;
constexpr uint16_t AEM_COMMAND_SET_CONFIGURATION = 0x0006;
constexpr uint16_t AEM_COMMAND_GET_CONFIGURATION = 0x0007;
constexpr uint16_t AEM_COMMAND_SET_STREAM_FORMAT = 0x0008;
constexpr uint16_t AEM_COMMAND_GET_STREAM_FORMAT = 0x0009;
constexpr uint16_t AEM_COMMAND_SET_VIDEO_FORMAT = 0x000A;
constexpr uint16_t AEM_COMMAND_GET_VIDEO_FORMAT = 0x000B;
constexpr uint16_t AEM_COMMAND_SET_SENSOR_FORMAT = 0x000C;
constexpr uint16_t AEM_COMMAND_GET_SENSOR_FORMAT = 0x000D;
constexpr uint16_t AEM_COMMAND_SET_STREAM_INFO = 0x000E;
constexpr uint16_t AEM_COMMAND_GET_STREAM_INFO = 0x000F;
constexpr uint16_t AEM_COMMAND_SET_NAME = 0x0010;
constexpr uint16_t AEM_COMMAND_GET_NAME = 0x0011;
constexpr uint16_t AEM_COMMAND_SET_ASSOCIATION_ID = 0x0012;
constexpr uint16_t AEM_COMMAND_GET_ASSOCIATION_ID = 0x0013;
constexpr uint16_t AEM_COMMAND_SET_SAMPLING_RATE = 0x0014;
constexpr uint16_t AEM_COMMAND_GET_SAMPLING_RATE = 0x0015;
constexpr uint16_t AEM_COMMAND_SET_CLOCK_SOURCE = 0x0016;
constexpr uint16_t AEM_COMMAND_GET_CLOCK_SOURCE = 0x0017;
constexpr uint16_t AEM_COMMAND_SET_CONTROL = 0x0018;
constexpr uint16_t AEM_COMMAND_GET_CONTROL = 0x0019;
constexpr uint16_t AEM_COMMAND_INCREMENT_CONTROL = 0x001A;
constexpr uint16_t AEM_COMMAND_DECREMENT_CONTROL = 0x001B;
constexpr uint16_t AEM_COMMAND_SET_SIGNAL_SELECTOR = 0x001C;
constexpr uint16_t AEM_COMMAND_GET_SIGNAL_SELECTOR = 0x001D;
constexpr uint16_t AEM_COMMAND_SET_MIXER = 0x001E;
constexpr uint16_t AEM_COMMAND_GET_MIXER = 0x001F;
constexpr uint16_t AEM_COMMAND_SET_MATRIX = 0x0020;
constexpr uint16_t AEM_COMMAND_GET_MATRIX = 0x0021;
constexpr uint16_t AEM_COMMAND_START_STREAMING = 0x0022;
constexpr uint16_t AEM_COMMAND_STOP_STREAMING = 0x0023;
constexpr uint16_t AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION = 0x0024;
constexpr uint16_t AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION = 0x0025;
constexpr uint16_t AEM_COMMAND_IDENTIFY_NOTIFICATION = 0x0026;
constexpr uint16_t AEM_COMMAND_GET_AVB_INFO = 0x0027;
constexpr uint16_t AEM_COMMAND_GET_AS_PATH = 0x0028;
constexpr uint16_t AEM_COMMAND_GET_COUNTERS = 0x0029;
constexpr uint16_t AEM_COMMAND_REBOOT = 0x002A;
constexpr uint16_t AEM_COMMAND_GET_AUDIO_MAP = 0x002B;
constexpr uint16_t AEM_COMMAND_ADD_AUDIO_MAPPINGS = 0x002C;
constexpr uint16_t AEM_COMMAND_REMOVE_AUDIO_MAPPINGS = 0x002D;
constexpr uint16_t AEM_COMMAND_GET_VIDEO_MAP = 0x002E;
constexpr uint16_t AEM_COMMAND_ADD_VIDEO_MAPPINGS = 0x002F;
constexpr uint16_t AEM_COMMAND_REMOVE_VIDEO_MAPPINGS = 0x0030;
constexpr uint16_t AEM_COMMAND_GET_SENSOR_MAP = 0x0031;
constexpr uint16_t AEM_COMMAND_ADD_SENSOR_MAPPINGS = 0x0032;
constexpr uint16_t AEM_COMMAND_REMOVE_SENSOR_MAPPINGS = 0x0033;
constexpr uint16_t AEM_COMMAND_START_OPERATION = 0x0034;
constexpr uint16_t AEM_COMMAND_ABORT_OPERATION = 0x0035;
constexpr uint16_t AEM_COMMAND_OPERATION_STATUS = 0x0036;
constexpr uint16_t AEM_COMMAND_AUTH_ADD_KEY = 0x0037;
constexpr uint16_t AEM_COMMAND_AUTH_DELETE_KEY = 0x0038;
constexpr uint16_t AEM_COMMAND_AUTH_GET_KEY_LIST = 0x0039;
constexpr uint16_t AEM_COMMAND_AUTH_GET_KEY = 0x003A;
constexpr uint16_t AEM_COMMAND_AUTH_ADD_KEY_TO_CHAIN = 0x003B;
constexpr uint16_t AEM_COMMAND_AUTH_DELETE_KEY_FROM_CHAIN = 0x003C;
constexpr uint16_t AEM_COMMAND_AUTH_GET_KEYCHAIN_LIST = 0x003D;
constexpr uint16_t AEM_COMMAND_AUTH_GET_IDENTITY = 0x003E;
constexpr uint16_t AEM_COMMAND_AUTH_ADD_TOKEN = 0x003F;
constexpr uint16_t AEM_COMMAND_AUTH_DELETE_TOKEN = 0x0040;
constexpr uint16_t AEM_COMMAND_AUTHENTICATE = 0x0041;
constexpr uint16_t AEM_COMMAND_DEAUTHENTICATE = 0x0042;
constexpr uint16_t AEM_COMMAND_ENABLE_TRANSPORT_SECURITY = 0x0043;
constexpr uint16_t AEM_COMMAND_DISABLE_TRANSPORT_SECURITY = 0x0044;
constexpr uint16_t AEM_COMMAND_ENABLE_STREAM_ENCRYPTION = 0x0045;
constexpr uint16_t AEM_COMMAND_DISABLE_STREAM_ENCRYPTION = 0x0046;
constexpr uint16_t AEM_COMMAND_SET_MEMORY_OBJECT_LENGTH = 0x0047;
constexpr uint16_t AEM_COMMAND_GET_MEMORY_OBJECT_LENGTH = 0x0048;
constexpr uint16_t AEM_COMMAND_SET_STREAM_BACKUP = 0x0049;
constexpr uint16_t AEM_COMMAND_GET_STREAM_BACKUP = 0x004A;
constexpr uint16_t AEM_COMMAND_GET_DYNAMIC_INFO = 0x004B;
constexpr uint16_t AEM_COMMAND_SET_MAX_TRANSIT_TIME = 0x004C;
constexpr uint16_t AEM_COMMAND_GET_MAX_TRANSIT_TIME = 0x004D;
constexpr uint16_t AEM_COMMAND_SET_SAMPLING_RATE_RANGE = 0x004E;
constexpr uint16_t AEM_COMMAND_GET_SAMPLING_RATE_RANGE = 0x004F;
constexpr uint16_t AEM_COMMAND_SET_PTP_INSTANCE_INFO = 0x0050;
constexpr uint16_t AEM_COMMAND_GET_PTP_INSTANCE_INFO = 0x0051;
constexpr uint16_t AEM_COMMAND_GET_PTP_INSTANCE_EXTENDED_INFO = 0x0052;
constexpr uint16_t AEM_COMMAND_GET_PTP_INSTANCE_GRANDMASTER_INFO = 0x0053;
constexpr uint16_t AEM_COMMAND_GET_PTP_INSTANCE_PATH_COUNT = 0x0054;
constexpr uint16_t AEM_COMMAND_GET_PTP_INSTANCE_PATH_TRACE = 0x0055;
constexpr uint16_t AEM_COMMAND_GET_PTP_INSTANCE_PERF_MON_COUNT = 0x0056;
constexpr uint16_t AEM_COMMAND_GET_PTP_INSTANCE_PERF_MON_RECORD = 0x0057;
constexpr uint16_t AEM_COMMAND_SET_PTP_PORT_INITIAL_INTERVALS = 0x0058;
constexpr uint16_t AEM_COMMAND_GET_PTP_PORT_INITIAL_INTERVALS = 0x0059;
// 0x005A reserved (SET_PTP_PORT_CURRENT_INTERVALS removed in IEEE 1722.1-2021)
constexpr uint16_t AEM_COMMAND_GET_PTP_PORT_CURRENT_INTERVALS = 0x005B;
constexpr uint16_t AEM_COMMAND_SET_PTP_PORT_REMOTE_INTERVALS = 0x005C;
constexpr uint16_t AEM_COMMAND_GET_PTP_PORT_REMOTE_INTERVALS = 0x005D;
constexpr uint16_t AEM_COMMAND_SET_PTP_PORT_INFO = 0x005E;
constexpr uint16_t AEM_COMMAND_GET_PTP_PORT_INFO = 0x005F;
constexpr uint16_t AEM_COMMAND_SET_PTP_PORT_OVERRIDES = 0x0060;
constexpr uint16_t AEM_COMMAND_GET_PTP_PORT_OVERRIDES = 0x0061;
constexpr uint16_t AEM_COMMAND_GET_PTP_PORT_PDELAY_MON_COUNT = 0x0062;
constexpr uint16_t AEM_COMMAND_GET_PTP_PORT_PDELAY_MON_RECORD = 0x0063;
constexpr uint16_t AEM_COMMAND_GET_PTP_PORT_PERF_MON_COUNT = 0x0064;
constexpr uint16_t AEM_COMMAND_GET_PTP_PORT_PERF_MON_RECORD = 0x0065;
constexpr uint16_t AEM_COMMAND_GET_PATH_LATENCY = 0x0066;
constexpr uint16_t AEM_COMMAND_AUTH_GET_NONCE = 0x0067;
constexpr uint16_t AEM_COMMAND_AUTH_ADD_KEY_NONCE = 0x0068;
constexpr uint16_t AEM_COMMAND_EXPANSION = 0x3FFF;

/// Get human-readable name for AEM command code
/// @param cmd AEM command code (15-bit)
[[nodiscard]] auto aem_command_name(uint16_t cmd) noexcept -> std::string_view;

//
// AECPDU AEM Header - Clause 9.2.1.2
// Wire format: 12 byte common control header + 12 byte AEM-specific = 24 bytes
// Note: AvtpSubtype and AECP_MESSAGE_TYPE_AEM_* are defined in :aecp module
//
/// ATDECC Entity Model Protocol Data Unit Header
/// Packed structure matching IEEE 1722.1 wire format
struct AemDu
{
    /// Length of AEM header on wire
    static constexpr size_t LENGTH = 24;

    /// Common control header length
    static constexpr size_t HEADER_LENGTH = 12;

    /// AEM-specific data length (controller_entity_id + sequence_id + command_type)
    static constexpr size_t AEM_DATA_LENGTH = 12;

    // Common AVTPDU Control Header (12 bytes) - IEEE 1722 Clause 5.3

    /// Byte 0: subtype[7:0]
    octet_t subtype{0};

    /// Byte 1: sv[7] | version[6:4] | message_type[3:0]
    octet_t sv_version_msgtype{0};

    /// Bytes 2-3: status[15:11] | control_data_length[10:0]
    doublet_t status_cdl{0};

    /// Bytes 4-11: Target Entity ID (8 bytes)
    Eui64 target_entity_id{};

    // AEM-specific fields (12 bytes) - IEEE 1722.1 Clause 9.2.1.2

    /// Bytes 12-19: Controller Entity ID
    Eui64 controller_entity_id{};

    /// Bytes 20-21: Sequence ID
    doublet_t sequence_id{0};

    /// Bytes 22-23: u[15] | command_type[14:0]
    doublet_t command_type{0};

    // Accessors for packed header fields

    /// Get the stream valid (sv) bit
    [[nodiscard]] constexpr auto sv() const noexcept -> bool { return (sv_version_msgtype.get() & 0x80) != 0; }

    /// Get the version field (should be 0 for IEEE 1722.1-2013)
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return (sv_version_msgtype.get() >> 4) & 0x07; }

    /// Get the message type
    [[nodiscard]] constexpr auto message_type() const noexcept -> uint8_t { return sv_version_msgtype.get() & 0x0F; }

    /// Set the message type
    /// @param msg_type AECP message type code
    constexpr void set_message_type(uint8_t msg_type) noexcept
    {
        sv_version_msgtype = static_cast<uint8_t>((sv_version_msgtype.get() & 0xF0) | (msg_type & 0x0F));
    }

    /// Get the status field
    [[nodiscard]] constexpr auto status() const noexcept -> uint8_t { return status_cdl.get_bits<uint8_t>(0xF800, 11); }

    /// Set the status field
    /// @param stat AEM status code
    constexpr void set_status(uint8_t stat) noexcept { status_cdl.set_bits(0xF800, 11, stat); }

    /// Get the control data length (11-bit field)
    [[nodiscard]] constexpr auto control_data_length() const noexcept -> uint16_t { return status_cdl.get_bits<uint16_t>(0x07FF); }

    /// Set the control data length
    /// @param len Control data length value (11-bit)
    constexpr void set_control_data_length(uint16_t len) noexcept { status_cdl.set_bits(0x07FF, 0, len); }

    /// Get the unsolicited (U) bit from command_type field
    [[nodiscard]] constexpr auto is_unsolicited() const noexcept -> bool
    {
        return command_type.has_flag(static_cast<uint16_t>(0x8000));
    }

    /// Set the unsolicited (U) bit
    /// @param u True to set the unsolicited flag, false to clear it
    constexpr void set_unsolicited(bool u) noexcept { command_type.set_flag(static_cast<uint16_t>(0x8000), u); }

    /// Get the command code (14-bit field, excluding U and CR bits)
    [[nodiscard]] constexpr auto command_code() const noexcept -> uint16_t { return command_type.get_bits<uint16_t>(0x3FFF); }

    /// Set the command code (14-bit field)
    /// @param cmd AEM command code
    constexpr void set_command_code(uint16_t cmd) noexcept { command_type.set_bits(0x3FFF, 0U, cmd); }

    /// Get the controller request (CR) bit from command_type field (bit 14)
    [[nodiscard]] constexpr auto is_controller_request() const noexcept -> bool
    {
        return command_type.has_flag(static_cast<uint16_t>(0x4000));
    }

    /// Set the controller request (CR) bit
    /// @param cr True to set the controller request flag, false to clear it
    constexpr void set_controller_request(bool cr) noexcept { command_type.set_flag(static_cast<uint16_t>(0x4000), cr); }

    // Message type helpers

    /// Check if this is a command message
    [[nodiscard]] constexpr auto is_command() const noexcept -> bool { return message_type() == AECP_MESSAGE_TYPE_AEM_COMMAND; }

    /// Check if this is a response message
    [[nodiscard]] constexpr auto is_response() const noexcept -> bool { return message_type() == AECP_MESSAGE_TYPE_AEM_RESPONSE; }

    // Initialization

    /// Initialize header fields for a command
    /// @param cmd AEM command code
    /// @param data_length Control data length for the message
    constexpr void init_command(uint16_t cmd, uint16_t data_length) noexcept
    {
        subtype = AvtpSubtype::aecp;
        set_message_type(AECP_MESSAGE_TYPE_AEM_COMMAND);
        set_status(AEM_STATUS_SUCCESS);
        set_control_data_length(data_length);
        set_unsolicited(false);
        set_controller_request(false);
        set_command_code(cmd);
    }

    /// Initialize header fields for a response
    /// @param cmd AEM command code
    /// @param stat AEM status code
    /// @param data_length Control data length for the message
    /// @param unsolicited True if this is an unsolicited response
    constexpr void init_response(uint16_t cmd, uint8_t stat, uint16_t data_length, bool unsolicited = false) noexcept
    {
        subtype = AvtpSubtype::aecp;
        set_message_type(AECP_MESSAGE_TYPE_AEM_RESPONSE);
        set_status(stat);
        set_control_data_length(data_length);
        set_unsolicited(unsolicited);
        set_controller_request(false);
        set_command_code(cmd);
    }

    auto operator<=>(AemDu const& rhs) const noexcept -> std::strong_ordering = default;

    // Validation

    /// Check if this is a valid AEM PDU
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        // Check subtype is AECP (0xFB)
        if (subtype.get() != AvtpSubtype::aecp) {
            return false;
        }
        // Check control data length is at least AEM_DATA_LENGTH (12 bytes)
        if (control_data_length() < AEM_DATA_LENGTH) {
            return false;
        }
        // Check control data length doesn't exceed maximum
        if (control_data_length() > AECP_MAX_CONTROL_DATA_LENGTH) {
            return false;
        }
        // Check message type is AEM command or response (0-1)
        uint8_t const mt = message_type();
        return mt == AECP_MESSAGE_TYPE_AEM_COMMAND || mt == AECP_MESSAGE_TYPE_AEM_RESPONSE;
    }
};

// Compile-time layout verification
static_assert(sizeof(AemDu) == 24, "AemDu must be exactly 24 bytes");
static_assert(alignof(AemDu) <= 4, "AemDu alignment must not exceed 4 bytes");
static_assert(offsetof(AemDu, subtype) == 0, "subtype must be at offset 0");
static_assert(offsetof(AemDu, sv_version_msgtype) == 1, "sv_version_msgtype must be at offset 1");
static_assert(offsetof(AemDu, status_cdl) == 2, "status_cdl must be at offset 2");
static_assert(offsetof(AemDu, target_entity_id) == 4, "target_entity_id must be at offset 4");
static_assert(offsetof(AemDu, controller_entity_id) == 12, "controller_entity_id must be at offset 12");
static_assert(offsetof(AemDu, sequence_id) == 20, "sequence_id must be at offset 20");
static_assert(offsetof(AemDu, command_type) == 22, "command_type must be at offset 22");

//
// AEM Command Tracker - Controller-side IN_PROGRESS handling
//

/// Tracks an outstanding AEM command and handles IN_PROGRESS responses
class AemCommandTracker
{
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    enum class Result : uint8_t
    {
        Pending,   ///< IN_PROGRESS received, deadline extended
        Complete,  ///< Final response received
        TimedOut,  ///< No response before deadline
    };

    /// Start tracking a new command
    void start(uint16_t sequence_id, uint16_t command_code, TimePoint now) noexcept
    {
        sequence_id_ = sequence_id;
        command_code_ = command_code;
        deadline_ = now + std::chrono::milliseconds(AEM_TIMEOUT_MS);
        active_ = true;
    }

    /// Process a received AEM response. Returns the tracking result.
    auto receive_response(uint8_t status, uint16_t sequence_id, TimePoint now) noexcept -> Result
    {
        if (!active_ || sequence_id != sequence_id_) {
            return Result::Complete;  // Not our command
        }

        if (status == AEM_STATUS_IN_PROGRESS) {
            deadline_ = now + std::chrono::milliseconds(AEM_TIMEOUT_MS);
            return Result::Pending;
        }

        active_ = false;
        return Result::Complete;
    }

    /// Check if the tracked command has timed out
    [[nodiscard]] auto check_timeout(TimePoint now) const noexcept -> bool { return active_ && now >= deadline_; }

    /// Cancel tracking
    void cancel() noexcept { active_ = false; }

    /// Whether a command is being tracked
    [[nodiscard]] auto is_active() const noexcept -> bool { return active_; }

    /// Get the tracked sequence ID
    [[nodiscard]] auto sequence_id() const noexcept -> uint16_t { return sequence_id_; }

    /// Get the tracked command code
    [[nodiscard]] auto command_code() const noexcept -> uint16_t { return command_code_; }

  private:
    uint16_t sequence_id_{0};
    uint16_t command_code_{0};
    TimePoint deadline_{};
    bool active_{false};
};

//
// AEM Pending Command - Entity-side IN_PROGRESS handling
//

/// Represents a deferred AEM command on the entity side
struct PendingAemCommand
{
    using TimePoint = std::chrono::steady_clock::time_point;

    AemDu header{};
    ieee::Eui64 controller_entity_id{};
    uint16_t sequence_id{0};
    uint16_t command_code{0};
    TimePoint next_in_progress{};
    bool active{false};

    /// Start tracking a deferred command
    void start(AemDu const& aem_header, TimePoint now) noexcept
    {
        header = aem_header;
        controller_entity_id = aem_header.controller_entity_id;
        sequence_id = aem_header.sequence_id.get();
        command_code = aem_header.command_code();
        next_in_progress = now + std::chrono::milliseconds(AEM_IN_PROGRESS_TIMEOUT_MS);
        active = true;
    }

    /// Check if it's time to send another IN_PROGRESS response
    [[nodiscard]] auto needs_in_progress(TimePoint now) const noexcept -> bool { return active && now >= next_in_progress; }

    /// Mark that an IN_PROGRESS response was sent
    void sent_in_progress(TimePoint now) noexcept
    {
        next_in_progress = now + std::chrono::milliseconds(AEM_IN_PROGRESS_TIMEOUT_MS);
    }

    /// Complete the pending command
    void complete() noexcept { active = false; }
};

}  // namespace statusbar::atdecc

// Serialization traits - AemDu is a packed wire format struct
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::AemDu> : std::true_type
{};

// Export using declarations for ADL to find the template functions
namespace statusbar::atdecc {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::atdecc
