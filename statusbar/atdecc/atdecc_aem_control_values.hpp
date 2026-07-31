#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AEM CONTROL descriptor `control_value_type` constants and wire-format
/// layout structs for the per-type value_details trailer.
/// (IEEE 1722.1-2021 Clause 7.3.5, "Control Values".)
///
/// A CONTROL descriptor carries a 16-bit `control_value_type` field:
///
///   ┌──┬──┬──────────────────────────────┐
///   │ r│ u│     value_type  (14 bits)    │
///   └──┴──┴──────────────────────────────┘
///    15 14 13                          0
///
/// - r (bit 15): read-only flag
/// - u (bit 14): unknown-value flag
/// - value_type (bits 13-0): CONTROL_LINEAR_INT8 … CONTROL_VENDOR
///
/// The value_types partition into four families that drive the
/// value_details wire layout:
///   Linear     (0x0000 — 0x0009): per-entry {min, max, step, default,
///                                 current, unit, string}
///   Selector   (0x000a — 0x0014): {current, default, option[0..N-1], unit}
///   Array      (0x0015 — 0x001e): {min, max, step, default, unit,
///                                 string, current[0..N-1]}
///   Specials                      UTF8, BODE_PLOT, SMPTE_TIME,
///                                 SAMPLE_RATE, GPTP_TIME, VENDOR
///
/// See atdecc_aem_units.hpp for the ControlUnits encoding used by the
/// per-family layouts.

#include "statusbar/atdecc/atdecc_aem_units.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace statusbar::atdecc::aem {

using ieee::doublet_t;
using ieee::octet_t;
using ieee::quadlet_t;

//
// control_value_type field bit layout
//

constexpr uint16_t CONTROL_VALUE_TYPE_READONLY_FLAG = 0x8000;
constexpr uint16_t CONTROL_VALUE_TYPE_UNKNOWN_FLAG = 0x4000;
constexpr uint16_t CONTROL_VALUE_TYPE_MASK = 0x3FFF;

/// Decoded view of a raw 16-bit control_value_type field.
struct ControlValueTypeBits
{
    bool read_only{false};   ///< bit 15 — value cannot be changed
    bool unknown{false};     ///< bit 14 — value field is not populated
    uint16_t value_type{0};  ///< bits 13..0 — one of the CONTROL_* value_type constants below
};

/// Unpack a raw 16-bit control_value_type field into its three components.
[[nodiscard]] constexpr auto unpack_control_value_type(uint16_t raw) noexcept -> ControlValueTypeBits
{
    return ControlValueTypeBits{
        .read_only = (raw & CONTROL_VALUE_TYPE_READONLY_FLAG) != 0,
        .unknown = (raw & CONTROL_VALUE_TYPE_UNKNOWN_FLAG) != 0,
        .value_type = static_cast<uint16_t>(raw & CONTROL_VALUE_TYPE_MASK),
    };
}

/// Pack read-only / unknown / value_type into the 16-bit wire field.
[[nodiscard]] constexpr auto pack_control_value_type(bool read_only, bool unknown, uint16_t value_type) noexcept -> uint16_t
{
    uint16_t raw = static_cast<uint16_t>(value_type & CONTROL_VALUE_TYPE_MASK);
    if (read_only) {
        raw |= CONTROL_VALUE_TYPE_READONLY_FLAG;
    }
    if (unknown) {
        raw |= CONTROL_VALUE_TYPE_UNKNOWN_FLAG;
    }
    return raw;
}

//
// value_type constants (IEEE 1722.1-2021 Table 7.12)
//

// Linear family (0x0000 — 0x0009)
constexpr uint16_t CONTROL_LINEAR_INT8 = 0x0000;
constexpr uint16_t CONTROL_LINEAR_UINT8 = 0x0001;
constexpr uint16_t CONTROL_LINEAR_INT16 = 0x0002;
constexpr uint16_t CONTROL_LINEAR_UINT16 = 0x0003;
constexpr uint16_t CONTROL_LINEAR_INT32 = 0x0004;
constexpr uint16_t CONTROL_LINEAR_UINT32 = 0x0005;
constexpr uint16_t CONTROL_LINEAR_INT64 = 0x0006;
constexpr uint16_t CONTROL_LINEAR_UINT64 = 0x0007;
constexpr uint16_t CONTROL_LINEAR_FLOAT = 0x0008;
constexpr uint16_t CONTROL_LINEAR_DOUBLE = 0x0009;

// Selector family (0x000a — 0x0014)
constexpr uint16_t CONTROL_SELECTOR_INT8 = 0x000A;
constexpr uint16_t CONTROL_SELECTOR_UINT8 = 0x000B;
constexpr uint16_t CONTROL_SELECTOR_INT16 = 0x000C;
constexpr uint16_t CONTROL_SELECTOR_UINT16 = 0x000D;
constexpr uint16_t CONTROL_SELECTOR_INT32 = 0x000E;
constexpr uint16_t CONTROL_SELECTOR_UINT32 = 0x000F;
constexpr uint16_t CONTROL_SELECTOR_INT64 = 0x0010;
constexpr uint16_t CONTROL_SELECTOR_UINT64 = 0x0011;
constexpr uint16_t CONTROL_SELECTOR_FLOAT = 0x0012;
constexpr uint16_t CONTROL_SELECTOR_DOUBLE = 0x0013;
constexpr uint16_t CONTROL_SELECTOR_STRING = 0x0014;

// Array family (0x0015 — 0x001e)
constexpr uint16_t CONTROL_ARRAY_INT8 = 0x0015;
constexpr uint16_t CONTROL_ARRAY_UINT8 = 0x0016;
constexpr uint16_t CONTROL_ARRAY_INT16 = 0x0017;
constexpr uint16_t CONTROL_ARRAY_UINT16 = 0x0018;
constexpr uint16_t CONTROL_ARRAY_INT32 = 0x0019;
constexpr uint16_t CONTROL_ARRAY_UINT32 = 0x001A;
constexpr uint16_t CONTROL_ARRAY_INT64 = 0x001B;
constexpr uint16_t CONTROL_ARRAY_UINT64 = 0x001C;
constexpr uint16_t CONTROL_ARRAY_FLOAT = 0x001D;
constexpr uint16_t CONTROL_ARRAY_DOUBLE = 0x001E;

// Specials
constexpr uint16_t CONTROL_UTF8 = 0x001F;
constexpr uint16_t CONTROL_BODE_PLOT = 0x0020;
constexpr uint16_t CONTROL_SMPTE_TIME = 0x0021;
constexpr uint16_t CONTROL_SAMPLE_RATE = 0x0022;
constexpr uint16_t CONTROL_GPTP_TIME = 0x0023;
constexpr uint16_t CONTROL_VENDOR = 0x3FFE;
constexpr uint16_t CONTROL_VALUE_TYPE_EXPANSION = 0x3FFF;

//
// Family predicates
//

[[nodiscard]] constexpr auto is_linear_value_type(uint16_t vt) noexcept -> bool
{
    return vt <= CONTROL_LINEAR_DOUBLE;
}

[[nodiscard]] constexpr auto is_selector_value_type(uint16_t vt) noexcept -> bool
{
    return vt >= CONTROL_SELECTOR_INT8 && vt <= CONTROL_SELECTOR_STRING;
}

[[nodiscard]] constexpr auto is_array_value_type(uint16_t vt) noexcept -> bool
{
    return vt >= CONTROL_ARRAY_INT8 && vt <= CONTROL_ARRAY_DOUBLE;
}

/// Element size V in octets (the "Value Size" column of Table 7.12).
/// Returns 0 for types whose element size is not a fixed constant
/// (CONTROL_UTF8, CONTROL_VENDOR, CONTROL_VALUE_TYPE_EXPANSION, or
/// unknown). CONTROL_SELECTOR_STRING returns 2 (localized_string_ref).
/// @param vt One of the 14-bit CONTROL_* value_type constants
[[nodiscard]] auto control_value_element_size(uint16_t vt) noexcept -> size_t;

/// Human-readable name of a control_value_type (e.g. "CONTROL_LINEAR_UINT16",
/// "CONTROL_BODE_PLOT"). Returns "Reserved" for undefined values, and
/// "EXPANSION" for CONTROL_VALUE_TYPE_EXPANSION.
/// @param vt One of the 14-bit CONTROL_* value_type constants
[[nodiscard]] auto control_value_type_name(uint16_t vt) noexcept -> char const*;

/// Compute the on-wire length of a CONTROL descriptor's value_details
/// trailer for the given control_value_type and number_of_values.
///
/// Per IEEE 1722.1-2021 Table 7.12:
///   Linear     L = N * (5V + 4)
///   Selector   L = (N + 2) * V + 2
///   Array      L = (N + 4) * V + 4         (but the spec table reads
///                                           4 + 4V + N*V — same value)
///   UTF-8      S (returned as 0 here; callers derive S from the
///              AECP control_data_length since it isn't encoded in
///              number_of_values)
///   BODE_PLOT  48 + N*12
///   SMPTE_TIME 10
///   SAMPLE_RATE 16 (Cor1-2025) or 4 (pre-Cor1); returns 16
///   GPTP_TIME  10
///   VENDOR     U (returned as 0 — same rationale as UTF-8)
///
/// Returns 0 for CONTROL_UTF8, CONTROL_VENDOR, CONTROL_VALUE_TYPE_EXPANSION,
/// and unrecognized value_types.
///
/// @param control_value_type 16-bit wire field (r/u flags are masked off)
/// @param number_of_values  N from the CONTROL descriptor
[[nodiscard]] auto control_value_details_length(uint16_t control_value_type, uint16_t number_of_values) noexcept -> size_t;

//
// Linear family per-entry wire layout
//
// Per IEEE 1722.1-2021 Table 7.13 a Linear entry is (5·V + 4) bytes:
//   min, max, step, default, current (V each) + unit (2) + string (2)
//

namespace detail {

/// Decode a big-endian wire buffer into the semantic type T. T may be an
/// integer type (signed or unsigned, 1/2/4/8 bytes) or a floating-point
/// type (float, double). The bytes are always interpreted in network
/// byte order.
template <typename T>
[[nodiscard]] constexpr auto decode_linear_field(std::array<uint8_t, sizeof(T)> const& bytes) noexcept -> T
{
    if constexpr (sizeof(T) == 1) {
        return std::bit_cast<T>(bytes[0]);
    } else {
        using U = std::conditional_t<sizeof(T) == 2, uint16_t, std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>>;
        U raw = 0;
        for (size_t i = 0; i < sizeof(T); ++i) {
            raw = static_cast<U>((raw << 8) | static_cast<U>(bytes[i]));
        }
        return std::bit_cast<T>(raw);
    }
}

/// Encode T into a big-endian wire buffer (inverse of decode_linear_field).
template <typename T>
constexpr auto encode_linear_field(T value, std::array<uint8_t, sizeof(T)>& bytes) noexcept -> void
{
    if constexpr (sizeof(T) == 1) {
        bytes[0] = std::bit_cast<uint8_t>(value);
    } else {
        using U = std::conditional_t<sizeof(T) == 2, uint16_t, std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>>;
        U raw = std::bit_cast<U>(value);
        for (size_t i = sizeof(T); i-- > 0;) {
            bytes[i] = static_cast<uint8_t>(raw & 0xFFU);
            raw >>= 8;
        }
    }
}

}  // namespace detail

/// Wire-format layout for a single entry in a linear control's value_details.
///
/// Template parameter T is the semantic element type matching the
/// control_value_type field. Storage is byte-wise so that integer and
/// floating-point types with 8-byte natural alignment do not pad the
/// struct (sizeof(Entry) must equal `5*V + 4` per Table 7.13).
///
/// Values are read via `minimum()`/`maximum()`/`step()`/`default_value()`
/// /`current()` accessors that decode network byte order; writers take
/// the semantic type and encode to the byte buffer.
///
///   int8_t / uint8_t           -> CONTROL_LINEAR_INT8 / UINT8
///   int16_t / uint16_t         -> CONTROL_LINEAR_INT16 / UINT16
///   int32_t / uint32_t / float -> CONTROL_LINEAR_INT32 / UINT32 / FLOAT
///   int64_t / uint64_t / double-> CONTROL_LINEAR_INT64 / UINT64 / DOUBLE
template <typename T>
struct LinearValueEntry
{
    using ElementBytes = std::array<uint8_t, sizeof(T)>;

    ElementBytes minimum_bytes{};
    ElementBytes maximum_bytes{};
    ElementBytes step_bytes{};
    ElementBytes default_bytes{};
    ElementBytes current_bytes{};
    ControlUnits unit{};
    doublet_t localized_string{0};

    [[nodiscard]] constexpr auto minimum() const noexcept -> T { return detail::decode_linear_field<T>(minimum_bytes); }
    [[nodiscard]] constexpr auto maximum() const noexcept -> T { return detail::decode_linear_field<T>(maximum_bytes); }
    [[nodiscard]] constexpr auto step() const noexcept -> T { return detail::decode_linear_field<T>(step_bytes); }
    [[nodiscard]] constexpr auto default_value() const noexcept -> T { return detail::decode_linear_field<T>(default_bytes); }
    [[nodiscard]] constexpr auto current() const noexcept -> T { return detail::decode_linear_field<T>(current_bytes); }

    constexpr auto set_minimum(T v) noexcept -> void { detail::encode_linear_field(v, minimum_bytes); }
    constexpr auto set_maximum(T v) noexcept -> void { detail::encode_linear_field(v, maximum_bytes); }
    constexpr auto set_step(T v) noexcept -> void { detail::encode_linear_field(v, step_bytes); }
    constexpr auto set_default_value(T v) noexcept -> void { detail::encode_linear_field(v, default_bytes); }
    constexpr auto set_current(T v) noexcept -> void { detail::encode_linear_field(v, current_bytes); }
};

/// Byte size of one linear value_details entry for an element of
/// @p element_size bytes: min, max, step, default, current + unit
/// doublet + localized-string doublet (Table 7.13). The runtime
/// counterpart of sizeof(LinearValueEntry<T>).
[[nodiscard]] constexpr auto linear_entry_size(size_t const element_size) noexcept -> size_t
{
    return (element_size * 5) + 4;
}

/// Byte offset of the `current` field within a linear value_details
/// entry. The runtime counterpart of offsetof(LinearValueEntry<T>,
/// current_bytes).
[[nodiscard]] constexpr auto linear_entry_current_offset(size_t const element_size) noexcept -> size_t
{
    return element_size * 4;
}

static_assert(linear_entry_size(sizeof(uint8_t)) == sizeof(LinearValueEntry<uint8_t>));
static_assert(linear_entry_size(sizeof(uint32_t)) == sizeof(LinearValueEntry<uint32_t>));
static_assert(linear_entry_current_offset(sizeof(uint8_t)) == offsetof(LinearValueEntry<uint8_t>, current_bytes));
static_assert(linear_entry_current_offset(sizeof(uint32_t)) == offsetof(LinearValueEntry<uint32_t>, current_bytes));

//
// Selector and Array families use variable-length layouts; the wire
// bytes are read/written via helpers in commit 4 (they need the
// CONTROL descriptor's number_of_values to know how many options /
// current values follow).
//

//
// Specials — fixed-size wire structs (except UTF8 and VENDOR which
// are raw byte blobs).
//

/// CONTROL_SMPTE_TIME value_details (IEEE 1722.1-2021 Table 7.18).
/// 10 bytes total.
struct SmpteTimeValue
{
    doublet_t hours{0};
    octet_t minutes{0};
    octet_t seconds{0};
    octet_t frames{0};
    doublet_t subframes{0};
    octet_t frames_per_second{0};
    octet_t drop_frame{0};
    octet_t pull{0};
};

static_assert(sizeof(SmpteTimeValue) == 10, "SmpteTimeValue must be exactly 10 bytes on the wire");

/// CONTROL_SAMPLE_RATE value_details (IEEE 1722.1-2021 Table 7.19).
/// 16 bytes. A pre-Cor1-2025 entity may only populate `current`
/// (4 bytes) — callers should consult the AECP control_data_length
/// before reading the later fields; the struct still defaults them
/// to zero so partial reads don't read uninitialized memory.
struct SampleRateValue
{
    quadlet_t current{0};
    quadlet_t default_value{0};
    quadlet_t minimum{0};
    quadlet_t maximum{0};
};

static_assert(sizeof(SampleRateValue) == 16, "SampleRateValue must be exactly 16 bytes on the wire");

/// CONTROL_GPTP_TIME value_details (IEEE 1722.1-2021 Table 7.20).
/// 10 bytes: 48-bit gptp_seconds followed by 32-bit gptp_nanoseconds.
struct GptpTimeValue
{
    std::array<uint8_t, 6> gptp_seconds{};
    quadlet_t gptp_nanoseconds{0};

    /// Read the 48-bit seconds field as a uint64_t in host byte order.
    [[nodiscard]] constexpr auto seconds() const noexcept -> uint64_t
    {
        return (static_cast<uint64_t>(gptp_seconds[0]) << 40) | (static_cast<uint64_t>(gptp_seconds[1]) << 32) |
            (static_cast<uint64_t>(gptp_seconds[2]) << 24) | (static_cast<uint64_t>(gptp_seconds[3]) << 16) |
            (static_cast<uint64_t>(gptp_seconds[4]) << 8) | static_cast<uint64_t>(gptp_seconds[5]);
    }

    constexpr auto set_seconds(uint64_t v) noexcept
    {
        gptp_seconds[0] = static_cast<uint8_t>((v >> 40) & 0xFF);
        gptp_seconds[1] = static_cast<uint8_t>((v >> 32) & 0xFF);
        gptp_seconds[2] = static_cast<uint8_t>((v >> 24) & 0xFF);
        gptp_seconds[3] = static_cast<uint8_t>((v >> 16) & 0xFF);
        gptp_seconds[4] = static_cast<uint8_t>((v >> 8) & 0xFF);
        gptp_seconds[5] = static_cast<uint8_t>(v & 0xFF);
    }
};

static_assert(sizeof(GptpTimeValue) == 10, "GptpTimeValue must be exactly 10 bytes on the wire");

/// CONTROL_BODE_PLOT value_details header (IEEE 1722.1-2021 Table 7.17).
/// 48 bytes of float min/max/step/default for each of frequency,
/// magnitude, and phase. Followed on the wire by N × BodePlotPoint
/// (12 bytes each) for the current values.
struct BodePlotHeader
{
    quadlet_t frequency_minimum{0};
    quadlet_t frequency_maximum{0};
    quadlet_t frequency_step{0};
    quadlet_t frequency_default{0};
    quadlet_t magnitude_minimum{0};
    quadlet_t magnitude_maximum{0};
    quadlet_t magnitude_step{0};
    quadlet_t magnitude_default{0};
    quadlet_t phase_minimum{0};
    quadlet_t phase_maximum{0};
    quadlet_t phase_step{0};
    quadlet_t phase_default{0};
};

static_assert(sizeof(BodePlotHeader) == 48, "BodePlotHeader must be exactly 48 bytes on the wire");

/// One current {frequency, magnitude, phase} triple in a Bode plot.
/// 12 bytes on the wire. Stored as three quadlet_t containing IEEE 754
/// single-precision float bit patterns; callers bit_cast to float after
/// reading .get().
struct BodePlotPoint
{
    quadlet_t frequency{0};
    quadlet_t magnitude{0};
    quadlet_t phase{0};
};

static_assert(sizeof(BodePlotPoint) == 12, "BodePlotPoint must be exactly 12 bytes on the wire");

//
// Sanity checks for LinearValueEntry sizes (Table 7.13, column T).
// T = 5*V + 4. The compiler may insert padding around the nested
// ControlUnits + doublet_t tail, so these static_asserts catch any
// accidental ABI drift.
//

static_assert(sizeof(LinearValueEntry<uint8_t>) == 9, "LinearValueEntry<uint8_t> wire size is T = 5*1 + 4 = 9");
static_assert(sizeof(LinearValueEntry<int8_t>) == 9, "LinearValueEntry<int8_t> wire size is T = 5*1 + 4 = 9");
static_assert(sizeof(LinearValueEntry<uint16_t>) == 14, "LinearValueEntry<uint16_t> wire size is T = 5*2 + 4 = 14");
static_assert(sizeof(LinearValueEntry<int16_t>) == 14, "LinearValueEntry<int16_t> wire size is T = 5*2 + 4 = 14");
static_assert(sizeof(LinearValueEntry<uint32_t>) == 24, "LinearValueEntry<uint32_t> wire size is T = 5*4 + 4 = 24");
static_assert(sizeof(LinearValueEntry<int32_t>) == 24, "LinearValueEntry<int32_t> wire size is T = 5*4 + 4 = 24");
static_assert(sizeof(LinearValueEntry<float>) == 24, "LinearValueEntry<float> wire size is T = 5*4 + 4 = 24");
static_assert(sizeof(LinearValueEntry<uint64_t>) == 44, "LinearValueEntry<uint64_t> wire size is T = 5*8 + 4 = 44");
static_assert(sizeof(LinearValueEntry<int64_t>) == 44, "LinearValueEntry<int64_t> wire size is T = 5*8 + 4 = 44");
static_assert(sizeof(LinearValueEntry<double>) == 44, "LinearValueEntry<double> wire size is T = 5*8 + 4 = 44");

}  // namespace statusbar::atdecc::aem
