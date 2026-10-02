// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_ancillary.hpp"

#include "statusbar/avtp/avtp_acf_ancillary_format.hpp"
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

static_assert(sizeof(AcfAncillaryMessage) == AcfAncillaryMessage::LENGTH);

namespace {

/// The fixed part with every field at its test value and the fixed-only length,
/// encoded independently of the accessors (the layout oracle); the length is the
/// smallest valid message (fixed part plus the minimum payload).
constexpr std::array<uint8_t, 8> LAYOUT{0x16, 0x02, 0x80, 0x06, 0x02, 0xA5, 0x61, 0x02};

/// A complete message on the wire: the fixed part (pad = 3, length = 4 quadlets)
/// followed by a 5-octet payload and its three pad octets.
constexpr std::array<uint8_t, 16> IMAGE{
    0x16, 0x04, 0xC0, 0x06, 0x02, 0xA5, 0x61, 0x02, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0x00, 0x00, 0x00};

constexpr std::array<uint8_t, 5> PAYLOAD{0xA1, 0xB2, 0xC3, 0xD4, 0xE5};

/// Set every field to its test value
void set_test_values(AcfAncillaryMessage& m)
{
    m.set_pad(0x2U);
    m.set_mode(0x1U);
    m.set_fp(true);
    m.set_lp(false);
    m.set_line_number(0x2A5U);
    m.set_did(0x61U);
    m.set_sdid_dbn(0x2U);
}

/// Check every field against its test value
void expect_test_values(AcfAncillaryMessage const& m)
{
    EXPECT_EQ(m.pad(), 0x2U);
    EXPECT_EQ(m.mode(), 0x1U);
    EXPECT_TRUE(m.fp());
    EXPECT_FALSE(m.lp());
    EXPECT_EQ(m.get_line_number(), 0x2A5U);
    EXPECT_EQ(m.get_did(), 0x61U);
    EXPECT_EQ(m.get_sdid_dbn(), 0x2U);
}

}  // namespace

TEST(acf_ancillary, init_and_layout)
{
    AcfAncillaryMessage m{};
    m.init();
    EXPECT_EQ(m.header.msg_type(), AcfMsgType::ancillary);
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

TEST(acf_ancillary, parse_wire_image)
{
    auto const view = acf_ancillary_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    EXPECT_EQ(view->fixed.pad(), 3U);
    EXPECT_EQ(view->fixed.mode(), 0x1U);
    EXPECT_TRUE(view->fixed.fp());
    EXPECT_FALSE(view->fixed.lp());
    EXPECT_EQ(view->fixed.get_line_number(), 0x2A5U);
    EXPECT_EQ(view->fixed.get_did(), 0x61U);
    EXPECT_EQ(view->fixed.get_sdid_dbn(), 0x2U);
    EXPECT_EQ(view->padded_payload.size(), 8U);
    EXPECT_EQ(view->payload.size(), 5U);
    for (size_t i = 0; i < PAYLOAD.size(); ++i) {
        EXPECT_EQ(view->payload[i], PAYLOAD[i]);
    }
}

TEST(acf_ancillary, build_round_trip)
{
    AcfAncillaryMessage m{};
    m.init();
    set_test_values(m);
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(acf_ancillary_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(PAYLOAD)), 16U);
    // The pad was computed, the payload copied, the tail zeroed: byte-identical to the image.
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i], IMAGE[i]);
    }
    auto const view = acf_ancillary_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 5U);
    }

    // Too small an output buffer builds nothing.
    std::array<uint8_t, 15> small{};
    EXPECT_EQ(acf_ancillary_build(std::span<uint8_t>(small), m, std::span<uint8_t const>(PAYLOAD)), 0U);

    // An empty payload is the fixed part alone.
    std::array<uint8_t, 8> bare{};
    EXPECT_EQ(acf_ancillary_build(std::span<uint8_t>(bare), m, std::span<uint8_t const>{}), 8U);
    auto const empty = acf_ancillary_parse(std::span<uint8_t const>(bare));
    EXPECT_TRUE(empty.has_value() && empty->payload.empty());

    // The 9-bit acf_msg_length caps the message at 511 quadlets.
    std::vector<uint8_t> const big(2044 - 8 + 1, 0x55);
    std::vector<uint8_t> room(2048);
    EXPECT_EQ(acf_ancillary_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(big)), 0U);
    std::vector<uint8_t> const max_payload(2044 - 8, 0x55);
    EXPECT_EQ(acf_ancillary_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(max_payload)), 2044U);
}

TEST(acf_ancillary, parse_rejects)
{
    // Shorter than the fixed part.
    EXPECT_FALSE(acf_ancillary_parse(std::span<uint8_t const>(IMAGE).first(7)).has_value());
    // Declared length past the buffer.
    EXPECT_FALSE(acf_ancillary_parse(std::span<uint8_t const>(IMAGE).first(15)).has_value());
    // Wrong acf_msg_type.
    auto wrong_type = IMAGE;
    wrong_type[0] = 0xEC;  // ACF_CHECKSUM
    EXPECT_FALSE(acf_ancillary_parse(std::span<uint8_t const>(wrong_type)).has_value());
    // A pad larger than the payload.
    auto bad_pad = IMAGE;
    bad_pad[1] = 2;  // length = fixed part only, pad still 3
    EXPECT_FALSE(acf_ancillary_parse(std::span<uint8_t const>(bad_pad)).has_value());
    // Length below the fixed part.
    auto too_short = IMAGE;
    too_short[1] = 0x01;
    EXPECT_FALSE(acf_ancillary_parse(std::span<uint8_t const>(too_short)).has_value());
}

TEST(acf_ancillary, ten_bit_packing_figure_83)
{
    // Seven 10-bit words -> three quadlets; the third carries one word and two zeros.
    constexpr std::array<uint16_t, 7> words{0x3FF, 0x001, 0x2AA, 0x155, 0x200, 0x0FF, 0x123};
    std::array<uint8_t, 12> packed{};
    EXPECT_EQ(acf_anc_pack_10bit(std::span<uint16_t const>(words), std::span<uint8_t>(packed)), 12U);
    // 0x3FF<<22 | 0x001<<12 | 0x2AA<<2 = 0xFFC01AA8
    EXPECT_EQ(packed[0], 0xFFU);
    EXPECT_EQ(packed[1], 0xC0U);
    EXPECT_EQ(packed[2], 0x1AU);
    EXPECT_EQ(packed[3], 0xA8U);
    // 0x123<<22 = 0x48C00000
    EXPECT_EQ(packed[8], 0x48U);
    EXPECT_EQ(packed[9], 0xC0U);
    EXPECT_EQ(packed[10], 0x00U);
    EXPECT_EQ(packed[11], 0x00U);

    std::array<uint16_t, 9> unpacked{};
    EXPECT_EQ(acf_anc_unpack_10bit(std::span<uint8_t const>(packed), std::span<uint16_t>(unpacked)), 9U);
    for (size_t i = 0; i < words.size(); ++i) {
        EXPECT_EQ(unpacked[i], words[i]);
    }
    EXPECT_EQ(unpacked[7], 0U);
    EXPECT_EQ(unpacked[8], 0U);

    // Bounds: a word past 10 bits, and outputs that are too small.
    constexpr std::array<uint16_t, 1> wide{0x400};
    EXPECT_EQ(acf_anc_pack_10bit(std::span<uint16_t const>(wide), std::span<uint8_t>(packed)), 0U);
    std::array<uint8_t, 11> small{};
    EXPECT_EQ(acf_anc_pack_10bit(std::span<uint16_t const>(words), std::span<uint8_t>(small)), 0U);
    std::array<uint16_t, 8> few{};
    EXPECT_EQ(acf_anc_unpack_10bit(std::span<uint8_t const>(packed), std::span<uint16_t>(few)), 0U);

    // A 10-bit-mode message round trip carries the packed words as its payload.
    AcfAncillaryMessage m{};
    m.init();
    m.set_mode(AcfAncMode::anc_10bit);
    m.set_fp(true);
    m.set_lp(true);
    m.set_line_number(21);
    std::array<uint8_t, 20> out{};
    EXPECT_EQ(acf_ancillary_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(packed)), 20U);
    auto const view = acf_ancillary_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->fixed.mode(), AcfAncMode::anc_10bit);
        EXPECT_EQ(view->fixed.pad(), 0U);
        EXPECT_EQ(view->payload.size(), 12U);
    }
}

TEST(acf_ancillary, format_to_output)
{
    auto const view = acf_ancillary_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    std::string result;
    format_to(std::back_inserter(result), *view);
    EXPECT_TRUE(result.contains("ACF_ANCILLARY"));
    EXPECT_TRUE(result.contains("payload=5 octets"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_ancillary_test)
