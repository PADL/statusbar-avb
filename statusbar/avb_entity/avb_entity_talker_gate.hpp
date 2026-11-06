#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// TalkerGate: the per-stream "should this talker transmit?" gate (Milan v1.2 Sections 5.3.7.3,
/// 5.5.2.5). Written by the reactor thread, read by the media-timer thread (should_transmit), so
/// its shared state is atomic. Each side times the grace window on the clock it is told.

#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/realtime/realtime_clock.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/tsn/tsn_stream_id.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace statusbar::nanoavb {
struct NanoAvbComponents;
}  // namespace statusbar::nanoavb

namespace statusbar::avb_entity {

struct TalkerGate
{
    /// The talker stream index (below STREAM_COUNT) a StreamID is declared on, or nullopt.
    using StreamIndexFn = sg14::inplace_function<std::optional<uint16_t>(tsn::StreamId const&), 32>;

    /// A gate whose note_listener_ready() finds streams by @p stream_index (reactor thread); with
    /// @p gate_enabled false every stream is admitted.
    TalkerGate(bool gate_enabled, StreamIndexFn stream_index) noexcept;
    /// The kit's gate: @p components' ACMP talker streams, which must outlive it.
    TalkerGate(bool gate_enabled, nanoavb::NanoAvbComponents const& components) noexcept;

    /// Install the reactor-thread logger for gate-transition status lines.
    void set_logger(logging::Logger const log) noexcept { logger_ = log; }

    /// One per talker stream (STREAM_OUTPUT descriptor index); sized for the
    /// data plane's maximum blob-declared stream table (kit phase 1).
    static constexpr size_t STREAM_COUNT = MAX_ENTITY_STREAMS;
    /// How long the gate stays open after the listener declaration was last seen registered, so
    /// an MRP LeaveAll re-registration blip does not chop the stream.
    static constexpr auto GRACE = std::chrono::seconds{5};
    static constexpr int64_t GRACE_NS = std::chrono::nanoseconds{GRACE}.count();
    /// The media wake's instant on CLOCK_MONOTONIC_RAW, the clock should_transmit is told.
    using RawTimePoint = realtime::MonotonicRawClock::time_point;
    /// "Never seen Listener Ready" on the media side: the grace window has nothing to run from.
    static constexpr RawTimePoint NEVER = RawTimePoint::min();

    /// Reactor thread: whether a Listener declaration permitting transmission (Ready or Ready
    /// Failed) is registered for @p stream_id at steady-clock @p now_ns. Registration and
    /// withdrawal both stamp the last-ready time, from which GRACE_NS runs.
    void note_listener_ready(tsn::StreamId const& stream_id, bool ready, int64_t now_ns);
    /// As above, at the steady clock's now.
    void note_listener_ready(tsn::StreamId const& stream_id, bool ready)
    {
        note_listener_ready(
            stream_id,
            ready,
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    /// Reactor thread: our Talker declarations were withdrawn. Every stream closes at once (no
    /// grace) and forgets its listener state until note_listener_ready re-learns it.
    void note_talker_withdrawn() noexcept;
    /// Reactor thread: stream @p idx's Talker Advertise alone was withdrawn (a
    /// Milan talker's declaration window closed, or MAAP lost its address):
    /// that stream closes at once, as above.
    void note_talker_withdrawn(uint16_t idx) noexcept;

    /// Any thread: mark talker stream @p idx Started (@p started true) or Stopped.
    /// Stopped suppresses transmit without tearing down the reservation.
    /// Defaults Started; out-of-range indices are ignored.
    void note_stream_started(uint16_t idx, bool started) noexcept
    {
        if (idx < stream_started_.size()) {
            stream_started_[idx].store(started, std::memory_order_relaxed);
        }
    }

    /// Any thread: is talker stream @p idx admitted at steady-clock @p now_ns? True if gating is
    /// disabled, or the stream is Started and a permitting listener declaration is registered now
    /// or was within GRACE_NS. Time-dependent: the reactor re-evaluates it (poll_admission).
    [[nodiscard]] auto admitted(uint16_t idx, int64_t now_ns) const noexcept -> bool;

    /// Whether stream @p idx has a permitting listener declaration now or within GRACE_NS,
    /// regardless of gate_enabled_ and Started (Milan v1.2 Section 5.3.7.3).
    [[nodiscard]] auto listener_admitted(uint16_t idx, int64_t now_ns) const noexcept -> bool;

    /// Media-timer thread: may talker stream @p idx transmit at the wake told as @p now
    /// (CLOCK_MONOTONIC_RAW)? admitted()'s predicate with the grace window run from the wake at
    /// which this thread last saw the declaration; one thread calls it.
    [[nodiscard]] auto should_transmit(uint16_t idx, RawTimePoint now) noexcept -> bool;

    // --- References / values (bound at construction) ---------------------------
    bool gate_enabled_;                        ///< config.gate_talker_on_listener, destructured at the call site
    StreamIndexFn stream_index_;               ///< maps stream_id -> talker stream index
    std::optional<logging::Logger> logger_{};  ///< reactor-thread gate-transition logging

    // --- Owned gate state (cross-thread) ---------------------------------------
    /// A listener declaration permitting transmission is registered, per stream.
    /// Written by note_listener_ready (reactor), read by admitted (any thread).
    std::array<std::atomic<bool>, STREAM_COUNT> listener_ready_{};
    /// Last steady-clock time (ns) each stream's listener declaration was known
    /// registered (0 = never, or forgotten by note_talker_withdrawn); the gate
    /// stays open for GRACE_NS past this.
    std::array<std::atomic<int64_t>, STREAM_COUNT> ready_ns_{};
    /// Stream Started state, per stream (defaults true; see the constructor). A
    /// Stopped stream is not admitted even with Listener Ready registered.
    std::array<std::atomic<bool>, STREAM_COUNT> stream_started_{};

  private:
    /// Per stream, counted by the reactor: Listener Ready registrations, so the media side opens
    /// its grace on one it did not see set, and Talker withdrawals, on which it forgets it.
    std::array<std::atomic<uint32_t>, STREAM_COUNT> readies_{};
    std::array<std::atomic<uint32_t>, STREAM_COUNT> withdrawals_{};

    [[nodiscard]] static constexpr auto never_seen() noexcept -> std::array<RawTimePoint, STREAM_COUNT>
    {
        std::array<RawTimePoint, STREAM_COUNT> seen{};
        seen.fill(NEVER);
        return seen;
    }
    /// Reactor thread: count a Ready for the media side and stamp the reactor's grace.
    void stamp_ready(uint16_t idx, bool ready, bool was, int64_t now_ns) noexcept;
    /// Take in the Readies and withdrawals the reactor counted since the last wake. Media-timer thread.
    void catch_up(uint16_t idx, RawTimePoint now) noexcept;
    /// Listener Ready now, or seen within GRACE of @p now (recorded here). Media-timer thread.
    [[nodiscard]] auto listener_ready_within_grace(uint16_t idx, RawTimePoint now) noexcept -> bool;

    /// Media-timer thread only: the wake each stream's declaration was last seen at, and the
    /// readies_ and withdrawals_ counts it was seen under.
    std::array<RawTimePoint, STREAM_COUNT> last_seen_ready_ = never_seen();
    std::array<uint32_t, STREAM_COUNT> seen_readies_{};
    std::array<uint32_t, STREAM_COUNT> seen_withdrawals_{};
};

}  // namespace statusbar::avb_entity
