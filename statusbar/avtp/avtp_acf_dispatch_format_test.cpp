// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_dispatch_format.hpp"

#include "statusbar/avtp/avtp_dispatch_format.hpp"
#include "statusbar/avtp/avtp_tscf.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>

using namespace statusbar;
using namespace statusbar::avtp;

namespace {

/// A CAN message (16-octet fixed part + 3 data octets in one quadlet), its
/// Checksum trailer, a user message and a SENSOR_BRIEF message:
/// 20 + 4 + 4 + 8 = 36 octets.
auto sample_payload() -> std::array<uint8_t, 36>
{
    std::array<uint8_t, 36> buf{};
    auto const out = std::span<uint8_t>(buf);
    AcfCanMessage can{};
    can.init();
    can.set_can_bus_id(3);
    can.set_eff(true);
    can.set_can_identifier(0x18DAF110);
    std::array<uint8_t, 3> const data{0x02, 0x10, 0x03};
    EXPECT_EQ(acf_can_build(out, can, std::span<uint8_t const>(data)), 20U);
    EXPECT_TRUE(acf_checksum_build(out.subspan(20), std::span<uint8_t const>(buf).first(20)));
    EXPECT_TRUE(acf_store_header(out.subspan(24), AcfMsgType::user_first, 1));
    buf[26] = 0xAB;
    AcfSensorBriefMessage sensor{};
    sensor.init();
    sensor.set_num_sensors(2);
    sensor.set_sz(2);
    std::array<uint8_t, 4> const values{0x12, 0x34, 0x56, 0x78};
    EXPECT_EQ(acf_sensor_brief_build(out.subspan(28), sensor, std::span<uint8_t const>(values)), 8U);
    return buf;
}

}  // namespace

TEST(acf_dispatch_format, lists_every_message_without_verification)
{
    auto const buf = sample_payload();
    std::string result;
    format_acf_payload(std::back_inserter(result), std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.contains("ACF_CAN "));
    EXPECT_TRUE(result.contains("can_identifier=0x18daf110"));
    EXPECT_TRUE(result.contains("payload=3 octets"));
    EXPECT_TRUE(result.contains("ACF_CHECKSUM checksum=0x"));
    EXPECT_TRUE(result.contains("ACF_USER(0x78)"));
    EXPECT_TRUE(result.contains("data=ab00"));
    EXPECT_TRUE(result.contains("ACF_SENSOR_BRIEF"));
    EXPECT_FALSE(result.contains('['));
    EXPECT_FALSE(result.contains("malformed"));
}

TEST(acf_dispatch_format, verifies_trailers_on_request)
{
    auto buf = sample_payload();
    std::string result;
    format_acf_payload(std::back_inserter(result), std::span<uint8_t const>(buf), true);
    EXPECT_TRUE(result.contains("ACF_CAN "));
    EXPECT_TRUE(result.contains("[checksum_ok]"));
    EXPECT_FALSE(result.contains("ACF_CHECKSUM"));  // consumed as the trailer

    buf[5] ^= 0x40U;  // damage the CAN message
    std::string bad;
    format_acf_payload(std::back_inserter(bad), std::span<uint8_t const>(buf), true);
    EXPECT_TRUE(bad.contains("[checksum_bad]"));
}

TEST(acf_dispatch_format, reports_a_malformed_tail_and_invalid_typed_messages)
{
    auto buf = sample_payload();
    buf[25] = 0x00;  // user message length -> 0
    std::string result;
    format_acf_payload(std::back_inserter(result), std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.contains("ACF_CHECKSUM"));
    EXPECT_TRUE(result.contains("malformed message length (12 octets unparsed)"));

    // A CAN message whose declared length exceeds the type's 16-quadlet payload bound.
    std::array<uint8_t, 84> big{};
    EXPECT_TRUE(acf_store_header(std::span<uint8_t>(big), AcfMsgType::can, 21));
    std::string invalid;
    format_acf_payload(std::back_inserter(invalid), std::span<uint8_t const>(big));
    EXPECT_TRUE(invalid.contains("ACF_CAN(0x01)"));
    EXPECT_TRUE(invalid.contains("(invalid for its type)"));
}

TEST(acf_dispatch_format, format_avtp_decodes_tscf_and_ntscf_frames)
{
    auto const acf = sample_payload();
    TscfPdu tscf{};
    tscf.init(StreamId{});
    tscf.set_stream_data_length(static_cast<uint16_t>(acf.size()));
    std::array<uint8_t, TscfPdu::HEADER_LENGTH + 36 + 8> frame{};  // + min-frame padding
    span_store(frame, tscf);
    for (size_t i = 0; i < acf.size(); ++i) {
        frame[TscfPdu::HEADER_LENGTH + i] = acf[i];
    }
    std::string result;
    format_avtp(std::back_inserter(result), std::span<uint8_t const>(frame));
    EXPECT_TRUE(result.contains("TSCF"));
    EXPECT_TRUE(result.contains("ACF_CAN "));
    EXPECT_TRUE(result.contains("ACF_SENSOR_BRIEF"));
    EXPECT_FALSE(result.contains("malformed"));  // the padding is clamped away

    NtscfPdu ntscf{};
    ntscf.init(StreamId{});
    ntscf.set_ntscf_data_length(static_cast<uint16_t>(acf.size()));
    std::array<uint8_t, NtscfPdu::HEADER_LENGTH + 36> nframe{};
    span_store(nframe, ntscf);
    for (size_t i = 0; i < acf.size(); ++i) {
        nframe[NtscfPdu::HEADER_LENGTH + i] = acf[i];
    }
    std::string nresult;
    format_avtp(std::back_inserter(nresult), std::span<uint8_t const>(nframe));
    EXPECT_TRUE(nresult.contains("NTSCF"));
    EXPECT_TRUE(nresult.contains("ACF_CAN "));
}

TEST_MAIN(statusbar_avtp, avtp_acf_dispatch_format_test)
