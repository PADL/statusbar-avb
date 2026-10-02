// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_parallel.hpp"

#include "statusbar/avtp/avtp_acf_parallel_format.hpp"
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

static_assert(sizeof(AcfParallelMessage) == AcfParallelMessage::LENGTH);

namespace {

/// The fixed part with every field at its test value and the fixed-only length,
/// encoded independently of the accessors (the layout oracle); the length is the
/// smallest valid message (fixed part plus the minimum payload).
constexpr std::array<uint8_t, 4> LAYOUT{0x0E, 0x02, 0x00, 0x25};

/// A complete message on the wire: the fixed part (pad = 3, length = 3 quadlets)
/// followed by a 5-octet payload and its three pad octets.
constexpr std::array<uint8_t, 12> IMAGE{0x0E, 0x03, 0x00, 0x25, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0x00, 0x00, 0x00};

constexpr std::array<uint8_t, 5> PAYLOAD{0xA1, 0xB2, 0xC3, 0xD4, 0xE5};

/// Set every field to its test value
void set_test_values(AcfParallelMessage& m)
{
    m.set_bit_width(0x25U);
}

/// Check every field against its test value
void expect_test_values(AcfParallelMessage const& m)
{
    EXPECT_EQ(m.get_bit_width(), 0x25U);
}

}  // namespace

TEST(acf_parallel, init_and_layout)
{
    AcfParallelMessage m{};
    m.init();
    EXPECT_EQ(m.header.msg_type(), AcfMsgType::parallel);
    EXPECT_EQ(m.header.msg_length(), 2U);

    set_test_values(m);
    expect_test_values(m);
    std::array<uint8_t, 4> bytes{};
    span_store(bytes, m);
    for (size_t i = 0; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], LAYOUT[i]);
    }
}

TEST(acf_parallel, parse_wire_image)
{
    auto const view = acf_parallel_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    EXPECT_EQ(view->fixed.pad(), 3U);
    EXPECT_EQ(view->fixed.get_bit_width(), 0x25U);
    EXPECT_EQ(view->padded_payload.size(), 8U);
    EXPECT_EQ(view->payload.size(), 5U);
    for (size_t i = 0; i < PAYLOAD.size(); ++i) {
        EXPECT_EQ(view->payload[i], PAYLOAD[i]);
    }
}

TEST(acf_parallel, build_round_trip)
{
    AcfParallelMessage m{};
    m.init();
    set_test_values(m);
    std::array<uint8_t, 12> out{};
    EXPECT_EQ(acf_parallel_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(PAYLOAD)), 12U);
    // The pad was computed, the payload copied, the tail zeroed: byte-identical to the image.
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i], IMAGE[i]);
    }
    auto const view = acf_parallel_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 5U);
    }

    // Too small an output buffer builds nothing.
    std::array<uint8_t, 11> small{};
    EXPECT_EQ(acf_parallel_build(std::span<uint8_t>(small), m, std::span<uint8_t const>(PAYLOAD)), 0U);
}

TEST(acf_parallel, parse_rejects)
{
    // Shorter than the fixed part.
    EXPECT_FALSE(acf_parallel_parse(std::span<uint8_t const>(IMAGE).first(3)).has_value());
    // Declared length past the buffer.
    EXPECT_FALSE(acf_parallel_parse(std::span<uint8_t const>(IMAGE).first(11)).has_value());
    // Wrong acf_msg_type.
    auto wrong_type = IMAGE;
    wrong_type[0] = 0xEC;  // ACF_CHECKSUM
    EXPECT_FALSE(acf_parallel_parse(std::span<uint8_t const>(wrong_type)).has_value());
    // Length below the fixed part.
    auto too_short = IMAGE;
    too_short[1] = 0x01;
    EXPECT_FALSE(acf_parallel_parse(std::span<uint8_t const>(too_short)).has_value());
}

TEST(acf_parallel, figure_76_bit_width_67_and_the_256_bit_case)
{
    // Figure 76: bit_width 67 -> 9 octets -> 3 payload quadlets -> acf_msg_length 4.
    AcfParallelMessage m{};
    m.init();
    m.set_bit_width(67);
    EXPECT_EQ(m.valid_bits(), 67U);
    EXPECT_EQ(m.payload_octets(), 9U);
    std::array<uint8_t, 9> pins{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07};
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(acf_parallel_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(pins)), 16U);
    EXPECT_EQ(out[1], 0x04U);
    auto const view = acf_parallel_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 9U);
        EXPECT_EQ(view->fixed.pad(), 3U);
    }

    // bit_width 0 means 256 bits: 32 octets, 8 quadlets.
    m.set_bit_width(0);
    EXPECT_EQ(m.valid_bits(), 256U);
    EXPECT_EQ(m.payload_octets(), 32U);
    std::array<uint8_t, 32> all{};
    std::array<uint8_t, 36> out256{};
    EXPECT_EQ(acf_parallel_build(std::span<uint8_t>(out256), m, std::span<uint8_t const>(all)), 36U);
    EXPECT_TRUE(acf_parallel_parse(std::span<uint8_t const>(out256)).has_value());

    // A payload that does not match bit_width is refused both ways.
    EXPECT_EQ(acf_parallel_build(std::span<uint8_t>(out256), m, std::span<uint8_t const>(pins)), 0U);
    auto wrong = IMAGE;
    wrong[3] = 0x00;  // 256 bits declared, 8 octets carried
    EXPECT_FALSE(acf_parallel_parse(std::span<uint8_t const>(wrong)).has_value());
}

TEST(acf_parallel, format_to_output)
{
    auto const view = acf_parallel_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    std::string result;
    format_to(std::back_inserter(result), *view);
    EXPECT_TRUE(result.contains("ACF_PARALLEL"));
    EXPECT_TRUE(result.contains("payload=5 octets"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_parallel_test)
