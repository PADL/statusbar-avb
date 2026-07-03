#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_talker_gate.hpp
/// @brief TalkerGate — the per-stream "should this talker transmit?" gate,
/// extracted from AvbEntityAudioIO (god-object phase 4).
///
/// Spec-correct SR-class admission, evaluated PER STREAM and independently of any
/// other stream (a CRF media-clock stream is a stream too — it must gate on its own
/// ACMP connection + reservation, never ride the audio streams' gate). A talker puts
/// stream `idx` on the wire only when all of its AVB preconditions hold:
///   1. an ACMP connection exists for that stream (a controller connected a listener),
///   2. the Talker Advertise attribute is declared — implied by (1)/(3): an ACMP
///      connection and a peer Listener Ready only arise after our Talker Advertise is
///      registered, so it needs no separate check,
///   3. the downstream listener permits transmit via MSRP Listener Ready (held across
///      an MRP LeaveAll re-registration blip by a grace window),
///   4. the stream is Started (defaults true; a hook to stop a stream without tearing
///      down its ACMP connection / reservation).
/// The gate is cross-thread: the MSRP listener-ready + stream-started state is written
/// by the reactor thread (note_listener_ready / note_stream_started) and read by the
/// media-timer thread (should_transmit), so those flags + the last-ready timestamps
/// are atomic. Reads ACMP connection state + the gate-enable flag through references
/// bound at construction.

#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/itc/itc_published.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace statusbar::avb_entity {

struct TalkerGate
{
    TalkerGate(AvbEntityAudioIOConfig const& config, nanoavb::NanoAvbComponents const& components) noexcept
        : config_{config}
        , components_{components}
    {
        // Stream Started defaults true: a stream is admitted once ACMP-connected +
        // Listener Ready unless explicitly stopped via note_stream_started().
        for (auto& started : stream_started_) {
            started.store(true, std::memory_order_relaxed);
        }
    }

    /// One per talker stream: 0=AM824, 1=AAF, 2=CRF.
    static constexpr size_t STREAM_COUNT = 3;
    /// Grace window the MSRP Listener Ready is held across a LeaveAll re-registration
    /// blip (Ready momentarily withdrawn for ~1-3 s on the ~10 s leave-all cycle), so
    /// a normal MRP cycle does NOT chop the stream. A genuine listener departure (no
    /// re-declare for GRACE) closes the gate.
    static constexpr int64_t GRACE_NS = 5'000'000'000;

    /// Reactor thread: an MSRP Listener Ready declaration for @p stream_id changed
    /// to @p ready. Matched to a talker stream index by ACMP stream_id (same value
    /// MSRP advertises); stamps the readiness flag + last-ready time.
    void note_listener_ready(nanoavb::StreamId const& stream_id, bool ready);

    /// Any thread: mark talker stream @p idx Started (@p started true) or Stopped.
    /// Stopped suppresses transmit without tearing down the ACMP connection or the
    /// reservation. Defaults Started; out-of-range indices are ignored.
    void note_stream_started(uint16_t idx, bool started) noexcept
    {
        if (idx < stream_started_.size()) {
            stream_started_[idx].store(started, std::memory_order_relaxed);
        }
    }

    /// Reactor thread: talker stream @p idx now has @p count ACMP connections
    /// (call from the acmp_talker connect/disconnect callback with the fresh
    /// acmp_talker.connection_count(idx)). Publishes it for the media-timer thread
    /// so should_transmit never reads the reactor-mutated connection list directly.
    void note_acmp_connections(uint16_t idx, uint32_t count) noexcept
    {
        if (idx < acmp_conn_.size()) {
            acmp_conn_[idx].publish(count);
        }
    }

    /// Media-timer thread: may talker stream @p idx put its stream on the wire at
    /// steady-clock time @p now_ns? True if gating is disabled, or ALL of this
    /// stream's own preconditions hold: an ACMP connection exists AND the stream is
    /// Started AND MSRP Listener Ready is set (or within GRACE_NS). Independent of
    /// every other stream.
    [[nodiscard]] auto should_transmit(uint16_t idx, int64_t now_ns) const noexcept -> bool;

    // --- References (bound at construction) ------------------------------------
    AvbEntityAudioIOConfig const& config_;
    nanoavb::NanoAvbComponents const& components_;  ///< reads acmp_talker connection state

    // --- Owned gate state (cross-thread) ---------------------------------------
    /// "A listener permits transmit" (MSRP Listener Ready), per stream. Written by
    /// note_listener_ready (reactor), read by should_transmit (media timer).
    std::array<std::atomic<bool>, STREAM_COUNT> listener_ready_{};
    /// Last steady-clock time (ns) each stream's Listener Ready was observed true;
    /// the strict gate keeps transmitting for GRACE_NS past this.
    std::array<std::atomic<int64_t>, STREAM_COUNT> ready_ns_{};
    /// Stream Started state, per stream (defaults true; see the constructor). A
    /// Stopped stream is not admitted even when connected + Listener Ready.
    std::array<std::atomic<bool>, STREAM_COUNT> stream_started_{};
    /// Per-stream ACMP connection count, published by note_acmp_connections
    /// (reactor) and read by should_transmit (media timer). Mirrors
    /// acmp_talker.connection_count(idx) so the media thread never reads the
    /// reactor-mutated connection list directly.
    std::array<itc::Published<uint32_t>, STREAM_COUNT> acmp_conn_{};
};

}  // namespace statusbar::avb_entity
