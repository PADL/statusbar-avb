// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The stream contexts told the time as a signed Nanoseconds value: the same
// answers as the unsigned form wherever that one has an answer, and an answer
// on a timeline that starts near zero or runs below it.

#include "statusbar/avtp/avtp_aaf_stream_input.hpp"
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_aaf_v1_stream_input.hpp"
#include "statusbar/avtp/avtp_aaf_v1_stream_output.hpp"
#include "statusbar/avtp/avtp_am824_stream_input.hpp"
#include "statusbar/avtp/avtp_am824_stream_output.hpp"
#include "statusbar/avtp/avtp_crf_stream_output.hpp"
#include "statusbar/avtp/avtp_stream_common.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <type_traits>

using namespace statusbar;
using namespace statusbar::avtp;
using namespace std::chrono_literals;

namespace {

constexpr int64_t WRAP = int64_t{1} << 32;
constexpr int64_t TAI_SCALE = 1'795'000'000'000'000'000;  // a TAI-epoch count of this decade
constexpr Nanoseconds OFFSET{2'000'000};                  // a 2 ms presentation offset

auto signed_form(uint32_t const field, int64_t const now) -> int64_t
{
    return reconstruct_avtp_timestamp(field, Nanoseconds{now}).count();
}

auto unsigned_form(uint32_t const field, int64_t const now) -> int64_t
{
    return static_cast<int64_t>(reconstruct_avtp_timestamp(field, static_cast<uint64_t>(now)));
}

/// The low 32 bits of a time: what the wire carries of it.
auto field_of(int64_t const time) -> uint32_t
{
    return static_cast<uint32_t>(static_cast<uint64_t>(time));
}

auto silence(uint8_t /*channel*/, std::span<float> dest) -> void
{
    for (auto& sample : dest) {
        sample = 0.0F;
    }
}

}  // namespace

// The type is a signed 64-bit nanosecond count, on every platform.
static_assert(std::is_same_v<Nanoseconds::rep, int64_t>);
static_assert(std::is_same_v<Nanoseconds::period, std::nano>);

TEST(avtp_nanoseconds, the_signed_reconstruction_agrees_with_the_unsigned_one)
{
    constexpr std::array<int64_t, 6> NOWS{
        WRAP + 1, (3 * WRAP) - 0x100, (3 * WRAP) + 0x7FFF'FFFF, (3 * WRAP) + 0x8000'0000, TAI_SCALE, TAI_SCALE + 0x1234'5678};
    // Steps from now to the presentation time: both directions, the wrap, and half a wrap either side.
    constexpr std::array<int64_t, 11> STEPS{
        0, 1, -1, 2'000'000, -2'000'000, 0x7FFF'FFFF, -0x7FFF'FFFF, 0x8000'0000LL, -0x8000'0000LL, 0x7FFF'FFFE, 123'456'789};
    for (int64_t const now : NOWS) {
        for (int64_t const step : STEPS) {
            uint32_t const field = field_of(now + step);
            EXPECT_EQ(signed_form(field, now), unsigned_form(field, now));
        }
    }
}

TEST(avtp_nanoseconds, the_reconstruction_is_the_nearest_time_with_those_low_bits)
{
    int64_t const now = TAI_SCALE;
    EXPECT_EQ(signed_form(field_of(now + 2'000'000), now), now + 2'000'000);  // ahead, the usual case
    EXPECT_EQ(signed_form(field_of(now - 5'000'000), now), now - 5'000'000);  // already past
    // Across a wrap of the field in either direction.
    int64_t const before_wrap = (5 * WRAP) - 1000;
    EXPECT_EQ(signed_form(field_of(before_wrap + 3000), before_wrap), before_wrap + 3000);
    int64_t const after_wrap = (5 * WRAP) + 1000;
    EXPECT_EQ(signed_form(field_of(after_wrap - 3000), after_wrap), after_wrap - 3000);
}

TEST(avtp_nanoseconds, a_time_before_the_timelines_zero_is_a_small_negative_time)
{
    // One second after the timeline began, a field from 4096 ns before it.
    int64_t const now = 1'000'000'000;
    uint32_t const field = field_of(-4096);
    EXPECT_EQ(signed_form(field, now), -4096);
    // The unsigned form cannot go below zero and answers a whole wrap late.
    EXPECT_EQ(unsigned_form(field, now), WRAP - 4096);
}

TEST(avtp_nanoseconds, a_timeline_below_zero_reconstructs_like_any_other)
{
    int64_t const now = -10'000'000'000;  // ten seconds before the timeline's zero
    EXPECT_EQ(signed_form(field_of(now + 2'000'000), now), now + 2'000'000);
    EXPECT_EQ(signed_form(field_of(now - 2'000'000), now), now - 2'000'000);
    // And straddling zero itself.
    EXPECT_EQ(signed_form(field_of(1500), -500), 1500);
    EXPECT_EQ(signed_form(field_of(-1500), 500), -1500);
}

TEST(avtp_nanoseconds, an_aaf_packet_built_and_read_on_the_signed_time_keeps_its_presentation_time)
{
    for (Nanoseconds const now : {Nanoseconds{TAI_SCALE}, Nanoseconds{500'000}, Nanoseconds{-10'000'000'000}}) {
        AafStreamOutputContext talker{
            tsn::StreamId{}, AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, static_cast<uint64_t>(OFFSET.count())};
        AafStreamInputContext listener{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
        AafPdu pdu{};
        std::array<uint8_t, 6 * 2 * 3> payload{};
        EXPECT_EQ(aaf_stream_serialize(talker, pdu, std::span{payload}, 6, now, silence), payload.size());

        Nanoseconds got_pts{};
        Nanoseconds got_period{};
        aaf_stream_deserialize(
            listener,
            pdu,
            std::span<uint8_t const>{payload},
            now,
            [&](uint8_t, std::span<float>, Nanoseconds pts, Nanoseconds period) {
                got_pts = pts;
                got_period = period;
            });
        EXPECT_TRUE(got_pts == now + OFFSET);
        EXPECT_TRUE(got_period == Nanoseconds{20833});
        EXPECT_TRUE(listener.compute_pts() == now + OFFSET + (6 * listener.sample_period()));  // the next packet's first sample
        EXPECT_EQ(listener.last_gptp_time_ns, now.count());
    }
}

TEST(avtp_nanoseconds, the_unsigned_aaf_calls_are_the_signed_ones_with_the_same_bits)
{
    constexpr uint64_t NOW = 0x0000'0002'FFFF'0000ULL;
    AafStreamOutputContext talker_u{tsn::StreamId{}, AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, 2'000'000};
    AafStreamOutputContext talker_s{tsn::StreamId{}, AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, 2'000'000};
    AafPdu pdu_u{};
    AafPdu pdu_s{};
    talker_u.build_packet_header(pdu_u, 6, NOW);
    talker_s.build_packet_header(pdu_s, 6, Nanoseconds{static_cast<int64_t>(NOW)});
    EXPECT_EQ(pdu_s.get_avtp_timestamp(), pdu_u.get_avtp_timestamp());
    EXPECT_EQ(pdu_s.get_sequence_num(), pdu_u.get_sequence_num());

    AafStreamInputContext listener_u{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    AafStreamInputContext listener_s{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    listener_u.process_packet_header(pdu_u, NOW);
    listener_s.process_packet_header(pdu_s, Nanoseconds{static_cast<int64_t>(NOW)});
    EXPECT_EQ(listener_u.compute_pts_ns(), NOW + 2'000'000U);
    EXPECT_EQ(listener_s.compute_pts_ns(), listener_u.compute_pts_ns());
    EXPECT_TRUE(listener_s.compute_pts() == listener_u.compute_pts());
}

TEST(avtp_nanoseconds, an_am824_packet_built_and_read_on_the_signed_time_keeps_its_presentation_time)
{
    for (Nanoseconds const now : {Nanoseconds{TAI_SCALE}, Nanoseconds{500'000}, Nanoseconds{-10'000'000'000}}) {
        Am824StreamOutputContext talker{tsn::StreamId{}, Am824SampleRate::rate_48_khz, 2, static_cast<uint64_t>(OFFSET.count())};
        Am824StreamInputContext listener{Am824SampleRate::rate_48_khz, 2};
        Am824Pdu pdu{};
        std::array<uint8_t, 6 * 2 * 4> payload{};
        EXPECT_EQ(am824_serialize_mbla(talker, pdu, std::span{payload}, 6, now, silence), payload.size());
        EXPECT_TRUE(pdu.tv());  // the first packet starts on a SYT interval boundary

        Nanoseconds got_pts{};
        size_t got_samples = 0;
        am824_deserialize_mbla(
            listener, pdu, std::span<uint8_t const>{payload}, now, [&](uint8_t, std::span<float> s, Nanoseconds pts, Nanoseconds) {
                got_pts = pts;
                got_samples = s.size();
            });
        EXPECT_EQ(got_samples, 6U);
        EXPECT_TRUE(got_pts == now + OFFSET);
        EXPECT_TRUE(listener.compute_base_pts() == now + OFFSET);
        EXPECT_EQ(listener.last_anchor_pts_ns, (now + OFFSET).count());
    }
}

TEST(avtp_nanoseconds, the_unsigned_am824_calls_are_the_signed_ones_with_the_same_bits)
{
    constexpr uint64_t NOW = 0x0000'0002'FFFF'FF00ULL;  // the field wraps inside the presentation offset
    Am824StreamOutputContext talker_u{tsn::StreamId{}, Am824SampleRate::rate_48_khz, 2, 2'000'000};
    Am824StreamOutputContext talker_s{tsn::StreamId{}, Am824SampleRate::rate_48_khz, 2, 2'000'000};
    Am824Pdu pdu_u{};
    Am824Pdu pdu_s{};
    talker_u.build_packet_header(pdu_u, 6, NOW);
    talker_s.build_packet_header(pdu_s, 6, Nanoseconds{static_cast<int64_t>(NOW)});
    EXPECT_EQ(pdu_s.avtp_timestamp(), pdu_u.avtp_timestamp());
    EXPECT_EQ(pdu_s.syt_timestamp(), pdu_u.syt_timestamp());

    Am824StreamInputContext listener_u{Am824SampleRate::rate_48_khz, 2};
    Am824StreamInputContext listener_s{Am824SampleRate::rate_48_khz, 2};
    listener_u.process_packet_header(pdu_u, 6, NOW);
    listener_s.process_packet_header(pdu_s, 6, Nanoseconds{static_cast<int64_t>(NOW)});
    EXPECT_EQ(listener_u.compute_base_pts_ns(), NOW + 2'000'000U);
    EXPECT_EQ(listener_s.compute_base_pts_ns(), listener_u.compute_base_pts_ns());
}

TEST(avtp_nanoseconds, the_mixed_am824_decoder_hands_each_callback_signed_times)
{
    Nanoseconds const now{-3'000'000'000};
    Am824StreamOutputContext talker{tsn::StreamId{}, Am824SampleRate::rate_48_khz, 2, static_cast<uint64_t>(OFFSET.count())};
    Am824StreamInputContext listener{Am824SampleRate::rate_48_khz, 2};
    Am824Pdu pdu{};
    std::array<uint8_t, 6 * 2 * 4> payload{};
    EXPECT_EQ(am824_serialize_mbla(talker, pdu, std::span{payload}, 6, now, silence), payload.size());

    int audio_calls = 0;
    Nanoseconds got_pts{};
    am824_deserialize_mixed(
        listener,
        pdu,
        std::span<uint8_t const>{payload},
        now,
        [&](uint8_t, std::span<float>, Nanoseconds pts, Nanoseconds period) {
            ++audio_calls;
            got_pts = pts;
            EXPECT_TRUE(period == Nanoseconds{20833});
        },
        [](uint8_t, std::span<uint8_t const>, Nanoseconds, Nanoseconds) {},
        [](uint8_t, uint8_t, std::span<uint8_t const, 3>, Nanoseconds, Nanoseconds) {});
    EXPECT_EQ(audio_calls, 2);
    EXPECT_TRUE(got_pts == now + OFFSET);
}

TEST(avtp_nanoseconds, an_aaf_v1_packet_built_and_read_on_the_signed_time_keeps_its_presentation_time)
{
    Nanoseconds const now{TAI_SCALE};
    AafV1StreamOutputContext talker{
        tsn::StreamId{}, AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, static_cast<uint64_t>(OFFSET.count())};
    AafV1StreamInputContext listener{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    AafV1Pdu pdu{};
    std::array<uint8_t, 6 * 2 * 3> payload{};
    EXPECT_EQ(aaf_v1_stream_serialize(talker, pdu, std::span{payload}, 6, now, silence), payload.size());
    EXPECT_EQ(pdu.get_avtp_timestamp(), static_cast<uint64_t>((now + OFFSET).count()));  // all 64 bits on the wire

    Nanoseconds got_pts{};
    int calls = 0;
    aaf_v1_stream_deserialize(
        listener,
        pdu,
        std::span<uint8_t const>{payload},
        now,
        [&](uint8_t, std::span<float> s, Nanoseconds pts, Nanoseconds period) {
            ++calls;
            got_pts = pts;
            EXPECT_EQ(s.size(), 6U);
            EXPECT_TRUE(period == Nanoseconds{20833});
        });
    EXPECT_EQ(calls, 2);
    EXPECT_TRUE(got_pts == now + OFFSET);
    EXPECT_EQ(listener.sequence_gaps, 0U);
    EXPECT_EQ(listener.packets_received, 1U);
}

TEST(avtp_nanoseconds, a_crf_packet_built_on_the_signed_time_carries_the_same_timestamps)
{
    constexpr uint64_t NOW = 1'795'000'000'000'123'456ULL;
    CrfStreamOutputContext talker_u{tsn::StreamId{}, CrfType::audio_sample, 48000, CrfPull::multiply_1_0, 160, 6};
    CrfStreamOutputContext talker_s{tsn::StreamId{}, CrfType::audio_sample, 48000, CrfPull::multiply_1_0, 160, 6};
    CrfPdu pdu_u{};
    CrfPdu pdu_s{};
    std::array<uint8_t, 6 * CrfPdu::TIMESTAMP_SIZE> data_u{};
    std::array<uint8_t, 6 * CrfPdu::TIMESTAMP_SIZE> data_s{};
    EXPECT_EQ(talker_u.build_packet(pdu_u, std::span{data_u}, NOW), data_u.size());
    EXPECT_EQ(talker_s.build_packet(pdu_s, std::span{data_s}, Nanoseconds{static_cast<int64_t>(NOW)}), data_s.size());
    EXPECT_TRUE(data_s == data_u);
    EXPECT_EQ(talker_s.next_timestamp_ns, talker_u.next_timestamp_ns);
}

TEST_MAIN(statusbar_avtp, avtp_stream_nanoseconds_test)
