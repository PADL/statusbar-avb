// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the STREAM_INPUT health-counter bookkeeping
// (avb_entity_stream_counters). The headline case: for AM824 / IEC 61883-6 the
// avtp_timestamp follows the SYT_INTERVAL cadence, so tv=0 between SYT-bearing
// packets is NORMAL and must NOT be tallied as TIMESTAMP_NOT_VALID (ts_sparse=
// true). AAF timestamps every packet, so tv=0 there IS a fault (ts_sparse=false).

#include "statusbar/avb_entity/avb_entity_stream_counters.hpp"

#include "statusbar/test/test.hpp"

#include <cstdint>

using statusbar::avb_entity::StreamInputCounters;
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

TEST_MAIN(statusbar_avb_entity, avb_entity_stream_counters_test)
