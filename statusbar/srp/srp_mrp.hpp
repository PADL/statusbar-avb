#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>
#include <type_traits>

namespace statusbar::srp::mrp {

using ieee::doublet_t;
using ieee::octet_t;
using ieee::quadlet_t;

/// Read a 16-bit big-endian value at byte position @p pos of an MRP PDU
/// (endmarks, AttributeListLength, VLAN first values). MRP parsing is
/// cursor-driven, so bounds are checked by the caller before reading.
[[nodiscard]] inline auto read_doublet_at(std::span<uint8_t const> pdu, size_t pos) noexcept -> uint16_t
{
    doublet_t v{};
    statusbar::span_load(v, pdu.subspan(pos, sizeof(v)));
    return v.get();
}

//
// MRP Constants - IEEE 802.1Q-2014 Clause 10
//
/// End mark value indicating end of attribute list or message
/// IEEE 802.1Q-2014 Clause 10.8.2.3
constexpr uint16_t END_MARK = 0x0000;

/// MRP Protocol Version
/// IEEE 802.1Q-2014 Clause 10.8.2.1
constexpr uint8_t PROTOCOL_VERSION = 0;

// MRP Timer Values from IEEE 802.1Q-2014 Clause 10.7.11, Table 10-7
/// JoinTime: 200 ms
constexpr int64_t TIMER_JOIN_TIME_NS = INT64_C(200) * 1000 * 1000;
/// LeaveTime: 800 ms
constexpr int64_t TIMER_LEAVE_TIME_NS = INT64_C(800) * 1000 * 1000;
/// LeaveAllTime: 12 seconds
constexpr int64_t TIMER_LEAVE_ALL_TIME_NS = INT64_C(12) * 1000 * 1000 * 1000;

//
// MRP Attribute Events - IEEE 802.1Q-2014 Clause 10.8.2.5
//
/// AttributeEvent encoding
/// IEEE 802.1Q-2014 Clause 10.8.2.5, Table 10-3
enum class AttributeEvent : uint8_t
{
    New = 0,
    JoinIn = 1,
    In = 2,
    JoinMt = 3,
    Mt = 4,
    Lv = 5,
};

/// Get the name of an AttributeEvent for debugging/logging
[[nodiscard]] auto attribute_event_name(AttributeEvent event) noexcept -> std::string_view;

//
// MRP Vector Header - IEEE 802.1Q-2014 Clause 10.8.2.6
//
/// VectorHeader bit definitions
/// Bits 0-12: NumberOfValues (0-8191)
/// Bit 13: LeaveAllEvent
/// Bits 14-15: Reserved (must be 0)
constexpr uint16_t VECTOR_HEADER_NUMBER_OF_VALUES_MASK = 0x1FFF;
constexpr uint16_t VECTOR_HEADER_LEAVE_ALL_BIT = 0x2000;

/// Calculate the VectorHeader value
/// IEEE 802.1Q-2014 Clause 10.8.2.6
[[nodiscard]] constexpr auto calculate_vector_header(bool leave_all, uint16_t number_of_values) noexcept -> uint16_t
{
    uint16_t const leave_all_bit = leave_all ? VECTOR_HEADER_LEAVE_ALL_BIT : 0;
    return leave_all_bit | (number_of_values & VECTOR_HEADER_NUMBER_OF_VALUES_MASK);
}

/// Extract the NumberOfValues from a VectorHeader
[[nodiscard]] constexpr auto extract_number_of_values(uint16_t vector_header) noexcept -> uint16_t
{
    return vector_header & VECTOR_HEADER_NUMBER_OF_VALUES_MASK;
}

/// Extract the LeaveAllEvent flag from a VectorHeader
[[nodiscard]] constexpr auto extract_leave_all(uint16_t vector_header) noexcept -> bool
{
    return (vector_header & VECTOR_HEADER_LEAVE_ALL_BIT) != 0;
}

//
// ThreePacked Event Encoding - IEEE 802.1Q-2014 Clause 10.8.2.10.1
//
/// Pack 3 AttributeEvent values into one octet
/// IEEE 802.1Q-2014 Clause 10.8.2.10.1
/// Value = (first × 36) + (second × 6) + third
[[nodiscard]] constexpr auto pack3_events(AttributeEvent first, AttributeEvent second, AttributeEvent third) noexcept -> uint8_t
{
    return static_cast<uint8_t>((static_cast<int>(first) * 36) + (static_cast<int>(second) * 6) + static_cast<int>(third));
}

/// Unpack 3 AttributeEvent values from one octet
/// IEEE 802.1Q-2014 Clause 10.8.2.10.1
struct ThreePackedEvents
{
    AttributeEvent first{};
    AttributeEvent second{};
    AttributeEvent third{};
};

[[nodiscard]] constexpr auto unpack3_events(uint8_t value) noexcept -> ThreePackedEvents
{
    int const first_value = value / 36;
    int const second_value = (value / 6) % 6;
    int const third_value = value % 6;

    return ThreePackedEvents{
        .first = static_cast<AttributeEvent>(first_value),
        .second = static_cast<AttributeEvent>(second_value),
        .third = static_cast<AttributeEvent>(third_value),
    };
}

/// Select the event at attribute index @p i (uses i % 3 for the position within the
/// packed octet), so callers don't hand-write the three-way switch.
[[nodiscard]] constexpr auto nth_of_three(ThreePackedEvents const& p, size_t i) noexcept -> AttributeEvent
{
    switch (i % 3) {
        case 0:
            return p.first;
        case 1:
            return p.second;
        default:
            return p.third;
    }
}

/// Calculate the number of octets needed to encode N events in ThreePacked format
[[nodiscard]] constexpr auto threepacked_octet_count(size_t number_of_events) noexcept -> size_t
{
    return (number_of_events + 2) / 3;
}

//
// FourPacked Event Encoding - IEEE 802.1Q-2014 Clause 10.8.2.10.2
// Used by MSRP Listener for declaration types
//
/// Pack 4 declaration values (0-3) into one octet
/// IEEE 802.1Q-2014 Clause 35.2.2.7.2
/// Value = (first × 64) + (second × 16) + (third × 4) + fourth
[[nodiscard]] constexpr auto pack4_declarations(uint8_t first, uint8_t second, uint8_t third, uint8_t fourth) noexcept -> uint8_t
{
    return static_cast<uint8_t>((first << 6) | (second << 4) | (third << 2) | fourth);
}

/// Unpack 4 declaration values from one octet
struct FourPackedDeclarations
{
    uint8_t first{};
    uint8_t second{};
    uint8_t third{};
    uint8_t fourth{};
};

[[nodiscard]] constexpr auto unpack4_declarations(uint8_t value) noexcept -> FourPackedDeclarations
{
    return FourPackedDeclarations{
        .first = static_cast<uint8_t>((value >> 6) & 0x03),
        .second = static_cast<uint8_t>((value >> 4) & 0x03),
        .third = static_cast<uint8_t>((value >> 2) & 0x03),
        .fourth = static_cast<uint8_t>(value & 0x03),
    };
}

/// Select the declaration at attribute index @p i (uses i % 4 for the position within
/// the packed octet), so callers don't hand-write the four-way switch.
[[nodiscard]] constexpr auto nth_of_four(FourPackedDeclarations const& p, size_t i) noexcept -> uint8_t
{
    switch (i % 4) {
        case 0:
            return p.first;
        case 1:
            return p.second;
        case 2:
            return p.third;
        default:
            return p.fourth;
    }
}

/// Calculate the number of octets needed to encode N declaration values in FourPacked format
[[nodiscard]] constexpr auto fourpacked_octet_count(size_t number_of_declarations) noexcept -> size_t
{
    return (number_of_declarations + 3) / 4;
}

//
// MRP Message Header - IEEE 802.1Q-2014 Clause 10.8.2
//
/// MRP Message structure (variable length)
/// IEEE 802.1Q-2014 Clause 10.8.2.2
///
/// Wire format:
/// - AttributeType (1 octet)
/// - AttributeLength (1 octet)
/// - AttributeList (variable)
///   - AttributeListLength (2 octets)
///   - VectorAttribute(s)...
///   - EndMark (2 octets, 0x0000)
/// - EndMark (2 octets, 0x0000) at end of all messages

/// MRP Attribute List Header
struct AttributeListHeader
{
    static constexpr size_t LENGTH = 2;

    octet_t attribute_type{};    // 0
    octet_t attribute_length{};  // 1

    auto operator<=>(AttributeListHeader const&) const noexcept = default;
};

static_assert(sizeof(AttributeListHeader) == 2);

/// MRP Vector Attribute Header (within AttributeList)
struct VectorAttributeHeader
{
    static constexpr size_t LENGTH = 2;

    doublet_t vector_header{};  // 0: LeaveAllEvent (bit 13), NumberOfValues (bits 0-12)

    [[nodiscard]] constexpr auto get_leave_all() const noexcept -> bool { return extract_leave_all(vector_header.get()); }

    [[nodiscard]] constexpr auto get_number_of_values() const noexcept -> uint16_t
    {
        return extract_number_of_values(vector_header.get());
    }

    constexpr void set(bool leave_all, uint16_t number_of_values) noexcept
    {
        vector_header = calculate_vector_header(leave_all, number_of_values);
    }

    auto operator<=>(VectorAttributeHeader const&) const noexcept = default;
};

static_assert(sizeof(VectorAttributeHeader) == 2);

}  // namespace statusbar::srp::mrp

// Serialization traits
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::srp::mrp::AttributeListHeader> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::srp::mrp::VectorAttributeHeader> : std::true_type
{};

// Export using declarations for ADL
namespace statusbar::srp::mrp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::srp::mrp
