#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// TalkerGate: the per-stream "should this talker transmit?" gate (Milan v1.2 Sections 5.3.7.3,
/// 5.5.2.5). Written by the reactor thread, read by the media-timer thread (should_transmit), so
/// its state is atomic. See docs/MILAN_TALKER.md.

#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace statusbar::avb_entity {

struct TalkerGate
{
    TalkerGate(bool gate_enabled, nanoavb::NanoAvbComponents const& components) noexcept
        : gate_enabled_{gate_enabled}
        , components_{components}
    {
        // Stream Started defaults true: a stream is admitted once a listener's
        // Ready is registered unless explicitly stopped via note_stream_started().
        for (auto& started : stream_started_) {
            started.store(true, std::memory_order_relaxed);
        }
    }

    /// Install the reactor-thread logger for gate-transition status lines.
    void set_logger(logging::Logger const log) noexcept { logger_ = log; }

    /// One per talker stream (STREAM_OUTPUT descriptor index); sized for the
    /// data plane's maximum blob-declared stream table (kit phase 1).
    static constexpr size_t STREAM_COUNT = MAX_ENTITY_STREAMS;
    /// How long the gate stays open after the listener declaration was last seen registered, so
    /// an MRP LeaveAll re-registration blip does not chop the stream.
    static constexpr int64_t GRACE_NS = 5'000'000'000;

    /// Reactor thread: whether a Listener declaration permitting transmission (Ready or Ready
    /// Failed) is registered for @p stream_id at steady-clock @p now_ns. Registration and
    /// withdrawal both stamp the last-ready time, from which GRACE_NS runs.
    void note_listener_ready(nanoavb::StreamId const& stream_id, bool ready, int64_t now_ns);

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

    /// Media-timer thread: may talker stream @p idx transmit at steady-clock @p now_ns? Exactly
    /// admitted(), independent of every other stream.
    [[nodiscard]] auto should_transmit(uint16_t idx, int64_t now_ns) const noexcept -> bool { return admitted(idx, now_ns); }

    // --- References / values (bound at construction) ---------------------------
    bool gate_enabled_;                             ///< config.gate_talker_on_listener, destructured at the call site
    nanoavb::NanoAvbComponents const& components_;  ///< maps stream_id -> talker stream index (acmp_talker)
    std::optional<logging::Logger> logger_{};       ///< reactor-thread gate-transition logging

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
};

}  // namespace statusbar::avb_entity
