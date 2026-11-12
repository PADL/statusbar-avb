// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_am824_stream_input.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace statusbar;
using namespace statusbar::avtp;
using namespace statusbar::tsn;

// ============================================================================
// quadlet_t span_load from network-order bytes
// ============================================================================

TEST(am824_stream_util, quadlet_span_load)
{
    uint8_t const bytes[] = {0x40, 0x12, 0x34, 0x56};
    ieee::quadlet_t q{};
    span_load(q, std::span<uint8_t const>(bytes, 4));
    EXPECT_EQ(static_cast<uint32_t>(q), 0x40123456U);
}

TEST(am824_stream_util, quadlet_span_load_zeros)
{
    uint8_t const bytes[] = {0x00, 0x00, 0x00, 0x00};
    ieee::quadlet_t q{};
    span_load(q, std::span<uint8_t const>(bytes, 4));
    EXPECT_EQ(static_cast<uint32_t>(q), 0U);
}

TEST(am824_stream_util, quadlet_span_load_all_ones)
{
    uint8_t const bytes[] = {0xFF, 0xFF, 0xFF, 0xFF};
    ieee::quadlet_t q{};
    span_load(q, std::span<uint8_t const>(bytes, 4));
    EXPECT_EQ(static_cast<uint32_t>(q), 0xFFFFFFFFU);
}

// ============================================================================
// am824_extract_midi_bytes
// ============================================================================

TEST(am824_stream_midi_extract, counter_zero_no_data)
{
    // label=0x80 (counter=0), no valid bytes
    uint32_t const quadlet = 0x80000000U;
    uint8_t out[3]{};
    EXPECT_EQ(am824_extract_midi_bytes(quadlet, out), 0);
}

TEST(am824_stream_midi_extract, counter_one)
{
    // label=0x81 (counter=1), Byte1=0x90
    uint32_t const quadlet = 0x81900000U;
    uint8_t out[3]{};
    EXPECT_EQ(am824_extract_midi_bytes(quadlet, out), 1);
    EXPECT_EQ(out[0], 0x90);
}

TEST(am824_stream_midi_extract, counter_two)
{
    // label=0x82 (counter=2), Byte1=0x90, Byte2=0x3C
    uint32_t const quadlet = 0x82903C00U;
    uint8_t out[3]{};
    EXPECT_EQ(am824_extract_midi_bytes(quadlet, out), 2);
    EXPECT_EQ(out[0], 0x90);
    EXPECT_EQ(out[1], 0x3C);
}

TEST(am824_stream_midi_extract, counter_three)
{
    // label=0x83 (counter=3), Byte1=0x90, Byte2=0x3C, Byte3=0x7F
    uint32_t const quadlet = 0x83903C7FU;
    uint8_t out[3]{};
    EXPECT_EQ(am824_extract_midi_bytes(quadlet, out), 3);
    EXPECT_EQ(out[0], 0x90);
    EXPECT_EQ(out[1], 0x3C);
    EXPECT_EQ(out[2], 0x7F);
}

// ============================================================================
// am824_extract_smpte_part
// ============================================================================

TEST(am824_stream_smpte_extract, counter_zero_no_data)
{
    uint32_t const quadlet = 0x88000000U;  // label=0x88, counter=0
    std::array<uint8_t, 3> out{};
    EXPECT_EQ(am824_extract_smpte_part(quadlet, out), 0);
}

TEST(am824_stream_smpte_extract, first_part)
{
    // label=0x89 (counter=1=first), payload: frames=0x15, seconds=0x30, minutes=0x01
    uint32_t const quadlet = 0x89153001U;
    std::array<uint8_t, 3> out{};
    EXPECT_EQ(am824_extract_smpte_part(quadlet, out), 1);
    EXPECT_EQ(out[0], 0x15);
    EXPECT_EQ(out[1], 0x30);
    EXPECT_EQ(out[2], 0x01);
}

TEST(am824_stream_smpte_extract, middle_part)
{
    // label=0x8A (counter=2=middle), payload: hours=0x12, bg1=0xAB, bg3=0xCD
    uint32_t const quadlet = 0x8A12ABCDU;
    std::array<uint8_t, 3> out{};
    EXPECT_EQ(am824_extract_smpte_part(quadlet, out), 2);
    EXPECT_EQ(out[0], 0x12);
    EXPECT_EQ(out[1], 0xAB);
    EXPECT_EQ(out[2], 0xCD);
}

TEST(am824_stream_smpte_extract, last_part)
{
    // label=0x8B (counter=3=last), payload: bg5=0x11, bg6=0x22, bg7_8=0x33
    uint32_t const quadlet = 0x8B112233U;
    std::array<uint8_t, 3> out{};
    EXPECT_EQ(am824_extract_smpte_part(quadlet, out), 3);
    EXPECT_EQ(out[0], 0x11);
    EXPECT_EQ(out[1], 0x22);
    EXPECT_EQ(out[2], 0x33);
}

// ============================================================================
// reconstruct_avtp_timestamp
// ============================================================================

TEST(am824_stream_reconstruct, same_epoch)
{
    // AVTP timestamp and gptp_now in the same 32-bit epoch
    uint64_t const gptp_now = 0x0000'0003'0000'0000ULL + 1'000'000;
    uint32_t const avtp_ts = 1'500'000;  // slightly ahead of now (presentation time)
    uint64_t const result = reconstruct_avtp_timestamp(avtp_ts, gptp_now);
    EXPECT_EQ(result, 0x0000'0003'0000'0000ULL + 1'500'000);
}

TEST(am824_stream_reconstruct, wrap_forward)
{
    // gptp_now is near the end of a 32-bit epoch, AVTP timestamp wrapped to next epoch
    uint64_t const gptp_now = 0x0000'0002'FFFF'FF00ULL;
    uint32_t const avtp_ts = 0x0000'0100U;  // just past the 32-bit wrap
    uint64_t const result = reconstruct_avtp_timestamp(avtp_ts, gptp_now);
    EXPECT_EQ(result, 0x0000'0003'0000'0100ULL);
}

TEST(am824_stream_reconstruct, wrap_backward)
{
    // gptp_now just past a 32-bit boundary, AVTP timestamp is from just before it
    uint64_t const gptp_now = 0x0000'0003'0000'0100ULL;
    uint32_t const avtp_ts = 0xFFFF'FF00U;
    uint64_t const result = reconstruct_avtp_timestamp(avtp_ts, gptp_now);
    EXPECT_EQ(result, 0x0000'0002'FFFF'FF00ULL);
}

TEST(am824_stream_reconstruct, zero_epoch)
{
    // When gptp_now is in the first 32-bit epoch, no upper bits to set
    uint64_t const gptp_now = 500'000;
    uint32_t const avtp_ts = 1'000'000;
    uint64_t const result = reconstruct_avtp_timestamp(avtp_ts, gptp_now);
    EXPECT_EQ(result, 1'000'000ULL);
}

TEST(am824_stream_reconstruct, large_gptp_time)
{
    // Realistic gPTP time ~100 seconds after boot (100 billion ns)
    uint64_t const gptp_now = 100'000'000'000ULL;
    uint32_t const lower32_now = static_cast<uint32_t>(gptp_now & 0xFFFF'FFFFU);
    // Presentation time is 2ms ahead
    uint32_t const avtp_ts = lower32_now + 2'000'000;
    uint64_t const result = reconstruct_avtp_timestamp(avtp_ts, gptp_now);
    EXPECT_EQ(result, gptp_now + 2'000'000);
}

// ============================================================================
// Am824StreamInputContext construction and reset
// ============================================================================

TEST(am824_stream_ctx, default_construction)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    EXPECT_EQ(ctx.config.channel_count, 2);
    EXPECT_EQ(ctx.config.syt_interval, 8);
    EXPECT_EQ(ctx.config.sample_period_ns, 20833);
    EXPECT_EQ(ctx.running_dbc, 0U);
    EXPECT_FALSE(ctx.has_valid_anchor);
    EXPECT_EQ(ctx.packets_received, 0U);
}

TEST(am824_stream_ctx, construction_96khz)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_96_khz, 8};
    EXPECT_EQ(ctx.config.channel_count, 8);
    EXPECT_EQ(ctx.config.syt_interval, 16);
    EXPECT_EQ(ctx.config.sample_period_ns, 10416);
}

TEST(am824_stream_ctx, reset)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    ctx.running_dbc = 1000;
    ctx.packets_received = 50;
    ctx.has_valid_anchor = true;
    ctx.reset();
    EXPECT_EQ(ctx.running_dbc, 0U);
    EXPECT_EQ(ctx.packets_received, 0U);
    EXPECT_FALSE(ctx.has_valid_anchor);
    EXPECT_EQ(ctx.config.syt_interval, 8);
    EXPECT_EQ(ctx.config.channel_count, 2);
}

// ============================================================================
// DBC unwrapping
// ============================================================================

TEST(am824_stream_ctx, dbc_unwrap_simple)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    ctx.update_dbc(0);
    EXPECT_EQ(ctx.running_dbc, 0U);

    ctx.update_dbc(6);
    EXPECT_EQ(ctx.running_dbc, 6U);

    ctx.update_dbc(12);
    EXPECT_EQ(ctx.running_dbc, 12U);
}

TEST(am824_stream_ctx, dbc_unwrap_across_boundary)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    ctx.running_dbc = 250;
    ctx.last_dbc = 250;
    ctx.packets_received = 1;  // not first packet

    ctx.update_dbc(254);
    EXPECT_EQ(ctx.running_dbc, 254U);

    ctx.update_dbc(4);
    EXPECT_EQ(ctx.running_dbc, 260U);
}

TEST(am824_stream_ctx, dbc_unwrap_multiple_wraps)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    ctx.running_dbc = 500;
    ctx.last_dbc = 244;
    ctx.packets_received = 1;

    ctx.update_dbc(6);
    EXPECT_EQ(ctx.running_dbc, 518U);
}

// ============================================================================
// Timestamp anchoring (with gptp_now_ns for 32→64 bit reconstruction)
// ============================================================================

TEST(am824_stream_pts, anchor_on_tv_set)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    ctx.update_dbc(0);
    // SYT_INTERVAL=8, DBC=0: index = mod(8 - mod(0,8), 8) = 0
    ctx.update_timestamp(true, 1'000'000U, 0, 1'000'000U);
    EXPECT_TRUE(ctx.has_valid_anchor);
    EXPECT_EQ(ctx.last_anchor_pts_ns, 1'000'000U);
    EXPECT_EQ(ctx.last_anchor_dbc, 0U);
}

TEST(am824_stream_pts, base_pts_first_packet)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    ctx.update_dbc(0);
    ctx.update_timestamp(true, 1'000'000U, 0, 1'000'000U);
    uint64_t base = ctx.compute_base_pts_ns();
    EXPECT_EQ(base, 1'000'000U);
}

TEST(am824_stream_pts, base_pts_extrapolated)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    ctx.update_dbc(0);
    ctx.update_timestamp(true, 1'000'000U, 0, 1'000'000U);
    ++ctx.packets_received;

    ctx.update_dbc(6);
    ctx.update_timestamp(false, 0, 6, 1'125'000U);
    uint64_t base = ctx.compute_base_pts_ns();
    EXPECT_EQ(base, 1'000'000U + 6U * 20833U);
}

TEST(am824_stream_pts, anchor_with_nonzero_index)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    // DBC=4: index = mod(8 - mod(4,8), 8) = 4
    ctx.update_dbc(4);
    ctx.update_timestamp(true, 2'000'000U, 4, 2'000'000U);
    EXPECT_EQ(ctx.last_anchor_dbc, 4U + 4U);

    uint64_t base = ctx.compute_base_pts_ns();
    EXPECT_EQ(base, 2'000'000U - 4U * 20833U);
}

TEST(am824_stream_pts, timestamp_reconstruction_across_32bit_wrap)
{
    // Test that a 32-bit AVTP timestamp near the wrap boundary is correctly
    // reconstructed to a full 64-bit gPTP value using the receive time
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    uint64_t const gptp_now = 0x0000'0002'FFFF'0000ULL;
    uint32_t const avtp_ts = 0xFFFF'1000U;  // slightly ahead of lower 32 bits of gptp_now
    ctx.update_dbc(0);
    ctx.update_timestamp(true, avtp_ts, 0, gptp_now);
    EXPECT_TRUE(ctx.has_valid_anchor);
    EXPECT_EQ(ctx.last_anchor_pts_ns, 0x0000'0002'FFFF'1000ULL);
}

TEST(am824_stream_pts, timestamp_reconstruction_wrap_forward)
{
    // gptp_now near end of 32-bit epoch, AVTP timestamp has wrapped to next epoch
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    uint64_t const gptp_now = 0x0000'0002'FFFF'FF00ULL;
    uint32_t const avtp_ts = 0x0000'0200U;  // just past the 32-bit wrap
    ctx.update_dbc(0);
    ctx.update_timestamp(true, avtp_ts, 0, gptp_now);
    EXPECT_TRUE(ctx.has_valid_anchor);
    EXPECT_EQ(ctx.last_anchor_pts_ns, 0x0000'0003'0000'0200ULL);
}

// ============================================================================
// Sequence gap detection
// ============================================================================

TEST(am824_stream_seq, no_gap)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    ctx.update_sequence_num(0);
    ++ctx.packets_received;
    ctx.update_sequence_num(1);
    ++ctx.packets_received;
    ctx.update_sequence_num(2);
    EXPECT_EQ(ctx.sequence_gaps, 0U);
}

TEST(am824_stream_seq, gap_detected)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    ctx.update_sequence_num(0);
    ++ctx.packets_received;
    ctx.update_sequence_num(1);
    ++ctx.packets_received;
    ctx.update_sequence_num(5);
    EXPECT_EQ(ctx.sequence_gaps, 1U);
}

TEST(am824_stream_seq, wrap_no_gap)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    ctx.update_sequence_num(254);
    ++ctx.packets_received;
    ctx.update_sequence_num(255);
    ++ctx.packets_received;
    ctx.update_sequence_num(0);
    EXPECT_EQ(ctx.sequence_gaps, 0U);
}

// ============================================================================
// Helpers for building test packets
// ============================================================================

static auto build_mbla_payload(uint8_t channels, uint8_t samples, float base_value = 0.5f) -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload(channels * samples * 4);
    for (uint8_t s = 0; s < samples; ++s) {
        for (uint8_t ch = 0; ch < channels; ++ch) {
            float const val = base_value * static_cast<float>(s + 1) / static_cast<float>(samples);
            int32_t const sample24 = float_to_am824_sample(val);
            uint32_t const quadlet = create_am824_quadlet(sample24);
            size_t const offset = (s * channels + ch) * 4;
            payload[offset + 0] = static_cast<uint8_t>((quadlet >> 24) & 0xFF);
            payload[offset + 1] = static_cast<uint8_t>((quadlet >> 16) & 0xFF);
            payload[offset + 2] = static_cast<uint8_t>((quadlet >> 8) & 0xFF);
            payload[offset + 3] = static_cast<uint8_t>(quadlet & 0xFF);
        }
    }
    return payload;
}

static auto build_am824_pdu(uint8_t channels, uint8_t samples, uint8_t dbc, bool tv, uint32_t avtp_ts, uint8_t seq_num) -> Am824Pdu
{
    Am824Pdu pdu{};
    StreamId sid{};
    pdu.init(sid, channels, Am824SampleRate::rate_48_khz);
    pdu.set_dimensions(samples, channels);
    pdu.set_data_block_count(dbc);
    pdu.set_tv(tv);
    pdu.set_avtp_timestamp(avtp_ts);
    pdu.set_sequence_num(seq_num);
    return pdu;
}

/// Build a quadlet from label and 24-bit payload, return as 4 big-endian bytes
static auto quadlet_bytes(uint8_t label, uint32_t payload24) -> std::array<uint8_t, 4>
{
    uint32_t const q = (static_cast<uint32_t>(label) << 24) | (payload24 & 0x00FFFFFFU);
    return {
        static_cast<uint8_t>((q >> 24) & 0xFF),
        static_cast<uint8_t>((q >> 16) & 0xFF),
        static_cast<uint8_t>((q >> 8) & 0xFF),
        static_cast<uint8_t>(q & 0xFF),
    };
}

// ============================================================================
// process_packet_header
// ============================================================================

TEST(am824_stream_ctx, process_packet_header)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    auto pdu = build_am824_pdu(2, 6, 0, true, 1'000'000, 0);
    ctx.process_packet_header(pdu, 6, 1'000'000U);
    EXPECT_EQ(ctx.packets_received, 1U);
    EXPECT_EQ(ctx.running_dbc, 0U);
    EXPECT_TRUE(ctx.has_valid_anchor);
    EXPECT_EQ(ctx.last_anchor_pts_ns, 1'000'000U);
}

// ============================================================================
// MBLA deserializer tests
// ============================================================================

TEST(am824_stream_mbla, stereo_6_samples)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    auto payload = build_mbla_payload(2, 6);
    auto pdu = build_am824_pdu(2, 6, 0, true, 1'000'000, 0);

    uint8_t callback_count = 0;
    am824_deserialize_mbla(
        ctx, pdu, std::span{payload}, 1'000'000U, [&](uint8_t channel, std::span<float> samples, uint64_t pts, uint64_t period) {
            EXPECT_TRUE(channel < 2);
            EXPECT_EQ(samples.size(), 6U);
            EXPECT_EQ(period, 20833U);
            EXPECT_EQ(pts, 1'000'000U);
            ++callback_count;
        });
    EXPECT_EQ(callback_count, 2);
    EXPECT_EQ(ctx.packets_received, 1U);
}

TEST(am824_stream_mbla, multi_packet_dbc_tracking)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};

    auto payload1 = build_mbla_payload(2, 6);
    auto pdu1 = build_am824_pdu(2, 6, 0, true, 1'000'000, 0);
    am824_deserialize_mbla(ctx, pdu1, std::span{payload1}, 1'000'000U, [](uint8_t, std::span<float>, uint64_t, uint64_t) {});

    auto payload2 = build_mbla_payload(2, 6);
    auto pdu2 = build_am824_pdu(2, 6, 6, false, 0, 1);

    uint64_t received_pts = 0;
    am824_deserialize_mbla(
        ctx, pdu2, std::span{payload2}, 1'125'000U, [&](uint8_t channel, std::span<float>, uint64_t pts, uint64_t) {
            if (channel == 0) {
                received_pts = pts;
            }
        });
    EXPECT_EQ(received_pts, 1'000'000U + 6U * 20833U);
    EXPECT_EQ(ctx.packets_received, 2U);
}

TEST(am824_stream_mbla, sample_values_correct)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 1};
    float const input_val = 0.75f;
    int32_t const sample24 = float_to_am824_sample(input_val);
    uint32_t const quadlet = create_am824_quadlet(sample24);
    std::array<uint8_t, 4> payload{
        static_cast<uint8_t>((quadlet >> 24) & 0xFF),
        static_cast<uint8_t>((quadlet >> 16) & 0xFF),
        static_cast<uint8_t>((quadlet >> 8) & 0xFF),
        static_cast<uint8_t>(quadlet & 0xFF),
    };
    auto pdu = build_am824_pdu(1, 1, 0, true, 0, 0);

    float received_sample = 0.0f;
    am824_deserialize_mbla(ctx, pdu, std::span{payload}, 0U, [&](uint8_t, std::span<float> samples, uint64_t, uint64_t) {
        received_sample = samples[0];
    });
    float const expected = am824_sample_to_float(float_to_am824_sample(input_val));
    EXPECT_TRUE(std::abs(received_sample - expected) < 1e-6f);
}

// ============================================================================
// Mixed deserializer tests
// ============================================================================

TEST(am824_stream_mixed, audio_plus_midi)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 3};

    std::vector<uint8_t> payload;
    // Data block 0: [ch0=audio, ch1=audio, ch2=MIDI(no data)]
    auto q0 = quadlet_bytes(AM824_LABEL_MBLA, 0x400000);
    auto q1 = quadlet_bytes(AM824_LABEL_MBLA, 0x200000);
    auto q2 = quadlet_bytes(AM824_LABEL_RAW_MIDI, 0x000000);
    payload.insert(payload.end(), q0.begin(), q0.end());
    payload.insert(payload.end(), q1.begin(), q1.end());
    payload.insert(payload.end(), q2.begin(), q2.end());

    // Data block 1: [ch0=audio, ch1=audio, ch2=MIDI(counter=2, bytes: 0x90, 0x3C)]
    auto q3 = quadlet_bytes(AM824_LABEL_MBLA, 0x600000);
    auto q4 = quadlet_bytes(AM824_LABEL_MBLA, 0x100000);
    auto q5 = quadlet_bytes(0x82, 0x903C00);  // label=0x82 means MIDI counter=2
    payload.insert(payload.end(), q3.begin(), q3.end());
    payload.insert(payload.end(), q4.begin(), q4.end());
    payload.insert(payload.end(), q5.begin(), q5.end());

    auto pdu = build_am824_pdu(3, 2, 0, true, 1'000'000, 0);

    uint8_t audio_callbacks = 0;
    uint8_t midi_callbacks = 0;
    size_t midi_byte_count = 0;

    am824_deserialize_mixed(
        ctx,
        pdu,
        std::span{payload},
        1'000'000U,
        [&](uint8_t ch, std::span<float> samples, uint64_t, uint64_t) {
            EXPECT_TRUE(ch < 2);
            EXPECT_EQ(samples.size(), 2U);
            ++audio_callbacks;
        },
        [&](uint8_t ch, std::span<uint8_t const> midi_bytes, uint64_t, uint64_t) {
            EXPECT_EQ(ch, 2);
            midi_byte_count = midi_bytes.size();
            EXPECT_EQ(midi_bytes[0], 0x90);
            EXPECT_EQ(midi_bytes[1], 0x3C);
            ++midi_callbacks;
        },
        [&](uint8_t, uint8_t, std::span<uint8_t const, 3>, uint64_t, uint64_t) {
            EXPECT_TRUE(false);  // no SMPTE expected
        });

    EXPECT_EQ(audio_callbacks, 2);
    EXPECT_EQ(midi_callbacks, 1);
    EXPECT_EQ(midi_byte_count, 2U);
}

TEST(am824_stream_mixed, midi_no_data_no_callback)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};

    std::vector<uint8_t> payload;
    auto q0 = quadlet_bytes(AM824_LABEL_MBLA, 0x400000);
    auto q1 = quadlet_bytes(AM824_LABEL_RAW_MIDI, 0x000000);
    auto q2 = quadlet_bytes(AM824_LABEL_MBLA, 0x400000);
    auto q3 = quadlet_bytes(AM824_LABEL_RAW_MIDI, 0x000000);
    payload.insert(payload.end(), q0.begin(), q0.end());
    payload.insert(payload.end(), q1.begin(), q1.end());
    payload.insert(payload.end(), q2.begin(), q2.end());
    payload.insert(payload.end(), q3.begin(), q3.end());

    auto pdu = build_am824_pdu(2, 2, 0, true, 0, 0);

    bool midi_called = false;
    am824_deserialize_mixed(
        ctx,
        pdu,
        std::span{payload},
        0U,
        [](uint8_t, std::span<float>, uint64_t, uint64_t) {},
        [&](uint8_t, std::span<uint8_t const>, uint64_t, uint64_t) { midi_called = true; },
        [](uint8_t, uint8_t, std::span<uint8_t const, 3>, uint64_t, uint64_t) {});

    EXPECT_FALSE(midi_called);
}

TEST(am824_stream_mixed, smpte_first_part)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};

    std::vector<uint8_t> payload;
    // Data block 0: [audio, SMPTE no-data]
    auto q0 = quadlet_bytes(AM824_LABEL_MBLA, 0x400000);
    auto q1 = quadlet_bytes(AM824_LABEL_SMPTE, 0x000000);  // counter=0
    payload.insert(payload.end(), q0.begin(), q0.end());
    payload.insert(payload.end(), q1.begin(), q1.end());

    // Data block 1: [audio, SMPTE first part (counter=1)]
    auto q2 = quadlet_bytes(AM824_LABEL_MBLA, 0x400000);
    // label=0x89 means SMPTE counter=1 (first part)
    auto q3 = quadlet_bytes(0x89, 0x153001);
    payload.insert(payload.end(), q2.begin(), q2.end());
    payload.insert(payload.end(), q3.begin(), q3.end());

    auto pdu = build_am824_pdu(2, 2, 0, true, 0, 0);

    uint8_t smpte_callbacks = 0;
    uint8_t received_part = 0;
    std::array<uint8_t, 3> received_bytes{};

    am824_deserialize_mixed(
        ctx,
        pdu,
        std::span{payload},
        0U,
        [](uint8_t, std::span<float>, uint64_t, uint64_t) {},
        [](uint8_t, std::span<uint8_t const>, uint64_t, uint64_t) {},
        [&](uint8_t ch, uint8_t part, std::span<uint8_t const, 3> bytes, uint64_t, uint64_t) {
            EXPECT_EQ(ch, 1);
            received_part = part;
            received_bytes[0] = bytes[0];
            received_bytes[1] = bytes[1];
            received_bytes[2] = bytes[2];
            ++smpte_callbacks;
        });

    EXPECT_EQ(smpte_callbacks, 1);
    EXPECT_EQ(received_part, 1);
    EXPECT_EQ(received_bytes[0], 0x15);
    EXPECT_EQ(received_bytes[1], 0x30);
    EXPECT_EQ(received_bytes[2], 0x01);
}

TEST(am824_stream_mixed, all_audio_same_as_mbla)
{
    Am824StreamInputContext ctx1{Am824SampleRate::rate_48_khz, 2};
    Am824StreamInputContext ctx2{Am824SampleRate::rate_48_khz, 2};
    auto payload = build_mbla_payload(2, 6, 0.3f);
    auto pdu = build_am824_pdu(2, 6, 0, true, 500'000, 0);

    std::array<float, 6> mbla_ch0{};
    std::array<float, 6> mixed_ch0{};

    am824_deserialize_mbla(ctx1, pdu, std::span{payload}, 500'000U, [&](uint8_t ch, std::span<float> samples, uint64_t, uint64_t) {
        if (ch == 0) {
            std::copy(samples.begin(), samples.end(), mbla_ch0.begin());
        }
    });

    am824_deserialize_mixed(
        ctx2,
        pdu,
        std::span{payload},
        500'000U,
        [&](uint8_t ch, std::span<float> samples, uint64_t, uint64_t) {
            if (ch == 0) {
                std::copy(samples.begin(), samples.end(), mixed_ch0.begin());
            }
        },
        [](uint8_t, std::span<uint8_t const>, uint64_t, uint64_t) {},
        [](uint8_t, uint8_t, std::span<uint8_t const, 3>, uint64_t, uint64_t) {});

    for (size_t i = 0; i < 6; ++i) {
        EXPECT_TRUE(std::abs(mbla_ch0[i] - mixed_ch0[i]) < 1e-10f);
    }
}

//
// AM824 deserializer safety: am824_deserialize_mbla and _mixed both
// compute `samples = payload.size() / (4 * channels)` and bail via
// is_valid_am824_deserialize_params. These tests exercise each guard.
//

TEST(am824_stream_input_safety, channels_zero_in_ctx)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 0};
    Am824Pdu pdu{};
    std::array<uint8_t, 48> payload{};
    bool called = false;
    am824_deserialize_mbla(
        ctx, pdu, std::span<uint8_t const>{payload}, 0U, [&](uint8_t, std::span<float>, uint64_t, uint64_t) { called = true; });
    EXPECT_FALSE(called);
}

TEST(am824_stream_input_safety, payload_smaller_than_one_frame)
{
    // 2 channels * 4 bytes/sample = 8 bytes per frame. Payload of 3
    // yields samples=0 → guard rejects.
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    Am824Pdu pdu{};
    std::array<uint8_t, 3> payload{0xAA, 0xBB, 0xCC};
    bool called = false;
    am824_deserialize_mbla(
        ctx, pdu, std::span<uint8_t const>{payload}, 0U, [&](uint8_t, std::span<float>, uint64_t, uint64_t) { called = true; });
    EXPECT_FALSE(called);
}

TEST(am824_stream_input_safety, payload_at_exact_one_frame_boundary)
{
    // 2 channels * 4 = 8 bytes — exactly one frame; should produce exactly
    // one callback per channel, and ctx state must advance.
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 2};
    Am824Pdu pdu{};
    std::array<uint8_t, 8> payload{0x40, 0, 0, 0, 0x40, 0, 0, 0};  // MBLA label per channel
    int callbacks = 0;
    am824_deserialize_mbla(ctx, pdu, std::span<uint8_t const>{payload}, 0U, [&](uint8_t, std::span<float> s, uint64_t, uint64_t) {
        EXPECT_EQ(s.size(), size_t{1});
        ++callbacks;
    });
    EXPECT_EQ(callbacks, 2);
    EXPECT_EQ(ctx.packets_received, 1U);
}

TEST(am824_stream_input_safety, a_packet_over_the_scratch_capacity_is_rejected)
{
    // The scratch holds MaxChannels * MaxSamples quadlets in all. One lane of 4
    // cannot take a 10-block mono packet; the guard rejects before the header.
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 1};
    Am824Pdu pdu{};
    std::array<uint8_t, 40> payload{};  // 10 samples × 1 channel × 4 = 40
    bool called = false;
    am824_deserialize_mbla</*MaxChannels=*/1, /*MaxSamples=*/4>(
        ctx, pdu, std::span<uint8_t const>{payload}, 0U, [&](uint8_t, std::span<float>, uint64_t, uint64_t) { called = true; });
    EXPECT_FALSE(called);
    EXPECT_EQ(ctx.packets_received, 0U);
}

// The bound is the scratch's capacity, not MaxSamples per channel: 192 kHz class B
// carries 48 data blocks a packet, which the per-channel bound of 32 dropped before
// the header was even read, so a 192 kHz stream decoded nothing and every packet
// counted as a sequence gap.
TEST(am824_stream_mbla, a_192_khz_class_b_stereo_packet_decodes_with_the_default_scratch)
{
    EXPECT_TRUE(48 > Am824Pdu::MAX_SAMPLES_PER_PACKET);  // the shape the old bound dropped
    Am824StreamInputContext ctx{Am824SampleRate::rate_192_khz, 2};
    auto payload = build_mbla_payload(2, 48);
    auto pdu = build_am824_pdu(2, 48, 0, true, 1'000'000, 0);
    pdu.set_sample_rate(Am824SampleRate::rate_192_khz);

    int callbacks = 0;
    am824_deserialize_mbla(ctx, pdu, std::span{payload}, 1'000'000U, [&](uint8_t ch, std::span<float> s, uint64_t pts, uint64_t) {
        EXPECT_EQ(s.size(), 48U);
        EXPECT_EQ(pts, 1'000'000U);
        // Lane ch holds channel ch's samples in order: the last is the largest (build_mbla_payload ramps).
        EXPECT_TRUE(s[47] > s[0]);
        (void)ch;
        ++callbacks;
    });
    EXPECT_EQ(callbacks, 2);
    EXPECT_EQ(ctx.packets_received, 1U);
    EXPECT_EQ(ctx.sequence_gaps, 0U);
}

TEST(am824_stream_mixed, a_192_khz_class_b_stereo_packet_decodes_with_the_default_scratch)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_192_khz, 2};
    auto payload = build_mbla_payload(2, 48);
    auto pdu = build_am824_pdu(2, 48, 0, true, 1'000'000, 0);
    int callbacks = 0;
    am824_deserialize_mixed(
        ctx,
        pdu,
        std::span{payload},
        1'000'000U,
        [&](uint8_t, std::span<float> s, uint64_t, uint64_t) {
            EXPECT_EQ(s.size(), 48U);
            ++callbacks;
        },
        [](uint8_t, std::span<uint8_t const>, uint64_t, uint64_t) {},
        [](uint8_t, uint8_t, std::span<uint8_t const, 3>, uint64_t, uint64_t) {});
    EXPECT_EQ(callbacks, 2);
}

// The data block count is no longer an octet: a mono packet filling a 1500-octet
// frame has 300-odd blocks, which an 8-bit count folded to 44 or so.
TEST(am824_stream_mbla, a_mono_packet_of_300_blocks_keeps_its_count)
{
    Am824StreamInputContext ctx{Am824SampleRate::rate_48_khz, 1};
    std::vector<uint8_t> payload(300U * 4U);
    for (size_t s = 0; s < 300; ++s) {
        auto const q = quadlet_bytes(AM824_LABEL_MBLA, 0x000100U * static_cast<uint32_t>(s + 1));
        for (size_t i = 0; i < 4; ++i) {
            payload[(s * 4) + i] = q[i];
        }
    }
    Am824Pdu pdu{};
    pdu.init(StreamId{}, 1, Am824SampleRate::rate_48_khz);
    pdu.set_stream_data_length(static_cast<uint16_t>(Cip61883Header::LENGTH + payload.size()));
    EXPECT_EQ(pdu.sample_count(), 300U);

    size_t got = 0;
    am824_deserialize_mbla(
        ctx, pdu, std::span{payload}, 0U, [&](uint8_t, std::span<float> s, uint64_t, uint64_t) { got = s.size(); });
    EXPECT_EQ(got, 300U);
}

TEST_MAIN(statusbar_avtp, avtp_am824_stream_test)
