#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_listener_streams.hpp
/// @brief ListenerStreams — the local AVB stream RX path.
///
/// Owns N per-stream RX slots (each holding the kind-matching AM824/AAF
/// deserialize context shaped by its StreamSpec, its RX telemetry, and its
/// IEEE 1722.1 STREAM_INPUT health counters) and the borrowed RX socket
/// handle, plus the methods that decode one received stream frame, gate it to
/// the connected stream, update the counters, and hand the accepted audio to
/// a StreamRxAudioSink. Runs on the reactor/RX thread (on_stream_rx_frame is
/// invoked by the entity's StreamRxHandler; the ACMP connect/disconnect
/// methods are wired as the acmp_listener connection callbacks).
///
/// Entity Construction Kit phase 1: the input table is no longer two fixed
/// AM824/AAF contexts — it is a vector of slots derived from StreamSpecs
/// (see avb_entity_stream_spec.hpp), and a frame is matched to its slot by
/// the connected stream_id, so an entity's RX shape follows its declarative
/// model. CRF input slots are not yet supported (kit phase 3).
///
/// Reads the entity's config / gPTP-now through references and the ACMP/MSRP
/// state through a NanoAvbComponents reference (all bound at construction).
/// It knows nothing about where the audio goes -- only the StreamRxAudioSink.

#include "statusbar/avb_entity/avb_entity_stream_counters.hpp"
#include "statusbar/avb_entity/avb_entity_stream_rx_sink.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avtp/avtp_aaf_stream_input.hpp"
#include "statusbar/avtp/avtp_am824_stream_input.hpp"
#include "statusbar/avtp/avtp_crf_stream_input.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_telemetry_counter.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/status.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace statusbar::avb_entity {

/// One RX stream slot: the StreamSpec that shaped it, its deserialize context
/// (exactly one of am824/aaf engaged, per spec.format.kind), its data-plane
/// telemetry, and its IEEE 1722.1 STREAM_INPUT health counters. Holds atomics,
/// so slots are constructed in place and never moved.
struct ListenerStreamSlot
{
    StreamSpec spec{};
    std::optional<avtp::Am824StreamInputContext> am824{};
    std::optional<avtp::AafStreamInputContext> aaf{};
    std::optional<avtp::CrfStreamInputContext> crf{};

    /// Per-stream RX consumer (kit phase 2): receives each decoded channel
    /// (floats + reconstructed 64-bit PTS) on the reactor/RX thread. Empty =
    /// decode for counters only (and the raw-byte StreamRxAudioSink, if any).
    StreamConsumeFn consume{};

    /// Per-stream CRF ingest (kit phase 3): receives each CRF timestamp with
    /// the local receive time — feed for media-clock recovery. CRF slots only.
    StreamCrfFn on_crf{};

    /// Per-slot data-plane counters (written on the reactor/RX thread, read for status).
    itc::TelemetryCounter<uint64_t> rx_packets{};
    itc::TelemetryCounter<uint64_t> rx_samples{};
    itc::TelemetryCounter<uint64_t> rx_bad{};

    /// IEEE 1722.1 STREAM_INPUT health counters (Clause 7.4.42). The
    /// per-packet update lives in avb_entity_stream_counters.hpp (unit-tested).
    StreamInputCounters counters{};
};

struct ListenerStreams
{
    /// @param lock_tolerance_ns  MEDIA_LOCKED step tolerance (config.lock_tolerance_ns).
    /// @param sample_rate        Stream sample rate in Hz (96000 for AudioIO/AM824, 48000
    ///                           for StereoIO) -- used for the media-lock nominal step.
    /// Taking the two values it actually needs (rather than a whole entity config) lets
    /// any listener entity reuse ListenerStreams, not just AvbEntityAudioIO.
    ListenerStreams(
        uint32_t lock_tolerance_ns,
        uint32_t sample_rate,
        nanoavb::NanoAvbComponents& components,
        std::atomic<uint64_t> const& last_gptp_ns,
        StreamRxAudioSink* audio_sink) noexcept
        : lock_tolerance_ns_{lock_tolerance_ns}
        , sample_rate_{sample_rate}
        , components_{components}
        , last_gptp_ns_{last_gptp_ns}
        , audio_sink_{audio_sink}
    {}

    /// Add an RX slot shaped by @p spec, constructing the kind-matching
    /// deserializer. A pending set_consume() registration for spec.index is
    /// bound here. Errors: unsupported kind (CRF input is kit phase 3;
    /// `other` never), unknown rate, or more than MAX_ENTITY_STREAMS slots.
    auto open_stream(StreamSpec const& spec) -> Status;

    /// Register a per-stream RX consumer for STREAM_INPUT @p stream_index:
    /// each decoded channel is delivered as floats with its reconstructed
    /// presentation time. Callable before or after the slot exists — the code
    /// is a menu, the model is the selection: an index the model never
    /// declares stays pending and inert (never an error). RX-thread callback.
    void set_consume(uint16_t stream_index, StreamConsumeFn fn);

    /// Register a per-stream CRF timestamp consumer for STREAM_INPUT
    /// @p stream_index (a CRF input slot). Same menu/selection + pending
    /// semantics as set_consume. RX-thread callback.
    void set_crf(uint16_t stream_index, StreamCrfFn fn);

    /// The slot whose spec.index == @p stream_index (the STREAM_INPUT
    /// descriptor index == ACMP listener unique id), or nullptr.
    [[nodiscard]] auto slot_for(uint16_t stream_index) noexcept -> ListenerStreamSlot*;
    [[nodiscard]] auto slot_for(uint16_t stream_index) const noexcept -> ListenerStreamSlot const*;
    /// The first slot of @p kind, or nullptr.
    [[nodiscard]] auto slot_of(StreamKind kind) noexcept -> ListenerStreamSlot*;
    [[nodiscard]] auto slot_of(StreamKind kind) const noexcept -> ListenerStreamSlot const*;

    /// Open the RX socket on @p interface_name — joined to @p static_groups
    /// up front (pass zero MACs when every join is dynamic, i.e. made on ACMP
    /// connect) — borrow it for those dynamic joins, and hand its drain
    /// handler to @p reactor. When @p keep_handler is true the handler is
    /// returned instead of added, so the entity can drain it from a dedicated
    /// SCHED_FIFO RX timer (drain_rx with the timer's wake time). One call
    /// from the owning entity's start() — the socket borrow is THIS class's
    /// concern, so this class assembles it (refactor phase A).
    [[nodiscard]] auto attach_rx(
        std::string_view interface_name,
        std::span<ieee::Eui48 const> static_groups,
        net::MessageReactor& reactor,
        bool keep_handler = false) -> StatusValue<std::unique_ptr<net::Pollable>>;

    /// Non-blocking-drain every queued frame from the RX socket and process each via
    /// on_stream_rx_frame, stamping them all with @p gptp_now_ns (the caller's fresh
    /// gPTP "now"). Called either from the reactor's thin RX adapter (gptp_now_ns =
    /// current_gptp_ns()) or, when stream_rx_rt_timer is set, from a dedicated
    /// SCHED_FIFO RX timer on an isolated core (gptp_now_ns = the timer's wake time),
    /// which keeps ingress frames tallied against a clock at most one tick stale.
    /// Returns the number of frames drained this call (for the RX-timer batch-size stat).
    auto drain_rx(int64_t gptp_now_ns) -> size_t;

    /// The gPTP time the reactor path stamps RX frames with: the media-timer wake
    /// (last_gptp_ns_). The RT-timer path passes its own, fresher wake time instead.
    [[nodiscard]] auto current_gptp_ns() const noexcept -> int64_t
    {
        return static_cast<int64_t>(last_gptp_ns_.load(std::memory_order_relaxed));
    }

    /// Decode one received AVTP stream frame: match it to the slot whose
    /// connected stream_id it carries (kind checked against the subtype),
    /// deserialize, update counters, and offer the accepted audio to the sink.
    /// @p gptp_now_ns is the gPTP receive time used for the deserialize anchor and the
    /// LATE/EARLY classification. Reactor/RX thread.
    void on_stream_rx_frame(std::span<uint8_t const> frame, int64_t gptp_now_ns);

    /// True if @p frame belongs to the stream currently connected to STREAM_INPUT
    /// @p stream_index (listener connected + stream_id at offset 4 matches).
    [[nodiscard]] auto frame_is_for_listener(uint16_t stream_index, std::span<uint8_t const> frame) const -> bool;

    /// Update STREAM_INPUT counters for one received packet (delegates to the pure,
    /// unit-tested tally_stream_input_packet). See the entity-level doc for ts_sparse.
    void update_stream_input_counters(
        ListenerStreamSlot& slot,
        uint8_t seq,
        uint32_t avtp_ts,
        bool tv,
        bool tu,
        bool mr,
        bool format_ok,
        uint64_t samples_per_ch,
        bool ts_sparse,
        int64_t gptp_now_ns);

    /// Fill the GET_COUNTERS bitmap + values for a STREAM_INPUT index (true if it is
    /// one of our stream inputs). Control plane (AEM handler).
    [[nodiscard]] auto fill_stream_input_counters(uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& out) const
        -> bool;

    /// Our listener connected to a remote talker: reserve via MSRP (Listener Ready)
    /// and join the talker's stream multicast group on the RX socket. Wired as the
    /// acmp_listener connect callback. Reactor thread.
    void on_listener_connected(uint16_t stream_index, ieee::Eui64 const& stream_id, ieee::Eui48 dest_mac);

    /// Our listener disconnected: withdraw the MSRP reservation and leave the
    /// talker's multicast group. Wired as the acmp_listener disconnect callback.
    void on_listener_disconnected(uint16_t stream_index);

    /// Install the reactor-thread logger for connect/disconnect status lines.
    void set_logger(logging::Logger const log) noexcept { logger_ = log; }

    /// Move a pending set_consume registration into its newly-opened slot.
    void bind_pending_consume(ListenerStreamSlot& slot);

    // --- References / collaborators (bound at construction) --------------------
    uint32_t lock_tolerance_ns_;              ///< MEDIA_LOCKED step tolerance
    uint32_t sample_rate_;                    ///< stream sample rate (Hz) for the media-lock nominal step
    nanoavb::NanoAvbComponents& components_;  ///< for acmp_listener (connection state) + msrp_handler (reservation)
    std::atomic<uint64_t> const& last_gptp_ns_;
    /// Where accepted listener audio goes (today the WAN tunnel). The listener does
    /// not know what the sink does with it. Nullable.
    StreamRxAudioSink* audio_sink_{nullptr};
    /// Non-owning handle to the StreamRxHandler's RX socket (owned by the reactor).
    /// Set by the entity in start() before the handler is moved into the reactor;
    /// used to join/leave a remote talker's stream multicast group on connect/disconnect.
    net::RawnetContext* rx_sock_{nullptr};
    /// Reactor-thread connect/disconnect logging (nullable).
    std::optional<logging::Logger> logger_{};
    /// Receive scratch for drain_rx (was owned by the StreamRxHandler). 2 KB covers a
    /// full AVB stream frame; drain_rx is single-threaded so one buffer is enough.
    std::array<uint8_t, 2048> rx_buf_{};

    // --- Owned RX state --------------------------------------------------------
    /// The RX slots (atomics inside: constructed in place, never moved/erased).
    sg14::inplace_vector<ListenerStreamSlot, MAX_ENTITY_STREAMS> slots_{};

    /// Consume registrations made before their slot exists (bound in
    /// open_stream; indexes the model never declares stay here, inert).
    struct PendingConsume
    {
        uint16_t stream_index{0};
        StreamConsumeFn fn{};
    };
    sg14::inplace_vector<PendingConsume, MAX_ENTITY_STREAMS> pending_consume_{};
    struct PendingCrf
    {
        uint16_t stream_index{0};
        StreamCrfFn fn{};
    };
    sg14::inplace_vector<PendingCrf, MAX_ENTITY_STREAMS> pending_crf_{};
};

}  // namespace statusbar::avb_entity
