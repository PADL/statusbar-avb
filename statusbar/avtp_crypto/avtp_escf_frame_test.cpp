// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// IEEE 1722-2025 Clause 16 ESCF as the November 2026 amendment reads it: the signed
// message, ECSSA_DSA_EMSA1_SHA256 and ED25519 signature_values, the Listener's
// timestamp window, and complete frames.

#include "statusbar/avtp_crypto/avtp_escf_frame.hpp"

#include "statusbar/crypto/25519/ed25519.hpp"
#include "statusbar/crypto/p256/p256_ecdsa.hpp"
#include "statusbar/crypto/util/crypto_util_internal.hpp"
#include "statusbar/test/test.hpp"

#include <array>

using namespace statusbar::crypto;
using namespace statusbar::crypto::avtp;
using statusbar::crypto::internal::span_compare;
using statusbar::crypto::internal::span_copy;

namespace {

constexpr std::array<uint8_t, 32> kat_seed{
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
};
statusbar::ieee::Eui64 const kat_key_id{0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x00, 0x42};
constexpr uint64_t kat_timestamp = 0x0123456789ABCDEFULL;
constexpr std::array<uint8_t, 16> kat_avtpdu{
    0x10,
    0x11,
    0x12,
    0x13,
    0x14,
    0x15,
    0x16,
    0x17,
    0x18,
    0x19,
    0x1A,
    0x1B,
    0x1C,
    0x1D,
    0x1E,
    0x1F,
};

// The signed message for the inputs above: key_id || signing_timestamp || encapsulated_avtpdu.
constexpr std::array<uint8_t, 32> kat_message{
    0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x00, 0x42, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

// ED25519 known answer from an independent RFC 8032 implementation (the cryptography
// package): the key of seed kat_seed, over kat_message.
constexpr std::array<uint8_t, 32> kat_ed25519_pk{
    0x79, 0xB5, 0x56, 0x2E, 0x8F, 0xE6, 0x54, 0xF9, 0x40, 0x78, 0xB1, 0x12, 0xE8, 0xA9, 0x8B, 0xA7,
    0x90, 0x1F, 0x85, 0x3A, 0xE6, 0x95, 0xBE, 0xD7, 0xE0, 0xE3, 0x91, 0x0B, 0xAD, 0x04, 0x96, 0x64,
};
constexpr EscfSignature kat_ed25519_signature{
    0x60, 0xA4, 0xF0, 0x3C, 0x79, 0x23, 0x33, 0xDE, 0x18, 0x5D, 0x84, 0xB8, 0xF9, 0x8B, 0x8B, 0x84,
    0xE0, 0x20, 0xE2, 0x34, 0x8C, 0x7C, 0xCE, 0xAA, 0xA6, 0x1F, 0x1E, 0xA4, 0xAC, 0x3B, 0x4D, 0x34,
    0x37, 0x56, 0x24, 0xC2, 0xA4, 0xB7, 0xC6, 0x74, 0xE0, 0xA2, 0xCC, 0x4A, 0x52, 0xC6, 0x96, 0x6B,
    0xED, 0x1E, 0x34, 0xB7, 0x72, 0xAC, 0x5A, 0x84, 0x5F, 0x91, 0xCF, 0x88, 0x35, 0x29, 0x45, 0x0F,
};

// ECSSA_DSA_EMSA1_SHA256 known answer: the P-256 key d = SHA-256(kat_seed) mod n over
// kat_message with the RFC 6979 nonce, c then d; verified by an independent ECDSA
// implementation (the cryptography package) in the Python frame test.
constexpr EscfSignature kat_ecssa_signature{
    0x99, 0x3E, 0xBB, 0xB7, 0x4F, 0x57, 0xD1, 0x0E, 0x01, 0xC5, 0x54, 0xE0, 0xF7, 0x06, 0x62, 0xD0,
    0xA5, 0x46, 0xD0, 0xAA, 0x66, 0x7E, 0x83, 0xFB, 0x69, 0x62, 0xFE, 0xE4, 0x3B, 0x96, 0xA6, 0x13,
    0x58, 0x0E, 0x68, 0xBB, 0xA1, 0x7A, 0x98, 0x8C, 0x19, 0x68, 0x39, 0x4B, 0xEC, 0xBF, 0x2A, 0x72,
    0x05, 0xFF, 0x94, 0x46, 0xA2, 0x63, 0xCE, 0xCC, 0xFC, 0x36, 0x3B, 0x8D, 0x38, 0x5B, 0x1C, 0x59,
};

statusbar::ieee::Eui48 const dest_mac{0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x01};
statusbar::ieee::Eui48 const src_mac{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

auto p256_key() -> P256PrivateKey
{
    return p256_ecdsa_keypair_from_seed(kat_seed);
}

auto ed25519_key() -> Ed25519PrivateKey
{
    return ed25519_keypair_from_seed(kat_seed);
}

}  // namespace

TEST(avtp_escf_frame, sizes_and_sig_values)
{
    EXPECT_TRUE(escf_header_size == 84);
    EXPECT_TRUE(escf_payload_offset == 98);
    EXPECT_TRUE(escf_signature_size == 64);
    EXPECT_TRUE(escf_sig_ecssa == 0);
    EXPECT_TRUE(escf_sig_ed25519 == 1);
    EXPECT_TRUE(escf_max_encapsulated_avtpdu_size == 1416);
}

TEST(avtp_escf_frame, signed_message_is_key_id_timestamp_avtpdu)
{
    std::array<uint8_t, 64> out{};
    auto const message = escf_signed_message(kat_key_id, kat_timestamp, kat_avtpdu, out);
    EXPECT_TRUE(message.has_value());
    EXPECT_TRUE(span_compare(*message, std::span<uint8_t const>(kat_message)));

    std::array<uint8_t, 31> too_small{};
    EXPECT_FALSE(escf_signed_message(kat_key_id, kat_timestamp, kat_avtpdu, too_small).has_value());
    std::array<uint8_t, 1417> too_long{};
    std::array<uint8_t, 2048> big{};
    EXPECT_FALSE(escf_signed_message(kat_key_id, kat_timestamp, too_long, big).has_value());
    EXPECT_TRUE(escf_signed_message(kat_key_id, kat_timestamp, std::span<uint8_t const>(too_long).first(1416), big).has_value());
}

TEST(avtp_escf_frame, ed25519_known_answer)
{
    auto const sk = ed25519_key();
    EXPECT_TRUE(span_compare(std::span<uint8_t const>(sk.public_key.data), std::span<uint8_t const>(kat_ed25519_pk)));
    auto const signature = escf_ed25519_sign(sk, kat_key_id, kat_timestamp, kat_avtpdu);
    EXPECT_TRUE(signature.has_value());
    EXPECT_TRUE(span_compare(std::span<uint8_t const>(*signature), std::span<uint8_t const>(kat_ed25519_signature)));
    EXPECT_TRUE(escf_ed25519_verify(sk.public_key, kat_key_id, kat_timestamp, kat_avtpdu, kat_ed25519_signature));
}

TEST(avtp_escf_frame, ed25519_rejects_any_changed_field)
{
    auto const pk = ed25519_key().public_key;
    statusbar::ieee::Eui64 const other_key_id{0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x00, 0x43};
    EXPECT_FALSE(escf_ed25519_verify(pk, other_key_id, kat_timestamp, kat_avtpdu, kat_ed25519_signature));
    EXPECT_FALSE(escf_ed25519_verify(pk, kat_key_id, kat_timestamp + 1, kat_avtpdu, kat_ed25519_signature));
    auto avtpdu = kat_avtpdu;
    avtpdu[5] ^= 0x01;
    EXPECT_FALSE(escf_ed25519_verify(pk, kat_key_id, kat_timestamp, avtpdu, kat_ed25519_signature));
    auto signature = kat_ed25519_signature;
    signature[63] ^= 0x01;
    EXPECT_FALSE(escf_ed25519_verify(pk, kat_key_id, kat_timestamp, kat_avtpdu, signature));
    // S >= L (RFC 8032 5.1.7): a non-canonical S is rejected even if it would otherwise verify.
    signature = kat_ed25519_signature;
    signature[63] |= 0xF0;
    EXPECT_FALSE(escf_ed25519_verify(pk, kat_key_id, kat_timestamp, kat_avtpdu, signature));
}

TEST(avtp_escf_frame, ecssa_signs_deterministically_and_verifies)
{
    auto const sk = p256_key();
    auto const first = escf_ecssa_sign(sk, kat_key_id, kat_timestamp, kat_avtpdu);
    auto const second = escf_ecssa_sign(sk, kat_key_id, kat_timestamp, kat_avtpdu);
    EXPECT_TRUE(first.has_value() && second.has_value());
    EXPECT_TRUE(span_compare(std::span<uint8_t const>(*first), std::span<uint8_t const>(*second)));  // RFC 6979
    EXPECT_TRUE(span_compare(std::span<uint8_t const>(*first), std::span<uint8_t const>(kat_ecssa_signature)));
    EXPECT_TRUE(escf_ecssa_verify(sk.public_key, kat_key_id, kat_timestamp, kat_avtpdu, *first));

    // c and d are the ECDSA r and s: the same signature verifies as one through the primitive.
    P256EcdsaSignature as_ecdsa{};
    span_copy(as_ecdsa.data, std::span<uint8_t const>(*first));
    EXPECT_TRUE(p256_ecdsa_verify(sk.public_key, kat_message, as_ecdsa));

    auto const other = p256_ecdsa_keypair_from_seed(kat_ed25519_pk);  // any other key
    EXPECT_FALSE(escf_ecssa_verify(other.public_key, kat_key_id, kat_timestamp, kat_avtpdu, *first));
    EXPECT_FALSE(escf_ecssa_verify(sk.public_key, kat_key_id, kat_timestamp + 1, kat_avtpdu, *first));
    auto tampered = *first;
    tampered[0] ^= 0x01;
    EXPECT_FALSE(escf_ecssa_verify(sk.public_key, kat_key_id, kat_timestamp, kat_avtpdu, tampered));
    std::array<uint8_t, 1417> too_long{};
    EXPECT_FALSE(escf_ecssa_sign(sk, kat_key_id, kat_timestamp, too_long).has_value());
}

TEST(avtp_escf_frame, timestamp_window)
{
    constexpr uint64_t now = 1'000'000'000'000ULL;
    EXPECT_TRUE(escf_signing_timestamp_is_acceptable(now, now));
    EXPECT_TRUE(escf_signing_timestamp_is_acceptable(now - escf_signing_timestamp_window_ns, now));
    EXPECT_FALSE(escf_signing_timestamp_is_acceptable(now - escf_signing_timestamp_window_ns - 1, now));
    EXPECT_TRUE(escf_signing_timestamp_is_acceptable(now + escf_signing_timestamp_window_ns, now));
    EXPECT_FALSE(escf_signing_timestamp_is_acceptable(now + escf_signing_timestamp_window_ns + 1, now));
    EXPECT_TRUE(escf_signing_timestamp_is_acceptable(escf_signing_timestamp_window_ns, 0));
    EXPECT_FALSE(escf_signing_timestamp_is_acceptable(0, escf_signing_timestamp_window_ns + 1));
    EXPECT_TRUE(escf_signing_timestamp_window_ns == 100'000'000ULL);
}

TEST(avtp_escf_frame, ed25519_frame_roundtrip)
{
    auto const sk = ed25519_key();
    std::array<uint8_t, 256> frame{};
    auto const built = build_signed_escf_frame(frame, dest_mac, src_mac, sk, kat_key_id, kat_timestamp, kat_avtpdu);
    EXPECT_TRUE(built.has_value());
    EXPECT_TRUE(built->size() == escf_payload_offset + kat_avtpdu.size());
    EXPECT_TRUE((*built)[14] == escf_subtype && (*built)[15] == escf_sig_ed25519 && (*built)[16] == 0 && (*built)[17] == 16);
    EXPECT_TRUE(span_compare(built->subspan(18, 8), std::span<uint8_t const>(kat_message).first(8)));
    EXPECT_TRUE(span_compare(built->subspan(26, 8), std::span<uint8_t const>(kat_message).subspan(8, 8)));
    EXPECT_TRUE(span_compare(built->subspan(34, 64), std::span<uint8_t const>(kat_ed25519_signature)));

    auto const parsed = parse_escf_frame(*built);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->sig == escf_sig_ed25519 && parsed->encapsulated_avtpdu_length == 16);
    EXPECT_TRUE(parsed->key_id == kat_key_id && parsed->signing_timestamp == kat_timestamp);
    EXPECT_TRUE(span_compare(parsed->encapsulated_avtpdu, std::span<uint8_t const>(kat_avtpdu)));
    EXPECT_TRUE(escf_verify_frame(*parsed, sk.public_key));
    EXPECT_FALSE(escf_verify_frame(*parsed, p256_key().public_key));  // sig says ED25519
}

TEST(avtp_escf_frame, ecssa_frame_roundtrip)
{
    auto const sk = p256_key();
    std::array<uint8_t, 256> frame{};
    auto const built = build_signed_escf_frame(frame, dest_mac, src_mac, sk, kat_key_id, kat_timestamp, kat_avtpdu);
    EXPECT_TRUE(built.has_value());
    auto const parsed = parse_escf_frame(*built);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->sig == escf_sig_ecssa);
    EXPECT_TRUE(escf_verify_frame(*parsed, sk.public_key));
    EXPECT_FALSE(escf_verify_frame(*parsed, ed25519_key().public_key));

    // Re-labelling the sig value, or touching the AVTPDU, fails validation.
    std::array<uint8_t, 256> altered{};
    span_copy(std::span<uint8_t>(altered).first(built->size()), *built);
    altered[15] = escf_sig_ed25519;
    EXPECT_FALSE(escf_verify_frame(*parse_escf_frame(std::span<uint8_t const>(altered).first(built->size())), sk.public_key));
    altered[15] = escf_sig_ecssa;
    altered[escf_payload_offset] ^= 0x01;
    EXPECT_FALSE(escf_verify_frame(*parse_escf_frame(std::span<uint8_t const>(altered).first(built->size())), sk.public_key));
}

TEST(avtp_escf_frame, parse_bounds_and_rejections)
{
    auto const sk = ed25519_key();
    std::array<uint8_t, 256> frame{};
    auto const built = build_signed_escf_frame(frame, dest_mac, src_mac, sk, kat_key_id, kat_timestamp, kat_avtpdu);

    // Padding after the declared AVTPDU is not part of it (and not signed).
    auto const padded = std::span<uint8_t const>(frame).first(built->size() + 20);
    auto const parsed = parse_escf_frame(padded);
    EXPECT_TRUE(parsed.has_value() && parsed->encapsulated_avtpdu.size() == 16);
    EXPECT_TRUE(escf_verify_frame(*parsed, sk.public_key));

    // A declared length past the frame, a short frame, a wrong ethertype or subtype.
    frame[17] = 200;
    EXPECT_FALSE(parse_escf_frame(padded).has_value());
    frame[17] = 16;
    EXPECT_FALSE(parse_escf_frame(std::span<uint8_t const>(frame).first(escf_payload_offset - 1)).has_value());
    frame[13] = 0x00;
    EXPECT_FALSE(parse_escf_frame(padded).has_value());
    frame[13] = 0xF0;
    frame[14] = 0xED;
    EXPECT_FALSE(parse_escf_frame(padded).has_value());

    // Build limits: the buffer and 16.2.6's AVTPDU size.
    std::array<uint8_t, escf_payload_offset + 15> too_small{};
    EXPECT_FALSE(build_signed_escf_frame(too_small, dest_mac, src_mac, sk, kat_key_id, kat_timestamp, kat_avtpdu).has_value());
    std::array<uint8_t, 1417> too_long{};
    std::array<uint8_t, 2048> big{};
    EXPECT_FALSE(build_signed_escf_frame(big, dest_mac, src_mac, sk, kat_key_id, kat_timestamp, too_long).has_value());
    EXPECT_TRUE(build_signed_escf_frame(
                    big, dest_mac, src_mac, sk, kat_key_id, kat_timestamp, std::span<uint8_t const>(too_long).first(1416))
                    .has_value());
}

namespace {

/// A signed Ed25519 frame of the KAT inputs with the given signing_timestamp, parsed.
struct SignedFrame
{
    std::array<uint8_t, 256> storage{};
    ParsedEscfFrame parsed{};
};

auto signed_frame(Ed25519PrivateKey const& sk, uint64_t signing_timestamp, statusbar::ieee::Eui64 const& key_id = kat_key_id)
    -> SignedFrame
{
    SignedFrame f{};
    auto const built = build_signed_escf_frame(f.storage, dest_mac, src_mac, sk, key_id, signing_timestamp, kat_avtpdu);
    f.parsed = *parse_escf_frame(*built);
    return f;
}

}  // namespace

// The three gates, in the order they run: a frame signed outside the window is refused
// before any signature arithmetic and leaves no trace in the guard.
TEST(avtp_escf_frame, told_verify_refuses_a_stale_timestamp_before_the_signature)
{
    auto const sk = ed25519_key();
    EscfReplayGuard guard;
    auto const frame = signed_frame(sk, kat_timestamp);
    EXPECT_TRUE(
        escf_verify_frame_at(frame.parsed, sk.public_key, kat_timestamp + escf_signing_timestamp_window_ns + 1, guard) ==
        EscfVerdict::stale_timestamp);
    EXPECT_TRUE(
        escf_verify_frame_at(frame.parsed, sk.public_key, kat_timestamp - escf_signing_timestamp_window_ns - 1, guard) ==
        EscfVerdict::stale_timestamp);
    EXPECT_EQ(guard.signers(), 0U);
    // At the window's edge, with the Listener a little ahead or behind, it is accepted.
    EXPECT_TRUE(
        escf_verify_frame_at(frame.parsed, sk.public_key, kat_timestamp + escf_signing_timestamp_window_ns, guard) ==
        EscfVerdict::accepted);
    EXPECT_EQ(guard.signers(), 1U);
}

TEST(avtp_escf_frame, told_verify_refuses_a_replay_and_a_frame_overtaken_by_a_later_one)
{
    auto const sk = ed25519_key();
    EscfReplayGuard guard;
    auto const first = signed_frame(sk, kat_timestamp);
    auto const second = signed_frame(sk, kat_timestamp + 1'000'000);  // signed 1 ms later
    uint64_t const now = kat_timestamp + 2'000'000;

    EXPECT_TRUE(escf_verify_frame_at(first.parsed, sk.public_key, now, guard) == EscfVerdict::accepted);
    EXPECT_TRUE(escf_verify_frame_at(first.parsed, sk.public_key, now, guard) == EscfVerdict::replayed);  // the same frame again
    EXPECT_TRUE(escf_verify_frame_at(second.parsed, sk.public_key, now, guard) == EscfVerdict::accepted);
    EXPECT_TRUE(
        escf_verify_frame_at(first.parsed, sk.public_key, now, guard) == EscfVerdict::replayed);  // older than the last accepted
    EXPECT_TRUE(escf_verify_frame_at(second.parsed, sk.public_key, now, guard) == EscfVerdict::replayed);
    EXPECT_EQ(guard.signers(), 1U);  // one key_id throughout
}

TEST(avtp_escf_frame, told_verify_refuses_a_bad_signature_and_does_not_record_it)
{
    auto const sk = ed25519_key();
    EscfReplayGuard guard;
    auto frame = signed_frame(sk, kat_timestamp);
    frame.storage[escf_payload_offset] ^= 0x01;  // touch the AVTPDU
    auto const altered = *parse_escf_frame(std::span<uint8_t const>(frame.storage).first(escf_payload_offset + kat_avtpdu.size()));
    EXPECT_TRUE(escf_verify_frame_at(altered, sk.public_key, kat_timestamp, guard) == EscfVerdict::bad_signature);
    EXPECT_TRUE(
        escf_verify_frame_at(altered, p256_key().public_key, kat_timestamp, guard) == EscfVerdict::bad_signature);  // wrong variant
    EXPECT_EQ(guard.signers(), 0U);
    // The genuine frame at the same timestamp is still accepted: the forgery recorded nothing.
    frame.storage[escf_payload_offset] ^= 0x01;
    auto const genuine = *parse_escf_frame(std::span<uint8_t const>(frame.storage).first(escf_payload_offset + kat_avtpdu.size()));
    EXPECT_TRUE(escf_verify_frame_at(genuine, sk.public_key, kat_timestamp, guard) == EscfVerdict::accepted);
}

TEST(avtp_escf_frame, told_verify_works_for_the_ecssa_variant_too)
{
    auto const sk = p256_key();
    EscfReplayGuard guard;
    std::array<uint8_t, 256> storage{};
    auto const built = build_signed_escf_frame(storage, dest_mac, src_mac, sk, kat_key_id, kat_timestamp, kat_avtpdu);
    auto const parsed = *parse_escf_frame(*built);
    EXPECT_TRUE(escf_verify_frame_at(parsed, sk.public_key, kat_timestamp, guard) == EscfVerdict::accepted);
    EXPECT_TRUE(escf_verify_frame_at(parsed, sk.public_key, kat_timestamp, guard) == EscfVerdict::replayed);
    EXPECT_TRUE(
        escf_verify_frame_at(parsed, ed25519_key().public_key, kat_timestamp + 1, guard) ==
        EscfVerdict::replayed);  // the guard runs before the key is consulted
}

TEST(avtp_escf_replay_guard, keeps_one_timestamp_per_signer_and_evicts_the_oldest)
{
    EscfReplayGuard guard;
    // max_signers distinct keys, the i-th last seen at t + i.
    for (size_t i = 0; i < EscfReplayGuard::max_signers; ++i) {
        statusbar::ieee::Eui64 key{0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x01, static_cast<uint8_t>(i)};
        guard.accept(key, kat_timestamp + i);
        EXPECT_TRUE(guard.is_replay(key, kat_timestamp + i));
        EXPECT_FALSE(guard.is_replay(key, kat_timestamp + i + 1));
    }
    EXPECT_EQ(guard.signers(), EscfReplayGuard::max_signers);
    // A known signer's newer frame updates its slot in place.
    statusbar::ieee::Eui64 const fifth{0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x01, 0x05};
    guard.accept(fifth, kat_timestamp + 100);
    EXPECT_EQ(guard.signers(), EscfReplayGuard::max_signers);
    EXPECT_TRUE(guard.is_replay(fifth, kat_timestamp + 99));
    // One more signer evicts the one with the oldest last frame (key 0 at t + 0), no other.
    statusbar::ieee::Eui64 const newcomer{0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x02, 0x00};
    guard.accept(newcomer, kat_timestamp + 200);
    statusbar::ieee::Eui64 const zeroth{0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x01, 0x00};
    statusbar::ieee::Eui64 const first{0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x01, 0x01};
    EXPECT_FALSE(guard.is_replay(zeroth, kat_timestamp));    // forgotten
    EXPECT_TRUE(guard.is_replay(first, kat_timestamp + 1));  // kept
    EXPECT_TRUE(guard.is_replay(newcomer, kat_timestamp + 200));
    EXPECT_EQ(guard.signers(), EscfReplayGuard::max_signers);
}

TEST_MAIN(statusbar_avtp_crypto, avtp_escf_frame_test)
