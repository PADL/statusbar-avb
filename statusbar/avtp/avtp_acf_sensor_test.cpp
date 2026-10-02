// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_sensor.hpp"

#include "statusbar/avtp/avtp_acf_sensor_format.hpp"
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

static_assert(sizeof(AcfSensorMessage) == AcfSensorMessage::LENGTH);

namespace {

/// The fixed part with every field at its test value and the fixed-only length,
/// encoded independently of the accessors (the layout oracle); the length is the
/// smallest valid message (fixed part plus the minimum payload).
constexpr std::array<uint8_t, 12> LAYOUT{0x10, 0x04, 0x85, 0x6A, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

/// A complete message on the wire: the fixed part (pad = 3, length = 5 quadlets)
/// followed by a 5-octet payload and its three pad octets.
constexpr std::array<uint8_t, 20> IMAGE{0x10, 0x05, 0x85, 0x6A, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                        0x07, 0x08, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0x00, 0x00, 0x00};

constexpr std::array<uint8_t, 5> PAYLOAD{0xA1, 0xB2, 0xC3, 0xD4, 0xE5};

/// Set every field to its test value
void set_test_values(AcfSensorMessage& m)
{
    m.set_mtv(true);
    m.set_num_sensors(0x5U);
    m.set_sz(0x1U);
    m.set_sensor_group(0x2AU);
    m.set_message_timestamp(0x102030405060708U);
}

/// Check every field against its test value
void expect_test_values(AcfSensorMessage const& m)
{
    EXPECT_TRUE(m.mtv());
    EXPECT_EQ(m.num_sensors(), 0x5U);
    EXPECT_EQ(m.sz(), 0x1U);
    EXPECT_EQ(m.sensor_group(), 0x2AU);
    EXPECT_EQ(m.get_message_timestamp(), 0x102030405060708U);
}

}  // namespace

TEST(acf_sensor, init_and_layout)
{
    AcfSensorMessage m{};
    m.init();
    EXPECT_EQ(m.header.msg_type(), AcfMsgType::sensor);
    EXPECT_EQ(m.header.msg_length(), 4U);

    set_test_values(m);
    expect_test_values(m);
    std::array<uint8_t, 12> bytes{};
    span_store(bytes, m);
    for (size_t i = 0; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], LAYOUT[i]);
    }
}

TEST(acf_sensor, parse_wire_image)
{
    auto const view = acf_sensor_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    EXPECT_EQ(view->fixed.pad(), 3U);
    EXPECT_TRUE(view->fixed.mtv());
    EXPECT_EQ(view->fixed.num_sensors(), 0x5U);
    EXPECT_EQ(view->fixed.sz(), 0x1U);
    EXPECT_EQ(view->fixed.sensor_group(), 0x2AU);
    EXPECT_EQ(view->fixed.get_message_timestamp(), 0x102030405060708U);
    EXPECT_EQ(view->padded_payload.size(), 8U);
    EXPECT_EQ(view->payload.size(), 5U);
    for (size_t i = 0; i < PAYLOAD.size(); ++i) {
        EXPECT_EQ(view->payload[i], PAYLOAD[i]);
    }
}

TEST(acf_sensor, build_round_trip)
{
    AcfSensorMessage m{};
    m.init();
    set_test_values(m);
    std::array<uint8_t, 20> out{};
    EXPECT_EQ(acf_sensor_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(PAYLOAD)), 20U);
    // The pad was computed, the payload copied, the tail zeroed: byte-identical to the image.
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i], IMAGE[i]);
    }
    auto const view = acf_sensor_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 5U);
    }

    // Too small an output buffer builds nothing.
    std::array<uint8_t, 19> small{};
    EXPECT_EQ(acf_sensor_build(std::span<uint8_t>(small), m, std::span<uint8_t const>(PAYLOAD)), 0U);
}

TEST(acf_sensor, parse_rejects)
{
    // Shorter than the fixed part.
    EXPECT_FALSE(acf_sensor_parse(std::span<uint8_t const>(IMAGE).first(11)).has_value());
    // Declared length past the buffer.
    EXPECT_FALSE(acf_sensor_parse(std::span<uint8_t const>(IMAGE).first(19)).has_value());
    // Wrong acf_msg_type.
    auto wrong_type = IMAGE;
    wrong_type[0] = 0xEC;  // ACF_CHECKSUM
    EXPECT_FALSE(acf_sensor_parse(std::span<uint8_t const>(wrong_type)).has_value());
    // Length below the fixed part.
    auto too_short = IMAGE;
    too_short[1] = 0x01;
    EXPECT_FALSE(acf_sensor_parse(std::span<uint8_t const>(too_short)).has_value());
}

TEST(acf_sensor, figure_78_and_79_lengths)
{
    // Figure 78: 17 one-octet values -> acf_msg_length 8; Figure 79: 19 two-octet
    // values -> acf_msg_length 13.
    AcfSensorMessage m{};
    m.init();
    m.set_num_sensors(17);
    m.set_sz(1);
    std::array<uint8_t, 17> seventeen{};
    for (size_t i = 0; i < seventeen.size(); ++i) {
        seventeen[i] = static_cast<uint8_t>(i + 1U);
    }
    std::array<uint8_t, 12 + 20> out{};
    EXPECT_EQ(acf_sensor_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(seventeen)), 12U + 20U);
    auto const view = acf_sensor_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->fixed.header.msg_length(), 8U);
        EXPECT_EQ(view->fixed.pad(), 3U);
        EXPECT_EQ(view->payload.size(), 17U);
        auto const v16 = acf_sensor_value(*view, 16);
        EXPECT_TRUE(v16.has_value() && *v16 == 17U);
        EXPECT_FALSE(acf_sensor_value(*view, 17).has_value());
    }

    m.set_num_sensors(19);
    m.set_sz(2);
    std::array<uint8_t, 38> nineteen{};
    nineteen[0] = 0x12;
    nineteen[1] = 0x34;
    nineteen[36] = 0xAB;
    nineteen[37] = 0xCD;
    std::array<uint8_t, 12 + 40> out2{};
    EXPECT_EQ(acf_sensor_build(std::span<uint8_t>(out2), m, std::span<uint8_t const>(nineteen)), 12U + 40U);
    auto const view2 = acf_sensor_parse(std::span<uint8_t const>(out2));
    EXPECT_TRUE(view2.has_value());
    if (view2.has_value()) {
        EXPECT_EQ(view2->fixed.header.msg_length(), 13U);
        EXPECT_EQ(view2->fixed.pad(), 2U);
        auto const first = acf_sensor_value(*view2, 0);
        auto const last = acf_sensor_value(*view2, 18);
        EXPECT_TRUE(first.has_value() && *first == 0x1234U);
        EXPECT_TRUE(last.has_value() && *last == 0xABCDU);
    }
}

TEST(acf_sensor, zero_fields_mean_128_values_of_4_octets)
{
    AcfSensorMessage m{};
    m.init();
    EXPECT_EQ(m.sensor_count(), 128U);
    EXPECT_EQ(m.sensor_value_size(), 4U);
    EXPECT_EQ(m.payload_octets(), 512U);
    std::vector<uint8_t> values(512, 0x5A);
    std::vector<uint8_t> out(12 + 512);
    EXPECT_EQ(acf_sensor_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(values)), 12U + 512U);
    auto const view = acf_sensor_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        auto const v = acf_sensor_value(*view, 127);
        EXPECT_TRUE(v.has_value() && *v == 0x5A5A5A5AU);
    }

    // A payload that does not match the fields is refused both ways.
    std::vector<uint8_t> const short_values(8, 0);
    EXPECT_EQ(acf_sensor_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(short_values)), 0U);
    auto wrong = IMAGE;
    wrong[2] = static_cast<uint8_t>(wrong[2] & 0x80U);  // num_sensors -> 0 (128 values) with 8 octets carried
    EXPECT_FALSE(acf_sensor_parse(std::span<uint8_t const>(wrong)).has_value());
}

TEST(acf_sensor, format_to_output)
{
    auto const view = acf_sensor_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    std::string result;
    format_to(std::back_inserter(result), *view);
    EXPECT_TRUE(result.contains("ACF_SENSOR"));
    EXPECT_TRUE(result.contains("payload=5 octets"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_sensor_test)
