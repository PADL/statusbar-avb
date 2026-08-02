#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// EECF - ECC Encrypted Control Format - IEEE 1722-2016 Clause 17
/// Discrete encrypted control PDU header
///
/// Wire format (12-byte header + variable encrypted_payload):
///   Byte 0:     subtype (0xED)
///   Byte 1:     r(1)|version(3)|enc(4)
///   Bytes 2-3:  reserved(5)|encrypted_payload_length(11)
///   Bytes 4-11: key_id (8 bytes, EUI-64)
///   Bytes 12+:  encrypted_payload

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
// EECF Encryption Algorithm Values - IEEE 1722-2016 Table 48
//
/// EECF enc field values
enum class EecfEncMode : uint8_t
{
    ecc1 = 0x00U,  ///< ECC1 encryption algorithm (ECIES)
    // 0x01 - 0x0F: Reserved
};

/// Get human-readable name for EECF encryption mode
/// @param enc The EECF encryption mode value
[[nodiscard]] auto eecf_enc_mode_name(uint8_t enc) noexcept -> std::string_view;

//
// EecfPdu - IEEE 1722-2016 Clause 17
// Wire format: 12-byte header + variable encrypted_payload
//
/// EECF Protocol Data Unit Header - IEEE 1722-2016 Clause 17 (subtype 0xED)
struct EecfPdu
{
    /// Total length of EECF header on wire
    static constexpr size_t HEADER_LENGTH = 12;

    // Byte 0: subtype[7:0] = 0xED for EECF
    octet_t subtype;

    // Byte 1: r[7] | version[6:4] | enc[3:0]
    octet_t r_version_enc;

    // Bytes 2-3: reserved[15:11] | encrypted_payload_length[10:0]
    doublet_t rsv_encrypted_payload_length;

    // Bytes 4-11: key_id (EUI-64)
    Eui64 key_id_;

    // Accessors

    /// Get the version field (bits 6:4)
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return r_version_enc.get_bits(0x70U, 4); }

    /// Get the enc field (bits 3:0)
    [[nodiscard]] constexpr auto enc() const noexcept -> uint8_t { return r_version_enc.get_bits(0x0FU, 0); }

    /// Set the enc field (bits 3:0)
    /// @param value Encryption algorithm (4 bits)
    constexpr void set_enc(uint8_t const value) noexcept { r_version_enc.set_bits(0x0FU, 0, value); }

    /// Get the encrypted_payload_length (11-bit field)
    [[nodiscard]] constexpr auto encrypted_payload_length() const noexcept -> uint16_t
    {
        return static_cast<uint16_t>(rsv_encrypted_payload_length.get() & 0x07FFU);
    }

    /// Set the encrypted_payload_length (11-bit field)
    /// @param length Encrypted payload length in octets (max 2047)
    constexpr void set_encrypted_payload_length(uint16_t const length) noexcept
    {
        uint16_t const current = rsv_encrypted_payload_length.get();
        rsv_encrypted_payload_length = static_cast<uint16_t>((current & 0xF800U) | (length & 0x07FFU));
    }

    /// Get the key_id
    [[nodiscard]] constexpr auto key_id() const noexcept -> Eui64 { return key_id_; }

    /// Set the key_id
    /// @param kid The key identifier (EUI-64)
    constexpr void set_key_id(Eui64 const& kid) noexcept { key_id_ = kid; }

    // Initialization

    /// Initialize for EECF encrypted control
    /// @param enc_mode Encryption algorithm (0=ECC1)
    /// @param kid Key identifier (EUI-64)
    constexpr void init(uint8_t const enc_mode, Eui64 const& kid) noexcept
    {
        subtype = AvtpSubtype::eecf;
        r_version_enc = 0U;
        set_enc(enc_mode);
        rsv_encrypted_payload_length = 0U;
        key_id_ = kid;
    }

    // Validation

    /// Check if this is a valid EECF header
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (subtype != AvtpSubtype::eecf) {
            return false;
        }
        if (version() != 0) {
            return false;
        }
        return true;
    }

    auto operator<=>(EecfPdu const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(EecfPdu) == 12, "EecfPdu must be exactly 12 bytes");
static_assert(alignof(EecfPdu) <= 4, "EecfPdu alignment must not exceed 4 bytes");
static_assert(offsetof(EecfPdu, subtype) == 0, "subtype must be at offset 0");
static_assert(offsetof(EecfPdu, r_version_enc) == 1, "r_version_enc must be at offset 1");
static_assert(offsetof(EecfPdu, rsv_encrypted_payload_length) == 2, "rsv_encrypted_payload_length must be at offset 2");
static_assert(offsetof(EecfPdu, key_id_) == 4, "key_id must be at offset 4");

//
// Parse/create helpers
//

/// Parse an EECF header from a packet
/// @param packet Raw packet data
[[nodiscard]] auto eecf_parse_header(std::span<uint8_t const> packet) noexcept -> std::optional<EecfPdu>;

/// Get encrypted payload span from an EECF packet
/// @param packet Raw packet data including the EECF header
[[nodiscard]] auto eecf_get_encrypted_payload(std::span<uint8_t const> packet) noexcept -> std::span<uint8_t const>;

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::EecfPdu> : std::true_type
{};

namespace statusbar::avtp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::avtp
