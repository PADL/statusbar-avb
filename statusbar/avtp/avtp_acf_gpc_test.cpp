// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_gpc.hpp"

#include "statusbar/avtp/avtp_acf_gpc_format.hpp"
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

static_assert(sizeof(AcfGpcMessage) == AcfGpcMessage::LENGTH);

namespace {

/// The fixed part with every field at its test value and the fixed-only length,
/// encoded independently of the accessors (the layout oracle); the length is the
/// smallest valid message (fixed part plus the minimum payload).
constexpr std::array<uint8_t, 8> LAYOUT{0x0A, 0x02, 0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};

/// A complete message on the wire: the fixed part (pad = 3, length = 4 quadlets)
/// followed by a 5-octet payload and its three pad octets.
constexpr std::array<uint8_t, 16> IMAGE{
    0x0A, 0x04, 0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0x00, 0x00, 0x00};

constexpr std::array<uint8_t, 5> PAYLOAD{0xA1, 0xB2, 0xC3, 0xD4, 0xE5};

/// Set every field to its test value
void set_test_values(AcfGpcMessage& m)
{
    m.set_gpc_msg_id(ieee::Eui48(0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56));
}

/// Check every field against its test value
void expect_test_values(AcfGpcMessage const& m)
{
    EXPECT_TRUE(m.get_gpc_msg_id() == ieee::Eui48(0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56));
}

}  // namespace

TEST(acf_gpc, init_and_layout)
{
    AcfGpcMessage m{};
    m.init();
    EXPECT_EQ(m.header.msg_type(), AcfMsgType::gpc);
    EXPECT_EQ(m.header.msg_length(), 2U);
    EXPECT_TRUE(m.is_valid());

    set_test_values(m);
    expect_test_values(m);
    std::array<uint8_t, 8> bytes{};
    span_store(bytes, m);
    for (size_t i = 0; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], LAYOUT[i]);
    }
}

TEST(acf_gpc, parse_wire_image)
{
    auto const view = acf_gpc_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    EXPECT_EQ(view->fixed.pad(), 0U);
    EXPECT_TRUE(view->fixed.get_gpc_msg_id() == ieee::Eui48(0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56));
    EXPECT_EQ(view->padded_payload.size(), 8U);
    EXPECT_EQ(view->payload.size(), 8U);
    for (size_t i = 0; i < PAYLOAD.size(); ++i) {
        EXPECT_EQ(view->payload[i], PAYLOAD[i]);
    }
}

TEST(acf_gpc, build_round_trip)
{
    AcfGpcMessage m{};
    m.init();
    set_test_values(m);
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(acf_gpc_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(PAYLOAD)), 16U);
    // The pad was computed, the payload copied, the tail zeroed: byte-identical to the image.
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i], IMAGE[i]);
    }
    auto const view = acf_gpc_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 8U);
    }

    // Too small an output buffer builds nothing.
    std::array<uint8_t, 15> small{};
    EXPECT_EQ(acf_gpc_build(std::span<uint8_t>(small), m, std::span<uint8_t const>(PAYLOAD)), 0U);

    // An empty payload is the fixed part alone.
    std::array<uint8_t, 8> bare{};
    EXPECT_EQ(acf_gpc_build(std::span<uint8_t>(bare), m, std::span<uint8_t const>{}), 8U);
    auto const empty = acf_gpc_parse(std::span<uint8_t const>(bare));
    EXPECT_TRUE(empty.has_value() && empty->payload.empty());

    // The 9-bit acf_msg_length caps the message at 511 quadlets.
    std::vector<uint8_t> const big(2044 - 8 + 1, 0x55);
    std::vector<uint8_t> room(2048);
    EXPECT_EQ(acf_gpc_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(big)), 0U);
    std::vector<uint8_t> const max_payload(2044 - 8, 0x55);
    EXPECT_EQ(acf_gpc_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(max_payload)), 2044U);
}

TEST(acf_gpc, parse_rejects)
{
    // Shorter than the fixed part.
    EXPECT_FALSE(acf_gpc_parse(std::span<uint8_t const>(IMAGE).first(7)).has_value());
    // Declared length past the buffer.
    EXPECT_FALSE(acf_gpc_parse(std::span<uint8_t const>(IMAGE).first(15)).has_value());
    // Wrong acf_msg_type.
    auto wrong_type = IMAGE;
    wrong_type[0] = 0xEC;  // ACF_CHECKSUM
    EXPECT_FALSE(acf_gpc_parse(std::span<uint8_t const>(wrong_type)).has_value());
    // Length below the fixed part.
    auto too_short = IMAGE;
    too_short[1] = 0x01;
    EXPECT_FALSE(acf_gpc_parse(std::span<uint8_t const>(too_short)).has_value());
}

TEST(acf_gpc, format_to_output)
{
    auto const view = acf_gpc_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    std::string result;
    format_to(std::back_inserter(result), *view);
    EXPECT_TRUE(result.contains("ACF_GPC"));
    EXPECT_TRUE(result.contains("payload=8 octets"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_gpc_test)
