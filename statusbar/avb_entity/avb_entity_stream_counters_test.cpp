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
#include <string>

using statusbar::avb_entity::interrupt_stream_input;
using statusbar::avb_entity::restart_stream_input;
using statusbar::avb_entity::StreamInputCounters;
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

// Test runner

TEST_MAIN(statusbar_avb_entity, avb_entity_stream_counters_test)
