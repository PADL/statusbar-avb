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
        // Not while the reactor's unlock since this packet began is pending: its run starts again first.
        if (c.locked_run >= LOCK_RUN_THRESHOLD && !c.restart_run.load(relaxed) && !c.is_locked.load(relaxed) &&
            !c.is_locked.exchange(true, relaxed)) {
            c.media_locked.fetch_add(1, relaxed);
        }
    } else {
        c.locked_run = 0;
        // The reactor may have dropped the lock already (fold, unbind): the
        // exchange counts each loss once.
        if (c.is_locked.load(relaxed) && c.is_locked.exchange(false, relaxed)) {
            c.media_unlocked.fetch_add(1, relaxed);
            c.stream_interrupted.fetch_add(1, relaxed);
        }
    }
    c.prev_delta = actual;
    c.have_prev_delta = true;
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
    if (c.restart_run.load(relaxed) && c.restart_run.exchange(false, relaxed)) {
        c.locked_run = 0;
        c.have_prev_delta = false;
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

void unlock_stream_input(StreamInputCounters& c) noexcept
{
    c.restart_run.store(true, std::memory_order_relaxed);
    if (c.is_locked.exchange(false, std::memory_order_relaxed)) {
        c.media_unlocked.fetch_add(1, std::memory_order_relaxed);
    }
}

namespace {

/// A locked input that received nothing since @p last_frames_rx has stopped: unlock it,
/// counting an interruption.
void stop_if_silent(StreamInputCounters& c, uint32_t const last_frames_rx) noexcept
{
    auto const relaxed = std::memory_order_relaxed;
    if (c.frames_rx.load(relaxed) != last_frames_rx || !c.is_locked.load(relaxed)) {
        return;
    }
    c.restart_run.store(true, relaxed);
    if (c.is_locked.exchange(false, relaxed)) {
        c.media_unlocked.fetch_add(1, relaxed);
        c.stream_interrupted.fetch_add(1, relaxed);
    }
}

/// Count one for each interval counter of @p c that moved, and bring the event counters up to date.
void fold_counters(StreamInputIntervals& in, StreamInputCounters const& c) noexcept
{  // NOLINT(statusbar-block-statements): one line per counter of Milan v1.2 Table 5.16
    using I = StreamInputIntervals;
    auto const relaxed = std::memory_order_relaxed;
    auto const interval = [&](I::Index const i, std::atomic<uint32_t> const& raw) {
        uint32_t const now = raw.load(relaxed);
        if (now != in.last[i]) {
            ++in.values[i];
            in.last[i] = now;
        }
    };
    interval(I::SEQ_NUM_MISMATCH, c.seq_num_mismatch);
    interval(I::MEDIA_RESET, c.media_reset);
    interval(I::TIMESTAMP_UNCERTAIN, c.timestamp_uncertain);
    interval(I::UNSUPPORTED_FORMAT, c.unsupported_format);
    interval(I::LATE_TIMESTAMP, c.late_timestamp);
    interval(I::EARLY_TIMESTAMP, c.early_timestamp);
    interval(I::FRAMES_RX, c.frames_rx);
    in.values[I::MEDIA_LOCKED] = c.media_locked.load(relaxed) - in.last[I::MEDIA_LOCKED];
    in.values[I::MEDIA_UNLOCKED] = c.media_unlocked.load(relaxed) - in.last[I::MEDIA_UNLOCKED];
    in.values[I::STREAM_INTERRUPTED] = c.stream_interrupted.load(relaxed) - in.last[I::STREAM_INTERRUPTED];
}

}  // namespace

auto StreamInputIntervals::fold(StreamInputCounters& c) noexcept -> bool
{
    // Nothing received in the interval while locked: the stream stopped.
    stop_if_silent(c, last[FRAMES_RX]);
    auto const before = values;
    fold_counters(*this, c);
    return values != before;
}

auto StreamOutputIntervals::set_streaming(bool const on, StreamOutputTotals const& totals) noexcept -> bool
{
    if (on == streaming) {
        return false;
    }
    streaming = on;
    if (on) {
        ++values[STREAM_START];
        values[MEDIA_RESET] = 0;
        values[TIMESTAMP_UNCERTAIN] = 0;
        values[FRAMES_TX] = 0;
        last = totals;
    } else {
        ++values[STREAM_STOP];
    }
    return true;
}

auto StreamOutputIntervals::fold(StreamOutputTotals const& totals) noexcept -> bool
{
    auto const before = values;
    if (totals.mr_toggles != last.mr_toggles) {
        ++values[MEDIA_RESET];
    }
    if (totals.tu_pdus != last.tu_pdus) {
        ++values[TIMESTAMP_UNCERTAIN];
    }
    if (totals.pdus != last.pdus) {
        ++values[FRAMES_TX];
    }
    last = totals;
    return fold_presence() || values != before;
}

void StreamOutputIntervals::narrow_presence(uint16_t const count) noexcept
{
    auto const latest = pending_presence ? pending_presence : presence;
    if (latest) {
        pending_presence = presence_of(*latest, count);
    }
}

auto StreamOutputIntervals::fold_presence() noexcept -> bool
{
    if (!pending_presence) {
        return false;  // nothing reported since the last fold: what is reported stands
    }
    bool const changed = pending_presence != presence;
    presence = std::exchange(pending_presence, std::nullopt);
    return changed;
}

void StreamInputIntervals::reset(StreamInputCounters& c) noexcept
{  // NOLINT(statusbar-block-statements): one line per counter of Milan v1.2 Table 5.16
    auto const relaxed = std::memory_order_relaxed;
    unlock_stream_input(c);
    values = {};
    last[MEDIA_LOCKED] = c.media_locked.load(relaxed);
    last[MEDIA_UNLOCKED] = c.media_unlocked.load(relaxed);
    last[STREAM_INTERRUPTED] = c.stream_interrupted.load(relaxed);
    last[SEQ_NUM_MISMATCH] = c.seq_num_mismatch.load(relaxed);
    last[MEDIA_RESET] = c.media_reset.load(relaxed);
    last[TIMESTAMP_UNCERTAIN] = c.timestamp_uncertain.load(relaxed);
    last[UNSUPPORTED_FORMAT] = c.unsupported_format.load(relaxed);
    last[LATE_TIMESTAMP] = c.late_timestamp.load(relaxed);
    last[EARLY_TIMESTAMP] = c.early_timestamp.load(relaxed);
    last[FRAMES_RX] = c.frames_rx.load(relaxed);
}

}  // namespace statusbar::avb_entity
