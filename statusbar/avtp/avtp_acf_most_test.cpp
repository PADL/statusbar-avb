// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_most.hpp"

#include "statusbar/avtp/avtp_acf_most_format.hpp"
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

static_assert(sizeof(AcfMostMessage) == AcfMostMessage::LENGTH);

namespace {

/// The fixed part with every field at its test value and the fixed-only length,
/// encoded independently of the accessors (the layout oracle); the length is the
/// smallest valid message (fixed part plus the minimum payload).
constexpr std::array<uint8_t, 20> LAYOUT{0x08, 0x05, 0x67, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                         0x07, 0x08, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0x00, 0x00};

/// A complete message on the wire: the fixed part (pad = 3, length = 7 quadlets)
/// followed by a 5-octet payload and its three pad octets.
constexpr std::array<uint8_t, 28> IMAGE{0x08, 0x07, 0xE7, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x01, 0x23,
                                        0x45, 0x67, 0x89, 0xAB, 0x00, 0x00, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0x00, 0x00, 0x00};

constexpr std::array<uint8_t, 5> PAYLOAD{0xA1, 0xB2, 0xC3, 0xD4, 0xE5};

/// Set every field to its test value
void set_test_values(AcfMostMessage& m)
{
    m.set_pad(0x1U);
    m.set_mtv(true);
    m.set_most_net_id(0x7U);
    m.set_message_timestamp(0x102030405060708U);
    m.set_device_id(0x123U);
    m.set_fblock_id(0x45U);
    m.set_inst_id(0x67U);
    m.set_func_id(0x89AU);
    m.set_op_type(0xBU);
}

/// Check every field against its test value
void expect_test_values(AcfMostMessage const& m)
{
    EXPECT_EQ(m.pad(), 0x1U);
    EXPECT_TRUE(m.mtv());
    EXPECT_EQ(m.most_net_id(), 0x7U);
    EXPECT_EQ(m.get_message_timestamp(), 0x102030405060708U);
    EXPECT_EQ(m.get_device_id(), 0x123U);
    EXPECT_EQ(m.get_fblock_id(), 0x45U);
    EXPECT_EQ(m.get_inst_id(), 0x67U);
    EXPECT_EQ(m.func_id(), 0x89AU);
    EXPECT_EQ(m.op_type(), 0xBU);
}

}  // namespace

TEST(acf_most, init_and_layout)
{
    AcfMostMessage m{};
    m.init();
    EXPECT_EQ(m.header.msg_type(), AcfMsgType::most);
    EXPECT_EQ(m.header.msg_length(), 5U);
    EXPECT_TRUE(m.is_valid());

    set_test_values(m);
    expect_test_values(m);
    std::array<uint8_t, 20> bytes{};
    span_store(bytes, m);
    for (size_t i = 0; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], LAYOUT[i]);
    }
}

TEST(acf_most, parse_wire_image)
{
    auto const view = acf_most_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    EXPECT_EQ(view->fixed.pad(), 3U);
    EXPECT_TRUE(view->fixed.mtv());
    EXPECT_EQ(view->fixed.most_net_id(), 0x7U);
    EXPECT_EQ(view->fixed.get_message_timestamp(), 0x102030405060708U);
    EXPECT_EQ(view->fixed.get_device_id(), 0x123U);
    EXPECT_EQ(view->fixed.get_fblock_id(), 0x45U);
    EXPECT_EQ(view->fixed.get_inst_id(), 0x67U);
    EXPECT_EQ(view->fixed.func_id(), 0x89AU);
    EXPECT_EQ(view->fixed.op_type(), 0xBU);
    EXPECT_EQ(view->padded_payload.size(), 8U);
    EXPECT_EQ(view->payload.size(), 5U);
    for (size_t i = 0; i < PAYLOAD.size(); ++i) {
        EXPECT_EQ(view->payload[i], PAYLOAD[i]);
    }
}

TEST(acf_most, build_round_trip)
{
    AcfMostMessage m{};
    m.init();
    set_test_values(m);
    std::array<uint8_t, 28> out{};
    EXPECT_EQ(acf_most_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(PAYLOAD)), 28U);
    // The pad was computed, the payload copied, the tail zeroed: byte-identical to the image.
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i], IMAGE[i]);
    }
    auto const view = acf_most_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 5U);
    }

    // Too small an output buffer builds nothing.
    std::array<uint8_t, 27> small{};
    EXPECT_EQ(acf_most_build(std::span<uint8_t>(small), m, std::span<uint8_t const>(PAYLOAD)), 0U);

    // An empty payload is the fixed part alone.
    std::array<uint8_t, 20> bare{};
    EXPECT_EQ(acf_most_build(std::span<uint8_t>(bare), m, std::span<uint8_t const>{}), 20U);
    auto const empty = acf_most_parse(std::span<uint8_t const>(bare));
    EXPECT_TRUE(empty.has_value() && empty->payload.empty());

    // The 9-bit acf_msg_length caps the message at 511 quadlets.
    std::vector<uint8_t> const big(2044 - 20 + 1, 0x55);
    std::vector<uint8_t> room(2048);
    EXPECT_EQ(acf_most_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(big)), 0U);
    std::vector<uint8_t> const max_payload(2044 - 20, 0x55);
    EXPECT_EQ(acf_most_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(max_payload)), 2044U);
}

TEST(acf_most, parse_rejects)
{
    // Shorter than the fixed part.
    EXPECT_FALSE(acf_most_parse(std::span<uint8_t const>(IMAGE).first(19)).has_value());
    // Declared length past the buffer.
    EXPECT_FALSE(acf_most_parse(std::span<uint8_t const>(IMAGE).first(27)).has_value());
    // Wrong acf_msg_type.
    auto wrong_type = IMAGE;
    wrong_type[0] = 0xEC;  // ACF_CHECKSUM
    EXPECT_FALSE(acf_most_parse(std::span<uint8_t const>(wrong_type)).has_value());
    // A pad larger than the payload.
    auto bad_pad = IMAGE;
    bad_pad[1] = 5;  // length = fixed part only, pad still 3
    EXPECT_FALSE(acf_most_parse(std::span<uint8_t const>(bad_pad)).has_value());
    // Length below the fixed part.
    auto too_short = IMAGE;
    too_short[1] = 0x01;
    EXPECT_FALSE(acf_most_parse(std::span<uint8_t const>(too_short)).has_value());
}

TEST(acf_most, format_to_output)
{
    auto const view = acf_most_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    std::string result;
    format_to(std::back_inserter(result), *view);
    EXPECT_TRUE(result.contains("ACF_MOST"));
    EXPECT_TRUE(result.contains("payload=5 octets"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_most_test)
