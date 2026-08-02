#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/srp/srp_mrp.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn_stream_id.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>
#include <type_traits>

namespace statusbar::srp::msrp {

using ieee::doublet_t;
using ieee::Eui48;
using ieee::Eui64;
using ieee::octet_t;
using ieee::quadlet_t;
using srp::mrp::AttributeEvent;
using tsn::StreamId;

//
// MSRP Constants - IEEE 802.1Q-2014 Clause 35
//
/// MSRP Protocol Version
/// IEEE 802.1Q-2014 Clause 35.2.2.2
constexpr uint8_t PROTOCOL_VERSION = 0;

/// MSRP EtherType
/// IEEE 802.1Q-2014 Clause 10.5, Table 10-2
constexpr uint16_t ETHERTYPE = 0x22EA;

/// MSRP Application Address (Nearest Customer Bridge)
/// IEEE 802.1Q-2014 Clause 35.2.2.1, Table 8-1
constexpr uint64_t APPLICATION_ADDRESS = 0x0180C200000E;

//
// MSRP Attribute Types - IEEE 802.1Q-2014 Clause 35.2.2.4, Table 35-1
//
enum class AttributeType : uint8_t
{
    TalkerAdvertise = 1,
    TalkerFailed = 2,
    Listener = 3,
    Domain = 4,
};

/// Get the name of an AttributeType for debugging/logging
[[nodiscard]] auto attribute_type_name(AttributeType type) noexcept -> std::string_view;

//
// MSRP Attribute Lengths - IEEE 802.1Q-2014 Clause 35.2.2.5, Table 35-2
//
enum class AttributeLength : uint8_t
{
    TalkerAdvertise = 25,
    TalkerFailed = 34,
    Listener = 8,
    Domain = 4,
};

//
// MSRP SR Class - IEEE 802.1Q-2014 Clause 35.2.2.9.2
//
/// SR Class identifiers
constexpr uint8_t SR_CLASS_A = 6;
constexpr uint8_t SR_CLASS_B = 5;

/// Default SR Class values
constexpr uint8_t default_sr_class = SR_CLASS_A;
constexpr uint8_t default_sr_class_priority = 3;
constexpr uint16_t default_sr_class_vid = 2;

//
// MSRP Listener Declaration Types - IEEE 802.1Q-2014 Clause 35.2.2.7.2, Table 35-3
//
enum class ListenerDeclaration : uint8_t
{
    Ignore = 0,
    AskingFailed = 1,
    Ready = 2,
    ReadyFailed = 3,
};

/// Get the name of a ListenerDeclaration for debugging/logging
[[nodiscard]] auto listener_declaration_name(ListenerDeclaration decl) noexcept -> std::string_view;

//
// MSRP Talker Failure Codes - IEEE 802.1Q-2014 Clause 35.2.2.8.7, Table 35-6
//
enum class FailureCode : uint8_t
{
    NoFailure = 0,
    InsufficientBandwidth = 1,
    InsufficientBridgeResources = 2,
    InsufficientBandwidthForTrafficClass = 3,
    StreamIdInUseByAnotherTalker = 4,
    StreamDestinationAddressAlreadyInUse = 5,
    StreamPreemptedByHigherRank = 6,
    ReportedLatencyHasChanged = 7,
    EgressPortIsNotAvbCapable = 8,
    UseADifferentDestinationAddress = 9,
    OutOfMsrpResources = 10,
    OutOfMmrpResources = 11,
    CannotStoreDestinationAddress = 12,
    RequestedPriorityIsNotAnSrClass = 13,
    MaxFrameSizeIsTooLargeForMedia = 14,
    MsrpMaxFanInPortsLimitHasBeenReached = 15,
    ChangesInFirstValueForRegisteredStreamId = 16,
    VlanIsBlockedOnThisEgressPort = 17,
    VlanTaggingIsDisabledOnThisEgressPort = 18,
    SrClassPriorityMismatch = 19,
};

/// Get the name of a FailureCode for debugging/logging
[[nodiscard]] auto failure_code_name(FailureCode code) noexcept -> std::string_view;

//
// MSRP Talker Rank - IEEE 802.1Q-2014 Clause 35.2.2.8.5
//
constexpr uint8_t RANK_EMERGENCY = 0;
constexpr uint8_t RANK_NON_EMERGENCY = 1;

/// Priority and Rank field bit layout
/// Bits 5-7: DataFramePriority (3 bits)
/// Bit 4: Rank (1 bit)
/// Bits 0-3: Reserved
constexpr uint8_t PRIORITY_AND_RANK_PRIORITY_SHIFT = 5;
constexpr uint8_t PRIORITY_AND_RANK_PRIORITY_MASK = 0xE0;
constexpr uint8_t PRIORITY_AND_RANK_RANK_SHIFT = 4;
constexpr uint8_t PRIORITY_AND_RANK_RANK_MASK = 0x10;

/// Encode DataFramePriority and Rank into a single byte
[[nodiscard]] constexpr auto encode_priority_and_rank(uint8_t priority, uint8_t rank) noexcept -> uint8_t
{
    return static_cast<uint8_t>(
        ((priority << PRIORITY_AND_RANK_PRIORITY_SHIFT) & PRIORITY_AND_RANK_PRIORITY_MASK) |
        ((rank << PRIORITY_AND_RANK_RANK_SHIFT) & PRIORITY_AND_RANK_RANK_MASK));
}

/// Decode DataFramePriority from Priority and Rank byte
[[nodiscard]] constexpr auto decode_priority(uint8_t priority_and_rank) noexcept -> uint8_t
{
    return (priority_and_rank & PRIORITY_AND_RANK_PRIORITY_MASK) >> PRIORITY_AND_RANK_PRIORITY_SHIFT;
}

/// Decode Rank from Priority and Rank byte
[[nodiscard]] constexpr auto decode_rank(uint8_t priority_and_rank) noexcept -> uint8_t
{
    return (priority_and_rank & PRIORITY_AND_RANK_RANK_MASK) >> PRIORITY_AND_RANK_RANK_SHIFT;
}

//
// Domain FirstValue - IEEE 802.1Q-2014 Clause 35.2.2.9.1
//
/// MSRP Domain FirstValue
/// IEEE 802.1Q-2014 Clause 35.2.2.9.1, 4 octets
struct DomainFirstValue
{
    static constexpr size_t LENGTH = 4;

    octet_t sr_class_id{default_sr_class};                 // 0: SRclassID
    octet_t sr_class_priority{default_sr_class_priority};  // 1: SRclassPriority
    doublet_t sr_class_vid{default_sr_class_vid};          // 2: SRclassVID

    auto operator<=>(DomainFirstValue const&) const noexcept = default;
};

static_assert(sizeof(DomainFirstValue) == 4);

//
// Listener FirstValue - IEEE 802.1Q-2014 Clause 35.2.2.7.1
//
/// MSRP Listener FirstValue
/// IEEE 802.1Q-2014 Clause 35.2.2.7.1, 8 octets
struct ListenerFirstValue
{
    static constexpr size_t LENGTH = 8;

    tsn::StreamId stream_id{};  // 0-7: StreamID

    auto operator<=>(ListenerFirstValue const&) const noexcept = default;
};

static_assert(sizeof(ListenerFirstValue) == 8);

//
// Talker Advertise FirstValue - IEEE 802.1Q-2014 Clause 35.2.2.8.1
//
/// MSRP Talker Advertise FirstValue
/// IEEE 802.1Q-2014 Clause 35.2.2.8.1, 25 octets
struct TalkerAdvertiseFirstValue
{
    static constexpr size_t LENGTH = 25;

    tsn::StreamId stream_id{};        // 0-7: StreamID
    Eui48 destination_address{};      // 8-13: DataFrameParameters.DestinationAddress
    doublet_t vlan_identifier{};      // 14-15: DataFrameParameters.VlanIdentifier
    doublet_t max_frame_size{};       // 16-17: TSpec.MaxFrameSize
    doublet_t max_interval_frames{};  // 18-19: TSpec.MaxIntervalFrames
    octet_t priority_and_rank{};      // 20: PriorityAndRank (Priority bits 5-7, Rank bit 4)
    quadlet_t accumulated_latency{};  // 21-24: AccumulatedLatency

    /// Get the DataFramePriority from the priority_and_rank field
    [[nodiscard]] constexpr auto get_priority() const noexcept -> uint8_t { return decode_priority(priority_and_rank.get()); }

    /// Set the DataFramePriority in the priority_and_rank field
    constexpr auto set_priority(uint8_t priority) noexcept { priority_and_rank = encode_priority_and_rank(priority, get_rank()); }

    /// Get the Rank from the priority_and_rank field
    [[nodiscard]] constexpr auto get_rank() const noexcept -> uint8_t { return decode_rank(priority_and_rank.get()); }

    /// Set the Rank in the priority_and_rank field
    constexpr auto set_rank(uint8_t rank) noexcept { priority_and_rank = encode_priority_and_rank(get_priority(), rank); }

    auto operator<=>(TalkerAdvertiseFirstValue const&) const noexcept = default;
};

static_assert(sizeof(TalkerAdvertiseFirstValue) == 25);

//
// Talker Failed FirstValue - IEEE 802.1Q-2014 Clause 35.2.2.8.1 + 35.2.2.8.7
//
/// MSRP Talker Failed FirstValue
/// IEEE 802.1Q-2014 Clause 35.2.2.8.1 + 35.2.2.8.7, 34 octets
struct TalkerFailedFirstValue
{
    static constexpr size_t LENGTH = 34;

    TalkerAdvertiseFirstValue advertise{};  // 0-24: Same as TalkerAdvertise
    Eui64 failure_bridge_id{};              // 25-32: FailureInformation.BridgeID
    octet_t failure_code{};                 // 33: FailureInformation.FailureCode

    /// Get the FailureCode as an enum
    [[nodiscard]] constexpr auto get_failure_code() const noexcept -> FailureCode
    {
        return static_cast<FailureCode>(failure_code.get());
    }

    /// Set the FailureCode from an enum
    constexpr auto set_failure_code(FailureCode code) noexcept { failure_code = static_cast<uint8_t>(code); }

    auto operator<=>(TalkerFailedFirstValue const&) const noexcept = default;
};

static_assert(sizeof(TalkerFailedFirstValue) == 34);

//
// FirstValue incrementation - IEEE 802.1Q-2014 Clause 35.2.2.{7,8,9}
//
// A VectorAttribute with NumberOfValues > 1 carries a single FirstValue; the
// attribute value for each subsequent AttributeEvent is the previous value
// incremented "in a manner defined by the application" (Clause 10.8.2.7). MSRP's
// per-AttributeType rules, as enumerated by AVnu End-Station test
// MSRP.End.c.35.1.11, are:
//
//   - Listener:        +1 to the StreamID Unique ID.
//   - Talker Advertise/Failed: +1 to BOTH the StreamID Unique ID and the
//                      DataFrameParameters destination_address.
//   - Domain:          +1 to BOTH SRclassID and SRclassPriority (VID unchanged).
//
// These are the canonical step functions used both when decoding a received
// multi-value vector and when coalescing consecutive declarations on transmit.

/// Listener: advance to the next attribute value in a multi-value vector.
constexpr void increment_first_value(ListenerFirstValue& fv) noexcept
{
    fv.stream_id.increment_unique_id();
}

/// Talker Advertise: advance to the next attribute value in a multi-value vector.
constexpr void increment_first_value(TalkerAdvertiseFirstValue& fv) noexcept
{
    fv.stream_id.increment_unique_id();
    fv.destination_address.from_uint64(fv.destination_address.to_uint64() + 1);
}

/// Talker Failed: advance to the next attribute value (increments the embedded
/// Talker Advertise; FailureInformation is carried unchanged).
constexpr void increment_first_value(TalkerFailedFirstValue& fv) noexcept
{
    increment_first_value(fv.advertise);
}

/// Domain: advance to the next attribute value in a multi-value vector.
constexpr void increment_first_value(DomainFirstValue& fv) noexcept
{
    fv.sr_class_id = static_cast<uint8_t>(fv.sr_class_id.get() + 1);
    fv.sr_class_priority = static_cast<uint8_t>(fv.sr_class_priority.get() + 1);
}

}  // namespace statusbar::srp::msrp

// Serialization traits
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::srp::msrp::DomainFirstValue> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::srp::msrp::ListenerFirstValue> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::srp::msrp::TalkerAdvertiseFirstValue> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::srp::msrp::TalkerFailedFirstValue> : std::true_type
{};

// Export using declarations for ADL
namespace statusbar::srp::msrp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::srp::msrp
