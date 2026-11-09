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
//      library's DL/ECIES (which speaks IEEE 1363a-2004's V, C, T triple) and
//      the ECC2 / ECC3 codecs (RFC 9180 HPKE over P-256 / X25519) of the IEEE
//      1722 security amendment proposal (avb/docs/IEEE_1722_CRYPTO_AMENDMENT.md).
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
#include "statusbar/crypto/hpke/hpke.hpp"
#include "statusbar/ieee/ieee_base.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

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

/// The Ethernet MTU the amendment's size rule is written against (17.4.4:
/// EncapMTU = MTU - 36 - Npk, the 36 being the EECF header, encryption_timestamp and tag).
inline constexpr size_t eecf_mtu = 1500;

/// Largest encrypted_payload that fits an EECF AVTPDU in one Ethernet frame: the MTU
/// less the 12-octet EECF header. (The encrypted_payload_length field itself is 16 bits.)
inline constexpr size_t eecf_max_encrypted_payload_size = eecf_mtu - eecf_header_size;  // 1488

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

/// EECF plaintext (IEEE 1722-2025 17.3.3, shared by every enc value): encryption_timestamp,
/// then the encapsulated AVTPDU. The AVTPDU span views the caller's plaintext buffer.
struct EecfPlaintext
{
    uint64_t encryption_timestamp{};
    std::span<uint8_t const> encapsulated_avtpdu;
};
using EecfEcc1Plaintext = EecfPlaintext;

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
    -> std::optional<EecfPlaintext>;

// ECC2 and ECC3 encrypted_payload (IEEE 1722 security amendment proposal, 17.4)
//
// Both are RFC 9180 HPKE in base mode with HKDF-SHA256 and AES-256-GCM, one HPKE
// context per PDU (sequence number 0); they differ only in the KEM (Table 53a):
//   ECC2  DHKEM(P-256, HKDF-SHA256) 0x0010, Npk 65 (SEC1 uncompressed 0x04 || x || y)
//   ECC3  DHKEM(X25519, HKDF-SHA256) 0x0020, Npk 32 (the RFC 7748 u-coordinate)
// The encrypted_payload is
//   enc  the sender's ephemeral public key, Npk octets
//   C    the AES-256-GCM ciphertext of the 17.3.3 plaintext, as long as the plaintext
//   T    the AES-256-GCM tag, 16 octets
// The HPKE info is the fixed string for the enc value; the HPKE aad is the frame's
// 12-octet EECF header (subtype through key_id, with the final
// encrypted_payload_length), so the header is authenticated with the payload. A
// receiver discards an encrypted_payload shorter than Npk + 24 (enc, the 8-octet
// encryption_timestamp and the tag; 17.4.3 step 2). The recipient's static key
// pair for the suite is the one key_id identifies (17.2.4).

/// EECF enc field value: ECC2 (from statusbar::avtp).
inline constexpr uint8_t eecf_enc_ecc2 = static_cast<uint8_t>(statusbar::avtp::EecfEncMode::ecc2);

/// EECF enc field value: ECC3 (from statusbar::avtp).
inline constexpr uint8_t eecf_enc_ecc3 = static_cast<uint8_t>(statusbar::avtp::EecfEncMode::ecc3);

/// ECC2 / ECC3 HPKE AEAD: AES-256-GCM (RFC 9180 AEAD id 0x0002).
inline constexpr HpkeAead eecf_hpke_aead = HpkeAead::aes256_gcm;
inline constexpr HpkeAead eecf_ecc2_aead = eecf_hpke_aead;
inline constexpr HpkeAead eecf_ecc3_aead = eecf_hpke_aead;

/// ECC2 / ECC3 HPKE info strings (Table 53a).
inline constexpr std::string_view eecf_ecc2_info = "IEEE 1722 EECF ECC2";
inline constexpr std::string_view eecf_ecc3_info = "IEEE 1722 EECF ECC3";

/// enc sizes (Npk).
inline constexpr size_t eecf_ecc2_enc_size = hpke_p256_public_key_size;    // 65
inline constexpr size_t eecf_ecc3_enc_size = hpke_x25519_public_key_size;  // 32

/// T (AES-256-GCM tag) size.
inline constexpr size_t eecf_ecc2_tag_size = hpke_tag_size;  // 16
inline constexpr size_t eecf_ecc3_tag_size = hpke_tag_size;  // 16

/// Octets besides C (enc ahead of it, T behind it).
inline constexpr size_t eecf_ecc2_overhead = eecf_ecc2_enc_size + eecf_ecc2_tag_size;  // 81
inline constexpr size_t eecf_ecc3_overhead = eecf_ecc3_enc_size + eecf_ecc3_tag_size;  // 48

/// The smallest encrypted_payload that can carry a 17.3.3 plaintext: Npk + 24 (17.4.3 step 2).
inline constexpr size_t eecf_ecc2_min_framed_size = eecf_ecc2_overhead + eecf_timestamp_size;  // 89
inline constexpr size_t eecf_ecc3_min_framed_size = eecf_ecc3_overhead + eecf_timestamp_size;  // 56

/// The largest encapsulated AVTPDU one Ethernet frame carries: EncapMTU = MTU - 36 - Npk (17.4.4).
inline constexpr size_t eecf_ecc2_max_encapsulated_avtpdu_size =
    eecf_max_encrypted_payload_size - eecf_timestamp_size - eecf_ecc2_overhead;  // 1399
inline constexpr size_t eecf_ecc3_max_encapsulated_avtpdu_size =
    eecf_max_encrypted_payload_size - eecf_timestamp_size - eecf_ecc3_overhead;  // 1432
static_assert(eecf_ecc2_max_encapsulated_avtpdu_size == eecf_mtu - 36 - eecf_ecc2_enc_size);
static_assert(eecf_ecc3_max_encapsulated_avtpdu_size == eecf_mtu - 36 - eecf_ecc3_enc_size);

/// Size of the ECC2 / ECC3 encrypted_payload for a plaintext of @p plaintext_size octets.
constexpr auto eecf_ecc2_encrypted_size(size_t plaintext_size) -> size_t
{
    return plaintext_size + eecf_ecc2_overhead;
}
constexpr auto eecf_ecc3_encrypted_size(size_t plaintext_size) -> size_t
{
    return plaintext_size + eecf_ecc3_overhead;
}

/// True when @p encrypted_payload_size can be an ECC2 / ECC3 encrypted_payload: enc and T
/// (C may be empty), and at most what one frame carries.
constexpr auto eecf_ecc2_payload_size_is_valid(size_t encrypted_payload_size) -> bool
{
    return encrypted_payload_size >= eecf_ecc2_overhead && encrypted_payload_size <= eecf_max_encrypted_payload_size;
}
constexpr auto eecf_ecc3_payload_size_is_valid(size_t encrypted_payload_size) -> bool
{
    return encrypted_payload_size >= eecf_ecc3_overhead && encrypted_payload_size <= eecf_max_encrypted_payload_size;
}

/// The EECF header an ECC2 / ECC3 encrypted_payload of @p encrypted_payload_size octets
/// under @p key_id is bound to as HPKE aad: exactly what build_eecf_frame writes for that enc.
auto eecf_ecc2_header(statusbar::ieee::Eui64 const& key_id, size_t encrypted_payload_size) -> std::array<uint8_t, eecf_header_size>;
auto eecf_ecc3_header(statusbar::ieee::Eui64 const& key_id, size_t encrypted_payload_size) -> std::array<uint8_t, eecf_header_size>;

/// Encrypt a plaintext into an ECC2 (P-256) or ECC3 (X25519) encrypted_payload enc || C || T.
///
/// @param recipient_pk Recipient's public key for the suite (the key_id's key).
/// @param aad The 12-octet EECF header the payload travels under (eecf_ecc2_header /
///        eecf_ecc3_header), or any octet string for the payload-level tests and tools.
/// @param plaintext The 17.3.3 plaintext (encryption_timestamp || encapsulated_avtpdu),
///        or any octet string.
/// @param out Output buffer, at least eecf_eccN_encrypted_size(plaintext.size()).
/// @param ikm_e 32 octets of fresh randomness for the ephemeral key (HPKE DeriveKeyPair).
/// @return The encrypted_payload within @p out, or nullopt when the buffer is too
///         small, the result would not fit an EECF frame, or HPKE fails.
auto eecf_ecc2_encrypt_payload(
    P256PublicKey const& recipient_pk,
    std::span<uint8_t const> aad,
    std::span<uint8_t const> plaintext,
    std::span<uint8_t> out,
    std::span<uint8_t const, hpke_min_ikm_size> ikm_e) -> std::optional<std::span<uint8_t const>>;
auto eecf_ecc3_encrypt_payload(
    X25519PublicKey const& recipient_pk,
    std::span<uint8_t const> aad,
    std::span<uint8_t const> plaintext,
    std::span<uint8_t> out,
    std::span<uint8_t const, hpke_min_ikm_size> ikm_e) -> std::optional<std::span<uint8_t const>>;

/// Decrypt an ECC2 / ECC3 encrypted_payload (enc || C || T) to its plaintext.
///
/// @param sk Recipient's private key for the suite.
/// @param aad The 12-octet EECF header as received (ParsedEecfFrame::header).
/// @param encrypted_payload The EECF encrypted_payload as parsed from the frame.
/// @param plaintext_out Output buffer, at least encrypted_payload.size() - eecf_eccN_overhead.
/// @return The plaintext within @p plaintext_out, or nullopt when the payload size is
///         not the suite's shape, the buffer is too small, enc is not a usable key, or
///         the tag does not verify (the header or payload was altered).
auto eecf_ecc2_decrypt_payload(
    P256PrivateKey const& sk,
    std::span<uint8_t const> aad,
    std::span<uint8_t const> encrypted_payload,
    std::span<uint8_t> plaintext_out) -> std::optional<std::span<uint8_t const>>;
auto eecf_ecc3_decrypt_payload(
    X25519PrivateKey const& sk,
    std::span<uint8_t const> aad,
    std::span<uint8_t const> encrypted_payload,
    std::span<uint8_t> plaintext_out) -> std::optional<std::span<uint8_t const>>;

/// Encrypt an encapsulated AVTPDU with its encryption_timestamp (17.3.3) into an
/// ECC2 / ECC3 encrypted_payload bound to the EECF header for @p key_id. @p out must
/// hold eecf_eccN_encrypted_size(eecf_timestamp_size + encapsulated_avtpdu.size()); the
/// AVTPDU may be at most eecf_eccN_max_encapsulated_avtpdu_size octets.
auto eecf_ecc2_encrypt(
    P256PublicKey const& recipient_pk,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t encryption_timestamp,
    std::span<uint8_t const> encapsulated_avtpdu,
    std::span<uint8_t> out,
    std::span<uint8_t const, hpke_min_ikm_size> ikm_e) -> std::optional<std::span<uint8_t const>>;
auto eecf_ecc3_encrypt(
    X25519PublicKey const& recipient_pk,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t encryption_timestamp,
    std::span<uint8_t const> encapsulated_avtpdu,
    std::span<uint8_t> out,
    std::span<uint8_t const, hpke_min_ikm_size> ikm_e) -> std::optional<std::span<uint8_t const>>;

/// Decrypt an ECC2 / ECC3 encrypted_payload received under @p eecf_header and split the
/// 17.3.3 plaintext into its encryption_timestamp and encapsulated AVTPDU. A payload
/// shorter than eecf_eccN_min_framed_size is discarded (17.4.3 step 2). @p plaintext_out
/// must hold encrypted_payload.size() octets; the returned AVTPDU span views it.
auto eecf_ecc2_decrypt(
    P256PrivateKey const& sk,
    std::span<uint8_t const, eecf_header_size> eecf_header,
    std::span<uint8_t const> encrypted_payload,
    std::span<uint8_t> plaintext_out) -> std::optional<EecfPlaintext>;
auto eecf_ecc3_decrypt(
    X25519PrivateKey const& sk,
    std::span<uint8_t const, eecf_header_size> eecf_header,
    std::span<uint8_t const> encrypted_payload,
    std::span<uint8_t> plaintext_out) -> std::optional<EecfPlaintext>;

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
/// The header and encrypted_payload members are zero-copy views into the original
/// frame buffer; for enc = ECC1 the payload is V || T || C (17.3.2), decrypted by
/// eecf_ecc1_decrypt; for enc = ECC2 / ECC3 it is enc || C || T, decrypted by
/// eecf_ecc2_decrypt / eecf_ecc3_decrypt with the header as its aad.
///
/// Wire layout (after Ethernet header):
///   Byte 0:     subtype (0xED)
///   Byte 1:     r(1)|version(3)|enc(4)
///   Bytes 2-3:  encrypted_payload_length (16 bits)
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
    std::span<uint8_t const> header;  ///< the 12-octet EECF header as received (the ECC2 / ECC3 HPKE aad)
    std::span<uint8_t const> encrypted_payload;
};

/// Build a complete Ethernet + EECF frame.
///
/// The encrypted_payload is already encrypted: for ECC1 it is the V || T || C
/// output of eecf_ecc1_encrypt(encryption_timestamp, encapsulated_avtpdu); for
/// ECC2 / ECC3 the enc || C || T output of eecf_ecc2_encrypt / eecf_ecc3_encrypt,
/// whose aad is the header this function writes (same enc, key_id and
/// encrypted_payload_length).
///
/// @param out Output buffer. Must be at least eecf_payload_offset + encrypted_payload.size().
/// @param dest_mac Destination MAC address for the Ethernet header.
/// @param src_mac Source MAC address for the Ethernet header.
/// @param enc Encryption algorithm identifier (the 4-bit enc field).
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
