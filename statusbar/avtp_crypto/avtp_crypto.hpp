// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AVTP cryptographic operations for key exchange and key distribution.
//
// Provides the crypto-layer types and operations for:
//   1. Signed ephemeral key exchange (Ed25519+X25519 or P-256 ECDSA+ECDH)
//   2. Transport key derivation (HKDF-SHA-256)
//   3. Nonce-bound key wrapping (AES-256-SIV)
//   4. Signed public key entry construction (chain-of-trust)
//
// Wire-format serialization is in avtp_crypto_pdu.hpp.

#pragma once

#include "statusbar/avtp_crypto/avtp_keychain.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/crypto/25519/ed25519.hpp"
#include "statusbar/crypto/aes/aes_common_internal.hpp"
#include "statusbar/crypto/keys.hpp"
#include "statusbar/crypto/p256/p256_ecdsa.hpp"
#include "statusbar/crypto/util/crypto_concepts.hpp"
#include "statusbar/crypto/util/crypto_util_internal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace statusbar::crypto::avtp {

// Key exchange message type constants

/// Key exchange message type values (carried in the first byte of key exchange PDUs).
inline constexpr uint8_t key_exchange_offer = 0x01;
inline constexpr uint8_t key_exchange_accept = 0x02;

// 64-bit nonce (IEEE 1722.1-2021 §7.4.103)

inline constexpr size_t nonce_size = 8;

/// 64-bit nonce used for AUTH_GET_NONCE challenge-response replay protection.
struct Nonce
{
    std::array<uint8_t, nonce_size> data{};
};

// Ed25519 + X25519 key exchange PDU

/// Ed25519 signed key exchange PDU (used for both offer and accept).
///
/// Wire layout (129 bytes):
///   [0]       message_type      (1 byte)
///   [1..32]   sender_identity   (32 bytes, Ed25519 public key)
///   [33..64]  ephemeral_pubkey  (32 bytes, X25519 public key)
///   [65..128] signature         (64 bytes, Ed25519 signature)
struct Ed25519KeyExchangePdu
{
    uint8_t message_type{};
    Ed25519PublicKey sender_identity{};
    X25519PublicKey ephemeral_pubkey{};
    Ed25519Signature signature{};
};

inline constexpr size_t ed25519_key_exchange_pdu_wire_size =
    1 + Ed25519PublicKey::LENGTH + X25519PublicKey::LENGTH + Ed25519Signature::LENGTH;  // 129

// P-256 ECDSA + ECDH key exchange PDU

/// P-256 signed key exchange PDU (used for both offer and accept).
///
/// Wire layout (193 bytes):
///   [0]        message_type      (1 byte)
///   [1..64]    sender_identity   (64 bytes, P-256 public key x||y)
///   [65..128]  ephemeral_pubkey  (64 bytes, P-256 ephemeral public key x||y)
///   [129..192] signature         (64 bytes, P-256 ECDSA signature r||s)
struct P256KeyExchangePdu
{
    uint8_t message_type{};
    P256PublicKey sender_identity{};
    P256PublicKey ephemeral_pubkey{};
    P256EcdsaSignature signature{};
};

inline constexpr size_t p256_key_exchange_pdu_wire_size =
    1 + P256PublicKey::LENGTH + P256PublicKey::LENGTH + P256EcdsaSignature::LENGTH;  // 193

// Per-key-type wrapped key structures

/// AES-128 key wrapped with AES-256-SIV (16 + 16 = 32 bytes).
struct Aes128WrappedKey
{
    std::array<uint8_t, Aes128Key::LENGTH> ciphertext{};
    std::array<uint8_t, aes_block_size> siv_tag{};
};

inline constexpr size_t aes128_wrapped_key_size = Aes128Key::LENGTH + aes_block_size;  // 32

/// AES-256 key wrapped with AES-256-SIV (32 + 16 = 48 bytes).
struct Aes256WrappedKey
{
    std::array<uint8_t, Aes256Key::LENGTH> ciphertext{};
    std::array<uint8_t, aes_block_size> siv_tag{};
};

inline constexpr size_t aes256_wrapped_key_size = Aes256Key::LENGTH + aes_block_size;  // 48

/// AES-128-SIV key wrapped with AES-256-SIV (32 + 16 = 48 bytes).
struct Aes128SivWrappedKey
{
    std::array<uint8_t, Aes128SivKey::LENGTH> ciphertext{};
    std::array<uint8_t, aes_block_size> siv_tag{};
};

inline constexpr size_t aes128_siv_wrapped_key_size = Aes128SivKey::LENGTH + aes_block_size;  // 48

/// AES-256-SIV key wrapped with AES-256-SIV (64 + 16 = 80 bytes).
struct Aes256SivWrappedKey
{
    std::array<uint8_t, Aes256SivKey::LENGTH> ciphertext{};
    std::array<uint8_t, aes_block_size> siv_tag{};
};

inline constexpr size_t aes256_siv_wrapped_key_size = Aes256SivKey::LENGTH + aes_block_size;  // 80

// Ed25519 key exchange: build and verify

/// Build a signed Ed25519 key exchange PDU (offer or accept).
///
/// The identity_key can be any type satisfying Ed25519SigningKey -- software
/// Ed25519PrivateKey or a secure-enclave key with ADL-visible overloads.
/// Signature covers: message_type (1) || sender_identity (32) || ephemeral_pubkey (32) = 65 bytes.
/// @param type The message type (key_exchange_offer or key_exchange_accept).
/// @param identity_key The Ed25519 signing key (software or enclave).
/// @param ephemeral_pk The X25519 ephemeral public key to include.
/// @return The signed Ed25519KeyExchangePdu.
template <Ed25519SigningKey K>
auto build_ed25519_key_exchange(uint8_t type, K const& identity_key, X25519PublicKey const& ephemeral_pk) -> Ed25519KeyExchangePdu
{
    Ed25519KeyExchangePdu pdu;
    pdu.message_type = type;
    pdu.sender_identity = ed25519_public_key(identity_key);
    pdu.ephemeral_pubkey = ephemeral_pk;

    // Construct the data to sign: message_type || sender_identity || ephemeral_pubkey
    std::array<uint8_t, 1 + Ed25519PublicKey::LENGTH + X25519PublicKey::LENGTH> signed_data{};
    signed_data[0] = type;
    internal::span_copy(
        std::span(signed_data).subspan(1, Ed25519PublicKey::LENGTH), std::span<uint8_t const>(pdu.sender_identity.data));
    internal::span_copy(
        std::span(signed_data).subspan(1 + Ed25519PublicKey::LENGTH, X25519PublicKey::LENGTH),
        std::span<uint8_t const>(ephemeral_pk.data));

    pdu.signature = ed25519_sign(identity_key, signed_data);
    return pdu;
}

/// Verify an Ed25519 key exchange PDU signature and extract the ephemeral public key.
///
/// Returns the peer's ephemeral X25519 public key on success, or nullopt if
/// the sender identity doesn't match or the signature is invalid.
/// @param pdu The received key exchange PDU.
/// @param expected_identity The expected sender's Ed25519 public key.
/// @return The peer's ephemeral X25519 public key, or nullopt on failure.
auto verify_ed25519_key_exchange(Ed25519KeyExchangePdu const& pdu, Ed25519PublicKey const& expected_identity)
    -> std::optional<X25519PublicKey>;

// P-256 key exchange: build and verify

/// Build a signed P-256 key exchange PDU (offer or accept).
///
/// The identity_key can be any type satisfying P256SigningKey -- software
/// P256PrivateKey or a secure-enclave key with ADL-visible overloads.
/// Signature covers: message_type (1) || sender_identity (64) || ephemeral_pubkey (64) = 129 bytes.
/// @param type The message type (key_exchange_offer or key_exchange_accept).
/// @param identity_key The P-256 signing key (software or enclave).
/// @param ephemeral_pk The P-256 ephemeral public key to include.
/// @return The signed P256KeyExchangePdu.
template <P256SigningKey K>
auto build_p256_key_exchange(uint8_t type, K const& identity_key, P256PublicKey const& ephemeral_pk) -> P256KeyExchangePdu
{
    P256KeyExchangePdu pdu;
    pdu.message_type = type;
    pdu.sender_identity = p256_public_key(identity_key);
    pdu.ephemeral_pubkey = ephemeral_pk;

    // Construct the data to sign: message_type || sender_identity || ephemeral_pubkey
    std::array<uint8_t, 1 + P256PublicKey::LENGTH + P256PublicKey::LENGTH> signed_data{};
    signed_data[0] = type;
    internal::span_copy(
        std::span(signed_data).subspan(1, P256PublicKey::LENGTH), std::span<uint8_t const>(pdu.sender_identity.data));
    internal::span_copy(
        std::span(signed_data).subspan(1 + P256PublicKey::LENGTH, P256PublicKey::LENGTH),
        std::span<uint8_t const>(ephemeral_pk.data));

    pdu.signature = p256_ecdsa_sign(identity_key, signed_data);
    return pdu;
}

/// Verify a P-256 key exchange PDU signature and extract the ephemeral public key.
///
/// Returns the peer's ephemeral P-256 public key on success, or nullopt if
/// the sender identity doesn't match or the signature is invalid.
/// @param pdu The received key exchange PDU.
/// @param expected_identity The expected sender's P-256 public key.
/// @return The peer's ephemeral P-256 public key, or nullopt on failure.
auto verify_p256_key_exchange(P256KeyExchangePdu const& pdu, P256PublicKey const& expected_identity)
    -> std::optional<P256PublicKey>;

// Ed25519 transport key derivation

/// Build HKDF info parameter for Ed25519 AVTP transport key derivation.
auto build_ed25519_transport_key_info(Ed25519PublicKey const& initiator_identity, Ed25519PublicKey const& responder_identity)
    -> std::array<uint8_t, 15 + Ed25519PublicKey::LENGTH + Ed25519PublicKey::LENGTH>;

/// Derive a 64-byte AES-256-SIV transport key from the X25519 shared secret.
auto derive_ed25519_transport_key(
    std::span<uint8_t const, X25519PublicKey::LENGTH> shared_secret,
    Ed25519PublicKey const& initiator_identity,
    Ed25519PublicKey const& responder_identity) -> Aes256SivKey;

// P-256 transport key derivation

/// Build HKDF info parameter for P-256 AVTP transport key derivation.
auto build_p256_transport_key_info(P256PublicKey const& initiator_identity, P256PublicKey const& responder_identity)
    -> std::array<uint8_t, 19 + P256PublicKey::LENGTH + P256PublicKey::LENGTH>;

/// Derive a 64-byte AES-256-SIV transport key from the P-256 ECDH shared secret.
auto derive_p256_transport_key(
    std::span<uint8_t const, P256PrivateKey::LENGTH> shared_secret,
    P256PublicKey const& initiator_identity,
    P256PublicKey const& responder_identity) -> Aes256SivKey;

// Per-key-type key wrapping / unwrapping
//
// Each wrap function encrypts a session key using AES-256-SIV with the transport key.
// AAD = controller_nonce(8) || target_nonce(8) || key_id(8) || key_type(1) = 25 bytes.
// This binds the wrapped key to the specific nonce exchange and key metadata.

/// Wrap an AES-128 session key for AUTH_ADD_KEY_NONCE distribution.
auto wrap_aes128_key(
    Aes256SivKey const& transport_key,
    Nonce const& controller_nonce,
    Nonce const& target_nonce,
    KeyId const& key_id,
    Aes128Key const& key) -> Aes128WrappedKey;

/// Unwrap a received AES-128 session key.
auto unwrap_aes128_key(
    Aes256SivKey const& transport_key,
    Nonce const& controller_nonce,
    Nonce const& target_nonce,
    KeyId const& key_id,
    Aes128WrappedKey const& wrapped) -> std::optional<Aes128Key>;

/// Wrap an AES-256 session key for AUTH_ADD_KEY_NONCE distribution.
auto wrap_aes256_key(
    Aes256SivKey const& transport_key,
    Nonce const& controller_nonce,
    Nonce const& target_nonce,
    KeyId const& key_id,
    Aes256Key const& key) -> Aes256WrappedKey;

/// Unwrap a received AES-256 session key.
auto unwrap_aes256_key(
    Aes256SivKey const& transport_key,
    Nonce const& controller_nonce,
    Nonce const& target_nonce,
    KeyId const& key_id,
    Aes256WrappedKey const& wrapped) -> std::optional<Aes256Key>;

/// Wrap an AES-128-SIV session key for AUTH_ADD_KEY_NONCE distribution.
auto wrap_aes128_siv_key(
    Aes256SivKey const& transport_key,
    Nonce const& controller_nonce,
    Nonce const& target_nonce,
    KeyId const& key_id,
    Aes128SivKey const& key) -> Aes128SivWrappedKey;

/// Unwrap a received AES-128-SIV session key.
auto unwrap_aes128_siv_key(
    Aes256SivKey const& transport_key,
    Nonce const& controller_nonce,
    Nonce const& target_nonce,
    KeyId const& key_id,
    Aes128SivWrappedKey const& wrapped) -> std::optional<Aes128SivKey>;

/// Wrap an AES-256-SIV session key for AUTH_ADD_KEY_NONCE distribution.
auto wrap_aes256_siv_key(
    Aes256SivKey const& transport_key,
    Nonce const& controller_nonce,
    Nonce const& target_nonce,
    KeyId const& key_id,
    Aes256SivKey const& key) -> Aes256SivWrappedKey;

/// Unwrap a received AES-256-SIV session key.
auto unwrap_aes256_siv_key(
    Aes256SivKey const& transport_key,
    Nonce const& controller_nonce,
    Nonce const& target_nonce,
    KeyId const& key_id,
    Aes256SivWrappedKey const& wrapped) -> std::optional<Aes256SivKey>;

// Ed25519 signed public key entry: build (concept-constrained template)

/// Build an Ed25519 signed public key entry for chain-of-trust key management.
///
/// The signing_key can be any type satisfying Ed25519SigningKey -- software
/// Ed25519PrivateKey or a secure-enclave key with ADL-visible overloads.
/// Signature covers: key_id (8) || related_key_id (8) || public_key (32) = 48 bytes.
/// The signature_key_id is stored as a lookup hint but is NOT signed.
template <Ed25519SigningKey K>
auto build_ed25519_signed_public_key_entry(
    KeyId const& key_id,
    KeyId const& related_key_id,
    Ed25519PublicKey const& public_key,
    KeyId const& signature_key_id,
    K const& signing_key) -> Ed25519SignedPublicKeyEntry
{
    Ed25519SignedPublicKeyEntry entry;
    entry.key_id = key_id;
    entry.related_key_id = related_key_id;
    entry.public_key = public_key;
    entry.signature_key_id = signature_key_id;

    std::array<uint8_t, ed25519_signed_public_key_entry_signed_data_size> signed_data{};
    auto sd = std::span<uint8_t>(signed_data);
    sd[0] = signed_public_key_entry_domain_ed25519;
    internal::span_copy(sd.subspan(1, key_id_size), std::span<uint8_t const>(key_id.data));
    internal::span_copy(sd.subspan(1 + key_id_size, key_id_size), std::span<uint8_t const>(related_key_id.data));
    internal::span_copy(sd.subspan(1 + (2 * key_id_size), Ed25519PublicKey::LENGTH), std::span<uint8_t const>(public_key.data));

    entry.signature = ed25519_sign(signing_key, signed_data);
    return entry;
}

// X25519 signed public key entry: build (concept-constrained template)

/// Build an X25519 signed public key entry for chain-of-trust key management.
///
/// X25519 is a DH-only key type and cannot sign, so the signature uses Ed25519
/// (same curve family). The signing_key can be any type satisfying Ed25519SigningKey.
/// Signature covers: key_id (8) || related_key_id (8) || public_key (32) = 48 bytes.
template <Ed25519SigningKey K>
auto build_x25519_signed_public_key_entry(
    KeyId const& key_id,
    KeyId const& related_key_id,
    X25519PublicKey const& public_key,
    KeyId const& signature_key_id,
    K const& signing_key) -> X25519SignedPublicKeyEntry
{
    X25519SignedPublicKeyEntry entry;
    entry.key_id = key_id;
    entry.related_key_id = related_key_id;
    entry.public_key = public_key;
    entry.signature_key_id = signature_key_id;

    std::array<uint8_t, x25519_signed_public_key_entry_signed_data_size> signed_data{};
    auto sd = std::span<uint8_t>(signed_data);
    sd[0] = signed_public_key_entry_domain_x25519;
    internal::span_copy(sd.subspan(1, key_id_size), std::span<uint8_t const>(key_id.data));
    internal::span_copy(sd.subspan(1 + key_id_size, key_id_size), std::span<uint8_t const>(related_key_id.data));
    internal::span_copy(sd.subspan(1 + (2 * key_id_size), X25519PublicKey::LENGTH), std::span<uint8_t const>(public_key.data));

    entry.signature = ed25519_sign(signing_key, signed_data);
    return entry;
}

// P-256 signed public key entry: build (concept-constrained template)

/// Build a P-256 signed public key entry for chain-of-trust key management.
///
/// The signing_key can be any type satisfying P256SigningKey.
/// Signature covers: key_id (8) || related_key_id (8) || public_key (64) = 80 bytes.
template <P256SigningKey K>
auto build_p256_signed_public_key_entry(
    KeyId const& key_id,
    KeyId const& related_key_id,
    P256PublicKey const& public_key,
    KeyId const& signature_key_id,
    K const& signing_key) -> P256SignedPublicKeyEntry
{
    P256SignedPublicKeyEntry entry;
    entry.key_id = key_id;
    entry.related_key_id = related_key_id;
    entry.public_key = public_key;
    entry.signature_key_id = signature_key_id;

    std::array<uint8_t, p256_signed_public_key_entry_signed_data_size> signed_data{};
    auto sd = std::span<uint8_t>(signed_data);
    sd[0] = signed_public_key_entry_domain_p256;
    internal::span_copy(sd.subspan(1, key_id_size), std::span<uint8_t const>(key_id.data));
    internal::span_copy(sd.subspan(1 + key_id_size, key_id_size), std::span<uint8_t const>(related_key_id.data));
    internal::span_copy(sd.subspan(1 + (2 * key_id_size), P256PublicKey::LENGTH), std::span<uint8_t const>(public_key.data));

    entry.signature = p256_ecdsa_sign(signing_key, signed_data);
    return entry;
}

// P-256 signed X25519 public key entry: build (concept-constrained template)

/// Build a P-256 signed X25519 public key entry for chain-of-trust key management.
///
/// Used when the trust root is a P-256 certificate chain but key exchange uses X25519.
/// The signing_key can be any type satisfying P256SigningKey.
/// Signature covers: key_id (8) || related_key_id (8) || public_key (32) = 48 bytes.
template <P256SigningKey K>
auto build_p256_signed_x25519_public_key_entry(
    KeyId const& key_id,
    KeyId const& related_key_id,
    X25519PublicKey const& public_key,
    KeyId const& signature_key_id,
    K const& signing_key) -> P256SignedX25519PublicKeyEntry
{
    P256SignedX25519PublicKeyEntry entry;
    entry.key_id = key_id;
    entry.related_key_id = related_key_id;
    entry.public_key = public_key;
    entry.signature_key_id = signature_key_id;

    std::array<uint8_t, p256_signed_x25519_public_key_entry_signed_data_size> signed_data{};
    auto sd = std::span<uint8_t>(signed_data);
    sd[0] = signed_public_key_entry_domain_p256_x25519;
    internal::span_copy(sd.subspan(1, key_id_size), std::span<uint8_t const>(key_id.data));
    internal::span_copy(sd.subspan(1 + key_id_size, key_id_size), std::span<uint8_t const>(related_key_id.data));
    internal::span_copy(sd.subspan(1 + (2 * key_id_size), X25519PublicKey::LENGTH), std::span<uint8_t const>(public_key.data));

    entry.signature = p256_ecdsa_sign(signing_key, signed_data);
    return entry;
}

}  // namespace statusbar::crypto::avtp
