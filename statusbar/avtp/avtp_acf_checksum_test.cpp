// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_checksum.hpp"

#include "statusbar/avtp/avtp_acf_checksum_format.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/checksum/checksum_ones_complement.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>

using namespace statusbar;
using namespace statusbar::avtp;

static_assert(sizeof(AcfChecksumMessage) == AcfChecksumMessage::LENGTH);

TEST(acf_checksum, init_and_accessors)
{
    AcfChecksumMessage message{};
    message.init(0xBEEF);
    EXPECT_EQ(message.header.msg_type(), AcfMsgType::checksum);
    EXPECT_EQ(message.header.msg_length(), 1U);
    EXPECT_EQ(message.get_checksum(), 0xBEEFU);
    EXPECT_TRUE(message.is_valid());

    message.header.set_msg_length(2);
    EXPECT_FALSE(message.is_valid());
    message.header.init(AcfMsgType::crc, 1);
    EXPECT_FALSE(message.is_valid());
}

TEST(acf_checksum, compute_matches_core_and_zero_validates)
{
    // A preceding GPC message: 0x0A03 header + ten octets.
    std::array<uint8_t, 12> preceding{0x0A, 0x03, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
    auto const bytes = std::span<uint8_t const>(preceding);
    EXPECT_EQ(acf_checksum_compute(bytes), checksum::internet_checksum16(bytes));

    // A message whose ones-complement sum is 0xFFFF has checksum 0x0000 -
    // and unlike UDP that zero is a value to validate (9.4.20).
    std::array<uint8_t, 4> zero_sum{0x0A, 0x01, 0xF5, 0xFE};
    EXPECT_EQ(acf_checksum_compute(std::span<uint8_t const>(zero_sum)), 0x0000U);
    AcfChecksumMessage trailer{};
    trailer.init(0x0000);
    EXPECT_TRUE(acf_checksum_verify(trailer, std::span<uint8_t const>(zero_sum)));
    trailer.init(0x0001);
    EXPECT_FALSE(acf_checksum_verify(trailer, std::span<uint8_t const>(zero_sum)));
}

TEST(acf_checksum, build_parse_verify_round_trip)
{
    std::array<uint8_t, 12> preceding{0x0A, 0x03, 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    auto const bytes = std::span<uint8_t const>(preceding);

    std::array<uint8_t, 4> wire{};
    EXPECT_TRUE(acf_checksum_build(std::span<uint8_t>(wire), bytes));
    EXPECT_EQ(wire[0], 0xECU);  // 0x76 << 1
    EXPECT_EQ(wire[1], 0x01U);

    auto const parsed = acf_checksum_parse(std::span<uint8_t const>(wire));
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(acf_checksum_verify(*parsed, bytes));

    auto corrupt = preceding;
    corrupt[6] ^= 0x10U;
    EXPECT_FALSE(acf_checksum_verify(*parsed, std::span<uint8_t const>(corrupt)));

    std::array<uint8_t, 3> small{};
    EXPECT_FALSE(acf_checksum_build(std::span<uint8_t>(small), bytes));
    EXPECT_FALSE(acf_checksum_parse(std::span<uint8_t const>(small)).has_value());
}

TEST(acf_checksum, parse_rejects_wrong_type_or_length)
{
    std::array<uint8_t, 4> wire{0xEC, 0x02, 0x00, 0x00};  // length 2
    EXPECT_FALSE(acf_checksum_parse(std::span<uint8_t const>(wire)).has_value());
    wire[0] = 0xEE;  // ACF_CRC
    wire[1] = 0x01;
    EXPECT_FALSE(acf_checksum_parse(std::span<uint8_t const>(wire)).has_value());
}

TEST(acf_checksum, format_to_output)
{
    AcfChecksumMessage message{};
    message.init(0x1234);
    std::string result;
    format_to(std::back_inserter(result), message);
    EXPECT_TRUE(result.contains("ACF_CHECKSUM"));
    EXPECT_TRUE(result.contains("0x1234"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_checksum_test)
