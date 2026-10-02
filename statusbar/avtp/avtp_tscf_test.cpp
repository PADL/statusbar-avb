// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_tscf.hpp"

#include "statusbar/avtp/avtp_tscf_format.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <string>

using namespace statusbar;
using namespace statusbar::avtp;
using namespace statusbar::ieee;
using namespace statusbar::tsn;

// Compile-time layout check
static_assert(sizeof(TscfPdu) == TscfPdu::HEADER_LENGTH);

TEST(tscf, init_and_accessors)
{
    TscfPdu pdu{};
    StreamId sid(Eui48(0x00, 0x11, 0x22, 0x33, 0x44, 0x55), 0x0001);
    pdu.init(sid);

    EXPECT_EQ(pdu.subtype.get(), AvtpSubtype::tscf);
    EXPECT_TRUE(pdu.sv());
    EXPECT_EQ(pdu.version(), 0);
    EXPECT_FALSE(pdu.mr());
    EXPECT_FALSE(pdu.tv());
    EXPECT_FALSE(pdu.tu());
    EXPECT_EQ(pdu.get_sequence_num(), 0);
    EXPECT_EQ(pdu.stream_id(), sid);
    EXPECT_EQ(pdu.get_avtp_timestamp(), 0U);
    EXPECT_EQ(pdu.get_stream_data_length(), 0);
    EXPECT_TRUE(pdu.is_valid());
}

TEST(tscf, init_sv_false)
{
    TscfPdu pdu{};
    StreamId sid{};
    pdu.init(sid, false);

    EXPECT_FALSE(pdu.sv());
    EXPECT_TRUE(pdu.is_valid());
}

TEST(tscf, set_flags)
{
    TscfPdu pdu{};
    StreamId sid{};
    pdu.init(sid);

    pdu.set_mr(true);
    EXPECT_TRUE(pdu.mr());

    pdu.set_tv(true);
    EXPECT_TRUE(pdu.tv());

    pdu.set_tu(true);
    EXPECT_TRUE(pdu.tu());

    // Verify other flags unaffected
    EXPECT_TRUE(pdu.sv());
    EXPECT_EQ(pdu.version(), 0);
}

TEST(tscf, sequence_num_updates_lsb)
{
    TscfPdu pdu{};
    StreamId sid{};
    pdu.init(sid);

    pdu.set_sequence_num(0xAB);
    EXPECT_EQ(pdu.get_sequence_num(), 0xABU);
    // sequence_num_lsb should mirror sequence_num (per 9.3.2)
    EXPECT_EQ(pdu.sequence_num_lsb.get(), 0xABU);
}

TEST(tscf, timestamp)
{
    TscfPdu pdu{};
    StreamId sid{};
    pdu.init(sid);

    pdu.set_tv(true);
    pdu.set_avtp_timestamp(0x12345678);
    EXPECT_EQ(pdu.get_avtp_timestamp(), 0x12345678U);
}

TEST(tscf, stream_data_length)
{
    TscfPdu pdu{};
    StreamId sid{};
    pdu.init(sid);

    pdu.set_stream_data_length(500);
    EXPECT_EQ(pdu.get_stream_data_length(), 500);

    pdu.set_stream_data_length(0xFFFF);
    EXPECT_EQ(pdu.get_stream_data_length(), 0xFFFF);
}

TEST(tscf, round_trip_serialization)
{
    TscfPdu original{};
    StreamId sid(Eui48(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF), 0x1234);
    original.init(sid);
    original.set_tv(true);
    original.set_avtp_timestamp(0xCAFEBABE);
    original.set_sequence_num(42);
    original.set_stream_data_length(100);

    std::array<uint8_t, 32> buf{};
    (void)store_unchecked(std::span<uint8_t>(buf), original);

    // Verify subtype byte
    EXPECT_EQ(buf[0], AvtpSubtype::tscf);

    // Deserialize
    TscfPdu loaded{};
    (void)load_unchecked(std::span<uint8_t const>(buf), &loaded);
    EXPECT_TRUE(loaded.is_valid());
    EXPECT_TRUE(loaded.sv());
    EXPECT_TRUE(loaded.tv());
    EXPECT_EQ(loaded.get_sequence_num(), 42);
    EXPECT_EQ(loaded.stream_id(), sid);
    EXPECT_EQ(loaded.get_avtp_timestamp(), 0xCAFEBABEU);
    EXPECT_EQ(loaded.get_stream_data_length(), 100);
}

TEST(tscf, parse_valid)
{
    TscfPdu pdu{};
    StreamId sid(Eui48(0x11, 0x22, 0x33, 0x44, 0x55, 0x66), 0x0001);
    pdu.init(sid);
    pdu.set_stream_data_length(200);

    std::array<uint8_t, 48> buf{};
    span_store(buf, pdu);

    auto parsed = tscf_parse_header(std::span<uint8_t const>(buf));
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->stream_id(), sid);
    EXPECT_EQ(parsed->get_stream_data_length(), 200);
}

TEST(tscf, parse_truncated)
{
    std::array<uint8_t, 16> buf{};
    buf[0] = AvtpSubtype::tscf;
    auto parsed = tscf_parse_header(std::span<uint8_t const>(buf));
    EXPECT_FALSE(parsed.has_value());
}

TEST(tscf, parse_wrong_subtype)
{
    TscfPdu pdu{};
    StreamId sid{};
    pdu.init(sid);
    pdu.subtype = AvtpSubtype::aaf;  // wrong

    std::array<uint8_t, 24> buf{};
    span_store(buf, pdu);
    auto parsed = tscf_parse_header(std::span<uint8_t const>(buf));
    EXPECT_FALSE(parsed.has_value());
}

TEST(tscf, get_acf_payload)
{
    std::array<uint8_t, 40> buf{};
    buf[0] = AvtpSubtype::tscf;
    buf[21] = 16;    // stream_data_length: the 16 octets after the header
    buf[24] = 0xBE;  // first byte of ACF payload

    auto payload = tscf_get_acf_payload(std::span<uint8_t const>(buf));
    EXPECT_EQ(payload.size(), 16U);
    EXPECT_EQ(payload[0], 0xBEU);

    // Header-only packet returns empty
    auto empty = tscf_get_acf_payload(std::span<uint8_t const>(buf.data(), TscfPdu::HEADER_LENGTH));
    EXPECT_TRUE(empty.empty());

    // A declared stream_data_length bounds the payload below the buffer
    // extent (Ethernet min-frame padding is not ACF data) ...
    buf[21] = 8;
    EXPECT_EQ(tscf_get_acf_payload(std::span<uint8_t const>(buf)).size(), 8U);
    // ... and a declared length past the buffer clamps to what is there.
    buf[21] = 200;
    EXPECT_EQ(tscf_get_acf_payload(std::span<uint8_t const>(buf)).size(), 16U);
}

TEST(tscf, format_to_output)
{
    TscfPdu pdu{};
    StreamId sid(Eui48(0x00, 0x11, 0x22, 0x33, 0x44, 0x55), 0x0001);
    pdu.init(sid);
    pdu.set_tv(true);
    pdu.set_avtp_timestamp(1000);
    pdu.set_stream_data_length(64);

    std::string result;
    format_to(std::back_inserter(result), pdu);
    EXPECT_TRUE(result.find("TSCF") != std::string::npos);
    EXPECT_TRUE(result.find("1000") != std::string::npos);
    EXPECT_TRUE(result.find("64") != std::string::npos);
}

TEST_MAIN(statusbar_avtp, avtp_tscf_test)
