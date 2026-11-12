// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_aaf_stream_input.hpp"

#include "statusbar/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace statusbar;
using namespace statusbar::avtp;
using namespace statusbar::tsn;

// ============================================================================
// AafStreamInputContext construction and reset
// ============================================================================

TEST(aaf_stream_input_ctx, construction_24bit_48khz)
{
    AafStreamInputContext ctx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    EXPECT_EQ(ctx.channel_count, 2);
    EXPECT_EQ(ctx.bit_depth, 24);
    EXPECT_EQ(ctx.sample_period_ns, 20833U);
    EXPECT_FALSE(ctx.has_valid_timestamp);
    EXPECT_EQ(ctx.packets_received, 0U);
}

TEST(aaf_stream_input_ctx, construction_float32_96khz)
{
    AafStreamInputContext ctx{AafFormat::float_32bit, AafSampleRate::rate_96_khz, 8, 32};
    EXPECT_EQ(ctx.channel_count, 8);
    EXPECT_EQ(ctx.bit_depth, 32);
    EXPECT_EQ(ctx.sample_period_ns, 10416U);
}

TEST(aaf_stream_input_ctx, reset)
{
    AafStreamInputContext ctx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    ctx.packets_received = 50;
    ctx.has_valid_timestamp = true;
    ctx.last_pts_ns = 12345;
    ctx.reset();
    EXPECT_EQ(ctx.packets_received, 0U);
    EXPECT_FALSE(ctx.has_valid_timestamp);
    EXPECT_EQ(ctx.last_pts_ns, 0U);
    EXPECT_EQ(ctx.channel_count, 2);
}

// ============================================================================
// Sequence gap detection
// ============================================================================

TEST(aaf_stream_input_seq, no_gap)
{
    AafStreamInputContext ctx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    ctx.update_sequence_num(0);
    ++ctx.packets_received;
    ctx.update_sequence_num(1);
    ++ctx.packets_received;
    ctx.update_sequence_num(2);
    EXPECT_EQ(ctx.sequence_gaps, 0U);
}

TEST(aaf_stream_input_seq, gap_detected)
{
    AafStreamInputContext ctx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    ctx.update_sequence_num(0);
    ++ctx.packets_received;
    ctx.update_sequence_num(5);
    EXPECT_EQ(ctx.sequence_gaps, 1U);
}

TEST(aaf_stream_input_seq, wrap_no_gap)
{
    AafStreamInputContext ctx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    ctx.update_sequence_num(255);
    ++ctx.packets_received;
    ctx.update_sequence_num(0);
    EXPECT_EQ(ctx.sequence_gaps, 0U);
}

// ============================================================================
// Helpers for building test packets
// ============================================================================

static auto build_aaf_pdu(
    AafFormat fmt,
    AafSampleRate rate,
    uint16_t channels,
    uint8_t depth,
    uint16_t samples,
    bool tv,
    uint32_t avtp_ts,
    uint8_t seq_num) -> AafPdu
{
    AafPdu pdu{};
    StreamId sid{};
    pdu.init(sid, fmt, rate, channels, depth);
    pdu.set_dimensions(samples, channels);
    pdu.set_tv(tv);
    pdu.set_avtp_timestamp(avtp_ts);
    pdu.set_sequence_num(seq_num);
    return pdu;
}

static auto build_int24_payload(uint16_t channels, uint16_t samples, float value = 0.5f) -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload(channels * samples * 3);
    for (uint16_t s = 0; s < samples; ++s) {
        for (uint16_t ch = 0; ch < channels; ++ch) {
            size_t const offset = (static_cast<size_t>(s) * channels + ch) * 3;
            (void)encode_aaf_sample(value, std::span{payload}, offset, AafFormat::int_24bit);
        }
    }
    return payload;
}

static auto build_float32_payload(uint16_t channels, uint16_t samples, float value = 0.5f) -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload(channels * samples * 4);
    for (uint16_t s = 0; s < samples; ++s) {
        for (uint16_t ch = 0; ch < channels; ++ch) {
            size_t const offset = (static_cast<size_t>(s) * channels + ch) * 4;
            (void)encode_aaf_sample(value, std::span{payload}, offset, AafFormat::float_32bit);
        }
    }
    return payload;
}

// ============================================================================
// Timestamp handling
// ============================================================================

TEST(aaf_stream_input_pts, first_packet_with_timestamp)
{
    AafStreamInputContext ctx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    auto pdu = build_aaf_pdu(AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, 6, true, 1'000'000U, 0);

    ctx.process_packet_header(pdu, 1'000'000U);
    EXPECT_TRUE(ctx.has_valid_timestamp);
    EXPECT_EQ(ctx.compute_pts_ns(), 1'000'000U);
    ctx.advance_samples(6);
}

TEST(aaf_stream_input_pts, extrapolated_pts)
{
    AafStreamInputContext ctx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};

    // Packet 1: tv=1, timestamp = 1'000'000
    auto pdu1 = build_aaf_pdu(AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, 6, true, 1'000'000U, 0);
    ctx.process_packet_header(pdu1, 1'000'000U);
    EXPECT_EQ(ctx.compute_pts_ns(), 1'000'000U);
    ctx.advance_samples(6);

    // Packet 2: tv=0, no timestamp — PTS extrapolated from anchor + 6 samples
    auto pdu2 = build_aaf_pdu(AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, 6, false, 0, 1);
    ctx.process_packet_header(pdu2, 1'125'000U);
    uint64_t const expected = 1'000'000U + 6U * 20833U;
    EXPECT_EQ(ctx.compute_pts_ns(), expected);
    ctx.advance_samples(6);
}

TEST(aaf_stream_input_pts, timestamp_reconstruction_across_wrap)
{
    AafStreamInputContext ctx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    uint64_t const gptp_now = 0x0000'0002'FFFF'FF00ULL;
    uint32_t const avtp_ts = 0x0000'0200U;  // just past 32-bit wrap

    auto pdu = build_aaf_pdu(AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, 6, true, avtp_ts, 0);
    ctx.process_packet_header(pdu, gptp_now);
    EXPECT_EQ(ctx.compute_pts_ns(), 0x0000'0003'0000'0200ULL);
    ctx.advance_samples(6);
}

TEST(aaf_stream_input_pts, no_timestamp_returns_zero)
{
    AafStreamInputContext ctx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    EXPECT_EQ(ctx.compute_pts_ns(), 0U);
}

// ============================================================================
// Deserializer — int24 format
// ============================================================================

TEST(aaf_stream_input_deser, int24_stereo_6_samples)
{
    AafStreamInputContext ctx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    auto payload = build_int24_payload(2, 6, 0.25f);
    auto pdu = build_aaf_pdu(AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, 6, true, 1'000'000U, 0);

    uint8_t callback_count = 0;
    aaf_stream_deserialize(
        ctx, pdu, std::span{payload}, 1'000'000U, [&](uint8_t channel, std::span<float> samples, uint64_t pts, uint64_t period) {
            EXPECT_TRUE(channel < 2);
            EXPECT_EQ(samples.size(), 6U);
            EXPECT_EQ(pts, 1'000'000U);
            EXPECT_EQ(period, 20833U);
            // Verify all samples decode to approximately 0.25
            for (float s : samples) {
                EXPECT_TRUE(std::abs(s - 0.25f) < 0.001f);
            }
            ++callback_count;
        });
    EXPECT_EQ(callback_count, 2);
    EXPECT_EQ(ctx.packets_received, 1U);
}

// ============================================================================
// Deserializer — float32 format
// ============================================================================

TEST(aaf_stream_input_deser, float32_mono_8_samples)
{
    AafStreamInputContext ctx{AafFormat::float_32bit, AafSampleRate::rate_48_khz, 1, 32};
    float const test_val = 0.75f;
    auto payload = build_float32_payload(1, 8, test_val);
    auto pdu = build_aaf_pdu(AafFormat::float_32bit, AafSampleRate::rate_48_khz, 1, 32, 8, true, 0, 0);

    bool called = false;
    aaf_stream_deserialize(ctx, pdu, std::span{payload}, 0U, [&](uint8_t channel, std::span<float> samples, uint64_t, uint64_t) {
        EXPECT_EQ(channel, 0);
        EXPECT_EQ(samples.size(), 8U);
        // float32 should be bit-exact
        for (float s : samples) {
            EXPECT_TRUE(s == test_val);
        }
        called = true;
    });
    EXPECT_TRUE(called);
}

// ============================================================================
// Deserializer — multi-packet PTS tracking
// ============================================================================

TEST(aaf_stream_input_deser, multi_packet_pts_tracking)
{
    AafStreamInputContext ctx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};

    auto payload = build_int24_payload(2, 6);

    // Packet 1: tv=1
    auto pdu1 = build_aaf_pdu(AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, 6, true, 1'000'000U, 0);
    uint64_t pts1 = 0;
    aaf_stream_deserialize(ctx, pdu1, std::span{payload}, 1'000'000U, [&](uint8_t ch, std::span<float>, uint64_t pts, uint64_t) {
        if (ch == 0) {
            pts1 = pts;
        }
    });
    EXPECT_EQ(pts1, 1'000'000U);

    // Packet 2: tv=0, extrapolated
    auto pdu2 = build_aaf_pdu(AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, 6, false, 0, 1);
    uint64_t pts2 = 0;
    aaf_stream_deserialize(ctx, pdu2, std::span{payload}, 1'125'000U, [&](uint8_t ch, std::span<float>, uint64_t pts, uint64_t) {
        if (ch == 0) {
            pts2 = pts;
        }
    });
    EXPECT_EQ(pts2, 1'000'000U + 6U * 20833U);

    // Packet 3: tv=1 with new anchor
    auto pdu3 = build_aaf_pdu(AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, 6, true, 2'000'000U, 2);
    uint64_t pts3 = 0;
    aaf_stream_deserialize(ctx, pdu3, std::span{payload}, 2'000'000U, [&](uint8_t ch, std::span<float>, uint64_t pts, uint64_t) {
        if (ch == 0) {
            pts3 = pts;
        }
    });
    EXPECT_EQ(pts3, 2'000'000U);
}

// ============================================================================
// Decoder safety: truncated / malformed packets must produce no callbacks
// and not crash. These tests prove the bounds checks in
// aaf_stream_deserialize() and is_valid_aaf_deserialize_params() actually
// fire on the inputs they're meant to reject.
// ============================================================================

TEST(aaf_stream_input_safety, payload_too_small_yields_no_callback)
{
    // ctx declares 2-channel int24 (bps=3); a one-byte payload can't even
    // form a single frame (needs 6 bytes). samples=0 path must trigger.
    AafStreamInputContext ctx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    auto pdu = build_aaf_pdu(AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, 6, true, 1'000'000U, 0);
    std::array<uint8_t, 1> tiny{0xAB};
    bool called = false;
    aaf_stream_deserialize(
        ctx, pdu, std::span<uint8_t const>{tiny}, 1'000'000U, [&](uint8_t, std::span<float>, uint64_t, uint64_t) {
            called = true;
        });
    EXPECT_FALSE(called);
    EXPECT_EQ(ctx.packets_received, 0U);  // no header processed when guard rejects
}

TEST(aaf_stream_input_safety, zero_channels_rejected)
{
    // channels=0 hits is_valid_aaf_deserialize_params, returns immediately.
    AafStreamInputContext ctx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 0, 24};
    auto pdu = build_aaf_pdu(AafFormat::int_24bit, AafSampleRate::rate_48_khz, 1, 24, 6, true, 1'000'000U, 0);
    std::array<uint8_t, 18> payload{};
    bool called = false;
    aaf_stream_deserialize(
        ctx, pdu, std::span<uint8_t const>{payload}, 1'000'000U, [&](uint8_t, std::span<float>, uint64_t, uint64_t) {
            called = true;
        });
    EXPECT_FALSE(called);
    EXPECT_EQ(ctx.packets_received, 0U);
}

TEST(aaf_stream_input_safety, user_specified_format_zero_bps_rejected)
{
    // bps=0 (user_specified) hits is_valid_aaf_deserialize_params guard.
    AafStreamInputContext ctx{AafFormat::user_specified, AafSampleRate::rate_48_khz, 2, 0};
    auto pdu = build_aaf_pdu(AafFormat::user_specified, AafSampleRate::rate_48_khz, 2, 0, 6, true, 0, 0);
    std::array<uint8_t, 12> payload{};
    bool called = false;
    aaf_stream_deserialize(
        ctx, pdu, std::span<uint8_t const>{payload}, 0U, [&](uint8_t, std::span<float>, uint64_t, uint64_t) { called = true; });
    EXPECT_FALSE(called);
}

TEST(aaf_stream_input_safety, a_packet_over_the_scratch_capacity_is_rejected)
{
    // The scratch holds MaxChannels * MaxSamples samples in all. Two lanes of 128
    // cannot take 300 samples of 2 channels; the guard rejects before the header.
    AafStreamInputContext ctx{AafFormat::int_16bit, AafSampleRate::rate_48_khz, 2, 16};
    auto pdu = build_aaf_pdu(AafFormat::int_16bit, AafSampleRate::rate_48_khz, 2, 16, 300, true, 0, 0);
    std::vector<uint8_t> payload(300U * 2U * 2U);  // 300 samples × 2 channels × 2 bytes
    bool called = false;
    aaf_stream_deserialize</*MaxChannels=*/2, /*MaxSamples=*/128>(
        ctx, pdu, std::span<uint8_t const>{payload}, 0U, [&](uint8_t, std::span<float>, uint64_t, uint64_t) { called = true; });
    EXPECT_FALSE(called);
    EXPECT_EQ(ctx.packets_received, 0U);
}

// The bound is the scratch's capacity, not MaxSamples per channel: a mono int16 packet
// filling an Ethernet frame carries 700-odd samples, which the per-channel bound of
// 256 dropped before the header was read.
TEST(aaf_stream_input, a_full_frame_of_mono_int16_decodes_with_the_default_scratch)
{
    constexpr uint16_t samples = 700;
    AafStreamInputContext ctx{AafFormat::int_16bit, AafSampleRate::rate_48_khz, 1, 16};
    auto pdu = build_aaf_pdu(AafFormat::int_16bit, AafSampleRate::rate_48_khz, 1, 16, samples, true, 1'000'000U, 0);
    std::vector<uint8_t> payload(size_t{samples} * 2U);
    for (size_t s = 0; s < samples; ++s) {
        payload[s * 2] = static_cast<uint8_t>(s >> 8);  // a ramp, big-endian int16
        payload[(s * 2) + 1] = static_cast<uint8_t>(s & 0xFFU);
    }
    size_t got = 0;
    float last = 0.0f;
    aaf_stream_deserialize(
        ctx, pdu, std::span<uint8_t const>{payload}, 1'000'000U, [&](uint8_t, std::span<float> s, uint64_t, uint64_t) {
            got = s.size();
            last = s.back();
        });
    EXPECT_EQ(got, size_t{samples});
    EXPECT_TRUE(last > 0.0f);  // the lane is this channel's samples in order
    EXPECT_EQ(ctx.packets_received, 1U);
}

TEST(aaf_stream_input_safety, format_mismatch_silently_uses_ctx_format)
{
    // Documenting current behavior: aaf_stream_deserialize never compares
    // pdu.format against ctx.format. A peer that switches encoding mid
    // stream will be silently mis-decoded. If we ever add a runtime check
    // that rejects format-change packets, this test should be updated.
    AafStreamInputContext ctx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    auto pdu = build_aaf_pdu(AafFormat::int_32bit, AafSampleRate::rate_48_khz, 2, 32, 4, true, 0, 0);
    // Sized for ctx (int24=3 bytes/sample): 2ch * 4samples * 3 = 24 bytes.
    std::vector<uint8_t> payload(24, 0x55);
    int callbacks = 0;
    aaf_stream_deserialize(
        ctx, pdu, std::span<uint8_t const>{payload}, 0U, [&](uint8_t, std::span<float>, uint64_t, uint64_t) { ++callbacks; });
    EXPECT_EQ(callbacks, 2);  // one per channel; decoded as int24 per ctx
}

TEST_MAIN(statusbar_avtp, avtp_aaf_stream_input_test)
