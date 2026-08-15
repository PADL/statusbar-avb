#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// IEEE 802.1AS-2020 TLV types carried inside FollowUp and Signaling
// message suffixes.
//
// Only the TLVs that a slave-role follower needs to read are modeled:
//
//   - FollowUpInformationTLV (Clause 11.4.4.3) — carries
//       cumulativeScaledRateOffset, gmTimeBaseIndicator,
//       lastGmPhaseChange, scaledLastGmFreqChange
//     The first two are load-bearing for servo input; the others are
//     tracked for diagnostic reporting on grandmaster step events.
//
//   - MessageIntervalRequestTLV (Clause 10.5.4.3) — signaling TLV
//     used by the AVnu Automotive Profile for interval renegotiation.
//
//   - PathTraceTLV (Clause 10.6.3.5) — parsed for diagnostic display
//     only, not used for BMCA (we're slave-only).
//

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/gptp/gptp_base.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>

namespace statusbar::gptp {

//
// ScaledNs (96 bits) — IEEE 802.1AS-2020 Clause 6.4.3.1.
//
// Signed nanosecond time interval in units of 2^-16 ns. 80 MSBs are
// integer ns (signed), 16 LSBs are fractional ns.
//
// For slave processing we only need to (a) read the wire bytes,
// (b) expose the integer ns portion as int64_t for diagnostics.
// We store the raw 12 wire bytes and provide a lossy accessor that
// returns the low 64 bits of the integer portion. Values that exceed
// int64_t range are absurd in practice (2^63 ns is ≈ 292 years) and
// are clamped.
//
struct ScaledNs
{
    static constexpr size_t LENGTH = 12;

    /// Wire layout (per Clause 6.4.3.1), all network byte order:
    ///   integer_high — top 16 bits of the 80-bit signed integer ns
    ///                  (sign extension in practice)
    ///   integer_low  — low 64 bits of the 80-bit signed integer ns
    ///   fraction     — 16-bit unsigned fractional ns (2^-16 ns units)
    doublet_t integer_high{0};
    octlet_t integer_low{0};
    doublet_t fraction{0};

    constexpr ScaledNs() noexcept = default;

    /// Construct from a plain int64_t integer ns (no fraction).
    constexpr explicit ScaledNs(int64_t integer_ns) noexcept
        : integer_high{integer_ns < 0 ? uint16_t{0xFFFF} : uint16_t{0}}
        , integer_low{static_cast<uint64_t>(integer_ns)}
    {}

    /// Read the low 64 bits of the 80-bit integer ns portion.
    /// Returns sign-extended int64_t. The two MSB bytes of the 80-bit
    /// integer are discarded; callers needing the full range should
    /// consult `integer_high` directly.
    [[nodiscard]] constexpr auto integer_ns() const noexcept -> int64_t { return static_cast<int64_t>(integer_low.get()); }

    /// Read the 16-bit fractional nanoseconds (unsigned, 2^-16 ns units).
    [[nodiscard]] constexpr auto fractional_ns() const noexcept -> uint16_t { return fraction.get(); }

    [[nodiscard]] static constexpr auto size() noexcept -> size_t { return LENGTH; }

    auto operator<=>(ScaledNs const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(ScaledNs) == ScaledNs::LENGTH, "ScaledNs must be exactly 12 bytes");

//
// TLV type codes — IEEE 1588-2019 Table 52 / 802.1AS-2020 Clause 10.6.3.1
//
constexpr uint16_t TLV_TYPE_MANAGEMENT = 0x0001;
constexpr uint16_t TLV_TYPE_ORGANIZATION_EXTENSION = 0x0003;
constexpr uint16_t TLV_TYPE_PATH_TRACE = 0x0008;
constexpr uint16_t TLV_TYPE_ORGANIZATION_EXTENSION_PROPAGATE = 0x4000;
constexpr uint16_t TLV_TYPE_ORGANIZATION_EXTENSION_DO_NOT_PROPAGATE = 0x8000;

//
// IEEE 802.1 OUI used for gPTP organizational TLVs
//
constexpr std::array<uint8_t, 3> IEEE_802_1_OUI = {0x00, 0x80, 0xC2};

//
// Organization subtypes assigned within the IEEE_802_1_OUI space for
// gPTP TLVs — IEEE 802.1AS-2020 Clause 10.6.3.3
//
constexpr std::array<uint8_t, 3> ORG_SUBTYPE_FOLLOW_UP_INFORMATION = {0x00, 0x00, 0x01};
constexpr std::array<uint8_t, 3> ORG_SUBTYPE_MESSAGE_INTERVAL_REQUEST = {0x00, 0x00, 0x02};
constexpr std::array<uint8_t, 3> ORG_SUBTYPE_GPTP_CAPABLE = {0x00, 0x00, 0x04};
constexpr std::array<uint8_t, 3> ORG_SUBTYPE_GPTP_CAPABLE_MESSAGE_INTERVAL = {0x00, 0x00, 0x05};

//
// TlvHeader — common 4-byte prefix (type + length) shared by all TLVs.
//
struct TlvHeader
{
    static constexpr size_t LENGTH = 4;

    doublet_t tlv_type{};      // 0-1: TLV type
    doublet_t length_field{};  // 2-3: length of the following value in bytes

    constexpr TlvHeader() noexcept = default;

    constexpr TlvHeader(uint16_t type, uint16_t len) noexcept
        : tlv_type{type}
        , length_field{len}
    {}

    [[nodiscard]] static constexpr auto size() noexcept -> size_t { return LENGTH; }

    auto operator<=>(TlvHeader const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(TlvHeader) == 4, "TlvHeader must be exactly 4 bytes");

//
// FollowUpInformationTLV — IEEE 802.1AS-2020 Clause 11.4.4.3.
//
// Layout (32 bytes total, including 4-byte TLV header):
//
//   TlvHeader (type = TLV_TYPE_ORGANIZATION_EXTENSION = 0x0003,
//              length_field = 28)
//   organization_id[3]            = IEEE_802_1_OUI
//   organization_sub_type[3]      = ORG_SUBTYPE_FOLLOW_UP_INFORMATION
//   cumulative_scaled_rate_offset (int32): signed, 2^-41 scale
//   gm_time_base_indicator (uint16)
//   last_gm_phase_change (ScaledNs, 12 bytes)
//   scaled_last_gm_freq_change (int32): signed, 2^-41 scale
//
struct FollowUpInformationTLV
{
    static constexpr size_t LENGTH = 32;
    static constexpr uint16_t EXPECTED_LENGTH_FIELD = 28;

    TlvHeader header{};
    std::array<uint8_t, 3> organization_id{};
    std::array<uint8_t, 3> organization_sub_type{};
    quadlet_t cumulative_scaled_rate_offset{};  // int32 signed, 2^-41
    doublet_t gm_time_base_indicator{};
    ScaledNs last_gm_phase_change{};
    quadlet_t scaled_last_gm_freq_change{};  // int32 signed, 2^-41

    constexpr FollowUpInformationTLV() noexcept = default;

    /// Initialize a FollowUp Information TLV with empty values.
    constexpr void init() noexcept
    {
        header = TlvHeader{TLV_TYPE_ORGANIZATION_EXTENSION, EXPECTED_LENGTH_FIELD};
        organization_id = IEEE_802_1_OUI;
        organization_sub_type = ORG_SUBTYPE_FOLLOW_UP_INFORMATION;
        cumulative_scaled_rate_offset = 0;
        gm_time_base_indicator = 0;
        last_gm_phase_change = ScaledNs{};
        scaled_last_gm_freq_change = 0;
    }

    /// Returns true if this TLV's organization id / subtype matches
    /// IEEE 802.1 FollowUp Information.
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        return header.tlv_type.get() == TLV_TYPE_ORGANIZATION_EXTENSION && header.length_field.get() == EXPECTED_LENGTH_FIELD &&
            organization_id == IEEE_802_1_OUI && organization_sub_type == ORG_SUBTYPE_FOLLOW_UP_INFORMATION;
    }

    /// Get the signed cumulative scaled rate offset as int32_t.
    [[nodiscard]] constexpr auto get_cumulative_scaled_rate_offset() const noexcept -> int32_t
    {
        return static_cast<int32_t>(cumulative_scaled_rate_offset.get());
    }

    constexpr void set_cumulative_scaled_rate_offset(int32_t v) noexcept
    {
        cumulative_scaled_rate_offset = static_cast<uint32_t>(v);
    }

    /// Get the scaled last GM frequency change as int32_t.
    [[nodiscard]] constexpr auto get_scaled_last_gm_freq_change() const noexcept -> int32_t
    {
        return static_cast<int32_t>(scaled_last_gm_freq_change.get());
    }

    constexpr void set_scaled_last_gm_freq_change(int32_t v) noexcept { scaled_last_gm_freq_change = static_cast<uint32_t>(v); }

    [[nodiscard]] static constexpr auto size() noexcept -> size_t { return LENGTH; }

    auto operator<=>(FollowUpInformationTLV const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(FollowUpInformationTLV) == FollowUpInformationTLV::LENGTH);
static_assert(offsetof(FollowUpInformationTLV, header) == 0);
static_assert(offsetof(FollowUpInformationTLV, organization_id) == 4);
static_assert(offsetof(FollowUpInformationTLV, organization_sub_type) == 7);
static_assert(offsetof(FollowUpInformationTLV, cumulative_scaled_rate_offset) == 10);
static_assert(offsetof(FollowUpInformationTLV, gm_time_base_indicator) == 14);
static_assert(offsetof(FollowUpInformationTLV, last_gm_phase_change) == 16);
static_assert(offsetof(FollowUpInformationTLV, scaled_last_gm_freq_change) == 28);

//
// MessageIntervalRequestTLV — IEEE 802.1AS-2020 Clause 10.6.4.4.5.
//
// Sent inside Signaling messages to request interval changes. Used
// by the AVnu Automotive Profile for runtime interval renegotiation.
// 16 bytes total.
//
struct MessageIntervalRequestTLV
{
    static constexpr size_t LENGTH = 16;
    static constexpr uint16_t EXPECTED_LENGTH_FIELD = 12;

    TlvHeader header{};
    std::array<uint8_t, 3> organization_id{};
    std::array<uint8_t, 3> organization_sub_type{};
    int8_t link_delay_interval{0};  // log2(seconds); 0x7F = no change, 0x7E = stop, -128 = initial
    int8_t time_sync_interval{0};
    int8_t announce_interval{0};
    octet_t flags{0};
    octet_t reserved{0};
    octet_t reserved2{0};

    constexpr MessageIntervalRequestTLV() noexcept = default;

    constexpr void init() noexcept
    {
        header = TlvHeader{TLV_TYPE_ORGANIZATION_EXTENSION, EXPECTED_LENGTH_FIELD};
        organization_id = IEEE_802_1_OUI;
        organization_sub_type = ORG_SUBTYPE_MESSAGE_INTERVAL_REQUEST;
        link_delay_interval = 0;
        time_sync_interval = 0;
        announce_interval = 0;
        flags = 0;
        reserved = 0;
        reserved2 = 0;
    }

    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        return header.tlv_type.get() == TLV_TYPE_ORGANIZATION_EXTENSION && header.length_field.get() == EXPECTED_LENGTH_FIELD &&
            organization_id == IEEE_802_1_OUI && organization_sub_type == ORG_SUBTYPE_MESSAGE_INTERVAL_REQUEST;
    }

    [[nodiscard]] static constexpr auto size() noexcept -> size_t { return LENGTH; }

    auto operator<=>(MessageIntervalRequestTLV const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(MessageIntervalRequestTLV) == MessageIntervalRequestTLV::LENGTH);

}  // namespace statusbar::gptp

// Serialization traits
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::gptp::ScaledNs> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::gptp::TlvHeader> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::gptp::FollowUpInformationTLV> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::gptp::MessageIntervalRequestTLV> : std::true_type
{};

namespace statusbar::gptp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::gptp
