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

// IEEE 1722-2016 Annex I: byte 4 is b|nb|ut|sc, byte 5 the IEC 60958 label count, byte 6 the
// MBLA count, byte 7 MIDI|SMPTE. The Annex-style word 0x00a0020840000800: 48 kHz, nb, 8 MBLA.
TEST(am824_stream_format, annex_word_is_48k_non_blocking_8_mbla)
{
    constexpr auto f = Am824StreamFormat::from_u64(0x00a0020840000800ULL);
    static_assert(f.is_61883_6());
    EXPECT_EQ(f.evt(), uint8_t{0});
    EXPECT_EQ(f.sfc(), uint8_t{2});
    EXPECT_EQ(am824_sfc_to_hz(f.sfc()), 48'000U);
    EXPECT_EQ(f.dbs.get(), uint8_t{8});
    EXPECT_TRUE(f.non_blocking());
    EXPECT_FALSE(f.blocking());
    EXPECT_FALSE(f.up_to());
    EXPECT_FALSE(f.synchronous_clock());
    EXPECT_EQ(f.label_iec_60958_cnt.get(), uint8_t{0});
    EXPECT_EQ(f.label_mbla_cnt.get(), uint8_t{8});
    EXPECT_EQ(f.label_midi_cnt(), uint8_t{0});
    EXPECT_EQ(f.label_smpte_cnt(), uint8_t{0});
}

// The MOTU 16A lists 0x00a00N0860000800 on its inputs (nb + ut) and 0x00a00N0870000800 on its
// outputs (nb + ut + sc): 8 MBLA quadlets, no IEC 60958 data, at each SFC.
TEST(am824_stream_format, motu_16a_input_and_output_words)
{
    for (uint8_t const sfc : {uint8_t{2}, uint8_t{4}, uint8_t{6}}) {
        uint64_t const sfc_bits = uint64_t{sfc} << 40U;
        auto const in = Am824StreamFormat::from_u64(0x00a0000860000800ULL | sfc_bits);
        auto const out = Am824StreamFormat::from_u64(0x00a0000870000800ULL | sfc_bits);
        EXPECT_EQ(in.sfc(), sfc);
        EXPECT_EQ(out.sfc(), sfc);
        EXPECT_TRUE(in.non_blocking() && in.up_to() && !in.synchronous_clock());
        EXPECT_TRUE(out.non_blocking() && out.up_to() && out.synchronous_clock());
        EXPECT_EQ(in.label_iec_60958_cnt.get(), uint8_t{0});
        EXPECT_EQ(in.label_mbla_cnt.get(), uint8_t{8});
        EXPECT_EQ(out.label_mbla_cnt.get(), uint8_t{8});
        EXPECT_EQ(in.dbs.get(), uint8_t{8});
    }
    // The 16A's one-channel media-clock stream words.
    EXPECT_EQ(Am824StreamFormat::from_u64(0x00a0020140000100ULL).label_mbla_cnt.get(), uint8_t{1});
    EXPECT_TRUE(Am824StreamFormat::from_u64(0x00a0020150000100ULL).synchronous_clock());
}

// make_am824_stream_format builds the non-blocking MBLA-only word the decoders read back.
TEST(am824_stream_format, make_builds_the_annex_and_motu_words)
{
    static_assert(make_am824_stream_format(2, 8) == 0x00a0020840000800ULL);
    static_assert(make_am824_stream_format(2, 2) == 0x00a0020240000200ULL);
    static_assert(make_am824_stream_format(4, 8, /*up_to=*/true) == 0x00a0040860000800ULL);
    static_assert(make_am824_stream_format(6, 8, /*up_to=*/true, /*synchronous_clock=*/true) == 0x00a0060870000800ULL);
    EXPECT_TRUE(stream_format_to_string(make_am824_stream_format(4, 6)) == "AM824 6ch 96kHz");
}

// A non-AM824 event type in the FDF is not read as a rate.
TEST(am824_stream_format, a_non_am824_event_type_has_no_rate)
{
    auto const s = stream_format_to_string(0x00a00a0840000800ULL);  // evt 1, sfc 2
    EXPECT_TRUE(s == "AM824 8ch SFC=0x0a");
}

TEST_MAIN(statusbar_avtp, avtp_stream_format_test)
