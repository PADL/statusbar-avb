#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_udptun_bridge.hpp
/// @brief EntityUdptunBridge — the inter-site WAN tunnel facade.
///
/// Refactor phase C: the former single-class tunnel (45 members, ~16 concerns)
/// is split into three single-concern collaborators the bridge owns and wires:
///
///   - UdptunTransport   — sockets, STUN punch worker, keepalive, RX watchdog
///   - UdptunIngestPath  — local audio -> codec/reframer -> peer (+ sweep/silence)
///   - UdptunEgressPath  — peer -> WCL playout -> local talkers (+ self-heal)
///
/// plus the shared UdptunTelemetry (diagnostics). The owning entity talks only
/// to this facade: establishment selection (start), the per-wake media-thread
/// calls (service / egress drain+fill / source_tick), and the silence-source
/// gate — the tunnel-source pacing that used to live inline in the entity's
/// process_audio. The entity never reaches into tunnel members.

#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/avb_entity/avb_entity_media_rate.hpp"
#include "statusbar/avb_entity/avb_entity_stream_rx_sink.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_egress_path.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_format.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_ingest_path.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_telemetry.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_transport.hpp"
#include "statusbar/itc/itc_atomic_triple_buffer.hpp"
#include "statusbar/logging/logging.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <vector>

namespace statusbar::avb_entity {

class EntityUdptunBridge : public StreamRxAudioSink
{
  public:
    EntityUdptunBridge(
        AvbEntityAudioIOConfig const& config,
        MediaClockRateTracker const& rate_tracker,
        itc::AtomicTripleBuffer<ptpclient::GpsTaiSnapshot>& tai_snapshot,
        std::pmr::vector<float>& audio_buffer,
        size_t const& channels,
        std::atomic<uint64_t> const& last_gptp_ns) noexcept
        : config_{config}
        , transport_{config, telemetry_}
        , ingest_{config, channels, rate_tracker, tai_snapshot, last_gptp_ns, transport_, telemetry_}
        , egress_{config, channels, audio_buffer, transport_, telemetry_}
    {
        transport_.set_egress(&egress_);
        // Fresh tunnel socket: re-anchor the ingest TAI + reset the egress playout
        // timeline. Fired by the transport inside the stage+tx locks (see
        // UdptunTransport::set_on_socket_installed).
        transport_.set_on_socket_installed([this](int64_t const now_tai_ns) {
            ingest_.reanchor();
            egress_.on_fresh_tunnel(now_tai_ns);
        });
    }

    /// Inject the main/reactor-thread logger (setup + rendezvous lines).
    void set_ctl_logger(logging::Logger const log) noexcept
    {
        transport_.set_ctl_logger(log);
        ingest_.set_ctl_logger(log);
        egress_.set_ctl_logger(log);
    }
    /// The punch worker's log channel — register with the tool's LogCollector.
    [[nodiscard]] auto worker_log_channel() noexcept -> logging::LogChannelBase& { return transport_.worker_log_channel(); }

    /// Bring up the tunnel per config (any failure is non-fatal — the entity
    /// runs its local AVB streams normally without the tunnel). With a
    /// rendezvous server, the async STUN punch-retry worker yields one shared
    /// socket for both directions; with direct ingest+egress one shared direct
    /// socket hole-punches both pinholes; otherwise each direction opens its
    /// own direct socket.
    void start();
    /// Tear down the worker + commit the optional egress timing recorder.
    void stop();

    // --- Media-thread per-wake calls -------------------------------------------
    [[nodiscard]] auto punch_service_active() const noexcept -> bool { return transport_.punch_service_active(); }
    void punch_service(int64_t const now_tai_ns) { transport_.service(now_tai_ns); }

    [[nodiscard]] auto egress_active() const noexcept -> bool { return egress_.active(); }
    void egress_drain_rx() { egress_.drain_rx(); }
    void egress_fill(int64_t const now_tai_ns, size_t const samples) { egress_.fill(now_tai_ns, samples); }

    /// The ingest silence-source gate, decided once per media wake. The silence
    /// source keeps the tunnel TX (and its NAT pinhole) alive ONLY while no real
    /// AVTP audio is arriving. The instant the listener source delivers packets,
    /// the reactor thread feeds those frames straight into the ingest; the
    /// media-timer thread MUST stand down, or the two threads would both submit
    /// to the same reframer -- double-feeding it (2x frame rate, TAI running
    /// ahead) and racing its non-thread-safe state. Mirrors the keepalive
    /// `streaming` predicate (one shared definition: udptun_source_streaming)
    /// so real audio always wins and is forwarded cleanly to the peer.
    /// @p now_tai_ns may be 0 (no tunnel clock yet): falls back to raw
    /// CLOCK_REALTIME + offset.
    [[nodiscard]] auto should_emit_silence(int64_t now_tai_ns) const -> bool;

    /// The tunnel source for one packet slot at TAI @p pkt_tai_ns (0 = no
    /// tunnel clock this wake): the TAI-paced test sweep when [sweep] is
    /// enabled, else @p samples of silence when the silence gate is open.
    /// Media thread; replaces the pacing block that lived in process_audio.
    void source_tick(int64_t pkt_tai_ns, size_t samples, bool emit_silence);

    /// Tunnel telemetry (print_state).
    [[nodiscard]] auto telemetry() const noexcept -> UdptunTelemetry const& { return *telemetry_; }

    /// The ingest path (tests / diagnostics).
    [[nodiscard]] auto ingest() noexcept -> UdptunIngestPath& { return ingest_; }

    /// StreamRxAudioSink: a listener offers one accepted stream packet's audio.
    /// Reactor thread; routed to the ingest path (source-stream + sweep gates).
    void on_listener_audio(
        uint16_t const stream_index, StreamAudioFormat const fmt, std::span<uint8_t const> const payload) override
    {
        ingest_.offer_listener_audio(stream_index, fmt, payload);
    }

  private:
    AvbEntityAudioIOConfig const& config_;
    /// Tunnel telemetry counters; shared with the entity's print_state.
    std::shared_ptr<UdptunTelemetry> telemetry_{std::make_shared<UdptunTelemetry>()};
    UdptunTransport transport_;
    UdptunIngestPath ingest_;
    UdptunEgressPath egress_;
};

}  // namespace statusbar::avb_entity
