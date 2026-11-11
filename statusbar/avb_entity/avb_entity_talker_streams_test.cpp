// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Pure (socket-free) tests for the CRF timestamp math extracted from
// TalkerStreams::transmit_crf. The regression under test (entity#4): CRF
// timestamps must track the LIVE media-clock position, not the media-clock
// anchor, so a stream whose gate opens seconds after start still conveys
// gPTP-now. transmit_crf() itself early-returns without a socket, but the
// timestamp math is now a pure function driven by a software MediaClockGenerator.

#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"

#include "statusbar/avtp/avtp_crf.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory_resource>

using namespace statusbar;
using statusbar::avb_entity::crf_aligned_base;
using statusbar::avb_entity::crf_sample_stride;
using statusbar::avb_entity::crf_schedule;
using statusbar::avb_entity::fill_crf_timestamps;

namespace {

// Milan-style CRF: 48 kHz base, 96 kHz audio -> each declared event spans 2
// audio samples; interval 96 -> stride 192 samples.
constexpr uint16_t kInterval = 96;
constexpr uint32_t kCrfBase = 48'000;
constexpr uint32_t kAudioRate = 96'000;  // 96 kHz audio

// Advance a fresh media clock by `ticks` packets of `nominal` samples at r=1.0
// and return the resulting live first_index. Anchors on the first advance.
auto advance_media_clock(ptpclient::MediaClockGenerator& mc, uint64_t t0_ns, uint32_t nominal, int ticks) -> uint64_t
{
    uint64_t const tick_ns = (static_cast<uint64_t>(nominal) * 1'000'000'000ULL) / kAudioRate;
    uint64_t first_index = 0;
    for (int k = 0; k < ticks; ++k) {
        auto const emit = mc.advance(t0_ns + (static_cast<uint64_t>(k) * tick_ns), 1.0, nominal);
        first_index = emit.first_index;
    }
    return first_index;
}

}  // namespace

TEST(crf_math, sample_stride)
{
    EXPECT_EQ(crf_sample_stride(kInterval, kCrfBase, kAudioRate), 192U);  // 96 * 96000 / 48000
    EXPECT_EQ(crf_sample_stride(1, 96'000, kAudioRate), 1U);
    EXPECT_EQ(crf_sample_stride(96, 0, kAudioRate), 0U);              // degenerate: base 0 -> 0, no div-by-zero
    EXPECT_EQ(crf_sample_stride(kInterval, kCrfBase, 48'000U), 96U);  // 48 kHz audio: 96 * 48000 / 48000
}

TEST(crf_math, aligned_base_floors_to_stride)
{
    EXPECT_EQ(crf_aligned_base(1000, 192), 960U);  // floor(1000/192)*192 = 5*192
    EXPECT_EQ(crf_aligned_base(960, 192), 960U);   // already aligned
    EXPECT_EQ(crf_aligned_base(1234, 0), 1234U);   // stride 0 -> passthrough
    // Successive PDUs advance first_index by a whole multiple of stride, so the
    // aligned base advances by exactly that multiple (contiguous, no gap).
    EXPECT_EQ(crf_aligned_base(960 + 192, 192) - crf_aligned_base(960, 192), 192U);
}

TEST(crf_math, fill_matches_media_clock_timestamps)
{
    ptpclient::MediaClockGenerator mc{};
    uint64_t const base = advance_media_clock(mc, /*t0_ns=*/1'000'000'000ULL, /*nominal=*/12, /*ticks=*/100);
    uint32_t const stride = crf_sample_stride(kInterval, kCrfBase, kAudioRate);
    uint64_t const aligned = crf_aligned_base(base, stride);

    constexpr uint16_t n_ts = 6;
    std::array<uint8_t, static_cast<size_t>(n_ts) * avtp::CrfPdu::TIMESTAMP_SIZE> ts_data{};
    fill_crf_timestamps(mc, aligned, stride, n_ts, ts_data);

    for (uint16_t i = 0; i < n_ts; ++i) {
        auto const got = avtp::crf_get_timestamp(ts_data, i);
        EXPECT_TRUE(got.has_value());
        // Each timestamp is exactly the media clock's presentation time for that
        // sample index -- evenly spaced by `stride` samples.
        EXPECT_EQ(*got, mc.timestamp_for(aligned + (static_cast<uint64_t>(i) * stride)));
    }
}

// The regression itself: after the clock has been running ~1 s, a CRF PDU based
// on the LIVE position carries a timestamp ~1 s ahead of one based on the anchor
// (index 0). Before the fix, transmit_crf always used an anchor-relative counter
// starting at 0, so a gate that opened late emitted permanently-stale timestamps.
TEST(crf_math, live_base_tracks_now_not_anchor)
{
    ptpclient::MediaClockGenerator mc{};
    // 8000 ticks * 12 samples / 96 kHz = 1.0 s of media time elapsed.
    uint64_t const base = advance_media_clock(mc, /*t0_ns=*/1'000'000'000ULL, /*nominal=*/12, /*ticks=*/8000);
    uint32_t const stride = crf_sample_stride(kInterval, kCrfBase, kAudioRate);

    uint64_t const ts_live = mc.timestamp_for(crf_aligned_base(base, stride));
    uint64_t const ts_anchor = mc.timestamp_for(0);  // the old, stale behavior

    EXPECT_TRUE(ts_live > ts_anchor);
    uint64_t const delta = ts_live - ts_anchor;
    // ~1 s elapsed; allow a few ms of slack for pacing quantization.
    EXPECT_TRUE(delta > 995'000'000ULL);
    EXPECT_TRUE(delta < 1'005'000'000ULL);
}

// --- CRF PDUs are owed by sample index, not by wake count ----------------------

// A Milan-style CRF at 44.1 kHz: base 44100, interval 96, one timestamp per
// PDU, so a PDU every 96 media-clock samples = 459.375 PDU/s. The media clock
// is paced at 8000 wakes/s and hands out 5 or 6 samples a wake (44100/8000 =
// 5.5125 on average); the old rule, one PDU every floor(96/5) = 19 wakes, gave
// 421 PDU/s. Counting samples gives the exact rate whatever the wake carries.
TEST(crf_schedule, pdus_are_spaced_by_samples_whatever_the_wake_carries)
{
    constexpr uint32_t stride = 96;  // crf_sample_stride(96, 44100, 44100)
    constexpr uint64_t span = 96;    // one timestamp per PDU
    std::optional<uint64_t> due{};
    std::optional<uint64_t> last_sent{};
    uint64_t live = 0;
    int sent = 0;
    for (uint64_t k = 0; k < 80'000; ++k) {  // 10 s of wakes
        auto const samples = static_cast<uint32_t>((((k + 1) * 44'100ULL) / 8000ULL) - ((k * 44'100ULL) / 8000ULL));
        auto const plan = crf_schedule(due, live, samples, stride, span);
        due = plan.due;
        if (plan.send) {
            if (last_sent) {
                EXPECT_EQ(*plan.send - *last_sent, span);  // contiguous, never a gap or a repeat
            }
            last_sent = plan.send;
            ++sent;
        }
        live += samples;
    }
    EXPECT_EQ(live, 441'000ULL);
    EXPECT_EQ(sent, 4594);  // the PDUs at 0, 96, ..., 440928: 441000 / 96 = 4593.75, so 4594
}

// Nothing scheduled starts at the stride-aligned live position, a wake that
// does not reach the owed index sends nothing and keeps it, the wake that
// covers it sends it, and a media clock that stepped by more than a span in
// either direction starts again from the live position.
TEST(crf_schedule, a_closed_gate_or_a_clock_step_restarts_from_the_aligned_live_position)
{
    auto const first = crf_schedule(std::nullopt, 1000, 6, 96, 96);
    EXPECT_TRUE(first.send.has_value());
    EXPECT_EQ(*first.send, 960U);
    EXPECT_EQ(*first.due, 1056U);

    auto const waiting = crf_schedule(first.due, 1006, 6, 96, 96);  // covers 1006..1011: not yet
    EXPECT_FALSE(waiting.send.has_value());
    EXPECT_EQ(*waiting.due, 1056U);

    auto const hit = crf_schedule(waiting.due, 1051, 6, 96, 96);  // covers 1051..1056
    EXPECT_EQ(*hit.send, 1056U);
    EXPECT_EQ(*hit.due, 1152U);

    auto const forward = crf_schedule(hit.due, 50'000, 6, 96, 96);  // the clock stepped ahead
    EXPECT_EQ(*forward.send, crf_aligned_base(50'000, 96));
    EXPECT_EQ(*forward.due, crf_aligned_base(50'000, 96) + 96);

    auto const back = crf_schedule(hit.due, 100, 6, 96, 96);  // and back
    EXPECT_EQ(*back.send, 96U);

    auto const degenerate = crf_schedule(std::nullopt, 1000, 6, 0, 0);  // stride 0: never owed
    EXPECT_FALSE(degenerate.send.has_value());
}

// --- An AAF slot is blocked by its format, not by the media clock rate --------

TEST(talker_streams, an_aaf_slot_carries_its_format_s_frames_per_pdu)
{
    ptpclient::MediaClockGenerator const mc{};
    std::atomic<int64_t> const gptp_now{0};
    statusbar::avb_entity::TalkerStreams talker{
        statusbar::avb_entity::TalkerStreamsConfig{.sample_rate = 44'100}, mc, gptp_now, std::pmr::get_default_resource()};
    auto open = [&talker](uint16_t index, uint64_t word) -> uint16_t {
        statusbar::avb_entity::StreamSpec spec{};
        spec.index = index;
        spec.format_word = word;
        spec.format = statusbar::avb_entity::decode_stream_format(word);
        EXPECT_TRUE(talker.open_stream(spec, {}, {}).has_value());
        return talker.slot_for(index)->aaf_samples;
    };
    EXPECT_EQ(open(0, 0x0204022002000000ULL), 6U);   // 44.1k, unspecified: Table 18, not 44100/8000 = 5
    EXPECT_EQ(open(1, 0x0204022002006000ULL), 6U);   // 44.1k, 6 named
    EXPECT_EQ(open(2, 0x0205022002005000ULL), 5U);   // 48k, 5 named: the word wins
    EXPECT_EQ(open(3, 0x020702200200C000ULL), 12U);  // 96k: unchanged
}

// --- The TX kick hand-off, with no port behind it -----------------------------

TEST(talker_streams, external_kick_needs_a_port)
{
    ptpclient::MediaClockGenerator const mc{};
    std::atomic<int64_t> const gptp_now{0};
    statusbar::avb_entity::TalkerStreams talker{
        statusbar::avb_entity::TalkerStreamsConfig{.sample_rate = 96000}, mc, gptp_now, std::pmr::get_default_resource()};
    // No open_tx: nothing to kick, so the request is refused and the state
    // stays "the tick kicks" -- which is a no-op too, with no port.
    EXPECT_FALSE(talker.set_external_kick(true));
    EXPECT_FALSE(talker.external_kick());
    EXPECT_TRUE(talker.set_external_kick(false));
    talker.flush_tx();  // no port: nothing happens, nothing crashes
    talker.kick_tx();
}

TEST(talker_streams, close_tx_lets_a_restart_open_the_streams_afresh)
{
    ptpclient::MediaClockGenerator const mc{};
    std::atomic<int64_t> const gptp_now{0};
    statusbar::avb_entity::TalkerStreams talker{
        statusbar::avb_entity::TalkerStreamsConfig{.sample_rate = 96000}, mc, gptp_now, std::pmr::get_default_resource()};
    statusbar::avb_entity::StreamSpec spec{};
    spec.index = 3;
    spec.format_word = 0x020702200200C000ULL;  // AAF 96k int32 8ch
    spec.format = statusbar::avb_entity::decode_stream_format(spec.format_word);

    // A run: one slot, stopped by a controller.
    EXPECT_TRUE(talker.open_stream(spec, {}, {}).has_value());
    talker.set_stream_stopped(3, true);
    EXPECT_TRUE(talker.stream_stopped(3));

    // Its end, then the next run opening the same stream: still one slot,
    // and the stream streams again - a restart is a first start.
    talker.close_tx();
    EXPECT_EQ(talker.slot_for(3), nullptr);
    EXPECT_FALSE(talker.stream_stopped(3));
    EXPECT_FALSE(talker.external_kick());
    EXPECT_TRUE(talker.open_stream(spec, {}, {}).has_value());
    EXPECT_NE(talker.slot_for(3), nullptr);
    EXPECT_EQ(talker.slot_of(statusbar::avb_entity::StreamKind::aaf), talker.slot_for(3));
}

TEST_MAIN(statusbar_avb_entity, avb_entity_talker_streams_test)
