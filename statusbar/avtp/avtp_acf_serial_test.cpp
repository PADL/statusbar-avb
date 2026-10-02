// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_serial.hpp"

#include "statusbar/avtp/avtp_acf_serial_format.hpp"
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

static_assert(sizeof(AcfSerialMessage) == AcfSerialMessage::LENGTH);

namespace {

/// The fixed part with every field at its test value and the fixed-only length,
/// encoded independently of the accessors (the layout oracle); the length is the
/// smallest valid message (fixed part plus the minimum payload).
constexpr std::array<uint8_t, 4> LAYOUT{0x0C, 0x01, 0x80, 0x2D};

/// A complete message on the wire: the fixed part (pad = 3, length = 3 quadlets)
/// followed by a 5-octet payload and its three pad octets.
constexpr std::array<uint8_t, 12> IMAGE{0x0C, 0x03, 0xC0, 0x2D, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0x00, 0x00, 0x00};

constexpr std::array<uint8_t, 5> PAYLOAD{0xA1, 0xB2, 0xC3, 0xD4, 0xE5};

/// Set every field to its test value
void set_test_values(AcfSerialMessage& m)
{
    m.set_pad(0x2U);
    m.set_dcd(true);
    m.set_dtr(false);
    m.set_dsr(true);
    m.set_rts(true);
    m.set_cts(false);
    m.set_ri(true);
}

/// Check every field against its test value
void expect_test_values(AcfSerialMessage const& m)
{
    EXPECT_EQ(m.pad(), 0x2U);
    EXPECT_TRUE(m.dcd());
    EXPECT_FALSE(m.dtr());
    EXPECT_TRUE(m.dsr());
    EXPECT_TRUE(m.rts());
    EXPECT_FALSE(m.cts());
    EXPECT_TRUE(m.ri());
}

}  // namespace

TEST(acf_serial, init_and_layout)
{
    AcfSerialMessage m{};
    m.init();
    EXPECT_EQ(m.header.msg_type(), AcfMsgType::serial);
    EXPECT_EQ(m.header.msg_length(), 1U);
    EXPECT_TRUE(m.is_valid());

    set_test_values(m);
    expect_test_values(m);
    std::array<uint8_t, 4> bytes{};
    span_store(bytes, m);
    for (size_t i = 0; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], LAYOUT[i]);
    }
}

TEST(acf_serial, parse_wire_image)
{
    auto const view = acf_serial_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    EXPECT_EQ(view->fixed.pad(), 3U);
    EXPECT_TRUE(view->fixed.dcd());
    EXPECT_FALSE(view->fixed.dtr());
    EXPECT_TRUE(view->fixed.dsr());
    EXPECT_TRUE(view->fixed.rts());
    EXPECT_FALSE(view->fixed.cts());
    EXPECT_TRUE(view->fixed.ri());
    EXPECT_EQ(view->padded_payload.size(), 8U);
    EXPECT_EQ(view->payload.size(), 5U);
    for (size_t i = 0; i < PAYLOAD.size(); ++i) {
        EXPECT_EQ(view->payload[i], PAYLOAD[i]);
    }
}

TEST(acf_serial, build_round_trip)
{
    AcfSerialMessage m{};
    m.init();
    set_test_values(m);
    std::array<uint8_t, 12> out{};
    EXPECT_EQ(acf_serial_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(PAYLOAD)), 12U);
    // The pad was computed, the payload copied, the tail zeroed: byte-identical to the image.
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i], IMAGE[i]);
    }
    auto const view = acf_serial_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 5U);
    }

    // Too small an output buffer builds nothing.
    std::array<uint8_t, 11> small{};
    EXPECT_EQ(acf_serial_build(std::span<uint8_t>(small), m, std::span<uint8_t const>(PAYLOAD)), 0U);

    // An empty payload is the fixed part alone.
    std::array<uint8_t, 4> bare{};
    EXPECT_EQ(acf_serial_build(std::span<uint8_t>(bare), m, std::span<uint8_t const>{}), 4U);
    auto const empty = acf_serial_parse(std::span<uint8_t const>(bare));
    EXPECT_TRUE(empty.has_value() && empty->payload.empty());

    // The 9-bit acf_msg_length caps the message at 511 quadlets.
    std::vector<uint8_t> const big(2044 - 4 + 1, 0x55);
    std::vector<uint8_t> room(2048);
    EXPECT_EQ(acf_serial_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(big)), 0U);
    std::vector<uint8_t> const max_payload(2044 - 4, 0x55);
    EXPECT_EQ(acf_serial_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(max_payload)), 2044U);
}

TEST(acf_serial, parse_rejects)
{
    // Shorter than the fixed part.
    EXPECT_FALSE(acf_serial_parse(std::span<uint8_t const>(IMAGE).first(3)).has_value());
    // Declared length past the buffer.
    EXPECT_FALSE(acf_serial_parse(std::span<uint8_t const>(IMAGE).first(11)).has_value());
    // Wrong acf_msg_type.
    auto wrong_type = IMAGE;
    wrong_type[0] = 0xEC;  // ACF_CHECKSUM
    EXPECT_FALSE(acf_serial_parse(std::span<uint8_t const>(wrong_type)).has_value());
    // A pad larger than the payload.
    auto bad_pad = IMAGE;
    bad_pad[1] = 1;  // length = fixed part only, pad still 3
    EXPECT_FALSE(acf_serial_parse(std::span<uint8_t const>(bad_pad)).has_value());
    // A one-quadlet message is the fixed part alone, which this type allows;
    // a zero length never is.
    auto zero_length = IMAGE;
    zero_length[1] = 0x00;
    EXPECT_FALSE(acf_serial_parse(std::span<uint8_t const>(zero_length)).has_value());
}

TEST(acf_serial, format_to_output)
{
    auto const view = acf_serial_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    std::string result;
    format_to(std::back_inserter(result), *view);
    EXPECT_TRUE(result.contains("ACF_SERIAL"));
    EXPECT_TRUE(result.contains("payload=5 octets"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_serial_test)
