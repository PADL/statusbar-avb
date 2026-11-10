// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Tests for IEEE 1722.1 AECP AEM frame builder/parser and EECF (Clause 17):
//   - AUTH_GET_NONCE command/response frame roundtrips
//   - AUTH_ADD_KEY_NONCE command/response frame roundtrips
//   - EECF encrypted control frame roundtrip
//   - ECC1 encrypted_payload order V || T || C (IEEE 1722-2025 17.3.2), with
//     known answers from an independent IEEE 1363a-2004 reference
//   - control_data_length validation
//   - Parser rejection of invalid frames

#include "statusbar/avtp_crypto/avtp_aem_frame.hpp"

#include "statusbar/crypto/p256/p256_ecdsa.hpp"
#include "statusbar/crypto/util/crypto_util_internal.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

using namespace statusbar::crypto;
using namespace statusbar::crypto::avtp;
using statusbar::crypto::internal::span_compare;

// ===========================================================================
// Test data
// ===========================================================================

// clang-format off
static constexpr statusbar::ieee::Eui48 test_controller_mac{0x70, 0xb3, 0xd5, 0xed, 0xcf, 0xf0};
static constexpr statusbar::ieee::Eui48 test_talker_mac    {0x70, 0xb3, 0xd5, 0xed, 0xcf, 0xf1};

static constexpr statusbar::ieee::Eui64 test_controller_eid{0x70, 0xb3, 0xd5, 0xff, 0xfe, 0xed, 0xcf, 0xf0};
static constexpr statusbar::ieee::Eui64 test_talker_eid    {0x70, 0xb3, 0xd5, 0xff, 0xfe, 0xed, 0xcf, 0xf1};

static constexpr Nonce test_controller_nonce = {{{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7}}};
static constexpr Nonce test_target_nonce     = {{{0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7}}};
static constexpr KeyId test_key_id           = {{{0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7}}};
// clang-format on

// ===========================================================================
// AUTH_GET_NONCE frame roundtrip
// ===========================================================================

TEST(avtp_aem_frame, auth_get_nonce_frame_roundtrip)
{
    std::array<uint8_t, 128> buf{};
    auto result = build_auth_get_nonce_frame(
        buf, test_controller_mac, test_talker_mac, test_talker_eid, test_controller_eid, 42, test_controller_nonce);

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(result->size() == 46);

    auto parsed = parse_aem_frame(*result);
    EXPECT_TRUE(parsed.has_value());

    EXPECT_TRUE(parsed->dest_mac == test_controller_mac);
    EXPECT_TRUE(parsed->src_mac == test_talker_mac);
    EXPECT_TRUE(parsed->ethertype == avtp_ethertype);
    EXPECT_TRUE(parsed->subtype == aecp_subtype);
    EXPECT_TRUE(parsed->message_type == aecp_message_type_aem_command);
    EXPECT_TRUE(parsed->status == aem_status_success);
    EXPECT_TRUE(parsed->control_data_length == 20);
    EXPECT_TRUE(parsed->target_entity_id == test_talker_eid);
    EXPECT_TRUE(parsed->controller_entity_id == test_controller_eid);
    EXPECT_TRUE(parsed->sequence_id == 42);
    EXPECT_TRUE(parsed->command_code == aem_cmd_auth_get_nonce);

    auto nonce_opt = extract_auth_get_nonce(*parsed);
    EXPECT_TRUE(nonce_opt.has_value());
    EXPECT_TRUE(span_compare(nonce_opt->data, test_controller_nonce.data));
}

// ===========================================================================
// AUTH_GET_NONCE response frame roundtrip
// ===========================================================================

TEST(avtp_aem_frame, auth_get_nonce_response_frame_roundtrip)
{
    std::array<uint8_t, 128> buf{};
    auto result = build_auth_get_nonce_response_frame(
        buf,
        test_controller_mac,
        test_talker_mac,
        test_talker_eid,
        test_controller_eid,
        42,
        aem_status_success,
        test_controller_nonce,
        test_target_nonce);

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(result->size() == 54);

    auto parsed = parse_aem_frame(*result);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->message_type == aecp_message_type_aem_response);
    EXPECT_TRUE(parsed->control_data_length == 28);
    EXPECT_TRUE(parsed->command_code == aem_cmd_auth_get_nonce);
    EXPECT_TRUE(parsed->sequence_id == 42);

    auto resp_opt = extract_auth_get_nonce_response(*parsed);
    EXPECT_TRUE(resp_opt.has_value());
    EXPECT_TRUE(span_compare(resp_opt->controller_nonce.data, test_controller_nonce.data));
    EXPECT_TRUE(span_compare(resp_opt->target_nonce.data, test_target_nonce.data));
}

// ===========================================================================
// AUTH_ADD_KEY_NONCE frame roundtrip (variable-length ciphertext)
// ===========================================================================

TEST(avtp_aem_frame, auth_add_key_nonce_frame_roundtrip)
{
    // Dummy ciphertext (64 bytes)
    std::array<uint8_t, 64> ciphertext{};
    for (size_t i = 0; i < ciphertext.size(); ++i) {
        ciphertext[i] = static_cast<uint8_t>(i);
    }

    std::array<uint8_t, 256> buf{};
    auto result = build_auth_add_key_nonce_frame(
        buf, test_controller_mac, test_talker_mac, test_talker_eid, test_controller_eid, 7, ciphertext);

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(result->size() == 38 + 64);

    auto parsed = parse_aem_frame(*result);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->message_type == aecp_message_type_aem_command);
    EXPECT_TRUE(parsed->control_data_length == 12 + 64);
    EXPECT_TRUE(parsed->command_code == aem_cmd_auth_add_key_nonce);
    EXPECT_TRUE(parsed->sequence_id == 7);

    auto ct = extract_auth_add_key_nonce_ciphertext(*parsed);
    EXPECT_TRUE(ct.size() == 64);
    EXPECT_TRUE(span_compare(ct, std::span<uint8_t const>(ciphertext)));
}

// ===========================================================================
// AUTH_ADD_KEY_NONCE response frame roundtrip
// ===========================================================================

TEST(avtp_aem_frame, auth_add_key_nonce_response_frame_roundtrip)
{
    std::array<uint8_t, 128> buf{};
    auto result = build_auth_add_key_nonce_response_frame(
        buf,
        test_controller_mac,
        test_talker_mac,
        test_talker_eid,
        test_controller_eid,
        99,
        aem_status_success,
        test_controller_nonce,
        test_target_nonce,
        test_key_id);

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(result->size() == 62);

    auto parsed = parse_aem_frame(*result);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->message_type == aecp_message_type_aem_response);
    EXPECT_TRUE(parsed->control_data_length == 36);
    EXPECT_TRUE(parsed->command_code == aem_cmd_auth_add_key_nonce);
    EXPECT_TRUE(parsed->sequence_id == 99);

    auto resp_opt = extract_auth_add_key_nonce_response(*parsed);
    EXPECT_TRUE(resp_opt.has_value());
    EXPECT_TRUE(span_compare(resp_opt->controller_nonce.data, test_controller_nonce.data));
    EXPECT_TRUE(span_compare(resp_opt->target_nonce.data, test_target_nonce.data));
    EXPECT_TRUE(span_compare(resp_opt->key_id.data, test_key_id.data));
}

// ===========================================================================
// control_data_length validation
// ===========================================================================

TEST(avtp_aem_frame, control_data_length_values)
{
    std::array<uint8_t, 256> buf{};

    // AUTH_GET_NONCE command: cdl = 12 + 8 = 20
    {
        auto r = build_auth_get_nonce_frame(
            buf, test_controller_mac, test_talker_mac, test_talker_eid, test_controller_eid, 0, test_controller_nonce);
        EXPECT_TRUE(r.has_value());
        auto p = parse_aem_frame(*r);
        EXPECT_TRUE(p.has_value() && p->control_data_length == 20);
    }

    // AUTH_GET_NONCE response: cdl = 12 + 16 = 28
    {
        auto r = build_auth_get_nonce_response_frame(
            buf,
            test_talker_mac,
            test_controller_mac,
            test_controller_eid,
            test_talker_eid,
            0,
            aem_status_success,
            test_controller_nonce,
            test_target_nonce);
        EXPECT_TRUE(r.has_value());
        auto p = parse_aem_frame(*r);
        EXPECT_TRUE(p.has_value() && p->control_data_length == 28);
    }

    // AUTH_ADD_KEY_NONCE command: cdl = 12 + N (test with 32 bytes)
    {
        std::array<uint8_t, 32> ct{};
        auto r =
            build_auth_add_key_nonce_frame(buf, test_controller_mac, test_talker_mac, test_talker_eid, test_controller_eid, 0, ct);
        EXPECT_TRUE(r.has_value());
        auto p = parse_aem_frame(*r);
        EXPECT_TRUE(p.has_value() && p->control_data_length == 44);
    }

    // AUTH_ADD_KEY_NONCE response: cdl = 12 + 24 = 36
    {
        auto r = build_auth_add_key_nonce_response_frame(
            buf,
            test_talker_mac,
            test_controller_mac,
            test_controller_eid,
            test_talker_eid,
            0,
            aem_status_success,
            test_controller_nonce,
            test_target_nonce,
            test_key_id);
        EXPECT_TRUE(r.has_value());
        auto p = parse_aem_frame(*r);
        EXPECT_TRUE(p.has_value() && p->control_data_length == 36);
    }
}

// ===========================================================================
// Parser rejection tests
// ===========================================================================

TEST(avtp_aem_frame, parse_rejects_short_frame)
{
    std::array<uint8_t, 37> short_frame{};
    auto result = parse_aem_frame(short_frame);
    EXPECT_TRUE(!result.has_value());
}

TEST(avtp_aem_frame, parse_rejects_wrong_ethertype)
{
    std::array<uint8_t, 64> frame{};
    // Set IPv4 ethertype at offset 12
    frame[12] = 0x08;
    frame[13] = 0x00;
    auto result = parse_aem_frame(frame);
    EXPECT_TRUE(!result.has_value());
}

TEST(avtp_aem_frame, parse_rejects_wrong_subtype)
{
    std::array<uint8_t, 64> frame{};
    // Set correct AVTP ethertype
    frame[12] = 0x22;
    frame[13] = 0xF0;
    // Set ADP subtype (0xFA) instead of AECP (0xFB)
    frame[14] = 0xFA;
    auto result = parse_aem_frame(frame);
    EXPECT_TRUE(!result.has_value());
}

// ===========================================================================
// Buffer too small test
// ===========================================================================

TEST(avtp_aem_frame, build_rejects_small_buffer)
{
    // AUTH_GET_NONCE needs 46 bytes; provide only 40
    std::array<uint8_t, 40> small_buf{};
    auto result = build_auth_get_nonce_frame(
        small_buf, test_controller_mac, test_talker_mac, test_talker_eid, test_controller_eid, 0, test_controller_nonce);
    EXPECT_TRUE(!result.has_value());
}

// ===========================================================================
// EECF frame roundtrip (IEEE 1722-2025 Clause 17)
// ===========================================================================

TEST(avtp_aem_frame, eecf_frame_roundtrip)
{
    // Dummy encrypted payload (96 bytes — typical ECIES output)
    std::array<uint8_t, 96> enc_payload{};
    for (size_t i = 0; i < enc_payload.size(); ++i) {
        enc_payload[i] = static_cast<uint8_t>(i + 0x10);
    }

    std::array<uint8_t, 256> buf{};
    auto result = build_eecf_frame(buf, test_controller_mac, test_talker_mac, eecf_enc_ecc1, test_talker_eid, enc_payload);

    EXPECT_TRUE(result.has_value());
    // 14 (ethernet) + 12 (eecf header) + 96 (payload) = 122
    EXPECT_TRUE(result->size() == 122);

    auto parsed = parse_eecf_frame(*result);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->dest_mac == test_controller_mac);
    EXPECT_TRUE(parsed->src_mac == test_talker_mac);
    EXPECT_TRUE(parsed->ethertype == avtp_ethertype);
    EXPECT_TRUE(parsed->subtype == eecf_subtype);
    EXPECT_TRUE(parsed->enc == eecf_enc_ecc1);
    EXPECT_TRUE(parsed->encrypted_payload_length == 96);
    EXPECT_TRUE(parsed->key_id == test_talker_eid);
    EXPECT_TRUE(parsed->encrypted_payload.size() == 96);
    EXPECT_TRUE(span_compare(parsed->encrypted_payload, std::span<uint8_t const>(enc_payload)));
}

// ===========================================================================
// Declared-length bounding (crypto#1, #2): the parsed payload must be bounded by
// the header's declared length, not the frame extent (padding / trailing bytes),
// and an over-claiming length must be rejected before a consumer can trust it.
// ===========================================================================

TEST(avtp_aem_frame, parse_aem_bounds_payload_by_control_data_length)
{
    std::array<uint8_t, 128> buf{};
    auto built = build_auth_get_nonce_frame(
        buf, test_controller_mac, test_talker_mac, test_talker_eid, test_controller_eid, 42, test_controller_nonce);
    EXPECT_TRUE(built.has_value());
    size_t const real_size = built->size();  // 46: 38 header + 8 nonce, control_data_length=20

    // Parse a span with 16 bytes of trailing padding -> payload stays the declared 8
    // bytes (control_data_length 20 - aem_data_length 12), not the padded extent.
    auto padded = std::span<uint8_t const>(buf.data(), real_size + 16);
    auto parsed = parse_aem_frame(padded);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->payload.size() == 8);

    // control_data_length claiming more than the frame holds -> rejected.
    buf[16] = static_cast<uint8_t>((buf[16] & 0xF8) | ((2000U >> 8) & 0x07));
    buf[17] = static_cast<uint8_t>(2000U & 0xFF);
    EXPECT_FALSE(parse_aem_frame(std::span<uint8_t const>(buf.data(), real_size)).has_value());

    // control_data_length below the AEM common header (< aem_data_length) -> rejected.
    buf[16] = static_cast<uint8_t>(buf[16] & 0xF8);
    buf[17] = 5;
    EXPECT_FALSE(parse_aem_frame(std::span<uint8_t const>(buf.data(), real_size)).has_value());
}

TEST(avtp_aem_frame, parse_eecf_bounds_payload_by_declared_length)
{
    std::array<uint8_t, 96> enc_payload{};
    for (size_t i = 0; i < enc_payload.size(); ++i) {
        enc_payload[i] = static_cast<uint8_t>(i + 0x10);
    }
    std::array<uint8_t, 256> buf{};
    auto built = build_eecf_frame(buf, test_controller_mac, test_talker_mac, eecf_enc_ecc1, test_talker_eid, enc_payload);
    EXPECT_TRUE(built.has_value());
    size_t const real_size = built->size();  // 122: 26 header + 96 payload

    // Trailing padding is dropped -> ciphertext stays the declared 96 bytes.
    auto padded = std::span<uint8_t const>(buf.data(), real_size + 16);
    auto parsed = parse_eecf_frame(padded);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->encrypted_payload.size() == 96);

    // encrypted_payload_length claiming more than the frame holds -> rejected.
    buf[16] = static_cast<uint8_t>((buf[16] & 0xF8) | ((2000U >> 8) & 0x07));
    buf[17] = static_cast<uint8_t>(2000U & 0xFF);
    EXPECT_FALSE(parse_eecf_frame(std::span<uint8_t const>(buf.data(), real_size)).has_value());
}

TEST(avtp_aem_frame, eecf_parse_rejects_wrong_subtype)
{
    std::array<uint8_t, 64> frame{};
    frame[12] = 0x22;
    frame[13] = 0xF0;
    frame[14] = 0xFB;  // AECP subtype, not EECF
    auto result = parse_eecf_frame(frame);
    EXPECT_TRUE(!result.has_value());
}

TEST(avtp_aem_frame, eecf_parse_rejects_short_frame)
{
    std::array<uint8_t, 25> short_frame{};  // Less than eecf_payload_offset (26)
    auto result = parse_eecf_frame(short_frame);
    EXPECT_TRUE(!result.has_value());
}

TEST(avtp_aem_frame, eecf_build_rejects_small_buffer)
{
    std::array<uint8_t, 20> small_buf{};  // Too small for even the header
    std::array<uint8_t, 16> payload{};
    auto result = build_eecf_frame(small_buf, test_controller_mac, test_talker_mac, eecf_enc_ecc1, test_talker_eid, payload);
    EXPECT_TRUE(!result.has_value());
}

// ===========================================================================
// ECC1 encrypted_payload (IEEE 1722-2025 17.3.2): V, T, C
//
// Known answers computed by an independent Python reference of IEEE 1363a-2004
// 11.3.2 (P-256 / KDF2-SHA-256 / AES-256-CBC-IV0 / HMAC-SHA-256, DHAES mode,
// EC2OSP-X; K1 = leftmost 256 bits) with the triple serialized as 17.3.2 says.
// Recipient key: d = SHA-256(kat_seed) mod n. Ephemeral key from kat_entropy.
// ===========================================================================

namespace {

constexpr std::array<uint8_t, 32> kat_seed{
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
};

constexpr std::array<uint8_t, 32> kat_entropy{
    0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
};

// encrypted_payload for the plaintext "Hello, DL/ECIES!" (16 octets -> 32-octet C).
constexpr std::array<uint8_t, 97> kat_ecc1_hello{
    0x01, 0xC2, 0x3F, 0x7A, 0x6A, 0x51, 0xAA, 0x50, 0xAE, 0xCD, 0x3A, 0xAA, 0xC1, 0xC6, 0x59, 0x33, 0xCF, 0xAA, 0x92, 0x56,
    0xEA, 0xFD, 0x36, 0x3C, 0xA2, 0xD3, 0x45, 0xF9, 0xE5, 0x3D, 0x95, 0xA5, 0x9A, 0xA0, 0x44, 0x7C, 0xD5, 0x6E, 0xDB, 0x42,
    0xA5, 0xCE, 0xC0, 0x4C, 0xA4, 0xD4, 0x50, 0x6E, 0xE7, 0xC1, 0xA2, 0x8F, 0x62, 0x07, 0x7C, 0x3C, 0xA9, 0x56, 0xE3, 0x5F,
    0xC8, 0x94, 0xBA, 0x21, 0x87, 0x29, 0x21, 0xCB, 0x33, 0x6C, 0xC0, 0x99, 0x52, 0x82, 0x0B, 0x97, 0xFD, 0xCA, 0x2A, 0xC7,
    0xC6, 0xBA, 0x74, 0x25, 0xAF, 0x01, 0x1E, 0x3D, 0xC5, 0xA2, 0x67, 0xE5, 0xD2, 0x68, 0x6E, 0x9A, 0x13,
};

// encrypted_payload for the 17.3.3 plaintext encryption_timestamp 0x0123456789ABCDEF
// followed by the 16-octet encapsulated AVTPDU 0x10, 0x11, ... 0x1F (24 octets -> 32-octet C).
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
constexpr std::array<uint8_t, 97> kat_ecc1_framed{
    0x01, 0xC2, 0x3F, 0x7A, 0x6A, 0x51, 0xAA, 0x50, 0xAE, 0xCD, 0x3A, 0xAA, 0xC1, 0xC6, 0x59, 0x33, 0xCF, 0xAA, 0x92, 0x56,
    0xEA, 0xFD, 0x36, 0x3C, 0xA2, 0xD3, 0x45, 0xF9, 0xE5, 0x3D, 0x95, 0xA5, 0x9A, 0xE9, 0x05, 0xF2, 0x76, 0x29, 0xD3, 0x3D,
    0x7F, 0xB9, 0x57, 0xDE, 0x4E, 0xA4, 0x70, 0x7A, 0x22, 0xAE, 0xF6, 0x22, 0xD9, 0x5D, 0xB6, 0xAD, 0x7D, 0x47, 0x61, 0x90,
    0x93, 0x65, 0xC6, 0x7C, 0x4E, 0x16, 0x6B, 0xED, 0x53, 0x5F, 0xD2, 0x6F, 0xF8, 0x74, 0x46, 0xA6, 0xE2, 0x3D, 0x2F, 0x8A,
    0xDF, 0x4D, 0x88, 0xA2, 0x64, 0x47, 0xD0, 0x16, 0x03, 0x9A, 0x3D, 0x68, 0x61, 0x92, 0x89, 0x60, 0x4A,
};

auto kat_recipient() -> P256PrivateKey
{
    return p256_ecdsa_keypair_from_seed(kat_seed);
}

// ECC2 (RFC 9180 HPKE base mode, AES-256-GCM) known answers from the independent
// Python HPKE reference (statusbar_crypto.py over the cryptography package):
// recipient d = SHA-256(kat_seed) mod n, ikm_e = kat_entropy, key_id 70-B3-D5-ED-C4-00-00-42,
// aad = the EECF header for that key_id and the payload's length.
statusbar::ieee::Eui64 const kat_key_id{0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x00, 0x42};

// encrypted_payload for the plaintext "Hello, HPKE ECC2" (16 octets -> 16-octet C).
constexpr std::array<uint8_t, 97> kat_ecc2_hello{
    0x04, 0x28, 0x9D, 0xCE, 0x76, 0x02, 0x6F, 0x47, 0xDC, 0xB7, 0xD7, 0x70, 0x06, 0x98, 0x2F, 0x29, 0xB0, 0x6F, 0x0E, 0x58,
    0xC1, 0x66, 0xB7, 0x38, 0xD5, 0xDF, 0xCD, 0x27, 0x62, 0x80, 0xDE, 0x6D, 0x23, 0x7C, 0x66, 0xC8, 0x67, 0x0A, 0x41, 0x91,
    0xC3, 0xFF, 0xB0, 0xDE, 0x2C, 0xB2, 0xBB, 0x79, 0x3C, 0x88, 0x5F, 0xC8, 0x63, 0x1D, 0x0D, 0xCA, 0x63, 0x79, 0x5C, 0x4D,
    0xAD, 0x39, 0x4A, 0xA7, 0xCD, 0xE9, 0x27, 0xF2, 0x1B, 0xE6, 0x15, 0x87, 0xD5, 0x11, 0xF4, 0x7E, 0x61, 0xB2, 0xA8, 0x48,
    0xEC, 0xDF, 0x45, 0x24, 0xFD, 0x10, 0x76, 0x2F, 0xE8, 0xEF, 0xA7, 0xAB, 0x8B, 0x52, 0x6E, 0xF7, 0x33,
};

// encrypted_payload for the 17.3.3 plaintext kat_timestamp || kat_avtpdu (24 octets -> 24-octet C).
constexpr std::array<uint8_t, 105> kat_ecc2_framed{
    0x04, 0x28, 0x9D, 0xCE, 0x76, 0x02, 0x6F, 0x47, 0xDC, 0xB7, 0xD7, 0x70, 0x06, 0x98, 0x2F, 0x29, 0xB0, 0x6F, 0x0E, 0x58, 0xC1,
    0x66, 0xB7, 0x38, 0xD5, 0xDF, 0xCD, 0x27, 0x62, 0x80, 0xDE, 0x6D, 0x23, 0x7C, 0x66, 0xC8, 0x67, 0x0A, 0x41, 0x91, 0xC3, 0xFF,
    0xB0, 0xDE, 0x2C, 0xB2, 0xBB, 0x79, 0x3C, 0x88, 0x5F, 0xC8, 0x63, 0x1D, 0x0D, 0xCA, 0x63, 0x79, 0x5C, 0x4D, 0xAD, 0x39, 0x4A,
    0xA7, 0xCD, 0xA0, 0x61, 0xDB, 0x10, 0x00, 0x92, 0x6A, 0x72, 0x51, 0xAE, 0x29, 0x52, 0xE3, 0xFE, 0x1D, 0xC9, 0x22, 0x45, 0xC6,
    0xBF, 0x41, 0x45, 0x2B, 0x5B, 0x84, 0xBB, 0xE9, 0x9E, 0xD1, 0x44, 0x96, 0xBE, 0x08, 0xC5, 0x18, 0x6E, 0xB4, 0xFD, 0xBD, 0x3F,
};

auto as_bytes(std::string_view text) -> std::span<uint8_t const>
{
    return {reinterpret_cast<uint8_t const*>(text.data()), text.size()};
}

}  // namespace

TEST(avtp_aem_frame, eecf_ecc1_sizes)
{
    EXPECT_TRUE(eecf_ecc1_v_size == 33);
    EXPECT_TRUE(eecf_ecc1_tag_size == 32);
    EXPECT_TRUE(eecf_ecc1_overhead == 65);
    EXPECT_TRUE(eecf_ecc1_encrypted_size(0) == 81);
    EXPECT_TRUE(eecf_ecc1_encrypted_size(16) == 97);
    EXPECT_TRUE(eecf_ecc1_encrypted_size(24) == 97);

    EXPECT_TRUE(eecf_ecc1_payload_size_is_valid(81));
    EXPECT_TRUE(eecf_ecc1_payload_size_is_valid(97));
    EXPECT_FALSE(eecf_ecc1_payload_size_is_valid(65));
    EXPECT_FALSE(eecf_ecc1_payload_size_is_valid(96));
    EXPECT_FALSE(eecf_ecc1_payload_size_is_valid(eecf_max_encrypted_payload_size + 16));
}

TEST(avtp_aem_frame, eecf_ecc1_payload_known_answer)
{
    auto const sk = kat_recipient();
    auto const plaintext = as_bytes("Hello, DL/ECIES!");

    std::array<uint8_t, 97> payload{};
    auto const built = eecf_ecc1_encrypt_payload(sk.public_key, plaintext, payload, kat_entropy);
    EXPECT_TRUE(built.has_value());
    EXPECT_TRUE(span_compare(*built, std::span<uint8_t const>(kat_ecc1_hello)));

    std::array<uint8_t, 97> recovered{};
    auto const opened = eecf_ecc1_decrypt_payload(sk, kat_ecc1_hello, recovered);
    EXPECT_TRUE(opened.has_value());
    EXPECT_TRUE(span_compare(*opened, plaintext));
}

TEST(avtp_aem_frame, eecf_ecc1_payload_is_v_then_t_then_c)
{
    // The library's 1363a triple is V || C || T; the wire payload is V || T || C.
    auto const sk = kat_recipient();
    auto const plaintext = as_bytes("Hello, DL/ECIES!");

    std::array<uint8_t, 97> triple{};
    auto const vct = ecies_encrypt(sk.public_key, plaintext, triple, kat_entropy);
    EXPECT_TRUE(vct.size() == 97);

    std::array<uint8_t, 97> payload{};
    auto const vtc = eecf_ecc1_encrypt_payload(sk.public_key, plaintext, payload, kat_entropy);
    EXPECT_TRUE(vtc.has_value());

    EXPECT_TRUE(span_compare(vtc->first(33), vct.first(33)));          // V
    EXPECT_TRUE(span_compare(vtc->subspan(33, 32), vct.last(32)));     // T
    EXPECT_TRUE(span_compare(vtc->subspan(65), vct.subspan(33, 32)));  // C

    // The library's order is not an ECC1 payload: the tag check must fail on it.
    std::array<uint8_t, 97> recovered{};
    EXPECT_FALSE(eecf_ecc1_decrypt_payload(sk, vct, recovered).has_value());
}

TEST(avtp_aem_frame, eecf_ecc1_framed_known_answer)
{
    auto const sk = kat_recipient();

    std::array<uint8_t, 97> payload{};
    auto const built = eecf_ecc1_encrypt(sk.public_key, kat_timestamp, kat_avtpdu, payload, kat_entropy);
    EXPECT_TRUE(built.has_value());
    EXPECT_TRUE(span_compare(*built, std::span<uint8_t const>(kat_ecc1_framed)));

    std::array<uint8_t, 97> plaintext{};
    auto const opened = eecf_ecc1_decrypt(sk, kat_ecc1_framed, plaintext);
    EXPECT_TRUE(opened.has_value());
    EXPECT_TRUE(opened->encryption_timestamp == kat_timestamp);
    EXPECT_TRUE(span_compare(opened->encapsulated_avtpdu, std::span<uint8_t const>(kat_avtpdu)));
}

TEST(avtp_aem_frame, eecf_ecc1_decrypt_rejects_tampering_and_bad_shapes)
{
    auto const sk = kat_recipient();
    std::array<uint8_t, 97> recovered{};

    auto tampered_tag = kat_ecc1_hello;
    tampered_tag[40] ^= 0x01;  // inside T
    EXPECT_FALSE(eecf_ecc1_decrypt_payload(sk, tampered_tag, recovered).has_value());

    auto tampered_cipher = kat_ecc1_hello;
    tampered_cipher[70] ^= 0x01;  // inside C
    EXPECT_FALSE(eecf_ecc1_decrypt_payload(sk, tampered_cipher, recovered).has_value());

    auto tampered_v = kat_ecc1_hello;
    tampered_v[5] ^= 0x01;  // inside V
    EXPECT_FALSE(eecf_ecc1_decrypt_payload(sk, tampered_v, recovered).has_value());

    // Not an ECC1 shape: no cipher block, a partial block, and a short output buffer.
    EXPECT_FALSE(eecf_ecc1_decrypt_payload(sk, std::span<uint8_t const>(kat_ecc1_hello).first(65), recovered).has_value());
    EXPECT_FALSE(eecf_ecc1_decrypt_payload(sk, std::span<uint8_t const>(kat_ecc1_hello).first(96), recovered).has_value());
    EXPECT_FALSE(eecf_ecc1_decrypt_payload(sk, kat_ecc1_hello, std::span<uint8_t>(recovered).first(32)).has_value());

    // Framed decrypt needs the 8-octet timestamp: "Hello, DL/ECIES!" is 16 octets, so this
    // one opens; a payload of an empty plaintext does not.
    std::array<uint8_t, 81> empty_payload{};
    auto const built = eecf_ecc1_encrypt_payload(sk.public_key, {}, empty_payload, kat_entropy);
    EXPECT_TRUE(built.has_value());
    EXPECT_FALSE(eecf_ecc1_decrypt(sk, *built, recovered).has_value());
}

TEST(avtp_aem_frame, eecf_ecc1_encrypt_rejects_small_buffer_and_oversize)
{
    auto const sk = kat_recipient();

    std::array<uint8_t, 96> too_small{};
    EXPECT_FALSE(eecf_ecc1_encrypt_payload(sk.public_key, as_bytes("Hello, DL/ECIES!"), too_small, kat_entropy).has_value());

    // 1488 octets (a 1500-octet MTU less the EECF header) is the most one frame carries;
    // 65 + 1424 = 1489 would not fit, 65 + 1408 = 1473 does.
    std::array<uint8_t, 1423> big_plaintext{};
    std::array<uint8_t, 4096> big_out{};
    EXPECT_FALSE(eecf_ecc1_encrypt_payload(sk.public_key, big_plaintext, big_out, kat_entropy).has_value());
    std::array<uint8_t, 1407> fits{};
    EXPECT_TRUE(eecf_ecc1_encrypt_payload(sk.public_key, fits, big_out, kat_entropy).has_value());
}

TEST(avtp_aem_frame, eecf_ecc1_full_frame_roundtrip)
{
    auto const sk = kat_recipient();

    std::array<uint8_t, 256> payload{};
    auto const encrypted = eecf_ecc1_encrypt(sk.public_key, kat_timestamp, kat_avtpdu, payload, kat_entropy);
    EXPECT_TRUE(encrypted.has_value());

    std::array<uint8_t, 512> frame{};
    auto const built = build_eecf_frame(frame, test_controller_mac, test_talker_mac, eecf_enc_ecc1, test_talker_eid, *encrypted);
    EXPECT_TRUE(built.has_value());
    EXPECT_TRUE(built->size() == eecf_payload_offset + 97);

    auto const parsed = parse_eecf_frame(*built);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->enc == eecf_enc_ecc1);
    EXPECT_TRUE(parsed->encrypted_payload_length == 97);
    EXPECT_TRUE(span_compare(parsed->encrypted_payload, std::span<uint8_t const>(kat_ecc1_framed)));

    std::array<uint8_t, 256> plaintext{};
    auto const opened = eecf_ecc1_decrypt(sk, parsed->encrypted_payload, plaintext);
    EXPECT_TRUE(opened.has_value());
    EXPECT_TRUE(opened->encryption_timestamp == kat_timestamp);
    EXPECT_TRUE(span_compare(opened->encapsulated_avtpdu, std::span<uint8_t const>(kat_avtpdu)));
}

// ===========================================================================
// ECC2 encrypted_payload (RFC 9180 HPKE base mode over P-256; IEEE 1722 security
// amendment proposal 17.4, avb/docs/IEEE_1722_CRYPTO_AMENDMENT.md)
// ===========================================================================

TEST(avtp_aem_frame, eecf_frame_size_rules)
{
    // 17.4.4: EncapMTU = MTU - 36 - Npk, 1399 for ECC2 and 1432 for ECC3 at a 1500-octet MTU.
    EXPECT_TRUE(eecf_mtu == 1500);
    EXPECT_TRUE(eecf_max_encrypted_payload_size == 1488);
    EXPECT_TRUE(eecf_ecc2_max_encapsulated_avtpdu_size == 1399);
    EXPECT_TRUE(eecf_ecc3_max_encapsulated_avtpdu_size == 1432);
    // 17.4.3 step 2: at least Npk + 24.
    EXPECT_TRUE(eecf_ecc2_min_framed_size == 89);
    EXPECT_TRUE(eecf_ecc3_min_framed_size == 56);
}

TEST(avtp_aem_frame, eecf_ecc2_sizes)
{
    EXPECT_TRUE(eecf_enc_ecc2 == 1);
    EXPECT_TRUE(eecf_ecc2_enc_size == 65);
    EXPECT_TRUE(eecf_ecc2_tag_size == 16);
    EXPECT_TRUE(eecf_ecc2_overhead == 81);
    EXPECT_TRUE(eecf_ecc2_encrypted_size(0) == 81);
    EXPECT_TRUE(eecf_ecc2_encrypted_size(24) == 105);
    EXPECT_TRUE(eecf_ecc2_payload_size_is_valid(81));
    EXPECT_TRUE(eecf_ecc2_payload_size_is_valid(1488));
    EXPECT_FALSE(eecf_ecc2_payload_size_is_valid(80));
    EXPECT_FALSE(eecf_ecc2_payload_size_is_valid(1489));
}

TEST(avtp_aem_frame, eecf_ecc2_header_is_the_frame_header)
{
    auto const header = eecf_ecc2_header(kat_key_id, 105);
    constexpr std::array<uint8_t, 12> expected{0xED, 0x01, 0x00, 0x69, 0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x00, 0x42};
    EXPECT_TRUE(span_compare(std::span<uint8_t const>(header), std::span<uint8_t const>(expected)));
}

TEST(avtp_aem_frame, eecf_ecc2_payload_known_answer)
{
    auto const sk = kat_recipient();
    auto const plaintext = as_bytes("Hello, HPKE ECC2");
    auto const aad = eecf_ecc2_header(kat_key_id, kat_ecc2_hello.size());

    std::array<uint8_t, 97> payload{};
    auto const built = eecf_ecc2_encrypt_payload(sk.public_key, aad, plaintext, payload, kat_entropy);
    EXPECT_TRUE(built.has_value());
    EXPECT_TRUE(span_compare(*built, std::span<uint8_t const>(kat_ecc2_hello)));

    std::array<uint8_t, 16> recovered{};
    auto const opened = eecf_ecc2_decrypt_payload(sk, aad, kat_ecc2_hello, recovered);
    EXPECT_TRUE(opened.has_value());
    EXPECT_TRUE(span_compare(*opened, plaintext));
}

TEST(avtp_aem_frame, eecf_ecc2_framed_known_answer)
{
    auto const sk = kat_recipient();

    std::array<uint8_t, 105> payload{};
    auto const built = eecf_ecc2_encrypt(sk.public_key, kat_key_id, kat_timestamp, kat_avtpdu, payload, kat_entropy);
    EXPECT_TRUE(built.has_value());
    EXPECT_TRUE(span_compare(*built, std::span<uint8_t const>(kat_ecc2_framed)));

    std::array<uint8_t, 105> plaintext{};
    auto const opened = eecf_ecc2_decrypt(sk, eecf_ecc2_header(kat_key_id, 105), kat_ecc2_framed, plaintext);
    EXPECT_TRUE(opened.has_value());
    EXPECT_TRUE(opened->encryption_timestamp == kat_timestamp);
    EXPECT_TRUE(span_compare(opened->encapsulated_avtpdu, std::span<uint8_t const>(kat_avtpdu)));
}

TEST(avtp_aem_frame, eecf_ecc2_header_is_authenticated)
{
    auto const sk = kat_recipient();
    std::array<uint8_t, 105> plaintext{};

    // A different key_id, the ECC1 enc value, or a different length in the header: all rejected.
    statusbar::ieee::Eui64 const other_key_id{0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x00, 0x43};
    EXPECT_FALSE(eecf_ecc2_decrypt(sk, eecf_ecc2_header(other_key_id, 105), kat_ecc2_framed, plaintext).has_value());
    auto downgraded = eecf_ecc2_header(kat_key_id, 105);
    downgraded[1] = eecf_enc_ecc1;
    EXPECT_FALSE(eecf_ecc2_decrypt(sk, downgraded, kat_ecc2_framed, plaintext).has_value());
    EXPECT_FALSE(eecf_ecc2_decrypt(sk, eecf_ecc2_header(kat_key_id, 106), kat_ecc2_framed, plaintext).has_value());
    EXPECT_TRUE(eecf_ecc2_decrypt(sk, eecf_ecc2_header(kat_key_id, 105), kat_ecc2_framed, plaintext).has_value());
}

TEST(avtp_aem_frame, eecf_ecc2_decrypt_rejects_tampering_and_bad_shapes)
{
    auto const sk = kat_recipient();
    auto const aad = eecf_ecc2_header(kat_key_id, 97);
    std::array<uint8_t, 16> recovered{};

    auto tampered_enc = kat_ecc2_hello;
    tampered_enc[10] ^= 0x01;  // inside enc: no longer the point that was used
    EXPECT_FALSE(eecf_ecc2_decrypt_payload(sk, aad, tampered_enc, recovered).has_value());
    auto tampered_cipher = kat_ecc2_hello;
    tampered_cipher[70] ^= 0x01;  // inside C
    EXPECT_FALSE(eecf_ecc2_decrypt_payload(sk, aad, tampered_cipher, recovered).has_value());
    auto tampered_tag = kat_ecc2_hello;
    tampered_tag[96] ^= 0x01;  // inside T
    EXPECT_FALSE(eecf_ecc2_decrypt_payload(sk, aad, tampered_tag, recovered).has_value());

    // Not an ECC2 shape, and a short output buffer.
    EXPECT_FALSE(eecf_ecc2_decrypt_payload(sk, aad, std::span<uint8_t const>(kat_ecc2_hello).first(80), recovered).has_value());
    EXPECT_FALSE(eecf_ecc2_decrypt_payload(sk, aad, kat_ecc2_hello, std::span<uint8_t>(recovered).first(15)).has_value());
}

TEST(avtp_aem_frame, eecf_ecc2_encrypt_rejects_small_buffer_and_oversize)
{
    auto const sk = kat_recipient();
    auto const plaintext = as_bytes("Hello, HPKE ECC2");
    std::array<uint8_t, 96> too_small{};
    EXPECT_FALSE(eecf_ecc2_encrypt_payload(sk.public_key, {}, plaintext, too_small, kat_entropy).has_value());

    // 1407 octets of plaintext fill the 1488-octet payload exactly; one more does not fit.
    std::array<uint8_t, 1407> big{};
    std::array<uint8_t, 2048> out{};
    EXPECT_TRUE(eecf_ecc2_encrypt_payload(sk.public_key, {}, big, out, kat_entropy).has_value());
    EXPECT_FALSE(
        eecf_ecc2_encrypt_payload(sk.public_key, {}, std::span<uint8_t const>(out).first(1408), out, kat_entropy).has_value());

    // The framed form bounds the AVTPDU at EncapMTU (1399) and the framed decrypt
    // discards anything shorter than Npk + 24 before touching HPKE.
    std::array<uint8_t, 1399> avtpdu{};
    EXPECT_TRUE(eecf_ecc2_encrypt(sk.public_key, kat_key_id, kat_timestamp, avtpdu, out, kat_entropy).has_value());
    EXPECT_FALSE(
        eecf_ecc2_encrypt(sk.public_key, kat_key_id, kat_timestamp, std::span<uint8_t const>(out).first(1400), out, kat_entropy)
            .has_value());
    std::array<uint8_t, 88> too_short{};
    EXPECT_FALSE(eecf_ecc2_decrypt(sk, eecf_ecc2_header(kat_key_id, 88), too_short, out).has_value());
}

TEST(avtp_aem_frame, eecf_ecc2_full_frame_roundtrip)
{
    auto const sk = kat_recipient();

    std::array<uint8_t, 256> payload{};
    auto const encrypted = eecf_ecc2_encrypt(sk.public_key, kat_key_id, kat_timestamp, kat_avtpdu, payload, kat_entropy);
    EXPECT_TRUE(encrypted.has_value());

    std::array<uint8_t, 512> frame{};
    auto const built = build_eecf_frame(frame, test_controller_mac, test_talker_mac, eecf_enc_ecc2, kat_key_id, *encrypted);
    EXPECT_TRUE(built.has_value());
    EXPECT_TRUE(built->size() == eecf_payload_offset + 105);

    auto const parsed = parse_eecf_frame(*built);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->enc == eecf_enc_ecc2);
    EXPECT_TRUE(span_compare(parsed->header, std::span<uint8_t const>(eecf_ecc2_header(kat_key_id, 105))));
    EXPECT_TRUE(span_compare(parsed->encrypted_payload, std::span<uint8_t const>(kat_ecc2_framed)));

    std::array<uint8_t, 256> plaintext{};
    auto const opened = eecf_ecc2_decrypt(sk, parsed->header.first<eecf_header_size>(), parsed->encrypted_payload, plaintext);
    EXPECT_TRUE(opened.has_value());
    EXPECT_TRUE(opened->encryption_timestamp == kat_timestamp);
    EXPECT_TRUE(span_compare(opened->encapsulated_avtpdu, std::span<uint8_t const>(kat_avtpdu)));
}

// ===========================================================================
// ECC3 encrypted_payload (RFC 9180 HPKE base mode over X25519; amendment 17.4)
//
// Known answers from the independent Python HPKE reference and the amendment's
// test-vector slide: recipient key = DeriveKeyPair(kat_seed) for DHKEM(X25519),
// ikm_e = kat_entropy, key_id 70-B3-D5-ED-C4-00-00-42, aad = the EECF header for
// that key_id, enc = 2 and the payload's length.
// ===========================================================================

namespace {

auto kat_ecc3_recipient() -> X25519PrivateKey
{
    return hpke_x25519_derive_key_pair(kat_seed).value();
}

constexpr std::array<uint8_t, 32> kat_ecc3_recipient_pk{
    0x49, 0xE4, 0x87, 0x4E, 0x25, 0xFE, 0x38, 0x9E, 0xD3, 0xC9, 0xFA, 0x2F, 0xD0, 0x9D, 0x07, 0x79,
    0x07, 0xEC, 0xC5, 0x80, 0x9C, 0x86, 0x19, 0xE2, 0x12, 0x71, 0x28, 0xB6, 0xA7, 0x2C, 0x7C, 0x7A,
};

// encrypted_payload for the plaintext "Hello, HPKE ECC3" (16 octets -> 16-octet C).
constexpr std::array<uint8_t, 64> kat_ecc3_hello{
    0x8A, 0x7D, 0x27, 0x73, 0xC3, 0xDC, 0xC1, 0xCB, 0xFA, 0x27, 0x5F, 0x67, 0x91, 0x5A, 0x8B, 0x3F,
    0x7B, 0x28, 0xB6, 0xE2, 0xC9, 0xB4, 0x73, 0x46, 0xFB, 0xFD, 0x23, 0x2C, 0xDF, 0x38, 0xFC, 0x22,
    0xC1, 0x01, 0x0C, 0xD6, 0x1E, 0xEF, 0x7D, 0x3D, 0x7E, 0xD7, 0x71, 0xC7, 0x0B, 0x4C, 0xB9, 0x0B,
    0xB5, 0x1F, 0x77, 0xE0, 0xD2, 0x3E, 0xE6, 0x46, 0xEC, 0x9E, 0x33, 0xE1, 0xB2, 0x3D, 0x2C, 0x56,
};

// encrypted_payload for the 17.3.3 plaintext kat_timestamp || kat_avtpdu (24 octets -> 24-octet C):
// the amendment's ECC3 test vector.
constexpr std::array<uint8_t, 72> kat_ecc3_framed{
    0x8A, 0x7D, 0x27, 0x73, 0xC3, 0xDC, 0xC1, 0xCB, 0xFA, 0x27, 0x5F, 0x67, 0x91, 0x5A, 0x8B, 0x3F, 0x7B, 0x28,
    0xB6, 0xE2, 0xC9, 0xB4, 0x73, 0x46, 0xFB, 0xFD, 0x23, 0x2C, 0xDF, 0x38, 0xFC, 0x22, 0x88, 0x47, 0x25, 0xDD,
    0xF8, 0x68, 0x90, 0x9A, 0x3E, 0x8D, 0x26, 0xF4, 0x5A, 0x1A, 0xEC, 0x2F, 0xB0, 0x54, 0x0E, 0xBC, 0xD1, 0xEB,
    0x7D, 0xF3, 0x0E, 0xB9, 0x5B, 0x3B, 0x51, 0x0A, 0x5A, 0xBE, 0x6C, 0x6B, 0xE6, 0x77, 0xDA, 0x63, 0x28, 0x43,
};

}  // namespace

TEST(avtp_aem_frame, eecf_ecc3_sizes)
{
    EXPECT_TRUE(eecf_enc_ecc3 == 2);
    EXPECT_TRUE(eecf_ecc3_enc_size == 32);
    EXPECT_TRUE(eecf_ecc3_tag_size == 16);
    EXPECT_TRUE(eecf_ecc3_overhead == 48);
    EXPECT_TRUE(eecf_ecc3_encrypted_size(0) == 48);
    EXPECT_TRUE(eecf_ecc3_encrypted_size(24) == 72);
    EXPECT_TRUE(eecf_ecc3_payload_size_is_valid(48));
    EXPECT_TRUE(eecf_ecc3_payload_size_is_valid(1488));
    EXPECT_FALSE(eecf_ecc3_payload_size_is_valid(47));
    EXPECT_FALSE(eecf_ecc3_payload_size_is_valid(1489));
    EXPECT_TRUE(span_compare(
        std::span<uint8_t const>(kat_ecc3_recipient().public_key.data), std::span<uint8_t const>(kat_ecc3_recipient_pk)));
}

TEST(avtp_aem_frame, eecf_ecc3_header_is_the_frame_header)
{
    auto const header = eecf_ecc3_header(kat_key_id, 72);
    constexpr std::array<uint8_t, 12> expected{0xED, 0x02, 0x00, 0x48, 0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x00, 0x42};
    EXPECT_TRUE(span_compare(std::span<uint8_t const>(header), std::span<uint8_t const>(expected)));
}

TEST(avtp_aem_frame, eecf_ecc3_payload_known_answer)
{
    auto const sk = kat_ecc3_recipient();
    auto const plaintext = as_bytes("Hello, HPKE ECC3");
    auto const aad = eecf_ecc3_header(kat_key_id, kat_ecc3_hello.size());

    std::array<uint8_t, 64> payload{};
    auto const built = eecf_ecc3_encrypt_payload(sk.public_key, aad, plaintext, payload, kat_entropy);
    EXPECT_TRUE(built.has_value());
    EXPECT_TRUE(span_compare(*built, std::span<uint8_t const>(kat_ecc3_hello)));

    std::array<uint8_t, 16> recovered{};
    auto const opened = eecf_ecc3_decrypt_payload(sk, aad, kat_ecc3_hello, recovered);
    EXPECT_TRUE(opened.has_value());
    EXPECT_TRUE(span_compare(*opened, plaintext));
}

TEST(avtp_aem_frame, eecf_ecc3_framed_known_answer)
{
    auto const sk = kat_ecc3_recipient();

    std::array<uint8_t, 72> payload{};
    auto const built = eecf_ecc3_encrypt(sk.public_key, kat_key_id, kat_timestamp, kat_avtpdu, payload, kat_entropy);
    EXPECT_TRUE(built.has_value());
    EXPECT_TRUE(span_compare(*built, std::span<uint8_t const>(kat_ecc3_framed)));

    std::array<uint8_t, 72> plaintext{};
    auto const opened = eecf_ecc3_decrypt(sk, eecf_ecc3_header(kat_key_id, 72), kat_ecc3_framed, plaintext);
    EXPECT_TRUE(opened.has_value());
    EXPECT_TRUE(opened->encryption_timestamp == kat_timestamp);
    EXPECT_TRUE(span_compare(opened->encapsulated_avtpdu, std::span<uint8_t const>(kat_avtpdu)));
}

TEST(avtp_aem_frame, eecf_ecc3_header_is_authenticated)
{
    auto const sk = kat_ecc3_recipient();
    std::array<uint8_t, 72> plaintext{};

    statusbar::ieee::Eui64 const other_key_id{0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x00, 0x43};
    EXPECT_FALSE(eecf_ecc3_decrypt(sk, eecf_ecc3_header(other_key_id, 72), kat_ecc3_framed, plaintext).has_value());
    auto relabelled = eecf_ecc3_header(kat_key_id, 72);
    relabelled[1] = eecf_enc_ecc2;
    EXPECT_FALSE(eecf_ecc3_decrypt(sk, relabelled, kat_ecc3_framed, plaintext).has_value());
    EXPECT_FALSE(eecf_ecc3_decrypt(sk, eecf_ecc3_header(kat_key_id, 73), kat_ecc3_framed, plaintext).has_value());
    EXPECT_TRUE(eecf_ecc3_decrypt(sk, eecf_ecc3_header(kat_key_id, 72), kat_ecc3_framed, plaintext).has_value());
}

TEST(avtp_aem_frame, eecf_ecc3_decrypt_rejects_tampering_and_bad_shapes)
{
    auto const sk = kat_ecc3_recipient();
    auto const aad = eecf_ecc3_header(kat_key_id, 64);
    std::array<uint8_t, 16> recovered{};

    auto tampered_enc = kat_ecc3_hello;
    tampered_enc[10] ^= 0x01;
    EXPECT_FALSE(eecf_ecc3_decrypt_payload(sk, aad, tampered_enc, recovered).has_value());
    auto tampered_cipher = kat_ecc3_hello;
    tampered_cipher[40] ^= 0x01;
    EXPECT_FALSE(eecf_ecc3_decrypt_payload(sk, aad, tampered_cipher, recovered).has_value());
    auto tampered_tag = kat_ecc3_hello;
    tampered_tag[63] ^= 0x01;
    EXPECT_FALSE(eecf_ecc3_decrypt_payload(sk, aad, tampered_tag, recovered).has_value());

    // A small-order enc (all zero u) fails HPKE setup (RFC 9180 7.1.4), as 17.4.3 step 4 says.
    auto small_order = kat_ecc3_hello;
    std::fill(small_order.begin(), small_order.begin() + 32, 0);
    EXPECT_FALSE(eecf_ecc3_decrypt_payload(sk, aad, small_order, recovered).has_value());

    EXPECT_FALSE(eecf_ecc3_decrypt_payload(sk, aad, std::span<uint8_t const>(kat_ecc3_hello).first(47), recovered).has_value());
    EXPECT_FALSE(eecf_ecc3_decrypt_payload(sk, aad, kat_ecc3_hello, std::span<uint8_t>(recovered).first(15)).has_value());
    std::array<uint8_t, 55> too_short{};
    std::array<uint8_t, 64> out{};
    EXPECT_FALSE(eecf_ecc3_decrypt(sk, eecf_ecc3_header(kat_key_id, 55), too_short, out).has_value());
}

TEST(avtp_aem_frame, eecf_ecc3_encrypt_rejects_small_buffer_and_oversize)
{
    auto const sk = kat_ecc3_recipient();
    std::array<uint8_t, 63> too_small{};
    EXPECT_FALSE(eecf_ecc3_encrypt_payload(sk.public_key, {}, as_bytes("Hello, HPKE ECC3"), too_small, kat_entropy).has_value());

    // 1440 octets of plaintext fill the 1488-octet payload exactly; EncapMTU is 1432 for the AVTPDU.
    std::array<uint8_t, 1440> big{};
    std::array<uint8_t, 2048> out{};
    EXPECT_TRUE(eecf_ecc3_encrypt_payload(sk.public_key, {}, big, out, kat_entropy).has_value());
    EXPECT_FALSE(
        eecf_ecc3_encrypt_payload(sk.public_key, {}, std::span<uint8_t const>(out).first(1441), out, kat_entropy).has_value());
    std::array<uint8_t, 1432> avtpdu{};
    EXPECT_TRUE(eecf_ecc3_encrypt(sk.public_key, kat_key_id, kat_timestamp, avtpdu, out, kat_entropy).has_value());
    EXPECT_FALSE(
        eecf_ecc3_encrypt(sk.public_key, kat_key_id, kat_timestamp, std::span<uint8_t const>(out).first(1433), out, kat_entropy)
            .has_value());
}

TEST(avtp_aem_frame, eecf_ecc3_full_frame_roundtrip)
{
    auto const sk = kat_ecc3_recipient();

    std::array<uint8_t, 256> payload{};
    auto const encrypted = eecf_ecc3_encrypt(sk.public_key, kat_key_id, kat_timestamp, kat_avtpdu, payload, kat_entropy);
    EXPECT_TRUE(encrypted.has_value());

    std::array<uint8_t, 512> frame{};
    auto const built = build_eecf_frame(frame, test_controller_mac, test_talker_mac, eecf_enc_ecc3, kat_key_id, *encrypted);
    EXPECT_TRUE(built.has_value());
    EXPECT_TRUE(built->size() == eecf_payload_offset + 72);

    auto const parsed = parse_eecf_frame(*built);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->enc == eecf_enc_ecc3);
    EXPECT_TRUE(span_compare(parsed->header, std::span<uint8_t const>(eecf_ecc3_header(kat_key_id, 72))));
    EXPECT_TRUE(span_compare(parsed->encrypted_payload, std::span<uint8_t const>(kat_ecc3_framed)));

    std::array<uint8_t, 256> plaintext{};
    auto const opened = eecf_ecc3_decrypt(sk, parsed->header.first<eecf_header_size>(), parsed->encrypted_payload, plaintext);
    EXPECT_TRUE(opened.has_value());
    EXPECT_TRUE(opened->encryption_timestamp == kat_timestamp);
    EXPECT_TRUE(span_compare(opened->encapsulated_avtpdu, std::span<uint8_t const>(kat_avtpdu)));
}

// ===========================================================================
// Main
// ===========================================================================

TEST_MAIN(statusbar_avtp_crypto, avtp_aem_frame_test)

TEST(avtp_aem_frame, eecf_timestamp_window)
{
    // 17.3.3.2 as amended (A8): the same symmetric 100 ms window as ESCF's A3.
    constexpr uint64_t now = 1'000'000'000'000ULL;
    constexpr uint64_t window = eecf_encryption_timestamp_window_ns;
    EXPECT_TRUE(window == 100'000'000ULL);
    EXPECT_TRUE(eecf_encryption_timestamp_is_acceptable(now, now));
    EXPECT_TRUE(eecf_encryption_timestamp_is_acceptable(now - window, now));
    EXPECT_FALSE(eecf_encryption_timestamp_is_acceptable(now - window - 1, now));
    EXPECT_TRUE(eecf_encryption_timestamp_is_acceptable(now + window, now));
    EXPECT_FALSE(eecf_encryption_timestamp_is_acceptable(now + window + 1, now));
    EXPECT_TRUE(eecf_encryption_timestamp_is_acceptable(0, window));
    EXPECT_FALSE(eecf_encryption_timestamp_is_acceptable(0, window + 1));
    EXPECT_TRUE(eecf_encryption_timestamp_is_acceptable(UINT64_MAX, UINT64_MAX - window));
    EXPECT_FALSE(eecf_encryption_timestamp_is_acceptable(UINT64_MAX, UINT64_MAX - window - 1));
}
