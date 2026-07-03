// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Tests for IEEE 1722.1 AECP AEM frame builder/parser and EECF (Clause 17):
//   - AUTH_GET_NONCE command/response frame roundtrips
//   - AUTH_ADD_KEY_NONCE command/response frame roundtrips
//   - EECF encrypted control frame roundtrip
//   - control_data_length validation
//   - Parser rejection of invalid frames

#include "statusbar/avtp_crypto/avtp_aem_frame.hpp"

#include "statusbar/crypto/util/crypto_util_internal.hpp"
#include "statusbar/test/test.hpp"

#include <cstring>

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
// EECF frame roundtrip (IEEE 1722-2016 Clause 17)
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
// Main
// ===========================================================================

TEST_MAIN(statusbar_avtp_crypto, avtp_aem_frame_test)
