// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_stream_counters.hpp"

namespace statusbar::avb_entity {

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
    uint64_t const gptp_now_ns,
    uint32_t const lock_tolerance_ns,
    uint32_t const sample_rate_hz)
{
    auto const relaxed = std::memory_order_relaxed;
    if (c.restart_run.load(relaxed) && c.restart_run.exchange(false, relaxed)) {
        c.locked_run = 0;
        c.have_prev_delta = false;
    }
    c.frames_rx.fetch_add(1, relaxed);
    if (!format_ok) {
        c.unsupported_format.fetch_add(1, relaxed);
        return;
    }

    // Sequence-number continuity (8-bit wrap).
    if (c.have_prev && seq != static_cast<uint8_t>(c.prev_seq + 1U)) {
        c.seq_num_mismatch.fetch_add(1, relaxed);
    }
    // Media clock restart: the talker toggles mr when its media clock resets.
    if (c.have_prev && mr != c.prev_mr) {
        c.media_reset.fetch_add(1, relaxed);
    }

    if (tv) {
        c.timestamp_valid.fetch_add(1, relaxed);
        if (tu) {
            c.timestamp_uncertain.fetch_add(1, relaxed);
        }
        // LATE/EARLY vs gPTP-now (lower 32 bits of ns). The reactor clock is
        // monotonic, so the caller passes last_gptp_ns_ published by the media
        // timer (<=125us stale, fine against ms-scale thresholds). diff<0 =>
        // presentation time already passed (late); diff implausibly large =>
        // stamped too early. gptp_now_ns==0 means "unknown" -> skip.
        constexpr uint32_t EARLY_THRESHOLD_NS = 50'000'000;  // 50 ms
        if (gptp_now_ns != 0) {
            int32_t const diff = static_cast<int32_t>(avtp_ts - static_cast<uint32_t>(gptp_now_ns));
            if (diff < 0) {
                c.late_timestamp.fetch_add(1, relaxed);
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
        if (c.have_prev_ts && samples_per_ch > 0) {
            uint32_t const actual = avtp_ts - c.prev_ts;                                    // wrap-safe (uint32)
            uint64_t const nominal = (samples_per_ch * 1'000'000'000ULL) / sample_rate_hz;  // 125 us @ 12/96k
            bool const plausible = (actual >= (nominal / 2)) && (actual < (nominal * 4));
            uint64_t const step_err = (actual > c.prev_delta) ? (actual - c.prev_delta) : (c.prev_delta - actual);
            if (plausible && c.have_prev_delta && step_err <= lock_tolerance_ns) {
                if (c.locked_run < LOCK_RUN_THRESHOLD) {
                    ++c.locked_run;
                }
                if (c.locked_run >= LOCK_RUN_THRESHOLD && !c.is_locked.load(relaxed) && !c.is_locked.exchange(true, relaxed)) {
                    c.media_locked.fetch_add(1, relaxed);
                }
            } else {
                c.locked_run = 0;
                // The reactor may have dropped the lock already (fold, unbind):
                // the exchange counts each loss once.
                if (c.is_locked.load(relaxed) && c.is_locked.exchange(false, relaxed)) {
                    c.media_unlocked.fetch_add(1, relaxed);
                    c.stream_interrupted.fetch_add(1, relaxed);
                }
            }
            c.prev_delta = actual;
            c.have_prev_delta = true;
        }
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

    c.prev_seq = seq;
    c.prev_mr = mr;
    c.have_prev = true;
}

void unlock_stream_input(StreamInputCounters& c) noexcept
{
    c.restart_run.store(true, std::memory_order_relaxed);
    if (c.is_locked.exchange(false, std::memory_order_relaxed)) {
        c.media_unlocked.fetch_add(1, std::memory_order_relaxed);
    }
}

auto StreamInputIntervals::fold(StreamInputCounters& c) noexcept -> bool
{
    auto const relaxed = std::memory_order_relaxed;
    // Nothing received in the interval while locked: the stream stopped.
    if (c.frames_rx.load(relaxed) == last[FRAMES_RX] && c.is_locked.load(relaxed)) {
        c.restart_run.store(true, relaxed);
        if (c.is_locked.exchange(false, relaxed)) {
            c.media_unlocked.fetch_add(1, relaxed);
            c.stream_interrupted.fetch_add(1, relaxed);
        }
    }
    auto const before = values;
    auto const interval = [&](Index const i, std::atomic<uint32_t> const& raw) {
        uint32_t const now = raw.load(relaxed);
        if (now != last[i]) {
            ++values[i];
            last[i] = now;
        }
    };
    interval(SEQ_NUM_MISMATCH, c.seq_num_mismatch);
    interval(MEDIA_RESET, c.media_reset);
    interval(TIMESTAMP_UNCERTAIN, c.timestamp_uncertain);
    interval(UNSUPPORTED_FORMAT, c.unsupported_format);
    interval(LATE_TIMESTAMP, c.late_timestamp);
    interval(EARLY_TIMESTAMP, c.early_timestamp);
    interval(FRAMES_RX, c.frames_rx);
    values[MEDIA_LOCKED] = c.media_locked.load(relaxed) - last[MEDIA_LOCKED];
    values[MEDIA_UNLOCKED] = c.media_unlocked.load(relaxed) - last[MEDIA_UNLOCKED];
    values[STREAM_INTERRUPTED] = c.stream_interrupted.load(relaxed) - last[STREAM_INTERRUPTED];
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
    return values != before;
}

void StreamInputIntervals::reset(StreamInputCounters& c) noexcept
{
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
