#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ESCF - ECC Signed Control Format - IEEE 1722-2025 Clause 16
/// Discrete signed control PDU header
///
/// Wire format (84-byte header + variable encapsulated_avtpdu, Figure 109):
///   Byte 0:      subtype (0xEC)
///   Byte 1:      r(1)|version(3)|sig(4)
///   Bytes 2-3:   encapsulated_avtpdu_length (16 bits)
///   Bytes 4-11:  key_id (8 bytes, EUI-64)
///   Bytes 12-19: signing_timestamp (8 bytes, PTP nanoseconds)
///   Bytes 20-83: signature_value (64 bytes; ECSSA: c then d, Ed25519: R || S)
///   Bytes 84+:   encapsulated_avtpdu
///
/// The field is named signature_value, and the ED25519 sig value exists, per
/// the IEEE 1722 security amendment proposal (November 2026; see
/// avb/docs/IEEE_1722_CRYPTO_AMENDMENT.md): the published text calls the same
/// 64 octets ecdsa_signature_c and ecdsa_signature_d.

#include "statusbar/avtp/avtp_types.hpp"
#include "statusbar/ieee/ieee_base.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"

#include <array>
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
using ieee::octlet_t;

//
// ESCF Signature Algorithm Values - IEEE 1722-2025 Table 52, plus the amendment's row
//
/// ESCF sig field values
enum class EscfSigMode : uint8_t
{
    ecssa_dsa_emsa1_sha256 = 0x00U,  ///< ECSSA with ECSP-DSA / ECVP-DSA, EMSA1, SHA-256 over P-256 (16.3)
    ed25519 = 0x01U,                 ///< Ed25519, RFC 8032 Section 5.1, no prehash, no context (amendment 16.4)
    // 0x02 - 0x0F: Reserved
};

/// Get human-readable name for ESCF signature mode
/// @param sig The ESCF signature mode value
[[nodiscard]] auto escf_sig_mode_name(uint8_t sig) noexcept -> std::string_view;

//
// EscfPdu - IEEE 1722-2025 Clause 16
// Wire format: 84-byte header + variable encapsulated_avtpdu
//
/// ESCF Protocol Data Unit Header - IEEE 1722-2025 Clause 16 (subtype 0xEC)
struct EscfPdu
{
    /// Total length of ESCF header on wire: through signature_value
    static constexpr size_t HEADER_LENGTH = 84;

    /// Size of the signature_value field (both sig variants)
    static constexpr size_t SIGNATURE_LENGTH = 64;

    // Byte 0: subtype[7:0] = 0xEC for ESCF
    octet_t subtype;

    // Byte 1: r[7] | version[6:4] | sig[3:0]
    octet_t r_version_sig;

    // Bytes 2-3: encapsulated_avtpdu_length[15:0] (16.2.3, Figure 109)
    doublet_t encapsulated_avtpdu_length_;

    // Bytes 4-11: key_id (EUI-64)
    Eui64 key_id_;

    // Bytes 12-19: signing_timestamp (16.2.5)
    octlet_t signing_timestamp_;

    // Bytes 20-83: signature_value (16.2.7)
    std::array<octet_t, SIGNATURE_LENGTH> signature_value_;

    // Accessors

    /// Get the version field (bits 6:4)
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return r_version_sig.get_bits(0x70U, 4); }

    /// Get the sig field (bits 3:0)
    [[nodiscard]] constexpr auto sig() const noexcept -> uint8_t { return r_version_sig.get_bits(0x0FU, 0); }

    /// Set the sig field (bits 3:0)
    /// @param value Signature algorithm (4 bits)
    constexpr void set_sig(uint8_t const value) noexcept { r_version_sig.set_bits(0x0FU, 0, value); }

    /// Get the encapsulated_avtpdu_length (16-bit field)
    [[nodiscard]] constexpr auto encapsulated_avtpdu_length() const noexcept -> uint16_t
    {
        return encapsulated_avtpdu_length_.get();
    }

    /// Set the encapsulated_avtpdu_length (16-bit field)
    /// @param length Encapsulated AVTPDU length in octets
    constexpr void set_encapsulated_avtpdu_length(uint16_t const length) noexcept { encapsulated_avtpdu_length_ = length; }

    /// Get the key_id
    [[nodiscard]] constexpr auto key_id() const noexcept -> Eui64 { return key_id_; }

    /// Set the key_id
    /// @param kid The key identifier (EUI-64)
    constexpr void set_key_id(Eui64 const& kid) noexcept { key_id_ = kid; }

    /// Get the signing_timestamp (PTP nanoseconds)
    [[nodiscard]] constexpr auto signing_timestamp() const noexcept -> uint64_t { return signing_timestamp_.get(); }

    /// Set the signing_timestamp (PTP nanoseconds)
    constexpr void set_signing_timestamp(uint64_t const timestamp) noexcept { signing_timestamp_ = timestamp; }

    /// Get the signature_value
    [[nodiscard]] constexpr auto signature_value() const noexcept -> std::array<uint8_t, SIGNATURE_LENGTH>
    {
        std::array<uint8_t, SIGNATURE_LENGTH> out{};
        for (size_t i = 0; i < SIGNATURE_LENGTH; ++i) {
            out[i] = signature_value_[i].get();
        }
        return out;
    }

    /// Set the signature_value
    constexpr void set_signature_value(std::span<uint8_t const, SIGNATURE_LENGTH> const value) noexcept
    {
        for (size_t i = 0; i < SIGNATURE_LENGTH; ++i) {
            signature_value_[i] = value[i];
        }
    }

    // Initialization

    /// Initialize for ESCF signed control: version 0, no length, timestamp or signature yet
    /// @param sig_mode Signature algorithm (0=ECSSA_DSA_EMSA1_SHA256, 1=ED25519)
    /// @param kid Key identifier (EUI-64)
    constexpr void init(uint8_t const sig_mode, Eui64 const& kid) noexcept
    {
        *this = EscfPdu{};  // r, version, length, timestamp and signature all zero
        subtype = AvtpSubtype::escf;
        set_sig(sig_mode);
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
static_assert(sizeof(EscfPdu) == 84, "EscfPdu must be exactly 84 bytes");
static_assert(alignof(EscfPdu) <= 4, "EscfPdu alignment must not exceed 4 bytes");
static_assert(offsetof(EscfPdu, subtype) == 0, "subtype must be at offset 0");
static_assert(offsetof(EscfPdu, r_version_sig) == 1, "r_version_sig must be at offset 1");
static_assert(offsetof(EscfPdu, encapsulated_avtpdu_length_) == 2, "encapsulated_avtpdu_length must be at offset 2");
static_assert(offsetof(EscfPdu, key_id_) == 4, "key_id must be at offset 4");
static_assert(offsetof(EscfPdu, signing_timestamp_) == 12, "signing_timestamp must be at offset 12");
static_assert(offsetof(EscfPdu, signature_value_) == 20, "signature_value must be at offset 20");

//
// Parse/create helpers
//

/// Parse an ESCF header from a packet
/// @param packet Raw packet data
[[nodiscard]] auto escf_parse_header(std::span<uint8_t const> packet) noexcept -> std::optional<EscfPdu>;

/// Get the encapsulated_avtpdu span from an ESCF packet (everything after the header;
/// a consumer bounds it by encapsulated_avtpdu_length)
/// @param packet Raw packet data including the ESCF header
[[nodiscard]] auto escf_get_encapsulated_avtpdu(std::span<uint8_t const> packet) noexcept -> std::span<uint8_t const>;

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::EscfPdu> : std::true_type
{};

namespace statusbar::avtp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::avtp
