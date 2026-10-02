// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_lin.hpp"

#include "statusbar/avtp/avtp_acf_lin_format.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <vector>

using namespace statusbar;
using namespace statusbar::avtp;

static_assert(sizeof(AcfLinMessage) == AcfLinMessage::LENGTH);

namespace {

/// The fixed part with every field at its test value and the fixed-only length,
/// encoded independently of the accessors (the layout oracle); the length is the
/// smallest valid message (fixed part plus the minimum payload).
constexpr std::array<uint8_t, 12> LAYOUT{0x06, 0x03, 0xAB, 0x3C, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

/// A complete message on the wire: the fixed part (pad = 3, length = 5 quadlets)
/// followed by a 5-octet payload and its three pad octets.
constexpr std::array<uint8_t, 20> IMAGE{0x06, 0x05, 0xEB, 0x3C, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                        0x07, 0x08, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0x00, 0x00, 0x00};

constexpr std::array<uint8_t, 5> PAYLOAD{0xA1, 0xB2, 0xC3, 0xD4, 0xE5};

/// Set every field to its test value
void set_test_values(AcfLinMessage& m)
{
    m.set_pad(0x2U);
    m.set_mtv(true);
    m.set_lin_bus_id(0xBU);
    m.set_lin_identifier(0x3CU);
    m.set_message_timestamp(0x102030405060708U);
}

/// Check every field against its test value
void expect_test_values(AcfLinMessage const& m)
{
    EXPECT_EQ(m.pad(), 0x2U);
    EXPECT_TRUE(m.mtv());
    EXPECT_EQ(m.lin_bus_id(), 0xBU);
    EXPECT_EQ(m.get_lin_identifier(), 0x3CU);
    EXPECT_EQ(m.get_message_timestamp(), 0x102030405060708U);
}

}  // namespace

TEST(acf_lin, init_and_layout)
{
    AcfLinMessage m{};
    m.init();
    EXPECT_EQ(m.header.msg_type(), AcfMsgType::lin);
    EXPECT_EQ(m.header.msg_length(), 3U);
    EXPECT_TRUE(m.is_valid());

    set_test_values(m);
    expect_test_values(m);
    std::array<uint8_t, 12> bytes{};
    span_store(bytes, m);
    for (size_t i = 0; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], LAYOUT[i]);
    }
}

TEST(acf_lin, parse_wire_image)
{
    auto const view = acf_lin_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    EXPECT_EQ(view->fixed.pad(), 3U);
    EXPECT_TRUE(view->fixed.mtv());
    EXPECT_EQ(view->fixed.lin_bus_id(), 0xBU);
    EXPECT_EQ(view->fixed.get_lin_identifier(), 0x3CU);
    EXPECT_EQ(view->fixed.get_message_timestamp(), 0x102030405060708U);
    EXPECT_EQ(view->padded_payload.size(), 8U);
    EXPECT_EQ(view->payload.size(), 5U);
    for (size_t i = 0; i < PAYLOAD.size(); ++i) {
        EXPECT_EQ(view->payload[i], PAYLOAD[i]);
    }
}

TEST(acf_lin, build_round_trip)
{
    AcfLinMessage m{};
    m.init();
    set_test_values(m);
    std::array<uint8_t, 20> out{};
    EXPECT_EQ(acf_lin_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(PAYLOAD)), 20U);
    // The pad was computed, the payload copied, the tail zeroed: byte-identical to the image.
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i], IMAGE[i]);
    }
    auto const view = acf_lin_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 5U);
    }

    // Too small an output buffer builds nothing.
    std::array<uint8_t, 19> small{};
    EXPECT_EQ(acf_lin_build(std::span<uint8_t>(small), m, std::span<uint8_t const>(PAYLOAD)), 0U);

    // An empty payload is the fixed part alone.
    std::array<uint8_t, 12> bare{};
    EXPECT_EQ(acf_lin_build(std::span<uint8_t>(bare), m, std::span<uint8_t const>{}), 12U);
    auto const empty = acf_lin_parse(std::span<uint8_t const>(bare));
    EXPECT_TRUE(empty.has_value() && empty->payload.empty());

    // One octet past the 2-quadlet maximum is rejected.
    std::vector<uint8_t> const big(9, 0x55);
    std::vector<uint8_t> room(12 + 9 + 3);
    EXPECT_EQ(acf_lin_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(big)), 0U);
    std::vector<uint8_t> const max_payload(8, 0x55);
    EXPECT_EQ(acf_lin_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(max_payload)), 20U);
}

TEST(acf_lin, parse_rejects)
{
    // Shorter than the fixed part.
    EXPECT_FALSE(acf_lin_parse(std::span<uint8_t const>(IMAGE).first(11)).has_value());
    // Declared length past the buffer.
    EXPECT_FALSE(acf_lin_parse(std::span<uint8_t const>(IMAGE).first(19)).has_value());
    // Wrong acf_msg_type.
    auto wrong_type = IMAGE;
    wrong_type[0] = 0xEC;  // ACF_CHECKSUM
    EXPECT_FALSE(acf_lin_parse(std::span<uint8_t const>(wrong_type)).has_value());
    // A pad larger than the payload.
    auto bad_pad = IMAGE;
    bad_pad[1] = 3;  // length = fixed part only, pad still 3
    EXPECT_FALSE(acf_lin_parse(std::span<uint8_t const>(bad_pad)).has_value());
    // Length below the fixed part.
    auto too_short = IMAGE;
    too_short[1] = 0x01;
    EXPECT_FALSE(acf_lin_parse(std::span<uint8_t const>(too_short)).has_value());
}

TEST(acf_lin, format_to_output)
{
    auto const view = acf_lin_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    std::string result;
    format_to(std::back_inserter(result), *view);
    EXPECT_TRUE(result.contains("ACF_LIN"));
    EXPECT_TRUE(result.contains("payload=5 octets"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_lin_test)
