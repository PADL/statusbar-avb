// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_stream_counters.hpp"

#include <format>
#include <iterator>
#include <utility>

namespace statusbar::avb_entity {

namespace {

auto const relaxed = std::memory_order_relaxed;

/// Sequence-number continuity (8-bit wrap) and media-clock restart (the
/// talker toggles mr when its media clock resets) against the previous
/// packet, which this one then becomes.
void tally_continuity(StreamInputCounters& c, uint8_t const seq, bool const mr) noexcept
{
    if (c.have_prev && seq != static_cast<uint8_t>(c.prev_seq + 1U)) {
        c.seq_num_mismatch.fetch_add(1, relaxed);
    }
    if (c.have_prev && mr != c.prev_mr) {
        c.media_reset.fetch_add(1, relaxed);
    }
    c.prev_seq = seq;
    c.prev_mr = mr;
    c.have_prev = true;
}

/// LATE/EARLY vs gPTP-now (lower 32 bits of ns), and the margin statistics
/// behind the counts. The reactor clock is monotonic, so the caller passes
/// last_gptp_ns_ published by the media timer (<=125us stale, fine against
/// ms-scale thresholds). diff<0 => presentation time already passed (late);
/// diff implausibly large => stamped too early. gptp_now_ns==0 means "unknown"
/// -> skip.
void tally_presentation_margin(StreamInputCounters& c, uint32_t const avtp_ts, int64_t const gptp_now_ns) noexcept
{
    constexpr uint32_t EARLY_THRESHOLD_NS = 50'000'000;  // 50 ms
    if (gptp_now_ns == 0) {
        return;
    }
    int32_t const diff = static_cast<int32_t>(avtp_ts - static_cast<uint32_t>(gptp_now_ns));
    c.margin.update(diff);
    c.margin_histogram.update(diff);
    if (diff < 0) {
        c.late_timestamp.fetch_add(1, relaxed);
        c.lateness.update(-static_cast<int64_t>(diff));
    } else if (static_cast<uint32_t>(diff) > EARLY_THRESHOLD_NS) {
        c.early_timestamp.fetch_add(1, relaxed);
    }
}

// Media lock = a STEADY inter-(valid-)timestamp STEP. AAF advances
// 125 us/packet (12 samples @ 96 kHz). AM824's avtp_timestamp instead
// follows the IEC 61883-6 SYT cadence: SYT_INTERVAL=16 @ 96 kHz, so a valid
// timestamp appears on 3 of every 4 (12-sample) packets and the valid stamps
// step a constant 166.67 us. So we lock on the step being CONSTANT, not on a
// hardcoded per-packet value (which only ever matched AAF). Both ends are
// gPTP-slaved so the step is correct by construction; jitter/drops change it
// and unlock. A coarse plausibility window vs the nominal packet period
// rejects garbage so two equal junk steps cannot false-lock.
constexpr int LOCK_RUN_THRESHOLD = 8;  // ~1 ms (AAF) / ~1.3 ms (AM824) of clean steps

[[nodiscard]] auto step_is_steady(
    StreamInputCounters const& c,
    uint32_t const actual,
    uint64_t const samples_per_ch,
    uint32_t const lock_tolerance_ns,
    uint32_t const sample_rate_hz) noexcept -> bool
{
    uint64_t const nominal = (samples_per_ch * 1'000'000'000ULL) / sample_rate_hz;  // 125 us @ 12/96k
    bool const plausible = (actual >= (nominal / 2)) && (actual < (nominal * 4));
    uint64_t const step_err = (actual > c.prev_delta) ? (actual - c.prev_delta) : (c.prev_delta - actual);
    return plausible && c.have_prev_delta && step_err <= lock_tolerance_ns;
}

void tally_media_lock(
    StreamInputCounters& c,
    uint32_t const avtp_ts,
    uint64_t const samples_per_ch,
    uint32_t const lock_tolerance_ns,
    uint32_t const sample_rate_hz) noexcept
{
    if (!c.have_prev_ts || samples_per_ch == 0 || sample_rate_hz == 0) {
        return;
    }
    uint32_t const actual = avtp_ts - c.prev_ts;  // wrap-safe (uint32)
    if (step_is_steady(c, actual, samples_per_ch, lock_tolerance_ns, sample_rate_hz)) {
        if (c.locked_run < LOCK_RUN_THRESHOLD) {
            ++c.locked_run;
        }
        if (c.locked_run >= LOCK_RUN_THRESHOLD && !c.is_locked.load()) {
            // Counted before it is shown as locked, so that the thread that
            // clears it never counts MEDIA_UNLOCKED ahead of MEDIA_LOCKED.
            c.media_locked.fetch_add(1, relaxed);
            (void)c.is_locked.exchange(true);
        }
    } else {
        c.locked_run = 0;
        interrupt_stream_input(c);
    }
    c.prev_delta = actual;
    c.have_prev_delta = true;
}

/// Forget the packet before this one: the next is the first of a stream.
void forget_run(StreamInputCounters& c) noexcept
{
    c.have_prev = false;
    c.have_prev_ts = false;
    c.have_prev_delta = false;
    c.locked_run = 0;
}

/// "{:+.1f} us" of a nanosecond value.
auto us(double const ns) -> double
{
    return ns / 1000.0;
}

void format_margin_lines_to(std::string& out, StreamInputCounters const& c)
{
    auto const m = c.margin.snapshot();
    auto const l = c.lateness.snapshot();
    std::format_to(
        std::back_inserter(out),
        "  Margin:          avg {:+.1f} us  min {:+.1f} us  max {:+.1f} us  sd {:.1f} us  ({} frames)\n",
        us(m.average_ns()),
        us(static_cast<double>(m.min_ns)),
        us(static_cast<double>(m.max_ns)),
        us(m.stddev_ns()),
        m.count);
    if (l.has_samples()) {
        std::format_to(
            std::back_inserter(out),
            "  Late by:         avg {:.1f} us  min {:.1f} us  max {:.1f} us  sd {:.1f} us\n",
            us(l.average_ns()),
            us(static_cast<double>(l.min_ns)),
            us(static_cast<double>(l.max_ns)),
            us(l.stddev_ns()));
    }
}

void format_heading_lines_to(std::string& out, uint16_t const stream_index, StreamInputCounters const& c)
{
    auto const frames = c.margin.count();
    auto const late = c.late_timestamp.load(relaxed);
    auto const early = c.early_timestamp.load(relaxed);
    std::format_to(
        std::back_inserter(out),
        "\n  --- STREAM_INPUT {} presentation margin (avtp_timestamp - gPTP now) ---\n"
        "  Late:            {} ({:.3f}%)   Early: {}\n",
        stream_index,
        late,
        100.0 * static_cast<double>(late) / static_cast<double>(frames),
        early);
}

void format_histogram_to(std::string& out, StreamInputCounters const& c)
{
    auto const hist = c.margin_histogram.snapshot();
    std::format_to(std::back_inserter(out), "  Histogram ({} ns bins):\n", hist.config.bin_width_ns);
    hist.format_to(std::back_inserter(out));
}

}  // namespace

void format_stream_input_timing_to(std::string& out, uint16_t const stream_index, StreamInputCounters const& c)
{
    if (c.margin.count() == 0) {
        return;
    }
    format_heading_lines_to(out, stream_index, c);
    format_margin_lines_to(out, c);
    format_histogram_to(out, c);
}

void unlock_stream_input(StreamInputCounters& c) noexcept
{
    if (c.is_locked.exchange(false)) {
        c.media_unlocked.fetch_add(1, relaxed);
    }
}

void interrupt_stream_input(StreamInputCounters& c) noexcept
{
    if (c.is_locked.exchange(false)) {
        c.media_unlocked.fetch_add(1, relaxed);
        c.stream_interrupted.fetch_add(1, relaxed);
    }
}

void restart_stream_input(StreamInputCounters& c) noexcept
{
    // Asked before the lock is dropped, so an AVTPDU that locks the input
    // again after the drop finds the restart waiting at the next one.
    c.restart_run.store(true);
    unlock_stream_input(c);
}

namespace {

/// @p c's media lock as a data plane beside the port checks it, @p locked: none gained in a report that cleared
/// the reactor's pending restart (@p restarted), as the RX thread's run starts afresh there.
void follow_media_lock(StreamInputCounters& c, bool const locked, bool const restarted) noexcept
{
    if (!locked) {
        interrupt_stream_input(c);
        return;
    }
    if (!restarted && !c.is_locked.load()) {
        c.media_locked.fetch_add(1, relaxed);
        (void)c.is_locked.exchange(true);
    }
}

/// add_stream_input_counts(): the AVTPDUs, and those out of sequence or with a media clock restart.
void add_frame_counts(StreamInputCounters& c, StreamInputCounts const& counts) noexcept
{
    c.frames_rx.fetch_add(counts.frames_rx, relaxed);
    c.seq_num_mismatch.fetch_add(counts.seq_num_mismatch, relaxed);
    c.media_reset.fetch_add(counts.media_reset, relaxed);
}

/// add_stream_input_counts(): the AVTPDUs by what their timestamp said, and those of another format.
void add_timestamp_counts(StreamInputCounters& c, StreamInputCounts const& counts) noexcept
{
    c.timestamp_uncertain.fetch_add(counts.timestamp_uncertain, relaxed);
    c.timestamp_valid.fetch_add(counts.timestamp_valid, relaxed);
    c.timestamp_not_valid.fetch_add(counts.timestamp_not_valid, relaxed);
    c.unsupported_format.fetch_add(counts.unsupported_format, relaxed);
    c.late_timestamp.fetch_add(counts.late_timestamp, relaxed);
    c.early_timestamp.fetch_add(counts.early_timestamp, relaxed);
}

}  // namespace

void add_stream_input_counts(StreamInputCounters& c, StreamInputCounts const& counts) noexcept
{
    add_frame_counts(c, counts);
    add_timestamp_counts(c, counts);
    // As at a packet: a pending restart is cleared by any report that says something of the stream.
    bool const restarted = (counts.frames_rx != 0 || counts.media_locked) && c.restart_run.exchange(false);
    if (counts.media_locked) {
        follow_media_lock(c, *counts.media_locked, restarted);
    }
}

void tally_discarded_stream_input_packet(StreamInputCounters& c) noexcept
{
    c.frames_rx.fetch_add(1, relaxed);
    unlock_stream_input(c);
    forget_run(c);
}

void tally_stream_input_packet(
    StreamInputCounters& c,
    uint8_t const seq,
    uint32_t const avtp_ts,
    bool const tv,
    bool const tu,
    bool const mr,
    bool const format_ok,
    uint64_t const samples_per_ch,
    bool const ts_sparse,
    int64_t const gptp_now_ns,
    uint32_t const lock_tolerance_ns,
    uint32_t const sample_rate_hz)
{
    if (c.restart_run.load(relaxed) && c.restart_run.exchange(false)) {
        forget_run(c);  // the input lost its stream since the last AVTPDU: this one starts a new one
    }
    c.frames_rx.fetch_add(1, relaxed);
    // Every Stream Data AVTPDU counts toward sequence and mr, whatever its format (Milan v1.3 Section 5.3.8.10).
    tally_continuity(c, seq, mr);
    if (!format_ok) {
        c.unsupported_format.fetch_add(1, relaxed);
        return;
    }

    if (tv) {
        c.timestamp_valid.fetch_add(1, relaxed);
        if (tu) {
            c.timestamp_uncertain.fetch_add(1, relaxed);
        }
        tally_presentation_margin(c, avtp_ts, gptp_now_ns);
        tally_media_lock(c, avtp_ts, samples_per_ch, lock_tolerance_ns, sample_rate_hz);
        c.prev_ts = avtp_ts;
        c.have_prev_ts = true;
    } else if (!ts_sparse) {
        // tv=0 is a real fault for a format that timestamps every packet (AAF).
        // For IEC 61883-6 (AM824) tv=0 between SYT-bearing packets is the normal
        // SYT_INTERVAL cadence (see the media-lock note above), so ts_sparse
        // streams do NOT tally it -- it would otherwise climb ~1/4 of FRAMES_RX
        // on a perfectly healthy, media-locked AM824 stream and read as an error.
        c.timestamp_not_valid.fetch_add(1, relaxed);
    }
}

namespace {

using RunningCounter = std::atomic<uint32_t> StreamInputCounters::*;
using I = StreamInputIntervals;

/// The running counter behind each interval counter of Milan v1.3 Table 5.6.
constexpr std::array<std::pair<I::Index, RunningCounter>, 7> INTERVAL_COUNTERS{{
    {I::SEQ_NUM_MISMATCH, &StreamInputCounters::seq_num_mismatch},
    {I::MEDIA_RESET, &StreamInputCounters::media_reset},
    {I::TIMESTAMP_UNCERTAIN, &StreamInputCounters::timestamp_uncertain},
    {I::UNSUPPORTED_FORMAT, &StreamInputCounters::unsupported_format},
    {I::LATE_TIMESTAMP, &StreamInputCounters::late_timestamp},
    {I::EARLY_TIMESTAMP, &StreamInputCounters::early_timestamp},
    {I::FRAMES_RX, &StreamInputCounters::frames_rx},
}};

/// The event counters of @p c less @p base into @p out, read at one instant:
/// MEDIA_UNLOCKED and STREAM_INTERRUPTED between two equal reads of
/// MEDIA_LOCKED, so neither is ahead of a lock the RX thread counted meanwhile.
void read_events(
    StreamInputCounters const& c, std::array<uint32_t, I::COUNT> const& base, std::array<uint32_t, I::COUNT>& out) noexcept
{
    for (uint32_t locked = c.media_locked.load();;) {
        out[I::MEDIA_UNLOCKED] = c.media_unlocked.load() - base[I::MEDIA_UNLOCKED];
        out[I::STREAM_INTERRUPTED] = c.stream_interrupted.load() - base[I::STREAM_INTERRUPTED];
        if (std::exchange(locked, c.media_locked.load()) == locked) {
            out[I::MEDIA_LOCKED] = locked - base[I::MEDIA_LOCKED];
            return;
        }
    }
}

}  // namespace

auto StreamInputIntervals::current(StreamInputCounters const& c) const noexcept -> std::array<uint32_t, COUNT>
{
    auto out = values;
    read_events(c, last, out);
    return out;
}

auto StreamInputIntervals::fold(StreamInputCounters& c) noexcept -> bool
{
    if (c.frames_rx.load(relaxed) == last[FRAMES_RX]) {
        // Nothing received for a whole interval: the stream stopped, and one that resumes starts afresh.
        c.restart_run.store(true);
        interrupt_stream_input(c);
    }
    for (auto const& [index, running] : INTERVAL_COUNTERS) {
        uint32_t const now = (c.*running).load(relaxed);
        values[index] += now != std::exchange(last[index], now) ? 1U : 0U;
    }
    values = current(c);
    return std::exchange(folded, values) != values;
}

void StreamInputIntervals::reset(StreamInputCounters const& c) noexcept
{
    values = {};
    for (auto const& [index, running] : INTERVAL_COUNTERS) {
        last[index] = (c.*running).load(relaxed);
    }
    read_events(c, {}, last);
}

namespace {

using O = StreamOutputIntervals;

/// The total behind each interval counter of Milan v1.3 Table 5.4.
constexpr std::array<std::pair<O::Index, uint32_t StreamOutputTotals::*>, 3> OUTPUT_INTERVAL_COUNTERS{{
    {O::MEDIA_RESET, &StreamOutputTotals::mr_toggles},
    {O::TIMESTAMP_UNCERTAIN, &StreamOutputTotals::tu_pdus},
    {O::FRAMES_TX, &StreamOutputTotals::pdus},
}};

/// The interval counters of @p values at the end of an interval in which the totals went from @p last to @p totals.
void count_output_intervals(
    std::array<uint32_t, O::COUNT>& values, StreamOutputTotals const& last, StreamOutputTotals const& totals) noexcept
{
    bool const started = totals.starts != last.starts;  // Table 5.4: "Reset to 0 each time the Talker starts streaming"
    for (auto const& [index, total] : OUTPUT_INTERVAL_COUNTERS) {
        values[index] = (started ? 0U : values[index]) + (totals.*total != last.*total ? 1U : 0U);
    }
}

}  // namespace

auto StreamOutputIntervals::current(StreamOutputTotals const& totals) const noexcept -> std::array<uint32_t, COUNT>
{
    auto out = totals.starts != last.starts ? std::array<uint32_t, COUNT>{} : values;  // Table 5.4: reset at a start
    out[STREAM_START] = totals.starts;
    out[STREAM_STOP] = totals.stops;
    return out;
}

auto StreamOutputIntervals::fold(StreamOutputTotals const& totals) noexcept -> bool
{
    auto const before = values;
    values[STREAM_START] = totals.starts;
    values[STREAM_STOP] = totals.stops;
    count_output_intervals(values, last, totals);
    last = totals;
    return values != before;
}

}  // namespace statusbar::avb_entity
