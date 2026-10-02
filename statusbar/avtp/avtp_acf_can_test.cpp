// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_can.hpp"

#include "statusbar/avtp/avtp_acf_can_format.hpp"
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

static_assert(sizeof(AcfCanMessage) == AcfCanMessage::LENGTH);

namespace {

/// The fixed part with every field at its test value and the fixed-only length,
/// encoded independently of the accessors (the layout oracle); the length is the
/// smallest valid message (fixed part plus the minimum payload).
constexpr std::array<uint8_t, 16> LAYOUT{
    0x02, 0x04, 0x6E, 0x13, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x01, 0xAB, 0xCD, 0xEF};

/// A complete message on the wire: the fixed part (pad = 3, length = 6 quadlets)
/// followed by a 5-octet payload and its three pad octets.
constexpr std::array<uint8_t, 24> IMAGE{0x02, 0x06, 0xEE, 0x13, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                        0x01, 0xAB, 0xCD, 0xEF, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0x00, 0x00, 0x00};

constexpr std::array<uint8_t, 5> PAYLOAD{0xA1, 0xB2, 0xC3, 0xD4, 0xE5};

/// Set every field to its test value
void set_test_values(AcfCanMessage& m)
{
    m.set_pad(0x1U);
    m.set_mtv(true);
    m.set_rtr(false);
    m.set_eff(true);
    m.set_brs(true);
    m.set_fdf(true);
    m.set_esi(false);
    m.set_can_bus_id(0x13U);
    m.set_message_timestamp(0x102030405060708U);
    m.set_can_identifier(0x1ABCDEFU);
}

/// Check every field against its test value
void expect_test_values(AcfCanMessage const& m)
{
    EXPECT_EQ(m.pad(), 0x1U);
    EXPECT_TRUE(m.mtv());
    EXPECT_FALSE(m.rtr());
    EXPECT_TRUE(m.eff());
    EXPECT_TRUE(m.brs());
    EXPECT_TRUE(m.fdf());
    EXPECT_FALSE(m.esi());
    EXPECT_EQ(m.can_bus_id(), 0x13U);
    EXPECT_EQ(m.get_message_timestamp(), 0x102030405060708U);
    EXPECT_EQ(m.can_identifier(), 0x1ABCDEFU);
}

}  // namespace

TEST(acf_can, init_and_layout)
{
    AcfCanMessage m{};
    m.init();
    EXPECT_EQ(m.header.msg_type(), AcfMsgType::can);
    EXPECT_EQ(m.header.msg_length(), 4U);
    EXPECT_TRUE(m.is_valid());

    set_test_values(m);
    expect_test_values(m);
    std::array<uint8_t, 16> bytes{};
    span_store(bytes, m);
    for (size_t i = 0; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], LAYOUT[i]);
    }
}

TEST(acf_can, parse_wire_image)
{
    auto const view = acf_can_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    EXPECT_EQ(view->fixed.pad(), 3U);
    EXPECT_TRUE(view->fixed.mtv());
    EXPECT_FALSE(view->fixed.rtr());
    EXPECT_TRUE(view->fixed.eff());
    EXPECT_TRUE(view->fixed.brs());
    EXPECT_TRUE(view->fixed.fdf());
    EXPECT_FALSE(view->fixed.esi());
    EXPECT_EQ(view->fixed.can_bus_id(), 0x13U);
    EXPECT_EQ(view->fixed.get_message_timestamp(), 0x102030405060708U);
    EXPECT_EQ(view->fixed.can_identifier(), 0x1ABCDEFU);
    EXPECT_EQ(view->padded_payload.size(), 8U);
    EXPECT_EQ(view->payload.size(), 5U);
    for (size_t i = 0; i < PAYLOAD.size(); ++i) {
        EXPECT_EQ(view->payload[i], PAYLOAD[i]);
    }
}

TEST(acf_can, build_round_trip)
{
    AcfCanMessage m{};
    m.init();
    set_test_values(m);
    std::array<uint8_t, 24> out{};
    EXPECT_EQ(acf_can_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(PAYLOAD)), 24U);
    // The pad was computed, the payload copied, the tail zeroed: byte-identical to the image.
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i], IMAGE[i]);
    }
    auto const view = acf_can_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 5U);
    }

    // Too small an output buffer builds nothing.
    std::array<uint8_t, 23> small{};
    EXPECT_EQ(acf_can_build(std::span<uint8_t>(small), m, std::span<uint8_t const>(PAYLOAD)), 0U);

    // An empty payload is the fixed part alone.
    std::array<uint8_t, 16> bare{};
    EXPECT_EQ(acf_can_build(std::span<uint8_t>(bare), m, std::span<uint8_t const>{}), 16U);
    auto const empty = acf_can_parse(std::span<uint8_t const>(bare));
    EXPECT_TRUE(empty.has_value() && empty->payload.empty());

    // One octet past the 16-quadlet maximum is rejected.
    std::vector<uint8_t> const big(65, 0x55);
    std::vector<uint8_t> room(16 + 65 + 3);
    EXPECT_EQ(acf_can_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(big)), 0U);
    std::vector<uint8_t> const max_payload(64, 0x55);
    EXPECT_EQ(acf_can_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(max_payload)), 80U);
}

TEST(acf_can, parse_rejects)
{
    // Shorter than the fixed part.
    EXPECT_FALSE(acf_can_parse(std::span<uint8_t const>(IMAGE).first(15)).has_value());
    // Declared length past the buffer.
    EXPECT_FALSE(acf_can_parse(std::span<uint8_t const>(IMAGE).first(23)).has_value());
    // Wrong acf_msg_type.
    auto wrong_type = IMAGE;
    wrong_type[0] = 0xEC;  // ACF_CHECKSUM
    EXPECT_FALSE(acf_can_parse(std::span<uint8_t const>(wrong_type)).has_value());
    // A pad larger than the payload.
    auto bad_pad = IMAGE;
    bad_pad[1] = 4;  // length = fixed part only, pad still 3
    EXPECT_FALSE(acf_can_parse(std::span<uint8_t const>(bad_pad)).has_value());
    // Length below the fixed part.
    auto too_short = IMAGE;
    too_short[1] = 0x01;
    EXPECT_FALSE(acf_can_parse(std::span<uint8_t const>(too_short)).has_value());
}

TEST(acf_can, format_to_output)
{
    auto const view = acf_can_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    std::string result;
    format_to(std::back_inserter(result), *view);
    EXPECT_TRUE(result.contains("ACF_CAN"));
    EXPECT_TRUE(result.contains("payload=5 octets"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_can_test)
