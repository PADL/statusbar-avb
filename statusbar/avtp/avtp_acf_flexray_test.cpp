// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_flexray.hpp"

#include "statusbar/avtp/avtp_acf_flexray_format.hpp"
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

static_assert(sizeof(AcfFlexrayMessage) == AcfFlexrayMessage::LENGTH);

namespace {

/// The fixed part with every field at its test value and the fixed-only length,
/// encoded independently of the accessors (the layout oracle); the length is the
/// smallest valid message (fixed part plus the minimum payload).
constexpr std::array<uint8_t, 16> LAYOUT{
    0x00, 0x04, 0xB5, 0x2B, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xB4, 0xA0, 0x00, 0x2A};

/// A complete message on the wire: the fixed part (pad = 3, length = 6 quadlets)
/// followed by a 5-octet payload and its three pad octets.
constexpr std::array<uint8_t, 24> IMAGE{0x00, 0x06, 0xF5, 0x2B, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                        0xB4, 0xA0, 0x00, 0x2A, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0x00, 0x00, 0x00};

constexpr std::array<uint8_t, 5> PAYLOAD{0xA1, 0xB2, 0xC3, 0xD4, 0xE5};

/// Set every field to its test value
void set_test_values(AcfFlexrayMessage& m)
{
    m.set_pad(0x2U);
    m.set_mtv(true);
    m.set_fr_bus_id(0x15U);
    m.set_chan(0x2U);
    m.set_str(true);
    m.set_syn(false);
    m.set_pre(true);
    m.set_nfi(true);
    m.set_message_timestamp(0x102030405060708U);
    m.set_fr_frame_id(0x5A5U);
    m.set_cycle(0x2AU);
}

/// Check every field against its test value
void expect_test_values(AcfFlexrayMessage const& m)
{
    EXPECT_EQ(m.pad(), 0x2U);
    EXPECT_TRUE(m.mtv());
    EXPECT_EQ(m.fr_bus_id(), 0x15U);
    EXPECT_EQ(m.chan(), 0x2U);
    EXPECT_TRUE(m.str());
    EXPECT_FALSE(m.syn());
    EXPECT_TRUE(m.pre());
    EXPECT_TRUE(m.nfi());
    EXPECT_EQ(m.get_message_timestamp(), 0x102030405060708U);
    EXPECT_EQ(m.fr_frame_id(), 0x5A5U);
    EXPECT_EQ(m.cycle(), 0x2AU);
}

}  // namespace

TEST(acf_flexray, init_and_layout)
{
    AcfFlexrayMessage m{};
    m.init();
    EXPECT_EQ(m.header.msg_type(), AcfMsgType::flexray);
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

TEST(acf_flexray, parse_wire_image)
{
    auto const view = acf_flexray_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    EXPECT_EQ(view->fixed.pad(), 3U);
    EXPECT_TRUE(view->fixed.mtv());
    EXPECT_EQ(view->fixed.fr_bus_id(), 0x15U);
    EXPECT_EQ(view->fixed.chan(), 0x2U);
    EXPECT_TRUE(view->fixed.str());
    EXPECT_FALSE(view->fixed.syn());
    EXPECT_TRUE(view->fixed.pre());
    EXPECT_TRUE(view->fixed.nfi());
    EXPECT_EQ(view->fixed.get_message_timestamp(), 0x102030405060708U);
    EXPECT_EQ(view->fixed.fr_frame_id(), 0x5A5U);
    EXPECT_EQ(view->fixed.cycle(), 0x2AU);
    EXPECT_EQ(view->padded_payload.size(), 8U);
    EXPECT_EQ(view->payload.size(), 5U);
    for (size_t i = 0; i < PAYLOAD.size(); ++i) {
        EXPECT_EQ(view->payload[i], PAYLOAD[i]);
    }
}

TEST(acf_flexray, build_round_trip)
{
    AcfFlexrayMessage m{};
    m.init();
    set_test_values(m);
    std::array<uint8_t, 24> out{};
    EXPECT_EQ(acf_flexray_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(PAYLOAD)), 24U);
    // The pad was computed, the payload copied, the tail zeroed: byte-identical to the image.
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i], IMAGE[i]);
    }
    auto const view = acf_flexray_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 5U);
    }

    // Too small an output buffer builds nothing.
    std::array<uint8_t, 23> small{};
    EXPECT_EQ(acf_flexray_build(std::span<uint8_t>(small), m, std::span<uint8_t const>(PAYLOAD)), 0U);

    // An empty payload is the fixed part alone.
    std::array<uint8_t, 16> bare{};
    EXPECT_EQ(acf_flexray_build(std::span<uint8_t>(bare), m, std::span<uint8_t const>{}), 16U);
    auto const empty = acf_flexray_parse(std::span<uint8_t const>(bare));
    EXPECT_TRUE(empty.has_value() && empty->payload.empty());

    // One octet past the 64-quadlet maximum is rejected.
    std::vector<uint8_t> const big(257, 0x55);
    std::vector<uint8_t> room(16 + 257 + 3);
    EXPECT_EQ(acf_flexray_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(big)), 0U);
    std::vector<uint8_t> const max_payload(256, 0x55);
    EXPECT_EQ(acf_flexray_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(max_payload)), 272U);
}

TEST(acf_flexray, parse_rejects)
{
    // Shorter than the fixed part.
    EXPECT_FALSE(acf_flexray_parse(std::span<uint8_t const>(IMAGE).first(15)).has_value());
    // Declared length past the buffer.
    EXPECT_FALSE(acf_flexray_parse(std::span<uint8_t const>(IMAGE).first(23)).has_value());
    // Wrong acf_msg_type.
    auto wrong_type = IMAGE;
    wrong_type[0] = 0xEC;  // ACF_CHECKSUM
    EXPECT_FALSE(acf_flexray_parse(std::span<uint8_t const>(wrong_type)).has_value());
    // A pad larger than the payload.
    auto bad_pad = IMAGE;
    bad_pad[1] = 4;  // length = fixed part only, pad still 3
    EXPECT_FALSE(acf_flexray_parse(std::span<uint8_t const>(bad_pad)).has_value());
    // Length below the fixed part.
    auto too_short = IMAGE;
    too_short[1] = 0x01;
    EXPECT_FALSE(acf_flexray_parse(std::span<uint8_t const>(too_short)).has_value());
}

TEST(acf_flexray, format_to_output)
{
    auto const view = acf_flexray_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    std::string result;
    format_to(std::back_inserter(result), *view);
    EXPECT_TRUE(result.contains("ACF_FLEXRAY"));
    EXPECT_TRUE(result.contains("payload=5 octets"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_flexray_test)
