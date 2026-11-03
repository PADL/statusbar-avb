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
/// The gate is cross-thread: the reactor thread writes the ACMP and MSRP Listener
/// state, and stamps each stream's Listener Ready on CLOCK_MONOTONIC_RAW
/// (note_listener_ready); any thread told a RAW instant reads the same predicate
/// from that stamp (admitted, should_transmit), so the media thread and the
/// reactor agree on when a grace runs out. The gate reads no clock: every
/// instant is told it, by the caller or by the loop that drives the reactor
/// (tell_time). Reads the stream ids through the components bound at
/// construction.

#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/itc/itc_published.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/realtime/realtime_clock.hpp"

#include <array>
#include <atomic>
#include <chrono>
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
        // Stream Started defaults true: a stream is admitted once ACMP-connected +
        // Listener Ready unless explicitly stopped via note_stream_started().
        for (auto& started : stream_started_) {
            started.store(true, std::memory_order_relaxed);
        }
    }

    /// Install the reactor-thread logger for gate-transition status lines.
    void set_logger(logging::Logger const log) noexcept { logger_ = log; }

    /// Whether a stream needs an ACMP connection to be admitted (the
    /// default, IEEE 1722.1). A Milan talker keeps no connections and
    /// streams "as long as bandwidth is reserved" (Milan v1.3 5.5.4.1): for
    /// it the gate is Started and MSRP Listener Ready alone. Set before the
    /// media timer runs.
    void set_needs_acmp_connection(bool const needed) noexcept { needs_acmp_connection_ = needed; }

    /// How long a stream stays admitted after its Listener Ready was
    /// withdrawn (GRACE by default). A Milan talker sets none: nothing but the
    /// Listener's registration admits its stream, the registrar keeps that
    /// registration through a LeaveAll for LeaveTime (5 s under Milan's
    /// timers), and a grace on top would keep frames going out after the
    /// last listener left and the Talker attribute was withdrawn. Set before
    /// the media timer runs.
    void set_listener_grace(std::chrono::nanoseconds const grace) noexcept { grace_ = grace; }

    /// One per talker stream (STREAM_OUTPUT descriptor index); sized for the
    /// data plane's maximum blob-declared stream table (kit phase 1).
    static constexpr size_t STREAM_COUNT = MAX_ENTITY_STREAMS;
    /// The gate's clock, CLOCK_MONOTONIC_RAW: a media wake is told it, and the
    /// reactor thread is told it once per reactor cycle (tell_time).
    using RawTimePoint = realtime::MonotonicRawClock::time_point;
    /// Grace window the MSRP Listener Ready is held across a LeaveAll re-registration
    /// blip (Ready momentarily withdrawn for ~1-3 s on the ~10 s leave-all cycle), so
    /// a normal MRP cycle does NOT chop the stream. A genuine listener departure (no
    /// re-declare for GRACE) closes the gate.
    static constexpr auto GRACE = std::chrono::seconds{5};

    /// Reactor thread: an MSRP Listener Ready declaration for @p stream_id changed
    /// to @p ready. Matched to a talker stream index by ACMP stream_id (same value
    /// MSRP advertises); sets the readiness flag and, while the stream has an
    /// ACMP connection, the admitted() latch. Needs no time: the grace window
    /// runs from when the media thread last saw the flag (should_transmit).
    void note_listener_ready(nanoavb::StreamId const& stream_id, bool ready);

    /// Reactor thread: the RAW instant of the reactor cycle now being served,
    /// read by the loop when the reactor read its own clock and before any
    /// handler of the cycle ran (entity_reactor_clock). Every Listener Ready
    /// noted without an instant in this cycle is stamped with it.
    void tell_time(RawTimePoint const now) noexcept { told_now_ = now; }
    /// The instant last told (tell_time); RawTimePoint{} before the first.
    [[nodiscard]] auto told_time() const noexcept -> RawTimePoint { return told_now_; }

    /// Any thread: mark talker stream @p idx Started (@p started true) or Stopped.
    /// Stopped suppresses transmit without tearing down the ACMP connection or the
    /// reservation. Defaults Started; out-of-range indices are ignored.
    void note_stream_started(uint16_t idx, bool started) noexcept
    {
        if (idx < stream_started_.size()) {
            stream_started_[idx].store(started, std::memory_order_relaxed);
        }
    }

    /// Reactor thread: talker stream @p idx now has @p count ACMP connections, published for the
    /// media thread. The last disconnect forgets the stream's reservation history; a connection
    /// made while Listener Ready is declared latches it at once.
    void note_acmp_connections(uint16_t idx, uint32_t count) noexcept;

    /// Reactor thread: is talker stream @p idx admitted (gating off, or an ACMP connection whose
    /// listener's reservation is ready)? Only the last disconnect ends admission, so the set is
    /// always a superset of what should_transmit passes; not time-dependent.
    [[nodiscard]] auto admitted(uint16_t idx) const noexcept -> bool
    {
        if (!gate_enabled_) {
            return true;
        }
        if (idx >= acmp_conn_.size() || acmp_conn_[idx].load() == 0) {
            return false;
        }
        return reservation_ready_[idx] || listener_ready_[idx].load(std::memory_order_relaxed);
    }

    /// Any thread: is talker stream @p idx admitted at the RAW instant @p now? True if
    /// gating is disabled, or the stream has an ACMP connection (unless
    /// set_needs_acmp_connection(false)), is Started, and its MSRP Listener Ready is
    /// registered or was withdrawn less than the grace before @p now.
    [[nodiscard]] auto admitted(uint16_t idx, RawTimePoint now) const noexcept -> bool;

    /// When talker stream @p idx's grace runs out: its Listener Ready's withdrawal
    /// stamp plus the grace (the withdrawal itself with no grace), whether or not the
    /// stream's other conditions hold. Nullopt while the Listener Ready is registered,
    /// and for a stream whose Listener Ready was never registered.
    [[nodiscard]] auto grace_ends(uint16_t idx) const noexcept -> std::optional<RawTimePoint>;

    /// Media-timer thread: may talker stream @p idx put its stream on the wire at the
    /// wake told as @p now? Exactly admitted(), independent of every other stream.
    [[nodiscard]] auto should_transmit(uint16_t const idx, RawTimePoint const now) const noexcept -> bool
    {
        return admitted(idx, now);
    }

    /// Reactor thread: set the admitted() latch.
    void latch_ready(uint16_t idx) noexcept;

    // --- References / values (bound at construction) ---------------------------
    bool gate_enabled_;                             ///< config.gate_talker_on_listener, destructured at the call site
    bool needs_acmp_connection_{true};              ///< see set_needs_acmp_connection
    std::chrono::nanoseconds grace_{GRACE};         ///< see set_listener_grace
    nanoavb::NanoAvbComponents const& components_;  ///< reads acmp_talker connection state
    std::optional<logging::Logger> logger_{};       ///< reactor-thread gate-transition logging
    RawTimePoint told_now_{};                       ///< see tell_time; reactor thread
    bool untold_logged_{false};                     ///< the one log line of an untold note_listener_ready

    // --- Owned gate state (cross-thread) ---------------------------------------
    /// "A listener permits transmit" (MSRP Listener Ready), per stream. Written by
    /// note_listener_ready (reactor) after the stamp, with release; read with acquire.
    std::array<std::atomic<bool>, STREAM_COUNT> listener_ready_{};
    /// The media wake at which each stream's Listener Ready was last seen set
    /// while connected (NEVER until then, and again once the last connection
    /// goes); the strict gate keeps transmitting for GRACE past it. Media-timer thread only.
    std::array<RawTimePoint, STREAM_COUNT> last_seen_ready_ = never_seen();
    /// Stream Started state, per stream (defaults true; see the constructor). A
    /// Stopped stream is not admitted even when connected + Listener Ready.
    std::array<std::atomic<bool>, STREAM_COUNT> stream_started_{};
    /// Per-stream ACMP connection count, published by note_acmp_connections
    /// (reactor) and read by should_transmit (media timer). Mirrors
    /// acmp_talker.connection_count(idx) so the media thread never reads the
    /// reactor-mutated connection list directly.
    std::array<itc::Published<uint32_t>, STREAM_COUNT> acmp_conn_{};
    /// Listener Ready seen while the stream had an ACMP connection (cleared
    /// when the last one goes). Reactor thread only; feeds admitted().
    std::array<bool, STREAM_COUNT> reservation_ready_{};

  private:
    [[nodiscard]] static constexpr auto never_seen() noexcept -> std::array<RawTimePoint, STREAM_COUNT>
    {
        std::array<RawTimePoint, STREAM_COUNT> seen{};
        seen.fill(NEVER);
        return seen;
    }
    /// MSRP Listener Ready now, or seen within GRACE of @p now (recorded here).
    [[nodiscard]] auto listener_ready_within_grace(uint16_t idx, RawTimePoint now) noexcept -> bool;
};

}  // namespace statusbar::avb_entity
