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
/// (tell_time). Finds a stream by its StreamID through the lookup given at
/// construction (the kit's: the components' ACMP talker).

#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/itc/itc_published.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/realtime/realtime_clock.hpp"
#include "statusbar/sg14/inplace_function.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace statusbar::avb_entity {

struct TalkerGate
{
    /// The talker stream index (below STREAM_COUNT) a StreamID is declared on, or nullopt.
    using StreamIndexFn = sg14::inplace_function<std::optional<uint16_t>(nanoavb::StreamId const&), 32>;

    /// A gate whose note_listener_ready() finds streams by @p stream_index (reactor thread); with
    /// @p gate_enabled false every stream is admitted.
    TalkerGate(bool gate_enabled, StreamIndexFn stream_index) noexcept;
    /// The kit's gate: the streams of @p components' ACMP talker, which must outlive it.
    TalkerGate(bool gate_enabled, nanoavb::NanoAvbComponents const& components) noexcept;

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
    /// to @p ready, known at the RAW instant @p now. Matched to a talker stream index
    /// by ACMP stream_id (same value MSRP advertises). A registration and the
    /// withdrawal that ends it both stamp @p now, from which the grace runs.
    void note_listener_ready(nanoavb::StreamId const& stream_id, bool ready, RawTimePoint now);
    /// As above, known at the instant last told (tell_time): for an MSRP hook,
    /// which is not handed the time of its event. Told no time yet, the flag is
    /// kept and no stamp is made, so a withdrawal then opens no grace; the
    /// first such call says so in the log.
    void note_listener_ready(nanoavb::StreamId const& stream_id, bool ready);

    /// Reactor thread: the RAW instant of the reactor cycle now being served,
    /// read by the loop when the reactor read its own clock and before any
    /// handler of the cycle ran (entity_reactor_clock). Every Listener Ready
    /// noted without an instant in this cycle is stamped with it.
    void tell_time(RawTimePoint const now) noexcept { told_now_ = now; }
    /// The instant last told (tell_time); RawTimePoint{} before the first.
    [[nodiscard]] auto told_time() const noexcept -> RawTimePoint { return told_now_; }

    /// Reactor thread: our Talker declarations were withdrawn. Every stream closes at once (no
    /// grace) and forgets its listener state until note_listener_ready re-learns it.
    void notify_talker_withdrawn() noexcept;
    /// Reactor thread: stream @p idx's Talker Advertise alone was withdrawn (a Milan talker's
    /// declaration window closed, or MAAP lost its address): that stream closes at once, as above.
    void notify_talker_withdrawn(uint16_t idx) noexcept;
    /// Reactor thread: talker stream @p idx's Talker Advertise is held back (a
    /// changed value waiting out the one it replaces, IEEE 802.1Q 35.2.2.8) or
    /// no longer. A held stream is not admitted, with the gate disabled too:
    /// no frame goes out of a stream whose attribute is being kept off the
    /// wire. Returns whether that changed; out-of-range indices change nothing.
    auto note_advertise_held(uint16_t const idx, bool const held) noexcept -> bool
    {
        return idx < advertise_held_.size() && advertise_held_[idx].exchange(held, std::memory_order_relaxed) != held;
    }

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

    /// Any thread: is talker stream @p idx admitted at the RAW instant @p now? True if
    /// gating is disabled, or the stream has an ACMP connection (unless
    /// set_needs_acmp_connection(false)), is Started, and its MSRP Listener Ready is
    /// registered or was withdrawn less than the grace before @p now.
    [[nodiscard]] auto admitted(uint16_t idx, RawTimePoint now) const noexcept -> bool;
    /// Whether stream @p idx's MSRP Listener Ready is registered at @p now or was withdrawn less
    /// than the grace before it, regardless of gate_enabled, Started and ACMP (Milan v1.3 5.3.7.3).
    [[nodiscard]] auto listener_admitted(uint16_t idx, RawTimePoint now) const noexcept -> bool;

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

    // --- References / values (bound at construction) ---------------------------
    bool gate_enabled_;                        ///< config.gate_talker_on_listener, destructured at the call site
    bool needs_acmp_connection_{true};         ///< see set_needs_acmp_connection
    std::chrono::nanoseconds grace_{GRACE};    ///< see set_listener_grace
    StreamIndexFn stream_index_;               ///< maps stream_id -> talker stream index
    std::optional<logging::Logger> logger_{};  ///< reactor-thread gate-transition logging
    RawTimePoint told_now_{};                  ///< see tell_time; reactor thread
    bool untold_logged_{false};                ///< the one log line of an untold note_listener_ready

    // --- Owned gate state (cross-thread) ---------------------------------------
    /// "A listener permits transmit" (MSRP Listener Ready), per stream. Written by
    /// note_listener_ready (reactor) after the stamp, with release; read with acquire.
    std::array<std::atomic<bool>, STREAM_COUNT> listener_ready_{};
    /// The RAW instant (ns) each stream's Listener Ready was last known registered; 0
    /// until it first is, and while the gate has been told no time. Written by
    /// note_listener_ready (reactor) alone.
    std::array<std::atomic<int64_t>, STREAM_COUNT> ready_raw_ns_{};
    /// Whether each stream's Talker Advertise is held back (note_advertise_held,
    /// reactor); read by admitted() on any thread.
    std::array<std::atomic<bool>, STREAM_COUNT> advertise_held_{};
    /// Stream Started state, per stream (defaults true; see the constructor). A
    /// Stopped stream is not admitted even when connected + Listener Ready.
    std::array<std::atomic<bool>, STREAM_COUNT> stream_started_{};
    /// Per-stream ACMP connection count, published by note_acmp_connections
    /// (reactor) and read by should_transmit (media timer). Mirrors
    /// acmp_talker.connection_count(idx) so the media thread never reads the
    /// reactor-mutated connection list directly.
    std::array<itc::Published<uint32_t>, STREAM_COUNT> acmp_conn_{};

  private:
    /// Reactor thread: stream @p idx's stamp, then its flag, for a Listener @p ready at
    /// @p now; returns the flag as it was.
    [[nodiscard]] auto stamp(uint16_t idx, bool ready, RawTimePoint now) noexcept -> bool;
    /// The stream's ACMP connection + Started preconditions (both published by
    /// the reactor thread, read here on the media timer's).
    [[nodiscard]] auto connected_and_started(uint16_t idx) const noexcept -> bool;
    /// MSRP Listener Ready registered now, or withdrawn less than the grace before @p now.
    [[nodiscard]] auto listener_ready_within_grace(uint16_t idx, RawTimePoint now) const noexcept -> bool;
    /// Stream @p idx's stamp plus the grace; nullopt with no stamp.
    [[nodiscard]] auto stamp_plus_grace(uint16_t idx) const noexcept -> std::optional<RawTimePoint>;
};

}  // namespace statusbar::avb_entity
