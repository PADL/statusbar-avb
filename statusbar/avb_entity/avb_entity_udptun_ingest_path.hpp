#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_udptun_ingest_path.hpp
/// @brief UdptunIngestPath — the inter-site tunnel's INGEST concern
/// (refactor phase C, split out of EntityUdptunBridge).
///
/// Local stream audio -> AAF-v1/AnnexJ datagrams to the peer: the codec + the
/// TAI-anchored reframer, the redundant-TX delay line, and the three sources
/// that can feed it (real listener audio with AM824->int32 transcode, the
/// silence filler, and the logarithmic test sweep with its TAI pacing). The
/// tunnel socket is borrowed from UdptunTransport (send()/tx_lock()); the
/// ingest never owns it.

#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/avb_entity/avb_entity_media_rate.hpp"
#include "statusbar/avb_entity/avb_entity_stream_rx_sink.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_format.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_telemetry.hpp"
#include "statusbar/avb_entity/log_sweep_generator.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_atomic_triple_buffer.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/udptun/udptun_aaf_v1_codec.hpp"
#include "statusbar/udptun/udptun_audio_ingest.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace statusbar::avb_entity {

class UdptunTransport;

class UdptunIngestPath
{
  public:
    UdptunIngestPath(
        AvbEntityAudioIOConfig const& config,
        size_t const& channels,
        MediaClockRateTracker const& rate_tracker,
        itc::AtomicTripleBuffer<ptpclient::GpsTaiSnapshot>& tai_snapshot,
        std::atomic<uint64_t> const& last_gptp_ns,
        UdptunTransport& transport,
        UdptunTelemetry telemetry) noexcept
        : config_{config}
        , channels_{channels}
        , rate_tracker_{rate_tracker}
        , tai_snapshot_{tai_snapshot}
        , last_gptp_ns_{last_gptp_ns}
        , transport_{transport}
        , telemetry_{std::move(telemetry)}
    {}

    UdptunIngestPath(UdptunIngestPath const&) = delete;
    auto operator=(UdptunIngestPath const&) -> UdptunIngestPath& = delete;
    UdptunIngestPath(UdptunIngestPath&&) = delete;
    auto operator=(UdptunIngestPath&&) -> UdptunIngestPath& = delete;
    ~UdptunIngestPath() = default;

    /// Inject the main/reactor-thread logger (setup lines).
    void set_ctl_logger(logging::Logger const log) noexcept { ctl_log_ = log; }

    /// Build the codec / reframer / buffer state and arm the ingest. Socket-free
    /// (the punch-worker path builds state long before a socket exists).
    void build_state();
    [[nodiscard]] auto enabled() const noexcept -> bool { return enable_; }

    /// Fresh tunnel socket: drop the TAI anchor + sweep pacing state so both
    /// re-anchor on the new stream. Called by the transport's install hook,
    /// inside the tx lock (the anchor is send-visible state).
    void reanchor() noexcept
    {
        anchored_ = false;
        sweep_tai_anchor_ns_ = 0;
        sweep_frames_emitted_ = 0;
    }

    /// A listener offers one accepted stream packet's audio (reactor thread).
    /// Ingested only when this is the configured tunnel source stream and the
    /// test sweep is not replacing the source; AM824 MBLA is transcoded to
    /// int32 first (the tunnel transport + far egress are int32).
    void offer_listener_audio(uint16_t stream_index, StreamAudioFormat fmt, std::span<uint8_t const> payload);

    /// Media-thread silence filler: feed @p frames of zero PCM so the entity
    /// transmits silence as if its listener source were sending zeros.
    void ingest_silence(size_t frames);

    /// Media-thread test-sweep source for one packet slot at TAI @p pkt_tai_ns:
    /// emits however many sweep frames elapsed TAI implies (bounded catch-up),
    /// so the ingest timestamps stay locked to TAI with zero drift (refactor
    /// phase C: this pacing moved here from AvbEntityAudioIO::process_audio).
    void sweep_tick(int64_t pkt_tai_ns);

    /// Diagnostic: current size of the AM824->int32 transcode scratch (grows on
    /// the first oversized packet; a growth proves the AM824 path ran).
    [[nodiscard]] auto am824_transcode_bytes() const noexcept -> size_t { return am824_transcode_buf_.size(); }

  private:
    /// GPS-TAI mapping for the ingest anchor/discipline, resolved without racing
    /// the single-threaded Kalman. On the media thread (rt_caller) the Kalman is
    /// coherent (same thread as its writer) so read it directly; on the reactor
    /// thread consume the latest published snapshot. Returns {have_sample, tai_ns}.
    struct IngestTai
    {
        bool have_sample;
        int64_t tai_ns;
    };
    [[nodiscard]] auto ingest_gps_tai(int64_t master_ns, bool rt_caller) -> IngestTai;

    void ingest_audio(std::span<uint8_t const> audio, bool real_source = true, bool rt_caller = false);
    void ingest_am824_as_int32(std::span<uint8_t const> mbla);
    void ingest_sweep(size_t frames);
    void send(int64_t tai_ns, std::span<uint8_t const> pcm);
    void send_encoded(ieee::Eui64 const& stream_id, uint32_t sequence, int64_t tai_ns, std::span<uint8_t const> pcm);

    AvbEntityAudioIOConfig const& config_;
    size_t const& channels_;
    MediaClockRateTracker const& rate_tracker_;
    itc::AtomicTripleBuffer<ptpclient::GpsTaiSnapshot>& tai_snapshot_;
    std::atomic<uint64_t> const& last_gptp_ns_;
    UdptunTransport& transport_;
    UdptunTelemetry telemetry_;
    std::optional<logging::Logger> ctl_log_{};

    bool enable_{false};
    std::optional<udptun::AudioIngest<>> ingest_{};
    std::optional<udptun::AafV1OverAnnexJCodec> codec_{};
    std::vector<uint8_t> txbuf_{};
    ieee::Eui64 stream_id_{};
    uint32_t seq_{0};
    bool anchored_{false};

    // --- Redundant-TX delay line (optional) -----------------------------------
    ieee::Eui64 redundant_id_{};
    struct RedunSlot
    {
        uint32_t seq{0};
        int64_t tai{0};
        bool valid{false};
        std::vector<uint8_t> pcm{};
    };
    std::vector<RedunSlot> redun_ring_{};
    size_t redun_head_{0};
    size_t redun_depth_{0};

    /// Pre-zeroed silence block for the silence source.
    std::vector<uint8_t> silence_buf_{};

    // --- Sweep test source ----------------------------------------------------
    LogSweepGenerator sweep_gen_{};
    std::vector<uint8_t> sweep_buf_{};
    int64_t sweep_tai_anchor_ns_{0};
    uint64_t sweep_frames_emitted_{0};

    /// AM824 MBLA -> int32 transcode scratch.
    std::vector<uint8_t> am824_transcode_buf_{};
};

}  // namespace statusbar::avb_entity
