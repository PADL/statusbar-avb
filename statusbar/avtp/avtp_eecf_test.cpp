// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_eecf.hpp"

#include "statusbar/avtp/avtp_eecf_format.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <array>

using namespace statusbar;
using namespace statusbar::avtp;
using namespace statusbar::ieee;

// Compile-time layout check
static_assert(sizeof(EecfPdu) == EecfPdu::HEADER_LENGTH);

TEST(eecf, init_and_accessors)
{
    EecfPdu pdu{};
    Eui64 kid(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    pdu.init(0, kid);

    EXPECT_EQ(pdu.subtype.get(), AvtpSubtype::eecf);
    EXPECT_EQ(pdu.version(), 0);
    EXPECT_EQ(pdu.enc(), 0);
    EXPECT_EQ(pdu.encrypted_payload_length(), 0);
    EXPECT_EQ(pdu.key_id(), kid);
    EXPECT_TRUE(pdu.is_valid());
}

TEST(eecf, encrypted_payload_length)
{
    EecfPdu pdu{};
    Eui64 kid{};
    pdu.init(0, kid);

    pdu.set_encrypted_payload_length(500);
    EXPECT_EQ(pdu.encrypted_payload_length(), 500);

    pdu.set_encrypted_payload_length(2047);
    EXPECT_EQ(pdu.encrypted_payload_length(), 2047);

    // Figure 110: a full 16-bit field (the MTU, not the field, bounds a real frame).
    pdu.set_encrypted_payload_length(0xFFFF);
    EXPECT_EQ(pdu.encrypted_payload_length(), 0xFFFF);
    std::array<uint8_t, 12> buf{};
    span_store(buf, pdu);
    EXPECT_EQ(buf[2], 0xFF);
    EXPECT_EQ(buf[3], 0xFF);
}

TEST(eecf, parse_valid)
{
    EecfPdu pdu{};
    Eui64 kid(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);
    pdu.init(0, kid);
    pdu.set_encrypted_payload_length(200);

    std::array<uint8_t, 32> buf{};
    span_store(buf, pdu);

    auto parsed = eecf_parse_header(std::span<uint8_t const>(buf));
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->enc(), 0);
    EXPECT_EQ(parsed->key_id(), kid);
    EXPECT_EQ(parsed->encrypted_payload_length(), 200);
}

TEST(eecf, parse_truncated)
{
    std::array<uint8_t, 8> buf{};
    buf[0] = AvtpSubtype::eecf;
    auto parsed = eecf_parse_header(std::span<uint8_t const>(buf));
    EXPECT_FALSE(parsed.has_value());
}

TEST(eecf, parse_wrong_subtype)
{
    EecfPdu pdu{};
    Eui64 kid{};
    pdu.init(0, kid);
    pdu.subtype = AvtpSubtype::escf;  // wrong

    std::array<uint8_t, 12> buf{};
    span_store(buf, pdu);
    auto parsed = eecf_parse_header(std::span<uint8_t const>(buf));
    EXPECT_FALSE(parsed.has_value());
}

TEST(eecf, get_encrypted_payload)
{
    std::array<uint8_t, 24> buf{};
    buf[0] = AvtpSubtype::eecf;

    auto payload = eecf_get_encrypted_payload(std::span<uint8_t const>(buf));
    EXPECT_EQ(payload.size(), 12U);

    auto empty = eecf_get_encrypted_payload(std::span<uint8_t const>(buf.data(), EecfPdu::HEADER_LENGTH));
    EXPECT_TRUE(empty.empty());
}

TEST(eecf, enc_mode_names)
{
    EXPECT_EQ(std::string(eecf_enc_mode_name(0)), "ECC1");
    EXPECT_EQ(std::string(eecf_enc_mode_name(1)), "ECC2");
    EXPECT_EQ(std::string(eecf_enc_mode_name(2)), "ECC3");
    EXPECT_EQ(std::string(eecf_enc_mode_name(3)), "Reserved");
}

TEST(eecf, format_to_output)
{
    EecfPdu pdu{};
    Eui64 kid(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    pdu.init(0, kid);
    pdu.set_encrypted_payload_length(64);

    std::string result;
    format_to(std::back_inserter(result), pdu);
    EXPECT_TRUE(result.find("EECF") != std::string::npos);
    EXPECT_TRUE(result.find("ECC1") != std::string::npos);
    EXPECT_TRUE(result.find("64") != std::string::npos);
}

TEST(eecf, matches_crypto_constants)
{
    // Verify our subtype matches what crypto/avtp uses
    EXPECT_EQ(AvtpSubtype::eecf, 0xEDU);
    EXPECT_EQ(EecfPdu::HEADER_LENGTH, 12U);
}

TEST_MAIN(statusbar_avtp, avtp_eecf_test)
