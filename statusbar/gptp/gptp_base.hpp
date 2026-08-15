#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

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

namespace statusbar::gptp {

using ieee::doublet_t;
using ieee::octet_t;
using ieee::octlet_t;
using ieee::quadlet_t;
using ieee::sextlet_t;
using statusbar::BufferError;
using statusbar::failure;
using statusbar::StatusValue;
using statusbar::success;

//
// gPTP Constants (IEEE 802.1AS)
//
constexpr uint16_t GPTP_ETHERTYPE = 0x88F7;

/// gPTP Multicast MAC address - canonical definition in ieee::protocols::GPTP_MULTICAST_MAC
inline constexpr auto const& GPTP_MULTICAST_MAC = ieee::protocols::GPTP_MULTICAST_MAC;

// Message types
constexpr uint8_t MESSAGE_TYPE_SYNC = 0;
constexpr uint8_t MESSAGE_TYPE_DELAY_REQ = 1;
constexpr uint8_t MESSAGE_TYPE_PDELAY_REQ = 2;
constexpr uint8_t MESSAGE_TYPE_PDELAY_RESP = 3;
constexpr uint8_t MESSAGE_TYPE_FOLLOW_UP = 8;
constexpr uint8_t MESSAGE_TYPE_DELAY_RESP = 9;
constexpr uint8_t MESSAGE_TYPE_PDELAY_RESP_FOLLOW_UP = 10;
constexpr uint8_t MESSAGE_TYPE_ANNOUNCE = 11;
constexpr uint8_t MESSAGE_TYPE_SIGNALING = 12;
constexpr uint8_t MESSAGE_TYPE_MANAGEMENT = 13;

// Version constants
constexpr uint8_t VERSION_PTP = 2;
constexpr uint8_t SDO_ID = 1;

// Message sizes
constexpr size_t HEADER_LENGTH = 34;
constexpr size_t SYNC_MESSAGE_LENGTH = 44;
constexpr size_t FOLLOW_UP_MESSAGE_LENGTH = 44;
constexpr size_t PDELAY_REQ_MESSAGE_LENGTH = 54;
constexpr size_t PDELAY_RESP_MESSAGE_LENGTH = 54;

//
// ClockIdentity (8 bytes) - use the canonical type from statusbar.tsn
//
using ClockIdentity = tsn::ClockIdentity;

//
// SourcePortIdentity (10 bytes)
//
struct SourcePortIdentity
{
    static constexpr size_t LENGTH = 10;
    ClockIdentity clock_identity;
    doublet_t port_number;

    constexpr SourcePortIdentity() noexcept
        : clock_identity{}
        , port_number{0}
    {}

    constexpr SourcePortIdentity(ClockIdentity const& clock_id, uint16_t port) noexcept
        : clock_identity{clock_id}
        , port_number{port}
    {}

    [[nodiscard]] static constexpr auto size() noexcept -> size_t { return LENGTH; }

    auto operator<=>(SourcePortIdentity const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(SourcePortIdentity) == 10, "SourcePortIdentity must be exactly 10 bytes");

//
// Timestamp (10 bytes) - 6 bytes seconds + 4 bytes nanoseconds
//
struct Timestamp
{
    static constexpr size_t LENGTH = 10;

    // 48-bit seconds + 32-bit nanoseconds, both network byte order
    // (ieee ordered types: byte-backed, alignment 1, no padding).
    sextlet_t seconds_field;
    quadlet_t nanoseconds_field;

    constexpr Timestamp() noexcept
        : seconds_field{0}
        , nanoseconds_field{0}
    {}

    constexpr Timestamp(uint64_t secs, uint32_t nsecs) noexcept
        : seconds_field{secs}
        , nanoseconds_field{nsecs}
    {}

    [[nodiscard]] static constexpr auto size() noexcept -> size_t { return LENGTH; }

    [[nodiscard]] constexpr auto seconds() const noexcept -> uint64_t { return seconds_field.get(); }

    constexpr void set_seconds(uint64_t secs) noexcept { seconds_field = secs; }

    [[nodiscard]] constexpr auto nanos() const noexcept -> uint32_t { return nanoseconds_field.get(); }

    constexpr void set_nanos(uint32_t nsecs) noexcept { nanoseconds_field = nsecs; }

    auto operator<=>(Timestamp const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(Timestamp) == 10, "Timestamp must be exactly 10 bytes");

//
// ClockQuality (4 bytes)
//
struct ClockQuality
{
    static constexpr size_t LENGTH = 4;
    octet_t clock_class;
    octet_t clock_accuracy;
    doublet_t offset_scaled_log_variance;

    constexpr ClockQuality() noexcept
        : clock_class{0}
        , clock_accuracy{0}
        , offset_scaled_log_variance{0}
    {}

    constexpr ClockQuality(uint8_t cls, uint8_t accuracy, uint16_t variance) noexcept
        : clock_class{cls}
        , clock_accuracy{accuracy}
        , offset_scaled_log_variance{variance}
    {}

    [[nodiscard]] static constexpr auto size() noexcept -> size_t { return LENGTH; }

    auto operator<=>(ClockQuality const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(ClockQuality) == 4, "ClockQuality must be exactly 4 bytes");

/// Get human-readable name for gPTP message type
[[nodiscard]] auto message_type_name(uint8_t msg_type) noexcept -> std::string_view;

}  // namespace statusbar::gptp

// Serialization traits - gPTP base structs are packed wire format
// Note: ClockIdentity trait is defined in statusbar.tsn:clock_identity
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::gptp::SourcePortIdentity> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::gptp::Timestamp> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::gptp::ClockQuality> : std::true_type
{};

// Export using declarations for ADL to find the template functions
namespace statusbar::gptp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::gptp
