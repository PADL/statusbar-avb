#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AEM CONTROL descriptor "unit" encoding
/// (IEEE 1722.1-2021 Clause 7.3.3, "Control Value Units").
///
/// A control unit is a 16-bit packed field carried inside the value
/// trailer of a CONTROL descriptor. The high 8 bits are a signed
/// `multiplier` (power of 10 applied to the value); the low 8 bits are
/// a `code` identifying the physical quantity. Codes are grouped into
/// 8-entry sub-ranges by quantity type (unitless / time / frequency /
/// distance / temperature / ... / levels) — see `UNIT_GROUP_*`.

#include "statusbar/ieee/ieee.hpp"

#include <compare>
#include <cstdint>
#include <string_view>

namespace statusbar::atdecc::aem {

using ieee::doublet_t;
using ieee::octet_t;

/// Packed 16-bit unit encoding as it appears on the wire inside a
/// CONTROL descriptor's value trailer.
///
/// Byte 0 (high byte on the wire): int8_t multiplier — power of 10
///   applied to the raw control value (-128..127).
/// Byte 1 (low byte): uint8_t code — the physical-quantity code from
///   Table 7.7. See the UNIT_CODE_* constants below.
struct ControlUnits
{
    octet_t multiplier{0};  ///< signed int8, stored in the network-order byte
    octet_t code{0};

    constexpr ControlUnits() noexcept = default;
    constexpr ControlUnits(int8_t mul, uint8_t unit_code) noexcept
        : multiplier{static_cast<uint8_t>(mul)}
        , code{unit_code}
    {}

    /// Signed multiplier (power of 10 applied to the raw value).
    [[nodiscard]] constexpr auto signed_multiplier() const noexcept -> int8_t { return static_cast<int8_t>(multiplier.get()); }

    auto operator<=>(ControlUnits const&) const noexcept = default;
};

static_assert(sizeof(ControlUnits) == 2, "ControlUnits must be exactly 2 bytes on the wire");

//
// Group sub-ranges (high nibble of the unit code, step of 8)
//

constexpr uint8_t UNIT_GROUP_UNITLESS = 0x00;
constexpr uint8_t UNIT_GROUP_TIME = 0x08;
constexpr uint8_t UNIT_GROUP_FREQUENCY = 0x10;
constexpr uint8_t UNIT_GROUP_DISTANCE = 0x18;
constexpr uint8_t UNIT_GROUP_TEMPERATURE = 0x20;
constexpr uint8_t UNIT_GROUP_MASS = 0x28;
constexpr uint8_t UNIT_GROUP_VOLTAGE = 0x30;
constexpr uint8_t UNIT_GROUP_CURRENT = 0x38;
constexpr uint8_t UNIT_GROUP_POWER = 0x40;
constexpr uint8_t UNIT_GROUP_PRESSURE = 0x48;
constexpr uint8_t UNIT_GROUP_MEMORY = 0x50;
constexpr uint8_t UNIT_GROUP_MEMORY_BANDWIDTH = 0x58;
constexpr uint8_t UNIT_GROUP_LUMINOSITY = 0x60;
constexpr uint8_t UNIT_GROUP_ENERGY = 0x68;
constexpr uint8_t UNIT_GROUP_ANGLE = 0x70;
constexpr uint8_t UNIT_GROUP_FORCE = 0x78;
constexpr uint8_t UNIT_GROUP_RESISTANCE = 0x80;
constexpr uint8_t UNIT_GROUP_VELOCITY = 0x88;
constexpr uint8_t UNIT_GROUP_ACCELERATION = 0x90;
constexpr uint8_t UNIT_GROUP_FLUX = 0x98;
constexpr uint8_t UNIT_GROUP_AREA = 0xA0;
constexpr uint8_t UNIT_GROUP_VOLUME = 0xA8;
constexpr uint8_t UNIT_GROUP_LEVELS = 0xB0;

//
// Unit code constants — the full enumeration from Clause 7.3.3 tables.
//

// Unitless (0x00 – 0x07)
constexpr uint8_t UNIT_CODE_UNITLESS = 0x00;
constexpr uint8_t UNIT_CODE_COUNT = 0x01;
constexpr uint8_t UNIT_CODE_PERCENT = 0x02;
constexpr uint8_t UNIT_CODE_FSTOP = 0x03;

// Time (0x08 – 0x0f)
constexpr uint8_t UNIT_CODE_SECONDS = 0x08;
constexpr uint8_t UNIT_CODE_MINUTES = 0x09;
constexpr uint8_t UNIT_CODE_HOURS = 0x0A;
constexpr uint8_t UNIT_CODE_DAYS = 0x0B;
constexpr uint8_t UNIT_CODE_MONTHS = 0x0C;
constexpr uint8_t UNIT_CODE_YEARS = 0x0D;
constexpr uint8_t UNIT_CODE_SAMPLES = 0x0E;
constexpr uint8_t UNIT_CODE_FRAMES = 0x0F;

// Frequency (0x10 – 0x17)
constexpr uint8_t UNIT_CODE_HERTZ = 0x10;
constexpr uint8_t UNIT_CODE_SEMITONES = 0x11;
constexpr uint8_t UNIT_CODE_CENTS = 0x12;
constexpr uint8_t UNIT_CODE_OCTAVES = 0x13;
constexpr uint8_t UNIT_CODE_FPS = 0x14;

// Distance (0x18 – 0x1f)
constexpr uint8_t UNIT_CODE_METRES = 0x18;

// Temperature (0x20 – 0x27)
constexpr uint8_t UNIT_CODE_KELVIN = 0x20;

// Mass (0x28 – 0x2f)
constexpr uint8_t UNIT_CODE_GRAMS = 0x28;

// Voltage (0x30 – 0x37)
constexpr uint8_t UNIT_CODE_VOLTS = 0x30;
constexpr uint8_t UNIT_CODE_DBV = 0x31;
constexpr uint8_t UNIT_CODE_DBU = 0x32;

// Current (0x38 – 0x3f)
constexpr uint8_t UNIT_CODE_AMPS = 0x38;

// Power (0x40 – 0x47)
constexpr uint8_t UNIT_CODE_WATTS = 0x40;
constexpr uint8_t UNIT_CODE_DBM = 0x41;
constexpr uint8_t UNIT_CODE_DBW = 0x42;

// Pressure (0x48 – 0x4f)
constexpr uint8_t UNIT_CODE_PASCALS = 0x48;

// Memory (0x50 – 0x57)
constexpr uint8_t UNIT_CODE_BITS = 0x50;
constexpr uint8_t UNIT_CODE_BYTES = 0x51;
constexpr uint8_t UNIT_CODE_KIBIBYTES = 0x52;
constexpr uint8_t UNIT_CODE_MEBIBYTES = 0x53;
constexpr uint8_t UNIT_CODE_GIBIBYTES = 0x54;
constexpr uint8_t UNIT_CODE_TEBIBYTES = 0x55;

// Memory Bandwidth (0x58 – 0x5f)
constexpr uint8_t UNIT_CODE_BITS_PER_SEC = 0x58;
constexpr uint8_t UNIT_CODE_BYTES_PER_SEC = 0x59;
constexpr uint8_t UNIT_CODE_KIBIBYTES_PER_SEC = 0x5A;
constexpr uint8_t UNIT_CODE_MEBIBYTES_PER_SEC = 0x5B;
constexpr uint8_t UNIT_CODE_GIBIBYTES_PER_SEC = 0x5C;
constexpr uint8_t UNIT_CODE_TEBIBYTES_PER_SEC = 0x5D;

// Luminosity (0x60 – 0x67)
constexpr uint8_t UNIT_CODE_CANDELAS = 0x60;

// Energy (0x68 – 0x6f)
constexpr uint8_t UNIT_CODE_JOULES = 0x68;

// Angle (0x70 – 0x77)
constexpr uint8_t UNIT_CODE_RADIANS = 0x70;

// Force (0x78 – 0x7f)
constexpr uint8_t UNIT_CODE_NEWTONS = 0x78;

// Resistance (0x80 – 0x87)
constexpr uint8_t UNIT_CODE_OHMS = 0x80;

// Velocity (0x88 – 0x8f)
constexpr uint8_t UNIT_CODE_METRES_PER_SEC = 0x88;
constexpr uint8_t UNIT_CODE_RADIANS_PER_SEC = 0x89;

// Acceleration (0x90 – 0x97)
constexpr uint8_t UNIT_CODE_METRES_PER_SEC_SQUARED = 0x90;
constexpr uint8_t UNIT_CODE_RADIANS_PER_SEC_SQUARED = 0x91;

// Magnetic flux and fields (0x98 – 0x9f)
constexpr uint8_t UNIT_CODE_TESLAS = 0x98;
constexpr uint8_t UNIT_CODE_WEBERS = 0x99;
constexpr uint8_t UNIT_CODE_AMPS_PER_METRE = 0x9A;

// Area (0xa0 – 0xa7)
constexpr uint8_t UNIT_CODE_METRES_SQUARED = 0xA0;

// Volume (0xa8 – 0xaf)
constexpr uint8_t UNIT_CODE_METRES_CUBED = 0xA8;
constexpr uint8_t UNIT_CODE_LITRES = 0xA9;

// Levels and loudness (0xb0 – 0xbf)
constexpr uint8_t UNIT_CODE_DB = 0xB0;
constexpr uint8_t UNIT_CODE_DB_PEAK = 0xB1;
constexpr uint8_t UNIT_CODE_DB_RMS = 0xB2;
constexpr uint8_t UNIT_CODE_DBFS = 0xB3;
constexpr uint8_t UNIT_CODE_DBFS_PEAK = 0xB4;
constexpr uint8_t UNIT_CODE_DBFS_RMS = 0xB5;
constexpr uint8_t UNIT_CODE_DBTP = 0xB6;
constexpr uint8_t UNIT_CODE_DB_SPL_A = 0xB7;
constexpr uint8_t UNIT_CODE_DB_Z = 0xB8;
constexpr uint8_t UNIT_CODE_DB_SPL_C = 0xB9;
constexpr uint8_t UNIT_CODE_DB_SPL = 0xBA;
constexpr uint8_t UNIT_CODE_LU = 0xBB;
constexpr uint8_t UNIT_CODE_LUFS = 0xBC;
constexpr uint8_t UNIT_CODE_DB_A = 0xBD;

/// Return the group sub-range ID for a unit code. Groups are 8-entry
/// sub-ranges except Levels, which spans 16 entries (0xb0 — 0xbf).
/// Values >= 0xc0 are reserved; this returns 0xc0 for those.
/// @param code Unit code from byte 1 of the wire encoding
[[nodiscard]] constexpr auto unit_code_group(uint8_t code) noexcept -> uint8_t
{
    if (code >= 0xC0) {
        return 0xC0;
    }
    if (code >= UNIT_GROUP_LEVELS) {
        return UNIT_GROUP_LEVELS;
    }
    return static_cast<uint8_t>(code & 0xF8);
}

/// Human-readable name of a unit code (e.g. "HERTZ", "DB_SPL_A").
/// Returns "Reserved" for undefined codes within a group sub-range,
/// and "Reserved (>= 0xc0)" for codes above the last defined group.
/// @param code Unit code from byte 1 of the wire encoding
[[nodiscard]] auto control_unit_code_name(uint8_t code) noexcept -> std::string_view;

/// Short printable suffix for a unit code (e.g. "Hz", "dB", "%", "s").
/// Returns the empty string for codes that have no suffix ("---" in
/// the spec table) or for reserved codes. @see control_unit_code_name.
/// @param code Unit code from byte 1 of the wire encoding
[[nodiscard]] auto control_unit_code_suffix(uint8_t code) noexcept -> std::string_view;

}  // namespace statusbar::atdecc::aem
