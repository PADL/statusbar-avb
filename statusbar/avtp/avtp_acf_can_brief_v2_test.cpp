// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_can_brief_v2.hpp"

#include "statusbar/avtp/avtp_acf_can_brief_v2_format.hpp"
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

static_assert(sizeof(AcfCanBriefV2Message) == AcfCanBriefV2Message::LENGTH);

namespace {

/// The fixed part with every field at its test value and the fixed-only length,
/// encoded independently of the accessors (the layout oracle); the length is the
/// smallest valid message (fixed part plus the minimum payload).
constexpr std::array<uint8_t, 8> LAYOUT{0x44, 0x02, 0x4D, 0xAB, 0xC1, 0xAB, 0xCD, 0xEF};

/// A complete message on the wire: the fixed part (pad = 3, length = 4 quadlets)
/// followed by a 5-octet payload and its three pad octets.
constexpr std::array<uint8_t, 16> IMAGE{
    0x44, 0x04, 0xCD, 0xAB, 0xC1, 0xAB, 0xCD, 0xEF, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0x00, 0x00, 0x00};

constexpr std::array<uint8_t, 5> PAYLOAD{0xA1, 0xB2, 0xC3, 0xD4, 0xE5};

/// Set every field to its test value
void set_test_values(AcfCanBriefV2Message& m)
{
    m.set_pad(0x1U);
    m.set_mtv(false);
    m.set_rtr(false);
    m.set_eff(true);
    m.set_can_bus_id(0x5ABU);
    m.set_brs(true);
    m.set_fdf(true);
    m.set_esi(false);
    m.set_can_identifier(0x1ABCDEFU);
}

/// Check every field against its test value
void expect_test_values(AcfCanBriefV2Message const& m)
{
    EXPECT_EQ(m.pad(), 0x1U);
    EXPECT_FALSE(m.mtv());
    EXPECT_FALSE(m.rtr());
    EXPECT_TRUE(m.eff());
    EXPECT_EQ(m.can_bus_id(), 0x5ABU);
    EXPECT_TRUE(m.brs());
    EXPECT_TRUE(m.fdf());
    EXPECT_FALSE(m.esi());
    EXPECT_EQ(m.can_identifier(), 0x1ABCDEFU);
}

}  // namespace

TEST(acf_can_brief_v2, init_and_layout)
{
    AcfCanBriefV2Message m{};
    m.init();
    EXPECT_EQ(m.header.msg_type(), AcfMsgType::can_brief_v2);
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

TEST(acf_can_brief_v2, parse_wire_image)
{
    auto const view = acf_can_brief_v2_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    EXPECT_EQ(view->fixed.pad(), 3U);
    EXPECT_FALSE(view->fixed.mtv());
    EXPECT_FALSE(view->fixed.rtr());
    EXPECT_TRUE(view->fixed.eff());
    EXPECT_EQ(view->fixed.can_bus_id(), 0x5ABU);
    EXPECT_TRUE(view->fixed.brs());
    EXPECT_TRUE(view->fixed.fdf());
    EXPECT_FALSE(view->fixed.esi());
    EXPECT_EQ(view->fixed.can_identifier(), 0x1ABCDEFU);
    EXPECT_EQ(view->padded_payload.size(), 8U);
    EXPECT_EQ(view->payload.size(), 5U);
    for (size_t i = 0; i < PAYLOAD.size(); ++i) {
        EXPECT_EQ(view->payload[i], PAYLOAD[i]);
    }
}

TEST(acf_can_brief_v2, build_round_trip)
{
    AcfCanBriefV2Message m{};
    m.init();
    set_test_values(m);
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(acf_can_brief_v2_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(PAYLOAD)), 16U);
    // The pad was computed, the payload copied, the tail zeroed: byte-identical to the image.
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i], IMAGE[i]);
    }
    auto const view = acf_can_brief_v2_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 5U);
    }

    // Too small an output buffer builds nothing.
    std::array<uint8_t, 15> small{};
    EXPECT_EQ(acf_can_brief_v2_build(std::span<uint8_t>(small), m, std::span<uint8_t const>(PAYLOAD)), 0U);

    // An empty payload is the fixed part alone.
    std::array<uint8_t, 8> bare{};
    EXPECT_EQ(acf_can_brief_v2_build(std::span<uint8_t>(bare), m, std::span<uint8_t const>{}), 8U);
    auto const empty = acf_can_brief_v2_parse(std::span<uint8_t const>(bare));
    EXPECT_TRUE(empty.has_value() && empty->payload.empty());

    // One octet past the 16-quadlet maximum is rejected.
    std::vector<uint8_t> const big(65, 0x55);
    std::vector<uint8_t> room(8 + 65 + 3);
    EXPECT_EQ(acf_can_brief_v2_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(big)), 0U);
    std::vector<uint8_t> const max_payload(64, 0x55);
    EXPECT_EQ(acf_can_brief_v2_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(max_payload)), 72U);
}

TEST(acf_can_brief_v2, parse_rejects)
{
    // Shorter than the fixed part.
    EXPECT_FALSE(acf_can_brief_v2_parse(std::span<uint8_t const>(IMAGE).first(7)).has_value());
    // Declared length past the buffer.
    EXPECT_FALSE(acf_can_brief_v2_parse(std::span<uint8_t const>(IMAGE).first(15)).has_value());
    // Wrong acf_msg_type.
    auto wrong_type = IMAGE;
    wrong_type[0] = 0xEC;  // ACF_CHECKSUM
    EXPECT_FALSE(acf_can_brief_v2_parse(std::span<uint8_t const>(wrong_type)).has_value());
    // A pad larger than the payload.
    auto bad_pad = IMAGE;
    bad_pad[1] = 2;  // length = fixed part only, pad still 3
    EXPECT_FALSE(acf_can_brief_v2_parse(std::span<uint8_t const>(bad_pad)).has_value());
    // Length below the fixed part.
    auto too_short = IMAGE;
    too_short[1] = 0x01;
    EXPECT_FALSE(acf_can_brief_v2_parse(std::span<uint8_t const>(too_short)).has_value());
}

TEST(acf_can_brief_v2, mtv_is_ignored_and_never_sent)
{
    // Received with mtv set: ignored, still parses.
    auto with_mtv = IMAGE;
    with_mtv[2] |= 0x20U;
    auto const view = acf_can_brief_v2_parse(std::span<uint8_t const>(with_mtv));
    EXPECT_TRUE(view.has_value() && view->fixed.mtv());

    // Built with mtv set: cleared on the wire (9.4.4.1 / 9.4.19.2).
    AcfCanBriefV2Message m{};
    m.init();
    set_test_values(m);
    m.set_mtv(true);
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(acf_can_brief_v2_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(PAYLOAD)), 16U);
    EXPECT_EQ(out[2] & 0x20U, 0U);
}

TEST(acf_can_brief_v2, format_to_output)
{
    auto const view = acf_can_brief_v2_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    std::string result;
    format_to(std::back_inserter(result), *view);
    EXPECT_TRUE(result.contains("ACF_CAN_BRIEF_V2"));
    EXPECT_TRUE(result.contains("payload=5 octets"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_can_brief_v2_test)
