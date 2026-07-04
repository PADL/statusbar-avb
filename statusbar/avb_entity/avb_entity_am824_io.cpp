// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity AM824 I/O Implementation
/// Implements the AvbEntityAm824IO class with blob-loaded entity model and N-channel DSP

#include "statusbar/avb_entity/avb_entity_am824_io.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/dsp/dsp.hpp"
#include "statusbar/dsp/dsp_biquad.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/nanoavb/nanoavb_acmp.hpp"
#include "statusbar/nanoavb/nanoavb_adp.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/nanoavb/nanoavb_entity_model.hpp"
#include "statusbar/nanoavb/nanoavb_gptp_sm.hpp"
#include "statusbar/nanoavb/nanoavb_listener_engine_sm.hpp"
#include "statusbar/nanoavb/nanoavb_msrp_listener_sm.hpp"
#include "statusbar/nanoavb/nanoavb_msrp_talker_sm.hpp"
#include "statusbar/nanoavb/nanoavb_mvrp_sm.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/nanoavb/nanoavb_supervisor_sm.hpp"
#include "statusbar/nanoavb/nanoavb_talker_engine_sm.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/catch_or_status.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <functional>
#include <memory>
#include <numbers>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace statusbar::avb_entity {

using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::nanoavb;

namespace {

/// Reactor port that receives AVTP AM824 stream frames on a dedicated socket
/// joined to the talker stream's multicast group, and hands each frame to the
/// owning entity for decode/metering. Owns its own RawnetContext so the stream
/// RX path is independent of the ATDECC control socket.
class StreamRxHandler : public net::Pollable
{
  public:
    StreamRxHandler(std::string_view interface_name, ieee::Eui48 const& group, ListenerStreams* owner)
        : owner_{owner}
    {
        // Join the stream multicast group; the NIC's multicast hash filter
        // delivers it to this (non-promiscuous) socket — verified on the Pi5
        // macb NIC. (Promiscuous mode was briefly used as a workaround but is
        // unnecessary, and its full-wire flood preempted the PTP time-bridge
        // sampler thread, which destabilised the media clock and caused bursty
        // streaming.)
        (void)sock_.open(interface_name, avtp::AVTP_ETHERTYPE, &group, /*qdisc_bypass=*/false);
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return sock_.fd() >= 0; }
    [[nodiscard]] auto socket() noexcept -> net::RawnetContext* { return &sock_; }

    [[nodiscard]] auto fd() const noexcept -> int override { return sock_.fd(); }

    // Reactor path: stamp RX frames with the media-timer gPTP (current_gptp_ns()). The
    // batch drain lives in ListenerStreams::drain_rx so the optional RT RX timer calls
    // the same code with its own wake time. The reactor's monotonic now_ns is ignored --
    // the tally + deserialize need gPTP.
    void on_ready(int64_t /*reactor_now_ns*/) override { owner_->drain_rx(owner_->current_gptp_ns()); }

    void tick(int64_t /*now_ns*/) override {}

    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    net::RawnetContext sock_{};
    ListenerStreams* owner_;
};

}  // namespace

//
// File-static helpers
//

/// Load a single AEM descriptor of type T from storage.
/// Accepts blobs smaller than sizeof(T) via partial-load fallback.
template <typename T>
static auto load_descriptor(DescriptorStorage const& storage, uint16_t config_idx, uint16_t type, uint16_t index) -> StatusValue<T>
{
    auto result = storage.get_descriptor(config_idx, type, index);
    if (!result) {
        return failure(result.error());
    }
    auto const blob = *result;
    T desc{};
    span_load_padded(desc, blob);
    return success(desc);
}

/// Channel count from the first AUDIO_CLUSTER descriptor (default 2). Drives the
/// data-plane buffer sizing; the rest of the model is served straight from the blob.
static auto channels_from_storage(DescriptorStorage const& storage) -> size_t
{
    size_t channels = 2;
    if (auto r = load_descriptor<DescriptorAudioCluster>(storage, 0, DESCRIPTOR_AUDIO_CLUSTER, 0)) {
        auto const ch = static_cast<uint16_t>(r->channel_count);
        if (ch >= 1) {
            channels = static_cast<size_t>(ch);
        }
    }
    return channels;
}

namespace {
/// Serves this entity's descriptors from its .aem blob (symbol-aware), patching the
/// ENTITY identity (entity_id/model_id/name/firmware) from config. Every other
/// descriptor -- including AVB_INTERFACE -- is served verbatim, matching the prior
/// build_entity_model (am824 did not patch the AVB_INTERFACE network/gPTP fields).
class Am824IODescriptorHandler : public nanoavb::DescriptorStorageHandler
{
  public:
    Am824IODescriptorHandler(DescriptorStorage storage, AvbEntityAm824IOConfig const& config)
        : DescriptorStorageHandler{storage}
        , entity_id_{config.entity_id}
        , entity_model_id_{config.entity_model_id}
        , firmware_version_{config.firmware_version}
    {
        // Built-in GET_NAME/SET_NAME of the ENTITY's entity_name (descriptor 0,
        // name 0): seed from config; the base serves get/set and reflects the
        // current value in on_get_entity. In-memory only (resets on restart).
        manage_entity_name(AtdeccString{config.entity_name.c_str()});
    }

    auto on_get_entity(DescriptorRef ref, uint32_t symbol, DescriptorEntity& desc) -> bool override
    {
        // Base fills the blob bytes and reflects the managed entity_name.
        if (!DescriptorStorageHandler::on_get_entity(ref, symbol, desc)) {
            return false;
        }
        desc.entity_id = entity_id_;
        desc.entity_model_id = entity_model_id_;
        desc.firmware_version = AtdeccString{firmware_version_.c_str()};
        return true;
    }

  private:
    ieee::Eui64 entity_id_;
    ieee::Eui64 entity_model_id_;
    std::string firmware_version_;
};
}  // namespace

/// ADP advertiser configuration for this entity (valid_time + reannounce).
static auto make_adp_config() -> AdpAdvertiserConfig
{
    AdpAdvertiserConfig adp_config{};
    adp_config.valid_time = 31;                                        // 62 seconds
    adp_config.reannounce_interval = std::chrono::milliseconds{5000};  // 5 seconds
    return adp_config;
}

//
// Factory method
//

auto AvbEntityAm824IO::create(AvbEntityAm824IOConfig config, std::pmr::memory_resource* memory_resource)
    -> StatusValue<std::unique_ptr<AvbEntityAm824IO>>
{
    // Step 1: Validate the descriptor storage blob
    auto storage_result = DescriptorStorage::create(std::span<uint8_t const>{config.descriptor_storage_blob});
    if (!storage_result) {
        return failure(storage_result.error());
    }

    // Step 2: Symbol-aware. Serve descriptors from the blob through a
    // DescriptorStorageHandler (retains the blob + symbol table) instead of a parsed
    // EntityModel; extract the channel count for the data plane.
    size_t const channels = channels_from_storage(*storage_result);
    auto handler = std::make_unique<Am824IODescriptorHandler>(*storage_result, config);

    // Step 3: Construct entity via make_unique (gated by CreateKey). The entity owns
    // the handler (through the host) and builds NanoAvbComponents in place (it is
    // non-movable), then wires the talker stream / VLAN / MSRP domain.
    std::pmr::memory_resource* const mr = memory_resource != nullptr ? memory_resource : std::pmr::get_default_resource();
    auto entity =
        std::make_unique<AvbEntityAm824IO>(AvbEntityAm824IO::CreateKey{}, std::move(config), std::move(handler), channels, mr);

    // Step 5: Configure filter
    entity->configure_filter(entity->config_.filter_freq_hz, entity->config_.filter_gain_db, entity->config_.filter_q);

    return success(std::move(entity));
}

//
// Constructor / Destructor
//

AvbEntityAm824IO::AvbEntityAm824IO(
    CreateKey,
    AvbEntityAm824IOConfig config,
    std::unique_ptr<nanoavb::AemEntityHandler> handler,
    size_t channels,
    std::pmr::memory_resource* memory_resource)
    // Build the control plane host in place: 1 talker stream (4 max listeners), 1
    // listener stream. Symbol-aware: the host serves descriptors through the handler
    // (retains the blob); NanoAvbComponents is non-movable, built in place.
    : config_{std::move(config)}
    , host_{std::move(handler), make_adp_config(), 1, 4, 1}
    , channels_{channels}
    , mem_resource_{memory_resource}
    , biquads_(channels, dsp::BiQuad<float>{}, mem_resource_)
    , audio_buffer_(SAMPLES_PER_PACKET * channels, 0.0f, mem_resource_)
    , oscillators_(channels, dsp::Oscillator<float>{}, mem_resource_)
{
    // Configure the per-channel sine source: identical frequency, but a distinct
    // initial phase per channel so no two channels carry the same signal.
    double const sr_recip = 1.0 / static_cast<double>(SAMPLE_RATE);
    for (size_t ch = 0; ch < channels_; ++ch) {
        double const phase =
            channels_ > 0 ? (2.0 * std::numbers::pi * static_cast<double>(ch) / static_cast<double>(channels_)) : 0.0;
        oscillators_[ch].state_.set_frequency(
            dsp::FrequencyParameters<double>{
                .sample_rate_recip = sr_recip, .frequency = config_.tone_freq_hz, .phase_in_radians = phase},
            0);
        oscillators_[ch].coeffs_.set_amplitude(config_.tone_amplitude, 0);
    }

    // Post-construction setup that needs config-supplied values. Done here
    // (not in NanoAvbComponents) because it is entity-specific. The parsed
    // EntityModel is no longer populated (descriptors come from the handler), so the
    // talker stream id is based on config_.entity_id directly.

    // Configure talker stream 0: stream id = entity id (first 6 bytes), with
    // the hardwired multicast destination MAC from config.
    ieee::Eui64 stream_id{};
    span_copy(stream_id.span().first(6), config_.entity_id.span().first(6));
    (void)host_.components().acmp_talker.configure_stream(0, stream_id, config_.talker_dest_mac);

    // Register the VLAN with MVRP and set the MSRP SR-class domain.
    (void)host_.components().mvrp_handler.register_vlan(config_.vlan_id, sm::Clock::now());
    host_.components().msrp_handler.set_domain(DomainInfo{
        .sr_class_id = 6,        // SR Class A
        .sr_class_priority = 3,  // Priority 3
        .sr_class_vid = config_.vlan_id});
}

AvbEntityAm824IO::~AvbEntityAm824IO()
{
    // Destructors must not throw; catch_or_status contains any exception from
    // stop() (and compiles to a plain call under -fno-exceptions).
    (void)statusbar::catch_or_status(
        [&]() -> statusbar::Status {
            if (host_.is_running()) {
                (void)stop();
            }
            return {};
        },
        std::errc::io_error);
}

//
// Stream-specific control-plane wiring (the host owns the generic SM wiring)
//

void AvbEntityAm824IO::wire_stream_callbacks()
{
    // One talker stream. The host declares the SR class domain for us before these
    // fire, then advertises/withdraws on the MSRP cycle. No transmit gate and no
    // listener hook here -- readiness is the host's generic SM drive.
    host_.set_advertise_streams([this](TimePoint time) {
        auto result = host_.components().msrp_handler.talker_advertise(make_talker_srp_info(), time);
        if (!result) {
            std::print(stderr, "Warning: MSRP talker_advertise failed: {}\n", result.error().message());
        }
    });
    host_.set_withdraw_streams(
        [this](TimePoint time) { (void)host_.components().msrp_handler.talker_withdraw(make_talker_srp_info().stream_id, time); });

    // ACMP: observe controller-initiated connections to our talker (diagnostic;
    // streaming is gated by MSRP listener-ready + gPTP).
    host_.components().acmp_talker.set_connection_callbacks(
        [](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            std::print(
                "[acmp] talker stream {} CONNECTED  by listener {:012x} unique_id {}\n",
                stream_index,
                listener_entity_id.to_uint64(),
                listener_unique_id);
        },
        [](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            std::print(
                "[acmp] talker stream {} DISCONNECTED by listener {:012x} unique_id {}\n",
                stream_index,
                listener_entity_id.to_uint64(),
                listener_unique_id);
        });
}

//
// Start / Stop
//

auto AvbEntityAm824IO::start(net::MessageReactor& reactor) -> Status
{
    // Bring up the shared control plane (net handlers + generic SM wiring), then
    // attach this entity's stream-specific callbacks.
    if (auto status = host_.start_control_plane(reactor, config_.interface_name); !status) {
        return status;
    }
    wire_stream_callbacks();

    // --- Stream data plane ---
    // Resolve the talker stream identity (stream id + dest MAC) from ACMP
    // stream 0 so the AVTP stream, MSRP reservation, and ACMP all agree.
    statusbar::tsn::StreamId sid{};
    if (auto const* s = host_.components().acmp_talker.get_stream(0); s != nullptr) {
        (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &sid);
        stream_dest_mac_ = s->stream_dest_mac;
    }
    talker_.am824_dest_mac_ = stream_dest_mac_;

    // Deterministic media clock (r=1.0, gPTP-locked): owns the presentation offset and
    // supplies the smooth avtp_timestamp, so the stream-output context below adds ZERO
    // extra offset -- the timestamp handed to transmit_am824 is already the final
    // presentation time.
    media_clock_ = ptpclient::MediaClockGenerator{ptpclient::MediaClockGenerator::Config{
        .sample_rate_hz = static_cast<double>(SAMPLE_RATE), .presentation_offset_ns = PRESENTATION_OFFSET_NS}};

    talker_.am824_out_.emplace(
        sid, avtp::Am824SampleRate::rate_96_khz, static_cast<uint8_t>(channels_), /*presentation_offset_ns=*/0);
    listener_.am824_in_.emplace(avtp::Am824SampleRate::rate_96_khz, static_cast<uint8_t>(channels_));

    // Transmit socket (PTP-thread egress). qdisc-bypass so we do not re-receive
    // our own stream frames on this host.
    (void)talker_.stream_tx_.open(config_.interface_name, avtp::AVTP_ETHERTYPE, nullptr, /*qdisc_bypass=*/true);

    // Receive port: join the stream multicast group; the shared ListenerStreams decodes
    // + tallies incoming AM824 (STREAM_INPUT health counters). Same handler shape as
    // AvbEntityAudioIO: a thin socket owner delegating the batch drain to drain_rx.
    auto rx = std::make_unique<StreamRxHandler>(config_.interface_name, stream_dest_mac_, &listener_);
    if (rx->valid()) {
        listener_.rx_sock_ = rx->socket();  // borrow before the move; used for dynamic listener joins
        if (config_.stream_rx_rt_timer) {
            rt_rx_handler_ = std::move(rx);  // kept alive; drained by the tool's SCHED_FIFO RX timer
        } else {
            reactor.add(std::move(rx));  // default: drained on the shared reactor thread
        }
    }

    // The per-stream transmit gate tracks MSRP Listener Ready + the ACMP connection
    // count. Re-sets the acmp_talker callbacks wire_stream_callbacks installed for
    // logging, MERGING the gate publish -- the media timer never reads the reactor-
    // mutated connection list directly. (Gating is disabled by default for this bench
    // entity, so the gate is effectively a no-op unless gate_talker_on_listener is set.)
    host_.set_on_listener_ready(
        [this](nanoavb::StreamId const& stream_id, bool ready) { gate_.note_listener_ready(stream_id, ready); });
    host_.components().acmp_talker.set_connection_callbacks(
        [this](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            std::print(
                "[acmp] talker stream {} CONNECTED  by listener {:012x} unique_id {}\n",
                stream_index,
                listener_entity_id.to_uint64(),
                listener_unique_id);
            gate_.note_acmp_connections(
                stream_index, static_cast<uint32_t>(host_.components().acmp_talker.connection_count(stream_index)));
        },
        [this](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            std::print(
                "[acmp] talker stream {} DISCONNECTED by listener {:012x} unique_id {}\n",
                stream_index,
                listener_entity_id.to_uint64(),
                listener_unique_id);
            gate_.note_acmp_connections(
                stream_index, static_cast<uint32_t>(host_.components().acmp_talker.connection_count(stream_index)));
        });

    // Listener connect/disconnect -> MSRP reserve + join/leave the talker's multicast
    // group on the RX socket. Wired as the acmp_listener connection callbacks.
    host_.components().acmp_listener.set_connection_callbacks(
        [this](uint16_t stream_index, ieee::Eui64 const& stream_id, ieee::Eui48 dest_mac) {
            listener_.on_listener_connected(stream_index, stream_id, dest_mac);
        },
        [this](uint16_t stream_index) { listener_.on_listener_disconnected(stream_index); });

    // AECP GET_COUNTERS: expose the IEEE 1722.1 STREAM_INPUT health counters
    // (Clause 7.4.42) for our AM824 listener stream.
    host_.components().aem_handler.set_get_counters(
        [this](uint16_t descriptor_type, uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& counters) -> bool {
            if (descriptor_type == DESCRIPTOR_STREAM_INPUT) {
                return listener_.fill_stream_input_counters(descriptor_index, valid, counters);
            }
            return false;
        });

    return success();
}

auto AvbEntityAm824IO::stop() -> Status
{
    if (!host_.is_running()) {
        return failure(std::make_error_code(std::errc::not_connected));
    }
    auto const now = TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    return host_.stop_control_plane(now);
}

//
// State Queries
//

void AvbEntityAm824IO::print_state() const
{
    std::print(
        "State: supervisor={} gptp={} mvrp={} acmp_connections={} channels={} stream_tx={} stream_rx={} rx_samples={} "
        "rx_bad={}\n",
        state_string(),
        host_.gptp_locked() ? "Locked" : "Unlocked",
        host_.mvrp_joined() ? "Joined" : "NotJoined",
        host_.components().acmp_talker.connection_count(0),
        channels_,
        talker_.am824_tx_packets_,
        listener_.am824_rx_packets_.load(),
        listener_.am824_rx_samples_.load(),
        listener_.am824_rx_bad_.load());
}

//
// MSRP talker reservation
//

auto AvbEntityAm824IO::make_talker_srp_info() const -> nanoavb::TalkerStreamSrpInfo
{
    nanoavb::TalkerStreamSrpInfo info{};

    // Source stream id / destination / VLAN from the ACMP-configured talker
    // stream 0 so MSRP advertises the exact same stream the controller sees.
    if (auto const* stream = host_.components().acmp_talker.get_stream(0); stream != nullptr) {
        (void)statusbar::tsn::load_unchecked(stream->stream_id.span(), &info.stream_id);
        info.dest_address = stream->stream_dest_mac;
        info.vlan_id = stream->stream_vlan_id;
    } else {
        info.vlan_id = config_.vlan_id;
    }

    // SR class A: one AVTP packet per 125 us measurement interval.
    info.max_interval_frames = 1;

    // max_frame_size is the L2 payload (MSDU): AVTP stream header (24) + CIP
    // header (8) = 32 bytes, then SAMPLES_PER_PACKET AM824 quadlets per channel
    // at 4 bytes each (12 samples x channels x 4 at 96 kHz).
    constexpr uint16_t AVTP_AM824_HEADER_BYTES = 32;
    constexpr uint16_t AM824_QUADLET_BYTES = 4;
    info.max_frame_size = static_cast<uint16_t>(AVTP_AM824_HEADER_BYTES + (SAMPLES_PER_PACKET * channels_ * AM824_QUADLET_BYTES));

    // We are the media source; downstream bridges accumulate their own latency.
    info.accumulated_latency = 0;
    return info;
}

//
// Event Handlers
//

// The entity's events drive the shared SM stack, owned by the host.
void AvbEntityAm824IO::on_link_up(TimePoint time)
{
    host_.on_link_up(time);
}
void AvbEntityAm824IO::on_link_down(TimePoint time)
{
    host_.on_link_down(time);
}
void AvbEntityAm824IO::on_gptp_announce(TimePoint time, bool has_grandmaster)
{
    host_.on_gptp_announce(time, has_grandmaster);
}
void AvbEntityAm824IO::on_timeout(TimePoint time)
{
    host_.on_timeout(time);
}

//
// Audio Processing
//

void AvbEntityAm824IO::process_audio(TimePoint time)
{
    // Publish this media-timer wake's gPTP time for the listener RX tally (the reactor /
    // RX-timer path reads it as the "now" for the STREAM_INPUT LATE/EARLY classification).
    last_gptp_ns_.store(static_cast<uint64_t>(time.time_since_epoch().count()), std::memory_order_relaxed);

    // Source: generate the per-channel sine into the interleaved buffer.
    for (size_t i = 0; i < SAMPLES_PER_PACKET; ++i) {
        for (size_t ch = 0; ch < channels_; ++ch) {
            audio_buffer_[(i * channels_) + ch] = oscillators_[ch](0.0F);
        }
    }

    // Process through per-channel biquad filters (interleaved layout).
    for (size_t i = 0; i < SAMPLES_PER_PACKET; ++i) {
        for (size_t ch = 0; ch < channels_; ++ch) {
            audio_buffer_[(i * channels_) + ch] = biquads_[ch](audio_buffer_[(i * channels_) + ch]);
        }
    }

    // Call custom audio callback if set.
    if (audio_callback_) {
        audio_callback_(std::span{audio_buffer_}, SAMPLES_PER_PACKET);
    }

    // Transmit via the shared TalkerStreams path. process_audio() only runs when the
    // media timer fires, which (with the linuxptp driver) only happens while the PTP
    // bridge is healthy — i.e. we have a usable clock. This entity deliberately does
    // NOT gate on the supervisor lock: loss of gPTP Announces must not stop
    // transmission (gate_talker_on_listener defaults false, so should_transmit is
    // always true). A proper "stop after prolonged sync loss" gate (drift-budget /
    // asCapable model) is future work — see the gptp-sync-loss-drift-budget note.
    auto const gptp_now = static_cast<uint64_t>(time.time_since_epoch().count());

    // Advance the deterministic media clock (r=1.0, gPTP-locked). first_index anchors
    // the smooth presentation timestamp used for the transmitted packet.
    auto const tick = media_clock_.advance(gptp_now, 1.0, static_cast<uint32_t>(SAMPLES_PER_PACKET));
    uint64_t const pts = media_clock_.timestamp_for(tick.first_index);

    // Steady-clock now for the MSRP-ready grace window (same clock note_listener_ready
    // stamps last-ready with); decide the gate once per wake.
    int64_t const now_steady_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (gate_.should_transmit(0, now_steady_ns)) {
        talker_.transmit_am824(pts, static_cast<uint32_t>(SAMPLES_PER_PACKET));
    }
}

void AvbEntityAm824IO::configure_filter(double const freq_hz, double const gain_db, double const q)
{
    double const sample_rate_recip = 1.0 / static_cast<double>(SAMPLE_RATE);

    // Apply the same peak EQ coefficients to all per-channel biquad filters
    for (size_t ch = 0; ch < channels_; ++ch) {
        biquads_[ch].coeffs.calculate_peak(
            {.sample_rate_recip = sample_rate_recip, .frequency = freq_hz, .q = q, .gain_db = gain_db, .channel = 0});
        biquads_[ch].reset();
    }
}

}  // namespace statusbar::avb_entity
