// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Pure (socket-free) tests for the CRF timestamp math extracted from
// TalkerStreams::transmit_crf. The regression under test (entity#4): CRF
// timestamps must track the LIVE media-clock position, not the media-clock
// anchor, so a stream whose gate opens seconds after start still conveys
// gPTP-now. transmit_crf() itself early-returns without a socket, but the
// timestamp math is now a pure function driven by a software MediaClockGenerator.

#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"

#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_crf.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

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

// --- What a stream output sent: Milan v1.3 Table 5.4, IEEE 1722.1-2021 Table 7-158 ---

TEST(talker_tx_counters, what_is_sent_counts_avtpdus_tu_and_mr_toggles)
{
    ptpclient::MediaClockGenerator const mc{};
    std::atomic<int64_t> const gptp_now{0};
    avb_entity::TalkerStreams talker{avb_entity::TalkerStreamsConfig{.sample_rate = 48'000}, mc, gptp_now, nullptr};
    avb_entity::TalkerStreamSlot slot{};
    slot.spec.index = 3;
    talker.count_sent(slot, /*tu=*/false, /*mr=*/false);
    talker.count_sent(slot, /*tu=*/true, /*mr=*/false);
    talker.count_sent(slot, /*tu=*/true, /*mr=*/true);  // mr toggled
    talker.count_sent(slot, /*tu=*/true, /*mr=*/true);
    auto const* const c = talker.tx_counters(3);
    EXPECT_TRUE(c != nullptr);
    EXPECT_EQ(c->pdus.load(), 4U);
    EXPECT_EQ(c->tu_pdus.load(), 3U);
    EXPECT_EQ(c->mr_toggles.load(), 1U);
    EXPECT_EQ(slot.tx_packets, 4U);
    EXPECT_EQ(talker.tx_counters(4)->pdus.load(), 0U);  // each stream its own
    EXPECT_TRUE(talker.tx_counters(static_cast<uint16_t>(avb_entity::MAX_ENTITY_STREAMS)) == nullptr);

    // A stream opened again after close_tx(): its first mr is compared with nothing.
    talker.close_tx();
    talker.count_sent(slot, /*tu=*/false, /*mr=*/false);
    EXPECT_EQ(c->mr_toggles.load(), 1U);
    EXPECT_EQ(c->pdus.load(), 5U);
}

TEST(talker_tx_counters, a_stream_starts_when_its_gate_opens_and_stops_when_it_closes)
{
    ptpclient::MediaClockGenerator const mc{};
    std::atomic<int64_t> const gptp_now{0};
    avb_entity::TalkerStreams talker{avb_entity::TalkerStreamsConfig{.sample_rate = 48'000}, mc, gptp_now, nullptr};
    avb_entity::TalkerStreamSlot slot{};
    slot.spec.index = 2;
    auto const& changes = talker.tx_counters(2)->streaming_changes;
    auto tick = [&](bool const gate_open) { talker.transmit_if_due(slot, {}, gate_open, 0, {}); };
    auto starts_stops = [&] {
        return std::array{avb_entity::stream_starts(changes.load()), avb_entity::stream_stops(changes.load())};
    };

    tick(false);
    EXPECT_EQ(starts_stops(), (std::array{0U, 0U}));
    tick(true);
    tick(true);  // streaming on: one start
    EXPECT_EQ(starts_stops(), (std::array{1U, 0U}));
    tick(false);
    EXPECT_EQ(starts_stops(), (std::array{1U, 1U}));
    tick(true);
    talker.set_stream_stopped(2, true);  // STOP_STREAMING is a closed gate
    tick(true);
    EXPECT_EQ(starts_stops(), (std::array{2U, 2U}));
    talker.set_stream_stopped(2, false);
    tick(true);
    EXPECT_EQ(starts_stops(), (std::array{3U, 2U}));
    // The streams end with the run: a streaming one counts its stop.
    talker.close_tx();
    EXPECT_EQ(starts_stops(), (std::array{3U, 3U}));
}

// --- AVTP mr: IEEE 1722-2016 Clauses 4.4.4.3 and 10.4.3 ---

namespace {

/// An AAF STREAM_OUTPUT @p index opened on @p talker, with no port behind it.
auto open_aaf_output(avb_entity::TalkerStreams& talker, uint16_t const index) -> avb_entity::TalkerStreamSlot&
{
    avb_entity::StreamSpec spec{};
    spec.index = index;
    spec.format_word = 0x0205022002006000ULL;  // AAF 48k int32 2ch, 6 frames
    spec.format = avb_entity::decode_stream_format(spec.format_word);
    EXPECT_TRUE(talker.open_stream(spec, {}, {}).has_value());
    return *talker.slot_for(index);
}

/// The mr of @p count AVTPDUs of @p slot, each stored and counted as transmit_* does.
auto send_mr(avb_entity::TalkerStreams& talker, avb_entity::TalkerStreamSlot& slot, int const count) -> std::vector<bool>
{
    std::vector<bool> sent;
    for (int i = 0; i < count; ++i) {
        sent.push_back(talker.next_mr(slot));
        talker.count_sent(slot, false, sent.back());
    }
    return sent;
}

}  // namespace

TEST(talker_mr, a_restart_toggles_mr_on_the_next_avtpdu_and_holds_it_for_eight)
{
    ptpclient::MediaClockGenerator const mc{};
    std::atomic<int64_t> const gptp_now{0};
    avb_entity::TalkerStreams talker{
        avb_entity::TalkerStreamsConfig{.sample_rate = 48'000}, mc, gptp_now, std::pmr::get_default_resource()};
    auto& slot = open_aaf_output(talker, 3);
    EXPECT_TRUE(send_mr(talker, slot, 2) == std::vector<bool>(2, false));

    // A second restart inside the hold toggles mr back on the first AVTPDU after it, not sooner.
    EXPECT_TRUE(talker.toggle_media_clock_restart(3).has_value());
    EXPECT_TRUE(send_mr(talker, slot, 1) == std::vector<bool>{true});
    EXPECT_TRUE(talker.toggle_media_clock_restart(3).has_value());
    EXPECT_TRUE(send_mr(talker, slot, 7) == std::vector<bool>(7, true));
    EXPECT_TRUE(send_mr(talker, slot, 9) == std::vector<bool>(9, false));

    // Restarts asked before the AVTPDU that carries them toggle mr once.
    EXPECT_TRUE(talker.toggle_media_clock_restart(3).has_value());
    EXPECT_TRUE(talker.toggle_media_clock_restart(3).has_value());
    EXPECT_TRUE(send_mr(talker, slot, 10) == std::vector<bool>(10, true));
    EXPECT_EQ(talker.tx_counters(3)->mr_toggles.load(), 3U);  // MEDIA_RESET
}

TEST(talker_mr, a_stream_not_open_refuses_a_restart_and_a_reopened_one_starts_clear)
{
    ptpclient::MediaClockGenerator const mc{};
    std::atomic<int64_t> const gptp_now{0};
    avb_entity::TalkerStreams talker{
        avb_entity::TalkerStreamsConfig{.sample_rate = 48'000}, mc, gptp_now, std::pmr::get_default_resource()};
    EXPECT_FALSE(talker.toggle_media_clock_restart(3).has_value());
    EXPECT_FALSE(talker.toggle_media_clock_restart(static_cast<uint16_t>(avb_entity::MAX_ENTITY_STREAMS)).has_value());

    // A restart asked of a run that ended before its AVTPDU went is not the next run's.
    (void)open_aaf_output(talker, 3);
    EXPECT_TRUE(talker.toggle_media_clock_restart(3).has_value());
    talker.close_tx();
    auto& slot = open_aaf_output(talker, 3);
    EXPECT_TRUE(send_mr(talker, slot, 3) == std::vector<bool>(3, false));
    EXPECT_EQ(talker.tx_counters(3)->mr_toggles.load(), 0U);
}

TEST(talker_mr, a_media_clock_re_anchored_on_a_stepped_gptp_timeline_toggles_mr)
{
    // The timestamps jump with the gPTP time the media clock re-anchors on: a restart of it.
    ptpclient::MediaClockGenerator mc{};
    std::atomic<int64_t> const gptp_now{0};
    avb_entity::TalkerStreams talker{
        avb_entity::TalkerStreamsConfig{.sample_rate = 48'000}, mc, gptp_now, std::pmr::get_default_resource()};
    (void)mc.advance(1'000'000'000, 1.0, 6);
    auto& slot = open_aaf_output(talker, 2);
    EXPECT_TRUE(send_mr(talker, slot, 2) == std::vector<bool>(2, false));
    (void)mc.advance(1'000'125'000, 1.0, 6);  // on time: no step
    EXPECT_TRUE(send_mr(talker, slot, 2) == std::vector<bool>(2, false));

    (void)mc.advance(9'000'000'000, 1.0, 6);  // gPTP stepped seconds ahead
    EXPECT_EQ(mc.step_count(), 1U);
    EXPECT_TRUE(send_mr(talker, slot, 8) == std::vector<bool>(8, true));
    (void)mc.advance(1'000'000'000, 1.0, 6);  // and back, the hold over: the next AVTPDU
    EXPECT_TRUE(send_mr(talker, slot, 1) == std::vector<bool>{false});
    EXPECT_EQ(talker.tx_counters(2)->mr_toggles.load(), 2U);
}

// A CRF stream format names up to 255 timestamps per AVTPDU; the talker's frame holds
// MAX_CRF_TIMESTAMPS_PER_PDU. A format that asks for more is refused when the stream is opened,
// since its AVTPDU would be sent from beyond the frame.
TEST(talker_streams, a_crf_stream_with_more_timestamps_than_the_frame_holds_is_refused)
{
    constexpr uint64_t CRF_48K_NO_COUNT = 0x041060010000BB80ULL & ~(0xFFULL << 32);
    auto const open_with = [](uint64_t const timestamps) {
        ptpclient::MediaClockGenerator const mc{};
        std::atomic<int64_t> const gptp_now{0};
        avb_entity::TalkerStreams talker{avb_entity::TalkerStreamsConfig{.sample_rate = 48'000}, mc, gptp_now, nullptr};
        avb_entity::StreamSpec spec{};
        spec.index = 2;
        spec.format_word = CRF_48K_NO_COUNT | (timestamps << 32);
        spec.format = avb_entity::decode_stream_format(spec.format_word);
        EXPECT_TRUE(spec.format.kind == avb_entity::StreamKind::crf);
        EXPECT_EQ(uint64_t{spec.format.crf_timestamps_per_pdu}, timestamps);
        return talker.open_stream(spec, {}, {}).has_value();
    };
    constexpr uint64_t most = avb_entity::TalkerStreams::MAX_CRF_TIMESTAMPS_PER_PDU;
    EXPECT_TRUE(open_with(1));
    EXPECT_TRUE(open_with(most));
    EXPECT_FALSE(open_with(most + 1));
    EXPECT_FALSE(open_with(255));
    EXPECT_FALSE(open_with(0));
}

// --- AVTP tu: IEEE 1722-2016 Clause 4.4.4.7, Milan v1.3 Section 4.3.5.2 and Annex B.1.1 ---

TEST(talker_tu, set_for_a_quarter_second_after_a_grandmaster_change_and_while_none_is_in_force)
{
    using Raw = avb_entity::TalkerStreams::RawTimePoint;
    using std::chrono::nanoseconds;
    using std::chrono::seconds;
    constexpr nanoseconds WINDOW{avb_entity::TalkerStreams::TU_AFTER_GM_CHANGE_NS};
    ptpclient::MediaClockGenerator const mc{};
    std::atomic<int64_t> const gptp_now{0};
    avb_entity::TalkerStreams talker{avb_entity::TalkerStreamsConfig{.sample_rate = 48'000}, mc, gptp_now, nullptr};
    EXPECT_FALSE(talker.timestamp_uncertain(Raw{seconds{1}}));  // nothing told: certain

    talker.notify_grandmaster_changed(Raw{seconds{10}});
    EXPECT_TRUE(talker.timestamp_uncertain(Raw{seconds{10}}));
    EXPECT_TRUE(talker.timestamp_uncertain(Raw{seconds{10}} + WINDOW - nanoseconds{1}));
    EXPECT_FALSE(talker.timestamp_uncertain(Raw{seconds{10}} + WINDOW));

    talker.notify_grandmaster_lost();
    EXPECT_TRUE(talker.timestamp_uncertain(Raw{seconds{100}}));  // until one is in force, however long
    talker.notify_grandmaster_changed(Raw{seconds{200}});
    EXPECT_TRUE(talker.timestamp_uncertain(Raw{seconds{200}} + nanoseconds{1}));
    EXPECT_FALSE(talker.timestamp_uncertain(Raw{seconds{200}} + WINDOW));
}

// The calls that take nanoseconds are as they were: the same window, on whatever clock the caller
// times tu on, with 0 for an instant never told.
TEST(talker_tu, the_calls_that_take_nanoseconds_time_the_same_window)
{
    constexpr int64_t SECOND_NS = 1'000'000'000;
    constexpr int64_t WINDOW_NS = avb_entity::TalkerStreams::TU_AFTER_GM_CHANGE_NS;
    ptpclient::MediaClockGenerator const mc{};
    std::atomic<int64_t> const gptp_now{0};
    avb_entity::TalkerStreams talker{avb_entity::TalkerStreamsConfig{.sample_rate = 48'000}, mc, gptp_now, nullptr};
    EXPECT_FALSE(talker.timestamp_uncertain(0));  // nothing told: certain

    talker.notify_grandmaster_changed(10 * SECOND_NS);
    EXPECT_TRUE(talker.timestamp_uncertain(10 * SECOND_NS));
    EXPECT_TRUE(talker.timestamp_uncertain((10 * SECOND_NS) + WINDOW_NS - 1));
    EXPECT_FALSE(talker.timestamp_uncertain((10 * SECOND_NS) + WINDOW_NS));
    EXPECT_TRUE(talker.timestamp_uncertain(avb_entity::TalkerStreams::RawTimePoint{std::chrono::seconds{10}}));  // one state
    EXPECT_FALSE(talker.timestamp_uncertain(0));  // an instant never told is in no window

    talker.notify_grandmaster_lost();
    EXPECT_TRUE(talker.timestamp_uncertain(100 * SECOND_NS));
    EXPECT_TRUE(talker.timestamp_uncertain(0));
    talker.notify_grandmaster_changed(int64_t{0});  // stamped by a caller told no time: no window
    EXPECT_FALSE(talker.timestamp_uncertain(int64_t{1}));
}

// The talker reads no clock: the AVTPDUs of a media wake take their tu at the instant the wake was
// told, and an instant never told is in no window.
TEST(talker_tu, an_avtpdu_takes_its_tu_at_the_wake_the_talker_was_told)
{
    using Raw = avb_entity::TalkerStreams::RawTimePoint;
    using std::chrono::nanoseconds;
    using std::chrono::seconds;
    constexpr nanoseconds WINDOW{avb_entity::TalkerStreams::TU_AFTER_GM_CHANGE_NS};
    ptpclient::MediaClockGenerator const mc{};
    std::atomic<int64_t> const gptp_now{0};
    avb_entity::TalkerStreams talker{avb_entity::TalkerStreamsConfig{.sample_rate = 48'000}, mc, gptp_now, nullptr};
    EXPECT_FALSE(talker.wake_timestamp_uncertain());  // told nothing at all

    talker.notify_grandmaster_changed(Raw{seconds{50}});
    EXPECT_FALSE(talker.wake_timestamp_uncertain());  // a window, and no wake told: not in it
    talker.tell_wake_time(Raw{seconds{50}} + nanoseconds{125'000});
    EXPECT_TRUE(talker.wake_timestamp_uncertain());
    talker.tell_wake_time(Raw{seconds{50}} + WINDOW);
    EXPECT_FALSE(talker.wake_timestamp_uncertain());

    talker.notify_grandmaster_lost();
    EXPECT_TRUE(talker.wake_timestamp_uncertain());  // no grandmaster: tu whatever the wake
    talker.tell_wake_time(Raw{});
    EXPECT_TRUE(talker.wake_timestamp_uncertain());

    // A grandmaster change stamped by a reactor that was told no time opens no window.
    talker.notify_grandmaster_changed(Raw{});
    talker.tell_wake_time(Raw{nanoseconds{1}});
    EXPECT_FALSE(talker.wake_timestamp_uncertain());
}

TEST(talker_tx_counters, signal_presence_is_unreported_until_set_and_kept_to_sixty_channels)
{
    // Milan v1.3 Section 5.3.7.7.1: channels 0-59 in two counters; nothing until the entity reports.
    ptpclient::MediaClockGenerator const mc{};
    std::atomic<int64_t> const gptp_now{0};
    avb_entity::TalkerStreams talker{avb_entity::TalkerStreamsConfig{.sample_rate = 48'000}, mc, gptp_now, nullptr};
    EXPECT_FALSE(talker.signal_presence(3).has_value());
    EXPECT_TRUE(talker.set_signal_presence(3, 0).has_value());
    EXPECT_EQ(talker.signal_presence(3), std::optional<uint64_t>{0});  // reported: no signal
    EXPECT_TRUE(talker.set_signal_presence(3, ~uint64_t{0}).has_value());
    EXPECT_EQ(talker.signal_presence(3), std::optional<uint64_t>{(uint64_t{1} << 60U) - 1U});
    EXPECT_FALSE(talker.signal_presence(4).has_value());  // each stream its own
    auto const past = static_cast<uint16_t>(avb_entity::MAX_ENTITY_STREAMS);
    EXPECT_FALSE(talker.set_signal_presence(past, 1).has_value());
    EXPECT_FALSE(talker.signal_presence(past).has_value());
}

// --- Stream frame templates: IEEE 1722-2016 Clauses 4.4.3, 7.2 and 10.4 ---

namespace {

/// A small deterministic generator, so a failing case can be found again.
struct Lcg
{
    uint64_t state{0x2545F4914F6CDD1DULL};
    auto next(uint32_t const bound) -> uint32_t
    {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        return static_cast<uint32_t>((state >> 33U) % bound);
    }
};

auto eui48_for(Lcg& rng) -> ieee::Eui48
{
    ieee::Eui48 mac{};
    for (auto& octet : mac.value) {
        octet = static_cast<uint8_t>(rng.next(256));
    }
    return mac;
}

/// Where a frame goes and how it is tagged.
struct Addressing
{
    ieee::Eui48 dest{};
    ieee::Eui48 src{};
    uint16_t vid{2};
    uint8_t pcp{3};

    [[nodiscard]] auto header() const -> ieee::EthernetFrame { return avb_entity::stream_frame_header(dest, src, vid, pcp); }
};

auto addressing_for(Lcg& rng) -> Addressing
{
    return {
        .dest = eui48_for(rng),
        .src = eui48_for(rng),
        .vid = static_cast<uint16_t>(1 + rng.next(4094)),
        .pcp = static_cast<uint8_t>(rng.next(8))};
}

/// The frame headers transmit_aaf() sends: aaf_stream_serialize()'s header behind build_stream_frame().
auto aaf_frame_headers(
    avtp::AafStreamOutputContext ctx,
    uint16_t const frames,
    uint32_t const timestamp,
    bool const mr,
    bool const tu,
    Addressing const& eth) -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload(static_cast<size_t>(frames) * ctx.channel_count * avtp::aaf_bytes_per_sample(ctx.format));
    avtp::AafPdu pdu{};
    auto const silence = [](uint8_t, std::span<float> dest) { std::ranges::fill(dest, 0.0F); };
    EXPECT_EQ(avtp::aaf_stream_serialize(ctx, pdu, payload, frames, uint64_t{timestamp}, silence), payload.size());
    pdu.set_tu(tu);
    pdu.set_mr(mr);
    std::array<uint8_t, avtp::AafPdu::HEADER_LENGTH> header{};
    span_store(std::span<uint8_t>{header}, pdu);
    std::vector<uint8_t> frame(64);
    auto const length = avb_entity::build_stream_frame(frame, eth.dest, eth.src, eth.vid, eth.pcp, header);
    frame.resize(length);
    return frame;
}

/// The frame headers transmit_crf() sends for @p ctx's next AVTPDU.
auto crf_frame_headers(avtp::CrfStreamOutputContext const& ctx, bool const mr, bool const tu, Addressing const& eth)
    -> std::vector<uint8_t>
{
    avtp::CrfPdu pdu{};
    pdu.init_audio_sample(ctx.stream_id, ctx.base_frequency, ctx.pull, ctx.timestamp_interval, ctx.timestamps_per_packet);
    pdu.set_sequence_num(ctx.sequence_num);
    pdu.set_tu(tu);
    pdu.set_mr(mr);
    std::array<uint8_t, avtp::CrfPdu::HEADER_LENGTH> header{};
    span_store(std::span<uint8_t>{header}, pdu);
    std::vector<uint8_t> frame(64);
    auto const length = avb_entity::build_stream_frame(frame, eth.dest, eth.src, eth.vid, eth.pcp, header);
    frame.resize(length);
    return frame;
}

/// @p tmpl written for one frame, as many octets as its header.
auto written_for(
    avb_entity::StreamFrameTemplate const& tmpl, uint8_t const sequence, uint32_t const timestamp, bool const mr, bool const tu)
    -> std::vector<uint8_t>
{
    std::vector<uint8_t> out(tmpl.length);
    EXPECT_TRUE(tmpl.write(sequence, timestamp, mr, tu, out));
    return out;
}

constexpr std::array<avtp::AafFormat, 4> AAF_FORMATS{
    avtp::AafFormat::float_32bit, avtp::AafFormat::int_32bit, avtp::AafFormat::int_24bit, avtp::AafFormat::int_16bit};
constexpr std::array<avtp::AafSampleRate, 3> AAF_RATES{
    avtp::AafSampleRate::rate_44_1_khz, avtp::AafSampleRate::rate_48_khz, avtp::AafSampleRate::rate_96_khz};

}  // namespace

TEST(stream_frame_template, the_aaf_template_s_fields_sit_where_ieee_1722_puts_them)
{
    avtp::AafStreamOutputContext const ctx{tsn::StreamId{}, avtp::AafFormat::int_32bit, avtp::AafSampleRate::rate_48_khz, 8, 32, 0};
    auto const tmpl = avb_entity::aaf_frame_template(ctx, 6, avb_entity::stream_frame_header({}, {}, 2, 3));
    EXPECT_TRUE(tmpl.has_value());
    EXPECT_EQ(tmpl->length, size_t{42});
    EXPECT_EQ(tmpl->mr_offset, size_t{19});  // octet 1 of the AVTPDU, behind 18 octets of tagged header
    EXPECT_EQ(tmpl->sequence_offset, size_t{20});
    EXPECT_EQ(tmpl->tu_offset, size_t{21});
    EXPECT_EQ(tmpl->timestamp_offset, size_t{30});
    EXPECT_EQ(tmpl->bytes[18], uint8_t{0x02});  // subtype AAF
    EXPECT_EQ(tmpl->bytes[19], uint8_t{0x81});  // sv, tv; mr clear
    EXPECT_EQ(tmpl->bytes[21], uint8_t{0x00});  // tu clear
    EXPECT_EQ(tmpl->bytes[38], uint8_t{0x00});  // stream_data_length 6 x 8 x 4 = 192
    EXPECT_EQ(tmpl->bytes[39], uint8_t{0xC0});
}

TEST(stream_frame_template, the_crf_template_s_fields_sit_where_ieee_1722_puts_them)
{
    avtp::CrfStreamOutputContext const ctx{
        tsn::StreamId{}, avtp::CrfType::audio_sample, 48'000, avtp::CrfPull::multiply_1_0, 96, 1};
    auto const tmpl = avb_entity::crf_frame_template(ctx, avb_entity::stream_frame_header({}, {}, 2, 3));
    EXPECT_TRUE(tmpl.has_value());
    EXPECT_EQ(tmpl->length, size_t{38});
    EXPECT_EQ(tmpl->mr_offset, size_t{19});  // mr and tu share octet 1 (Clause 10.4)
    EXPECT_EQ(tmpl->tu_offset, size_t{19});
    EXPECT_EQ(tmpl->sequence_offset, size_t{20});
    EXPECT_EQ(tmpl->timestamp_offset, size_t{0});  // its timestamps are its payload
    EXPECT_EQ(tmpl->bytes[18], uint8_t{0x04});     // subtype CRF
}

TEST(stream_frame_template, written_an_aaf_template_is_the_header_the_talker_sends)
{
    Lcg rng{};
    for (int i = 0; i < 2000; ++i) {
        auto const format = AAF_FORMATS[rng.next(AAF_FORMATS.size())];
        auto const channels = static_cast<uint16_t>(1 + rng.next(8));
        auto const frames = static_cast<uint16_t>(1 + rng.next(12));
        tsn::StreamId stream_id{};
        (void)tsn::load_unchecked(eui48_for(rng).span(), &stream_id);
        avtp::AafStreamOutputContext ctx{stream_id, format, AAF_RATES[rng.next(AAF_RATES.size())], channels, 32, 0};
        ctx.sequence_num = static_cast<uint8_t>(rng.next(256));
        auto const eth = addressing_for(rng);
        uint32_t const timestamp = rng.next(0xFFFF'FFFFU);
        bool const mr = rng.next(2) != 0;
        bool const tu = rng.next(2) != 0;

        auto const tmpl = avb_entity::aaf_frame_template(ctx, frames, eth.header());
        EXPECT_TRUE(tmpl.has_value());
        EXPECT_TRUE(
            written_for(*tmpl, ctx.sequence_num, timestamp, mr, tu) == aaf_frame_headers(ctx, frames, timestamp, mr, tu, eth));
    }
}

TEST(stream_frame_template, written_a_crf_template_is_the_header_the_talker_sends)
{
    Lcg rng{};
    for (int i = 0; i < 500; ++i) {
        tsn::StreamId stream_id{};
        (void)tsn::load_unchecked(eui48_for(rng).span(), &stream_id);
        avtp::CrfStreamOutputContext ctx{
            stream_id,
            avtp::CrfType::audio_sample,
            rng.next(2) != 0 ? 48'000U : 96'000U,
            avtp::CrfPull::multiply_1_0,
            static_cast<uint16_t>(1 + rng.next(192)),
            static_cast<uint16_t>(1 + rng.next(avb_entity::TalkerStreams::MAX_CRF_TIMESTAMPS_PER_PDU))};
        ctx.sequence_num = static_cast<uint8_t>(rng.next(256));
        auto const eth = addressing_for(rng);
        bool const mr = rng.next(2) != 0;
        bool const tu = rng.next(2) != 0;

        auto const tmpl = avb_entity::crf_frame_template(ctx, eth.header());
        EXPECT_TRUE(tmpl.has_value());
        EXPECT_TRUE(written_for(*tmpl, ctx.sequence_num, 0, mr, tu) == crf_frame_headers(ctx, mr, tu, eth));
    }
}

TEST(stream_frame_template, a_frame_the_talker_would_not_send_has_no_template)
{
    auto const eth = avb_entity::stream_frame_header({}, {}, 2, 3);
    avtp::AafStreamOutputContext const aaf{tsn::StreamId{}, avtp::AafFormat::int_32bit, avtp::AafSampleRate::rate_48_khz, 8, 32, 0};
    EXPECT_FALSE(avb_entity::aaf_frame_template(aaf, 0, eth).has_value());   // no frames
    EXPECT_TRUE(avb_entity::aaf_frame_template(aaf, 46, eth).has_value());   // 24 + 1472 octets: an Ethernet payload
    EXPECT_FALSE(avb_entity::aaf_frame_template(aaf, 47, eth).has_value());  // 24 + 1504: past one
    avtp::AafStreamOutputContext const unlaid{
        tsn::StreamId{}, avtp::AafFormat::user_specified, avtp::AafSampleRate::rate_48_khz, 8, 32, 0};
    EXPECT_FALSE(avb_entity::aaf_frame_template(unlaid, 6, eth).has_value());  // no byte layout

    auto crf_template_for = [&](uint16_t const timestamps) {
        avtp::CrfStreamOutputContext const crf{
            tsn::StreamId{}, avtp::CrfType::audio_sample, 48'000, avtp::CrfPull::multiply_1_0, 96, timestamps};
        return avb_entity::crf_frame_template(crf, eth).has_value();
    };
    EXPECT_FALSE(crf_template_for(0));
    EXPECT_TRUE(crf_template_for(avb_entity::TalkerStreams::MAX_CRF_TIMESTAMPS_PER_PDU));
    EXPECT_FALSE(crf_template_for(avb_entity::TalkerStreams::MAX_CRF_TIMESTAMPS_PER_PDU + 1));
}

TEST(stream_frame_template, write_leaves_a_buffer_too_short_for_the_headers_alone)
{
    avtp::CrfStreamOutputContext const ctx{
        tsn::StreamId{}, avtp::CrfType::audio_sample, 48'000, avtp::CrfPull::multiply_1_0, 96, 1};
    auto const tmpl = avb_entity::crf_frame_template(ctx, avb_entity::stream_frame_header({}, {}, 2, 3));
    EXPECT_TRUE(tmpl.has_value());
    std::vector<uint8_t> out(tmpl->length - 1, 0xEE);
    EXPECT_FALSE(tmpl->write(7, 0, true, true, out));
    EXPECT_TRUE(std::ranges::all_of(out, [](uint8_t const b) { return b == 0xEE; }));
}

TEST(stream_frame_template, a_talker_stream_s_template_follows_its_destination_and_the_vlan_in_force)
{
    ptpclient::MediaClockGenerator const mc{};
    std::atomic<int64_t> const gptp_now{0};
    avb_entity::TalkerStreams talker{
        avb_entity::TalkerStreamsConfig{.sample_rate = 48'000, .vlan_id = 2, .stream_pcp = 3},
        mc,
        gptp_now,
        std::pmr::get_default_resource()};
    ieee::Eui48 const dest{0x91, 0xE0, 0xF0, 0x00, 0x0E, 0x80};
    ieee::Eui48 const src{0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    auto open = [&talker, &dest](uint16_t const index, uint64_t const word) {
        avb_entity::StreamSpec spec{};
        spec.index = index;
        spec.format_word = word;
        spec.format = avb_entity::decode_stream_format(word);
        EXPECT_TRUE(talker.open_stream(spec, {}, dest).has_value());
    };
    open(0, 0x0205022002006000ULL);  // AAF 48k int32 2ch, 6 frames
    open(1, 0x041060010000BB80ULL);  // CRF 48k
    open(2, 0x00A0020840000800ULL);  // AM824: no template

    talker.set_vlan(0x123, 5);
    Addressing const tagged{.dest = dest, .src = src, .vid = 0x123, .pcp = 5};
    auto const aaf = talker.frame_template(0, src);
    EXPECT_TRUE(aaf.has_value());
    auto const& aaf_slot = *talker.slot_for(0);
    EXPECT_TRUE(aaf_slot.aaf.has_value());
    EXPECT_TRUE(
        written_for(*aaf, 0, 0, false, false) == aaf_frame_headers(*aaf_slot.aaf, aaf_slot.aaf_samples, 0, false, false, tagged));
    EXPECT_EQ(aaf->bytes[14], uint8_t{0xA1});  // PCP 5, VID 0x123
    EXPECT_EQ(aaf->bytes[15], uint8_t{0x23});

    auto const crf = talker.frame_template(1, src);
    EXPECT_TRUE(crf.has_value());
    auto const& crf_ctx = talker.slot_for(1)->crf;
    EXPECT_TRUE(crf_ctx.has_value());
    EXPECT_TRUE(written_for(*crf, 0, 0, false, false) == crf_frame_headers(*crf_ctx, false, false, tagged));
    EXPECT_FALSE(talker.frame_template(2, src).has_value());
    EXPECT_FALSE(talker.frame_template(3, src).has_value());  // no stream there
}

TEST_MAIN(statusbar_avb_entity, avb_entity_talker_streams_test)
