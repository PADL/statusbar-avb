// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// IEEE 1722.1 key management implementation.

#include "statusbar/avtp_crypto/avtp_keychain.hpp"

#include "statusbar/avtp_crypto/p256_wire.hpp"
#include "statusbar/crypto/25519/ed25519.hpp"
#include "statusbar/crypto/p256/p256_ecdsa.hpp"
#include "statusbar/crypto/util/crypto_util_internal.hpp"

namespace statusbar::crypto::avtp {

using internal::span_copy;
using std::span;

//
// Key type helpers
//

auto key_type_data_size(KeyType type) -> size_t
{
    switch (type) {
        case KeyType::aes128:
            return Aes128Key::LENGTH;  // 16
        case KeyType::aes256:
            return Aes256Key::LENGTH;  // 32
        case KeyType::ecc_public_256:
            return ecc_public_256_wire_size;  // 336
        case KeyType::ecc_private_256:
            return ecc_private_256_wire_size;  // 232
        case KeyType::aes128_siv:
            return Aes128SivKey::LENGTH;  // 32
        case KeyType::aes256_siv:
            return Aes256SivKey::LENGTH;  // 64
        case KeyType::ed25519_public:
            return Ed25519PublicKey::LENGTH;  // 32
        case KeyType::ed25519_private:
            return ed25519_seed_size;
        case KeyType::x25519_public:
            return X25519PublicKey::LENGTH;  // 32
        case KeyType::x25519_private:
            return X25519PrivateKey::LENGTH;  // 32
        default:
            return 0;
    }
}

auto is_valid_key_type(KeyType type) -> bool
{
    return static_cast<uint8_t>(type) <= 9;
}

//
// Ed25519 signed public key entry
//

auto verify_ed25519_signed_public_key_entry(Ed25519SignedPublicKeyEntry const& entry, Ed25519PublicKey const& signer_public_key)
    -> bool
{
    std::array<uint8_t, ed25519_signed_public_key_entry_signed_data_size> signed_data{};
    auto sd = span<uint8_t>(signed_data);
    sd[0] = signed_public_key_entry_domain_ed25519;
    span_copy(sd.subspan(1, key_id_size), entry.key_id.data);
    span_copy(sd.subspan(1 + key_id_size, key_id_size), entry.related_key_id.data);
    span_copy(sd.subspan(1 + (2 * key_id_size), Ed25519PublicKey::LENGTH), entry.public_key.data);

    return ed25519_verify(signer_public_key, signed_data, entry.signature);
}

auto serialize_ed25519_signed_public_key_entry(
    Ed25519SignedPublicKeyEntry const& entry, span<uint8_t, ed25519_signed_public_key_entry_wire_size> out) -> void
{
    size_t offset = 0;
    span_copy(out.subspan(offset, key_id_size), entry.key_id.data);
    offset += key_id_size;
    span_copy(out.subspan(offset, key_id_size), entry.related_key_id.data);
    offset += key_id_size;
    span_copy(out.subspan(offset, Ed25519PublicKey::LENGTH), entry.public_key.data);
    offset += Ed25519PublicKey::LENGTH;
    span_copy(out.subspan(offset, key_id_size), entry.signature_key_id.data);
    offset += key_id_size;
    span_copy(out.subspan(offset, Ed25519Signature::LENGTH), entry.signature.data);
}

auto deserialize_ed25519_signed_public_key_entry(span<uint8_t const, ed25519_signed_public_key_entry_wire_size> in)
    -> Ed25519SignedPublicKeyEntry
{
    Ed25519SignedPublicKeyEntry entry;
    size_t offset = 0;
    span_copy(entry.key_id.data, in.subspan(offset, key_id_size));
    offset += key_id_size;
    span_copy(entry.related_key_id.data, in.subspan(offset, key_id_size));
    offset += key_id_size;
    span_copy(entry.public_key.data, in.subspan(offset, Ed25519PublicKey::LENGTH));
    offset += Ed25519PublicKey::LENGTH;
    span_copy(entry.signature_key_id.data, in.subspan(offset, key_id_size));
    offset += key_id_size;
    span_copy(entry.signature.data, in.subspan(offset, Ed25519Signature::LENGTH));
    return entry;
}

//
// X25519 signed public key entry
//

auto verify_x25519_signed_public_key_entry(X25519SignedPublicKeyEntry const& entry, Ed25519PublicKey const& signer_public_key)
    -> bool
{
    std::array<uint8_t, x25519_signed_public_key_entry_signed_data_size> signed_data{};
    auto sd = span<uint8_t>(signed_data);
    sd[0] = signed_public_key_entry_domain_x25519;
    span_copy(sd.subspan(1, key_id_size), entry.key_id.data);
    span_copy(sd.subspan(1 + key_id_size, key_id_size), entry.related_key_id.data);
    span_copy(sd.subspan(1 + (2 * key_id_size), X25519PublicKey::LENGTH), entry.public_key.data);

    return ed25519_verify(signer_public_key, signed_data, entry.signature);
}

auto serialize_x25519_signed_public_key_entry(
    X25519SignedPublicKeyEntry const& entry, span<uint8_t, x25519_signed_public_key_entry_wire_size> out) -> void
{
    size_t offset = 0;
    span_copy(out.subspan(offset, key_id_size), entry.key_id.data);
    offset += key_id_size;
    span_copy(out.subspan(offset, key_id_size), entry.related_key_id.data);
    offset += key_id_size;
    span_copy(out.subspan(offset, X25519PublicKey::LENGTH), entry.public_key.data);
    offset += X25519PublicKey::LENGTH;
    span_copy(out.subspan(offset, key_id_size), entry.signature_key_id.data);
    offset += key_id_size;
    span_copy(out.subspan(offset, Ed25519Signature::LENGTH), entry.signature.data);
}

auto deserialize_x25519_signed_public_key_entry(span<uint8_t const, x25519_signed_public_key_entry_wire_size> in)
    -> X25519SignedPublicKeyEntry
{
    X25519SignedPublicKeyEntry entry;
    size_t offset = 0;
    span_copy(entry.key_id.data, in.subspan(offset, key_id_size));
    offset += key_id_size;
    span_copy(entry.related_key_id.data, in.subspan(offset, key_id_size));
    offset += key_id_size;
    span_copy(entry.public_key.data, in.subspan(offset, X25519PublicKey::LENGTH));
    offset += X25519PublicKey::LENGTH;
    span_copy(entry.signature_key_id.data, in.subspan(offset, key_id_size));
    offset += key_id_size;
    span_copy(entry.signature.data, in.subspan(offset, Ed25519Signature::LENGTH));
    return entry;
}

//
// P-256 signed public key entry
//

auto verify_p256_signed_public_key_entry(P256SignedPublicKeyEntry const& entry, P256PublicKey const& signer_public_key) -> bool
{
    std::array<uint8_t, p256_signed_public_key_entry_signed_data_size> signed_data{};
    auto sd = span<uint8_t>(signed_data);
    sd[0] = signed_public_key_entry_domain_p256;
    span_copy(sd.subspan(1, key_id_size), entry.key_id.data);
    span_copy(sd.subspan(1 + key_id_size, key_id_size), entry.related_key_id.data);
    span_copy(sd.subspan(1 + (2 * key_id_size), P256PublicKey::LENGTH), entry.public_key.data);

    return p256_ecdsa_verify(signer_public_key, signed_data, entry.signature);
}

auto serialize_p256_signed_public_key_entry(
    P256SignedPublicKeyEntry const& entry, span<uint8_t, p256_signed_public_key_entry_wire_size> out) -> void
{
    size_t offset = 0;
    span_copy(out.subspan(offset, key_id_size), entry.key_id.data);
    offset += key_id_size;
    span_copy(out.subspan(offset, key_id_size), entry.related_key_id.data);
    offset += key_id_size;
    span_copy(out.subspan(offset, P256PublicKey::LENGTH), entry.public_key.data);
    offset += P256PublicKey::LENGTH;
    span_copy(out.subspan(offset, key_id_size), entry.signature_key_id.data);
    offset += key_id_size;
    span_copy(out.subspan(offset, P256EcdsaSignature::LENGTH), entry.signature.data);
}

auto deserialize_p256_signed_public_key_entry(span<uint8_t const, p256_signed_public_key_entry_wire_size> in)
    -> P256SignedPublicKeyEntry
{
    P256SignedPublicKeyEntry entry;
    size_t offset = 0;
    span_copy(entry.key_id.data, in.subspan(offset, key_id_size));
    offset += key_id_size;
    span_copy(entry.related_key_id.data, in.subspan(offset, key_id_size));
    offset += key_id_size;
    span_copy(entry.public_key.data, in.subspan(offset, P256PublicKey::LENGTH));
    offset += P256PublicKey::LENGTH;
    span_copy(entry.signature_key_id.data, in.subspan(offset, key_id_size));
    offset += key_id_size;
    span_copy(entry.signature.data, in.subspan(offset, P256EcdsaSignature::LENGTH));
    return entry;
}

//
// P-256 signed X25519 public key entry
//

auto verify_p256_signed_x25519_public_key_entry(P256SignedX25519PublicKeyEntry const& entry, P256PublicKey const& signer_public_key)
    -> bool
{
    std::array<uint8_t, p256_signed_x25519_public_key_entry_signed_data_size> signed_data{};
    auto sd = span<uint8_t>(signed_data);
    sd[0] = signed_public_key_entry_domain_p256_x25519;
    span_copy(sd.subspan(1, key_id_size), entry.key_id.data);
    span_copy(sd.subspan(1 + key_id_size, key_id_size), entry.related_key_id.data);
    span_copy(sd.subspan(1 + (2 * key_id_size), X25519PublicKey::LENGTH), entry.public_key.data);

    return p256_ecdsa_verify(signer_public_key, signed_data, entry.signature);
}

auto serialize_p256_signed_x25519_public_key_entry(
    P256SignedX25519PublicKeyEntry const& entry, span<uint8_t, p256_signed_x25519_public_key_entry_wire_size> out) -> void
{
    size_t offset = 0;
    span_copy(out.subspan(offset, key_id_size), entry.key_id.data);
    offset += key_id_size;
    span_copy(out.subspan(offset, key_id_size), entry.related_key_id.data);
    offset += key_id_size;
    span_copy(out.subspan(offset, X25519PublicKey::LENGTH), entry.public_key.data);
    offset += X25519PublicKey::LENGTH;
    span_copy(out.subspan(offset, key_id_size), entry.signature_key_id.data);
    offset += key_id_size;
    span_copy(out.subspan(offset, P256EcdsaSignature::LENGTH), entry.signature.data);
}

auto deserialize_p256_signed_x25519_public_key_entry(span<uint8_t const, p256_signed_x25519_public_key_entry_wire_size> in)
    -> P256SignedX25519PublicKeyEntry
{
    P256SignedX25519PublicKeyEntry entry;
    size_t offset = 0;
    span_copy(entry.key_id.data, in.subspan(offset, key_id_size));
    offset += key_id_size;
    span_copy(entry.related_key_id.data, in.subspan(offset, key_id_size));
    offset += key_id_size;
    span_copy(entry.public_key.data, in.subspan(offset, X25519PublicKey::LENGTH));
    offset += X25519PublicKey::LENGTH;
    span_copy(entry.signature_key_id.data, in.subspan(offset, key_id_size));
    offset += key_id_size;
    span_copy(entry.signature.data, in.subspan(offset, P256EcdsaSignature::LENGTH));
    return entry;
}

}  // namespace statusbar::crypto::avtp
