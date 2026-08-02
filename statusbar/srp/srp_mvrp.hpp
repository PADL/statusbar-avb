#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/srp/srp_mrp.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>
#include <type_traits>

namespace statusbar::srp::mvrp {

using ieee::doublet_t;
using ieee::octet_t;
using srp::mrp::AttributeEvent;

//
// MVRP Constants - IEEE 802.1Q-2014 Clause 11.2
//
/// MVRP Protocol Version
/// IEEE 802.1Q-2014 Clause 11.2.3.1.2
constexpr uint8_t PROTOCOL_VERSION = 0;

/// MVRP EtherType
/// IEEE 802.1Q-2014 Clause 10.5, Table 10-2
constexpr uint16_t ETHERTYPE = 0x88F5;

/// MVRP Application Address (Nearest Customer Bridge)
/// IEEE 802.1Q-2014 Clause 10.5, Table 10-1
constexpr uint64_t APPLICATION_ADDRESS = 0x0180C2000021;

//
// MVRP Attribute Types - IEEE 802.1Q-2014 Clause 11.2.3.1.5
//
enum class AttributeType : uint8_t
{
    VlanIdentifier = 1,
};

/// Get the name of an AttributeType for debugging/logging
[[nodiscard]] auto attribute_type_name(AttributeType type) noexcept -> std::string_view;

//
// MVRP Attribute Lengths - IEEE 802.1Q-2014 Clause 11.2.3.1.5
//
enum class AttributeLength : uint8_t
{
    VlanIdentifier = 2,
};

//
// VLAN Identifier FirstValue - IEEE 802.1Q-2014 Clause 11.2.3.1.5
//
/// MVRP VLAN Identifier FirstValue
/// IEEE 802.1Q-2014 Clause 11.2.3.1.5, 2 octets
struct VlanIdentifierFirstValue
{
    static constexpr size_t LENGTH = 2;

    doublet_t vlan_identifier{};  // 0-1: VID (12 bits, bits 0-11)

    /// Get the 12-bit VLAN ID
    [[nodiscard]] constexpr auto get_vid() const noexcept -> uint16_t { return vlan_identifier.get() & 0x0FFF; }

    /// Set the 12-bit VLAN ID
    constexpr auto set_vid(uint16_t vid) noexcept { vlan_identifier = vid & 0x0FFF; }

    auto operator<=>(VlanIdentifierFirstValue const&) const noexcept = default;
};

static_assert(sizeof(VlanIdentifierFirstValue) == 2);

}  // namespace statusbar::srp::mvrp

// Serialization traits
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::srp::mvrp::VlanIdentifierFirstValue> : std::true_type
{};

// Export using declarations for ADL
namespace statusbar::srp::mvrp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::srp::mvrp
