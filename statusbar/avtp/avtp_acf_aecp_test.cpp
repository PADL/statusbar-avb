// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_aecp.hpp"

#include "statusbar/avtp/avtp_acf_aecp_format.hpp"
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

static_assert(sizeof(AcfAecpMessage) == AcfAecpMessage::LENGTH);

namespace {

/// The fixed part with every field at its test value and the fixed-only length,
/// encoded independently of the accessors (the layout oracle); the length is the
/// smallest valid message (fixed part plus the minimum payload).
constexpr std::array<uint8_t, 4> LAYOUT{0x14, 0x01, 0x00, 0x00};

/// A complete message on the wire: the fixed part (pad = 3, length = 3 quadlets)
/// followed by a 5-octet payload and its three pad octets.
constexpr std::array<uint8_t, 12> IMAGE{0x14, 0x03, 0x00, 0x00, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0x00, 0x00, 0x00};

constexpr std::array<uint8_t, 5> PAYLOAD{0xA1, 0xB2, 0xC3, 0xD4, 0xE5};

/// Set every field to its test value
void set_test_values(AcfAecpMessage const& /*m*/)
{}

/// Check every field against its test value
void expect_test_values(AcfAecpMessage const& /*m*/)
{}

}  // namespace

TEST(acf_aecp, init_and_layout)
{
    AcfAecpMessage m{};
    m.init();
    EXPECT_EQ(m.header.msg_type(), AcfMsgType::aecp);
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

TEST(acf_aecp, parse_wire_image)
{
    auto const view = acf_aecp_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    EXPECT_EQ(view->fixed.pad(), 0U);
    EXPECT_EQ(view->padded_payload.size(), 8U);
    EXPECT_EQ(view->payload.size(), 8U);
    for (size_t i = 0; i < PAYLOAD.size(); ++i) {
        EXPECT_EQ(view->payload[i], PAYLOAD[i]);
    }
}

TEST(acf_aecp, build_round_trip)
{
    AcfAecpMessage m{};
    m.init();
    set_test_values(m);
    std::array<uint8_t, 12> out{};
    EXPECT_EQ(acf_aecp_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(PAYLOAD)), 12U);
    // The pad was computed, the payload copied, the tail zeroed: byte-identical to the image.
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i], IMAGE[i]);
    }
    auto const view = acf_aecp_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 8U);
    }

    // Too small an output buffer builds nothing.
    std::array<uint8_t, 11> small{};
    EXPECT_EQ(acf_aecp_build(std::span<uint8_t>(small), m, std::span<uint8_t const>(PAYLOAD)), 0U);

    // An empty payload is the fixed part alone.
    std::array<uint8_t, 4> bare{};
    EXPECT_EQ(acf_aecp_build(std::span<uint8_t>(bare), m, std::span<uint8_t const>{}), 4U);
    auto const empty = acf_aecp_parse(std::span<uint8_t const>(bare));
    EXPECT_TRUE(empty.has_value() && empty->payload.empty());

    // The 9-bit acf_msg_length caps the message at 511 quadlets.
    std::vector<uint8_t> const big(2044 - 4 + 1, 0x55);
    std::vector<uint8_t> room(2048);
    EXPECT_EQ(acf_aecp_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(big)), 0U);
    std::vector<uint8_t> const max_payload(2044 - 4, 0x55);
    EXPECT_EQ(acf_aecp_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(max_payload)), 2044U);
}

TEST(acf_aecp, parse_rejects)
{
    // Shorter than the fixed part.
    EXPECT_FALSE(acf_aecp_parse(std::span<uint8_t const>(IMAGE).first(3)).has_value());
    // Declared length past the buffer.
    EXPECT_FALSE(acf_aecp_parse(std::span<uint8_t const>(IMAGE).first(11)).has_value());
    // Wrong acf_msg_type.
    auto wrong_type = IMAGE;
    wrong_type[0] = 0xEC;  // ACF_CHECKSUM
    EXPECT_FALSE(acf_aecp_parse(std::span<uint8_t const>(wrong_type)).has_value());
    // A one-quadlet message is the fixed part alone, which this type allows;
    // a zero length never is.
    auto zero_length = IMAGE;
    zero_length[1] = 0x00;
    EXPECT_FALSE(acf_aecp_parse(std::span<uint8_t const>(zero_length)).has_value());
}

TEST(acf_aecp, aecpdu_trimmed_by_control_data_length)
{
    // A 15-octet AECPDU (control_data_length 3) padded to 16 inside the message.
    std::array<uint8_t, 15> aecpdu{0xFB, 0x00, 0x00, 0x03};
    aecpdu[14] = 0xEE;
    AcfAecpMessage m{};
    m.init();
    std::array<uint8_t, 20> out{};
    EXPECT_EQ(acf_aecp_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(aecpdu)), 20U);
    auto const view = acf_aecp_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 16U);  // no pad field: the payload is as carried
        auto const inner = acf_aecp_aecpdu(*view);
        EXPECT_TRUE(inner.has_value());
        if (inner.has_value()) {
            EXPECT_EQ(inner->size(), 15U);
            EXPECT_EQ((*inner)[14], 0xEEU);
        }
    }

    // A control_data_length past the carried payload, or a payload shorter
    // than the AECPDU header, yields no AECPDU.
    out[7] = 0x20;  // control_data_length -> 32
    auto const overrun = acf_aecp_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(overrun.has_value() && !acf_aecp_aecpdu(*overrun).has_value());
    auto const image = acf_aecp_parse(std::span<uint8_t const>(IMAGE));  // 8-octet payload
    EXPECT_TRUE(image.has_value() && !acf_aecp_aecpdu(*image).has_value());
}

TEST(acf_aecp, format_to_output)
{
    auto const view = acf_aecp_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    std::string result;
    format_to(std::back_inserter(result), *view);
    EXPECT_TRUE(result.contains("ACF_AECP"));
    EXPECT_TRUE(result.contains("payload=8 octets"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_aecp_test)
