// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// IEEE 1722.1 AECP AEM frame builder/parser for AVB key exchange,
// plus IEEE 1722-2025 Clause 17 EECF (ECC Encrypted Control Format).
//
// Builds and parses complete Ethernet frames for:
//   1. AUTH_GET_NONCE command/response (IEEE 1722.1-2021 §7.4.103)
//   2. AUTH_ADD_KEY_NONCE command/response (IEEE 1722.1-2021 §7.4.104)
//   3. EECF encrypted control wrapping (IEEE 1722-2025 Clause 17), including
//      the ECC1 encrypted_payload codec (17.3.2: V, T, C) over the crypto
//      library's DL/ECIES (which speaks IEEE 1363a-2004's V, C, T triple).
//
// Wire layout: 14-byte Ethernet header + 24-byte AemDu + variable payload.
// EECF layout: 14-byte Ethernet header + 12-byte EECF header + encrypted payload.
// Payload-level serialization is in avtp_crypto_pdu.hpp.

#pragma once

#include "statusbar/avtp/avtp_eecf.hpp"
#include "statusbar/avtp/avtp_types.hpp"
#include "statusbar/avtp_crypto/avtp_crypto.hpp"
#include "statusbar/avtp_crypto/avtp_crypto_pdu.hpp"
#include "statusbar/avtp_crypto/avtp_keychain.hpp"
#include "statusbar/crypto/ecies/ecies.hpp"
#include "statusbar/ieee/ieee_base.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace statusbar::crypto::avtp {

// Wire format constants (IEEE 1722-2016 / IEEE 1722.1-2021)

/// Ethernet header size (dest_mac + src_mac + ethertype).
inline constexpr size_t ethernet_header_size = 14;

/// AEM Data Unit size (AECP common + AEM-specific fields).
inline constexpr size_t aem_du_size = 24;

/// Offset of AEM payload within a complete Ethernet frame.
inline constexpr size_t aem_payload_offset = ethernet_header_size + aem_du_size;  // 38

/// IEEE 1722 AVTP EtherType (from statusbar::avtp).
inline constexpr uint16_t avtp_ethertype = statusbar::avtp::AVTP_ETHERTYPE;

/// AECP subtype (from statusbar::avtp).
inline constexpr uint8_t aecp_subtype = statusbar::avtp::AvtpSubtype::aecp;

/// AECP message type: AEM command.
inline constexpr uint8_t aecp_message_type_aem_command = 0;

/// AECP message type: AEM response.
inline constexpr uint8_t aecp_message_type_aem_response = 1;

/// AEM status: success.
inline constexpr uint8_t aem_status_success = 0;

/// AEM common data length: controller_entity_id(8) + sequence_id(2) + command_type(2).
inline constexpr size_t aem_data_length = 12;

/// AEM command code: AUTH_GET_NONCE (IEEE 1722.1-2021 §7.4.103).
inline constexpr uint16_t aem_cmd_auth_get_nonce = 0x0067;

/// AEM command code: AUTH_ADD_KEY_NONCE (IEEE 1722.1-2021 §7.4.104).
inline constexpr uint16_t aem_cmd_auth_add_key_nonce = 0x0068;

/// EECF subtype (from statusbar::avtp).
inline constexpr uint8_t eecf_subtype = statusbar::avtp::AvtpSubtype::eecf;

/// EECF header size (from statusbar::avtp::EecfPdu).
inline constexpr size_t eecf_header_size = statusbar::avtp::EecfPdu::HEADER_LENGTH;

/// Offset of EECF encrypted_payload within a complete Ethernet frame.
inline constexpr size_t eecf_payload_offset = ethernet_header_size + eecf_header_size;  // 26

/// EECF enc field value: ECC1 (from statusbar::avtp).
inline constexpr uint8_t eecf_enc_ecc1 = static_cast<uint8_t>(statusbar::avtp::EecfEncMode::ecc1);

/// ECC1 plaintext timestamp size (8-byte gPTP nanoseconds, IEEE 1722-2025 17.3.3).
inline constexpr size_t eecf_timestamp_size = 8;

/// Largest encrypted_payload an EECF header can declare (11-bit encrypted_payload_length).
inline constexpr size_t eecf_max_encrypted_payload_size = 0x07FF;  // 2047

// ECC1 encrypted_payload (IEEE 1722-2025 17.3.2)
//
// For enc = ECC1 the encrypted_payload carries the three outputs of the IEEE
// 1363a-2004 11.3 encryption operation in the order IEEE 1722-2025 fixes:
//   V  the sender's ephemeral public key, EC2OSP-X (0x01 || x), 33 octets
//   T  the HMAC-SHA-256 tag, 32 octets
//   C  the AES-256-CBC-IV0 ciphertext, a multiple of 16 octets
// The crypto library's ecies_encrypt/ecies_decrypt hand over the 1363a triple
// as V || C || T. The eecf_ecc1_* functions below are the only place the two
// orders meet; nothing else in the stack may assume one or the other.

/// ECC1 encrypted_payload: V (EC2OSP-X) size.
inline constexpr size_t eecf_ecc1_v_size = ecies_ephemeral_key_size;  // 33

/// ECC1 encrypted_payload: T (HMAC-SHA-256) size.
inline constexpr size_t eecf_ecc1_tag_size = ecies_mac_tag_size;  // 32

/// ECC1 encrypted_payload: octets ahead of C (V then T).
inline constexpr size_t eecf_ecc1_overhead = eecf_ecc1_v_size + eecf_ecc1_tag_size;  // 65

/// Size of the ECC1 encrypted_payload for a plaintext of @p plaintext_size octets
/// (V + T + the PKCS#7-padded ciphertext).
constexpr auto eecf_ecc1_encrypted_size(size_t plaintext_size) -> size_t
{
    return ecies_output_size(plaintext_size);
}

/// True when @p encrypted_payload_size can be an ECC1 encrypted_payload:
/// V and T, then at least one 16-octet cipher block, and at most what the
/// EECF header can declare.
constexpr auto eecf_ecc1_payload_size_is_valid(size_t encrypted_payload_size) -> bool
{
    return encrypted_payload_size >= eecf_ecc1_overhead + 16 && encrypted_payload_size <= eecf_max_encrypted_payload_size &&
        ((encrypted_payload_size - eecf_ecc1_overhead) % 16) == 0;
}

/// ECC1 plaintext (IEEE 1722-2025 17.3.3): encryption_timestamp, then the
/// encapsulated AVTPDU. The AVTPDU span views the caller's plaintext buffer.
struct EecfEcc1Plaintext
{
    uint64_t encryption_timestamp{};
    std::span<uint8_t const> encapsulated_avtpdu;
};

/// Encrypt an ECC1 plaintext into an EECF encrypted_payload laid out as V || T || C.
///
/// @param recipient_pk Recipient's P-256 public key (the key_id's key).
/// @param plaintext The 17.3.3 plaintext (encryption_timestamp || encapsulated_avtpdu),
///        or any octet string for the payload-level tests and tools.
/// @param out Output buffer, at least eecf_ecc1_encrypted_size(plaintext.size()).
/// @param entropy 32 octets of fresh randomness for the ephemeral key.
/// @return The encrypted_payload within @p out, or nullopt when the buffer is too
///         small, the result would not fit an EECF frame, or ECIES fails.
auto eecf_ecc1_encrypt_payload(
    P256PublicKey const& recipient_pk,
    std::span<uint8_t const> plaintext,
    std::span<uint8_t> out,
    std::span<uint8_t const, p256_scalar_size> entropy) -> std::optional<std::span<uint8_t const>>;

/// Decrypt an EECF ECC1 encrypted_payload (V || T || C) to its plaintext.
///
/// @param sk Recipient's P-256 private key.
/// @param encrypted_payload The EECF encrypted_payload as parsed from the frame.
/// @param plaintext_out Output buffer, at least encrypted_payload.size().
/// @return The plaintext within @p plaintext_out, or nullopt when the payload size
///         is not an ECC1 shape, the buffer is too small, or the tag does not verify.
auto eecf_ecc1_decrypt_payload(
    P256PrivateKey const& sk, std::span<uint8_t const> encrypted_payload, std::span<uint8_t> plaintext_out)
    -> std::optional<std::span<uint8_t const>>;

/// Encrypt an encapsulated AVTPDU with its encryption_timestamp (17.3.3) into an
/// ECC1 encrypted_payload (V || T || C). @p out must hold
/// eecf_ecc1_encrypted_size(eecf_timestamp_size + encapsulated_avtpdu.size()).
auto eecf_ecc1_encrypt(
    P256PublicKey const& recipient_pk,
    uint64_t encryption_timestamp,
    std::span<uint8_t const> encapsulated_avtpdu,
    std::span<uint8_t> out,
    std::span<uint8_t const, p256_scalar_size> entropy) -> std::optional<std::span<uint8_t const>>;

/// Decrypt an ECC1 encrypted_payload and split the 17.3.3 plaintext into its
/// encryption_timestamp and encapsulated AVTPDU. @p plaintext_out must hold
/// encrypted_payload.size() octets; the returned AVTPDU span views it.
auto eecf_ecc1_decrypt(P256PrivateKey const& sk, std::span<uint8_t const> encrypted_payload, std::span<uint8_t> plaintext_out)
    -> std::optional<EecfEcc1Plaintext>;

// Parsed frame structure

/// Parsed fields from a complete Ethernet + AemDu frame.
/// The payload member is a zero-copy view into the original frame buffer.
struct ParsedAemFrame
{
    statusbar::ieee::Eui48 dest_mac{};
    statusbar::ieee::Eui48 src_mac{};
    statusbar::ieee::doublet_t ethertype{};
    uint8_t subtype{};
    uint8_t message_type{};
    uint8_t status{};
    uint16_t control_data_length{};
    statusbar::ieee::Eui64 target_entity_id{};
    statusbar::ieee::Eui64 controller_entity_id{};
    uint16_t sequence_id{};
    uint16_t command_code{};
    std::span<uint8_t const> payload;
};

// Generic AEM frame builder

/// Build a complete Ethernet + AemDu frame into the caller-provided output buffer.
///
/// Wire layout:
///   Bytes 0-5:   dest_mac
///   Bytes 6-11:  src_mac
///   Bytes 12-13: ethertype (0x22F0, big-endian)
///   Byte 14:     subtype (0xFB)
///   Byte 15:     sv(0)|version(0)|message_type[3:0]
///   Byte 16:     status[7:3]|control_data_length[10:8]
///   Byte 17:     control_data_length[7:0]
///   Bytes 18-25: target_entity_id
///   Bytes 26-33: controller_entity_id
///   Bytes 34-35: sequence_id (big-endian)
///   Bytes 36-37: u(0)|command_type[14:0] (big-endian)
///   Bytes 38+:   payload
///
/// @param out Output buffer. Must be at least aem_payload_offset + payload.size() bytes.
/// @param dest_mac Destination MAC address for the Ethernet header.
/// @param src_mac Source MAC address for the Ethernet header.
/// @param message_type AVTP message type field (bits [3:0] of byte 15).
/// @param status Status field (bits [7:3] of byte 16).
/// @param target_entity_id Entity ID of the target device.
/// @param controller_entity_id Entity ID of the controlling device.
/// @param sequence_id Sequence number for matching commands to responses.
/// @param command_code AEM command type (u-bit cleared, bits [14:0]).
/// @param payload AEM command or response payload bytes appended after the header.
/// @return Span of the written frame, or nullopt if output buffer is too small.
auto build_aem_frame(
    std::span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    uint8_t message_type,
    uint8_t status,
    statusbar::ieee::Eui64 const& target_entity_id,
    statusbar::ieee::Eui64 const& controller_entity_id,
    uint16_t sequence_id,
    uint16_t command_code,
    std::span<uint8_t const> payload) -> std::optional<std::span<uint8_t const>>;

// AUTH_GET_NONCE frame builders (IEEE 1722.1-2021 §7.4.103)

/// Build AUTH_GET_NONCE command frame (46 bytes: 38 header + 8 payload).
auto build_auth_get_nonce_frame(
    std::span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    statusbar::ieee::Eui64 const& target_entity_id,
    statusbar::ieee::Eui64 const& controller_entity_id,
    uint16_t sequence_id,
    Nonce const& controller_nonce) -> std::optional<std::span<uint8_t const>>;

/// Build AUTH_GET_NONCE response frame (54 bytes: 38 header + 16 payload).
auto build_auth_get_nonce_response_frame(
    std::span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    statusbar::ieee::Eui64 const& target_entity_id,
    statusbar::ieee::Eui64 const& controller_entity_id,
    uint16_t sequence_id,
    uint8_t status,
    Nonce const& controller_nonce,
    Nonce const& target_nonce) -> std::optional<std::span<uint8_t const>>;

// AUTH_ADD_KEY_NONCE frame builders (IEEE 1722.1-2021 §7.4.104)

/// Build AUTH_ADD_KEY_NONCE command frame (38 + ciphertext_len bytes).
auto build_auth_add_key_nonce_frame(
    std::span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    statusbar::ieee::Eui64 const& target_entity_id,
    statusbar::ieee::Eui64 const& controller_entity_id,
    uint16_t sequence_id,
    std::span<uint8_t const> ecies_ciphertext) -> std::optional<std::span<uint8_t const>>;

/// Build AUTH_ADD_KEY_NONCE response frame (62 bytes: 38 header + 24 payload).
auto build_auth_add_key_nonce_response_frame(
    std::span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    statusbar::ieee::Eui64 const& target_entity_id,
    statusbar::ieee::Eui64 const& controller_entity_id,
    uint16_t sequence_id,
    uint8_t status,
    Nonce const& controller_nonce,
    Nonce const& target_nonce,
    KeyId const& key_id) -> std::optional<std::span<uint8_t const>>;

// Frame parser

/// Parse a raw Ethernet frame into structured AEM fields.
/// Returns nullopt if the frame is too short, has wrong ethertype, or wrong subtype.
auto parse_aem_frame(std::span<uint8_t const> frame) -> std::optional<ParsedAemFrame>;

// Payload extraction helpers

/// Extract controller_nonce from AUTH_GET_NONCE command payload.
/// Returns nullopt if payload is too short (< 8 bytes).
auto extract_auth_get_nonce(ParsedAemFrame const& frame) -> std::optional<Nonce>;

/// Extract (controller_nonce, target_nonce) from AUTH_GET_NONCE response payload.
/// Returns nullopt if payload is too short (< 16 bytes).
auto extract_auth_get_nonce_response(ParsedAemFrame const& frame) -> std::optional<AuthGetNonceResponsePayload>;

/// Extract ECIES ciphertext from AUTH_ADD_KEY_NONCE command payload.
/// Returns the entire payload as a span (variable length).
auto extract_auth_add_key_nonce_ciphertext(ParsedAemFrame const& frame) -> std::span<uint8_t const>;

/// Extract (controller_nonce, target_nonce, key_id) from AUTH_ADD_KEY_NONCE response payload.
/// Returns nullopt if payload is too short (< 24 bytes).
auto extract_auth_add_key_nonce_response(ParsedAemFrame const& frame) -> std::optional<AuthAddKeyNonceResponsePayload>;

// EECF frame (IEEE 1722-2025 Clause 17)

/// Parsed fields from a complete Ethernet + EECF frame.
/// The encrypted_payload member is a zero-copy view into the original frame buffer;
/// for enc = ECC1 it is V || T || C (17.3.2), decrypted by eecf_ecc1_decrypt.
///
/// Wire layout (after Ethernet header):
///   Byte 0:     subtype (0xED)
///   Byte 1:     r(1)|version(3)|enc(4)
///   Bytes 2-3:  reserved(5)|encrypted_payload_length(11)
///   Bytes 4-11: key_id (8 bytes, EUI-64)
///   Bytes 12+:  encrypted_payload
struct ParsedEecfFrame
{
    statusbar::ieee::Eui48 dest_mac{};
    statusbar::ieee::Eui48 src_mac{};
    statusbar::ieee::doublet_t ethertype{};
    uint8_t subtype{};
    uint8_t enc{};
    uint16_t encrypted_payload_length{};
    statusbar::ieee::Eui64 key_id{};
    std::span<uint8_t const> encrypted_payload;
};

/// Build a complete Ethernet + EECF frame.
///
/// The encrypted_payload is already encrypted: for ECC1 it is the V || T || C
/// output of eecf_ecc1_encrypt(encryption_timestamp, encapsulated_avtpdu).
///
/// @param out Output buffer. Must be at least eecf_payload_offset + encrypted_payload.size().
/// @param dest_mac Destination MAC address for the Ethernet header.
/// @param src_mac Source MAC address for the Ethernet header.
/// @param enc Encryption algorithm identifier (bits [1:0] of the enc field).
/// @param key_id Entity ID of the key used to encrypt the payload.
/// @param encrypted_payload The encrypted_payload (ECC1: V || T || C) appended after the header.
/// @return Span of the written frame, or nullopt if output buffer is too small.
auto build_eecf_frame(
    std::span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    uint8_t enc,
    statusbar::ieee::Eui64 const& key_id,
    std::span<uint8_t const> encrypted_payload) -> std::optional<std::span<uint8_t const>>;

/// Parse a raw Ethernet frame as an EECF frame.
/// Returns nullopt if the frame is too short, has wrong ethertype, or wrong subtype.
auto parse_eecf_frame(std::span<uint8_t const> frame) -> std::optional<ParsedEecfFrame>;

}  // namespace statusbar::crypto::avtp
