// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the STREAM_INPUT health-counter bookkeeping
// (avb_entity_stream_counters). The headline case: for AM824 / IEC 61883-6 the
// avtp_timestamp follows the SYT_INTERVAL cadence, so tv=0 between SYT-bearing
// packets is NORMAL and must NOT be tallied as TIMESTAMP_NOT_VALID (ts_sparse=
// true). AAF timestamps every packet, so tv=0 there IS a fault (ts_sparse=false).

#include "statusbar/avb_entity/avb_entity_stream_counters.hpp"

#include "statusbar/test/test.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

using statusbar::avb_entity::add_stream_input_counts;
using statusbar::avb_entity::interrupt_stream_input;
using statusbar::avb_entity::restart_stream_input;
using statusbar::avb_entity::StreamInputCounters;
using statusbar::avb_entity::StreamInputCounts;
using statusbar::avb_entity::StreamInputIntervals;
using statusbar::avb_entity::StreamOutputIntervals;
using statusbar::avb_entity::StreamOutputTotals;
using statusbar::avb_entity::tally_discarded_stream_input_packet;
using statusbar::avb_entity::tally_stream_input_packet;

namespace {

constexpr uint64_t SAMPLES_PER_PKT = 12;  // 12 samples/packet @ 96 kHz = 8000 pps
constexpr uint32_t FS = 96000;
constexpr uint32_t LOCK_TOL_NS = 5000;  // default ±5 us
constexpr uint64_t GPTP_OFF = 0;        // 0 => skip LATE/EARLY detection

// Feed one packet with the per-test defaults (tu=false, mr=false, format_ok=true).
void feed(StreamInputCounters& c, uint8_t seq, uint32_t avtp_ts, bool tv, bool ts_sparse)
{
    tally_stream_input_packet(
        c, seq, avtp_ts, tv, /*tu=*/false, /*mr=*/false, /*format_ok=*/true, SAMPLES_PER_PKT, ts_sparse, GPTP_OFF, LOCK_TOL_NS, FS);
}

}  // namespace

// --- The fix: AM824 cadence tv=0 packets are not "not valid" ----------------

TEST(stream_counters, am824_cadence_tv0_not_counted_as_not_valid)
{
    StreamInputCounters c;
    // 100 packets in the 61883-6 [valid,valid,valid,invalid] cadence (3 of 4
    // carry a SYT). ts_sparse=true => the 25 tv=0 packets are expected, not faults.
    uint32_t valid_ts = 0;
    for (uint8_t i = 0; i < 100; ++i) {
        bool const tv = (i % 4) != 3;  // tv=0 on every 4th packet
        feed(c, i, tv ? valid_ts : 0, tv, /*ts_sparse=*/true);
        if (tv) {
            valid_ts += 166'667;  // ~166.67 us between valid stamps
        }
    }

    EXPECT_EQ(c.frames_rx.load(), 100U);
    EXPECT_EQ(c.timestamp_valid.load(), 75U);     // 3 of every 4
    EXPECT_EQ(c.timestamp_not_valid.load(), 0U);  // <-- off-SYT tv=0 packets must NOT count as TIMESTAMP_NOT_VALID
    EXPECT_EQ(c.unsupported_format.load(), 0U);
    EXPECT_EQ(c.seq_num_mismatch.load(), 0U);
}

// --- AAF: every packet is stamped, so tv=0 IS a fault -----------------------

TEST(stream_counters, aaf_tv0_is_counted_as_not_valid)
{
    StreamInputCounters c;
    // 10 AAF packets, 2 of them with tv cleared (a real anomaly for AAF).
    for (uint8_t i = 0; i < 10; ++i) {
        bool const tv = (i != 3 && i != 7);
        feed(c, i, static_cast<uint32_t>(i) * 125'000U, tv, /*ts_sparse=*/false);
    }

    EXPECT_EQ(c.frames_rx.load(), 10U);
    EXPECT_EQ(c.timestamp_valid.load(), 8U);
    EXPECT_EQ(c.timestamp_not_valid.load(), 2U);
}

// --- Other bookkeeping survived the extraction ------------------------------

TEST(stream_counters, unsupported_format_counts_and_short_circuits)
{
    StreamInputCounters c;
    tally_stream_input_packet(
        c, 0, 0, /*tv=*/false, false, false, /*format_ok=*/false, 0, /*ts_sparse=*/true, GPTP_OFF, LOCK_TOL_NS, FS);
    EXPECT_EQ(c.frames_rx.load(), 1U);
    EXPECT_EQ(c.unsupported_format.load(), 1U);
    EXPECT_EQ(c.timestamp_valid.load(), 0U);
    EXPECT_EQ(c.timestamp_not_valid.load(), 0U);  // short-circuited before tv check
}

TEST(stream_counters, sequence_gap_is_flagged)
{
    StreamInputCounters c;
    feed(c, 0, 0, true, false);  // first: no prev, no check
    feed(c, 1, 125'000, true, false);
    feed(c, 2, 250'000, true, false);
    feed(c, 4, 375'000, true, false);  // gap: 4 != 2+1
    EXPECT_EQ(c.seq_num_mismatch.load(), 1U);
}

TEST(stream_counters, media_locks_on_steady_step)
{
    StreamInputCounters c;
    // A constant 125 us inter-timestamp step (within the plausibility window and
    // < lock tolerance error) must engage MEDIA_LOCKED after the run threshold.
    for (uint8_t i = 0; i < 14; ++i) {
        feed(c, i, static_cast<uint32_t>(i) * 125'000U, /*tv=*/true, /*ts_sparse=*/false);
    }
    EXPECT_EQ(c.media_locked.load(), 1U);
    EXPECT_EQ(c.media_unlocked.load(), 0U);
}

TEST(stream_counters, jittered_step_does_not_lock)
{
    StreamInputCounters c;
    // Steps swinging well past the ±5 us tolerance never accumulate a locked run.
    uint32_t ts = 0;
    for (uint8_t i = 0; i < 20; ++i) {
        ts += (i % 2 == 0) ? 125'000U : 250'000U;  // alternating step
        feed(c, i, ts, /*tv=*/true, /*ts_sparse=*/false);
    }
    EXPECT_EQ(c.media_locked.load(), 0U);
}

// --- STREAM_INTERRUPTED: Milan v1.3 Table 5.6 --------------------------------

TEST(stream_counters, a_lock_lost_to_a_step_out_of_tolerance_is_an_interruption)
{
    // A locked stream whose timestamps break their step (lost AVTPDUs) loses
    // the lock while it is received: MEDIA_UNLOCKED and STREAM_INTERRUPTED.
    StreamInputCounters c;
    uint32_t ts = 0;
    uint8_t seq = 0;
    for (; seq < 14; ++seq, ts += 125'000U) {
        feed(c, seq, ts, /*tv=*/true, /*ts_sparse=*/false);
    }
    ts += 125'000U;  // one AVTPDU lost
    feed(c, ++seq, ts, /*tv=*/true, /*ts_sparse=*/false);
    EXPECT_FALSE(c.is_locked.load());
    EXPECT_EQ(c.media_unlocked.load(), 1U);
    EXPECT_EQ(c.stream_interrupted.load(), 1U);

    // Locked again, then stopped (a controller's STOP_STREAMING) and restarted
    // (a disconnect): the lock goes each time, and neither is an interruption.
    for (int i = 0; i < 14; ++i) {
        ts += 125'000U;
        feed(c, ++seq, ts, /*tv=*/true, /*ts_sparse=*/false);
    }
    EXPECT_EQ(c.media_locked.load(), 2U);
    tally_discarded_stream_input_packet(c);
    for (int i = 0; i < 14; ++i) {
        ts += 125'000U;
        feed(c, ++seq, ts, /*tv=*/true, /*ts_sparse=*/false);
    }
    EXPECT_EQ(c.media_locked.load(), 3U);
    restart_stream_input(c);
    EXPECT_EQ(c.media_unlocked.load(), 3U);
    EXPECT_EQ(c.stream_interrupted.load(), 1U);
    // Of an input that is not locked, nothing is counted.
    interrupt_stream_input(c);
    EXPECT_EQ(c.stream_interrupted.load(), 1U);
}

// --- A stopped input: Milan v1.3 5.3.8.7 -------------------------------------

TEST(stream_counters, a_discarded_packet_counts_as_received_and_as_nothing_else)
{
    StreamInputCounters c;
    for (int i = 0; i < 5; ++i) {
        tally_discarded_stream_input_packet(c);
    }
    EXPECT_EQ(c.frames_rx.load(), 5U);
    EXPECT_EQ(c.media_locked.load(), 0U);
    EXPECT_EQ(c.media_unlocked.load(), 0U);
    EXPECT_EQ(c.seq_num_mismatch.load(), 0U);
    EXPECT_EQ(c.timestamp_valid.load(), 0U);
    EXPECT_EQ(c.timestamp_not_valid.load(), 0U);
    EXPECT_EQ(c.late_timestamp.load(), 0U);
    EXPECT_EQ(c.margin.count(), 0U);
}

TEST(stream_counters, stopping_a_locked_input_unlocks_it_once)
{
    // 5.3.8.10: MEDIA_LOCKED is MEDIA_UNLOCKED, or one more while the input
    // is synchronized. A stopped input is not.
    StreamInputCounters c;
    for (uint8_t i = 0; i < 14; ++i) {
        feed(c, i, static_cast<uint32_t>(i) * 125'000U, /*tv=*/true, /*ts_sparse=*/false);
    }
    EXPECT_EQ(c.media_locked.load(), 1U);
    for (int i = 0; i < 100; ++i) {
        tally_discarded_stream_input_packet(c);
    }
    EXPECT_EQ(c.media_locked.load(), 1U);
    EXPECT_EQ(c.media_unlocked.load(), 1U);
    EXPECT_EQ(c.frames_rx.load(), 114U);
}

// --- An input that loses its stream: Milan v1.3 5.3.8.10 ----------------------

TEST(stream_counters, unlocking_a_locked_input_counts_media_unlocked_once)
{
    StreamInputCounters c;
    for (uint8_t i = 0; i < 14; ++i) {
        feed(c, i, static_cast<uint32_t>(i) * 125'000U, /*tv=*/true, /*ts_sparse=*/false);
    }
    EXPECT_EQ(c.media_locked.load(), 1U);
    EXPECT_TRUE(c.is_locked.load());

    unlock_stream_input(c);
    EXPECT_FALSE(c.is_locked.load());
    EXPECT_EQ(c.media_locked.load(), 1U);
    EXPECT_EQ(c.media_unlocked.load(), 1U);
    // Asked again, and of an input that never locked: nothing more.
    unlock_stream_input(c);
    EXPECT_EQ(c.media_unlocked.load(), 1U);
    StreamInputCounters never{};
    unlock_stream_input(never);
    EXPECT_EQ(never.media_unlocked.load(), 0U);
}

TEST(stream_counters, an_unlocked_input_locks_again_on_a_steady_stream)
{
    // The stream goes on (or another comes): the detector's run is still
    // good, so the next steady step locks it again, and the two counters
    // stay one apart.
    StreamInputCounters c;
    for (uint8_t i = 0; i < 14; ++i) {
        feed(c, i, static_cast<uint32_t>(i) * 125'000U, /*tv=*/true, /*ts_sparse=*/false);
    }
    unlock_stream_input(c);
    feed(c, 14, 14U * 125'000U, /*tv=*/true, /*ts_sparse=*/false);
    EXPECT_TRUE(c.is_locked.load());
    EXPECT_EQ(c.media_locked.load(), 2U);
    EXPECT_EQ(c.media_unlocked.load(), 1U);
}

TEST(stream_counters, a_restarted_inputs_next_avtpdu_is_the_first_of_a_stream)
{
    // The input lost its stream (a disconnect) and another came: the first
    // AVTPDU follows no sequence_num and no mr of the old stream, and the
    // input locks again on a run of the new stream's, not on the old one's.
    StreamInputCounters c;
    for (uint8_t i = 0; i < 14; ++i) {
        feed(c, i, static_cast<uint32_t>(i) * 125'000U, /*tv=*/true, /*ts_sparse=*/false);
    }
    restart_stream_input(c);
    restart_stream_input(c);  // safe to call twice
    EXPECT_FALSE(c.is_locked.load());
    EXPECT_EQ(c.media_unlocked.load(), 1U);

    uint32_t const start = 7'000'000;
    for (uint8_t i = 0; i < 14; ++i) {
        tally_stream_input_packet(
            c,
            static_cast<uint8_t>(200U + i),
            start + (i * 125'000U),
            true,
            false,
            /*mr=*/true,
            true,
            SAMPLES_PER_PKT,
            false,
            GPTP_OFF,
            LOCK_TOL_NS,
            FS);
        if (i == 0) {
            EXPECT_EQ(c.media_locked.load(), 1U);  // no lock from the old stream's run
        }
    }
    EXPECT_EQ(c.seq_num_mismatch.load(), 0U);
    EXPECT_EQ(c.media_reset.load(), 0U);
    EXPECT_EQ(c.media_locked.load(), 2U);
    EXPECT_EQ(c.media_unlocked.load(), 1U);
}

TEST(stream_counters, a_started_input_begins_from_the_stream_as_it_is)
{
    // A hundred packets went by unread while the input was stopped: the
    // first one read after the start is no sequence error, no media reset,
    // and the input locks again as a new stream does.
    StreamInputCounters c;
    for (uint8_t i = 0; i < 14; ++i) {
        feed(c, i, static_cast<uint32_t>(i) * 125'000U, /*tv=*/true, /*ts_sparse=*/false);
    }
    for (int i = 0; i < 100; ++i) {
        tally_discarded_stream_input_packet(c);
    }
    for (uint8_t i = 114; i < 130; ++i) {
        tally_stream_input_packet(
            c,
            i,
            static_cast<uint32_t>(i) * 125'000U,
            true,
            false,
            /*mr=*/true,
            true,
            SAMPLES_PER_PKT,
            false,
            GPTP_OFF,
            LOCK_TOL_NS,
            FS);
    }
    EXPECT_EQ(c.seq_num_mismatch.load(), 0U);
    EXPECT_EQ(c.media_reset.load(), 0U);  // mr differs from before the stop: nobody was looking
    EXPECT_EQ(c.media_locked.load(), 2U);
    EXPECT_EQ(c.media_unlocked.load(), 1U);
}

// --- Presentation margin: how late is late ----------------------------------

TEST(stream_counters, margin_stats_record_how_late_and_how_early)
{
    StreamInputCounters c;
    // gPTP now = 10 ms. Stamps at +900 us, +950 us (healthy), -20 us, -40 us (late).
    constexpr uint64_t NOW = 10'000'000;
    int64_t const margins[] = {900'000, 950'000, -20'000, -40'000};
    uint8_t seq = 0;
    for (auto const m : margins) {
        tally_stream_input_packet(
            c,
            seq++,
            static_cast<uint32_t>(static_cast<int64_t>(NOW) + m),
            true,
            false,
            false,
            true,
            SAMPLES_PER_PKT,
            false,
            NOW,
            LOCK_TOL_NS,
            FS);
    }
    EXPECT_EQ(c.late_timestamp.load(), 2U);
    EXPECT_EQ(c.early_timestamp.load(), 0U);
    auto const margin = c.margin.snapshot();
    EXPECT_EQ(margin.count, 4);
    EXPECT_EQ(margin.min_ns, -40'000);
    EXPECT_EQ(margin.max_ns, 950'000);
    auto const late = c.lateness.snapshot();
    EXPECT_EQ(late.count, 2);
    EXPECT_EQ(late.min_ns, 20'000);
    EXPECT_EQ(late.max_ns, 40'000);
    EXPECT_EQ(late.sum_ns, 60'000);
}

TEST(stream_counters, margin_histogram_bins_by_presentation_offset)
{
    StreamInputCounters c;
    // 1 ms offset: [-500 us, +1.5 ms) in 15.625 us bins. A stamp 900 us ahead
    // lands in bin (900000 + 500000) / 15625 = 89; one 20 us late in bin 30.
    auto const cfg = StreamInputCounters::margin_histogram_config_for(1'000'000);
    EXPECT_EQ(cfg.low_ns, -500'000);
    EXPECT_EQ(cfg.high_ns, 1'500'000);
    EXPECT_EQ(cfg.bin_width_ns, 15'625);
    EXPECT_EQ(cfg.num_bins(), 128);
    constexpr uint64_t NOW = 10'000'000;
    tally_stream_input_packet(c, 0, NOW + 900'000, true, false, false, true, SAMPLES_PER_PKT, false, NOW, LOCK_TOL_NS, FS);
    tally_stream_input_packet(c, 1, NOW - 20'000, true, false, false, true, SAMPLES_PER_PKT, false, NOW, LOCK_TOL_NS, FS);
    auto const h = c.margin_histogram.snapshot();
    EXPECT_EQ(h.total_count(), 2);
    EXPECT_EQ(h.bins[89], 1);
    EXPECT_EQ(h.bins[30], 1);
    EXPECT_EQ(h.underflow, 0);
    EXPECT_EQ(h.overflow, 0);
}

TEST(stream_counters, margin_histogram_config_holds_128_bins_for_any_offset)
{
    // 2 ms: [-1 ms, +3 ms) in 31.25 us bins. 100 ns: the bin floors at 1 ns and
    // the range narrows to what 128 of them cover. Both keep exactly 128 bins,
    // which is what the AtomicHistogram<128> behind them can hold.
    auto const two_ms = StreamInputCounters::margin_histogram_config_for(2'000'000);
    EXPECT_EQ(two_ms.low_ns, -1'000'000);
    EXPECT_EQ(two_ms.high_ns, 3'000'000);
    EXPECT_EQ(two_ms.bin_width_ns, 31'250);
    EXPECT_EQ(two_ms.num_bins(), 128);
    auto const tiny = StreamInputCounters::margin_histogram_config_for(100);
    EXPECT_EQ(tiny.bin_width_ns, 1);
    EXPECT_EQ(tiny.low_ns, -50);
    EXPECT_EQ(tiny.high_ns, 78);
    EXPECT_EQ(tiny.num_bins(), 128);
    EXPECT_TRUE(tiny.is_valid());
}

TEST(stream_counters, margin_skipped_without_gptp_now)
{
    StreamInputCounters c;
    feed(c, 0, 125'000, /*tv=*/true, /*ts_sparse=*/false);  // GPTP_OFF: no reference clock
    EXPECT_EQ(c.margin.count(), 0);
    EXPECT_EQ(c.late_timestamp.load(), 0U);
    EXPECT_EQ(c.margin_histogram.snapshot().total_count(), 0);
}

TEST(stream_counters, timing_report_names_the_input_and_the_late_frames)
{
    StreamInputCounters c;
    std::string report;
    statusbar::avb_entity::format_stream_input_timing_to(report, 2, c);
    EXPECT_TRUE(report.empty());  // nothing to say before a timestamped frame
    constexpr uint64_t NOW = 10'000'000;
    tally_stream_input_packet(c, 0, NOW + 900'000, true, false, false, true, SAMPLES_PER_PKT, false, NOW, LOCK_TOL_NS, FS);
    tally_stream_input_packet(c, 1, NOW - 20'000, true, false, false, true, SAMPLES_PER_PKT, false, NOW, LOCK_TOL_NS, FS);
    statusbar::avb_entity::format_stream_input_timing_to(report, 2, c);
    EXPECT_NE(report.find("STREAM_INPUT 2"), std::string::npos);
    EXPECT_NE(report.find("Late:            1 (50.000%)"), std::string::npos);
    EXPECT_NE(report.find("Late by:         avg 20.0 us"), std::string::npos);
    EXPECT_NE(report.find("Histogram (15625 ns bins)"), std::string::npos);
}

// --- Milan v1.2 Table 5.6: observation intervals (StreamInputIntervals) -----

namespace {

using statusbar::avb_entity::StreamInputIntervals;

/// @p n locked-grade AAF packets from sequence @p seq, timestamp @p ts.
void feed_run(StreamInputCounters& c, uint8_t& seq, uint32_t& ts, int const n, bool const tu = false)
{
    for (int i = 0; i < n; ++i) {
        tally_stream_input_packet(
            c, seq++, ts, true, tu, false, true, SAMPLES_PER_PKT, false, GPTP_OFF, LOCK_TOL_NS, FS);
        ts += 125'000;
    }
}

}  // namespace

TEST(stream_input_intervals, interval_counters_count_intervals_not_packets)
{
    StreamInputCounters c;
    StreamInputIntervals iv;
    uint8_t seq = 0;
    uint32_t ts = 0;
    feed_run(c, seq, ts, 100, /*tu=*/true);  // locks after 9
    EXPECT_TRUE(iv.fold(c));
    EXPECT_EQ(iv.values[StreamInputIntervals::FRAMES_RX], 1U);
    EXPECT_EQ(iv.values[StreamInputIntervals::TIMESTAMP_UNCERTAIN], 1U);
    EXPECT_EQ(iv.values[StreamInputIntervals::MEDIA_LOCKED], 1U);
    EXPECT_EQ(iv.values[StreamInputIntervals::MEDIA_UNLOCKED], 0U);
    // A second interval with a sequence gap: one SEQ_NUM_MISMATCH however
    // many packets it hit; tu no longer set.
    seq += 3;
    feed_run(c, seq, ts, 50);
    seq += 5;
    feed_run(c, seq, ts, 50);
    EXPECT_TRUE(iv.fold(c));
    EXPECT_EQ(iv.values[StreamInputIntervals::FRAMES_RX], 2U);
    EXPECT_EQ(iv.values[StreamInputIntervals::SEQ_NUM_MISMATCH], 1U);
    EXPECT_EQ(iv.values[StreamInputIntervals::TIMESTAMP_UNCERTAIN], 1U);
    EXPECT_EQ(iv.values[StreamInputIntervals::STREAM_INTERRUPTED], 0U);  // the timestamps stayed in step
    EXPECT_EQ(StreamInputIntervals::VALID, 0x0F3FU);  // Table 5.16's ten counters
}

TEST(stream_input_intervals, a_timestamp_break_or_a_stopped_stream_interrupts)
{
    StreamInputCounters c;
    StreamInputIntervals iv;
    uint8_t seq = 0;
    uint32_t ts = 0;
    feed_run(c, seq, ts, 20);
    ts += 3'000'000;  // AVTPDUs lost: the timestamps jump
    feed_run(c, seq, ts, 20);  // and lock again
    (void)iv.fold(c);
    EXPECT_EQ(iv.values[StreamInputIntervals::MEDIA_LOCKED], 2U);
    EXPECT_EQ(iv.values[StreamInputIntervals::MEDIA_UNLOCKED], 1U);
    EXPECT_EQ(iv.values[StreamInputIntervals::STREAM_INTERRUPTED], 1U);
    // Nothing in the next interval: the stream stopped while locked.
    EXPECT_TRUE(iv.fold(c));
    EXPECT_EQ(iv.values[StreamInputIntervals::MEDIA_UNLOCKED], 2U);
    EXPECT_EQ(iv.values[StreamInputIntervals::STREAM_INTERRUPTED], 2U);
    EXPECT_EQ(iv.values[StreamInputIntervals::FRAMES_RX], 1U);
    EXPECT_FALSE(iv.fold(c));  // and only once
}

TEST(stream_input_intervals, unbind_is_no_interruption_and_bind_resets)
{
    StreamInputCounters c;
    StreamInputIntervals iv;
    uint8_t seq = 0;
    uint32_t ts = 0;
    feed_run(c, seq, ts, 20);
    (void)iv.fold(c);
    statusbar::avb_entity::unlock_stream_input(c);  // UNBIND_RX
    (void)iv.fold(c);
    EXPECT_EQ(iv.values[StreamInputIntervals::MEDIA_LOCKED], 1U);
    EXPECT_EQ(iv.values[StreamInputIntervals::MEDIA_UNLOCKED], 1U);
    EXPECT_EQ(iv.values[StreamInputIntervals::STREAM_INTERRUPTED], 0U);
    // Bound again, locked again: reset zeroes everything, a lock it held
    // goes first, and counting starts over.
    feed_run(c, seq, ts, 20);
    iv.reset(c);
    EXPECT_TRUE(iv.values == decltype(iv.values){});
    EXPECT_FALSE(c.is_locked.load());
    feed_run(c, seq, ts, 20);
    (void)iv.fold(c);
    EXPECT_EQ(iv.values[StreamInputIntervals::MEDIA_LOCKED], 1U);
    EXPECT_EQ(iv.values[StreamInputIntervals::MEDIA_UNLOCKED], 0U);
    EXPECT_EQ(iv.values[StreamInputIntervals::FRAMES_RX], 1U);
}

TEST(stream_input_intervals, a_reactor_unlock_restarts_the_lock_run)
{
    StreamInputCounters c;
    uint8_t seq = 0;
    uint32_t ts = 0;
    feed_run(c, seq, ts, 20);
    EXPECT_TRUE(c.is_locked.load());
    statusbar::avb_entity::unlock_stream_input(c);
    feed_run(c, seq, ts, 8);  // the run starts again: not yet
    EXPECT_FALSE(c.is_locked.load());
    feed_run(c, seq, ts, 2);
    EXPECT_TRUE(c.is_locked.load());
    EXPECT_EQ(c.media_locked.load(), 2U);
    EXPECT_EQ(c.media_unlocked.load(), 1U);
    EXPECT_EQ(c.stream_interrupted.load(), 0U);
}

// --- Milan v1.2 Table 5.4: STREAM_OUTPUT (StreamOutputIntervals) -----------

TEST(stream_output_intervals, start_stop_and_intervals)
{
    using statusbar::avb_entity::StreamOutputIntervals;
    using statusbar::avb_entity::StreamOutputTotals;
    StreamOutputIntervals iv;
    EXPECT_EQ(StreamOutputIntervals::VALID, 0x1FU);
    EXPECT_FALSE(iv.set_streaming(false, {}));
    EXPECT_TRUE(iv.set_streaming(true, {.pdus = 10}));
    EXPECT_EQ(iv.values[StreamOutputIntervals::STREAM_START], 1U);
    EXPECT_FALSE(iv.set_streaming(true, {.pdus = 10}));
    EXPECT_TRUE(iv.fold({.pdus = 8010, .tu_pdus = 2000}));
    EXPECT_TRUE(iv.fold({.pdus = 16010, .tu_pdus = 2000, .mr_toggles = 1}));
    EXPECT_FALSE(iv.fold({.pdus = 16010, .tu_pdus = 2000, .mr_toggles = 1}));  // nothing sent
    EXPECT_EQ(iv.values[StreamOutputIntervals::FRAMES_TX], 2U);
    EXPECT_EQ(iv.values[StreamOutputIntervals::TIMESTAMP_UNCERTAIN], 1U);
    EXPECT_EQ(iv.values[StreamOutputIntervals::MEDIA_RESET], 1U);
    // STREAM_START == STREAM_STOP + 1 while streaming, == when not.
    EXPECT_TRUE(iv.set_streaming(false, {.pdus = 16010}));
    EXPECT_EQ(iv.values[StreamOutputIntervals::STREAM_STOP], 1U);
    // Starting again zeroes MEDIA_RESET, TIMESTAMP_UNCERTAIN and FRAMES_TX.
    EXPECT_TRUE(iv.set_streaming(true, StreamOutputTotals{.pdus = 16010, .tu_pdus = 2000, .mr_toggles = 1}));
    EXPECT_EQ(iv.values[StreamOutputIntervals::STREAM_START], 2U);
    EXPECT_EQ(iv.values[StreamOutputIntervals::FRAMES_TX], 0U);
    EXPECT_EQ(iv.values[StreamOutputIntervals::TIMESTAMP_UNCERTAIN], 0U);
    EXPECT_EQ(iv.values[StreamOutputIntervals::MEDIA_RESET], 0U);
    EXPECT_TRUE(iv.fold({.pdus = 16020, .tu_pdus = 2000, .mr_toggles = 1}));
    EXPECT_EQ(iv.values[StreamOutputIntervals::FRAMES_TX], 1U);
    EXPECT_EQ(iv.values[StreamOutputIntervals::TIMESTAMP_UNCERTAIN], 0U);
}

// Test runner

// --- Milan v1.3 Table 5.6: by observation interval ---------------------------

namespace {

using I = StreamInputIntervals;

/// @p count steady AAF packets from sequence_num @p seq and timestamp @p ts on, both moved on past them.
void feed_run(StreamInputCounters& c, uint8_t& seq, uint32_t& ts, int const count)
{
    for (int i = 0; i < count; ++i, ++seq, ts += 125'000U) {
        feed(c, seq, ts, /*tv=*/true, /*ts_sparse=*/false);
    }
}

}  // namespace

TEST(stream_input_intervals, an_interval_counter_counts_intervals_not_avtpdus)
{
    StreamInputCounters c;
    StreamInputIntervals in;
    uint8_t seq = 0;
    uint32_t ts = 0;
    // Interval 1: three sequence gaps among a thousand AVTPDUs.
    feed_run(c, seq, ts, 100);
    for (int gap = 0; gap < 3; ++gap) {
        ++seq;
        feed_run(c, seq, ts, 300);
    }
    EXPECT_EQ(c.seq_num_mismatch.load(), 3U);
    EXPECT_TRUE(in.fold(c));
    EXPECT_EQ(in.values[I::SEQ_NUM_MISMATCH], 1U);
    EXPECT_EQ(in.values[I::FRAMES_RX], 1U);
    EXPECT_EQ(in.values[I::MEDIA_LOCKED], 1U);
    // Interval 2: a clean run moves FRAMES_RX alone.
    feed_run(c, seq, ts, 300);
    EXPECT_TRUE(in.fold(c));
    EXPECT_EQ(in.values[I::SEQ_NUM_MISMATCH], 1U);
    EXPECT_EQ(in.values[I::FRAMES_RX], 2U);
    EXPECT_EQ(in.values[I::MEDIA_LOCKED], 1U);
    EXPECT_EQ(in.values[I::MEDIA_UNLOCKED], 0U);
    // The values are those of Table 5.13 alone: TIMESTAMP_VALID and _NOT_VALID are not among them.
    EXPECT_EQ(I::VALID, 0x0000'0F3FU);
    EXPECT_EQ(in.values[6] + in.values[7], 0U);
}

TEST(stream_input_intervals, a_locked_input_that_receives_nothing_for_an_interval_is_interrupted)
{
    StreamInputCounters c;
    StreamInputIntervals in;
    uint8_t seq = 0;
    uint32_t ts = 0;
    feed_run(c, seq, ts, 20);
    EXPECT_TRUE(in.fold(c));
    // Nothing for a whole interval: the stream stopped while the input was locked.
    EXPECT_TRUE(in.fold(c));
    EXPECT_FALSE(c.is_locked.load());
    EXPECT_EQ(in.values[I::MEDIA_LOCKED], 1U);
    EXPECT_EQ(in.values[I::MEDIA_UNLOCKED], 1U);
    EXPECT_EQ(in.values[I::STREAM_INTERRUPTED], 1U);
    EXPECT_EQ(in.values[I::FRAMES_RX], 1U);
    // A further silent interval changes nothing, and says so.
    EXPECT_FALSE(in.fold(c));
    EXPECT_EQ(in.values[I::STREAM_INTERRUPTED], 1U);
}

TEST(stream_input_intervals, a_reset_zeroes_every_value_and_the_events_count_from_there)
{
    StreamInputCounters c;
    StreamInputIntervals in;
    uint8_t seq = 0;
    uint32_t ts = 0;
    feed_run(c, seq, ts, 20);
    ++seq;
    feed_run(c, seq, ts, 20);
    EXPECT_TRUE(in.fold(c));
    EXPECT_TRUE(in.fold(c));  // silent: interrupted
    EXPECT_EQ(in.values[I::STREAM_INTERRUPTED], 1U);

    in.reset(c);
    EXPECT_TRUE(std::ranges::all_of(in.values, [](uint32_t const v) { return v == 0; }));
    // The fold after a reset reports the change, and counts from the reset.
    EXPECT_TRUE(in.fold(c));
    EXPECT_TRUE(std::ranges::all_of(in.values, [](uint32_t const v) { return v == 0; }));
    ts += 40'000'000U;
    ++seq;
    feed_run(c, seq, ts, 20);
    EXPECT_TRUE(in.fold(c));
    EXPECT_EQ(in.values[I::MEDIA_LOCKED], 1U);
    EXPECT_EQ(in.values[I::MEDIA_UNLOCKED], 0U);
    EXPECT_EQ(in.values[I::SEQ_NUM_MISMATCH], 0U);  // after a silent interval, a stream of its own
    EXPECT_EQ(in.values[I::FRAMES_RX], 1U);
}

TEST(stream_input_intervals, the_event_counters_are_answered_as_they_stand_and_the_others_at_a_fold)
{
    // Milan v1.3 Table 5.6: MEDIA_LOCKED and MEDIA_UNLOCKED are incremented
    // each time, so that at any time they say whether the input is locked.
    StreamInputCounters c;
    StreamInputIntervals in;
    uint8_t seq = 0;
    uint32_t ts = 0;
    feed_run(c, seq, ts, 20);
    auto now = in.current(c);
    EXPECT_EQ(now[I::MEDIA_LOCKED], 1U);
    EXPECT_EQ(now[I::MEDIA_UNLOCKED], 0U);
    EXPECT_EQ(now[I::FRAMES_RX], 0U);  // until the interval ends
    EXPECT_TRUE(in.fold(c));
    interrupt_stream_input(c);
    now = in.current(c);
    EXPECT_EQ(now[I::MEDIA_UNLOCKED], 1U);
    EXPECT_EQ(now[I::STREAM_INTERRUPTED], 1U);
    EXPECT_EQ(now[I::FRAMES_RX], 1U);
    EXPECT_EQ(in.values[I::MEDIA_UNLOCKED], 0U);  // values are the last fold's
}

TEST(stream_input_intervals, a_stream_that_resumes_after_a_silent_interval_starts_afresh)
{
    // The talker paused for more than an interval and numbers its AVTPDUs
    // anew: no SEQ_NUM_MISMATCH or MEDIA_RESET against the AVTPDU before the pause.
    StreamInputCounters c;
    StreamInputIntervals in;
    uint8_t seq = 0;
    uint32_t ts = 0;
    feed_run(c, seq, ts, 20);
    EXPECT_TRUE(in.fold(c));
    EXPECT_TRUE(in.fold(c));  // silent
    seq = 100;
    ts += 400'000'000U;
    feed_run(c, seq, ts, 20);
    EXPECT_EQ(c.seq_num_mismatch.load(), 0U);
    EXPECT_EQ(c.media_locked.load(), 2U);
    EXPECT_EQ(c.media_unlocked.load(), 1U);
}

// --- Milan v1.3 Table 5.4: a stream output by observation interval ----------

TEST(stream_output_intervals, starts_and_stops_are_counts_and_the_others_intervals)
{
    using O = StreamOutputIntervals;
    StreamOutputIntervals out;
    StreamOutputTotals totals{};
    EXPECT_FALSE(out.fold(totals));  // nothing sent, nothing started

    // Started, and a second of AVTPDUs: one start, one interval of FRAMES_TX.
    totals.starts = 1;
    totals.pdus = 8000;
    EXPECT_TRUE(out.fold(totals));
    EXPECT_EQ(out.values[O::STREAM_START], 1U);
    EXPECT_EQ(out.values[O::FRAMES_TX], 1U);
    // Another second, two hundred AVTPDUs of it with tu set and one mr toggle.
    totals.pdus += 8000;
    totals.tu_pdus += 200;
    totals.mr_toggles += 1;
    EXPECT_TRUE(out.fold(totals));
    EXPECT_EQ(out.values[O::FRAMES_TX], 2U);
    EXPECT_EQ(out.values[O::TIMESTAMP_UNCERTAIN], 1U);
    EXPECT_EQ(out.values[O::MEDIA_RESET], 1U);

    // Stopped: a stop, and the interval counters stay; a silent second changes nothing.
    totals.stops = 1;
    EXPECT_TRUE(out.fold(totals));
    EXPECT_EQ(out.values[O::STREAM_STOP], 1U);
    EXPECT_EQ(out.values[O::FRAMES_TX], 2U);
    EXPECT_FALSE(out.fold(totals));

    // Started again: the interval counters start again from zero.
    totals.starts = 2;
    totals.pdus += 4000;
    EXPECT_TRUE(out.fold(totals));
    EXPECT_EQ(out.values[O::STREAM_START], 2U);
    EXPECT_EQ(out.values[O::STREAM_STOP], 1U);
    EXPECT_EQ(out.values[O::FRAMES_TX], 1U);
    EXPECT_EQ(out.values[O::TIMESTAMP_UNCERTAIN], 0U);
    EXPECT_EQ(out.values[O::MEDIA_RESET], 0U);
    EXPECT_EQ(O::VALID, 0x0000'009BU);
}

TEST(stream_output_intervals, starts_and_stops_are_answered_as_they_stand)
{
    // Milan v1.3 Table 5.4: at any time STREAM_START = STREAM_STOP + 1 says
    // the talker streams, and a start zeroes the interval counters.
    using O = StreamOutputIntervals;
    StreamOutputIntervals out;
    StreamOutputTotals totals{.starts = 1, .stops = 0, .pdus = 8000, .tu_pdus = 0, .mr_toggles = 0};
    auto now = out.current(totals);
    EXPECT_EQ(now[O::STREAM_START], 1U);
    EXPECT_EQ(now[O::FRAMES_TX], 0U);  // until the interval ends
    EXPECT_TRUE(out.fold(totals));
    totals.stops = 1;
    now = out.current(totals);
    EXPECT_EQ(now[O::STREAM_STOP], 1U);
    EXPECT_EQ(now[O::FRAMES_TX], 1U);
    totals.starts = 2;
    now = out.current(totals);
    EXPECT_EQ(now[O::STREAM_START], 2U);
    EXPECT_EQ(now[O::FRAMES_TX], 0U);  // the start zeroes it
    EXPECT_EQ(out.values[O::FRAMES_TX], 1U);
}

// --- Counts from a data plane beside the stream port ---

TEST(stream_input_counts, a_report_adds_to_every_counter_and_follows_the_lock)
{
    StreamInputCounters c{};
    add_stream_input_counts(
        c,
        StreamInputCounts{
            .frames_rx = 11,
            .seq_num_mismatch = 1,
            .media_reset = 2,
            .timestamp_uncertain = 3,
            .timestamp_valid = 4,
            .timestamp_not_valid = 5,
            .unsupported_format = 6,
            .late_timestamp = 7,
            .early_timestamp = 8});
    EXPECT_EQ(c.frames_rx.load(), 11U);
    EXPECT_EQ(c.seq_num_mismatch.load(), 1U);
    EXPECT_EQ(c.media_reset.load(), 2U);
    EXPECT_EQ(c.timestamp_uncertain.load(), 3U);
    EXPECT_EQ(c.timestamp_valid.load(), 4U);
    EXPECT_EQ(c.timestamp_not_valid.load(), 5U);
    EXPECT_EQ(c.unsupported_format.load(), 6U);
    EXPECT_EQ(c.late_timestamp.load(), 7U);
    EXPECT_EQ(c.early_timestamp.load(), 8U);
    EXPECT_FALSE(c.is_locked.load());  // no lock told: none followed

    add_stream_input_counts(c, StreamInputCounts{.frames_rx = 1, .media_locked = true});
    add_stream_input_counts(c, StreamInputCounts{.frames_rx = 1, .media_locked = true});
    EXPECT_EQ(c.media_locked.load(), 1U);  // gained once
    EXPECT_TRUE(c.is_locked.load());
    add_stream_input_counts(c, StreamInputCounts{.frames_rx = 1});
    EXPECT_TRUE(c.is_locked.load());
    add_stream_input_counts(c, StreamInputCounts{.frames_rx = 1, .media_locked = false});
    EXPECT_EQ(c.media_unlocked.load(), 1U);  // lost: an interruption (Milan v1.3 Table 5.6)
    EXPECT_EQ(c.stream_interrupted.load(), 1U);
    EXPECT_FALSE(c.is_locked.load());
}

// As at the RX thread's next AVTPDU after the input lost its stream: the report that clears the pending
// restart starts afresh and gains no lock; the next one may.
TEST(stream_input_counts, a_report_after_a_restart_clears_it_and_gains_no_lock_until_the_next)
{
    StreamInputCounters c{};
    restart_stream_input(c);
    add_stream_input_counts(c, StreamInputCounts{});
    EXPECT_TRUE(c.restart_run.load());  // a report of nothing says nothing of the stream
    add_stream_input_counts(c, StreamInputCounts{.frames_rx = 3, .media_locked = true});
    EXPECT_FALSE(c.restart_run.load());
    EXPECT_EQ(c.media_locked.load(), 0U);
    add_stream_input_counts(c, StreamInputCounts{.frames_rx = 3, .media_locked = true});
    EXPECT_EQ(c.media_locked.load(), 1U);

    restart_stream_input(c);  // unlocks
    add_stream_input_counts(c, StreamInputCounts{.media_locked = false});
    EXPECT_FALSE(c.restart_run.load());
    EXPECT_EQ(c.media_unlocked.load(), 1U);
    EXPECT_EQ(c.stream_interrupted.load(), 0U);  // already unlocked: nothing more
}

// Milan v1.3 Table 5.6: the observation interval reads reported counts as it reads the kit's own; an
// interval with no AVTPDU reported while locked is an interruption.
TEST(stream_input_counts, an_interval_with_no_avtpdu_reported_is_silence)
{
    StreamInputCounters c{};
    StreamInputIntervals intervals{};
    add_stream_input_counts(c, StreamInputCounts{.frames_rx = 8, .media_locked = true});
    (void)intervals.fold(c);
    EXPECT_EQ(intervals.values[I::FRAMES_RX], 1U);
    add_stream_input_counts(c, StreamInputCounts{.media_locked = true});
    (void)intervals.fold(c);
    EXPECT_EQ(c.stream_interrupted.load(), 1U);
    EXPECT_FALSE(c.is_locked.load());
}

TEST_MAIN(statusbar_avb_entity, avb_entity_stream_counters_test)
