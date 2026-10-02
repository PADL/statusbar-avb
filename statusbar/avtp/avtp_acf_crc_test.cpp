// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_crc.hpp"

#include "statusbar/avtp/avtp_acf_crc_format.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>

using namespace statusbar;
using namespace statusbar::avtp;

static_assert(sizeof(AcfCrcMessage) == AcfCrcMessage::LENGTH);

namespace {

// "123456789": CRC_ETH 0xCBF43926, CRC_32P4 0x1697D06A.
constexpr std::array<uint8_t, 9> CHECK_STRING{0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39};

}  // namespace

TEST(acf_crc, init_and_accessors)
{
    AcfCrcMessage fixed{};
    fixed.init(AcfCrcType::crc_32p4);
    EXPECT_EQ(fixed.header.msg_type(), AcfMsgType::crc);
    EXPECT_EQ(fixed.header.msg_length(), 2U);
    EXPECT_TRUE(fixed.crc_type() == AcfCrcType::crc_32p4);
    EXPECT_EQ(fixed.reserved_crc_type.get(), 0x0001U);
    EXPECT_TRUE(fixed.is_valid());

    fixed.set_crc_type(AcfCrcType::crc_user);
    EXPECT_TRUE(fixed.crc_type() == AcfCrcType::crc_user);
    fixed.header.set_msg_length(1);
    EXPECT_FALSE(fixed.is_valid());  // no crc_data quadlet
}

TEST(acf_crc, type_names)
{
    EXPECT_EQ(acf_crc_type_name(AcfCrcType::crc_eth), "CRC_ETH");
    EXPECT_EQ(acf_crc_type_name(AcfCrcType::crc_32p4), "CRC_32P4");
    EXPECT_EQ(acf_crc_type_name(AcfCrcType::crc_user), "CRC_USER");
    // A reserved value only ever arrives from the wire.
    std::array<uint8_t, 8> reserved{0xEE, 0x02, 0x00, 0x07, 0, 0, 0, 0};
    auto const view = acf_crc_parse(std::span<uint8_t const>(reserved));
    EXPECT_TRUE(view.has_value());
    EXPECT_EQ(acf_crc_type_name(view->fixed.crc_type()), "CRC_RESERVED");
}

TEST(acf_crc, compute_catalogue_values)
{
    auto const bytes = std::span<uint8_t const>(CHECK_STRING);
    auto const eth = acf_crc_compute(AcfCrcType::crc_eth, bytes);
    auto const p4 = acf_crc_compute(AcfCrcType::crc_32p4, bytes);
    EXPECT_TRUE(eth.has_value() && *eth == 0xCBF43926U);
    EXPECT_TRUE(p4.has_value() && *p4 == 0x1697D06AU);
    EXPECT_FALSE(acf_crc_compute(AcfCrcType::crc_user, bytes).has_value());
    std::array<uint8_t, 8> reserved{0xEE, 0x02, 0x00, 0x02, 0, 0, 0, 0};
    auto const view = acf_crc_parse(std::span<uint8_t const>(reserved));
    EXPECT_TRUE(view.has_value());
    EXPECT_FALSE(acf_crc_compute(view->fixed.crc_type(), bytes).has_value());
}

TEST(acf_crc, build_parse_verify_round_trip)
{
    auto const bytes = std::span<uint8_t const>(CHECK_STRING);
    for (auto const type : {AcfCrcType::crc_eth, AcfCrcType::crc_32p4}) {
        std::array<uint8_t, 8> wire{};
        EXPECT_TRUE(acf_crc_build(std::span<uint8_t>(wire), type, bytes));
        EXPECT_EQ(wire[0], 0xEEU);  // 0x77 << 1
        EXPECT_EQ(wire[1], 0x02U);
        EXPECT_EQ(wire[3], static_cast<uint8_t>(type));

        auto const view = acf_crc_parse(std::span<uint8_t const>(wire));
        EXPECT_TRUE(view.has_value());
        EXPECT_EQ(view->crc_data.size(), 4U);
        auto const value = view->crc32();
        auto const expected = acf_crc_compute(type, bytes);
        EXPECT_TRUE(value.has_value() && expected.has_value() && *value == *expected);
        auto const verdict = acf_crc_verify(*view, bytes);
        EXPECT_TRUE(verdict.has_value() && *verdict);

        auto corrupt = CHECK_STRING;
        corrupt[4] ^= 0x01U;
        auto const bad = acf_crc_verify(*view, std::span<uint8_t const>(corrupt));
        EXPECT_TRUE(bad.has_value() && !*bad);
    }

    std::array<uint8_t, 7> small{};
    EXPECT_FALSE(acf_crc_build(std::span<uint8_t>(small), AcfCrcType::crc_eth, bytes));
    std::array<uint8_t, 8> user{};
    EXPECT_FALSE(acf_crc_build(std::span<uint8_t>(user), AcfCrcType::crc_user, bytes));
}

TEST(acf_crc, parse_bounds_and_wide_crc_data)
{
    // Wrong type / too-short length.
    std::array<uint8_t, 8> wire{0xEE, 0x01, 0x00, 0x00, 0, 0, 0, 0};
    EXPECT_FALSE(acf_crc_parse(std::span<uint8_t const>(wire)).has_value());
    wire[1] = 0x03;  // 3 quadlets declared, 2 present
    EXPECT_FALSE(acf_crc_parse(std::span<uint8_t const>(wire)).has_value());
    std::array<uint8_t, 3> short_buf{};
    EXPECT_FALSE(acf_crc_parse(std::span<uint8_t const>(short_buf)).has_value());

    // A user-typed CRC with two quadlets of crc_data parses, but has no
    // 32-bit value and cannot be verified here.
    std::array<uint8_t, 12> wide{0xEE, 0x03, 0x00, 0x0F, 1, 2, 3, 4, 5, 6, 7, 8};
    auto const view = acf_crc_parse(std::span<uint8_t const>(wide));
    EXPECT_TRUE(view.has_value());
    EXPECT_EQ(view->crc_data.size(), 8U);
    EXPECT_FALSE(view->crc32().has_value());
    EXPECT_FALSE(acf_crc_verify(*view, std::span<uint8_t const>(CHECK_STRING)).has_value());
}

TEST(acf_crc, format_to_output)
{
    std::array<uint8_t, 8> wire{};
    EXPECT_TRUE(acf_crc_build(std::span<uint8_t>(wire), AcfCrcType::crc_eth, std::span<uint8_t const>(CHECK_STRING)));
    auto const view = acf_crc_parse(std::span<uint8_t const>(wire));
    EXPECT_TRUE(view.has_value());
    std::string result;
    format_to(std::back_inserter(result), *view);
    EXPECT_TRUE(result.contains("ACF_CRC"));
    EXPECT_TRUE(result.contains("CRC_ETH"));
    EXPECT_TRUE(result.contains("0xcbf43926"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_crc_test)
