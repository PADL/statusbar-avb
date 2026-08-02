#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AEF - AES Encrypted Format - IEEE 1722-2016 Clause 13
/// Continuous (subtype 0x6E) and Discrete (subtype 0xEE) encrypted PDU headers
///
/// Wire format (12-byte header + variable encrypted_payload):
///   Byte 0:     subtype (0x6E continuous, 0xEE discrete)
///   Byte 1:     r(1)|version(3)|enc(4)
///   Bytes 2-3:  stream_data_length(16) for continuous,
///               reserved(5)|control_data_length(11) for discrete
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
// AEF Encryption Mode Values - IEEE 1722-2016 Table 37
//
/// AEF enc field values
enum class AefEncMode : uint8_t
{
    aes_siv = 0x00U,      ///< AES-SIV (RFC 5297)
    aes_gcm_siv = 0x01U,  ///< AES-GCM-SIV (RFC 8452)
    // 0x02 - 0x0F: Reserved
};

/// Get human-readable name for AEF encryption mode
/// @param enc The AEF encryption mode value
[[nodiscard]] auto aef_enc_mode_name(uint8_t enc) noexcept -> std::string_view;

//
// AefContinuousPdu - IEEE 1722-2016 Clause 13, continuous stream variant
// Wire format: 12-byte header + variable encrypted_payload
//
/// AEF Continuous PDU Header - IEEE 1722-2016 Clause 13 (subtype 0x6E)
struct AefContinuousPdu
{
    /// Total length of AEF continuous header on wire
    static constexpr size_t HEADER_LENGTH = 12;

    // Byte 0: subtype[7:0] = 0x6E for AEF continuous
    octet_t subtype;

    // Byte 1: r[7] | version[6:4] | enc[3:0]
    octet_t r_version_enc;

    // Bytes 2-3: stream_data_length[15:0]
    doublet_t stream_data_length;

    // Bytes 4-11: key_id (EUI-64)
    Eui64 key_id_;

    // Accessors

    /// Get the version field (bits 6:4)
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return r_version_enc.get_bits(0x70U, 4); }

    /// Get the enc field (bits 3:0)
    [[nodiscard]] constexpr auto enc() const noexcept -> uint8_t { return r_version_enc.get_bits(0x0FU, 0); }

    /// Set the enc field (bits 3:0)
    /// @param value Encryption mode (4 bits)
    constexpr void set_enc(uint8_t const value) noexcept { r_version_enc.set_bits(0x0FU, 0, value); }

    /// Get the stream_data_length (payload length in octets)
    [[nodiscard]] constexpr auto get_stream_data_length() const noexcept -> uint16_t { return stream_data_length.get(); }

    /// Set the stream_data_length
    /// @param length Payload length in octets
    constexpr void set_stream_data_length(uint16_t const length) noexcept { stream_data_length = length; }

    /// Get the key_id
    [[nodiscard]] constexpr auto key_id() const noexcept -> Eui64 { return key_id_; }

    /// Set the key_id
    /// @param kid The key identifier (EUI-64)
    constexpr void set_key_id(Eui64 const& kid) noexcept { key_id_ = kid; }

    // Initialization

    /// Initialize for AEF continuous stream
    /// @param enc_mode Encryption mode (0=AES-SIV, 1=AES-GCM-SIV)
    /// @param kid Key identifier (EUI-64)
    constexpr void init(uint8_t const enc_mode, Eui64 const& kid) noexcept
    {
        subtype = AvtpSubtype::aef_continuous;
        r_version_enc = 0U;
        set_enc(enc_mode);
        stream_data_length = 0U;
        key_id_ = kid;
    }

    // Validation

    /// Check if this is a valid AEF continuous header
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (subtype != AvtpSubtype::aef_continuous) {
            return false;
        }
        if (version() != 0) {
            return false;
        }
        return true;
    }

    auto operator<=>(AefContinuousPdu const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AefContinuousPdu) == 12, "AefContinuousPdu must be exactly 12 bytes");
static_assert(alignof(AefContinuousPdu) <= 4, "AefContinuousPdu alignment must not exceed 4 bytes");
static_assert(offsetof(AefContinuousPdu, subtype) == 0, "subtype must be at offset 0");
static_assert(offsetof(AefContinuousPdu, r_version_enc) == 1, "r_version_enc must be at offset 1");
static_assert(offsetof(AefContinuousPdu, stream_data_length) == 2, "stream_data_length must be at offset 2");
static_assert(offsetof(AefContinuousPdu, key_id_) == 4, "key_id must be at offset 4");

//
// AefDiscretePdu - IEEE 1722-2016 Clause 13, discrete control variant
// Wire format: 12-byte header + variable encrypted_payload
//
/// AEF Discrete PDU Header - IEEE 1722-2016 Clause 13 (subtype 0xEE)
struct AefDiscretePdu
{
    /// Total length of AEF discrete header on wire
    static constexpr size_t HEADER_LENGTH = 12;

    // Byte 0: subtype[7:0] = 0xEE for AEF discrete
    octet_t subtype;

    // Byte 1: r[7] | version[6:4] | enc[3:0]
    octet_t r_version_enc;

    // Bytes 2-3: reserved[15:11] | control_data_length[10:0]
    doublet_t rsv_control_data_length;

    // Bytes 4-11: key_id (EUI-64)
    Eui64 key_id_;

    // Accessors

    /// Get the version field (bits 6:4)
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return r_version_enc.get_bits(0x70U, 4); }

    /// Get the enc field (bits 3:0)
    [[nodiscard]] constexpr auto enc() const noexcept -> uint8_t { return r_version_enc.get_bits(0x0FU, 0); }

    /// Set the enc field (bits 3:0)
    /// @param value Encryption mode (4 bits)
    constexpr void set_enc(uint8_t const value) noexcept { r_version_enc.set_bits(0x0FU, 0, value); }

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

    /// Initialize for AEF discrete control
    /// @param enc_mode Encryption mode (0=AES-SIV, 1=AES-GCM-SIV)
    /// @param kid Key identifier (EUI-64)
    constexpr void init(uint8_t const enc_mode, Eui64 const& kid) noexcept
    {
        subtype = AvtpSubtype::aef_discrete;
        r_version_enc = 0U;
        set_enc(enc_mode);
        rsv_control_data_length = 0U;
        key_id_ = kid;
    }

    // Validation

    /// Check if this is a valid AEF discrete header
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (subtype != AvtpSubtype::aef_discrete) {
            return false;
        }
        if (version() != 0) {
            return false;
        }
        return true;
    }

    auto operator<=>(AefDiscretePdu const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AefDiscretePdu) == 12, "AefDiscretePdu must be exactly 12 bytes");
static_assert(alignof(AefDiscretePdu) <= 4, "AefDiscretePdu alignment must not exceed 4 bytes");
static_assert(offsetof(AefDiscretePdu, subtype) == 0, "subtype must be at offset 0");
static_assert(offsetof(AefDiscretePdu, r_version_enc) == 1, "r_version_enc must be at offset 1");
static_assert(offsetof(AefDiscretePdu, rsv_control_data_length) == 2, "rsv_control_data_length must be at offset 2");
static_assert(offsetof(AefDiscretePdu, key_id_) == 4, "key_id must be at offset 4");

//
// Parse/create helpers
//

/// Parse an AEF continuous header from a packet
/// @param packet Raw packet data
[[nodiscard]] auto aef_continuous_parse_header(std::span<uint8_t const> packet) noexcept -> std::optional<AefContinuousPdu>;

/// Parse an AEF discrete header from a packet
/// @param packet Raw packet data
[[nodiscard]] auto aef_discrete_parse_header(std::span<uint8_t const> packet) noexcept -> std::optional<AefDiscretePdu>;

/// Get encrypted payload span from an AEF continuous packet
/// @param packet Raw packet data including the AEF header
[[nodiscard]] auto aef_continuous_get_encrypted_payload(std::span<uint8_t const> packet) noexcept -> std::span<uint8_t const>;

/// Get encrypted payload span from an AEF discrete packet
/// @param packet Raw packet data including the AEF header
[[nodiscard]] auto aef_discrete_get_encrypted_payload(std::span<uint8_t const> packet) noexcept -> std::span<uint8_t const>;

}  // namespace statusbar::avtp

// Serialization traits
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AefContinuousPdu> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AefDiscretePdu> : std::true_type
{};

namespace statusbar::avtp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::avtp
