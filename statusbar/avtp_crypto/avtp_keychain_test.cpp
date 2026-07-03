// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Tests for IEEE 1722.1 key management types:
//   - KeyId (EUI-64 identifier, static/dynamic distinction)
//   - KeyType (4-bit enum, size helpers)
//   - KeyChainId (keychain identifier values)
//   - Ed25519SignedPublicKeyEntry (build, verify, tamper detection, serialization)
//   - X25519SignedPublicKeyEntry (build, verify, tamper detection, serialization)
//   - P256SignedPublicKeyEntry (build, verify, tamper detection, serialization)
//   - Private key entries (Ed25519, X25519, P-256)
//   - Transport key entries (AES-128, AES-256, AES-128-SIV, AES-256-SIV)
//   - Typed keychain containers (PublicKeyChain, PrivateKeyChain, SessionKeyChain)
//   - Chain-of-trust (root self-signs, root signs endpoint)

#include "statusbar/avtp_crypto/avtp_keychain.hpp"

#include "statusbar/avtp_crypto/avtp_crypto_pdu.hpp"
#include "statusbar/crypto/25519/ed25519.hpp"
#include "statusbar/crypto/25519/x25519.hpp"
#include "statusbar/crypto/p256/p256_ecdsa.hpp"
#include "statusbar/crypto/util/crypto_util_internal.hpp"
#include "statusbar/test/test.hpp"

#include <cstring>

using namespace statusbar::crypto;
using namespace statusbar::crypto::avtp;
using statusbar::crypto::internal::span_compare;

//
// KeyId tests
//

TEST(avtp_keychain, key_id)
{
    // Static key: bit 0 of first octet == 0
    KeyId static_key{};
    static_key.data = {0x00, 0x1B, 0xC5, 0x00, 0x00, 0x01, 0x00, 0x01};
    EXPECT_TRUE(static_key.is_static());
    EXPECT_TRUE(!static_key.is_dynamic());

    // Dynamic key: bit 0 of first octet == 1
    KeyId dynamic_key{};
    dynamic_key.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01, 0x00, 0x01};
    EXPECT_TRUE(!dynamic_key.is_static());
    EXPECT_TRUE(dynamic_key.is_dynamic());

    // IEEE 1722 default OUI 91-E0-F0 dynamic key
    EXPECT_TRUE(dynamic_key.data[0] == 0x91);
    EXPECT_TRUE(dynamic_key.data[1] == 0xE0);
    EXPECT_TRUE(dynamic_key.data[2] == 0xF0);

    // Equality
    KeyId a{};
    a.data = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    KeyId b{};
    b.data = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    KeyId c{};
    c.data = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x09};
    EXPECT_TRUE(a == b);
    EXPECT_TRUE(a != c);
}

//
// KeyType tests
//

TEST(avtp_keychain, key_type)
{
    // Verify data sizes
    EXPECT_TRUE(key_type_data_size(KeyType::aes128) == 16);
    EXPECT_TRUE(key_type_data_size(KeyType::aes256) == 32);
    EXPECT_TRUE(key_type_data_size(KeyType::ecc_public_256) == 336);
    EXPECT_TRUE(key_type_data_size(KeyType::ecc_private_256) == 232);
    EXPECT_TRUE(key_type_data_size(KeyType::aes128_siv) == 32);
    EXPECT_TRUE(key_type_data_size(KeyType::aes256_siv) == 64);
    EXPECT_TRUE(key_type_data_size(KeyType::ed25519_public) == 32);
    EXPECT_TRUE(key_type_data_size(KeyType::ed25519_private) == 32);
    EXPECT_TRUE(key_type_data_size(KeyType::x25519_public) == 32);
    EXPECT_TRUE(key_type_data_size(KeyType::x25519_private) == 32);

    // Reserved types return 0
    EXPECT_TRUE(key_type_data_size(static_cast<KeyType>(10)) == 0);
    EXPECT_TRUE(key_type_data_size(static_cast<KeyType>(15)) == 0);

    // Validity checks
    for (uint8_t i = 0; i <= 9; ++i) {
        EXPECT_TRUE(is_valid_key_type(static_cast<KeyType>(i)));
    }
    for (uint8_t i = 10; i <= 15; ++i) {
        EXPECT_TRUE(!is_valid_key_type(static_cast<KeyType>(i)));
    }
}

//
// KeyChainId tests
//

TEST(avtp_keychain, keychain_id)
{
    EXPECT_TRUE(static_cast<uint16_t>(KeyChainId::entity_public) == 0x0000);
    EXPECT_TRUE(static_cast<uint16_t>(KeyChainId::entity_private) == 0x0001);
    EXPECT_TRUE(static_cast<uint16_t>(KeyChainId::manufacturer_public) == 0x0002);
    EXPECT_TRUE(static_cast<uint16_t>(KeyChainId::controllers) == 0x0003);
    EXPECT_TRUE(static_cast<uint16_t>(KeyChainId::transport) == 0x0004);
}

//
// Ed25519 signed public key entry tests
//

TEST(avtp_keychain, ed25519_signed_entry)
{
    // Generate a root authority key
    std::array<uint8_t, 32> root_seed{};
    for (size_t i = 0; i < 32; ++i) {
        root_seed[i] = static_cast<uint8_t>(i + 1);
    }
    auto root_sk = ed25519_keypair_from_seed(root_seed);
    auto root_pk = ed25519_public_key(root_sk);

    // Generate an endpoint key
    std::array<uint8_t, 32> endpoint_seed{};
    for (size_t i = 0; i < 32; ++i) {
        endpoint_seed[i] = static_cast<uint8_t>(i + 0x80);
    }
    auto endpoint_sk = ed25519_keypair_from_seed(endpoint_seed);
    auto endpoint_pk = ed25519_public_key(endpoint_sk);

    // Key IDs
    KeyId pub_key_id{};
    pub_key_id.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x01};
    KeyId priv_key_id{};
    priv_key_id.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x02};
    KeyId root_key_id{};
    root_key_id.data = {0x00, 0x1B, 0xC5, 0x00, 0x00, 0x00, 0x00, 0x01};

    // --- Build + verify with authority key ---
    auto entry = build_ed25519_signed_public_key_entry(pub_key_id, priv_key_id, endpoint_pk, root_key_id, root_sk);

    EXPECT_TRUE(entry.key_id == pub_key_id);
    EXPECT_TRUE(entry.related_key_id == priv_key_id);
    EXPECT_TRUE(entry.public_key.data == endpoint_pk.data);
    EXPECT_TRUE(entry.signature_key_id == root_key_id);

    EXPECT_TRUE(verify_ed25519_signed_public_key_entry(entry, root_pk));

    // --- Wrong signer public key ---
    EXPECT_TRUE(!verify_ed25519_signed_public_key_entry(entry, endpoint_pk));

    // --- Tamper with key_id ---
    {
        auto tampered = entry;
        tampered.key_id.data[7] ^= 0xFF;
        EXPECT_TRUE(!verify_ed25519_signed_public_key_entry(tampered, root_pk));
    }

    // --- Tamper with related_key_id ---
    {
        auto tampered = entry;
        tampered.related_key_id.data[0] ^= 0x01;
        EXPECT_TRUE(!verify_ed25519_signed_public_key_entry(tampered, root_pk));
    }

    // --- Tamper with public_key ---
    {
        auto tampered = entry;
        tampered.public_key.data[15] ^= 0xFF;
        EXPECT_TRUE(!verify_ed25519_signed_public_key_entry(tampered, root_pk));
    }

    // --- Tamper with signature ---
    {
        auto tampered = entry;
        tampered.signature.data[0] ^= 0xFF;
        EXPECT_TRUE(!verify_ed25519_signed_public_key_entry(tampered, root_pk));
    }

    // --- Tamper with signature_key_id (NOT signed) — verify still passes ---
    {
        auto tampered = entry;
        tampered.signature_key_id.data[7] ^= 0xFF;
        EXPECT_TRUE(verify_ed25519_signed_public_key_entry(tampered, root_pk));
    }

    // --- Self-signed root key ---
    auto root_entry = build_ed25519_signed_public_key_entry(root_key_id, root_key_id, root_pk, root_key_id, root_sk);
    EXPECT_TRUE(verify_ed25519_signed_public_key_entry(root_entry, root_pk));
}

//
// Ed25519 serialization roundtrip
//

TEST(avtp_keychain, ed25519_serialization)
{
    // Generate a key and entry
    std::array<uint8_t, 32> seed{};
    for (size_t i = 0; i < 32; ++i) {
        seed[i] = static_cast<uint8_t>(i + 0x42);
    }
    auto sk = ed25519_keypair_from_seed(seed);
    auto pk = ed25519_public_key(sk);

    KeyId kid{};
    kid.data = {0x91, 0xE0, 0xF0, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    KeyId rkid{};
    rkid.data = {0x91, 0xE0, 0xF0, 0x11, 0x22, 0x33, 0x44, 0x55};
    KeyId skid{};
    skid.data = {0x00, 0x1B, 0xC5, 0xFF, 0xFE, 0x00, 0x00, 0x01};

    auto entry = build_ed25519_signed_public_key_entry(kid, rkid, pk, skid, sk);

    // Serialize
    std::array<uint8_t, ed25519_signed_public_key_entry_wire_size> wire{};
    serialize_ed25519_signed_public_key_entry(entry, wire);

    // Deserialize
    auto recovered = deserialize_ed25519_signed_public_key_entry(wire);

    // Verify all fields match
    EXPECT_TRUE(recovered.key_id == entry.key_id);
    EXPECT_TRUE(recovered.related_key_id == entry.related_key_id);
    EXPECT_TRUE(recovered.public_key.data == entry.public_key.data);
    EXPECT_TRUE(recovered.signature_key_id == entry.signature_key_id);
    EXPECT_TRUE(recovered.signature.data == entry.signature.data);

    // Deserialized entry should still verify
    EXPECT_TRUE(verify_ed25519_signed_public_key_entry(recovered, pk));
}

//
// Ed25519 chain of trust: root -> endpoint
//

TEST(avtp_keychain, ed25519_chain_of_trust)
{
    // Root key
    std::array<uint8_t, 32> root_seed{};
    for (size_t i = 0; i < 32; ++i) {
        root_seed[i] = static_cast<uint8_t>(i + 0x10);
    }
    auto root_sk = ed25519_keypair_from_seed(root_seed);
    auto root_pk = ed25519_public_key(root_sk);

    KeyId root_pub_kid{};
    root_pub_kid.data = {0x00, 0x1B, 0xC5, 0x00, 0x00, 0x00, 0x00, 0x01};
    KeyId root_priv_kid{};
    root_priv_kid.data = {0x00, 0x1B, 0xC5, 0x00, 0x00, 0x00, 0x00, 0x02};

    // Root self-signs its own public key
    auto root_entry = build_ed25519_signed_public_key_entry(root_pub_kid, root_priv_kid, root_pk, root_pub_kid, root_sk);
    EXPECT_TRUE(verify_ed25519_signed_public_key_entry(root_entry, root_pk));

    // Endpoint key
    std::array<uint8_t, 32> ep_seed{};
    for (size_t i = 0; i < 32; ++i) {
        ep_seed[i] = static_cast<uint8_t>(i + 0xA0);
    }
    auto ep_sk = ed25519_keypair_from_seed(ep_seed);
    auto ep_pk = ed25519_public_key(ep_sk);

    KeyId ep_pub_kid{};
    ep_pub_kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x10};
    KeyId ep_priv_kid{};
    ep_priv_kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x11};

    // Root signs the endpoint's public key
    auto ep_entry = build_ed25519_signed_public_key_entry(ep_pub_kid, ep_priv_kid, ep_pk, root_pub_kid, root_sk);

    // Verify endpoint entry with root's public key
    EXPECT_TRUE(verify_ed25519_signed_public_key_entry(ep_entry, root_pk));

    // Verify endpoint entry with wrong key (endpoint's own key) fails
    EXPECT_TRUE(!verify_ed25519_signed_public_key_entry(ep_entry, ep_pk));
}

//
// X25519 signed public key entry tests
//

TEST(avtp_keychain, x25519_signed_entry)
{
    // Ed25519 authority key (signs the X25519 entries)
    std::array<uint8_t, 32> root_seed{};
    for (size_t i = 0; i < 32; ++i) {
        root_seed[i] = static_cast<uint8_t>(i + 1);
    }
    auto root_sk = ed25519_keypair_from_seed(root_seed);
    auto root_pk = ed25519_public_key(root_sk);

    // X25519 endpoint key
    std::array<uint8_t, 32> x_seed{};
    for (size_t i = 0; i < 32; ++i) {
        x_seed[i] = static_cast<uint8_t>(i + 0x60);
    }
    auto x_sk = x25519_keypair_from_seed(x_seed);
    auto x_pk = x_sk.public_key;

    // A different Ed25519 key to test wrong-signer rejection
    std::array<uint8_t, 32> other_seed{};
    for (size_t i = 0; i < 32; ++i) {
        other_seed[i] = static_cast<uint8_t>(i + 0xC0);
    }
    auto other_sk = ed25519_keypair_from_seed(other_seed);
    auto other_pk = ed25519_public_key(other_sk);

    KeyId pub_kid{};
    pub_kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x20};
    KeyId priv_kid{};
    priv_kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x21};
    KeyId signer_kid{};
    signer_kid.data = {0x00, 0x1B, 0xC5, 0x00, 0x00, 0x00, 0x00, 0x01};

    // Build + verify
    auto entry = build_x25519_signed_public_key_entry(pub_kid, priv_kid, x_pk, signer_kid, root_sk);

    EXPECT_TRUE(entry.key_id == pub_kid);
    EXPECT_TRUE(entry.related_key_id == priv_kid);
    EXPECT_TRUE(entry.public_key.data == x_pk.data);
    EXPECT_TRUE(entry.signature_key_id == signer_kid);

    EXPECT_TRUE(verify_x25519_signed_public_key_entry(entry, root_pk));

    // Wrong signer
    EXPECT_TRUE(!verify_x25519_signed_public_key_entry(entry, other_pk));

    // Tamper with key_id
    {
        auto tampered = entry;
        tampered.key_id.data[7] ^= 0xFF;
        EXPECT_TRUE(!verify_x25519_signed_public_key_entry(tampered, root_pk));
    }

    // Tamper with public_key
    {
        auto tampered = entry;
        tampered.public_key.data[15] ^= 0xFF;
        EXPECT_TRUE(!verify_x25519_signed_public_key_entry(tampered, root_pk));
    }

    // Tamper with signature
    {
        auto tampered = entry;
        tampered.signature.data[0] ^= 0xFF;
        EXPECT_TRUE(!verify_x25519_signed_public_key_entry(tampered, root_pk));
    }

    // Tamper with signature_key_id (NOT signed) — verify still passes
    {
        auto tampered = entry;
        tampered.signature_key_id.data[7] ^= 0xFF;
        EXPECT_TRUE(verify_x25519_signed_public_key_entry(tampered, root_pk));
    }
}

//
// X25519 serialization roundtrip
//

TEST(avtp_keychain, x25519_serialization)
{
    std::array<uint8_t, 32> ed_seed{};
    for (size_t i = 0; i < 32; ++i) {
        ed_seed[i] = static_cast<uint8_t>(i + 0x42);
    }
    auto ed_sk = ed25519_keypair_from_seed(ed_seed);
    auto ed_pk = ed25519_public_key(ed_sk);

    std::array<uint8_t, 32> x_seed{};
    for (size_t i = 0; i < 32; ++i) {
        x_seed[i] = static_cast<uint8_t>(i + 0x70);
    }
    auto x_sk = x25519_keypair_from_seed(x_seed);

    KeyId kid{};
    kid.data = {0x91, 0xE0, 0xF0, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    KeyId rkid{};
    rkid.data = {0x91, 0xE0, 0xF0, 0x11, 0x22, 0x33, 0x44, 0x55};
    KeyId skid{};
    skid.data = {0x00, 0x1B, 0xC5, 0xFF, 0xFE, 0x00, 0x00, 0x01};

    auto entry = build_x25519_signed_public_key_entry(kid, rkid, x_sk.public_key, skid, ed_sk);

    // Serialize
    std::array<uint8_t, x25519_signed_public_key_entry_wire_size> wire{};
    serialize_x25519_signed_public_key_entry(entry, wire);

    // Deserialize
    auto recovered = deserialize_x25519_signed_public_key_entry(wire);

    EXPECT_TRUE(recovered.key_id == entry.key_id);
    EXPECT_TRUE(recovered.related_key_id == entry.related_key_id);
    EXPECT_TRUE(recovered.public_key.data == entry.public_key.data);
    EXPECT_TRUE(recovered.signature_key_id == entry.signature_key_id);
    EXPECT_TRUE(recovered.signature.data == entry.signature.data);

    EXPECT_TRUE(verify_x25519_signed_public_key_entry(recovered, ed_pk));
}

//
// P-256 signed public key entry tests
//

TEST(avtp_keychain, p256_signed_entry)
{
    // P-256 authority key
    std::array<uint8_t, 32> root_scalar{};
    for (size_t i = 0; i < 32; ++i) {
        root_scalar[i] = static_cast<uint8_t>(i + 1);
    }
    auto root_sk_opt = p256_keypair_from_scalar(root_scalar);
    EXPECT_TRUE(root_sk_opt.has_value());
    auto root_sk = *root_sk_opt;
    auto root_pk = p256_public_key(root_sk);

    // P-256 endpoint key
    std::array<uint8_t, 32> ep_scalar{};
    for (size_t i = 0; i < 32; ++i) {
        ep_scalar[i] = static_cast<uint8_t>(i + 0x40);
    }
    auto ep_sk_opt = p256_keypair_from_scalar(ep_scalar);
    EXPECT_TRUE(ep_sk_opt.has_value());
    auto ep_sk = *ep_sk_opt;
    auto ep_pk = p256_public_key(ep_sk);

    KeyId pub_kid{};
    pub_kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x30};
    KeyId priv_kid{};
    priv_kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x31};
    KeyId signer_kid{};
    signer_kid.data = {0x00, 0x1B, 0xC5, 0x00, 0x00, 0x00, 0x00, 0x10};

    // Build + verify
    auto entry = build_p256_signed_public_key_entry(pub_kid, priv_kid, ep_pk, signer_kid, root_sk);

    EXPECT_TRUE(entry.key_id == pub_kid);
    EXPECT_TRUE(entry.related_key_id == priv_kid);
    EXPECT_TRUE(entry.public_key.data == ep_pk.data);
    EXPECT_TRUE(entry.signature_key_id == signer_kid);

    EXPECT_TRUE(verify_p256_signed_public_key_entry(entry, root_pk));

    // Wrong signer
    EXPECT_TRUE(!verify_p256_signed_public_key_entry(entry, ep_pk));

    // Tamper with key_id
    {
        auto tampered = entry;
        tampered.key_id.data[7] ^= 0xFF;
        EXPECT_TRUE(!verify_p256_signed_public_key_entry(tampered, root_pk));
    }

    // Tamper with public_key
    {
        auto tampered = entry;
        tampered.public_key.data[15] ^= 0xFF;
        EXPECT_TRUE(!verify_p256_signed_public_key_entry(tampered, root_pk));
    }

    // Tamper with signature
    {
        auto tampered = entry;
        tampered.signature.data[0] ^= 0xFF;
        EXPECT_TRUE(!verify_p256_signed_public_key_entry(tampered, root_pk));
    }

    // Tamper with signature_key_id (NOT signed) — verify still passes
    {
        auto tampered = entry;
        tampered.signature_key_id.data[7] ^= 0xFF;
        EXPECT_TRUE(verify_p256_signed_public_key_entry(tampered, root_pk));
    }

    // Self-signed root key
    auto root_entry = build_p256_signed_public_key_entry(signer_kid, signer_kid, root_pk, signer_kid, root_sk);
    EXPECT_TRUE(verify_p256_signed_public_key_entry(root_entry, root_pk));
}

//
// P-256 serialization roundtrip
//

TEST(avtp_keychain, p256_serialization)
{
    std::array<uint8_t, 32> scalar{};
    for (size_t i = 0; i < 32; ++i) {
        scalar[i] = static_cast<uint8_t>(i + 0x42);
    }
    auto sk_opt = p256_keypair_from_scalar(scalar);
    EXPECT_TRUE(sk_opt.has_value());
    auto sk = *sk_opt;
    auto pk = p256_public_key(sk);

    KeyId kid{};
    kid.data = {0x91, 0xE0, 0xF0, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    KeyId rkid{};
    rkid.data = {0x91, 0xE0, 0xF0, 0x11, 0x22, 0x33, 0x44, 0x55};
    KeyId skid{};
    skid.data = {0x00, 0x1B, 0xC5, 0xFF, 0xFE, 0x00, 0x00, 0x01};

    auto entry = build_p256_signed_public_key_entry(kid, rkid, pk, skid, sk);

    // Serialize
    std::array<uint8_t, p256_signed_public_key_entry_wire_size> wire{};
    serialize_p256_signed_public_key_entry(entry, wire);

    // Deserialize
    auto recovered = deserialize_p256_signed_public_key_entry(wire);

    EXPECT_TRUE(recovered.key_id == entry.key_id);
    EXPECT_TRUE(recovered.related_key_id == entry.related_key_id);
    EXPECT_TRUE(recovered.public_key.data == entry.public_key.data);
    EXPECT_TRUE(recovered.signature_key_id == entry.signature_key_id);
    EXPECT_TRUE(recovered.signature.data == entry.signature.data);

    EXPECT_TRUE(verify_p256_signed_public_key_entry(recovered, pk));
}

//
// Private key entry tests
//

TEST(avtp_keychain, private_key_entries)
{
    KeyId kid{};
    kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x50};

    // Ed25519PrivateKeyEntry
    {
        std::array<uint8_t, 32> seed{};
        for (size_t i = 0; i < 32; ++i) {
            seed[i] = static_cast<uint8_t>(i + 1);
        }
        auto sk = ed25519_keypair_from_seed(seed);

        Ed25519PrivateKeyEntry entry;
        entry.key_id = kid;
        entry.private_key = sk;
        EXPECT_TRUE(entry.key_id == kid);
        EXPECT_TRUE(entry.private_key.public_key.data == sk.public_key.data);
    }

    // X25519PrivateKeyEntry
    {
        std::array<uint8_t, 32> seed{};
        for (size_t i = 0; i < 32; ++i) {
            seed[i] = static_cast<uint8_t>(i + 0x30);
        }
        auto sk = x25519_keypair_from_seed(seed);

        X25519PrivateKeyEntry entry;
        entry.key_id = kid;
        entry.private_key = sk;
        EXPECT_TRUE(entry.key_id == kid);
        EXPECT_TRUE(entry.private_key.public_key.data == sk.public_key.data);
    }

    // P256PrivateKeyEntry
    {
        std::array<uint8_t, 32> scalar{};
        for (size_t i = 0; i < 32; ++i) {
            scalar[i] = static_cast<uint8_t>(i + 0x50);
        }
        auto sk_opt = p256_keypair_from_scalar(scalar);
        EXPECT_TRUE(sk_opt.has_value());
        auto sk = *sk_opt;

        P256PrivateKeyEntry entry;
        entry.key_id = kid;
        entry.private_key = sk;
        EXPECT_TRUE(entry.key_id == kid);
        EXPECT_TRUE(entry.private_key.public_key.data == sk.public_key.data);
    }
}

//
// Transport key entry tests
//

TEST(avtp_keychain, transport_key_entries)
{
    KeyId kid{};
    kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x60};

    // Aes128KeyEntry
    {
        Aes128KeyEntry entry;
        entry.key_id = kid;
        entry.key.data.fill(0xAA);
        EXPECT_TRUE(entry.key_id == kid);
        EXPECT_TRUE(entry.key.data[0] == 0xAA);
    }

    // Aes256KeyEntry
    {
        Aes256KeyEntry entry;
        entry.key_id = kid;
        entry.key.data.fill(0xBB);
        EXPECT_TRUE(entry.key_id == kid);
        EXPECT_TRUE(entry.key.data[0] == 0xBB);
    }

    // Aes128SivKeyEntry
    {
        Aes128SivKeyEntry entry;
        entry.key_id = kid;
        entry.key.data.fill(0xCC);
        EXPECT_TRUE(entry.key_id == kid);
        EXPECT_TRUE(entry.key.data[0] == 0xCC);
        EXPECT_TRUE(entry.key.data.size() == 32);
    }

    // Aes256SivKeyEntry
    {
        Aes256SivKeyEntry entry;
        entry.key_id = kid;
        entry.key.data.fill(0xDD);
        EXPECT_TRUE(entry.key_id == kid);
        EXPECT_TRUE(entry.key.data[0] == 0xDD);
        EXPECT_TRUE(entry.key.data.size() == 64);
    }
}

//
// P-256 signed X25519 public key entry tests
//

TEST(avtp_keychain, p256_signed_x25519_entry)
{
    // P-256 authority key
    std::array<uint8_t, 32> root_scalar{};
    for (size_t i = 0; i < 32; ++i) {
        root_scalar[i] = static_cast<uint8_t>(i + 1);
    }
    auto root_sk_opt = p256_keypair_from_scalar(root_scalar);
    EXPECT_TRUE(root_sk_opt.has_value());
    auto root_sk = *root_sk_opt;
    auto root_pk = p256_public_key(root_sk);

    // A different P-256 key for wrong-signer test
    std::array<uint8_t, 32> other_scalar{};
    for (size_t i = 0; i < 32; ++i) {
        other_scalar[i] = static_cast<uint8_t>(i + 0x40);
    }
    auto other_sk_opt = p256_keypair_from_scalar(other_scalar);
    EXPECT_TRUE(other_sk_opt.has_value());
    auto other_pk = p256_public_key(*other_sk_opt);

    // X25519 endpoint key
    std::array<uint8_t, 32> x_seed{};
    for (size_t i = 0; i < 32; ++i) {
        x_seed[i] = static_cast<uint8_t>(i + 0x60);
    }
    auto x_sk = x25519_keypair_from_seed(x_seed);
    auto x_pk = x_sk.public_key;

    KeyId pub_kid{};
    pub_kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x40};
    KeyId priv_kid{};
    priv_kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x41};
    KeyId signer_kid{};
    signer_kid.data = {0x00, 0x1B, 0xC5, 0x00, 0x00, 0x00, 0x00, 0x10};

    // Build + verify
    auto entry = build_p256_signed_x25519_public_key_entry(pub_kid, priv_kid, x_pk, signer_kid, root_sk);

    EXPECT_TRUE(entry.key_id == pub_kid);
    EXPECT_TRUE(entry.related_key_id == priv_kid);
    EXPECT_TRUE(entry.public_key.data == x_pk.data);
    EXPECT_TRUE(entry.signature_key_id == signer_kid);

    EXPECT_TRUE(verify_p256_signed_x25519_public_key_entry(entry, root_pk));

    // Wrong signer
    EXPECT_TRUE(!verify_p256_signed_x25519_public_key_entry(entry, other_pk));

    // Tamper with key_id
    {
        auto tampered = entry;
        tampered.key_id.data[7] ^= 0xFF;
        EXPECT_TRUE(!verify_p256_signed_x25519_public_key_entry(tampered, root_pk));
    }

    // Tamper with public_key
    {
        auto tampered = entry;
        tampered.public_key.data[15] ^= 0xFF;
        EXPECT_TRUE(!verify_p256_signed_x25519_public_key_entry(tampered, root_pk));
    }

    // Tamper with signature
    {
        auto tampered = entry;
        tampered.signature.data[0] ^= 0xFF;
        EXPECT_TRUE(!verify_p256_signed_x25519_public_key_entry(tampered, root_pk));
    }

    // Tamper with signature_key_id (NOT signed) — verify still passes
    {
        auto tampered = entry;
        tampered.signature_key_id.data[7] ^= 0xFF;
        EXPECT_TRUE(verify_p256_signed_x25519_public_key_entry(tampered, root_pk));
    }
}

//
// P-256 signed X25519 serialization roundtrip
//

TEST(avtp_keychain, p256_signed_x25519_serialization)
{
    std::array<uint8_t, 32> scalar{};
    for (size_t i = 0; i < 32; ++i) {
        scalar[i] = static_cast<uint8_t>(i + 0x42);
    }
    auto sk_opt = p256_keypair_from_scalar(scalar);
    EXPECT_TRUE(sk_opt.has_value());
    auto sk = *sk_opt;
    auto pk = p256_public_key(sk);

    std::array<uint8_t, 32> x_seed{};
    for (size_t i = 0; i < 32; ++i) {
        x_seed[i] = static_cast<uint8_t>(i + 0x70);
    }
    auto x_sk = x25519_keypair_from_seed(x_seed);

    KeyId kid{};
    kid.data = {0x91, 0xE0, 0xF0, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    KeyId rkid{};
    rkid.data = {0x91, 0xE0, 0xF0, 0x11, 0x22, 0x33, 0x44, 0x55};
    KeyId skid{};
    skid.data = {0x00, 0x1B, 0xC5, 0xFF, 0xFE, 0x00, 0x00, 0x01};

    auto entry = build_p256_signed_x25519_public_key_entry(kid, rkid, x_sk.public_key, skid, sk);

    // Serialize
    std::array<uint8_t, p256_signed_x25519_public_key_entry_wire_size> wire{};
    serialize_p256_signed_x25519_public_key_entry(entry, wire);

    // Deserialize
    auto recovered = deserialize_p256_signed_x25519_public_key_entry(wire);

    EXPECT_TRUE(recovered.key_id == entry.key_id);
    EXPECT_TRUE(recovered.related_key_id == entry.related_key_id);
    EXPECT_TRUE(recovered.public_key.data == entry.public_key.data);
    EXPECT_TRUE(recovered.signature_key_id == entry.signature_key_id);
    EXPECT_TRUE(recovered.signature.data == entry.signature.data);

    EXPECT_TRUE(verify_p256_signed_x25519_public_key_entry(recovered, pk));
}

//
// Typed keychain container tests
//

TEST(avtp_keychain, typed_keychains)
{
    // PublicKeyChain can hold all three signed entry types
    {
        PublicKeyChain chain;

        // Add an Ed25519 signed entry
        std::array<uint8_t, 32> ed_seed{};
        ed_seed.fill(0x01);
        auto ed_sk = ed25519_keypair_from_seed(ed_seed);
        auto ed_pk = ed25519_public_key(ed_sk);
        KeyId kid1{};
        kid1.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x01};
        auto ed_entry = build_ed25519_signed_public_key_entry(kid1, kid1, ed_pk, kid1, ed_sk);
        chain.emplace_back(ed_entry);

        // Add an X25519 signed entry
        std::array<uint8_t, 32> x_seed{};
        x_seed.fill(0x02);
        auto x_sk = x25519_keypair_from_seed(x_seed);
        KeyId kid2{};
        kid2.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x02};
        auto x_entry = build_x25519_signed_public_key_entry(kid2, kid2, x_sk.public_key, kid1, ed_sk);
        chain.emplace_back(x_entry);

        // Add a P-256 signed entry
        std::array<uint8_t, 32> p_scalar{};
        p_scalar.fill(0x03);
        auto p_sk_opt = p256_keypair_from_scalar(p_scalar);
        EXPECT_TRUE(p_sk_opt.has_value());
        auto p_sk = *p_sk_opt;
        auto p_pk = p256_public_key(p_sk);
        KeyId kid3{};
        kid3.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x03};
        auto p_entry = build_p256_signed_public_key_entry(kid3, kid3, p_pk, kid3, p_sk);
        chain.emplace_back(p_entry);

        // Add a P-256 signed X25519 entry
        std::array<uint8_t, 32> x2_seed{};
        x2_seed.fill(0x04);
        auto x2_sk = x25519_keypair_from_seed(x2_seed);
        KeyId kid4{};
        kid4.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x04};
        auto px_entry = build_p256_signed_x25519_public_key_entry(kid4, kid4, x2_sk.public_key, kid3, p_sk);
        chain.emplace_back(px_entry);

        EXPECT_TRUE(chain.size() == 4);
        EXPECT_TRUE(std::holds_alternative<Ed25519SignedPublicKeyEntry>(chain[0]));
        EXPECT_TRUE(std::holds_alternative<X25519SignedPublicKeyEntry>(chain[1]));
        EXPECT_TRUE(std::holds_alternative<P256SignedPublicKeyEntry>(chain[2]));
        EXPECT_TRUE(std::holds_alternative<P256SignedX25519PublicKeyEntry>(chain[3]));
    }

    // PrivateKeyChain can hold all three private entry types
    {
        PrivateKeyChain chain;

        KeyId kid{};
        kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x10};

        std::array<uint8_t, 32> seed{};
        seed.fill(0x01);
        auto ed_sk = ed25519_keypair_from_seed(seed);
        chain.emplace_back(Ed25519PrivateKeyEntry{kid, ed_sk});

        seed.fill(0x02);
        auto x_sk = x25519_keypair_from_seed(seed);
        chain.emplace_back(X25519PrivateKeyEntry{kid, x_sk});

        seed.fill(0x03);
        auto p_sk_opt = p256_keypair_from_scalar(seed);
        EXPECT_TRUE(p_sk_opt.has_value());
        chain.emplace_back(P256PrivateKeyEntry{kid, *p_sk_opt});

        EXPECT_TRUE(chain.size() == 3);
        EXPECT_TRUE(std::holds_alternative<Ed25519PrivateKeyEntry>(chain[0]));
        EXPECT_TRUE(std::holds_alternative<X25519PrivateKeyEntry>(chain[1]));
        EXPECT_TRUE(std::holds_alternative<P256PrivateKeyEntry>(chain[2]));
    }

    // SessionKeyChain can hold all four symmetric entry types
    {
        SessionKeyChain chain;

        KeyId kid{};
        kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x20};

        Aes128KeyEntry aes128_entry;
        aes128_entry.key_id = kid;
        chain.emplace_back(aes128_entry);

        Aes256KeyEntry aes256_entry;
        aes256_entry.key_id = kid;
        chain.emplace_back(aes256_entry);

        Aes128SivKeyEntry siv128_entry;
        siv128_entry.key_id = kid;
        chain.emplace_back(siv128_entry);

        Aes256SivKeyEntry siv256_entry;
        siv256_entry.key_id = kid;
        chain.emplace_back(siv256_entry);

        EXPECT_TRUE(chain.size() == 4);
        EXPECT_TRUE(std::holds_alternative<Aes128KeyEntry>(chain[0]));
        EXPECT_TRUE(std::holds_alternative<Aes256KeyEntry>(chain[1]));
        EXPECT_TRUE(std::holds_alternative<Aes128SivKeyEntry>(chain[2]));
        EXPECT_TRUE(std::holds_alternative<Aes256SivKeyEntry>(chain[3]));
    }
}

//
// Keychain entry accessor and lookup tests
//

TEST(avtp_keychain, keychain_lookup)
{
    // --- get_key_entry_key_id on PublicKeyEntry ---
    {
        uint8_t seed[32] = {0x01};
        auto sk = ed25519_keypair_from_seed(seed);
        KeyId kid{};
        kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x01};
        KeyId related{};
        related.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x02};

        auto entry = build_ed25519_signed_public_key_entry(kid, related, ed25519_public_key(sk), kid, sk);
        PublicKeyEntry variant = entry;
        EXPECT_TRUE(get_key_entry_key_id(variant) == kid);
    }

    // --- get_key_entry_key_id on PrivateKeyEntry ---
    {
        KeyId kid{};
        kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x03};

        Ed25519PrivateKeyEntry priv_entry;
        priv_entry.key_id = kid;
        PrivateKeyEntry variant = priv_entry;
        EXPECT_TRUE(get_key_entry_key_id(variant) == kid);
    }

    // --- get_key_entry_key_id on SessionKeyEntry ---
    {
        KeyId kid{};
        kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x04};

        Aes128KeyEntry aes_entry;
        aes_entry.key_id = kid;
        SessionKeyEntry variant = aes_entry;
        EXPECT_TRUE(get_key_entry_key_id(variant) == kid);
    }

    // --- find_key_entry: hit ---
    {
        KeyId kid1{};
        kid1.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x10};
        KeyId kid2{};
        kid2.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x11};
        KeyId kid3{};
        kid3.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x12};

        PrivateKeyChain chain;

        Ed25519PrivateKeyEntry e1;
        e1.key_id = kid1;
        chain.emplace_back(e1);

        X25519PrivateKeyEntry e2;
        e2.key_id = kid2;
        chain.emplace_back(e2);

        P256PrivateKeyEntry e3;
        e3.key_id = kid3;
        chain.emplace_back(e3);

        auto found = find_key_entry(chain, kid2);
        EXPECT_TRUE(found.has_value());
        EXPECT_TRUE(get_key_entry_key_id(found->get()) == kid2);
        EXPECT_TRUE(std::holds_alternative<X25519PrivateKeyEntry>(found->get()));
    }

    // --- find_key_entry: miss ---
    {
        KeyId kid1{};
        kid1.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x20};
        KeyId missing{};
        missing.data = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

        SessionKeyChain chain;
        Aes128KeyEntry e1;
        e1.key_id = kid1;
        chain.emplace_back(e1);

        auto found = find_key_entry(chain, missing);
        EXPECT_TRUE(!found.has_value());
    }

    // --- find_key_entry: empty chain ---
    {
        KeyId kid{};
        kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x30};

        PublicKeyChain chain;
        auto found = find_key_entry(chain, kid);
        EXPECT_TRUE(!found.has_value());
    }
}

//

// Regression (crypto#3): Ed25519 and X25519 signed entries otherwise share an
// identical signed message, so a CA signature over one binds the other. The
// per-type domain tag must make a repurposed signature fail to verify.
TEST(avtp_keychain, cross_type_signature_confusion_rejected)
{
    std::array<uint8_t, 32> ca_seed{};
    ca_seed.fill(0x11);
    auto ca_sk = ed25519_keypair_from_seed(ca_seed);
    auto ca_pk = ed25519_public_key(ca_sk);

    std::array<uint8_t, 32> x_seed{};
    x_seed.fill(0x22);
    auto x_kp = x25519_keypair_from_seed(x_seed);

    KeyId kid{};
    kid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x0A};
    KeyId rkid{};
    rkid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x0B};
    KeyId skid{};
    skid.data = {0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x0C};

    // The CA legitimately signs an X25519 (DH-only) key binding.
    auto x_entry = build_x25519_signed_public_key_entry(kid, rkid, x_kp.public_key, skid, ca_sk);
    EXPECT_TRUE(verify_x25519_signed_public_key_entry(x_entry, ca_pk));

    // An attacker repurposes that exact signature as an Ed25519 (signing) key
    // binding for the same key_id: same fields, public_key = the X25519 bytes, same
    // signature. The domain tag must make this fail (it would verify without it).
    Ed25519SignedPublicKeyEntry forged_ed{};
    forged_ed.key_id = x_entry.key_id;
    forged_ed.related_key_id = x_entry.related_key_id;
    forged_ed.public_key.data = x_entry.public_key.data;
    forged_ed.signature_key_id = x_entry.signature_key_id;
    forged_ed.signature = x_entry.signature;
    EXPECT_FALSE(verify_ed25519_signed_public_key_entry(forged_ed, ca_pk));

    // Symmetric direction: an Ed25519 binding repurposed as X25519.
    auto ed_entry = build_ed25519_signed_public_key_entry(kid, rkid, ca_pk, skid, ca_sk);
    EXPECT_TRUE(verify_ed25519_signed_public_key_entry(ed_entry, ca_pk));
    X25519SignedPublicKeyEntry forged_x{};
    forged_x.key_id = ed_entry.key_id;
    forged_x.related_key_id = ed_entry.related_key_id;
    forged_x.public_key.data = ed_entry.public_key.data;
    forged_x.signature_key_id = ed_entry.signature_key_id;
    forged_x.signature = ed_entry.signature;
    EXPECT_FALSE(verify_x25519_signed_public_key_entry(forged_x, ca_pk));
}

TEST_MAIN(statusbar_avtp_crypto, avtp_keychain_test)
