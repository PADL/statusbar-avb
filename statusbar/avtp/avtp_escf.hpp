#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ESCF - ECC Signed Control Format - IEEE 1722-2016 Clause 16
/// Discrete signed control PDU header
///
/// Wire format (12-byte header + variable signed_payload):
///   Byte 0:     subtype (0xEC)
///   Byte 1:     r(1)|version(3)|sig(4)
///   Bytes 2-3:  reserved(5)|control_data_length(11)
///   Bytes 4-11: key_id (8 bytes, EUI-64)
///   Bytes 12+:  signed_payload

#include "statusbar/avtp/avtp_types.hpp"
#include "statusbar/ieee/ieee_base.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace statusbar::avtp {

using ieee::doublet_t;
using ieee::Eui64;
using ieee::octet_t;

//
// ESCF Signature Algorithm Values - IEEE 1722-2016 Table 46
//
/// ESCF sig field values
enum class EscfSigMode : uint8_t
{
    ecc1 = 0x00U,  ///< ECC1 signature algorithm
    // 0x01 - 0x0F: Reserved
};

/// Get human-readable name for ESCF signature mode
/// @param sig The ESCF signature mode value
[[nodiscard]] auto escf_sig_mode_name(uint8_t sig) noexcept -> std::string_view;

//
// EscfPdu - IEEE 1722-2016 Clause 16
// Wire format: 12-byte header + variable signed_payload
//
/// ESCF Protocol Data Unit Header - IEEE 1722-2016 Clause 16 (subtype 0xEC)
struct EscfPdu
{
    /// Total length of ESCF header on wire
    static constexpr size_t HEADER_LENGTH = 12;

    // Byte 0: subtype[7:0] = 0xEC for ESCF
    octet_t subtype;

    // Byte 1: r[7] | version[6:4] | sig[3:0]
    octet_t r_version_sig;

    // Bytes 2-3: reserved[15:11] | control_data_length[10:0]
    doublet_t rsv_control_data_length;

    // Bytes 4-11: key_id (EUI-64)
    Eui64 key_id_;

    // Accessors

    /// Get the version field (bits 6:4)
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return r_version_sig.get_bits(0x70U, 4); }

    /// Get the sig field (bits 3:0)
    [[nodiscard]] constexpr auto sig() const noexcept -> uint8_t { return r_version_sig.get_bits(0x0FU, 0); }

    /// Set the sig field (bits 3:0)
    /// @param value Signature algorithm (4 bits)
    constexpr void set_sig(uint8_t const value) noexcept { r_version_sig.set_bits(0x0FU, 0, value); }

    /// Get the control_data_length (11-bit field)
    [[nodiscard]] constexpr auto control_data_length() const noexcept -> uint16_t
    {
        return static_cast<uint16_t>(rsv_control_data_length.get() & 0x07FFU);
    }

    /// Set the control_data_length (11-bit field)
    /// @param length Payload length in octets (max 2047)
    constexpr void set_control_data_length(uint16_t const length) noexcept
    {
        uint16_t const current = rsv_control_data_length.get();
        rsv_control_data_length = static_cast<uint16_t>((current & 0xF800U) | (length & 0x07FFU));
    }

    /// Get the key_id
    [[nodiscard]] constexpr auto key_id() const noexcept -> Eui64 { return key_id_; }

    /// Set the key_id
    /// @param kid The key identifier (EUI-64)
    constexpr void set_key_id(Eui64 const& kid) noexcept { key_id_ = kid; }

    // Initialization

    /// Initialize for ESCF signed control
    /// @param sig_mode Signature algorithm (0=ECC1)
    /// @param kid Key identifier (EUI-64)
    constexpr void init(uint8_t const sig_mode, Eui64 const& kid) noexcept
    {
        subtype = AvtpSubtype::escf;
        r_version_sig = 0U;
        set_sig(sig_mode);
        rsv_control_data_length = 0U;
        key_id_ = kid;
    }

    // Validation

    /// Check if this is a valid ESCF header
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (subtype != AvtpSubtype::escf) {
            return false;
        }
        if (version() != 0) {
            return false;
        }
        return true;
    }

    auto operator<=>(EscfPdu const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(EscfPdu) == 12, "EscfPdu must be exactly 12 bytes");
static_assert(alignof(EscfPdu) <= 4, "EscfPdu alignment must not exceed 4 bytes");
static_assert(offsetof(EscfPdu, subtype) == 0, "subtype must be at offset 0");
static_assert(offsetof(EscfPdu, r_version_sig) == 1, "r_version_sig must be at offset 1");
static_assert(offsetof(EscfPdu, rsv_control_data_length) == 2, "rsv_control_data_length must be at offset 2");
static_assert(offsetof(EscfPdu, key_id_) == 4, "key_id must be at offset 4");

//
// Parse/create helpers
//

/// Parse an ESCF header from a packet
/// @param packet Raw packet data
[[nodiscard]] auto escf_parse_header(std::span<uint8_t const> packet) noexcept -> std::optional<EscfPdu>;

/// Get signed payload span from an ESCF packet
/// @param packet Raw packet data including the ESCF header
[[nodiscard]] auto escf_get_signed_payload(std::span<uint8_t const> packet) noexcept -> std::span<uint8_t const>;

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::EscfPdu> : std::true_type
{};

namespace statusbar::avtp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::avtp
