// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AVTP wire-format PDU serialization/deserialization implementation

#include "statusbar/avtp_crypto/avtp_crypto_pdu.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/crypto/aes_gcm_siv/aes128_gcm_siv.hpp"
#include "statusbar/crypto/aes_gcm_siv/aes256_gcm_siv.hpp"
#include "statusbar/crypto/aes_siv/aes128_siv.hpp"
#include "statusbar/crypto/aes_siv/aes256_siv.hpp"
#include "statusbar/crypto/util/crypto_util_internal.hpp"
#include "statusbar/ieee/ieee.hpp"

namespace statusbar::crypto::avtp {

using internal::span_copy;
using std::span;

// Ed25519 wire-format serialization

auto serialize_ed25519_key_exchange(Ed25519KeyExchangePdu const& pdu, span<uint8_t, ed25519_key_exchange_pdu_wire_size> out) -> void
{
    size_t offset = 0;
    out[offset++] = pdu.message_type;
    span_copy(out.subspan(offset, Ed25519PublicKey::LENGTH), pdu.sender_identity.data);
    offset += Ed25519PublicKey::LENGTH;
    span_copy(out.subspan(offset, X25519PublicKey::LENGTH), pdu.ephemeral_pubkey.data);
    offset += X25519PublicKey::LENGTH;
    span_copy(out.subspan(offset, Ed25519Signature::LENGTH), pdu.signature.data);
}

auto deserialize_ed25519_key_exchange(span<uint8_t const, ed25519_key_exchange_pdu_wire_size> in)
    -> std::optional<Ed25519KeyExchangePdu>
{
    auto raw_type = in[0];
    if (raw_type < key_exchange_offer || raw_type > key_exchange_accept) {
        return std::nullopt;
    }

    Ed25519KeyExchangePdu pdu{};
    size_t offset = 0;
    pdu.message_type = in[offset++];
    span_copy(pdu.sender_identity.data, in.subspan(offset, Ed25519PublicKey::LENGTH));
    offset += Ed25519PublicKey::LENGTH;
    span_copy(pdu.ephemeral_pubkey.data, in.subspan(offset, X25519PublicKey::LENGTH));
    offset += X25519PublicKey::LENGTH;
    span_copy(pdu.signature.data, in.subspan(offset, Ed25519Signature::LENGTH));
    return pdu;
}

// P-256 wire-format serialization

auto serialize_p256_key_exchange(P256KeyExchangePdu const& pdu, span<uint8_t, p256_key_exchange_pdu_wire_size> out) -> void
{
    size_t offset = 0;
    out[offset++] = pdu.message_type;
    span_copy(out.subspan(offset, P256PublicKey::LENGTH), pdu.sender_identity.data);
    offset += P256PublicKey::LENGTH;
    span_copy(out.subspan(offset, P256PublicKey::LENGTH), pdu.ephemeral_pubkey.data);
    offset += P256PublicKey::LENGTH;
    span_copy(out.subspan(offset, P256EcdsaSignature::LENGTH), pdu.signature.data);
}

auto deserialize_p256_key_exchange(span<uint8_t const, p256_key_exchange_pdu_wire_size> in) -> std::optional<P256KeyExchangePdu>
{
    auto raw_type = in[0];
    if (raw_type < key_exchange_offer || raw_type > key_exchange_accept) {
        return std::nullopt;
    }

    P256KeyExchangePdu pdu{};
    size_t offset = 0;
    pdu.message_type = in[offset++];
    span_copy(pdu.sender_identity.data, in.subspan(offset, P256PublicKey::LENGTH));
    offset += P256PublicKey::LENGTH;
    span_copy(pdu.ephemeral_pubkey.data, in.subspan(offset, P256PublicKey::LENGTH));
    offset += P256PublicKey::LENGTH;
    span_copy(pdu.signature.data, in.subspan(offset, P256EcdsaSignature::LENGTH));
    return pdu;
}

// AUTH_GET_NONCE wire-format serialization

auto serialize_auth_get_nonce(AuthGetNoncePayload const& payload, span<uint8_t, auth_get_nonce_payload_size> out) -> void
{
    span_copy(out.subspan(0, nonce_size), payload.controller_nonce.data);
}

auto deserialize_auth_get_nonce(span<uint8_t const, auth_get_nonce_payload_size> in) -> AuthGetNoncePayload
{
    AuthGetNoncePayload payload{};
    span_copy(payload.controller_nonce.data, in.subspan(0, nonce_size));
    return payload;
}

auto serialize_auth_get_nonce_response(
    AuthGetNonceResponsePayload const& payload, span<uint8_t, auth_get_nonce_response_payload_size> out) -> void
{
    span_copy(out.subspan(0, nonce_size), payload.controller_nonce.data);
    span_copy(out.subspan(nonce_size, nonce_size), payload.target_nonce.data);
}

auto deserialize_auth_get_nonce_response(span<uint8_t const, auth_get_nonce_response_payload_size> in)
    -> AuthGetNonceResponsePayload
{
    AuthGetNonceResponsePayload payload{};
    span_copy(payload.controller_nonce.data, in.subspan(0, nonce_size));
    span_copy(payload.target_nonce.data, in.subspan(nonce_size, nonce_size));
    return payload;
}

// AUTH_ADD_KEY_NONCE wire-format serialization

auto serialize_auth_add_key_nonce_header(AuthAddKeyNonceHeader const& header, span<uint8_t, auth_add_key_nonce_header_size> out)
    -> void
{
    size_t offset = 0;
    span_copy(out.subspan(offset, nonce_size), header.controller_nonce.data);
    offset += nonce_size;
    span_copy(out.subspan(offset, nonce_size), header.target_nonce.data);
    offset += nonce_size;
    span_copy(out.subspan(offset, key_id_size), header.key_id.data);
    offset += key_id_size;
    // key_type (4 bits) | key_length high (4 bits)
    out[offset++] = static_cast<uint8_t>((static_cast<uint8_t>(header.key_type) << 4) | ((header.key_length >> 8) & 0x0F));
    // key_length low (8 bits)
    out[offset++] = static_cast<uint8_t>(header.key_length & 0xFF);
    // reserved (16 bits, zero)
    out[offset++] = 0;
    out[offset] = 0;
}

auto deserialize_auth_add_key_nonce_header(span<uint8_t const, auth_add_key_nonce_header_size> in)
    -> std::optional<AuthAddKeyNonceHeader>
{
    AuthAddKeyNonceHeader header{};
    size_t offset = 0;
    span_copy(header.controller_nonce.data, in.subspan(offset, nonce_size));
    offset += nonce_size;
    span_copy(header.target_nonce.data, in.subspan(offset, nonce_size));
    offset += nonce_size;
    span_copy(header.key_id.data, in.subspan(offset, key_id_size));
    offset += key_id_size;
    // key_type[15:12] | key_length[11:0]
    ieee::doublet_t key_field{};
    span_load(key_field, in.subspan(offset, sizeof(key_field)));
    header.key_type = static_cast<KeyType>(key_field.get_bits<uint8_t>(0xF000, 12));
    header.key_length = key_field.get_bits<uint16_t>(0x0FFF);
    offset += 2;
    // Skip 2 reserved bytes (no validation needed)
    return header;
}

auto serialize_auth_add_key_nonce_response(
    AuthAddKeyNonceResponsePayload const& payload, span<uint8_t, auth_add_key_nonce_response_payload_size> out) -> void
{
    size_t offset = 0;
    span_copy(out.subspan(offset, nonce_size), payload.controller_nonce.data);
    offset += nonce_size;
    span_copy(out.subspan(offset, nonce_size), payload.target_nonce.data);
    offset += nonce_size;
    span_copy(out.subspan(offset, key_id_size), payload.key_id.data);
}

auto deserialize_auth_add_key_nonce_response(span<uint8_t const, auth_add_key_nonce_response_payload_size> in)
    -> AuthAddKeyNonceResponsePayload
{
    AuthAddKeyNonceResponsePayload payload{};
    size_t offset = 0;
    span_copy(payload.controller_nonce.data, in.subspan(offset, nonce_size));
    offset += nonce_size;
    span_copy(payload.target_nonce.data, in.subspan(offset, nonce_size));
    offset += nonce_size;
    span_copy(payload.key_id.data, in.subspan(offset, key_id_size));
    return payload;
}

// Wrapped key wire-format serialization

auto serialize_aes128_wrapped_key(Aes128WrappedKey const& wk, span<uint8_t, aes128_wrapped_key_size> out) -> void
{
    span_copy(out.subspan(0, Aes128Key::LENGTH), wk.ciphertext);
    span_copy(out.subspan(Aes128Key::LENGTH, aes_block_size), wk.siv_tag);
}

auto deserialize_aes128_wrapped_key(span<uint8_t const, aes128_wrapped_key_size> in) -> Aes128WrappedKey
{
    Aes128WrappedKey wk{};
    span_copy(wk.ciphertext, in.subspan(0, Aes128Key::LENGTH));
    span_copy(wk.siv_tag, in.subspan(Aes128Key::LENGTH, aes_block_size));
    return wk;
}

auto serialize_aes256_wrapped_key(Aes256WrappedKey const& wk, span<uint8_t, aes256_wrapped_key_size> out) -> void
{
    span_copy(out.subspan(0, Aes256Key::LENGTH), wk.ciphertext);
    span_copy(out.subspan(Aes256Key::LENGTH, aes_block_size), wk.siv_tag);
}

auto deserialize_aes256_wrapped_key(span<uint8_t const, aes256_wrapped_key_size> in) -> Aes256WrappedKey
{
    Aes256WrappedKey wk{};
    span_copy(wk.ciphertext, in.subspan(0, Aes256Key::LENGTH));
    span_copy(wk.siv_tag, in.subspan(Aes256Key::LENGTH, aes_block_size));
    return wk;
}

auto serialize_aes128_siv_wrapped_key(Aes128SivWrappedKey const& wk, span<uint8_t, aes128_siv_wrapped_key_size> out) -> void
{
    span_copy(out.subspan(0, Aes128SivKey::LENGTH), wk.ciphertext);
    span_copy(out.subspan(Aes128SivKey::LENGTH, aes_block_size), wk.siv_tag);
}

auto deserialize_aes128_siv_wrapped_key(span<uint8_t const, aes128_siv_wrapped_key_size> in) -> Aes128SivWrappedKey
{
    Aes128SivWrappedKey wk{};
    span_copy(wk.ciphertext, in.subspan(0, Aes128SivKey::LENGTH));
    span_copy(wk.siv_tag, in.subspan(Aes128SivKey::LENGTH, aes_block_size));
    return wk;
}

auto serialize_aes256_siv_wrapped_key(Aes256SivWrappedKey const& wk, span<uint8_t, aes256_siv_wrapped_key_size> out) -> void
{
    span_copy(out.subspan(0, Aes256SivKey::LENGTH), wk.ciphertext);
    span_copy(out.subspan(Aes256SivKey::LENGTH, aes_block_size), wk.siv_tag);
}

auto deserialize_aes256_siv_wrapped_key(span<uint8_t const, aes256_siv_wrapped_key_size> in) -> Aes256SivWrappedKey
{
    Aes256SivWrappedKey wk{};
    span_copy(wk.ciphertext, in.subspan(0, Aes256SivKey::LENGTH));
    span_copy(wk.siv_tag, in.subspan(Aes256SivKey::LENGTH, aes_block_size));
    return wk;
}

// AEF encrypt (IEEE 1722-2016 Clause 13)

auto aef_encrypt(
    uint8_t enc,
    SessionKeyChain const& keychain,
    KeyId const& key_id,
    span<uint8_t const> plaintext,
    span<uint8_t> encrypted_payload_out,
    span<uint8_t const> aad,
    AefNonceCounter* nonce_counter) -> std::optional<span<uint8_t const>>
{
    // Look up the key
    auto entry_opt = find_key_entry(keychain, key_id);
    if (!entry_opt) {
        return std::nullopt;
    }
    auto const& entry_ref = entry_opt->get();

    if (enc == aef_enc_aes_siv) {
        // enc=0: AES-SIV — encrypted_payload = siv_tag(16) || ciphertext
        size_t const output_size = aef_siv_overhead + plaintext.size();
        if (encrypted_payload_out.size() < output_size) {
            return std::nullopt;
        }

        // Copy plaintext to ciphertext region (after 16-byte tag slot)
        auto ciphertext_region = encrypted_payload_out.subspan(aef_siv_overhead, plaintext.size());
        span_copy(ciphertext_region, plaintext);

        // Dispatch based on key type
        auto siv_tag_opt = std::visit(
            [&](auto const& entry) -> std::optional<std::array<uint8_t, 16>> {
                using T = std::decay_t<decltype(entry)>;
                if constexpr (std::is_same_v<T, Aes128SivKeyEntry>) {
                    return aes128_siv_encrypt(entry.key, ciphertext_region, aad);
                } else if constexpr (std::is_same_v<T, Aes256SivKeyEntry>) {
                    return aes256_siv_encrypt(entry.key, ciphertext_region, aad);
                } else {
                    return std::nullopt;
                }
            },
            entry_ref);

        if (!siv_tag_opt) {
            internal::secure_zero(encrypted_payload_out.subspan(0, output_size));
            return std::nullopt;
        }

        // Write SIV tag into first 16 bytes
        span_copy(encrypted_payload_out.subspan(0, aef_siv_overhead), *siv_tag_opt);
        return span<uint8_t const>(encrypted_payload_out.data(), output_size);
    }

    if (enc == aef_enc_aes_gcm_siv) {
        // enc=1: AES-GCM-SIV — encrypted_payload = nonce(12) || ciphertext || tag(16)
        if (nonce_counter == nullptr) {
            return std::nullopt;
        }

        size_t const output_size = aef_gcm_siv_overhead + plaintext.size();
        if (encrypted_payload_out.size() < output_size) {
            return std::nullopt;
        }

        // Copy current nonce to output header
        span_copy(encrypted_payload_out.subspan(0, aes_gcm_siv_nonce_size), *nonce_counter);

        // Copy plaintext to ciphertext region (after nonce)
        auto ciphertext_region = encrypted_payload_out.subspan(aes_gcm_siv_nonce_size, plaintext.size());
        span_copy(ciphertext_region, plaintext);

        // Create fixed-extent nonce span from the output
        auto nonce_span = span<uint8_t const, aes_gcm_siv_nonce_size>(encrypted_payload_out.data(), aes_gcm_siv_nonce_size);

        // Dispatch based on key type
        auto tag_opt = std::visit(
            [&](auto const& entry) -> std::optional<std::array<uint8_t, aes_gcm_siv_tag_size>> {
                using T = std::decay_t<decltype(entry)>;
                if constexpr (std::is_same_v<T, Aes128KeyEntry>) {
                    return aes128_gcm_siv_encrypt(entry.key, nonce_span, ciphertext_region, aad);
                } else if constexpr (std::is_same_v<T, Aes256KeyEntry>) {
                    return aes256_gcm_siv_encrypt(entry.key, nonce_span, ciphertext_region, aad);
                } else {
                    return std::nullopt;
                }
            },
            entry_ref);

        if (!tag_opt) {
            internal::secure_zero(encrypted_payload_out.subspan(0, output_size));
            return std::nullopt;
        }

        // Write tag after ciphertext
        span_copy(encrypted_payload_out.subspan(aes_gcm_siv_nonce_size + plaintext.size(), aes_gcm_siv_tag_size), *tag_opt);

        // Increment nonce counter (big-endian, least-significant byte last)
        for (int i = 11; i >= 0; --i) {
            if (++(*nonce_counter)[static_cast<size_t>(i)] != 0) {
                break;
            }
        }

        return span<uint8_t const>(encrypted_payload_out.data(), output_size);
    }

    // Unknown enc value
    return std::nullopt;
}

// AEF decrypt (IEEE 1722-2016 Clause 13)

auto aef_decrypt(
    uint8_t enc,
    SessionKeyChain const& keychain,
    KeyId const& key_id,
    span<uint8_t const> encrypted_payload,
    span<uint8_t> plaintext_out,
    span<uint8_t const> aad) -> std::optional<span<uint8_t const>>
{
    // Look up the key
    auto entry_opt = find_key_entry(keychain, key_id);
    if (!entry_opt) {
        return std::nullopt;
    }
    auto const& entry_ref = entry_opt->get();

    if (enc == aef_enc_aes_siv) {
        // enc=0: AES-SIV — encrypted_payload = siv_tag(16) || ciphertext
        if (encrypted_payload.size() < aef_siv_overhead) {
            return std::nullopt;
        }
        size_t const ciphertext_size = encrypted_payload.size() - aef_siv_overhead;
        if (plaintext_out.size() < ciphertext_size) {
            return std::nullopt;
        }

        // Extract SIV tag (first 16 bytes)
        std::array<uint8_t, 16> siv_tag{};
        span_copy(siv_tag, encrypted_payload.subspan(0, aef_siv_overhead));

        // Copy ciphertext to plaintext_out for in-place decryption
        auto plaintext_region = plaintext_out.subspan(0, ciphertext_size);
        span_copy(plaintext_region, encrypted_payload.subspan(aef_siv_overhead, ciphertext_size));

        // Dispatch based on key type
        bool const ok = std::visit(
            [&](auto const& entry) -> bool {
                using T = std::decay_t<decltype(entry)>;
                if constexpr (std::is_same_v<T, Aes128SivKeyEntry>) {
                    return aes128_siv_decrypt(entry.key, plaintext_region, span<uint8_t const, aes128_block_size>(siv_tag), aad);
                } else if constexpr (std::is_same_v<T, Aes256SivKeyEntry>) {
                    return aes256_siv_decrypt(entry.key, plaintext_region, span<uint8_t const, aes256_block_size>(siv_tag), aad);
                } else {
                    return false;
                }
            },
            entry_ref);

        if (!ok) {
            return std::nullopt;
        }
        return span<uint8_t const>(plaintext_out.data(), ciphertext_size);
    }

    if (enc == aef_enc_aes_gcm_siv) {
        // enc=1: AES-GCM-SIV — encrypted_payload = nonce(12) || ciphertext || tag(16)
        if (encrypted_payload.size() < aef_gcm_siv_overhead) {
            return std::nullopt;
        }
        size_t const ciphertext_size = encrypted_payload.size() - aef_gcm_siv_overhead;
        if (plaintext_out.size() < ciphertext_size) {
            return std::nullopt;
        }

        // Extract nonce (first 12 bytes)
        auto nonce_span = span<uint8_t const, aes_gcm_siv_nonce_size>(encrypted_payload.data(), aes_gcm_siv_nonce_size);

        // Extract tag (last 16 bytes)
        std::array<uint8_t, aes_gcm_siv_tag_size> tag{};
        span_copy(tag, encrypted_payload.subspan(aes_gcm_siv_nonce_size + ciphertext_size, aes_gcm_siv_tag_size));
        auto tag_span = span<uint8_t const, aes_gcm_siv_tag_size>(tag);

        // Copy ciphertext to plaintext_out for in-place decryption
        auto plaintext_region = plaintext_out.subspan(0, ciphertext_size);
        span_copy(plaintext_region, encrypted_payload.subspan(aes_gcm_siv_nonce_size, ciphertext_size));

        // Dispatch based on key type
        bool const ok = std::visit(
            [&](auto const& entry) -> bool {
                using T = std::decay_t<decltype(entry)>;
                if constexpr (std::is_same_v<T, Aes128KeyEntry>) {
                    return aes128_gcm_siv_decrypt(entry.key, nonce_span, plaintext_region, tag_span, aad);
                } else if constexpr (std::is_same_v<T, Aes256KeyEntry>) {
                    return aes256_gcm_siv_decrypt(entry.key, nonce_span, plaintext_region, tag_span, aad);
                } else {
                    return false;
                }
            },
            entry_ref);

        if (!ok) {
            return std::nullopt;
        }
        return span<uint8_t const>(plaintext_out.data(), ciphertext_size);
    }

    // Unknown enc value
    return std::nullopt;
}

}  // namespace statusbar::crypto::avtp
