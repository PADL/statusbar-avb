#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// TalkerGate: the per-stream "should this talker transmit?" gate (Milan v1.2 Sections 5.3.7.3,
/// 5.5.2.5). The reactor stamps each stream's Listener Ready on CLOCK_MONOTONIC_RAW; any thread
/// told a RAW instant reads the same predicate from it. See docs/MILAN_TALKER.md.

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
    /// The gate's clock: CLOCK_MONOTONIC_RAW, the clock a media wake is told on (net::rx_clock_now()
    /// reads it elsewhere). The reactor reads it once per event it stamps.
    using RawTimePoint = realtime::MonotonicRawClock::time_point;

    /// A gate whose note_listener_ready() finds streams by @p stream_index (reactor thread); with
    /// @p gate_enabled false every stream is admitted. A departed Listener admits for @p grace.
    TalkerGate(bool gate_enabled, StreamIndexFn stream_index, std::chrono::nanoseconds grace = GRACE) noexcept;
    /// The kit's gate: @p components' ACMP talker streams, which must outlive it; MILAN_GRACE
    /// for a Milan entity, GRACE for a classic one.
    TalkerGate(bool gate_enabled, nanoavb::NanoAvbComponents const& components) noexcept;

    /// Install the reactor-thread logger for gate-transition status lines.
    void set_logger(logging::Logger const log) noexcept { logger_ = log; }

    /// One per talker stream (STREAM_OUTPUT descriptor index); sized for the
    /// data plane's maximum blob-declared stream table (kit phase 1).
    static constexpr size_t STREAM_COUNT = MAX_ENTITY_STREAMS;
    /// How long the gate stays open after the listener declaration was last seen registered, so
    /// an MRP LeaveAll re-registration blip does not chop the stream.
    static constexpr auto GRACE = std::chrono::seconds{5};
    /// A Milan entity's grace: none. Its MSRP registrar holds a Listener through a LeaveAll for
    /// LeaveTime and drops one at its Lv (Milan v1.3 Section 4.2.7.2.2).
    static constexpr auto MILAN_GRACE = std::chrono::nanoseconds{0};

    /// Reactor thread: whether a Listener declaration permitting transmission (Ready or Ready
    /// Failed) is registered for @p stream_id at @p now. Registration and withdrawal both stamp
    /// the last-ready instant, from which the grace runs.
    void note_listener_ready(tsn::StreamId const& stream_id, bool ready, RawTimePoint now);
    /// As above, for a caller with no RAW reading in hand: reads the clock once.
    void note_listener_ready(tsn::StreamId const& stream_id, bool ready);

    /// Reactor thread: our Talker declarations were withdrawn. Every stream closes at once (no
    /// grace) and forgets its listener state until note_listener_ready re-learns it.
    void notify_talker_withdrawn() noexcept;
    /// Reactor thread: stream @p idx's Talker Advertise alone was withdrawn (a
    /// Milan talker's declaration window closed, or MAAP lost its address):
    /// that stream closes at once, as above.
    void notify_talker_withdrawn(uint16_t idx) noexcept;

    /// Any thread: mark talker stream @p idx Started (@p started true) or Stopped.
    /// Stopped suppresses transmit without tearing down the reservation.
    /// Defaults Started; out-of-range indices are ignored.
    void note_stream_started(uint16_t idx, bool started) noexcept
    {
        if (idx < stream_started_.size()) {
            stream_started_[idx].store(started, std::memory_order_relaxed);
        }
    }

    /// Any thread: is talker stream @p idx admitted at @p now? True if gating is disabled, or
    /// the stream is Started and a permitting listener declaration is registered now or was
    /// within the grace. Time-dependent: the kit re-evaluates it at grace_ends() (AvbEntityKit::on_admission_timer).
    [[nodiscard]] auto admitted(uint16_t idx, RawTimePoint now) const noexcept -> bool;

    /// Whether stream @p idx has a permitting listener declaration now or within the grace,
    /// regardless of gate_enabled_ and Started (Milan v1.2 Section 5.3.7.3).
    [[nodiscard]] auto listener_admitted(uint16_t idx, RawTimePoint now) const noexcept -> bool;

    /// When stream @p idx's grace runs out: its last-ready stamp plus the grace, nullopt with no stamp
    /// (never registered, or forgotten by a withdrawal). The declaration may still be registered.
    [[nodiscard]] auto grace_ends(uint16_t idx) const noexcept -> std::optional<RawTimePoint>;

    /// Media-timer thread: may talker stream @p idx transmit at the wake told as @p now? Exactly
    /// admitted(), independent of every other stream.
    [[nodiscard]] auto should_transmit(uint16_t const idx, RawTimePoint const now) const noexcept -> bool
    {
        return admitted(idx, now);
    }

    // --- References / values (bound at construction) ---------------------------
    bool gate_enabled_;                        ///< config.gate_talker_on_listener, destructured at the call site
    StreamIndexFn stream_index_;               ///< maps stream_id -> talker stream index
    std::chrono::nanoseconds grace_;           ///< how long a departed Listener still admits
    std::optional<logging::Logger> logger_{};  ///< reactor-thread gate-transition logging

  private:
    /// Reactor thread: stream @p idx's stamp and flag for a Listener @p ready at @p now; the flag before.
    auto stamp(uint16_t idx, bool ready, RawTimePoint now) noexcept -> bool;

    /// A listener declaration permitting transmission is registered, per stream. Written by
    /// note_listener_ready (reactor, after the stamp, release), read by admitted (acquire).
    std::array<std::atomic<bool>, STREAM_COUNT> listener_ready_{};
    /// The RAW instant (ns) each stream's listener declaration was last known registered
    /// (0 = never, or forgotten by notify_talker_withdrawn); the gate stays open for the grace past it.
    std::array<std::atomic<int64_t>, STREAM_COUNT> ready_raw_ns_{};
    /// Stream Started state, per stream (defaults true; see the constructor). A
    /// Stopped stream is not admitted even with Listener Ready registered.
    std::array<std::atomic<bool>, STREAM_COUNT> stream_started_{};
};

}  // namespace statusbar::avb_entity
