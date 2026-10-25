#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_listener_streams.hpp
/// @brief ListenerStreams — the local AVB stream RX path.
///
/// Owns N per-stream RX slots (each holding the kind-matching AM824/AAF
/// deserialize context shaped by its StreamSpec, its RX telemetry, and its
/// IEEE 1722.1 STREAM_INPUT health counters) and the borrowed StreamPort,
/// plus the methods that decode one received stream frame, gate it to
/// the connected stream, update the counters, and hand the accepted audio to
/// a StreamRxAudioSink. Runs on the reactor/RX thread (on_stream_rx_frame is
/// invoked by drain_rx; the ACMP connect/disconnect
/// methods are wired as the acmp_listener connection callbacks).
///
/// Entity Construction Kit phase 1: the input table is no longer two fixed
/// AM824/AAF contexts — it is a vector of slots derived from StreamSpecs
/// (see avb_entity_stream_spec.hpp), and a frame is matched to its slot by
/// the connected stream_id, so an entity's RX shape follows its declarative
/// model. A CRF slot hands each timestamp to its StreamCrfFn (set_crf).
///
/// Reads the entity's config / gPTP-now through references and the ACMP/MSRP
/// state through a NanoAvbComponents reference (all bound at construction).
/// It knows nothing about where the audio goes -- only the StreamRxAudioSink.

#include "statusbar/avb_entity/avb_entity_slot_guard.hpp"
#include "statusbar/avb_entity/avb_entity_stream_counters.hpp"
#include "statusbar/avb_entity/avb_entity_stream_port.hpp"
#include "statusbar/avb_entity/avb_entity_stream_rx_sink.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avtp/avtp_aaf_stream_input.hpp"
#include "statusbar/avtp/avtp_am824_stream_input.hpp"
#include "statusbar/avtp/avtp_crf_stream_input.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_telemetry_counter.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/status.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace statusbar::avb_entity {

/// One RX stream slot: the StreamSpec that shaped it, its deserialize context
/// (exactly one of am824/aaf/crf engaged, per kind), its data-plane
/// telemetry, and its IEEE 1722.1 STREAM_INPUT health counters. Holds atomics,
/// so slots are constructed in place and never moved.
struct ListenerStreamSlot
{
    StreamSpec spec{};
    /// spec.format.kind: what the RX thread matches frames by, unlocked. Changes (AAF and AM824
    /// only) only under the guard, so the RX thread checks it again once entered.
    std::atomic<StreamKind> kind{StreamKind::other};
    std::optional<avtp::Am824StreamInputContext> am824{};
    std::optional<avtp::AafStreamInputContext> aaf{};
    std::optional<avtp::CrfStreamInputContext> crf{};

    /// Per-stream RX consumer (kit phase 2): receives each decoded channel
    /// (floats + reconstructed 64-bit PTS) on the reactor/RX thread. Empty =
    /// decode for counters only (and the raw-byte StreamRxAudioSink, if any).
    StreamConsumeFn consume{};

    /// Per-stream CRF ingest: each CRF timestamp with the local receive time
    /// and the PDU's mr bit (see StreamCrfFn). CRF slots only.
    StreamCrfFn on_crf{};
    /// ACMP connections made to this input (on_listener_connected); reactor writes, RX reads.
    std::atomic<uint32_t> connections{0};
    /// The stream_id and connection of the last CRF timestamp, and StreamCrfFn's source generation. RX thread only.
    uint64_t crf_stream_id{0};
    uint32_t crf_connection{0};
    uint32_t crf_source{0};

    /// Per-slot data-plane counters (written on the reactor/RX thread, read for status).
    itc::TelemetryCounter<uint64_t> rx_packets{};
    itc::TelemetryCounter<uint64_t> rx_samples{};
    itc::TelemetryCounter<uint64_t> rx_bad{};

    /// IEEE 1722.1 STREAM_INPUT health counters (Clause 7.4.42). The
    /// per-packet update lives in avb_entity_stream_counters.hpp (unit-tested).
    StreamInputCounters counters{};
    /// Their Milan observation-interval form (reactor thread only).
    StreamInputIntervals intervals{};

    /// A bound Milan input that is stopped (STOP_STREAMING, or bound with
    /// STREAMING_WAIT) discards its stream's AVTPDUs (Milan v1.2 Section
    /// 5.3.8.7). Written on the reactor, read on the RX thread.
    std::atomic<bool> stopped{false};

    /// The RX thread's per-frame work against the reactor's reformat_stream.
    SlotGuard guard{};
};

/// The timed packet for @p frames whole frames of an accepted AAF payload presented from @p pts_ns,
/// after @p ctx processed its header: the payload trimmed to those frames, the sample format, tu and
/// mr. Both AAF receive paths build their packet with it.
[[nodiscard]] auto aaf_timed_packet(
    uint16_t stream_index,
    avtp::AafStreamInputContext const& ctx,
    avtp::AafPdu const& pdu,
    std::span<uint8_t const> payload,
    uint16_t frames,
    uint64_t pts_ns) noexcept -> StreamRxPacket;

/// The timed packet for one AAF frame without decoding it (the raw-only RX path): accepts what
/// avtp::aaf_stream_deserialize accepts and updates @p ctx as it does. A refused payload returns
/// frames == 0 and leaves @p ctx untouched.
[[nodiscard]] auto aaf_rx_packet(
    uint16_t stream_index,
    avtp::AafStreamInputContext& ctx,
    avtp::AafPdu const& pdu,
    std::span<uint8_t const> payload,
    uint64_t gptp_now_ns) noexcept -> StreamRxPacket;

/// Whether @p pdu's AAF header carries @p format, the STREAM_INPUT's current format (Milan v1.2
/// Section 4.4.2.2): sample format, nominal rate, channels and samples per frame match, and the
/// bit depth is valid for the container. See docs/MILAN_LISTENER.md.
[[nodiscard]] auto aaf_header_matches_format(StreamFormatFields const& format, avtp::AafPdu const& pdu) noexcept -> bool;

/// The timed packet for @p frames data blocks of an accepted AM824 payload presented from @p pts_ns, after
/// @p ctx processed its header: the payload trimmed to frames x channels quadlets, as received (label,
/// 24-bit sample, big-endian). Both AM824 receive paths build their packet with it.
[[nodiscard]] auto am824_timed_packet(
    uint16_t stream_index,
    avtp::Am824StreamInputContext const& ctx,
    avtp::Am824Pdu const& pdu,
    std::span<uint8_t const> payload,
    uint32_t frames,
    uint64_t pts_ns) noexcept -> StreamRxPacket;

/// The timed packet for one AM824 frame without decoding it (the raw-only RX path): accepts what
/// avtp::am824_deserialize_mbla accepts and updates @p ctx as it does; pts_ns is the first block's,
/// from the last tv anchor (IEEE 1722-2016 Equation (7)). A refused payload returns frames == 0.
[[nodiscard]] auto am824_rx_packet(
    uint16_t stream_index,
    avtp::Am824StreamInputContext& ctx,
    avtp::Am824Pdu const& pdu,
    std::span<uint8_t const> payload,
    uint64_t gptp_now_ns) noexcept -> StreamRxPacket;

/// Whether @p pdu's CIP header carries @p format, the STREAM_INPUT's current format: DBS is the
/// channel count and the FDF's SFC the nominal rate (IEC 61883-6 Section 10.3.2).
[[nodiscard]] auto am824_header_matches_format(StreamFormatFields const& format, avtp::Am824Pdu const& pdu) noexcept -> bool;

/// Whether @p pdu's CRF header carries @p format, the STREAM_INPUT's current format (Milan v1.2
/// Section 4.4.2.2): type, timestamp interval, timestamps per PDU, pull and base frequency match.
[[nodiscard]] auto crf_header_matches_format(StreamFormatFields const& format, avtp::CrfPdu const& pdu) noexcept -> bool;

struct ListenerStreams
{
    /// @param lock_tolerance_ns  MEDIA_LOCKED step tolerance (config.lock_tolerance_ns); each
    ///                           stream's nominal step is at its own format's rate.
    ListenerStreams(
        uint32_t lock_tolerance_ns,
        nanoavb::NanoAvbComponents& components,
        std::atomic<uint64_t> const& last_gptp_ns,
        StreamRxAudioSink* audio_sink) noexcept
        : lock_tolerance_ns_{lock_tolerance_ns}
        , components_{components}
        , last_gptp_ns_{last_gptp_ns}
        , audio_sink_{audio_sink}
    {}

    /// Add an RX slot shaped by @p spec, constructing the kind-matching
    /// deserializer. A pending set_consume() registration for spec.index is
    /// bound here. Errors: kind `other`, unknown rate, or more than MAX_ENTITY_STREAMS slots.
    auto open_stream(StreamSpec const& spec) -> Status;

    /// Re-shape the slot of @p spec.index for @p spec (Milan v1.2 Section 4.4.2.2), keeping its
    /// consumers and counters; an audio slot may change kind (AAF, AM824). Reactor thread; the slot
    /// is locked (SlotGuard) so RX drops its frames. Errors (slot unchanged): no such slot, a kind
    /// change to or from CRF, or a format open_stream would refuse.
    auto reformat_stream(StreamSpec const& spec) -> Status;

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
    /// set_crf for a consumer that takes no source generation.
    template <typename Fn>
        requires(
            std::is_invocable_v<Fn&, uint64_t, uint16_t, int64_t, bool> &&
            !std::is_invocable_v<Fn&, uint64_t, uint16_t, int64_t, bool, uint32_t>)
    void set_crf(uint16_t const stream_index, Fn fn)
    {
        set_crf(stream_index, StreamCrfFn{[fn = std::move(fn)](uint64_t ts, uint16_t index, int64_t rx, bool mr, uint32_t) mutable {
                    fn(ts, index, rx, mr);
                }});
    }

    /// The slot whose spec.index == @p stream_index (the STREAM_INPUT
    /// descriptor index == ACMP listener unique id), or nullptr.
    [[nodiscard]] auto slot_for(uint16_t stream_index) noexcept -> ListenerStreamSlot*;
    [[nodiscard]] auto slot_for(uint16_t stream_index) const noexcept -> ListenerStreamSlot const*;
    /// The first slot of @p kind, or nullptr.
    [[nodiscard]] auto slot_of(StreamKind kind) noexcept -> ListenerStreamSlot*;
    [[nodiscard]] auto slot_of(StreamKind kind) const noexcept -> ListenerStreamSlot const*;

    /// Receive from @p port (outliving this and the handler) joined to @p static_groups
    /// (only the first join's failure fails); add its drain handler to @p reactor, or
    /// with @p keep_handler return it for an RX timer. Reactor thread.
    [[nodiscard]] auto attach_rx(
        StreamPort& port,
        std::span<ieee::Eui48 const> static_groups,
        net::MessageReactor& reactor,
        bool keep_handler = false) -> StatusValue<std::unique_ptr<net::Pollable>>;

    /// The most frames one drain_rx takes from the port: four a stream, room
    /// for a late tick's backlog.
    static constexpr size_t RX_DRAIN_MAX = 4 * MAX_ENTITY_STREAMS;

    /// Drain up to RX_DRAIN_MAX queued frames from the port without blocking through
    /// on_stream_rx_frame, stamped with the caller's gPTP now @p gptp_now_ns. Returns the number drained.
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

    /// GET_COUNTERS for a STREAM_INPUT index as Milan v1.2 Table 5.16 has them
    /// (StreamInputIntervals: interval counters as of the last fold). True if
    /// it is one of our stream inputs. Reactor thread.
    [[nodiscard]] auto fill_interval_stream_input_counters(
        uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& out) const -> bool;

    /// End STREAM_INPUT @p descriptor_index's observation interval
    /// (StreamInputIntervals::fold): true when a counter changed. Reactor thread.
    [[nodiscard]] auto fold_input_counters(uint16_t descriptor_index) -> bool;

    /// The input became bound: zero its counters (Milan v1.2 Section 5.3.8.10).
    /// Reactor thread.
    void reset_input_counters(uint16_t descriptor_index);

    /// The input was unbound (a controller's UNBIND_RX, or a classic
    /// DISCONNECT_RX): its media lock goes without counting an interruption.
    /// Reactor thread.
    void note_input_unbound(uint16_t descriptor_index);

    /// Our listener connected to a remote talker: reserve via MSRP (Listener Ready)
    /// and join the talker's stream multicast group on the port. Wired as the
    /// acmp_listener connect callback. Reactor thread.
    void on_listener_connected(uint16_t stream_index, ieee::Eui64 const& stream_id, ieee::Eui48 dest_mac);

    /// Our listener disconnected: withdraw the MSRP reservation and leave the
    /// talker's multicast group. Wired as the acmp_listener disconnect callback.
    void on_listener_disconnected(uint16_t stream_index);

    /// Attach (nullptr detaches) the timed raw sink offered every accepted AM824/AAF packet with its
    /// presentation time; a stream with no StreamConsumeFn is then not decoded to float.
    /// Set before streams flow; the RX thread reads it.
    void set_timed_audio_sink(StreamRxTimedAudioSink* sink) noexcept { timed_sink_ = sink; }

    /// Discard (@p stopped) or process STREAM_INPUT @p stream_index's
    /// AVTPDUs (ListenerStreamSlot::stopped). Reactor thread.
    void set_stream_stopped(uint16_t const stream_index, bool const stopped) noexcept
    {
        if (auto* const slot = slot_for(stream_index); slot != nullptr) {
            slot->stopped.store(stopped, std::memory_order_relaxed);
        }
    }

    /// Install the reactor-thread logger for connect/disconnect status lines.
    void set_logger(logging::Logger const log) noexcept { logger_ = log; }

    /// Move a pending set_consume registration into its newly-opened slot.
    void bind_pending_consume(ListenerStreamSlot& slot);
    /// Build @p slot's deserializer for @p spec: what open_stream and
    /// reformat_stream share. Errors as open_stream's.
    static auto shape(ListenerStreamSlot& slot, StreamSpec const& spec) -> Status;

    // --- References / collaborators (bound at construction) --------------------
    uint32_t lock_tolerance_ns_;              ///< MEDIA_LOCKED step tolerance
    nanoavb::NanoAvbComponents& components_;  ///< for acmp_listener (connection state) + msrp_handler (reservation)
    std::atomic<uint64_t> const& last_gptp_ns_;
    /// Where accepted listener audio goes (today the WAN tunnel). The listener does
    /// not know what the sink does with it. Nullable.
    StreamRxAudioSink* audio_sink_{nullptr};
    /// Timestamped raw sink (the media bridge's listener seam). Nullable.
    StreamRxTimedAudioSink* timed_sink_{nullptr};
    /// The port frames are received from (attach_rx), also used to join and
    /// leave a remote talker's stream group on connect/disconnect. Borrowed.
    StreamPort* port_{nullptr};
    /// Reactor-thread connect/disconnect logging (nullable).
    std::optional<logging::Logger> logger_{};

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
