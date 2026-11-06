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
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace statusbar::avb_entity {

/// IEEE 1722.1 STREAM_INPUT counters (Clause 7.4.42) for incoming-stream
/// health, queryable via AECP GET_COUNTERS. Published values are atomic
/// (written on the RX thread, read by the AECP handler); the lock/sequence
/// detector state below is touched only on the RX thread, except is_locked,
/// which the reactor also clears (StreamInputIntervals::fold, unlock_stream_input).
struct StreamInputCounters
{
    std::atomic<uint32_t> media_locked{0};
    std::atomic<uint32_t> media_unlocked{0};
    /// Each loss of media lock other than an unbind (Milan v1.2 Table 5.6):
    /// a timestamp step out of tolerance (lost AVTPDUs, wrong timestamps), or
    /// the stream stopping while locked.
    std::atomic<uint32_t> stream_interrupted{0};
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
    std::atomic<bool> is_locked{false};
    /// Set by the reactor when it drops the lock: the RX thread starts its
    /// clean-step run again before it may lock anew.
    std::atomic<bool> restart_run{false};
};

/// Drop @p c's media lock without an interruption (an unbind, or a reset):
/// MEDIA_UNLOCKED counts it so MEDIA_LOCKED stays MEDIA_UNLOCKED or one more.
/// Reactor thread.
void unlock_stream_input(StreamInputCounters& c) noexcept;

/// Milan v1.2 Table 5.6 STREAM_INPUT counters from the per-packet ones: interval counters count
/// the observation intervals in which their per-packet counter moved; event counters (MEDIA_LOCKED,
/// MEDIA_UNLOCKED, STREAM_INTERRUPTED) are the per-packet ones since the last reset. Reactor thread.
struct StreamInputIntervals
{
    /// GET_COUNTERS values, indexed by bit value position (bit value 1 << i
    /// is values[i]; Milan v1.2 Table 5.16).
    enum Index : uint8_t
    {
        MEDIA_LOCKED = 0,
        MEDIA_UNLOCKED = 1,
        STREAM_INTERRUPTED = 2,
        SEQ_NUM_MISMATCH = 3,
        MEDIA_RESET = 4,
        TIMESTAMP_UNCERTAIN = 5,
        UNSUPPORTED_FORMAT = 8,
        LATE_TIMESTAMP = 9,
        EARLY_TIMESTAMP = 10,
        FRAMES_RX = 11,
    };
    /// The counters_valid bits Milan v1.2 Table 5.16 mandates.
    static constexpr uint32_t VALID = 0x0000'0F3FU;

    std::array<uint32_t, 12> values{};
    /// The per-packet counters at the last fold (interval counters) or reset
    /// (event counters), same indices.
    std::array<uint32_t, 12> last{};

    /// End one observation interval of @p c: a locked stream that received nothing has stopped,
    /// each interval counter whose per-packet counter moved counts one, and the event counters
    /// are brought up to date. True when a value changed.
    [[nodiscard]] auto fold(StreamInputCounters& c) noexcept -> bool;

    /// Zero every counter (Milan v1.2 Section 5.3.8.10: the input became
    /// bound), dropping any media lock first (unlock_stream_input).
    void reset(StreamInputCounters& c) noexcept;
};

/// What a STREAM_OUTPUT has transmitted, as running totals (TalkerTxCounters).
struct StreamOutputTotals
{
    uint32_t pdus{0};
    uint32_t tu_pdus{0};
    uint32_t mr_toggles{0};
};

/// Milan v1.3 Table 5.4 STREAM_OUTPUT counters: STREAM_START / STREAM_STOP count starts and stops;
/// MEDIA_RESET, TIMESTAMP_UNCERTAIN and FRAMES_TX count intervals with an mr toggle, tu set or any
/// AVTPDU, zeroed at each start; the optional signal presence (Section 5.3.7.7.1). Reactor thread only.
struct StreamOutputIntervals
{
    /// GET_COUNTERS values, indexed by bit value position in Milan v1.2 (Table 5.17).
    enum Index : uint8_t
    {
        STREAM_START = 0,
        STREAM_STOP = 1,
        MEDIA_RESET = 2,
        TIMESTAMP_UNCERTAIN = 3,
        FRAMES_TX = 4,
    };
    static constexpr uint32_t VALID = 0x0000'001FU;
    /// Each value's bit value position in Milan v1.3 (Table 5.14), IEEE 1722.1-2021's (Clause 7.4.42.2.5).
    static constexpr std::array<uint8_t, 5> V1_3_POSITIONS{0, 1, 3, 4, 7};
    /// ENTITY_SPECIFIC_9 and _10's positions: signal presence of channels 0-31 and 32-59 (Table 5.4).
    static constexpr uint8_t PRESENCE_0_31 = 23;
    static constexpr uint8_t PRESENCE_32_59 = 22;
    static constexpr size_t PRESENCE_CHANNELS = 60;

    std::array<uint32_t, 5> values{};
    StreamOutputTotals last{};
    bool streaming{false};
    /// Signal presence, bit k channel k: reported (once monitored), and what set_presence or
    /// narrow_presence gave since the last fold.
    std::optional<uint64_t> presence{};
    std::optional<uint64_t> pending_presence{};

    /// The latest signal presence of the output's channels, bit k channel k (the stream's channels
    /// alone, presence_of): reported at the next fold.
    void set_presence(uint64_t const channels) noexcept { pending_presence = channels; }
    /// The stream's format now has @p count channels: what is reported or pending is cut to them,
    /// reported at the next fold (Milan v1.3 Section 5.3.7.7.1: the channels the stream has).
    void narrow_presence(uint16_t count) noexcept;

    /// The talker is (@p on) or is not streaming, @p totals sent so far. True
    /// when that starts or stops it.
    [[nodiscard]] auto set_streaming(bool on, StreamOutputTotals const& totals) noexcept -> bool;

    /// End one observation interval at @p totals, taking the latest presence. True when a value changed.
    [[nodiscard]] auto fold(StreamOutputTotals const& totals) noexcept -> bool;
    /// Report what set_presence or narrow_presence gave since the last fold; true when that changed what is reported.
    [[nodiscard]] auto fold_presence() noexcept -> bool;
};

/// Signal presence @p channels (bit k channel k) with every bit past the stream's @p count clear:
/// Milan v1.3 Section 5.3.7.7.1, the channels the stream has, as its format is now.
[[nodiscard]] constexpr auto presence_of(uint64_t const channels, uint16_t const count) noexcept -> uint64_t
{
    return count >= 64 ? channels : channels & ((uint64_t{1} << count) - 1);
}

/// ENTITY_SPECIFIC_9 (@p first 0) or _10 (@p first 32) of signal presence @p channels: channel
/// first + i at bit i counted from the MSB, channels past 59 clear (Milan v1.3 Figures 5.2-5.3).
[[nodiscard]] constexpr auto presence_quadlet(uint64_t const channels, size_t const first) noexcept -> uint32_t
{
    uint32_t quadlet = 0;
    for (size_t i = 0; i < 32 && first + i < StreamOutputIntervals::PRESENCE_CHANNELS; ++i) {
        if (((channels >> (first + i)) & 1U) != 0) {
            quadlet |= 0x8000'0000U >> i;
        }
    }
    return quadlet;
}

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
