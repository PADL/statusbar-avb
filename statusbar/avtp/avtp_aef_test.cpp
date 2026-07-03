// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_aef.hpp"

#include "statusbar/avtp/avtp_aef_format.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <array>

using namespace statusbar;
using namespace statusbar::avtp;
using namespace statusbar::ieee;

// Compile-time layout checks
static_assert(sizeof(AefContinuousPdu) == AefContinuousPdu::HEADER_LENGTH);
static_assert(sizeof(AefDiscretePdu) == AefDiscretePdu::HEADER_LENGTH);

// AEF Continuous tests

TEST(aef_continuous, init_and_accessors)
{
    AefContinuousPdu pdu{};
    Eui64 kid(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    pdu.init(0x01, kid);

    EXPECT_EQ(pdu.subtype.get(), AvtpSubtype::aef_continuous);
    EXPECT_EQ(pdu.version(), 0);
    EXPECT_EQ(pdu.enc(), 1);
    EXPECT_EQ(pdu.get_stream_data_length(), 0);
    EXPECT_EQ(pdu.key_id(), kid);
    EXPECT_TRUE(pdu.is_valid());
}

TEST(aef_continuous, stream_data_length)
{
    AefContinuousPdu pdu{};
    Eui64 kid{};
    pdu.init(0, kid);

    pdu.set_stream_data_length(1500);
    EXPECT_EQ(pdu.get_stream_data_length(), 1500);

    pdu.set_stream_data_length(0);
    EXPECT_EQ(pdu.get_stream_data_length(), 0);

    pdu.set_stream_data_length(0xFFFF);
    EXPECT_EQ(pdu.get_stream_data_length(), 0xFFFF);
}

TEST(aef_continuous, parse_valid)
{
    AefContinuousPdu pdu{};
    Eui64 kid(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    pdu.init(0, kid);
    pdu.set_stream_data_length(100);

    // Serialize to buffer
    std::array<uint8_t, 64> buf{};
    span_store(buf, pdu);
    // Add some payload bytes
    for (size_t i = AefContinuousPdu::HEADER_LENGTH; i < 64; ++i) {
        buf[i] = static_cast<uint8_t>(i);
    }

    auto parsed = aef_continuous_parse_header(std::span<uint8_t const>(buf));
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->enc(), 0);
    EXPECT_EQ(parsed->key_id(), kid);
    EXPECT_EQ(parsed->get_stream_data_length(), 100);
}

TEST(aef_continuous, parse_truncated)
{
    std::array<uint8_t, 8> buf{};
    buf[0] = AvtpSubtype::aef_continuous;
    auto parsed = aef_continuous_parse_header(std::span<uint8_t const>(buf));
    EXPECT_FALSE(parsed.has_value());
}

TEST(aef_continuous, parse_wrong_subtype)
{
    AefContinuousPdu pdu{};
    Eui64 kid{};
    pdu.init(0, kid);
    // Corrupt subtype
    pdu.subtype = 0x00;

    std::array<uint8_t, 12> buf{};
    span_store(buf, pdu);
    auto parsed = aef_continuous_parse_header(std::span<uint8_t const>(buf));
    EXPECT_FALSE(parsed.has_value());
}

TEST(aef_continuous, get_encrypted_payload)
{
    std::array<uint8_t, 32> buf{};
    buf[0] = AvtpSubtype::aef_continuous;
    buf[3] = 16;  // stream_data_length = 16 (bytes 2-3, big-endian)

    // Payload is bounded to the declared stream_data_length, not the 20-byte buffer
    // remainder (which would include Ethernet padding).
    auto payload = aef_continuous_get_encrypted_payload(std::span<uint8_t const>(buf));
    EXPECT_EQ(payload.size(), 16U);

    // Too short -> empty
    auto empty = aef_continuous_get_encrypted_payload(std::span<uint8_t const>(buf.data(), AefContinuousPdu::HEADER_LENGTH));
    EXPECT_TRUE(empty.empty());
}

TEST(aef_continuous, enc_mode_names)
{
    EXPECT_EQ(std::string(aef_enc_mode_name(0)), "AES-SIV");
    EXPECT_EQ(std::string(aef_enc_mode_name(1)), "AES-GCM-SIV");
    EXPECT_EQ(std::string(aef_enc_mode_name(2)), "Reserved");
}

// AEF Discrete tests

TEST(aef_discrete, init_and_accessors)
{
    AefDiscretePdu pdu{};
    Eui64 kid(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    pdu.init(0x01, kid);

    EXPECT_EQ(pdu.subtype.get(), AvtpSubtype::aef_discrete);
    EXPECT_EQ(pdu.version(), 0);
    EXPECT_EQ(pdu.enc(), 1);
    EXPECT_EQ(pdu.control_data_length(), 0);
    EXPECT_EQ(pdu.key_id(), kid);
    EXPECT_TRUE(pdu.is_valid());
}

TEST(aef_discrete, control_data_length)
{
    AefDiscretePdu pdu{};
    Eui64 kid{};
    pdu.init(0, kid);

    pdu.set_control_data_length(500);
    EXPECT_EQ(pdu.control_data_length(), 500);

    // Max 11-bit value
    pdu.set_control_data_length(2047);
    EXPECT_EQ(pdu.control_data_length(), 2047);

    // Overflow clamped to 11 bits
    pdu.set_control_data_length(0xFFFF);
    EXPECT_EQ(pdu.control_data_length(), 2047);
}

TEST(aef_discrete, parse_valid)
{
    AefDiscretePdu pdu{};
    Eui64 kid(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);
    pdu.init(1, kid);
    pdu.set_control_data_length(200);

    std::array<uint8_t, 32> buf{};
    span_store(buf, pdu);

    auto parsed = aef_discrete_parse_header(std::span<uint8_t const>(buf));
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->enc(), 1);
    EXPECT_EQ(parsed->key_id(), kid);
    EXPECT_EQ(parsed->control_data_length(), 200);
}

TEST(aef_discrete, parse_wrong_subtype)
{
    AefDiscretePdu pdu{};
    Eui64 kid{};
    pdu.init(0, kid);
    pdu.subtype = AvtpSubtype::aef_continuous;  // wrong

    std::array<uint8_t, 12> buf{};
    span_store(buf, pdu);
    auto parsed = aef_discrete_parse_header(std::span<uint8_t const>(buf));
    EXPECT_FALSE(parsed.has_value());
}

TEST(aef_discrete, get_encrypted_payload)
{
    std::array<uint8_t, 24> buf{};
    buf[0] = AvtpSubtype::aef_discrete;
    buf[3] = 8;  // control_data_length = 8 (11-bit field in bytes 2-3)

    // Payload is bounded to the declared control_data_length, not the 12-byte buffer
    // remainder.
    auto payload = aef_discrete_get_encrypted_payload(std::span<uint8_t const>(buf));
    EXPECT_EQ(payload.size(), 8U);
}

// Format tests

TEST(aef_continuous, format_to_output)
{
    AefContinuousPdu pdu{};
    Eui64 kid(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    pdu.init(0, kid);
    pdu.set_stream_data_length(256);

    std::string result;
    format_to(std::back_inserter(result), pdu);
    EXPECT_TRUE(result.find("AEF-C") != std::string::npos);
    EXPECT_TRUE(result.find("AES-SIV") != std::string::npos);
    EXPECT_TRUE(result.find("256") != std::string::npos);
}

TEST(aef_discrete, format_to_output)
{
    AefDiscretePdu pdu{};
    Eui64 kid(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    pdu.init(1, kid);
    pdu.set_control_data_length(128);

    std::string result;
    format_to(std::back_inserter(result), pdu);
    EXPECT_TRUE(result.find("AEF-D") != std::string::npos);
    EXPECT_TRUE(result.find("AES-GCM-SIV") != std::string::npos);
    EXPECT_TRUE(result.find("128") != std::string::npos);
}

TEST_MAIN(statusbar_avtp, avtp_aef_test)
