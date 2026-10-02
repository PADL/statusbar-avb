// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_byte_bus.hpp"

#include "statusbar/avtp/avtp_acf_byte_bus_format.hpp"
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

static_assert(sizeof(AcfByteBusMessage) == AcfByteBusMessage::LENGTH);

namespace {

/// The fixed part with every field at its test value and the fixed-only length,
/// encoded independently of the accessors (the layout oracle); the length is the
/// smallest valid message (fixed part plus the minimum payload).
constexpr std::array<uint8_t, 16> LAYOUT{
    0x1A, 0x04, 0x63, 0xC5, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xA2, 0x5C, 0x99, 0xAB};

/// A complete message on the wire: the fixed part (pad = 3, length = 6 quadlets)
/// followed by a 5-octet payload and its three pad octets.
constexpr std::array<uint8_t, 24> IMAGE{0x1A, 0x06, 0xE3, 0xC5, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                        0xA2, 0x5C, 0x99, 0xAB, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0x00, 0x00, 0x00};

constexpr std::array<uint8_t, 5> PAYLOAD{0xA1, 0xB2, 0xC3, 0xD4, 0xE5};

/// Set every field to its test value
void set_test_values(AcfByteBusMessage& m)
{
    m.set_pad(0x1U);
    m.set_mtv(true);
    m.set_byte_bus_id(0x3C5U);
    m.set_message_timestamp(0x102030405060708U);
    m.set_evt(0xAU);
    m.set_hs(true);
    m.set_cs(false);
    m.set_transaction_num(0x5CU);
    m.set_op(true);
    m.set_rsp(false);
    m.set_err(false);
    m.set_ms(true);
    m.set_read_size_segment_num(0x9ABU);
}

/// Check every field against its test value
void expect_test_values(AcfByteBusMessage const& m)
{
    EXPECT_EQ(m.pad(), 0x1U);
    EXPECT_TRUE(m.mtv());
    EXPECT_EQ(m.byte_bus_id(), 0x3C5U);
    EXPECT_EQ(m.get_message_timestamp(), 0x102030405060708U);
    EXPECT_EQ(m.evt(), 0xAU);
    EXPECT_TRUE(m.hs());
    EXPECT_FALSE(m.cs());
    EXPECT_EQ(m.transaction_num(), 0x5CU);
    EXPECT_TRUE(m.op());
    EXPECT_FALSE(m.rsp());
    EXPECT_FALSE(m.err());
    EXPECT_TRUE(m.ms());
    EXPECT_EQ(m.read_size_segment_num(), 0x9ABU);
}

}  // namespace

TEST(acf_byte_bus, init_and_layout)
{
    AcfByteBusMessage m{};
    m.init();
    EXPECT_EQ(m.header.msg_type(), AcfMsgType::byte_bus);
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

TEST(acf_byte_bus, parse_wire_image)
{
    auto const view = acf_byte_bus_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    EXPECT_EQ(view->fixed.pad(), 3U);
    EXPECT_TRUE(view->fixed.mtv());
    EXPECT_EQ(view->fixed.byte_bus_id(), 0x3C5U);
    EXPECT_EQ(view->fixed.get_message_timestamp(), 0x102030405060708U);
    EXPECT_EQ(view->fixed.evt(), 0xAU);
    EXPECT_TRUE(view->fixed.hs());
    EXPECT_FALSE(view->fixed.cs());
    EXPECT_EQ(view->fixed.transaction_num(), 0x5CU);
    EXPECT_TRUE(view->fixed.op());
    EXPECT_FALSE(view->fixed.rsp());
    EXPECT_FALSE(view->fixed.err());
    EXPECT_TRUE(view->fixed.ms());
    EXPECT_EQ(view->fixed.read_size_segment_num(), 0x9ABU);
    EXPECT_EQ(view->padded_payload.size(), 8U);
    EXPECT_EQ(view->payload.size(), 5U);
    for (size_t i = 0; i < PAYLOAD.size(); ++i) {
        EXPECT_EQ(view->payload[i], PAYLOAD[i]);
    }
}

TEST(acf_byte_bus, build_round_trip)
{
    AcfByteBusMessage m{};
    m.init();
    set_test_values(m);
    std::array<uint8_t, 24> out{};
    EXPECT_EQ(acf_byte_bus_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(PAYLOAD)), 24U);
    // The pad was computed, the payload copied, the tail zeroed: byte-identical to the image.
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i], IMAGE[i]);
    }
    auto const view = acf_byte_bus_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 5U);
    }

    // Too small an output buffer builds nothing.
    std::array<uint8_t, 23> small{};
    EXPECT_EQ(acf_byte_bus_build(std::span<uint8_t>(small), m, std::span<uint8_t const>(PAYLOAD)), 0U);

    // An empty payload is the fixed part alone.
    std::array<uint8_t, 16> bare{};
    EXPECT_EQ(acf_byte_bus_build(std::span<uint8_t>(bare), m, std::span<uint8_t const>{}), 16U);
    auto const empty = acf_byte_bus_parse(std::span<uint8_t const>(bare));
    EXPECT_TRUE(empty.has_value() && empty->payload.empty());

    // The 9-bit acf_msg_length caps the message at 511 quadlets.
    std::vector<uint8_t> const big(2044 - 16 + 1, 0x55);
    std::vector<uint8_t> room(2048);
    EXPECT_EQ(acf_byte_bus_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(big)), 0U);
    std::vector<uint8_t> const max_payload(2044 - 16, 0x55);
    EXPECT_EQ(acf_byte_bus_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(max_payload)), 2044U);
}

TEST(acf_byte_bus, parse_rejects)
{
    // Shorter than the fixed part.
    EXPECT_FALSE(acf_byte_bus_parse(std::span<uint8_t const>(IMAGE).first(15)).has_value());
    // Declared length past the buffer.
    EXPECT_FALSE(acf_byte_bus_parse(std::span<uint8_t const>(IMAGE).first(23)).has_value());
    // Wrong acf_msg_type.
    auto wrong_type = IMAGE;
    wrong_type[0] = 0xEC;  // ACF_CHECKSUM
    EXPECT_FALSE(acf_byte_bus_parse(std::span<uint8_t const>(wrong_type)).has_value());
    // A pad larger than the payload.
    auto bad_pad = IMAGE;
    bad_pad[1] = 4;  // length = fixed part only, pad still 3
    EXPECT_FALSE(acf_byte_bus_parse(std::span<uint8_t const>(bad_pad)).has_value());
    // Length below the fixed part.
    auto too_short = IMAGE;
    too_short[1] = 0x01;
    EXPECT_FALSE(acf_byte_bus_parse(std::span<uint8_t const>(too_short)).has_value());
}

TEST(acf_byte_bus, read_size_units)
{
    EXPECT_EQ(acf_byte_bus_read_size_octets(0x0000U), 0U);
    EXPECT_EQ(acf_byte_bus_read_size_octets(0x07FFU), 2047U);
    EXPECT_EQ(acf_byte_bus_read_size_octets(0x0800U), 1024U);
    EXPECT_TRUE(acf_byte_bus_read_size_unsegmented(0x0800U));
    EXPECT_EQ(acf_byte_bus_read_size_octets(0x0801U), 1024U);
    EXPECT_FALSE(acf_byte_bus_read_size_unsegmented(0x0801U));
    EXPECT_EQ(acf_byte_bus_read_size_octets(0x0802U), 2048U);
    EXPECT_EQ(acf_byte_bus_read_size_octets(0x0FFFU), size_t{2047} * 1024U);
}

TEST(acf_byte_bus, format_to_output)
{
    auto const view = acf_byte_bus_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    std::string result;
    format_to(std::back_inserter(result), *view);
    EXPECT_TRUE(result.contains("ACF_BYTE_BUS"));
    EXPECT_TRUE(result.contains("payload=5 octets"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_byte_bus_test)
