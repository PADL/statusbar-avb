// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_ntscf.hpp"

#include "statusbar/avtp/avtp_ntscf_format.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <string>

using namespace statusbar;
using namespace statusbar::avtp;
using namespace statusbar::ieee;
using namespace statusbar::tsn;

// Compile-time layout check
static_assert(sizeof(NtscfPdu) == NtscfPdu::HEADER_LENGTH);

TEST(ntscf, init_and_accessors)
{
    NtscfPdu pdu{};
    StreamId sid(Eui48(0x00, 0x11, 0x22, 0x33, 0x44, 0x55), 0x0001);
    pdu.init(sid);

    EXPECT_EQ(pdu.subtype.get(), AvtpSubtype::ntscf);
    EXPECT_TRUE(pdu.sv());
    EXPECT_EQ(pdu.version(), 0);
    EXPECT_EQ(pdu.ntscf_data_length(), 0);
    EXPECT_EQ(pdu.get_sequence_num_lsb(), 0);
    EXPECT_EQ(pdu.stream_id(), sid);
    EXPECT_TRUE(pdu.is_valid());
}

TEST(ntscf, init_sv_false)
{
    NtscfPdu pdu{};
    StreamId sid{};
    pdu.init(sid, false);

    EXPECT_FALSE(pdu.sv());
    EXPECT_TRUE(pdu.is_valid());
}

TEST(ntscf, data_length_small)
{
    NtscfPdu pdu{};
    StreamId sid{};
    pdu.init(sid);

    pdu.set_ntscf_data_length(100);
    EXPECT_EQ(pdu.ntscf_data_length(), 100);
}

TEST(ntscf, data_length_max)
{
    NtscfPdu pdu{};
    StreamId sid{};
    pdu.init(sid);

    pdu.set_ntscf_data_length(2047);
    EXPECT_EQ(pdu.ntscf_data_length(), 2047);
}

TEST(ntscf, data_length_preserves_sv_version)
{
    NtscfPdu pdu{};
    StreamId sid{};
    pdu.init(sid);  // sv=true, version=0

    pdu.set_ntscf_data_length(2047);
    EXPECT_TRUE(pdu.sv());
    EXPECT_EQ(pdu.version(), 0);
    EXPECT_EQ(pdu.ntscf_data_length(), 2047);
}

TEST(ntscf, sequence_num_lsb)
{
    NtscfPdu pdu{};
    StreamId sid{};
    pdu.init(sid);

    pdu.set_sequence_num_lsb(0xFF);
    EXPECT_EQ(pdu.get_sequence_num_lsb(), 0xFFU);

    pdu.set_sequence_num_lsb(0);
    EXPECT_EQ(pdu.get_sequence_num_lsb(), 0U);
}

TEST(ntscf, round_trip_serialization)
{
    NtscfPdu original{};
    StreamId sid(Eui48(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF), 0x1234);
    original.init(sid);
    original.set_ntscf_data_length(500);
    original.set_sequence_num_lsb(42);

    std::array<uint8_t, 16> buf{};
    (void)store_unchecked(std::span<uint8_t>(buf), original);

    // Verify subtype byte
    EXPECT_EQ(buf[0], AvtpSubtype::ntscf);

    // Deserialize
    NtscfPdu loaded{};
    (void)load_unchecked(std::span<uint8_t const>(buf), &loaded);
    EXPECT_TRUE(loaded.is_valid());
    EXPECT_TRUE(loaded.sv());
    EXPECT_EQ(loaded.ntscf_data_length(), 500);
    EXPECT_EQ(loaded.get_sequence_num_lsb(), 42);
    EXPECT_EQ(loaded.stream_id(), sid);
}

TEST(ntscf, parse_valid)
{
    NtscfPdu pdu{};
    StreamId sid(Eui48(0x11, 0x22, 0x33, 0x44, 0x55, 0x66), 0x0001);
    pdu.init(sid);
    pdu.set_ntscf_data_length(300);

    std::array<uint8_t, 32> buf{};
    span_store(buf, pdu);

    auto parsed = ntscf_parse_header(std::span<uint8_t const>(buf));
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->stream_id(), sid);
    EXPECT_EQ(parsed->ntscf_data_length(), 300);
}

TEST(ntscf, parse_truncated)
{
    std::array<uint8_t, 8> buf{};
    buf[0] = AvtpSubtype::ntscf;
    auto parsed = ntscf_parse_header(std::span<uint8_t const>(buf));
    EXPECT_FALSE(parsed.has_value());
}

TEST(ntscf, parse_wrong_subtype)
{
    NtscfPdu pdu{};
    StreamId sid{};
    pdu.init(sid);
    pdu.subtype = AvtpSubtype::escf;  // wrong

    std::array<uint8_t, 12> buf{};
    span_store(buf, pdu);
    auto parsed = ntscf_parse_header(std::span<uint8_t const>(buf));
    EXPECT_FALSE(parsed.has_value());
}

TEST(ntscf, get_acf_payload)
{
    std::array<uint8_t, 24> buf{};
    buf[0] = AvtpSubtype::ntscf;
    buf[2] = 12;     // ntscf_data_length: the 12 octets after the header
    buf[12] = 0xCA;  // first byte of ACF payload

    auto payload = ntscf_get_acf_payload(std::span<uint8_t const>(buf));
    EXPECT_EQ(payload.size(), 12U);
    EXPECT_EQ(payload[0], 0xCAU);

    // Header-only packet returns empty
    auto empty = ntscf_get_acf_payload(std::span<uint8_t const>(buf.data(), NtscfPdu::HEADER_LENGTH));
    EXPECT_TRUE(empty.empty());

    // A declared ntscf_data_length bounds the payload below the buffer
    // extent (Ethernet min-frame padding is not ACF data) ...
    buf[2] = 8;
    EXPECT_EQ(ntscf_get_acf_payload(std::span<uint8_t const>(buf)).size(), 8U);
    // ... and a declared length past the buffer clamps to what is there.
    buf[2] = 200;
    EXPECT_EQ(ntscf_get_acf_payload(std::span<uint8_t const>(buf)).size(), 12U);
}

TEST(ntscf, format_to_output)
{
    NtscfPdu pdu{};
    StreamId sid(Eui48(0x00, 0x11, 0x22, 0x33, 0x44, 0x55), 0x0001);
    pdu.init(sid);
    pdu.set_ntscf_data_length(64);
    pdu.set_sequence_num_lsb(7);

    std::string result;
    format_to(std::back_inserter(result), pdu);
    EXPECT_TRUE(result.find("NTSCF") != std::string::npos);
    EXPECT_TRUE(result.find("64") != std::string::npos);
    EXPECT_TRUE(result.find("7") != std::string::npos);
}

TEST_MAIN(statusbar_avtp, avtp_ntscf_test)
