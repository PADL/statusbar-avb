#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_udptun_bridge.hpp
/// @brief EntityUdptunBridge — the inter-site WAN tunnel state, extracted from
/// AvbEntityAudioIO (god-object phase 2, step 4).
///
/// Holds the whole tunnel data plane's runtime state: the UDP socket(s), the
/// ingest (AudioIngest + codec + redundancy ring), the egress (AudioEgress +
/// codec + colbin), the sweep test source, the async punch-retry worker (thread
/// + mutex-guarded staging), and the watchdog/keepalive bookkeeping. The
/// telemetry counters are the shared UdptunTelemetry (still readable by the
/// entity's print_state). This step groups the ~45 tunnel members out of the
/// god-object into one collaborator; the methods that operate on them follow.

#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/avb_entity/avb_entity_media_rate.hpp"
#include "statusbar/itc/itc_atomic_triple_buffer.hpp"
#include "statusbar/itc/itc_spin_lock.hpp"
#include "statusbar/avb_entity/avb_entity_stream_rx_sink.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_telemetry.hpp"
#include "statusbar/avb_entity/log_sweep_generator.hpp"
#include "statusbar/colbin/colbin_writer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/net/net_address.hpp"
#include "statusbar/net/net_socket.hpp"
#include "statusbar/udptun/udptun_aaf_v1_codec.hpp"
#include "statusbar/udptun/udptun_audio_egress.hpp"
#include "statusbar/udptun/udptun_audio_ingest.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace statusbar::avb_entity {

/// The inter-site WAN tunnel collaborator: owns the tunnel state and the
/// operating methods (ingest/egress/send/punch worker/watchdog). The entity owns
/// it by unique_ptr and forwards into it; it reads the entity's clock/config/audio
/// state through the references below (bound at construction, same names as the
/// entity's members so the moved method bodies are unchanged). Non-copyable.
struct EntityUdptunBridge : public StreamRxAudioSink
{
    EntityUdptunBridge(
        AvbEntityAudioIOConfig const& config,
        MediaClockRateTracker const& rate_tracker,
        itc::AtomicTripleBuffer<ptpclient::GpsTaiSnapshot>& tai_snapshot,
        std::pmr::vector<float>& audio_buffer,
        size_t const& channels,
        std::atomic<uint64_t> const& last_gptp_ns) noexcept
        : config_{config}
        , rate_tracker_{rate_tracker}
        , tai_snapshot_{tai_snapshot}
        , audio_buffer_{audio_buffer}
        , channels_{channels}
        , last_gptp_ns_{last_gptp_ns}
    {}

    /// GPS-TAI mapping for the ingest anchor/discipline, resolved without racing
    /// the single-threaded Kalman. On the media thread (rt_caller) the Kalman is
    /// coherent (same thread as its writer) so read it directly; on the reactor
    /// thread consume the latest published snapshot. Returns {have_sample, tai_ns}.
    struct IngestTai
    {
        bool have_sample;
        int64_t tai_ns;
    };
    [[nodiscard]] auto ingest_gps_tai(int64_t master_ns, bool rt_caller) -> IngestTai
    {
        if (rt_caller) {
            bool const have = rate_tracker_.has_tai_sample();
            return {.have_sample = have, .tai_ns = have ? rate_tracker_.tai_ns(master_ns) : 0};
        }
        auto const snap = tai_snapshot_.consume();  // reactor thread: sole consumer
        return {.have_sample = snap.have_sample, .tai_ns = snap.have_sample ? ptpclient::tai_ns(snap, master_ns) : 0};
    }

    // --- Operations (run on the entity's threads; reach into the state below) --
    [[nodiscard]] auto setup_udptun_ingest() -> bool;
    void udptun_ingest_audio(std::span<uint8_t const> audio, bool real_source = true, bool rt_caller = false);
    void udptun_ingest_am824_as_int32(std::span<uint8_t const> mbla);
    [[nodiscard]] auto setup_udptun_direct_shared() -> bool;
    void udptun_ingest_silence(size_t frames);
    void udptun_ingest_sweep(size_t frames);
    void udptun_send(int64_t tai_ns, std::span<uint8_t const> pcm);
    void udptun_send_encoded(ieee::Eui64 const& stream_id, uint32_t sequence, int64_t tai_ns, std::span<uint8_t const> pcm);
    [[nodiscard]] auto setup_udptun_egress() -> bool;
    [[nodiscard]] auto setup_udptun_rendezvous() -> bool;
    [[nodiscard]] auto start_udptun_punch_worker() -> bool;
    void udptun_punch_loop();
    void stop_udptun_punch_worker();
    void udptun_punch_service(int64_t now_tai_ns);

    /// Whether the media-thread must run udptun_punch_service() this wake.
    /// True for the STUN worker (punch_run) AND for a DIRECT-SHARED socket:
    /// direct-shared has no worker to publish punch_run, but still needs the
    /// service's NAT keepalive + egress anchor-reset self-heal (the STUN-only
    /// teardown/re-punch paths inside the service are separately gated off by
    /// direct_shared_mode_). Pure so it is unit-testable without a socket.
    [[nodiscard]] static constexpr auto punch_service_should_run(bool punch_run, bool direct_shared_mode) noexcept -> bool
    {
        return punch_run || direct_shared_mode;
    }
    [[nodiscard]] auto punch_service_active() const noexcept -> bool
    {
        return punch_service_should_run(punch_run_.load(), direct_shared_mode_);
    }
    void build_udptun_ingest_state();
    void build_udptun_egress_state();
    [[nodiscard]] auto udptun_rx_fd() const noexcept -> int { return shared_socket_ ? fd_.get() : rx_fd_.get(); }
    void udptun_egress_drain_rx();
    void udptun_egress_fill(int64_t now_tai_ns, size_t samples);

    /// StreamRxAudioSink: a listener offers one accepted stream packet's audio.
    /// The tunnel ingests it only when this is the configured tunnel source stream
    /// and the test sweep is not replacing the source; AM824 MBLA is transcoded to
    /// int32 first (the tunnel transport + far egress are int32). Reactor thread.
    void on_listener_audio(uint16_t stream_index, StreamAudioFormat fmt, std::span<uint8_t const> payload) override;

    // --- References into the owning entity (bound at construction) -------------
    static constexpr uint32_t SAMPLE_RATE = 96000;
    static constexpr uint32_t CLASS_A_PACKETS_PER_SEC = 8000;
    static constexpr size_t SAMPLES_PER_PACKET = SAMPLE_RATE / CLASS_A_PACKETS_PER_SEC;
    static constexpr avtp::AafFormat AAF_FORMAT = avtp::AafFormat::int_32bit;
    static constexpr avtp::AafSampleRate AAF_SAMPLE_RATE = avtp::AafSampleRate::rate_96_khz;
    static constexpr uint8_t AAF_BIT_DEPTH = 32;
    /// Redundancy flag bit in the tunnel stream_id (see AvbEntityAudioIO). The
    /// redundant copy is sent with `primary | UDPTUN_REDUN_BIT`; it must sit in the
    /// EUI-64 b3/b4 bytes owlm masks for primary+redundant pair grouping.
    static constexpr uint64_t UDPTUN_REDUN_BIT = (1ULL << 24);
    static constexpr uint64_t OWLM_PAIR_MASK_MIDBYTES = 0x000000FF'FF000000ULL;
    AvbEntityAudioIOConfig const& config_;
    MediaClockRateTracker const& rate_tracker_;
    itc::AtomicTripleBuffer<ptpclient::GpsTaiSnapshot>& tai_snapshot_;
    std::pmr::vector<float>& audio_buffer_;
    size_t const& channels_;
    std::atomic<uint64_t> const& last_gptp_ns_;

    // --- Ingest (TX): listener audio -> AAF-v1/AnnexJ datagrams -> peer --------
    bool enable_{false};
    net::FileDescriptor fd_{};
    net::SocketAddress peer_{};
    std::optional<udptun::AudioIngest<>> ingest_{};
    std::optional<udptun::AafV1OverAnnexJCodec> codec_{};
    std::vector<uint8_t> txbuf_{};
    ieee::Eui64 stream_id_{};
    uint32_t seq_{0};
    bool anchored_{false};
    /// Serializes the two ingest producers (reactor RX = real audio, media RT =
    /// silence/sweep filler) into the non-thread-safe reframer, AND guards the
    /// send-visible socket state (fd_/peer_/anchored_) so the punch service's
    /// install/teardown never overlaps a reactor-thread sendto. RT caller
    /// try-acquires (wait-free skip on contention); reactor caller spin-acquires.
    itc::SpinLock ingest_lock_{};

    /// Tunnel telemetry counters; shared with the entity's print_state.
    std::shared_ptr<UdptunTelemetry> telemetry_{std::make_shared<UdptunTelemetry>()};

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

    // --- Socket sharing mode flags --------------------------------------------
    bool rendezvous_active_{false};   ///< fd_ is a STUN-traversed socket shared by TX+RX
    bool shared_socket_{false};       ///< fd_ does both ingest TX and egress RX (STUN or direct)
    bool direct_shared_mode_{false};  ///< DIRECT-SHARED (no STUN); stable for the socket's life

    /// Pre-zeroed silence block for the silence source.
    std::vector<uint8_t> silence_buf_{};

    // --- Sweep test source ----------------------------------------------------
    LogSweepGenerator sweep_gen_{};
    std::vector<uint8_t> sweep_buf_{};
    int64_t sweep_tai_anchor_ns_{0};
    uint64_t sweep_frames_emitted_{0};

    /// AM824 MBLA -> int32 transcode scratch.
    std::vector<uint8_t> am824_transcode_buf_{};

    // --- Egress (RX): peer datagrams -> AudioEgress -> local talkers ----------
    bool egress_active_{false};
    net::FileDescriptor rx_fd_{};
    std::optional<udptun::AudioEgress<>> egress_{};
    std::optional<udptun::AafV1OverAnnexJCodec> egress_codec_{};
    std::vector<uint8_t> rxbuf_{};       ///< one-datagram recv scratch
    std::vector<uint8_t> egress_pcm_{};  ///< per-tick playout scratch (int32)
    std::optional<statusbar::colbin::Writer> egress_colbin_{};

    // --- Async punch-retry worker + media-thread install/watchdog -------------
    std::thread punch_thread_{};
    itc::Published<bool> punch_run_{};
    std::mutex stage_mutex_{};
    net::FileDescriptor staged_fd_{};   ///< guarded by stage_mutex_
    net::SocketAddress staged_peer_{};  ///< guarded by stage_mutex_
    bool staged_ready_{false};          ///< guarded by stage_mutex_
    itc::Published<bool> punch_retry_{};
    int64_t install_tai_ns_{0};             ///< media: when current socket installed (0 = none)
    uint64_t rx_baseline_{0};               ///< media: any-rx snapshot for liveness
    int64_t last_rx_ns_{0};                 ///< media: last time any-rx advanced
    bool saw_data_{false};                  ///< media: any datagram since this install
    uint64_t egress_real_frames_{0};        ///< media: cumulative real (non-concealed) egress frames
    uint64_t egress_play_baseline_{0};      ///< media: watchdog snapshot of the above
    int64_t egress_last_play_ns_{0};        ///< media: last time real egress frames advanced
    uint64_t egress_audio_rx_baseline_{0};  ///< media: watchdog snapshot of telemetry rx_packets
    int64_t egress_last_audio_ns_{0};       ///< media: last time decoded tunnel audio arrived
    int egress_reset_streak_{0};            ///< media: consecutive resets without recovery
    int64_t last_keepalive_ns_{0};          ///< media: last keepalive send TAI (rate limit)
};

}  // namespace statusbar::avb_entity
