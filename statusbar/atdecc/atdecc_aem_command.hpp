#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ATDECC Entity Model (AEM) Commands and Responses - IEEE 1722.1 Clause 7.4
/// Modernized C++23 implementation based on jdksatdecc-c
/// This module contains the command-specific payload structures that follow the AemDu header.

#include "statusbar/atdecc/atdecc_aecp.hpp"
#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
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

namespace statusbar::atdecc::aem {

using ieee::doublet_t;
using ieee::Eui64;
using ieee::octet_t;
using ieee::octlet_t;
using ieee::quadlet_t;
using statusbar::BufferError;
using statusbar::failure;
using statusbar::StatusValue;
using statusbar::success;
using tsn::ClockIdentity;

//
// AEM Acquire/Lock Flags - Clause 7.4.1 and 7.4.2
//
namespace aem_acquire_flags {
/// Persistent flag - entity should remember this acquire across reboot
constexpr uint32_t PERSISTENT = 0x00000001;
/// Release flag - release ownership
constexpr uint32_t RELEASE = 0x80000000;
}  // namespace aem_acquire_flags

namespace aem_lock_flags {
/// Unlock flag - release the lock
constexpr uint32_t UNLOCK = 0x00000001;
}  // namespace aem_lock_flags

//
// Common AEM Command Payload Base
// The AemDu header (24 bytes) precedes all these payloads.
// Offsets in these structures are relative to the end of the AemDu header (byte 24).
//
//
// ACQUIRE_ENTITY Command/Response - Clause 7.4.1
// Total PDU: AemDu header (24 bytes) + 16 bytes payload = 40 bytes
//
/// ACQUIRE_ENTITY command/response payload (after AemDu header)
struct AemAcquireEntityPayload
{
    static constexpr size_t LENGTH = 16;

    /// Bytes 0-3: Acquire flags
    quadlet_t flags{0};

    /// Bytes 4-11: Owner Entity ID (set by responder in response)
    Eui64 owner_entity_id{};

    /// Bytes 12-13: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 14-15: Descriptor index
    doublet_t descriptor_index{0};

    // Flag accessors
    [[nodiscard]] constexpr auto is_persistent() const noexcept -> bool { return flags.has_flag(aem_acquire_flags::PERSISTENT); }
    [[nodiscard]] constexpr auto is_release() const noexcept -> bool { return flags.has_flag(aem_acquire_flags::RELEASE); }

    constexpr void set_persistent(bool v) noexcept { flags.set_flag(aem_acquire_flags::PERSISTENT, v); }
    constexpr void set_release(bool v) noexcept { flags.set_flag(aem_acquire_flags::RELEASE, v); }

    auto operator<=>(AemAcquireEntityPayload const&) const noexcept = default;
};

static_assert(sizeof(AemAcquireEntityPayload) == 16, "AemAcquireEntityPayload must be 16 bytes");
static_assert(offsetof(AemAcquireEntityPayload, flags) == 0, "flags must be at offset 0");
static_assert(offsetof(AemAcquireEntityPayload, owner_entity_id) == 4, "owner_entity_id must be at offset 4");
static_assert(offsetof(AemAcquireEntityPayload, descriptor_type) == 12, "descriptor_type must be at offset 12");
static_assert(offsetof(AemAcquireEntityPayload, descriptor_index) == 14, "descriptor_index must be at offset 14");

//
// LOCK_ENTITY Command/Response - Clause 7.4.2
// Total PDU: AemDu header (24 bytes) + 16 bytes payload = 40 bytes
//
/// LOCK_ENTITY command/response payload (after AemDu header)
struct AemLockEntityPayload
{
    static constexpr size_t LENGTH = 16;

    /// Bytes 0-3: Lock flags
    quadlet_t flags{0};

    /// Bytes 4-11: Locked Entity ID (set by responder in response)
    Eui64 locked_entity_id{};

    /// Bytes 12-13: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 14-15: Descriptor index
    doublet_t descriptor_index{0};

    // Flag accessors
    [[nodiscard]] constexpr auto is_unlock() const noexcept -> bool { return flags.has_flag(aem_lock_flags::UNLOCK); }
    constexpr void set_unlock(bool v) noexcept { flags.set_flag(aem_lock_flags::UNLOCK, v); }

    auto operator<=>(AemLockEntityPayload const&) const noexcept = default;
};

static_assert(sizeof(AemLockEntityPayload) == 16, "AemLockEntityPayload must be 16 bytes");
static_assert(offsetof(AemLockEntityPayload, flags) == 0, "flags must be at offset 0");
static_assert(offsetof(AemLockEntityPayload, locked_entity_id) == 4, "locked_entity_id must be at offset 4");
static_assert(offsetof(AemLockEntityPayload, descriptor_type) == 12, "descriptor_type must be at offset 12");
static_assert(offsetof(AemLockEntityPayload, descriptor_index) == 14, "descriptor_index must be at offset 14");

//
// ENTITY_AVAILABLE Command/Response - Clause 7.4.3
// Total PDU: AemDu header (24 bytes) + 0 bytes payload = 24 bytes (header only)
//
// No payload - command and response are just the AemDu header

//
// CONTROLLER_AVAILABLE Command/Response - Clause 7.4.4
// Total PDU: AemDu header (24 bytes) + 0 bytes payload = 24 bytes (header only)
//
// No payload - command and response are just the AemDu header

//
// READ_DESCRIPTOR Command - Clause 7.4.5
// Total PDU: AemDu header (24 bytes) + 8 bytes payload = 32 bytes
//
/// READ_DESCRIPTOR command payload (after AemDu header)
struct AemReadDescriptorCommandPayload
{
    static constexpr size_t LENGTH = 8;

    /// Bytes 0-1: Configuration index
    doublet_t configuration_index{0};

    /// Bytes 2-3: Reserved
    doublet_t reserved{0};

    /// Bytes 4-5: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 6-7: Descriptor index
    doublet_t descriptor_index{0};

    auto operator<=>(AemReadDescriptorCommandPayload const&) const noexcept = default;
};

static_assert(sizeof(AemReadDescriptorCommandPayload) == 8, "AemReadDescriptorCommandPayload must be 8 bytes");

//
// READ_DESCRIPTOR Response - Clause 7.4.5
// Total PDU: AemDu header (24 bytes) + 4 bytes fixed + variable descriptor
//
/// READ_DESCRIPTOR response payload header (after AemDu header)
struct AemReadDescriptorResponsePayload
{
    static constexpr size_t LENGTH = 4;  // Fixed portion only

    /// Bytes 0-1: Configuration index
    doublet_t configuration_index{0};

    /// Bytes 2-3: Reserved
    doublet_t reserved{0};

    // Followed by variable-length descriptor data

    auto operator<=>(AemReadDescriptorResponsePayload const&) const noexcept = default;
};

static_assert(sizeof(AemReadDescriptorResponsePayload) == 4, "AemReadDescriptorResponsePayload must be 4 bytes");

//
// SET_CONFIGURATION / GET_CONFIGURATION - Clause 7.4.7 / 7.4.8
//
/// SET_CONFIGURATION command payload
struct AemSetConfigurationPayload
{
    static constexpr size_t LENGTH = 4;

    /// Bytes 0-1: Reserved
    doublet_t reserved{0};

    /// Bytes 2-3: Configuration index
    doublet_t configuration_index{0};

    auto operator<=>(AemSetConfigurationPayload const&) const noexcept = default;
};

static_assert(sizeof(AemSetConfigurationPayload) == 4, "AemSetConfigurationPayload must be 4 bytes");

/// GET_CONFIGURATION command has no payload (header only)
/// GET_CONFIGURATION response uses same payload as SET_CONFIGURATION

//
// SET_STREAM_FORMAT / GET_STREAM_FORMAT - Clause 7.4.9 / 7.4.10
//
/// Stream format command/response payload
struct AemStreamFormatPayload
{
    static constexpr size_t LENGTH = 12;

    /// Bytes 0-1: Descriptor type (STREAM_INPUT or STREAM_OUTPUT)
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-11: Stream format (64-bit)
    std::array<octet_t, 8> stream_format{};

    auto operator<=>(AemStreamFormatPayload const&) const noexcept = default;
};

static_assert(sizeof(AemStreamFormatPayload) == 12, "AemStreamFormatPayload must be 12 bytes");

/// GET_STREAM_FORMAT command payload (no stream_format field)
struct AemGetStreamFormatCommandPayload
{
    static constexpr size_t LENGTH = 4;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    auto operator<=>(AemGetStreamFormatCommandPayload const&) const noexcept = default;
};

static_assert(sizeof(AemGetStreamFormatCommandPayload) == 4, "AemGetStreamFormatCommandPayload must be 4 bytes");

//
// SET_STREAM_INFO / GET_STREAM_INFO - Clause 7.4.15 / 7.4.16
//
/// Stream info flags
namespace stream_info_flags {
constexpr uint32_t CLASS_B = 0x00000001;
constexpr uint32_t FAST_CONNECT = 0x00000002;
constexpr uint32_t SAVED_STATE = 0x00000004;
constexpr uint32_t STREAMING_WAIT = 0x00000008;
constexpr uint32_t SUPPORTS_ENCRYPTED = 0x00000010;
constexpr uint32_t ENCRYPTED_PDU = 0x00000020;
constexpr uint32_t TALKER_FAILED = 0x00000040;
constexpr uint32_t STREAM_VLAN_ID_VALID = 0x02000000;
constexpr uint32_t CONNECTED = 0x04000000;
constexpr uint32_t MSRP_FAILURE_VALID = 0x08000000;
constexpr uint32_t STREAM_DEST_MAC_VALID = 0x10000000;
constexpr uint32_t MSRP_ACC_LAT_VALID = 0x20000000;
constexpr uint32_t STREAM_ID_VALID = 0x40000000;
constexpr uint32_t STREAM_FORMAT_VALID = 0x80000000;
}  // namespace stream_info_flags

/// GET_STREAM_INFO command payload
struct AemGetStreamInfoCommandPayload
{
    static constexpr size_t LENGTH = 4;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    auto operator<=>(AemGetStreamInfoCommandPayload const&) const noexcept = default;
};

static_assert(sizeof(AemGetStreamInfoCommandPayload) == 4, "AemGetStreamInfoCommandPayload must be 4 bytes");

/// SET_STREAM_INFO / GET_STREAM_INFO response payload
struct AemStreamInfoPayload
{
    static constexpr size_t LENGTH = 48;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-7: Flags
    quadlet_t flags{0};

    /// Bytes 8-15: Stream format
    std::array<octet_t, 8> stream_format{};

    /// Bytes 16-23: Stream ID
    Eui64 stream_id{};

    /// Bytes 24-27: MSRP accumulated latency
    quadlet_t msrp_accumulated_latency{0};

    /// Bytes 28-33: Stream destination MAC
    Eui48 stream_dest_mac{};

    /// Bytes 34: MSRP failure code
    octet_t msrp_failure_code{0};

    /// Bytes 35: Reserved
    octet_t reserved1{0};

    /// Bytes 36-43: MSRP failure bridge ID
    Eui64 msrp_failure_bridge_id{};

    /// Bytes 44-45: Stream VLAN ID
    doublet_t stream_vlan_id{0};

    /// Bytes 46-47: Reserved
    doublet_t reserved2{0};

    // Flag accessors
    [[nodiscard]] constexpr auto is_connected() const noexcept -> bool { return flags.has_flag(stream_info_flags::CONNECTED); }
    [[nodiscard]] constexpr auto is_class_b() const noexcept -> bool { return flags.has_flag(stream_info_flags::CLASS_B); }
    [[nodiscard]] constexpr auto is_stream_id_valid() const noexcept -> bool
    {
        return flags.has_flag(stream_info_flags::STREAM_ID_VALID);
    }
    [[nodiscard]] constexpr auto is_stream_format_valid() const noexcept -> bool
    {
        return flags.has_flag(stream_info_flags::STREAM_FORMAT_VALID);
    }

    auto operator<=>(AemStreamInfoPayload const&) const noexcept = default;
};

static_assert(sizeof(AemStreamInfoPayload) == 48, "AemStreamInfoPayload must be 48 bytes");

//
// SET_NAME / GET_NAME - Clause 7.4.17 / 7.4.18
//
/// SET_NAME / GET_NAME command payload
struct AemNameCommandPayload
{
    static constexpr size_t LENGTH = 8;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-5: Name index
    doublet_t name_index{0};

    /// Bytes 6-7: Configuration index
    doublet_t configuration_index{0};

    auto operator<=>(AemNameCommandPayload const&) const noexcept = default;
};

static_assert(sizeof(AemNameCommandPayload) == 8, "AemNameCommandPayload must be 8 bytes");

/// SET_NAME / GET_NAME response payload
struct AemNamePayload
{
    static constexpr size_t LENGTH = 72;  // 8 + 64 name

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-5: Name index
    doublet_t name_index{0};

    /// Bytes 6-7: Configuration index
    doublet_t configuration_index{0};

    /// Bytes 8-71: Name (64 bytes, UTF-8, null-padded)
    std::array<char, 64> name{};

    auto operator<=>(AemNamePayload const&) const noexcept = default;
};

static_assert(sizeof(AemNamePayload) == 72, "AemNamePayload must be 72 bytes");

//
// SET_SAMPLING_RATE / GET_SAMPLING_RATE - Clause 7.4.21 / 7.4.22
//
/// SET_SAMPLING_RATE command/response payload
struct AemSamplingRatePayload
{
    static constexpr size_t LENGTH = 8;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-7: Sampling rate
    quadlet_t sampling_rate{0};

    auto operator<=>(AemSamplingRatePayload const&) const noexcept = default;
};

static_assert(sizeof(AemSamplingRatePayload) == 8, "AemSamplingRatePayload must be 8 bytes");

/// GET_SAMPLING_RATE command payload (no sampling_rate field)
struct AemGetSamplingRateCommandPayload
{
    static constexpr size_t LENGTH = 4;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    auto operator<=>(AemGetSamplingRateCommandPayload const&) const noexcept = default;
};

static_assert(sizeof(AemGetSamplingRateCommandPayload) == 4, "AemGetSamplingRateCommandPayload must be 4 bytes");

//
// SET_CLOCK_SOURCE / GET_CLOCK_SOURCE - Clause 7.4.23 / 7.4.24
//
/// SET_CLOCK_SOURCE command/response payload
struct AemClockSourcePayload
{
    static constexpr size_t LENGTH = 8;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-5: Clock source index
    doublet_t clock_source_index{0};

    /// Bytes 6-7: Reserved
    doublet_t reserved{0};

    auto operator<=>(AemClockSourcePayload const&) const noexcept = default;
};

static_assert(sizeof(AemClockSourcePayload) == 8, "AemClockSourcePayload must be 8 bytes");

/// GET_CLOCK_SOURCE command payload
struct AemGetClockSourceCommandPayload
{
    static constexpr size_t LENGTH = 4;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    auto operator<=>(AemGetClockSourceCommandPayload const&) const noexcept = default;
};

static_assert(sizeof(AemGetClockSourceCommandPayload) == 4, "AemGetClockSourceCommandPayload must be 4 bytes");

//
// START_STREAMING / STOP_STREAMING - Clause 7.4.35 / 7.4.36
//
/// START_STREAMING / STOP_STREAMING command/response payload
struct AemStreamingPayload
{
    static constexpr size_t LENGTH = 4;

    /// Bytes 0-1: Descriptor type (STREAM_INPUT or STREAM_OUTPUT)
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    auto operator<=>(AemStreamingPayload const&) const noexcept = default;
};

static_assert(sizeof(AemStreamingPayload) == 4, "AemStreamingPayload must be 4 bytes");

//
// REGISTER_UNSOLICITED_NOTIFICATION / DEREGISTER_UNSOLICITED_NOTIFICATION
// Clause 7.4.37 / 7.4.38
//
// No payload - command and response are just the AemDu header

//
// IDENTIFY_NOTIFICATION - Clause 7.4.39
//
/// IDENTIFY_NOTIFICATION command/response payload
struct AemIdentifyPayload
{
    static constexpr size_t LENGTH = 4;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    auto operator<=>(AemIdentifyPayload const&) const noexcept = default;
};

static_assert(sizeof(AemIdentifyPayload) == 4, "AemIdentifyPayload must be 4 bytes");

//
// GET_AVB_INFO - Clause 7.4.40
//
/// GET_AVB_INFO command payload
struct AemGetAvbInfoCommandPayload
{
    static constexpr size_t LENGTH = 4;

    /// Bytes 0-1: Descriptor type (should be AVB_INTERFACE)
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    auto operator<=>(AemGetAvbInfoCommandPayload const&) const noexcept = default;
};

static_assert(sizeof(AemGetAvbInfoCommandPayload) == 4, "AemGetAvbInfoCommandPayload must be 4 bytes");

/// AVB info flags - Clause 7.4.40
namespace avb_info_flags {
constexpr uint8_t AS_CAPABLE = 0x01;
constexpr uint8_t GPTP_ENABLED = 0x02;
constexpr uint8_t SRP_ENABLED = 0x04;
constexpr uint8_t AVTP_DOWN = 0x08;
constexpr uint8_t AVTP_DOWN_VALID = 0x10;
}  // namespace avb_info_flags

/// GET_AVB_INFO response payload (fixed portion, 20 bytes)
struct AemAvbInfoPayload
{
    static constexpr size_t LENGTH = 20;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-11: gPTP grandmaster ID
    ClockIdentity gptp_grandmaster_id{};

    /// Bytes 12-15: Propagation delay
    quadlet_t propagation_delay{0};

    /// Bytes 16: gPTP domain number
    octet_t gptp_domain_number{0};

    /// Bytes 17: Flags
    octet_t flags{0};

    /// Bytes 18-19: MSRP mappings count
    doublet_t msrp_mappings_count{0};

    // Followed by variable MSRP mappings (4 bytes each) starting at offset 20

    // Flag accessors
    [[nodiscard]] constexpr auto is_as_capable() const noexcept -> bool { return flags.has_flag(avb_info_flags::AS_CAPABLE); }
    [[nodiscard]] constexpr auto is_gptp_enabled() const noexcept -> bool { return flags.has_flag(avb_info_flags::GPTP_ENABLED); }
    [[nodiscard]] constexpr auto is_srp_enabled() const noexcept -> bool { return flags.has_flag(avb_info_flags::SRP_ENABLED); }
    [[nodiscard]] constexpr auto is_avtp_down() const noexcept -> bool { return flags.has_flag(avb_info_flags::AVTP_DOWN); }
    [[nodiscard]] constexpr auto is_avtp_down_valid() const noexcept -> bool
    {
        return flags.has_flag(avb_info_flags::AVTP_DOWN_VALID);
    }

    auto operator<=>(AemAvbInfoPayload const&) const noexcept = default;
};

static_assert(sizeof(AemAvbInfoPayload) == 20, "AemAvbInfoPayload must be 20 bytes");

//
// GET_COUNTERS - Clause 7.4.42
//
/// GET_COUNTERS command payload
struct AemGetCountersCommandPayload
{
    static constexpr size_t LENGTH = 4;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    auto operator<=>(AemGetCountersCommandPayload const&) const noexcept = default;
};

static_assert(sizeof(AemGetCountersCommandPayload) == 4, "AemGetCountersCommandPayload must be 4 bytes");

/// GET_COUNTERS response payload
struct AemCountersPayload
{
    static constexpr size_t LENGTH = 136;  // 2 + 2 + 4 + 32*4 (matches sizeof; see static_assert below)

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-7: Counters valid bitmap
    quadlet_t counters_valid{0};

    /// Bytes 8-135: 32 counter values (32 * 4 = 128 bytes)
    std::array<quadlet_t, 32> counters{};

    auto operator<=>(AemCountersPayload const&) const noexcept = default;
};

static_assert(sizeof(AemCountersPayload) == 136, "AemCountersPayload must be 136 bytes");

//
// PTP_PORT counters_valid bits - Clause 7.4.42.2.6 (Cor1)
//
namespace ptp_port_counters {
constexpr uint32_t RX_SYNC = 0x00000001;
constexpr uint32_t RX_ONE_STEP_SYNC = 0x00000002;
constexpr uint32_t RX_FOLLOWUP = 0x00000004;
constexpr uint32_t RX_PDELAY_REQUEST = 0x00000008;
constexpr uint32_t RX_PDELAY_RESPONSE = 0x00000010;
constexpr uint32_t RX_PDELAY_RESPONSE_FOLLOWUP = 0x00000020;
constexpr uint32_t RX_ANNOUNCE = 0x00000040;
constexpr uint32_t RX_SIGNAL = 0x00000080;
constexpr uint32_t RX_PACKET_DISCARD = 0x00000100;
constexpr uint32_t RX_DELAY_REQUEST = 0x00000200;
constexpr uint32_t RX_DELAY_RESPONSE = 0x00000400;
constexpr uint32_t SYNC_RECEIPT_TIMEOUT = 0x00000800;
constexpr uint32_t ANNOUNCE_RECEIPT_TIMEOUT = 0x00001000;
constexpr uint32_t PDELAY_ALLOWED_EXCEEDED = 0x00002000;
constexpr uint32_t TX_SYNC = 0x00004000;
constexpr uint32_t TX_ONE_STEP_SYNC = 0x00008000;
constexpr uint32_t TX_FOLLOWUP = 0x00010000;
constexpr uint32_t TX_PDELAY_REQUEST = 0x00020000;
constexpr uint32_t TX_PDELAY_RESPONSE = 0x00040000;
constexpr uint32_t TX_PDELAY_RESPONSE_FOLLOWUP = 0x00080000;
constexpr uint32_t TX_ANNOUNCE = 0x00100000;
constexpr uint32_t TX_SIGNAL = 0x00200000;
constexpr uint32_t TX_DELAY_REQUEST = 0x00400000;
constexpr uint32_t TX_DELAY_RESPONSE = 0x00800000;
constexpr uint32_t ENTITY_SPECIFIC_8 = 0x01000000;
constexpr uint32_t ENTITY_SPECIFIC_7 = 0x02000000;
constexpr uint32_t ENTITY_SPECIFIC_6 = 0x04000000;
constexpr uint32_t ENTITY_SPECIFIC_5 = 0x08000000;
constexpr uint32_t ENTITY_SPECIFIC_4 = 0x10000000;
constexpr uint32_t ENTITY_SPECIFIC_3 = 0x20000000;
constexpr uint32_t ENTITY_SPECIFIC_2 = 0x40000000;
constexpr uint32_t ENTITY_SPECIFIC_1 = 0x80000000;
}  // namespace ptp_port_counters

//
// REBOOT - Clause 7.4.43
//
/// REBOOT command/response payload
struct AemRebootPayload
{
    static constexpr size_t LENGTH = 4;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    auto operator<=>(AemRebootPayload const&) const noexcept = default;
};

static_assert(sizeof(AemRebootPayload) == 4, "AemRebootPayload must be 4 bytes");

//
// SET_CONTROL / GET_CONTROL - Clause 7.4.25 / 7.4.26
//
/// SET_CONTROL / GET_CONTROL command header (values follow)
struct AemControlPayloadHeader
{
    static constexpr size_t LENGTH = 4;

    /// Bytes 0-1: Descriptor type (should be CONTROL)
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    // Followed by variable-length control values

    auto operator<=>(AemControlPayloadHeader const&) const noexcept = default;
};

static_assert(sizeof(AemControlPayloadHeader) == 4, "AemControlPayloadHeader must be 4 bytes");

//
// SET_SIGNAL_SELECTOR / GET_SIGNAL_SELECTOR - Clause 7.4.29 / 7.4.30
//
/// Signal selector command/response payload
struct AemSignalSelectorPayload
{
    static constexpr size_t LENGTH = 12;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-9: The {signal_type, signal_index, signal_output} source triple
    SignalSource source{};

    /// Bytes 10-11: Reserved
    doublet_t reserved{0};

    auto operator<=>(AemSignalSelectorPayload const&) const noexcept = default;
};

static_assert(sizeof(AemSignalSelectorPayload) == 12, "AemSignalSelectorPayload must be 12 bytes");

/// GET_SIGNAL_SELECTOR command payload
struct AemGetSignalSelectorCommandPayload
{
    static constexpr size_t LENGTH = 4;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    auto operator<=>(AemGetSignalSelectorCommandPayload const&) const noexcept = default;
};

static_assert(sizeof(AemGetSignalSelectorCommandPayload) == 4, "AemGetSignalSelectorCommandPayload must be 4 bytes");

//
// SET_MATRIX / GET_MATRIX - Clause 7.4.33 / 7.4.34
//
/// Fixed header of the SET_MATRIX / GET_MATRIX command and response
/// payloads (Figure 7-56 / 7-57). Followed on the wire by the region's
/// matrix point values (SET command, SET/GET responses; each value is
/// one element of the matrix's control_value_type).
struct AemMatrixPayloadHeader
{
    static constexpr size_t LENGTH = 16;

    /// rep_direction_value_count bit layout: rep (bit 15, reserved/zero in
    /// GET), direction (bits 14-13, Table 7-146), value_count (bits 12-0).
    static constexpr uint16_t REP_FLAG = 0x8000;
    static constexpr uint16_t DIRECTION_SHIFT = 13;
    static constexpr uint16_t DIRECTION_MASK = 0x3;
    static constexpr uint16_t VALUE_COUNT_MASK = 0x1FFF;

    static constexpr uint16_t DIRECTION_HORIZONTAL = 0;
    static constexpr uint16_t DIRECTION_VERTICAL = 1;

    /// Bytes 0-1: Descriptor type (MATRIX)
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-5: Starting column of the subregion
    doublet_t matrix_column{0};

    /// Bytes 6-7: Starting row of the subregion
    doublet_t matrix_row{0};

    /// Bytes 8-9: Column count of the subregion
    doublet_t region_width{0};

    /// Bytes 10-11: Row count of the subregion
    doublet_t region_height{0};

    /// Bytes 12-13: rep | direction | value_count (see bit layout above)
    doublet_t rep_direction_value_count{0};

    /// Bytes 14-15: Items in the subregion to skip (in `direction` order)
    /// before applying/reading values
    doublet_t item_offset{0};

    [[nodiscard]] auto rep() const noexcept -> bool { return (rep_direction_value_count.get() & REP_FLAG) != 0; }
    [[nodiscard]] auto direction() const noexcept -> uint16_t
    {
        return static_cast<uint16_t>((rep_direction_value_count.get() >> DIRECTION_SHIFT) & DIRECTION_MASK);
    }
    [[nodiscard]] auto value_count() const noexcept -> uint16_t
    {
        return static_cast<uint16_t>(rep_direction_value_count.get() & VALUE_COUNT_MASK);
    }
    void set_rep_direction_value_count(bool const rep_flag, uint16_t const direction, uint16_t const value_count) noexcept
    {
        rep_direction_value_count = static_cast<uint16_t>(
            (rep_flag ? REP_FLAG : 0) | ((direction & DIRECTION_MASK) << DIRECTION_SHIFT) | (value_count & VALUE_COUNT_MASK));
    }

    auto operator<=>(AemMatrixPayloadHeader const&) const noexcept = default;
};

static_assert(sizeof(AemMatrixPayloadHeader) == 16, "AemMatrixPayloadHeader must be 16 bytes");

//
// SET_MIXER / GET_MIXER - Clause 7.4.35 / 7.4.36
//
/// Fixed header of the SET_MIXER / GET_MIXER command and response
/// payloads (Figure 7-58 / 7-59). Followed on the wire by the mixer's
/// single control value (one element of the mixer's control_value_type)
/// in the SET command and in both responses; the GET command is the
/// header alone.
struct AemMixerPayloadHeader
{
    static constexpr size_t LENGTH = 4;

    /// Bytes 0-1: Descriptor type (MIXER)
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    // Followed by the mixer value bytes

    auto operator<=>(AemMixerPayloadHeader const&) const noexcept = default;
};

static_assert(sizeof(AemMixerPayloadHeader) == 4, "AemMixerPayloadHeader must be 4 bytes");

//
// GET_AUDIO_MAP / ADD_AUDIO_MAPPINGS / REMOVE_AUDIO_MAPPINGS
// Clause 7.4.44 / 7.4.45 / 7.4.46
//
/// Audio mapping entry
struct AemAudioMapping
{
    static constexpr size_t LENGTH = 8;

    /// Bytes 0-1: Stream index
    doublet_t stream_index{0};

    /// Bytes 2-3: Stream channel
    doublet_t stream_channel{0};

    /// Bytes 4-5: Cluster offset
    doublet_t cluster_offset{0};

    /// Bytes 6-7: Cluster channel
    doublet_t cluster_channel{0};

    auto operator<=>(AemAudioMapping const&) const noexcept = default;
};

static_assert(sizeof(AemAudioMapping) == 8, "AemAudioMapping must be 8 bytes");

/// GET_AUDIO_MAP command payload
struct AemGetAudioMapCommandPayload
{
    static constexpr size_t LENGTH = 8;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-5: Map index
    doublet_t map_index{0};

    /// Bytes 6-7: Reserved
    doublet_t reserved{0};

    auto operator<=>(AemGetAudioMapCommandPayload const&) const noexcept = default;
};

static_assert(sizeof(AemGetAudioMapCommandPayload) == 8, "AemGetAudioMapCommandPayload must be 8 bytes");

/// GET_AUDIO_MAP response header
struct AemAudioMapResponseHeader
{
    static constexpr size_t LENGTH = 12;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-5: Map index
    doublet_t map_index{0};

    /// Bytes 6-7: Number of mappings
    doublet_t number_of_mappings{0};

    /// Bytes 8-9: Number of maps
    doublet_t number_of_maps{0};

    /// Bytes 10-11: Reserved
    doublet_t reserved{0};

    // Followed by array of AemAudioMapping

    auto operator<=>(AemAudioMapResponseHeader const&) const noexcept = default;
};

static_assert(sizeof(AemAudioMapResponseHeader) == 12, "AemAudioMapResponseHeader must be 12 bytes");

/// ADD_AUDIO_MAPPINGS / REMOVE_AUDIO_MAPPINGS command header
struct AemAudioMappingsCommandHeader
{
    static constexpr size_t LENGTH = 8;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-5: Number of mappings
    doublet_t number_of_mappings{0};

    /// Bytes 6-7: Reserved
    doublet_t reserved{0};

    // Followed by array of AemAudioMapping

    auto operator<=>(AemAudioMappingsCommandHeader const&) const noexcept = default;
};

static_assert(sizeof(AemAudioMappingsCommandHeader) == 8, "AemAudioMappingsCommandHeader must be 8 bytes");

//
// START_OPERATION - Clause 7.4.47
//
/// Memory Object operation types for START_OPERATION command - Clause 7
namespace operation_type {
constexpr uint16_t STORE = 0x0000;             ///< Validate and store to persistent storage
constexpr uint16_t STORE_AND_REBOOT = 0x0001;  ///< Store and reboot the device
constexpr uint16_t READ = 0x0002;              ///< Read from persistent storage
constexpr uint16_t ERASE = 0x0003;             ///< Erase from persistent storage
constexpr uint16_t UPLOAD = 0x0004;            ///< Upload new contents, ready for STORE
constexpr uint16_t VENDOR_SPECIFIC_START = 0x8000;

/// Get human-readable name for an operation type
[[nodiscard]] inline auto name(uint16_t op) noexcept -> std::string_view
{
    switch (op) {
        case STORE:
            return "STORE";
        case STORE_AND_REBOOT:
            return "STORE_AND_REBOOT";
        case READ:
            return "READ";
        case ERASE:
            return "ERASE";
        case UPLOAD:
            return "UPLOAD";
        default:
            return op >= VENDOR_SPECIFIC_START ? "VENDOR_SPECIFIC" : "UNKNOWN";
    }
}
}  // namespace operation_type

/// START_OPERATION command payload
struct AemStartOperationCommandPayload
{
    static constexpr size_t LENGTH = 8;

    /// Bytes 0-1: Descriptor type (typically MEMORY_OBJECT)
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-5: Operation ID (assigned by controller, echoed in response/status)
    doublet_t operation_id{0};

    /// Bytes 6-7: Operation type
    doublet_t operation_type{0};

    // Followed by variable operation-specific data

    auto operator<=>(AemStartOperationCommandPayload const&) const noexcept = default;
};

static_assert(sizeof(AemStartOperationCommandPayload) == 8, "AemStartOperationCommandPayload must be 8 bytes");

/// START_OPERATION response payload
struct AemStartOperationResponsePayload
{
    static constexpr size_t LENGTH = 8;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-5: Operation ID (echoed from command)
    doublet_t operation_id{0};

    /// Bytes 6-7: Operation type
    doublet_t operation_type{0};

    auto operator<=>(AemStartOperationResponsePayload const&) const noexcept = default;
};

static_assert(sizeof(AemStartOperationResponsePayload) == 8, "AemStartOperationResponsePayload must be 8 bytes");

//
// ABORT_OPERATION - Clause 7.4.48
//
/// ABORT_OPERATION command/response payload
struct AemAbortOperationPayload
{
    static constexpr size_t LENGTH = 8;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-5: Operation ID to abort
    doublet_t operation_id{0};

    /// Bytes 6-7: Reserved
    doublet_t reserved{0};

    auto operator<=>(AemAbortOperationPayload const&) const noexcept = default;
};

static_assert(sizeof(AemAbortOperationPayload) == 8, "AemAbortOperationPayload must be 8 bytes");

//
// OPERATION_STATUS - Clause 7.4.49
// Unsolicited response only - no command payload
//
/// OPERATION_STATUS unsolicited response payload
struct AemOperationStatusPayload
{
    static constexpr size_t LENGTH = 8;

    /// Bytes 0-1: Descriptor type
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-5: Operation ID
    doublet_t operation_id{0};

    /// Bytes 6-7: Percent complete (0-10000 = 0.00% - 100.00%)
    doublet_t percent_complete{0};

    auto operator<=>(AemOperationStatusPayload const&) const noexcept = default;
};

static_assert(sizeof(AemOperationStatusPayload) == 8, "AemOperationStatusPayload must be 8 bytes");

//
// SET_MAX_TRANSIT_TIME / GET_MAX_TRANSIT_TIME - IEEE 1722.1-2021 Clause 7.4.76 / 7.4.77
//
/// SET_MAX_TRANSIT_TIME command/response payload
struct AemSetMaxTransitTimePayload
{
    static constexpr size_t LENGTH = 12;

    /// Bytes 0-1: Descriptor type (STREAM_INPUT or STREAM_OUTPUT)
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    /// Bytes 4-11: Max transit time in nanoseconds
    octlet_t max_transit_time{0};

    auto operator<=>(AemSetMaxTransitTimePayload const&) const noexcept = default;
};

static_assert(sizeof(AemSetMaxTransitTimePayload) == 12, "AemSetMaxTransitTimePayload must be 12 bytes");
static_assert(offsetof(AemSetMaxTransitTimePayload, descriptor_type) == 0, "descriptor_type must be at offset 0");
static_assert(offsetof(AemSetMaxTransitTimePayload, descriptor_index) == 2, "descriptor_index must be at offset 2");
static_assert(offsetof(AemSetMaxTransitTimePayload, max_transit_time) == 4, "max_transit_time must be at offset 4");

/// GET_MAX_TRANSIT_TIME command payload (no max_transit_time field)
struct AemGetMaxTransitTimeCommandPayload
{
    static constexpr size_t LENGTH = 4;

    /// Bytes 0-1: Descriptor type (STREAM_INPUT or STREAM_OUTPUT)
    doublet_t descriptor_type{0};

    /// Bytes 2-3: Descriptor index
    doublet_t descriptor_index{0};

    auto operator<=>(AemGetMaxTransitTimeCommandPayload const&) const noexcept = default;
};

static_assert(sizeof(AemGetMaxTransitTimeCommandPayload) == 4, "AemGetMaxTransitTimeCommandPayload must be 4 bytes");
static_assert(offsetof(AemGetMaxTransitTimeCommandPayload, descriptor_type) == 0, "descriptor_type must be at offset 0");
static_assert(offsetof(AemGetMaxTransitTimeCommandPayload, descriptor_index) == 2, "descriptor_index must be at offset 2");

/// GET_MAX_TRANSIT_TIME response payload (same as SET_MAX_TRANSIT_TIME)
using AemGetMaxTransitTimeResponsePayload = AemSetMaxTransitTimePayload;

//
// SET_PTP_PORT_INFO / GET_PTP_PORT_INFO Command - Clause 7.4.94 / 7.4.95
//
/// SET_PTP_PORT_INFO command/response and GET_PTP_PORT_INFO command payload
struct AemPtpPortInfoCommandPayload
{
    static constexpr size_t LENGTH = 36;

    doublet_t descriptor_type{0};               // 0
    doublet_t descriptor_index{0};              // 2
    doublet_t reserved1{0};                     // 4
    doublet_t flags{0};                         // 6
    octet_t delay_mechanism{0};                 // 8
    octet_t announce_receipt_timeout{0};        // 9
    octet_t sync_receipt_timeout{0};            // 10
    octet_t port_flags{0};                      // 11: cc|ccnr|asc|imd|ioto|icmd|icnr|pe (packed bits)
    octlet_t mean_link_delay_threshold{0};      // 12
    octlet_t delay_asymmetry{0};                // 20
    doublet_t allowed_lost_responses{0};        // 28
    doublet_t allowed_faults{0};                // 30
    doublet_t gptp_capable_receipt_timeout{0};  // 32
    octet_t port_state{0};                      // 34
    octet_t reserved2{0};                       // 35

    auto operator<=>(AemPtpPortInfoCommandPayload const&) const noexcept = default;
};

static_assert(sizeof(AemPtpPortInfoCommandPayload) == 36);
static_assert(offsetof(AemPtpPortInfoCommandPayload, flags) == 6);
static_assert(offsetof(AemPtpPortInfoCommandPayload, mean_link_delay_threshold) == 12);
static_assert(offsetof(AemPtpPortInfoCommandPayload, delay_asymmetry) == 20);
static_assert(offsetof(AemPtpPortInfoCommandPayload, port_state) == 34);

/// GET_PTP_PORT_INFO response payload (Cor1 updated with mean_link_delay, neighbor_rate_ratio, version)
struct AemGetPtpPortInfoResponsePayload
{
    static constexpr size_t LENGTH = 76;
    static constexpr size_t MINIMUM_LENGTH = 36;  // pre-Cor1 compat

    doublet_t descriptor_type{0};               // 0
    doublet_t descriptor_index{0};              // 2
    doublet_t reserved1{0};                     // 4
    doublet_t flags{0};                         // 6
    octet_t delay_mechanism{0};                 // 8
    octet_t announce_receipt_timeout{0};        // 9
    octet_t sync_receipt_timeout{0};            // 10
    octet_t port_flags{0};                      // 11: cc|ccnr|asc|imd|ioto|icmd|icnr|pe
    octlet_t mean_link_delay_threshold{0};      // 12
    octlet_t delay_asymmetry{0};                // 20
    doublet_t allowed_lost_responses{0};        // 28
    doublet_t allowed_faults{0};                // 30
    doublet_t gptp_capable_receipt_timeout{0};  // 32
    octet_t port_state{0};                      // 34
    octet_t reserved2{0};                       // 35
    quadlet_t reserved3{0};                     // 36
    octlet_t mean_link_delay{0};                // 40  - Cor1
    octlet_t neighbor_rate_ratio{0};            // 48  - Cor1
    octet_t major_version{0};                   // 56  - Cor1
    octet_t minor_version{0};                   // 57  - Cor1
    octet_t ext_port_flags{0};                  // 58  - Cor1: rsvd|osto|osr|ost|coto|sl
    octet_t reserved4{0};                       // 59
    octlet_t reserved5{0};                      // 60
    octlet_t reserved6{0};                      // 68

    auto operator<=>(AemGetPtpPortInfoResponsePayload const&) const noexcept = default;
};

static_assert(sizeof(AemGetPtpPortInfoResponsePayload) == 76);
static_assert(offsetof(AemGetPtpPortInfoResponsePayload, mean_link_delay) == 40);
static_assert(offsetof(AemGetPtpPortInfoResponsePayload, neighbor_rate_ratio) == 48);
static_assert(offsetof(AemGetPtpPortInfoResponsePayload, major_version) == 56);
static_assert(offsetof(AemGetPtpPortInfoResponsePayload, minor_version) == 57);
static_assert(offsetof(AemGetPtpPortInfoResponsePayload, ext_port_flags) == 58);

//
// SET_PTP_PORT_INFO flags - Clause 7.4.94
//
namespace set_ptp_port_info_flags {
constexpr uint16_t SET_ENABLE = 0x0001;
constexpr uint16_t SET_LINK_DELAY_THRESHOLD = 0x0002;
constexpr uint16_t SET_DELAY_MECHANISM = 0x0004;
constexpr uint16_t SET_DELAY_ASYMMETRY = 0x0008;
constexpr uint16_t SET_ANNOUNCE_TIMEOUTS = 0x0010;
constexpr uint16_t SET_SYNC_TIMEOUTS = 0x0020;
constexpr uint16_t SET_GPTP_CAPABLE_TIMEOUTS = 0x0040;
constexpr uint16_t SET_PDELAY_TIMEOUTS = 0x0080;
constexpr uint16_t SET_IOTO = 0x0100;
constexpr uint16_t SET_ICMD = 0x0200;
constexpr uint16_t SET_ICNR = 0x0400;
constexpr uint16_t SET_FAULTS = 0x0800;
}  // namespace set_ptp_port_info_flags

//
// GET_PTP_PORT_INFO flags - Clause 7.4.95
//
namespace get_ptp_port_info_flags {
constexpr uint16_t DELAY_MECHANISM = 0x0001;
constexpr uint16_t DELAY_ASYMMETRY = 0x0002;
constexpr uint16_t GPTP_CAPABLE_TIMEOUTS = 0x0004;
constexpr uint16_t PDELAY_TIMEOUTS = 0x0008;
constexpr uint16_t IOTO = 0x0010;
constexpr uint16_t FAULTS = 0x0020;
constexpr uint16_t OSTO = 0x0040;
constexpr uint16_t OSR = 0x0080;
constexpr uint16_t OST = 0x0100;
constexpr uint16_t COTO = 0x0200;
constexpr uint16_t SL = 0x0400;
}  // namespace get_ptp_port_info_flags

//
// GET_PTP_INSTANCE_PERF_MON_RECORD flags - Clause 7.4.88 (Cor1)
//
namespace ptp_perf_mon_flags {
constexpr uint16_t MEASUREMENT_VALID = 0x0001;
constexpr uint16_t PERIOD_COMPLETE = 0x0002;
constexpr uint16_t MASTER_SLAVE_DELAY_VALID = 0x0004;
constexpr uint16_t SLAVE_MASTER_DELAY_VALID = 0x0008;
constexpr uint16_t MEAN_PATH_DELAY_VALID = 0x0010;
constexpr uint16_t OFFSET_FROM_MASTER_VALID = 0x0020;
}  // namespace ptp_perf_mon_flags

}  // namespace statusbar::atdecc::aem

//
// Serialization traits - All AEM payload structs are packed wire format
//
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemAcquireEntityPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemLockEntityPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemReadDescriptorCommandPayload>
    : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemReadDescriptorResponsePayload>
    : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemSetConfigurationPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemStreamFormatPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemGetStreamFormatCommandPayload>
    : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemGetStreamInfoCommandPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemStreamInfoPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemNameCommandPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemNamePayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemSamplingRatePayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemGetSamplingRateCommandPayload>
    : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemClockSourcePayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemGetClockSourceCommandPayload>
    : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemStreamingPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemIdentifyPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemGetAvbInfoCommandPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemAvbInfoPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemGetCountersCommandPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemCountersPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemRebootPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemControlPayloadHeader> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemSignalSelectorPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemGetSignalSelectorCommandPayload>
    : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemAudioMapping> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemGetAudioMapCommandPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemAudioMapResponseHeader> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemAudioMappingsCommandHeader> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemStartOperationCommandPayload>
    : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemStartOperationResponsePayload>
    : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemAbortOperationPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemOperationStatusPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemSetMaxTransitTimePayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemGetMaxTransitTimeCommandPayload>
    : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemMatrixPayloadHeader> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemPtpPortInfoCommandPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AemGetPtpPortInfoResponsePayload>
    : std::true_type
{};
