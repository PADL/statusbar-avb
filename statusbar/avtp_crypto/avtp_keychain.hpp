// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// IEEE 1722.1 key management types and keychains for AVTP cryptographic operations.
//
// Defines KeyId (EUI-64 key identifiers), KeyType (key classification enum),
// KeyChainId (keychain identifiers per Table 7-184), and typed keychain entry
// structs for public, private, and transport keychains.
//
// Public keychains (entity_public, manufacturer_public, controllers) contain
// signed public key entries: Ed25519SignedPublicKeyEntry, X25519SignedPublicKeyEntry,
// P256SignedPublicKeyEntry, P256SignedX25519PublicKeyEntry — each binding a
// public key to a KeyId with a chain-of-trust signature.
//
// Private keychains (entity_private) contain private key entries:
// Ed25519PrivateKeyEntry, X25519PrivateKeyEntry, P256PrivateKeyEntry.
//
// Transport keychains contain symmetric key entries:
// Aes128KeyEntry, Aes256KeyEntry, Aes128SivKeyEntry, Aes256SivKeyEntry.
//
// Key struct definitions (Aes128Key, Ed25519PublicKey, P256PrivateKey, etc.)
// are in statusbar_crypto/avtp/avtp_keys.hpp.
//
// IEEE 1722.1 ECC wire formats (EccPublic256Wire, EccPrivate256Wire) are in
// statusbar_crypto/avtp/p256_wire.hpp.

#pragma once

#include "statusbar/crypto/keys.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace statusbar::crypto::avtp {

// Key types from statusbar::crypto (avtp_keys.hpp)
using ::statusbar::crypto::Aes128Key;
using ::statusbar::crypto::Aes128SivKey;
using ::statusbar::crypto::Aes256Key;
using ::statusbar::crypto::Aes256SivKey;
using ::statusbar::crypto::Ed25519PrivateKey;
using ::statusbar::crypto::Ed25519PublicKey;
using ::statusbar::crypto::Ed25519Signature;
using ::statusbar::crypto::P256EcdsaSignature;
using ::statusbar::crypto::P256PrivateKey;
using ::statusbar::crypto::P256PublicKey;
using ::statusbar::crypto::X25519PrivateKey;
using ::statusbar::crypto::X25519PublicKey;

// IEEE 1722.1 key management types

/// @brief Size of a key identifier (EUI-64) in bytes.
inline constexpr size_t key_id_size = 8;

/// @brief Domain-separation tags prepended to the SIGNED MESSAGE of each
/// signed-public-key entry type.
///
/// Without a tag, an Ed25519 and an X25519 entry produce a byte-identical 48-byte
/// signed message (key_id || related_key_id || 32-byte public_key), both verified
/// with Ed25519 -- so a CA signature over one binds the other (a key-type
/// confusion). The tag makes each type sign a distinct message. It is part of the
/// signed message only; it is NOT stored on the wire, so *_wire_size is unchanged.
/// (Mirrors the AKE path, which prepends message_type.)
inline constexpr uint8_t signed_public_key_entry_domain_ed25519 = 0x01;
inline constexpr uint8_t signed_public_key_entry_domain_x25519 = 0x02;
inline constexpr uint8_t signed_public_key_entry_domain_p256 = 0x03;
inline constexpr uint8_t signed_public_key_entry_domain_p256_x25519 = 0x04;

/// @brief EUI-64 key identifier per IEEE 1722.1.
///
/// Bit 0 of the first octet distinguishes static (0) from dynamic (1) keys.
/// Static keys use an OUI-based EUI-64. Dynamic/ephemeral keys use the
/// IEEE 1722 default OUI 91-E0-F0 when no device OUI is available.
struct KeyId
{
    static constexpr size_t LENGTH = key_id_size;

    std::array<uint8_t, LENGTH> data{};

    /// @brief True if this is a statically assigned key (bit 0 of first octet == 0).
    /// @return true if the key is static.
    auto is_static() const -> bool { return (data[0] & 0x01) == 0; }

    /// @brief True if this is a dynamically assigned ephemeral key (bit 0 of first octet == 1).
    /// @return true if the key is dynamic/ephemeral.
    auto is_dynamic() const -> bool { return (data[0] & 0x01) != 0; }

    /// @brief Equality comparison.
    auto operator==(KeyId const& other) const -> bool { return data == other.data; }
    /// @brief Inequality comparison.
    auto operator!=(KeyId const& other) const -> bool { return data != other.data; }
};

/// @brief Key type classification (4-bit field, values 0-15).
///
/// Matches IEEE 1722.1 key_type values 0-3, extended with new types 4-9
/// for SIV double-length keys and Ed25519/X25519 curve keys.
/// AES-GCM-SIV uses the same key format as plain AES-128/256, so no
/// separate key types are needed for GCM-SIV.
enum class KeyType : uint8_t
{
    aes128 = 0,           ///< AES-128 symmetric key (16 bytes)
    aes256 = 1,           ///< AES-256 symmetric key (32 bytes)
    ecc_public_256 = 2,   ///< NIST P-256 public key (64 bytes, x||y uncompressed)
    ecc_private_256 = 3,  ///< NIST P-256 private key (32 bytes, scalar d)
    aes128_siv = 4,       ///< AES-128-SIV combined key (32 bytes, double-length)
    aes256_siv = 5,       ///< AES-256-SIV combined key (64 bytes, double-length)
    ed25519_public = 6,   ///< Ed25519 public key (32 bytes)
    ed25519_private = 7,  ///< Ed25519 private key seed (32 bytes)
    x25519_public = 8,    ///< X25519 public key (32 bytes)
    x25519_private = 9,   ///< X25519 private key (32 bytes)
    // 10-15: reserved
};

/// @brief Return the key data size in bytes for a given key type.
/// @param type The key type to query.
/// @return The key data size in bytes for the given type.
auto key_type_data_size(KeyType type) -> size_t;

/// @brief Return true if the key type value is a defined type (0-9).
/// @param type The key type value to validate.
/// @return true if type is in the range 0-9.
auto is_valid_key_type(KeyType type) -> bool;

// Signed public key entries (chain-of-trust)

/// @brief Wire size of a serialized Ed25519SignedPublicKeyEntry (120 bytes).
inline constexpr size_t ed25519_signed_public_key_entry_wire_size = 120;

/// @brief Size of the data covered by the Ed25519 signature (key_id + related_key_id + public_key = 48 bytes).
inline constexpr size_t ed25519_signed_public_key_entry_signed_data_size = 49;  // 1 tag + 8 + 8 + 32

/// @brief A public key entry signed by an authority's Ed25519 key.
///
/// The signature covers key_id || related_key_id || public_key (48 bytes).
/// The signature_key_id is a lookup hint for finding the verifier's public key
/// and is NOT included in the signed data.
struct Ed25519SignedPublicKeyEntry
{
    static constexpr size_t LENGTH = ed25519_signed_public_key_entry_wire_size;

    KeyId key_id{};                 ///< This key's EUI-64 identifier (8 bytes)
    KeyId related_key_id{};         ///< Corresponding private key's identifier (8 bytes)
    Ed25519PublicKey public_key{};  ///< The public key value (32 bytes)
    KeyId signature_key_id{};       ///< Lookup hint for the signing key (8 bytes, NOT signed)
    Ed25519Signature signature{};   ///< Ed25519 signature over key_id||related_key_id||public_key (64 bytes)
};

/// @brief Wire size of a serialized X25519SignedPublicKeyEntry (120 bytes).
inline constexpr size_t x25519_signed_public_key_entry_wire_size = 120;

/// @brief Size of the data covered by the Ed25519 signature (key_id + related_key_id + public_key = 48 bytes).
inline constexpr size_t x25519_signed_public_key_entry_signed_data_size = 49;  // 1 tag + 8 + 8 + 32

/// @brief An X25519 public key entry signed by an authority's Ed25519 key.
///
/// The signature covers key_id || related_key_id || public_key (48 bytes).
/// X25519 is a DH-only key type, so the signature uses Ed25519 (same curve family).
/// The signature_key_id is a lookup hint and is NOT included in the signed data.
struct X25519SignedPublicKeyEntry
{
    static constexpr size_t LENGTH = x25519_signed_public_key_entry_wire_size;

    KeyId key_id{};                ///< This key's EUI-64 identifier (8 bytes)
    KeyId related_key_id{};        ///< Corresponding private key's identifier (8 bytes)
    X25519PublicKey public_key{};  ///< The X25519 public key value (32 bytes)
    KeyId signature_key_id{};      ///< Lookup hint for the signing key (8 bytes, NOT signed)
    Ed25519Signature signature{};  ///< Ed25519 signature over key_id||related_key_id||public_key (64 bytes)
};

/// @brief Wire size of a serialized P256SignedPublicKeyEntry (152 bytes).
inline constexpr size_t p256_signed_public_key_entry_wire_size = 152;

/// @brief Size of the data covered by the P-256 ECDSA signature (key_id + related_key_id + public_key = 80 bytes).
inline constexpr size_t p256_signed_public_key_entry_signed_data_size = 81;  // 1 tag + 8 + 8 + 64

/// @brief A P-256 public key entry signed by an authority's P-256 ECDSA key.
///
/// The signature covers key_id || related_key_id || public_key (80 bytes).
/// The signature_key_id is a lookup hint and is NOT included in the signed data.
struct P256SignedPublicKeyEntry
{
    static constexpr size_t LENGTH = p256_signed_public_key_entry_wire_size;

    KeyId key_id{};                  ///< This key's EUI-64 identifier (8 bytes)
    KeyId related_key_id{};          ///< Corresponding private key's identifier (8 bytes)
    P256PublicKey public_key{};      ///< The P-256 public key value (64 bytes, x||y)
    KeyId signature_key_id{};        ///< Lookup hint for the signing key (8 bytes, NOT signed)
    P256EcdsaSignature signature{};  ///< P-256 ECDSA signature over key_id||related_key_id||public_key (64 bytes)
};

// --- Ed25519 signed public key entry functions ---

/// @brief Verify the signature on an Ed25519 signed public key entry.
/// @param entry The signed public key entry to verify.
/// @param signer_public_key The authority's Ed25519 public key used to verify the signature.
/// @return true if the signature is valid.
auto verify_ed25519_signed_public_key_entry(Ed25519SignedPublicKeyEntry const& entry, Ed25519PublicKey const& signer_public_key)
    -> bool;

/// @brief Serialize an Ed25519SignedPublicKeyEntry to a 120-byte wire buffer.
/// @param entry The entry to serialize.
/// @param out Destination buffer (120 bytes).
auto serialize_ed25519_signed_public_key_entry(
    Ed25519SignedPublicKeyEntry const& entry, std::span<uint8_t, ed25519_signed_public_key_entry_wire_size> out) -> void;

/// @brief Deserialize an Ed25519SignedPublicKeyEntry from a 120-byte wire buffer.
/// @param in Source buffer (120 bytes).
/// @return The deserialized entry.
auto deserialize_ed25519_signed_public_key_entry(std::span<uint8_t const, ed25519_signed_public_key_entry_wire_size> in)
    -> Ed25519SignedPublicKeyEntry;

// --- X25519 signed public key entry functions ---

/// @brief Verify the signature on an X25519 signed public key entry.
///
/// Reconstructs the 48-byte signed data (key_id || related_key_id || public_key)
/// and verifies the Ed25519 signature.
/// @param entry The signed X25519 public key entry to verify.
/// @param signer_public_key The authority's Ed25519 public key used to verify the signature.
/// @return true if the signature is valid.
auto verify_x25519_signed_public_key_entry(X25519SignedPublicKeyEntry const& entry, Ed25519PublicKey const& signer_public_key)
    -> bool;

/// @brief Serialize an X25519SignedPublicKeyEntry to a 120-byte wire buffer.
/// @param entry The entry to serialize.
/// @param out Destination buffer (120 bytes).
auto serialize_x25519_signed_public_key_entry(
    X25519SignedPublicKeyEntry const& entry, std::span<uint8_t, x25519_signed_public_key_entry_wire_size> out) -> void;

/// @brief Deserialize an X25519SignedPublicKeyEntry from a 120-byte wire buffer.
/// @param in Source buffer (120 bytes).
/// @return The deserialized entry.
auto deserialize_x25519_signed_public_key_entry(std::span<uint8_t const, x25519_signed_public_key_entry_wire_size> in)
    -> X25519SignedPublicKeyEntry;

// --- P-256 signed X25519 public key entry ---

/// @brief Wire size of a serialized P256SignedX25519PublicKeyEntry (120 bytes).
inline constexpr size_t p256_signed_x25519_public_key_entry_wire_size = 120;

/// @brief Size of the data covered by the P-256 ECDSA signature (key_id + related_key_id + public_key = 48 bytes).
inline constexpr size_t p256_signed_x25519_public_key_entry_signed_data_size = 49;  // 1 tag + 8 + 8 + 32

/// @brief An X25519 public key entry signed by an authority's P-256 ECDSA key.
///
/// Used when the trust root is a P-256 certificate chain but key exchange uses X25519.
/// The signature covers key_id || related_key_id || public_key (48 bytes).
/// The signature_key_id is a lookup hint and is NOT included in the signed data.
struct P256SignedX25519PublicKeyEntry
{
    static constexpr size_t LENGTH = p256_signed_x25519_public_key_entry_wire_size;

    KeyId key_id{};                  ///< This key's EUI-64 identifier (8 bytes)
    KeyId related_key_id{};          ///< Corresponding private key's identifier (8 bytes)
    X25519PublicKey public_key{};    ///< The X25519 public key value (32 bytes)
    KeyId signature_key_id{};        ///< Lookup hint for the signing key (8 bytes, NOT signed)
    P256EcdsaSignature signature{};  ///< P-256 ECDSA signature over key_id||related_key_id||public_key (64 bytes)
};

// --- P256 signed public key entry functions ---

/// @brief Verify the signature on a P-256 signed public key entry.
///
/// Reconstructs the 80-byte signed data (key_id || related_key_id || public_key)
/// and verifies the P-256 ECDSA signature.
/// @param entry The signed P-256 public key entry to verify.
/// @param signer_public_key The authority's P-256 public key used to verify the ECDSA signature.
/// @return true if the signature is valid.
auto verify_p256_signed_public_key_entry(P256SignedPublicKeyEntry const& entry, P256PublicKey const& signer_public_key) -> bool;

/// @brief Serialize a P256SignedPublicKeyEntry to a 152-byte wire buffer.
/// @param entry The entry to serialize.
/// @param out Destination buffer (152 bytes).
auto serialize_p256_signed_public_key_entry(
    P256SignedPublicKeyEntry const& entry, std::span<uint8_t, p256_signed_public_key_entry_wire_size> out) -> void;

/// @brief Deserialize a P256SignedPublicKeyEntry from a 152-byte wire buffer.
/// @param in Source buffer (152 bytes).
/// @return The deserialized entry.
auto deserialize_p256_signed_public_key_entry(std::span<uint8_t const, p256_signed_public_key_entry_wire_size> in)
    -> P256SignedPublicKeyEntry;

// --- P-256 signed X25519 public key entry functions ---

/// @brief Verify the signature on a P-256 signed X25519 public key entry.
///
/// Reconstructs the 48-byte signed data (key_id || related_key_id || public_key)
/// and verifies the P-256 ECDSA signature.
/// @param entry The signed X25519 public key entry to verify.
/// @param signer_public_key The authority's P-256 public key used to verify the ECDSA signature.
/// @return true if the signature is valid.
auto verify_p256_signed_x25519_public_key_entry(P256SignedX25519PublicKeyEntry const& entry, P256PublicKey const& signer_public_key)
    -> bool;

/// @brief Serialize a P256SignedX25519PublicKeyEntry to a 120-byte wire buffer.
/// @param entry The entry to serialize.
/// @param out Destination buffer (120 bytes).
auto serialize_p256_signed_x25519_public_key_entry(
    P256SignedX25519PublicKeyEntry const& entry, std::span<uint8_t, p256_signed_x25519_public_key_entry_wire_size> out) -> void;

/// @brief Deserialize a P256SignedX25519PublicKeyEntry from a 120-byte wire buffer.
/// @param in Source buffer (120 bytes).
/// @return The deserialized entry.
auto deserialize_p256_signed_x25519_public_key_entry(std::span<uint8_t const, p256_signed_x25519_public_key_entry_wire_size> in)
    -> P256SignedX25519PublicKeyEntry;

// Private key entries (KeyId + private key)

/// @brief Ed25519 private key with its EUI-64 identifier.
struct Ed25519PrivateKeyEntry
{
    KeyId key_id{};                   ///< This key's EUI-64 identifier (8 bytes)
    Ed25519PrivateKey private_key{};  ///< The Ed25519 private key (64 bytes expanded)
};

/// @brief X25519 private key with its EUI-64 identifier.
struct X25519PrivateKeyEntry
{
    KeyId key_id{};                  ///< This key's EUI-64 identifier (8 bytes)
    X25519PrivateKey private_key{};  ///< The X25519 private key (32 bytes)
};

/// @brief P-256 private key with its EUI-64 identifier.
struct P256PrivateKeyEntry
{
    KeyId key_id{};                ///< This key's EUI-64 identifier (8 bytes)
    P256PrivateKey private_key{};  ///< The P-256 private key (32 bytes scalar)
};

// Transport key entries (KeyId + symmetric key)

/// @brief AES-128 symmetric key with its EUI-64 identifier.
struct Aes128KeyEntry
{
    KeyId key_id{};   ///< This key's EUI-64 identifier (8 bytes)
    Aes128Key key{};  ///< The AES-128 key (16 bytes)
};

/// @brief AES-256 symmetric key with its EUI-64 identifier.
struct Aes256KeyEntry
{
    KeyId key_id{};   ///< This key's EUI-64 identifier (8 bytes)
    Aes256Key key{};  ///< The AES-256 key (32 bytes)
};

/// @brief AES-128-SIV combined key with its EUI-64 identifier.
struct Aes128SivKeyEntry
{
    KeyId key_id{};      ///< This key's EUI-64 identifier (8 bytes)
    Aes128SivKey key{};  ///< The AES-128-SIV key (32 bytes)
};

/// @brief AES-256-SIV combined key with its EUI-64 identifier.
struct Aes256SivKeyEntry
{
    KeyId key_id{};      ///< This key's EUI-64 identifier (8 bytes)
    Aes256SivKey key{};  ///< The AES-256-SIV key (64 bytes)
};

// Typed keychain variants and containers

/// @brief A public key entry from a public keychain (entity_public, manufacturer_public, controllers).
using PublicKeyEntry =
    std::variant<Ed25519SignedPublicKeyEntry, X25519SignedPublicKeyEntry, P256SignedPublicKeyEntry, P256SignedX25519PublicKeyEntry>;

/// @brief A private key entry from the entity_private keychain.
using PrivateKeyEntry = std::variant<Ed25519PrivateKeyEntry, X25519PrivateKeyEntry, P256PrivateKeyEntry>;

/// @brief A session key entry from the transport keychain.
using SessionKeyEntry = std::variant<Aes128KeyEntry, Aes256KeyEntry, Aes128SivKeyEntry, Aes256SivKeyEntry>;

/// @brief Container for public keychains (entity_public, manufacturer_public, controllers).
///
/// Uses std::pmr::vector so callers can pin allocations to a specific
/// memory_resource (e.g. a monotonic_buffer_resource preallocated at
/// entity init). Default-constructed chains fall back to
/// std::pmr::get_default_resource().
using PublicKeyChain = std::pmr::vector<PublicKeyEntry>;

/// @brief Container for the entity_private keychain.
using PrivateKeyChain = std::pmr::vector<PrivateKeyEntry>;

/// @brief Container for the transport keychain.
using SessionKeyChain = std::pmr::vector<SessionKeyEntry>;

// Keychain entry accessors

/// @brief Extract the key_id from any keychain entry variant.
///
/// Works with PublicKeyEntry, PrivateKeyEntry, or SessionKeyEntry.
/// @param entry The keychain entry variant to extract the key_id from.
/// @return Reference to the entry's KeyId.
template <typename EntryVariant>
auto get_key_entry_key_id(EntryVariant const& entry) -> KeyId const&
{
    return std::visit([](auto const& e) -> KeyId const& { return e.key_id; }, entry);
}

/// @brief Find an entry by KeyId in a keychain. Returns nullptr if not found.
///
/// Works with PublicKeyChain, PrivateKeyChain, or SessionKeyChain.
/// @param chain The keychain to search.
/// @param id The KeyId to look up.
/// @return Optional reference to the matching entry, or std::nullopt if not found.
template <typename KeyChainT>
auto find_key_entry(KeyChainT const& chain, KeyId const& id)
    -> std::optional<std::reference_wrapper<typename KeyChainT::value_type const>>
{
    for (auto const& entry : chain) {
        if (get_key_entry_key_id(entry) == id) {
            return entry;
        }
    }
    return std::nullopt;
}

// IEEE 1722.1 Keychain identifiers (Table 7-184)

/// @brief Keychain identifier values per IEEE 1722.1-2021 Table 7-184.
enum class KeyChainId : uint16_t
{
    entity_public = 0x0000,        ///< Entity's ECC_PUBLIC_256 key for verifying/encrypting
    entity_private = 0x0001,       ///< Entity's ECC_PRIVATE_256 key (paired with entity_public)
    manufacturer_public = 0x0002,  ///< Manufacturer's ECC_PUBLIC_256 signing key(s)
    controllers = 0x0003,          ///< Authorized controller ECC_PUBLIC_256 keys
    transport = 0x0004,            ///< Transport security keys
};

struct KeyChains
{
    PublicKeyChain entity_public;
    PrivateKeyChain entity_private;
    PublicKeyChain manufacturer_public;
    PublicKeyChain controllers;
    SessionKeyChain transport;

    /// @brief Find a public key entry by KeyId across all public keychains.
    /// @param id The KeyId to look up.
    /// @return Optional reference to the matching entry, or std::nullopt if not found.
    auto find_public_key_entry(KeyId const& id) const -> std::optional<std::reference_wrapper<PublicKeyEntry const>>
    {
        if (auto entry = find_key_entry(manufacturer_public, id)) {
            return entry;
        }
        if (auto entry = find_key_entry(controllers, id)) {
            return entry;
        }
        if (auto entry = find_key_entry(entity_public, id)) {
            return entry;
        }
        return std::nullopt;
    }

    /// @brief Find a private key entry by KeyId in the entity_private keychain.
    /// @param id The KeyId to look up.
    /// @return Optional reference to the matching entry, or std::nullopt if not found.
    auto find_private_key_entry(KeyId const& id) const -> std::optional<std::reference_wrapper<PrivateKeyEntry const>>
    {
        return find_key_entry(entity_private, id);
    }

    /// @brief Find a session key entry by KeyId in the transport keychain.
    /// @param id The KeyId to look up.
    /// @return Optional reference to the matching entry, or std::nullopt if not found.
    auto find_session_key_entry(KeyId const& id) const -> std::optional<std::reference_wrapper<SessionKeyEntry const>>
    {
        return find_key_entry(transport, id);
    }
};

}  // namespace statusbar::crypto::avtp
