#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity Audio I/O Module (dual-format)
/// Defines an AVB entity with TWO talker stream sources and TWO listener stream
/// sinks, each N-channel 96 kHz:
///   - stream 0: AM824 (IEC 61883-6, MBLA 24-in-32)
///   - stream 1: AAF (IEEE 1722 AVTP Audio Format, 32-bit PCM)
/// Both streams share one audio engine (per-channel sine + biquad), one AVTP
/// transmit socket, and one stream RX port that dispatches by AVTP subtype.
/// The entity model is loaded from a descriptor storage blob that declares two
/// stream inputs and two stream outputs.

// The local stream data plane, the WAN tunnel, the media-clock rate tracker and
// the transmit gate are each their own collaborator now (god-object phases 0-4);
// this coordinator includes those collaborator headers (which re-export the avtp /
// udptun / colbin / stun / itc / ptpclient internals they own) plus only what it
// uses directly: the ATDECC/nanoavb control plane, DSP, the AAF reframer, the media
// clock, and the reactor/clock/status glue.
#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/avb_entity/avb_entity_aaf_reframe.hpp"
#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/avb_entity/avb_entity_crf_clock_recovery.hpp"
#include "statusbar/avb_entity/avb_entity_host.hpp"
#include "statusbar/avb_entity/avb_entity_listener_streams.hpp"
#include "statusbar/avb_entity/avb_entity_maap.hpp"
#include "statusbar/avb_entity/avb_entity_media_rate.hpp"
#include "statusbar/avb_entity/avb_entity_talker_gate.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_bridge.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/avtp/avtp_maap_handler.hpp"
#include "statusbar/dsp/dsp.hpp"
#include "statusbar/gptp/gptp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_atomic_triple_buffer.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/realtime/realtime.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace statusbar::avb_entity {

using statusbar::failure;
using statusbar::Status;
using statusbar::StatusValue;
using statusbar::success;

/// Audio processing callback type. Called with interleaved N-channel samples.
using AudioProcessCallback = statusbar::sg14::inplace_function<void(std::span<float> samples, size_t sample_count), 64>;

/// AVB Entity with dual-format (AM824 + AAF) N-channel audio streams.
///
/// Talker stream 0 transmits AM824 to am824_talker_dest_mac; talker stream 1
/// transmits AAF (int32 PCM) to aaf_talker_dest_mac. The single stream RX port
/// joins both multicast groups and dispatches received frames by AVTP subtype
/// (0x00 -> AM824, 0x02 -> AAF). Both talkers carry the same per-channel sine
/// source (distinct phase per channel) so the two formats are directly
/// comparable on the wire. Coordination (gPTP, MVRP, MSRP, ACMP, ADP) reuses the
/// nanoavb supervisor stack exactly as the single-format AM824 entity does.
class AvbEntityAudioIO
{
  public:
    using TimePoint = sm::TimePoint;

    /// Stream format constants. 96 kHz, SR class A (125 us interval = 8000
    /// packets/s), so samples-per-packet = 96000/8000 = 12.
    static constexpr uint32_t CLASS_A_PACKETS_PER_SEC = 8000;
    static constexpr uint32_t SAMPLE_RATE = 96000;
    static constexpr size_t SAMPLES_PER_PACKET = SAMPLE_RATE / CLASS_A_PACKETS_PER_SEC;

    /// CRF (media-clock reference) base frequency. Milan mandates a 48 kHz CRF
    /// media clock; 48 kHz AND 96 kHz clients both lock to it. Our audio runs at
    /// SAMPLE_RATE (96 kHz), so each CRF timestamp covers SAMPLE_RATE/CRF_BASE_FREQUENCY
    /// (= 2) audio samples -- the on-wire timestamp values stay spaced at
    /// 1/CRF_BASE_FREQUENCY s. Must divide SAMPLE_RATE evenly.
    static constexpr uint32_t CRF_BASE_FREQUENCY = 48000;
    static_assert(SAMPLE_RATE % CRF_BASE_FREQUENCY == 0, "CRF base must divide the audio sample rate");

    /// Redundancy flag bit in the tunnel stream_id: the redundant copy is sent with
    /// `primary | UDPTUN_REDUN_BIT`, so the egress tells primary from redundant. It
    /// MUST sit in the modified-EUI-64 middle bytes (b3/b4 = the inserted 0xFF:0xFE,
    /// uint64 bits 24..39) — the bytes owlm_analyze masks when grouping primary +
    /// redundant into one logical sender — otherwise the two copies land in different
    /// pair_ids and recovery accounting breaks. Bit 24 (LSB of b4 = 0xFE) is reliably
    /// clear in a MAC-derived id and inside owlm's mask. (A NIC-half bit like 1<<23
    /// would be wrong on both counts: it can be set in the MAC, and lies outside
    /// owlm's mask.) The primary id force-clears this bit so the redundant id is always
    /// distinct. The b3/b4 mask owlm applies for pairing is OWLM_PAIR_MASK_MIDBYTES.
    static constexpr uint64_t UDPTUN_REDUN_BIT = (1ULL << 24);
    static constexpr uint64_t OWLM_PAIR_MASK_MIDBYTES = 0x000000FF'FF000000ULL;
    static_assert((UDPTUN_REDUN_BIT & (UDPTUN_REDUN_BIT - 1)) == 0, "REDUN_BIT must be a single bit");
    static_assert(
        (UDPTUN_REDUN_BIT & ~OWLM_PAIR_MASK_MIDBYTES) == 0,
        "REDUN_BIT must live in the EUI-64 b3/b4 bytes that owlm masks for pair grouping");

    /// AAF stream wire format: 32-bit signed PCM at 96 kHz.
    static constexpr avtp::AafFormat AAF_FORMAT = avtp::AafFormat::int_32bit;
    static constexpr avtp::AafSampleRate AAF_SAMPLE_RATE = avtp::AafSampleRate::rate_96_khz;
    static constexpr uint8_t AAF_BIT_DEPTH = 32;

    /// Stream descriptor indices.
    static constexpr uint16_t AM824_STREAM_INDEX = 0;
    static constexpr uint16_t AAF_STREAM_INDEX = 1;
    static constexpr uint16_t CRF_STREAM_INDEX = 2;  // media-clock (no audio, no listener sink)
    /// STREAM_INPUT index of the CRF media-clock input (kit phase 3c).
    static constexpr uint16_t CRF_INPUT_STREAM_INDEX = 2;

    /// Factory method — constructs and validates the entity from configuration.
    /// Parses the descriptor storage blob (which must declare >=2 stream inputs
    /// and >=2 stream outputs) to determine channel count and entity model.
    [[nodiscard]] static auto create(AvbEntityAudioIOConfig config, std::pmr::memory_resource* memory_resource = nullptr)
        -> StatusValue<std::unique_ptr<AvbEntityAudioIO>>;

    ~AvbEntityAudioIO();

    AvbEntityAudioIO(AvbEntityAudioIO const&) = delete;
    auto operator=(AvbEntityAudioIO const&) -> AvbEntityAudioIO& = delete;
    AvbEntityAudioIO(AvbEntityAudioIO&&) = delete;
    auto operator=(AvbEntityAudioIO&&) -> AvbEntityAudioIO& = delete;

    /// Passkey gating the public constructor (see AvbEntityAm824IO::CreateKey).
    class CreateKey
    {
        CreateKey() = default;
        friend class AvbEntityAudioIO;
    };

    AvbEntityAudioIO(
        CreateKey,
        AvbEntityAudioIOConfig config,
        std::unique_ptr<nanoavb::AemEntityHandler> handler,
        nanoavb::DescriptorStorageHandler* storage_handler,
        uint16_t initial_clock_source,
        std::optional<uint16_t> crf_clock_source_index,
        size_t channels,
        std::pmr::memory_resource* memory_resource);

    /// The CLOCK_DOMAIN's active clock-source index (blob default until a
    /// controller SET_CLOCK_SOURCE changes it).
    [[nodiscard]] auto active_clock_source() const noexcept -> uint16_t
    {
        return active_clock_source_.load(std::memory_order_acquire);
    }
    /// The clock-source index backed by the CRF stream input, when the model
    /// declares one (INPUT_STREAM located at STREAM_INPUT CRF_INPUT_STREAM_INDEX).
    [[nodiscard]] auto crf_clock_source_index() const noexcept -> std::optional<uint16_t> { return crf_clock_source_index_; }
    /// The CRF-input media-clock recovery (rate/phase of the remote clock).
    [[nodiscard]] auto crf_recovery() noexcept -> CrfClockRecovery& { return crf_recovery_; }

    /// Start the entity and add handlers to reactor
    [[nodiscard]] auto start(net::MessageReactor& reactor) -> Status;

    /// Stop the entity and clean up resources
    [[nodiscard]] auto stop() -> Status;

    [[nodiscard]] auto is_running() const noexcept -> bool { return host_.is_running(); }
    [[nodiscard]] auto is_ready() const noexcept -> bool { return host_.is_ready(); }
    [[nodiscard]] auto state_string() const -> std::string_view { return host_.state_string(); }
    auto print_state() const -> void;

    auto on_link_up(TimePoint time) -> void;
    auto on_link_down(TimePoint time) -> void;
    auto on_gptp_announce(TimePoint time, bool has_grandmaster) -> void;
    auto on_timeout(TimePoint time) -> void;

    /// Process one audio packet period (called from PTP timer at 8000 Hz):
    /// generate audio once, then transmit both an AM824 and an AAF packet.
    auto process_audio(TimePoint time) -> void;

    /// Whether talker stream `idx` (0=AM824, 1=AAF, 2=CRF) should put its AVTP
    /// stream on the wire this tick: true if gating is disabled, or ALL of this
    /// stream's own preconditions hold -- an ACMP connection exists AND the stream
    /// is Started AND MSRP Listener Ready (with grace). Each stream (CRF included)
    /// gates independently. See TalkerGate / gate_talker_on_listener.
    [[nodiscard]] auto talker_should_transmit(uint16_t idx, int64_t now_ns) const noexcept -> bool;

    auto set_audio_callback(AudioProcessCallback callback) -> void { audio_callback_ = std::move(callback); }

    /// Register a per-stream RX consumer (decoded floats + PTS, kit phase 2).
    /// Menu/selection: an index the model never declares stays inert.
    void set_consume(uint16_t stream_index, StreamConsumeFn fn) { listener_->set_consume(stream_index, std::move(fn)); }
    auto configure_filter(double freq_hz, double gain_db, double q) -> void;

    [[nodiscard]] auto components() -> nanoavb::NanoAvbComponents& { return host_.components(); }
    [[nodiscard]] auto components() const -> nanoavb::NanoAvbComponents const& { return host_.components(); }
    [[nodiscard]] auto net_handlers() -> nanoavb::NanoAvbNetHandlers* { return host_.net_handlers(); }

    // --- Logging (see AvbEntityHost) --------------------------------------------
    [[nodiscard]] auto ctl_log_channel() noexcept -> logging::LogChannelBase& { return host_.ctl_log_channel(); }
    [[nodiscard]] auto media_log_channel() noexcept -> logging::LogChannelBase& { return host_.media_log_channel(); }
    void set_log_verbosity(logging::LogLevel const v) noexcept
    {
        host_.set_log_verbosity(v);
        udptun_->worker_log_channel().set_verbosity(v);
    }
    /// The udptun punch worker's log channel (its own producer thread).
    [[nodiscard]] auto udptun_log_channel() noexcept -> logging::LogChannelBase& { return udptun_->worker_log_channel(); }
    [[nodiscard]] auto config() const noexcept -> AvbEntityAudioIOConfig const& { return config_; }
    [[nodiscard]] auto channels() const noexcept -> size_t { return channels_; }

    /// TX stream-capture (see AvbEntityAudioIOConfig::tx_pcap_path). The capture
    /// itself runs RT-safe on the media thread; the FILE write must happen off the
    /// RT path, so the non-RT main loop polls tx_pcap_ready_to_write() after each
    /// poll and calls flush_tx_pcap() once to persist the file.
    [[nodiscard]] auto tx_pcap_ready_to_write() const noexcept -> bool { return talker_->tx_pcap_ready_to_write(); }
    [[nodiscard]] auto flush_tx_pcap() -> Status { return talker_->flush_tx_pcap(); }
    [[nodiscard]] auto tx_pcap_frame_count() const noexcept -> size_t { return talker_->tx_pcap_frame_count(); }

    /// Batch-drain the AM824/AAF RX socket, stamping frames with @p wake_gptp_ns. Called
    /// from the tool's dedicated SCHED_FIFO RX timer when config.stream_rx_rt_timer is
    /// set (see start()); a no-op when the RX handler is on the reactor instead.
    auto drain_stream_rx(int64_t wake_gptp_ns) -> size_t
    {
        return (rt_rx_handler_ != nullptr) ? listener_->drain_rx(wake_gptp_ns) : 0;
    }

  private:
    /// Attach this entity's STREAM-specific behavior to the host: the MSRP
    /// advertise/withdraw reservations + the talker gate (typed hooks), the ACMP
    /// talker/listener connection callbacks, and the AEM GET_COUNTERS/GET_STREAM_INFO
    /// handlers (all via host_.components()). The host owns the generic SM wiring.
    auto wire_stream_callbacks() -> void;

    /// Build the MSRP talker reservation for the AM824 stream (stream 0). MSRP
    /// advertises a single talker reservation matching the existing single-format
    /// entity; both data-plane streams transmit regardless (link-up driven).
    [[nodiscard]] auto make_talker_srp_info(uint16_t stream_index) const -> nanoavb::TalkerStreamSrpInfo;

    /// Declare the MSRP Talker Advertise for all three talker streams, using each
    /// stream's CURRENT destination MAC. Deferred until the dest MACs are final
    /// (maap_addresses_ready_): in MAAP mode the addresses arrive asynchronously
    /// AFTER gPTP lock (which is what first triggers the advertise), so advertising
    /// earlier would declare a stale/static dest that won't match the MAAP address
    /// ACMP hands the listener -> the listener can't reserve (AskingFailed).
    /// Called from the host advertise hook and re-called from on_acquired once the
    /// MAAP block is defended. Static mode: the flag is always set (advertise now).
    void advertise_talker_streams(TimePoint time);

    /// In "maap" stream_address_mode, acquire a contiguous block of multicast
    /// addresses (one per talker stream) via MAAP and (re)configure the talker
    /// streams' destination MACs from it BEFORE the data plane reads them. Blocks
    /// until the block is defended (or a timeout), then hands the MaapHandler to
    /// the reactor to keep defending. On failure logs and leaves the static MACs.
    /// No-op in "static" mode. Called from start() after the control plane is up.
    [[nodiscard]] auto acquire_maap_addresses(net::MessageReactor& reactor) -> Status;

    /// Periodically (re)estimate the GPS frequency ratio r = switch/GPS by
    /// sampling CLOCK_REALTIME (GPS, via chrony) against the gPTP media time, and
    /// feed it to the deterministic media-clock generator. Runs on the media
    /// thread; cheap and rate-limited.
    void update_gps_ratio(uint64_t gptp_now_ns);

    AvbEntityAudioIOConfig config_;
    AudioProcessCallback audio_callback_;

    /// The reusable AVB control plane: state machines + NanoAvbComponents + net
    /// handlers + lifecycle. This entity supplies only its streams + data plane and
    /// attaches them via host_.components() + the typed hooks (wire_stream_callbacks).
    /// Declared after config_ (gate_/listener_ bind host_.components()).
    AvbEntityHost host_;

    /// Per-stream transmit gate (ACMP-AND-MSRP + grace). Written by the MSRP
    /// listener-ready callback (reactor), read by process_audio (media timer).
    /// Binds config_ + host_.components(), declared after them. See avb_entity_talker_gate.hpp.
    TalkerGate gate_{config_.gate_talker_on_listener, host_.components()};

    size_t channels_{0};

    //
    // DSP processing (shared by both talker streams)
    //
    std::pmr::memory_resource* mem_resource_{std::pmr::get_default_resource()};
    std::pmr::vector<dsp::BiQuad<float>> biquads_;
    std::pmr::vector<float> audio_buffer_;
    std::pmr::vector<dsp::Oscillator<float>> oscillators_;

    //
    // Stream data plane (one TX socket shared by both talkers)
    //

    /// Latest gPTP time (ns) seen by the media timer; the RX thread reads it as
    /// "gPTP now" (<=125 us stale) for LATE/EARLY_TIMESTAMP detection, since the
    /// reactor's own clock is CLOCK_MONOTONIC, not gPTP.
    std::atomic<uint64_t> last_gptp_ns_{0};

    /// Deterministic, GPS-rate-pinned presentation-timestamp generator (shared by
    /// both talker streams -- they emit the same sample count per tick). The
    /// avtp_timestamp comes from this, not the jittery media-timer wake time, so a
    /// listener recovering its media clock from the stream stays rock-steady.
    ptpclient::MediaClockGenerator media_clock_;
    /// Pins the media-clock rate to GPS: tracks r = switch/GPS from CLOCK_REALTIME
    /// vs gPTP and maps the gPTP/PHC master clock to absolute GPS-TAI for the
    /// inter-site tunnel timeline (source pacing, ingest avtp_timestamp, egress
    /// playout all ride this single cross-site-common clock). Fed by
    /// update_gps_ratio on the media thread. See avb_entity_media_rate.hpp.
    MediaClockRateTracker rate_tracker_{};

    /// CRF-input media-clock recovery (kit phase 3c): fed by the CRF stream
    /// input's timestamps on the RX thread; consulted for the media-clock rate
    /// on the media thread when the CRF clock source is active.
    CrfClockRecovery crf_recovery_{};
    /// The CLOCK_DOMAIN's active clock-source index. Written by the
    /// SET_CLOCK_SOURCE apply callback (reactor thread), read per tick by the
    /// media thread.
    std::atomic<uint16_t> active_clock_source_{0};
    /// The clock-source index whose CLOCK_SOURCE descriptor is the CRF stream
    /// input (resolved from the blob at create; nullopt when not modeled).
    std::optional<uint16_t> crf_clock_source_index_{};
    /// The blob-backed descriptor handler (owned by host_); used to register
    /// the clock-source apply callback.
    nanoavb::DescriptorStorageHandler* storage_handler_{nullptr};

    /// Latest GPS-TAI translator snapshot, published by update_gps_ratio on the
    /// media thread and consumed by the tunnel bridge's reactor-thread ingest
    /// path so it never touches the single-threaded Kalman directly. SPSC:
    /// media = producer, reactor = sole consumer.
    itc::AtomicTripleBuffer<ptpclient::GpsTaiSnapshot> tai_snapshot_{};

    /// The inter-site WAN tunnel collaborator (god-object phase 2): owns all tunnel
    /// state + the ingest/egress/send/punch-worker/watchdog methods; the entity
    /// forwards into it (udptun_->...). Declared AFTER the members it references
    /// (config_, rate_tracker_, tai_snapshot_, audio_buffer_, channels_, last_gptp_ns_)
    /// so those bind constructed and outlive it (it destructs first). Always allocated.
    std::unique_ptr<EntityUdptunBridge> udptun_{
        std::make_unique<EntityUdptunBridge>(config_, rate_tracker_, tai_snapshot_, audio_buffer_, channels_, last_gptp_ns_)};

    /// The local AVB stream RX path (god-object phase 3, RX half): owns the AM824/
    /// AAF deserialize contexts, the RX counters, the STREAM_INPUT health counters,
    /// and the borrowed RX socket, plus on_stream_rx_frame / frame_is_for_listener /
    /// the counter readers and the ACMP connect/disconnect handlers. Reads the entity
    /// config / gPTP-now / ACMP+MSRP state through refs (config_, last_gptp_ns_,
    /// components_) and delivers accepted audio to the tunnel via the StreamRxAudioSink
    /// (udptun_). Declared AFTER components_/config_/last_gptp_ns_/udptun_. Always allocated.
    std::unique_ptr<ListenerStreams> listener_{std::make_unique<ListenerStreams>(
        config_.lock_tolerance_ns, SAMPLE_RATE, host_.components(), last_gptp_ns_, udptun_.get())};

    /// The stream RX socket handler. In the default path it is moved into the reactor
    /// by start(); when config.stream_rx_rt_timer is set it is kept HERE (alive, owning
    /// the socket) and drained by the tool's SCHED_FIFO RX timer via drain_stream_rx().
    /// Base type so the concrete StreamRxHandler (defined in the .cpp) stays private.
    std::unique_ptr<net::Pollable> rt_rx_handler_{};

    /// The local AVB stream TX path (god-object phase 3): owns the qdisc-bypass TX
    /// socket, the AM824/AAF/CRF serializers, dest MACs, TX counters, and the
    /// TX-capture recorder, plus the transmit_* methods process_audio forwards to.
    /// Declared after the members it references (config_, media_clock_,
    /// audio_buffer_, channels_, last_gptp_ns_). Always allocated.
    std::unique_ptr<TalkerStreams> talker_{std::make_unique<TalkerStreams>(
        TalkerStreamsConfig{.sample_rate = SAMPLE_RATE, .vlan_id = config_.vlan_id, .stream_pcp = config_.stream_pcp},
        media_clock_,
        last_gptp_ns_,
        mem_resource_)};

    /// MAAP dynamic-address acquisition (active only in "maap"
    /// stream_address_mode; ready() defaults true for static MACs).
    MaapAddressAcquirer maap_{};

    /// Fill the GET_COUNTERS bitmap + values for a STREAM_OUTPUT (talker) index
    /// (true if it is one of our talker streams). Exposes FRAMES_TX so a reader
    /// can see our actual transmit rate.
};

}  // namespace statusbar::avb_entity
