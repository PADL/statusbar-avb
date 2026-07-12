// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_stream_format.hpp"

#include "statusbar/test/test.hpp"

#include <cstdint>
#include <string>

using namespace statusbar;
using namespace statusbar::avtp;

// AAF stream format (IEEE 1722-2016 Clause 7.3.4): byte0=0x02, byte1 low nibble=nsr,
// byte2=format, byte3=bit_depth, bytes4-7 = channels<<22 | samples_per_frame<<12.
// 48000(nsr=5) 2ch 24-bit INT_24(0x03): 02 05 03 18 | (2<<22)=00 80 00 00
TEST(stream_format, aaf_48khz_2ch_24bit)
{
    auto const s = stream_format_to_string(0x0205031800800000ULL);
    EXPECT_TRUE(s == "AAF 2ch 48kHz 24-bit");
}

// Verified against a reference STREAM_INPUT: 02 07 02 20 02 00 c0 00
// = 96000(nsr=7) INT_32(0x02) 32-bit, 8 channels, 12 samples/frame.
TEST(stream_format, aaf_96khz_8ch_32bit)
{
    auto const s = stream_format_to_string(0x020702200200C000ULL);
    EXPECT_TRUE(s == "AAF 8ch 96kHz 32-bit");
}

// 44100(nsr=4) 2ch 16-bit INT_16(0x04): 02 04 04 10 | (2<<22)=00 80 00 00
TEST(stream_format, aaf_441khz_2ch_16bit)
{
    auto const s = stream_format_to_string(0x0204041000800000ULL);
    EXPECT_TRUE(s == "AAF 2ch 44.1kHz 16-bit");
}

// AM824/IEC 61883-6 real-world format from the audio interface: 0x00a0020840000800
// Byte 2 = 0x02 = SFC for 48 kHz (whole byte)
// Byte 3 = 0x08 = 8 channels (DBS)
TEST(stream_format, am824_audio_iface_is_8ch_48khz)
{
    auto const s = stream_format_to_string(0x00a0020840000800ULL);
    EXPECT_TRUE(s == "AM824 8ch 48kHz");
}

// AM824 at 96 kHz, 2 channels
// Byte 2 = 0x04 (SFC for 96 kHz), Byte 3 = 0x02 (DBS = 2)
TEST(stream_format, am824_2ch_96khz)
{
    auto const s = stream_format_to_string(0x00a0040200000000ULL);
    EXPECT_TRUE(s == "AM824 2ch 96kHz");
}

TEST(stream_format, unknown_subtype_falls_back_to_hex_only)
{
    auto const s = stream_format_to_string(0x1234567890ABCDEFULL);
    EXPECT_TRUE(s == "0x1234567890abcdef");
}

TEST(stream_format, crf_decodes_milan_media_clock_format)
{
    // The Milan media-clock CRF format (matching the Meyer Galaxy reference
    // capture): AUDIO_SAMPLE, 48 kHz base, timestamp_interval 96, one
    // timestamp per PDU, pull x1.0 (omitted from the string when 1.0).
    auto const s = stream_format_to_string(0x041060010000BB80ULL);
    EXPECT_TRUE(s == "CRF Audio Sample 48kHz interval=96 ts/pdu=1");

    // A pulled variant renders the multiplier.
    auto const pulled = stream_format_to_string(0x041060012000BB80ULL);  // pull=1 (x1/1.001)
    EXPECT_TRUE(pulled.find("pull=") != std::string::npos);
}

// nsr in the low nibble = 0x0F (no mapped rate) -> "rate?(15)"
TEST(stream_format, aaf_unknown_rate_shows_nsr_code)
{
    auto const s = stream_format_to_string(0x020F011000000000ULL);
    EXPECT_TRUE(s.find("rate?") != std::string::npos);
}

TEST_MAIN(statusbar_avtp, avtp_stream_format_test)
