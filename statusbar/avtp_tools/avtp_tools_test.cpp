// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_am824.hpp"
#include "statusbar/avtp/avtp_types.hpp"
#include "statusbar/avtp_tools/avtp_audio_detect.hpp"
#include "statusbar/avtp_tools/avtp_audio_stream_decoder.hpp"
#include "statusbar/avtp_tools/avtp_channel_interleaver.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace statusbar::avtp_tools;

TEST(avtp_audio_detect, kind_name_matches_enum)
{
    EXPECT_EQ(std::string{kind_name(StreamKind::aaf)}, std::string{"AAF"});
    EXPECT_EQ(std::string{kind_name(StreamKind::am824_mbla)}, std::string{"AM824-MBLA"});
    EXPECT_EQ(std::string{kind_name(StreamKind::unknown)}, std::string{"?"});
}

TEST(avtp_audio_detect, payload_too_short_is_neither)
{
    std::array<uint8_t, 1> tiny{statusbar::avtp::AvtpSubtype::aaf};
    EXPECT_FALSE(payload_is_aaf(std::span<uint8_t const>(tiny)));
    EXPECT_FALSE(payload_is_am824(std::span<uint8_t const>(tiny)));
}

TEST(avtp_audio_detect, payload_with_aaf_subtype_byte_recognized)
{
    std::vector<uint8_t> buf(statusbar::avtp::AafPdu::HEADER_LENGTH, 0);
    buf[0] = statusbar::avtp::AvtpSubtype::aaf;
    EXPECT_TRUE(payload_is_aaf(std::span<uint8_t const>(buf)));
    EXPECT_FALSE(payload_is_am824(std::span<uint8_t const>(buf)));
}

TEST(avtp_audio_detect, payload_with_am824_subtype_byte_recognized)
{
    std::vector<uint8_t> buf(statusbar::avtp::Am824Pdu::HEADER_LENGTH, 0);
    buf[0] = statusbar::avtp::AvtpSubtype::iec_61883_iidc;
    EXPECT_TRUE(payload_is_am824(std::span<uint8_t const>(buf)));
    EXPECT_FALSE(payload_is_aaf(std::span<uint8_t const>(buf)));
}

TEST(avtp_audio_detect, stream_id_extracted_from_aaf_header_bytes)
{
    // Build a minimal AAF header with a known stream_id at the standard
    // offset (bytes 4..11). This tests stream_id_of's reading path without
    // depending on AafPdu's setter ergonomics — which are themselves
    // covered in the avtp module tests.
    std::vector<uint8_t> buf(statusbar::avtp::AafPdu::HEADER_LENGTH, 0);
    buf[0] = statusbar::avtp::AvtpSubtype::aaf;
    constexpr uint64_t expected = 0x0011223344556677ULL;
    for (size_t i = 0; i < 8; ++i) {
        buf[4 + i] = static_cast<uint8_t>(expected >> ((7 - i) * 8));
    }
    EXPECT_EQ(stream_id_of(std::span<uint8_t const>(buf), StreamKind::aaf), expected);
}

TEST(avtp_audio_detect, stream_id_extracted_from_am824_header_bytes)
{
    std::vector<uint8_t> buf(statusbar::avtp::Am824Pdu::HEADER_LENGTH, 0);
    buf[0] = statusbar::avtp::AvtpSubtype::iec_61883_iidc;
    constexpr uint64_t expected = 0xCAFEBABEDEADBEEFULL;
    for (size_t i = 0; i < 8; ++i) {
        buf[4 + i] = static_cast<uint8_t>(expected >> ((7 - i) * 8));
    }
    EXPECT_EQ(stream_id_of(std::span<uint8_t const>(buf), StreamKind::am824_mbla), expected);
}

TEST(avtp_channel_interleaver, deposits_channels_into_lanes)
{
    std::vector<float> scratch;
    ChannelInterleaver il{scratch, /*channel_count=*/3};
    il.prepare(/*samples_per_frame=*/4);

    std::array<float, 4> ch0{1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 4> ch1{10.0f, 20.0f, 30.0f, 40.0f};
    std::array<float, 4> ch2{100.0f, 200.0f, 300.0f, 400.0f};
    il.deposit(0, std::span<float const>(ch0));
    il.deposit(1, std::span<float const>(ch1));
    il.deposit(2, std::span<float const>(ch2));

    EXPECT_EQ(scratch.size(), 12U);
    EXPECT_EQ(scratch[0], 1.0f);
    EXPECT_EQ(scratch[1], 10.0f);
    EXPECT_EQ(scratch[2], 100.0f);
    EXPECT_EQ(scratch[3], 2.0f);
    EXPECT_EQ(scratch[4], 20.0f);
    EXPECT_EQ(scratch[5], 200.0f);
}

TEST(avtp_channel_interleaver, missing_channel_stays_silent)
{
    std::vector<float> scratch;
    ChannelInterleaver il{scratch, /*channel_count=*/3};
    il.prepare(/*samples_per_frame=*/2);

    std::array<float, 2> ch0{1.0f, 2.0f};
    std::array<float, 2> ch2{5.0f, 6.0f};
    // Channel 1 deliberately not deposited.
    il.deposit(0, std::span<float const>(ch0));
    il.deposit(2, std::span<float const>(ch2));

    EXPECT_EQ(scratch[0], 1.0f);  // ch0
    EXPECT_EQ(scratch[1], 0.0f);  // ch1 silent
    EXPECT_EQ(scratch[2], 5.0f);  // ch2
    EXPECT_EQ(scratch[3], 2.0f);  // ch0
    EXPECT_EQ(scratch[4], 0.0f);  // ch1 silent
    EXPECT_EQ(scratch[5], 6.0f);  // ch2
}

TEST(avtp_channel_interleaver, out_of_range_channel_is_dropped)
{
    std::vector<float> scratch;
    ChannelInterleaver il{scratch, /*channel_count=*/2};
    il.prepare(/*samples_per_frame=*/2);

    std::array<float, 2> bogus{99.0f, 99.0f};
    il.deposit(/*channel=*/5, std::span<float const>(bogus));

    EXPECT_EQ(scratch.size(), 4U);
    for (float v : scratch) {
        EXPECT_EQ(v, 0.0f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// AvtpAudioStreamDecoder tests. Build synthetic AAF packets via the
// matching serializer so the round-trip exercises the full decode path
// (header parse + stream-input deserializer + interleaver + sink).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Build one fully-formed AAF packet: PDU header + interleaved float32
// audio body. Returns the wire bytes ready to feed to the decoder.
auto make_aaf_packet_float32(
    statusbar::avtp::AafStreamOutputContext& ctx, uint16_t samples_per_channel, std::vector<std::vector<float>> const& per_channel)
    -> std::vector<uint8_t>
{
    auto const channels = ctx.channel_count;
    auto const bps = statusbar::avtp::aaf_bytes_per_sample(ctx.format);
    size_t const audio_bytes = size_t{samples_per_channel} * channels * bps;

    std::vector<uint8_t> packet(statusbar::avtp::AafPdu::HEADER_LENGTH + audio_bytes, uint8_t{0});
    statusbar::avtp::AafPdu pdu;

    auto audio_span = std::span<uint8_t>(packet).subspan(statusbar::avtp::AafPdu::HEADER_LENGTH);
    statusbar::avtp::aaf_stream_serialize(
        ctx, pdu, audio_span, samples_per_channel, /*gptp_now_ns=*/1'000'000ULL, [&](uint8_t ch, std::span<float> dest) {
            for (size_t i = 0; i < dest.size(); ++i) {
                dest[i] = per_channel[ch][i];
            }
        });

    statusbar::span_store(std::span<uint8_t>(packet).subspan(0, statusbar::avtp::AafPdu::HEADER_LENGTH), pdu);
    return packet;
}

}  // namespace

TEST(avtp_audio_stream_decoder, starts_uninitialized)
{
    AvtpAudioStreamDecoder dec;
    EXPECT_FALSE(dec.initialized());
    EXPECT_EQ(dec.format().kind, StreamKind::unknown);
    EXPECT_EQ(dec.packets_decoded(), 0U);
    EXPECT_EQ(dec.sample_frames_delivered(), 0U);
}

TEST(avtp_audio_stream_decoder, malformed_payload_does_not_initialize)
{
    AvtpAudioStreamDecoder dec;
    std::array<uint8_t, 3> tiny{0, 0, 0};
    auto const result = dec.feed(std::span<uint8_t const>(tiny));
    EXPECT_EQ(result.status, FeedStatus::format_error);
    EXPECT_FALSE(dec.initialized());
}

TEST(avtp_audio_stream_decoder, aaf_round_trip_delivers_interleaved_samples_to_sink)
{
    constexpr uint16_t channels = 2;
    constexpr uint16_t samples_per_packet = 8;

    statusbar::tsn::StreamId sid;
    sid.from_uint64(0x0102030405060708ULL);
    statusbar::avtp::AafStreamOutputContext ctx{
        sid,
        statusbar::avtp::AafFormat::float_32bit,
        statusbar::avtp::AafSampleRate::rate_48_khz,
        channels,
        /*depth=*/32,
        /*pres_offset=*/0ULL};

    std::vector<std::vector<float>> per_channel{
        {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f},
        {-0.1f, -0.2f, -0.3f, -0.4f, -0.5f, -0.6f, -0.7f, -0.8f},
    };
    auto const packet = make_aaf_packet_float32(ctx, samples_per_packet, per_channel);

    AvtpAudioStreamDecoder dec;
    std::vector<float> received;
    size_t sink_calls = 0;
    dec.set_sink([&](AvtpAudioSamples const& s) -> bool {
        ++sink_calls;
        received.assign(s.interleaved.begin(), s.interleaved.end());
        return true;
    });

    auto const result = dec.feed(std::span<uint8_t const>(packet));
    EXPECT_EQ(result.status, FeedStatus::ok);
    EXPECT_TRUE(result.samples_delivered);
    EXPECT_TRUE(dec.initialized());
    EXPECT_EQ(dec.format().kind, StreamKind::aaf);
    EXPECT_EQ(dec.format().stream_id, 0x0102030405060708ULL);
    EXPECT_EQ(dec.format().sample_rate_hz, 48000U);
    EXPECT_EQ(dec.format().channel_count, channels);
    EXPECT_EQ(sink_calls, 1U);
    EXPECT_EQ(received.size(), size_t{samples_per_packet} * channels);
    EXPECT_EQ(dec.packets_decoded(), 1U);
    EXPECT_EQ(dec.sample_frames_delivered(), size_t{samples_per_packet});

    // Verify interleaving: channel 0 sample 0, channel 1 sample 0,
    // channel 0 sample 1, channel 1 sample 1, ...
    for (size_t i = 0; i < samples_per_packet; ++i) {
        EXPECT_EQ(received[(i * 2) + 0], per_channel[0][i]);
        EXPECT_EQ(received[(i * 2) + 1], per_channel[1][i]);
    }
}

// Regression: an AAF frame whose captured payload exceeds stream_data_length
// (Ethernet min-frame padding, or a crafted/oversized pcap) must decode ONLY the
// declared samples. Before the fix the deserializer counted samples from the full
// padded payload while the interleave scratch was sized from stream_data_length,
// so the delivered span read past the scratch buffer (heap over-read).
TEST(avtp_audio_stream_decoder, aaf_payload_padding_is_not_decoded_as_audio)
{
    constexpr uint16_t channels = 2;
    constexpr uint16_t samples_per_packet = 8;

    statusbar::tsn::StreamId sid;
    sid.from_uint64(0x0102030405060708ULL);
    statusbar::avtp::AafStreamOutputContext ctx{
        sid,
        statusbar::avtp::AafFormat::float_32bit,
        statusbar::avtp::AafSampleRate::rate_48_khz,
        channels,
        /*depth=*/32,
        /*pres_offset=*/0ULL};

    std::vector<std::vector<float>> per_channel{
        {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f},
        {-0.1f, -0.2f, -0.3f, -0.4f, -0.5f, -0.6f, -0.7f, -0.8f},
    };
    auto packet = make_aaf_packet_float32(ctx, samples_per_packet, per_channel);

    // Append 32 bytes of trailing padding after the valid audio. stream_data_length
    // still reflects only the 8 real frames -- this is what a padded/crafted capture
    // looks like on the wire.
    packet.resize(packet.size() + 32, uint8_t{0xAB});

    AvtpAudioStreamDecoder dec;
    std::vector<float> received;
    dec.set_sink([&](AvtpAudioSamples const& s) -> bool {
        received.assign(s.interleaved.begin(), s.interleaved.end());
        return true;
    });

    auto const result = dec.feed(std::span<uint8_t const>(packet));
    EXPECT_EQ(result.status, FeedStatus::ok);
    // Only the declared 8 frames are decoded; the padding is ignored, and the
    // delivered span never exceeds the scratch buffer.
    EXPECT_EQ(dec.sample_frames_delivered(), size_t{samples_per_packet});
    EXPECT_EQ(received.size(), size_t{samples_per_packet} * channels);
    for (size_t i = 0; i < samples_per_packet; ++i) {
        EXPECT_EQ(received[(i * 2) + 0], per_channel[0][i]);
        EXPECT_EQ(received[(i * 2) + 1], per_channel[1][i]);
    }
}

TEST(avtp_audio_stream_decoder, sink_returning_false_reports_sink_error)
{
    constexpr uint16_t channels = 1;
    constexpr uint16_t samples_per_packet = 4;

    statusbar::tsn::StreamId sid;
    sid.from_uint64(0xAAAA0000BBBB0000ULL);
    statusbar::avtp::AafStreamOutputContext ctx{
        sid, statusbar::avtp::AafFormat::float_32bit, statusbar::avtp::AafSampleRate::rate_48_khz, channels, 32, 0ULL};

    auto const packet = make_aaf_packet_float32(ctx, samples_per_packet, {{0.0f, 0.0f, 0.0f, 0.0f}});

    AvtpAudioStreamDecoder dec;
    dec.set_sink([](AvtpAudioSamples const&) -> bool { return false; });

    auto const result = dec.feed(std::span<uint8_t const>(packet));
    EXPECT_EQ(result.status, FeedStatus::sink_error);
}

TEST(avtp_audio_stream_decoder, second_packet_of_different_kind_reports_wrong_kind)
{
    constexpr uint16_t channels = 1;
    constexpr uint16_t samples_per_packet = 4;

    statusbar::tsn::StreamId sid;
    sid.from_uint64(0xCCCC1111DDDD2222ULL);
    statusbar::avtp::AafStreamOutputContext ctx{
        sid, statusbar::avtp::AafFormat::float_32bit, statusbar::avtp::AafSampleRate::rate_48_khz, channels, 32, 0ULL};

    auto const aaf_packet = make_aaf_packet_float32(ctx, samples_per_packet, {{0.0f, 0.0f, 0.0f, 0.0f}});

    AvtpAudioStreamDecoder dec;
    dec.set_sink([](AvtpAudioSamples const&) { return true; });

    auto const first = dec.feed(std::span<uint8_t const>(aaf_packet));
    EXPECT_EQ(first.status, FeedStatus::ok);
    EXPECT_EQ(dec.format().kind, StreamKind::aaf);

    // Now feed a payload claiming to be AM824 — different subtype.
    std::vector<uint8_t> am824_buf(statusbar::avtp::Am824Pdu::HEADER_LENGTH, 0);
    am824_buf[0] = statusbar::avtp::AvtpSubtype::iec_61883_iidc;
    auto const second = dec.feed(std::span<uint8_t const>(am824_buf));
    EXPECT_EQ(second.status, FeedStatus::wrong_kind);
}

TEST(avtp_audio_stream_decoder, diagnostic_fires_on_unusable_first_packet)
{
    // Build an AAF header where the channel count is zero (unusable).
    std::vector<uint8_t> packet(statusbar::avtp::AafPdu::HEADER_LENGTH, uint8_t{0});
    packet[0] = statusbar::avtp::AvtpSubtype::aaf;
    // Leave format/nsr/channels at defaults — sample_rate_hz will be 0.

    AvtpAudioStreamDecoder dec;
    std::string seen_diagnostic;
    dec.set_diagnostic([&](std::string_view m) { seen_diagnostic.assign(m); });

    auto const result = dec.feed(std::span<uint8_t const>(packet));
    EXPECT_EQ(result.status, FeedStatus::format_error);
    EXPECT_FALSE(dec.initialized());
    EXPECT_FALSE(seen_diagnostic.empty());
}

TEST_MAIN(statusbar_avtp_tools, avtp_tools_test)
