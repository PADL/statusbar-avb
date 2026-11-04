#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_stream_counters.hpp
/// @brief IEEE 1722.1 STREAM_INPUT health counters + the per-packet update.
///
/// Pulled out of AvbEntityAudioIO so the counter bookkeeping (and its lock /
/// timestamp-cadence logic) is unit-testable without standing up a full entity.

#include "statusbar/stats/stats_atomic_histogram.hpp"
#include "statusbar/stats/stats_atomic_time_stats.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

namespace statusbar::avb_entity {

/// IEEE 1722.1 STREAM_INPUT counters (Clause 7.4.42) for incoming-stream
/// health, queryable via AECP GET_COUNTERS. Published values are atomic
/// (written on the RX thread, read by the AECP handler); the lock/sequence
/// detector state below is touched only on the RX thread.
struct StreamInputCounters
{
    std::atomic<uint32_t> media_locked{0};
    std::atomic<uint32_t> media_unlocked{0};
    std::atomic<uint32_t> seq_num_mismatch{0};
    std::atomic<uint32_t> media_reset{0};
    std::atomic<uint32_t> timestamp_uncertain{0};
    std::atomic<uint32_t> timestamp_valid{0};
    std::atomic<uint32_t> timestamp_not_valid{0};
    std::atomic<uint32_t> unsupported_format{0};
    std::atomic<uint32_t> late_timestamp{0};
    std::atomic<uint32_t> early_timestamp{0};
    std::atomic<uint32_t> frames_rx{0};

    /// How much of the presentation offset was left when the listener examined
    /// each timestamped frame: avtp_timestamp minus the listener's gPTP now, in
    /// ns. Positive = time to spare; negative = LATE_TIMESTAMP, and by how much.
    /// Every tv=1 frame when gPTP now is known. Lock-free (written on the RX
    /// thread, read for the exit report). The histogram answers "how late" and
    /// "how much margin" at once: see margin_histogram_config_for().
    stats::AtomicTimeStats margin{};
    /// The late frames alone: how far past presentation each one was, in ns (>0).
    stats::AtomicTimeStats lateness{};
    stats::AtomicHistogram<128> margin_histogram{margin_histogram_config_for(DEFAULT_PRESENTATION_OFFSET_NS)};

    static constexpr uint64_t DEFAULT_PRESENTATION_OFFSET_NS = 1'000'000;

    /// The margin histogram's range for a talker presenting @p
    /// presentation_offset_ns ahead: [-offset/2, +1.5 offset) in 128 bins, so
    /// a healthy stream sits in the upper half and a late frame is resolved to
    /// a 64th of the offset (15.6 us at 1 ms). Both ends of a statusbar link
    /// use the same offset, so the listener's own value is the right guess.
    [[nodiscard]] static constexpr auto margin_histogram_config_for(uint64_t const presentation_offset_ns) noexcept
        -> stats::AtomicHistogramConfig
    {
        auto const offset = static_cast<int64_t>(presentation_offset_ns);
        int64_t const bin = std::max<int64_t>(1, (2 * offset) / 128);
        return stats::AtomicHistogramConfig{.low_ns = -offset / 2, .high_ns = (-offset / 2) + (128 * bin), .bin_width_ns = bin};
    }

    bool have_prev{false};
    bool have_prev_ts{false};
    uint8_t prev_seq{0};
    bool prev_mr{false};
    uint32_t prev_ts{0};
    // Previous inter-(valid-)timestamp step, for constant-step media lock. AAF
    // steps 125 us/packet; AM824's avtp_timestamp follows the 61883-6 SYT
    // cadence (SYT_INTERVAL=16 @ 96 kHz -> 166.67 us between valid stamps, on 3
    // of every 4 packets), so we lock on a STEADY step, not a hardcoded value.
    uint32_t prev_delta{0};
    bool have_prev_delta{false};
    int locked_run{0};
    bool is_locked{false};
};

/// Update one StreamInputCounters from a received stream packet.
///
/// Pure w.r.t. entity state: the gPTP-now reference, the media-lock tolerance,
/// and the nominal sample rate are passed in. `gptp_now_ns` may be 0 to skip
/// LATE/EARLY detection.
///
/// @p ts_sparse marks a format that legitimately omits the AVTP timestamp on
/// some packets: IEC 61883-6 (AM824) carries a valid timestamp only on the
/// SYT-bearing packets (SYT_INTERVAL cadence), so tv=0 in between is NORMAL,
/// not an anomaly. When set, those tv=0 packets are counted in neither the
/// TIMESTAMP_VALID nor the TIMESTAMP_NOT_VALID bucket. For AAF (ts_sparse
/// false) every packet carries a timestamp, so tv=0 is a real fault and is
/// tallied as TIMESTAMP_NOT_VALID.
void tally_stream_input_packet(
    StreamInputCounters& c,
    uint8_t seq,
    uint32_t avtp_ts,
    bool tv,
    bool tu,
    bool mr,
    bool format_ok,
    uint64_t samples_per_ch,
    bool ts_sparse,
    int64_t gptp_now_ns,
    uint32_t lock_tolerance_ns,
    uint32_t sample_rate_hz);

/// Append the presentation-margin report for one STREAM_INPUT to @p out:
/// late/early counts, margin and lateness statistics, and the margin histogram
/// as an ASCII bar chart. Nothing is appended when no frame carried a usable
/// timestamp. Exit-time reporting; reads the atomics relaxed.
void format_stream_input_timing_to(std::string& out, uint16_t stream_index, StreamInputCounters const& c);

}  // namespace statusbar::avb_entity
