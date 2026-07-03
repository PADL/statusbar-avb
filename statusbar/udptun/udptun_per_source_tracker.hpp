#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc.hpp"
#include "statusbar/stats/stats_atomic_histogram.hpp"
#include "statusbar/udptun/udptun_stats.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace statusbar::udptun {

/// Per-source slot. observe() mutates it on the RT drain thread while the reporter
/// thread reads it, so every reporter-visible field is an itc primitive (no bare
/// atomics): TelemetryCounter for the monotonic counts, Published for the
/// point-in-time state. Fields only the RT thread ever reads (LRU bookkeeping)
/// stay plain. in_use is published LAST in init_state so its release publish makes
/// a freshly-initialised slot fully visible to a reader that saw in_use == true.
struct SourceState
{
    itc::Published<uint64_t> sender_eui64{};  // packed Eui64 (to_uint64); reporter + RT find_existing
    std::unique_ptr<LatencyStats> stats{};    // LatencyStats holds atomics (non-movable)
    itc::Published<uint32_t> first_seq{};
    itc::Published<uint32_t> last_seq{};
    int64_t first_seen_gptp_ns{0};    // RT-only (LRU); reporter never reads
    int64_t last_seen_gptp_ns{0};     // RT-only (LRU)
    uint32_t announced_interval_us{0};  // RT-only
    itc::TelemetryCounter<uint64_t> received_count{};
    itc::TelemetryCounter<uint64_t> out_of_order_count{};
    itc::TelemetryCounter<uint64_t> duplicate_count{};
    itc::Published<bool> in_use{};
};

/// Per-source latency tracker with capped storage and LRU eviction.
class PerSourceTracker
{
  public:
    PerSourceTracker(statusbar::stats::AtomicHistogramConfig const& cfg, size_t max_sources);

    /// Returns the SourceState for this sender (creates if new, evicts
    /// LRU by last_seen_gptp_ns if at capacity). Records `latency_ns`
    /// (the caller-supplied real one-way transit, *not* lateness vs
    /// the wire deadline) into the source's stats. `rx_gptp_ns` is used
    /// for last-seen / LRU bookkeeping. Returns nullptr only if
    /// max_sources == 0.
    auto observe(ieee::Eui64 sender, uint32_t seq, int64_t latency_ns, int64_t rx_gptp_ns, uint32_t interval_us) noexcept
        -> SourceState*;

    /// Returns all slots — callers filter on `s.in_use`. Used by report
    /// formatters which iterate active sources.
    [[nodiscard]] auto sources() const -> std::span<SourceState const>;

    [[nodiscard]] auto dropped_invalid() const noexcept -> uint64_t { return dropped_invalid_.load(); }
    void increment_dropped_invalid() noexcept { dropped_invalid_.add(1); }

    /// Truncated datagrams: recvfrom reported a datagram larger than the
    /// drain buffer, so the application-visible bytes are an incomplete
    /// prefix and the codec can't decode them safely. Surfaces a wiring
    /// problem (peer sending jumbo frames at a buffer sized for standard
    /// MTU, or a path-MTU mismatch) rather than a decode-level failure.
    [[nodiscard]] auto truncated() const noexcept -> uint64_t { return truncated_.load(); }
    void increment_truncated() noexcept { truncated_.add(1); }

  private:
    [[nodiscard]] auto find_existing(ieee::Eui64 const& s) noexcept -> SourceState*;
    [[nodiscard]] auto find_free_or_lru() noexcept -> SourceState*;

    statusbar::stats::AtomicHistogramConfig hist_cfg_;
    std::vector<SourceState> slots_;
    itc::TelemetryCounter<uint64_t> dropped_invalid_{};
    itc::TelemetryCounter<uint64_t> truncated_{};
};

}  // namespace statusbar::udptun
