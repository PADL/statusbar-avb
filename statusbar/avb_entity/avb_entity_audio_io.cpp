// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity Audio I/O Implementation (dual-format AM824 + AAF)
/// Implements AvbEntityAudioIO: blob-loaded model, shared N-channel DSP source,
/// one AM824 talker/listener (stream 0) and one AAF talker/listener (stream 1).

#include "statusbar/avb_entity/avb_entity_audio_io.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_egress.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_ingest.hpp"
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
#include "statusbar/net/net_address.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_posix_util.hpp"
#include "statusbar/net/net_socket.hpp"
#include "statusbar/net/net_util.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/catch_or_status.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/udptun/udptun_aaf_v1_codec.hpp"
#include "statusbar/udptun/udptun_audio_ingest.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <expected>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <sys/socket.h>

namespace statusbar::avb_entity {

using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::nanoavb;

namespace {

/// Reactor port that receives AVTP stream frames on a dedicated socket joined to
/// BOTH stream multicast groups (AM824 + AAF), and hands each frame to the owning
/// entity for subtype dispatch / decode / metering.
class StreamRxHandler : public net::Pollable
{
  public:
    StreamRxHandler(
        std::string_view interface_name, ieee::Eui48 const& am824_group, ieee::Eui48 const& aaf_group, ListenerStreams* owner)
        : owner_{owner}
    {
        // Open joined to the AM824 group, then add the AAF group. The NIC's
        // multicast hash filter delivers both to this (non-promiscuous) socket.
        (void)sock_.open(interface_name, avtp::AVTP_ETHERTYPE, &am824_group, /*qdisc_bypass=*/false);
        (void)sock_.join_multicast(aaf_group);
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return sock_.fd() >= 0; }

    /// Non-owning access to the RX socket, so the entity can join/leave a remote
    /// talker's stream multicast group at connect/disconnect time. Stable across
    /// the move into the reactor (the handler is heap-allocated; only the
    /// unique_ptr moves), so a pointer taken before the move stays valid.
    [[nodiscard]] auto socket() noexcept -> net::RawnetContext* { return &sock_; }

    [[nodiscard]] auto fd() const noexcept -> int override { return sock_.fd(); }

    void on_ready(int64_t now_ns) override
    {
        ieee::Eui48 src{};
        ieee::Eui48 dst{};
        while (true) {
            auto const r = sock_.recv(&src, &dst, buf_);
            if (!r || *r <= 0) {
                break;
            }
            owner_->on_stream_rx_frame({buf_.data(), static_cast<size_t>(*r)}, now_ns);
        }
    }

    void tick(int64_t /*now_ns*/) override {}

    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    net::RawnetContext sock_{};
    std::array<uint8_t, 2048> buf_{};
    ListenerStreams* owner_;
};

//
// File-static helpers
//

template <typename T>
auto load_descriptor(DescriptorStorage const& storage, uint16_t config_idx, uint16_t type, uint16_t index) -> StatusValue<T>
{
    auto result = storage.get_descriptor(config_idx, type, index);
    if (!result) {
        return failure(result.error());
    }
    T desc{};
    span_load_padded(desc, *result);
    return success(desc);
}

/// Channel count from the first AUDIO_CLUSTER descriptor (default 2). Drives the
/// data-plane buffer sizing; the rest of the model is served straight from the blob.
auto channels_from_storage(DescriptorStorage const& storage) -> size_t
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

/// Serves this entity's descriptors from its .aem blob (symbol-aware), patching the
/// two runtime-only seams that cannot live in a static blob: the ENTITY identity
/// (entity_id/model_id/name/firmware, from config) and the AVB_INTERFACE network +
/// gPTP identity (live NIC MAC, its modified-EUI-64 clock identity, and the slave-only
/// gPTP params the blob leaves zero). Every other descriptor is served verbatim.
class AudioIODescriptorHandler : public nanoavb::DescriptorStorageHandler
{
  public:
    AudioIODescriptorHandler(DescriptorStorage storage, AvbEntityAudioIOConfig const& config, std::optional<ieee::Eui48> iface_mac)
        : DescriptorStorageHandler{storage}
        , entity_id_{config.entity_id}
        , entity_model_id_{config.entity_model_id}
        , firmware_version_{config.firmware_version}
        , iface_mac_{iface_mac}
    {
        // Built-in GET_NAME/SET_NAME of the ENTITY's entity_name (descriptor 0,
        // name 0): seed it from config; the base then serves get/set and reflects
        // the current value here in on_get_entity. In-memory only (resets on
        // restart) unless a caller wires set_on_entity_name_changed for NV storage.
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

    auto on_get_avb_interface(DescriptorRef ref, uint32_t symbol, DescriptorAvbInterface& desc) -> bool override
    {
        if (!DescriptorStorageHandler::on_get_avb_interface(ref, symbol, desc)) {
            return false;
        }
        if (iface_mac_) {
            desc.mac_address = *iface_mac_;
            desc.clock_identity = iface_mac_->to_modified_eui64();
        }
        desc.priority1 = 248;    // gPTP default priority1
        desc.clock_class = 248;  // not grandmaster-capable (slave-only)
        desc.offset_scaled_log_variance = 0x436A;
        desc.clock_accuracy = 0xFE;  // unknown
        desc.priority2 = 248;
        desc.domain_number = 0;
        desc.log_sync_interval = static_cast<uint8_t>(static_cast<int8_t>(-3));  // 125 ms (gPTP Class A)
        desc.log_announce_interval = 0;                                          // 1 s
        desc.log_pdelay_interval = 0;                                            // 1 s
        return true;
    }

  private:
    ieee::Eui64 entity_id_;
    ieee::Eui64 entity_model_id_;
    std::string firmware_version_;
    std::optional<ieee::Eui48> iface_mac_;
};

auto make_adp_config() -> AdpAdvertiserConfig
{
    AdpAdvertiserConfig adp_config{};
    adp_config.valid_time = 31;                                        // 62 seconds
    adp_config.reannounce_interval = std::chrono::milliseconds{5000};  // 5 seconds
    return adp_config;
}

/// Derive a stream id from the entity id, with the low byte set to the stream
/// index so the two talker streams have distinct ids.
// Derive a globally-unique IEEE 1722 stream_id from the talker's NIC MAC (high 6
// bytes, unique per interface) plus a per-stream unique_id in the low 16 bits.
// Basing it on the MAC -- not the entity_id -- guarantees two entities never get
// colliding stream_ids even when their entity_ids share their high 48 bits (which
// otherwise silently broke RX stream demux + MSRP reservations: two AAF streams
// with the same stream_id are indistinguishable to a listener).
auto stream_id_for(ieee::Eui48 const& base_mac, uint16_t index) -> ieee::Eui64
{
    ieee::Eui64 sid{};
    auto const mac_bytes = make_const_span(base_mac);
    span_copy(sid.span().first(6), mac_bytes.first(6));
    sid.span()[6] = 0;
    sid.span()[7] = static_cast<uint8_t>(index & 0xFFU);
    return sid;
}

}  // namespace

//
// Factory method
//

auto AvbEntityAudioIO::create(AvbEntityAudioIOConfig config, std::pmr::memory_resource* memory_resource)
    -> StatusValue<std::unique_ptr<AvbEntityAudioIO>>
{
    auto storage_result = DescriptorStorage::create(std::span<uint8_t const>{config.descriptor_storage_blob});
    if (!storage_result) {
        return failure(storage_result.error());
    }

    // Symbol-aware: serve descriptors from the blob through a DescriptorStorageHandler
    // (retains the blob + its symbol table) instead of a parsed EntityModel. The handler
    // patches the runtime ENTITY identity + AVB_INTERFACE network/gPTP fields.
    size_t const channels = channels_from_storage(*storage_result);
    auto const iface_mac = net::read_interface_mac(config.interface_name);
    auto handler = std::make_unique<AudioIODescriptorHandler>(*storage_result, config, iface_mac);

    std::pmr::memory_resource* const mr = memory_resource != nullptr ? memory_resource : std::pmr::get_default_resource();
    auto entity =
        std::make_unique<AvbEntityAudioIO>(AvbEntityAudioIO::CreateKey{}, std::move(config), std::move(handler), channels, mr);

    entity->configure_filter(entity->config_.filter_freq_hz, entity->config_.filter_gain_db, entity->config_.filter_q);

    return success(std::move(entity));
}

//
// Constructor / Destructor
//

AvbEntityAudioIO::AvbEntityAudioIO(
    CreateKey,
    AvbEntityAudioIOConfig config,
    std::unique_ptr<nanoavb::AemEntityHandler> handler,
    size_t channels,
    std::pmr::memory_resource* memory_resource)
    : config_{std::move(config)}  // 3 talker streams (AM824, AAF, CRF), 4 max listeners each; 2 listener streams.
    // Symbol-aware: the host serves descriptors through the handler (retains the blob).
    , host_{std::move(handler), make_adp_config(), 3, 4, 2}
    , channels_{channels}
    , mem_resource_{memory_resource}
    , biquads_(channels, dsp::BiQuad<float>{}, mem_resource_)
    , audio_buffer_((SAMPLES_PER_PACKET + 1) * channels, 0.0f, mem_resource_)  // +1: GPS pacing may emit nominal+1
    , oscillators_(channels, dsp::Oscillator<float>{}, mem_resource_)
{
    // Per-channel sine source: identical frequency, distinct initial phase.
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

    // ATDECC descriptor wire version. Default "2016": emit descriptors at their
    // 1722.1-2013/2016 lengths so controllers that reject 2021-length descriptors
    // can enumerate the model. "2021" emits the full 2021 forms.
    host_.components().aem_handler.set_legacy_2016(config_.atdecc_version != "2021");

    // Base the talker stream_ids on the NIC MAC (globally unique per box). Fall
    // back to the config entity_id's high 6 bytes only when the interface MAC can't
    // be read (e.g. unit tests with a dummy interface). The parsed EntityModel is no
    // longer populated (descriptors come from the handler), so use config_ directly.
    ieee::Eui48 stream_base_mac{};
    if (auto const mac = net::read_interface_mac(config_.interface_name)) {
        stream_base_mac = *mac;
    } else {
        span_copy(stream_base_mac.span(), config_.entity_id.span().first(6));
    }

    // Configure talker stream 0 (AM824) and stream 1 (AAF) with distinct stream
    // ids and destination multicast MACs, so MSRP/ACMP/AVTP agree per stream.
    (void)host_.components().acmp_talker.configure_stream(
        AM824_STREAM_INDEX, stream_id_for(stream_base_mac, AM824_STREAM_INDEX), config_.am824_talker_dest_mac);
    (void)host_.components().acmp_talker.configure_stream(
        AAF_STREAM_INDEX, stream_id_for(stream_base_mac, AAF_STREAM_INDEX), config_.aaf_talker_dest_mac);
    (void)host_.components().acmp_talker.configure_stream(
        CRF_STREAM_INDEX, stream_id_for(stream_base_mac, CRF_STREAM_INDEX), config_.crf_talker_dest_mac);

    (void)host_.components().mvrp_handler.register_vlan(config_.vlan_id, sm::Clock::now());
    host_.components().msrp_handler.set_domain(
        DomainInfo{.sr_class_id = 6, .sr_class_priority = 3, .sr_class_vid = config_.vlan_id});
    host_.components().msrp_handler.set_redeclare_registered_listeners(config_.redeclare_registered_listeners);
    host_.components().msrp_handler.set_suppress_leaveall(config_.suppress_leaveall);
}

AvbEntityAudioIO::~AvbEntityAudioIO()
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

void AvbEntityAudioIO::wire_stream_callbacks()
{
    // MSRP reservations for our three talker streams. The host declares the SR class
    // domain for us before calling these, then advertises/withdraws on the MSRP cycle.
    host_.set_advertise_streams([this](TimePoint time) { advertise_talker_streams(time); });
    host_.set_withdraw_streams([this](TimePoint time) {
        for (uint16_t const idx : {AM824_STREAM_INDEX, AAF_STREAM_INDEX, CRF_STREAM_INDEX}) {
            (void)host_.components().msrp_handler.talker_withdraw(make_talker_srp_info(idx).stream_id, time);
        }
    });
    // The per-stream transmit gate tracks MSRP Listener Ready.
    host_.set_on_listener_ready(
        [this](nanoavb::StreamId const& stream_id, bool ready) { gate_.note_listener_ready(stream_id, ready); });

    // ACMP: log talker connections; drive our listener (MSRP Listener Ready + mcast
    // join) on listener connect/disconnect. See ListenerStreams.
    host_.components().acmp_talker.set_connection_callbacks(
        [this](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            std::print(
                "[acmp] talker stream {} ({}) CONNECTED by listener {:012x} unique_id {}\n",
                stream_index,
                stream_index == AAF_STREAM_INDEX ? "AAF" : "AM824",
                listener_entity_id.to_uint64(),
                listener_unique_id);
            // Publish the fresh connection count for the media-timer gate.
            gate_.note_acmp_connections(
                stream_index, static_cast<uint32_t>(host_.components().acmp_talker.connection_count(stream_index)));
        },
        [this](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            std::print(
                "[acmp] talker stream {} ({}) DISCONNECTED by listener {:012x} unique_id {}\n",
                stream_index,
                stream_index == AAF_STREAM_INDEX ? "AAF" : "AM824",
                listener_entity_id.to_uint64(),
                listener_unique_id);
            gate_.note_acmp_connections(
                stream_index, static_cast<uint32_t>(host_.components().acmp_talker.connection_count(stream_index)));
        });
    host_.components().acmp_listener.set_connection_callbacks(
        [this](uint16_t stream_index, ieee::Eui64 const& stream_id, ieee::Eui48 dest_mac) {
            listener_->on_listener_connected(stream_index, stream_id, dest_mac);
        },
        [this](uint16_t stream_index) { listener_->on_listener_disconnected(stream_index); });

    // AECP GET_COUNTERS (STREAM_INPUT listener health + STREAM_OUTPUT talker rate)
    // and GET_STREAM_INFO (talker stream_id/format/dest/VLAN, queried by a Milan
    // listener to verify the stream before sustaining a connection).
    host_.components().aem_handler.set_get_counters(
        [this](uint16_t descriptor_type, uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& counters) -> bool {
            if (descriptor_type == DESCRIPTOR_STREAM_INPUT) {
                return listener_->fill_stream_input_counters(descriptor_index, valid, counters);
            }
            if (descriptor_type == DESCRIPTOR_STREAM_OUTPUT) {
                return fill_stream_output_counters(descriptor_index, valid, counters);
            }
            return false;
        });
    host_.components().aem_handler.set_get_stream_info(
        [this](uint16_t descriptor_type, uint16_t descriptor_index, atdecc::aem::AemStreamInfoPayload& out) -> bool {
            return fill_stream_output_info(descriptor_type, descriptor_index, out);
        });
}

//
// Start / Stop
//

auto AvbEntityAudioIO::acquire_maap_addresses(net::MessageReactor& reactor) -> Status
{
    // Our station MAC drives the MAAP conflict tie-break and filters our own frames.
    ieee::Eui48 our_mac{};
    if (auto const mac = net::read_interface_mac(config_.interface_name)) {
        our_mac = *mac;
    } else {
        span_copy(our_mac.span(), config_.entity_id.span().first(6));
    }

    // One allocation covering all three talker streams; the PDUs carry the AM824
    // stream's id, and the seed spreads our initial random pick per station.
    statusbar::tsn::StreamId maap_sid{};
    if (auto const* s = host_.components().acmp_talker.get_stream(AM824_STREAM_INDEX); s != nullptr) {
        (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &maap_sid);
    }
    maap_handler_ = std::make_unique<avtp::MaapHandler>(our_mac, maap_sid, our_mac.to_uint64());

    constexpr uint16_t kBlockCount = 3;  // AM824 (+0), AAF (+1), CRF (+2)

    // Gate stream TX until the block is defended. ADP/gPTP/SRP/ACMP are unaffected --
    // they keep running independently in the reactor; only the talker waits.
    maap_addresses_ready_.store(false, std::memory_order_release);

    maap_handler_->set_on_acquired([this](ieee::Eui48 const& block_start, uint16_t /*count*/) {
        // Reactor thread, once the block is defended. Publish each per-stream dest MAC
        // to BOTH the ACMP stream model (for CONNECT_TX / GET_STREAM_INFO / MSRP) and
        // the live TX cache (talker_), then release the gate so the media thread's
        // acquire-load sees the new MACs before it transmits on them.
        auto assign = [this, &block_start](uint16_t idx, uint16_t offset, ieee::Eui48& tx_cache) {
            ieee::Eui48 const dest = avtp::maap_block_address(block_start, offset);
            if (auto const* s = host_.components().acmp_talker.get_stream(idx); s != nullptr) {
                (void)host_.components().acmp_talker.configure_stream(idx, s->stream_id, dest);
            }
            tx_cache = dest;
        };
        assign(AM824_STREAM_INDEX, 0, talker_->am824_dest_mac_);
        assign(AAF_STREAM_INDEX, 1, talker_->aaf_dest_mac_);
        assign(CRF_STREAM_INDEX, 2, talker_->crf_dest_mac_);
        maap_addresses_ready_.store(true, std::memory_order_release);
        std::print(stderr, "MAAP: acquired 3 stream addresses from {}\n", ieee::to_string(block_start).view());
        // (Re)declare the MSRP Talker Advertise now that the dest MACs are final, so
        // listeners reserve against the MAAP address (not the stale static dest the
        // gPTP-lock advertise may have skipped). Only once the SR-class domain is
        // declared (supervisor Ready); otherwise the gPTP-lock advertise hook fires
        // it -- it now sees maap_addresses_ready_ and uses the MAAP dest.
        if (host_.is_ready()) {
            advertise_talker_streams(sm::Clock::now());
        }
    });

    maap_handler_->set_on_lost([this](ieee::Eui48 const& /*start*/, uint16_t /*count*/) {
        // Conflict: close the TX gate until a new block is defended (the handler is
        // already re-probing; on_acquired re-opens it). Full re-advertise/reconnect
        // handling is Phase 4.
        maap_addresses_ready_.store(false, std::memory_order_release);
        std::print(stderr, "Warning: MAAP address lost to a conflict; re-acquiring\n");
    });

    auto net_handler = std::make_unique<nanoavb::MaapNetHandler>(config_.interface_name, *maap_handler_);
    if (!net_handler->valid()) {
        std::print(stderr, "Warning: MAAP socket open failed on {}; using static stream dest MACs\n", config_.interface_name);
        maap_handler_.reset();
        maap_addresses_ready_.store(true, std::memory_order_release);  // fall back: do not gate
        return {};
    }

    // Hand the handler to the reactor: acquisition runs ASYNCHRONOUSLY alongside
    // ADP/gPTP/SRP/ACMP -- start() never blocks. The talker stays gated until
    // on_acquired fires; the handler then keeps the block defended (announce/DEFEND).
    reactor.add(std::move(net_handler));
    maap_handler_->acquire(kBlockCount, net::monotonic_ns());
    return {};
}

auto AvbEntityAudioIO::start(net::MessageReactor& reactor) -> Status
{
    // Bring up the shared control plane (net handlers + generic SM wiring), then
    // attach this entity's stream-specific callbacks (hooks + ACMP + AEM handlers).
    if (auto status = host_.start_control_plane(reactor, config_.interface_name); !status) {
        return status;
    }
    wire_stream_callbacks();

    // In "maap" mode, claim the talker stream destination addresses via MAAP and
    // overwrite the static defaults BEFORE the data plane reads them below.
    // Non-fatal: on failure the entity keeps the static MACs (logged inside).
    if (config_.stream_address_mode == "maap") {
        if (auto status = acquire_maap_addresses(reactor); !status) {
            return status;
        }
    }

    // --- Stream data plane ---
    // Resolve both talker stream identities (id + dest MAC) from ACMP.
    statusbar::tsn::StreamId am824_sid{};
    if (auto const* s = host_.components().acmp_talker.get_stream(AM824_STREAM_INDEX); s != nullptr) {
        (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &am824_sid);
        talker_->am824_dest_mac_ = s->stream_dest_mac;
    }
    statusbar::tsn::StreamId aaf_sid{};
    if (auto const* s = host_.components().acmp_talker.get_stream(AAF_STREAM_INDEX); s != nullptr) {
        (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &aaf_sid);
        talker_->aaf_dest_mac_ = s->stream_dest_mac;
    }
    statusbar::tsn::StreamId crf_sid{};
    if (auto const* s = host_.components().acmp_talker.get_stream(CRF_STREAM_INDEX); s != nullptr) {
        (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &crf_sid);
        talker_->crf_dest_mac_ = s->stream_dest_mac;
    }

    // The deterministic media clock owns the presentation offset and supplies the
    // avtp_timestamp, so the stream-output contexts add ZERO extra offset -- the
    // timestamp we pass them is already the final presentation time.
    media_clock_ = ptpclient::MediaClockGenerator{ptpclient::MediaClockGenerator::Config{
        .sample_rate_hz = static_cast<double>(SAMPLE_RATE), .presentation_offset_ns = config_.presentation_offset_ns}};
    rate_tracker_.configure(ptpclient::KalmanRatioTracker::Config{.meas_noise_ns = 1000.0, .jerk_psd = 1e-3});

    talker_->am824_out_.emplace(
        am824_sid, avtp::Am824SampleRate::rate_96_khz, static_cast<uint8_t>(channels_), /*presentation_offset_ns=*/0);
    listener_->am824_in_.emplace(avtp::Am824SampleRate::rate_96_khz, static_cast<uint8_t>(channels_));
    talker_->aaf_out_.emplace(
        aaf_sid, AAF_FORMAT, AAF_SAMPLE_RATE, static_cast<uint16_t>(channels_), AAF_BIT_DEPTH, /*presentation_offset_ns=*/0);
    listener_->aaf_in_.emplace(AAF_FORMAT, AAF_SAMPLE_RATE, static_cast<uint16_t>(channels_), AAF_BIT_DEPTH);
    // CRF media-clock talker: Milan 48 kHz audio-sample reference, pull x1.0. The
    // declared base is 48 kHz (CRF_BASE_FREQUENCY) so both 48 kHz and 96 kHz Milan
    // clients lock to it; our 96 kHz audio rides as a 2x multiple of this base.
    talker_->crf_out_.emplace(
        crf_sid,
        avtp::CrfType::audio_sample,
        CRF_BASE_FREQUENCY,
        avtp::CrfPull::multiply_1_0,
        config_.crf_timestamp_interval,
        config_.crf_timestamps_per_packet);

    // One TX socket (qdisc-bypass so our own egress is not re-received here).
    (void)talker_->stream_tx_.open(config_.interface_name, avtp::AVTP_ETHERTYPE, nullptr, /*qdisc_bypass=*/true);

    // Optional TX stream capture: because the socket is qdisc-bypass its egress is
    // invisible to any local capture, so tap it at the socket and record our own
    // transmitted frames (gPTP-timestamped) to a pcap for offline inspection.
    if (!config_.tx_pcap_path.empty()) {
        talker_->tx_pcap_recorder_.configure(
            config_.tx_pcap_path,
            config_.tx_pcap_max_bytes,
            /*snaplen=*/1522,
            static_cast<uint64_t>(config_.tx_pcap_seconds) * 1'000'000'000ULL);
        talker_->stream_tx_.set_tx_tap(
            [this](std::span<uint8_t const> frame) { talker_->tx_pcap_recorder_.record(frame, talker_->last_tx_gptp_ns_); });
    }

    // One RX port joined to both stream groups; dispatch by subtype. The handler
    // delivers frames to the listener; the listener borrows the socket for dynamic
    // multicast joins on ACMP connect/disconnect.
    auto rx = std::make_unique<StreamRxHandler>(
        config_.interface_name, talker_->am824_dest_mac_, talker_->aaf_dest_mac_, listener_.get());
    if (rx->valid()) {
        listener_->rx_sock_ = rx->socket();  // borrow before the move; used for dynamic listener joins
        reactor.add(std::move(rx));
    }

    // Inter-site UDPTUN (optional; any failure is non-fatal -- the entity runs its
    // local AVB streams normally without the tunnel). With a rendezvous server,
    // STUN traversal yields one shared socket for both directions; otherwise the
    // ingest/egress each open their own direct socket.
    if (!config_.udptun_rendezvous_server.empty()) {
        // Async punch-RETRY worker (not the one-shot blocking rendezvous): the two
        // sites' entities can't coordinate a single startup handshake, so retry
        // with a clock-derived rotating session id until data flows. See
        // udptun_->start_udptun_punch_worker(). udptun_->setup_udptun_rendezvous() remains for the
        // (unused) one-shot path / reference.
        (void)udptun_->start_udptun_punch_worker();
    } else if (config_.udptun_enable && config_.udptun_egress && !config_.udptun_peer_host.empty()) {
        // Bidirectional direct peer: one shared socket so both ends transmitting
        // hole-punches both NAT pinholes without STUN.
        (void)udptun_->setup_udptun_direct_shared();
    } else {
        (void)udptun_->setup_udptun_ingest();
        (void)udptun_->setup_udptun_egress();
    }

    return success();
}

auto AvbEntityAudioIO::stop() -> Status
{
    if (!host_.is_running()) {
        return failure(std::make_error_code(std::errc::not_connected));
    }

    // Tear down our data plane, then the shared control plane (host stops ADP +
    // releases the net handlers + clears running_).
    udptun_->stop_udptun_punch_worker();
    if (udptun_->egress_colbin_) {
        (void)udptun_->egress_colbin_->commit();
        udptun_->egress_colbin_.reset();
    }
    auto const now = TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    return host_.stop_control_plane(now);
}

void AvbEntityAudioIO::print_state() const
{
    std::print(
        "State: supervisor={} gptp={} mvrp={} acmp[am824={} aaf={}] channels={} | "
        "AM824 tx={} rx={} rx_samples={} rx_bad={} | AAF tx={} rx={} rx_samples={} rx_bad={} | "
        "egress[resets={} repunch={}]\n",
        state_string(),
        host_.gptp_locked() ? "Locked" : "Unlocked",
        host_.mvrp_joined() ? "Joined" : "NotJoined",
        host_.components().acmp_talker.connection_count(AM824_STREAM_INDEX),
        host_.components().acmp_talker.connection_count(AAF_STREAM_INDEX),
        channels_,
        talker_->am824_tx_packets_,
        listener_->am824_rx_packets_.load(),
        listener_->am824_rx_samples_.load(),
        listener_->am824_rx_bad_.load(),
        talker_->aaf_tx_packets_,
        listener_->aaf_rx_packets_.load(),
        listener_->aaf_rx_samples_.load(),
        listener_->aaf_rx_bad_.load(),
        udptun_->telemetry_->egress_reset_count.load(),
        udptun_->telemetry_->egress_repunch_count.load());
}

//
// MSRP talker reservation (advertise the AM824 stream)
//

auto AvbEntityAudioIO::make_talker_srp_info(uint16_t stream_index) const -> nanoavb::TalkerStreamSrpInfo
{
    nanoavb::TalkerStreamSrpInfo info{};
    if (auto const* stream = host_.components().acmp_talker.get_stream(stream_index); stream != nullptr) {
        (void)statusbar::tsn::load_unchecked(stream->stream_id.span(), &info.stream_id);
        info.dest_address = stream->stream_dest_mac;
        info.vlan_id = stream->stream_vlan_id;
    } else {
        info.vlan_id = config_.vlan_id;
    }
    info.max_interval_frames = 1;
    constexpr uint16_t QUADLET_BYTES = 4;
    if (stream_index == CRF_STREAM_INDEX) {
        // CRF: 20-byte header + timestamps_per_packet * 8-byte timestamps (no audio).
        info.max_frame_size =
            static_cast<uint16_t>(avtp::CrfPdu::HEADER_LENGTH + (config_.crf_timestamps_per_packet * avtp::CrfPdu::TIMESTAMP_SIZE));
    } else {
        // L2 payload size per packet: AVTP header (32 for AM824/61883, 24 for AAF)
        // plus one packet of audio quadlets (channels * samples * 4 bytes). +1
        // sample of headroom: GPS-rate pacing emits nominal or nominal+/-1.
        uint16_t const header_bytes = (stream_index == AAF_STREAM_INDEX) ? avtp::AafPdu::HEADER_LENGTH : uint16_t{32};
        info.max_frame_size = static_cast<uint16_t>(header_bytes + ((SAMPLES_PER_PACKET + 1) * channels_ * QUADLET_BYTES));
    }
    info.accumulated_latency = 0;
    return info;
}

void AvbEntityAudioIO::advertise_talker_streams(TimePoint const time)
{
    // Defer until the stream destination MACs are final. In MAAP mode they are
    // acquired asynchronously after gPTP lock (which first triggers this), so
    // advertising a pre-MAAP dest would not match the MAAP address ACMP hands the
    // listener -> AskingFailed. on_acquired re-calls this once the block is defended.
    if (!maap_addresses_ready_.load(std::memory_order_acquire)) {
        return;
    }
    for (uint16_t const idx : {AM824_STREAM_INDEX, AAF_STREAM_INDEX, CRF_STREAM_INDEX}) {
        auto result = host_.components().msrp_handler.talker_advertise(make_talker_srp_info(idx), time);
        if (!result) {
            std::print(stderr, "Warning: MSRP talker_advertise (stream {}) failed: {}\n", idx, result.error().message());
        }
    }
}

//
// Event handlers
//

// The entity's events drive the shared SM stack, owned by the host.
void AvbEntityAudioIO::on_link_up(TimePoint time)
{
    host_.on_link_up(time);
}
void AvbEntityAudioIO::on_link_down(TimePoint time)
{
    host_.on_link_down(time);
}
void AvbEntityAudioIO::on_gptp_announce(TimePoint time, bool has_grandmaster)
{
    host_.on_gptp_announce(time, has_grandmaster);
}
void AvbEntityAudioIO::on_timeout(TimePoint time)
{
    host_.on_timeout(time);
}

//
// Audio processing
//

void AvbEntityAudioIO::process_audio(TimePoint time)
{
    auto const base_now_ns = static_cast<uint64_t>(time.time_since_epoch().count());
    // One packet's worth of gPTP time: 12 samples @ 96 kHz = 125 us.
    constexpr uint64_t PACKET_INTERVAL_NS = (static_cast<uint64_t>(SAMPLES_PER_PACKET) * 1'000'000'000ULL) / SAMPLE_RATE;

    // Emit config_.packets_per_wake packets per stream this wake. Each packet
    // carries a fresh SAMPLES_PER_PACKET block (oscillators advance) and an
    // avtp_timestamp one packet interval later than the previous, so the on-wire
    // cadence (8000 x 12-sample Class A frames/s) and the listener's expected
    // 125 us timestamp step are UNCHANGED -- only the media-timer WAKE rate drops
    // by packets_per_wake, giving that many fewer deadlines to miss. The
    // burst-of-N egress is not strict Class A shaping; presentation_offset_ns
    // (>> N*125 us) absorbs it at the listener. See packets_per_wake doc.
    // Re-estimate the GPS frequency ratio that pins the media-clock rate. When
    // media_lock_to_gptp is set, r stays 1.0 (media clock == gPTP) and we skip the
    // CLOCK_REALTIME sampling entirely -- see Config::media_lock_to_gptp.
    if (!config_.media_lock_to_gptp) {
        update_gps_ratio(base_now_ns);
    }

    // Inter-site rendezvous punch-retry (media-thread half): install a freshly
    // hole-punched socket staged by the worker and watchdog the RX. Runs before
    // the drain so a just-installed socket is drained this same wake.
    if (udptun_->punch_service_active()) {
        udptun_->udptun_punch_service(realtime_tai_ns(config_.udptun_tai_offset_ns));
    }

    // Inter-site egress: drain the UDP socket and snapshot the TAI playout clock
    // once per wake (CLOCK_REALTIME + offset). Done on this (media-timer) thread so
    // AudioEgress stays single-threaded.
    int64_t udptun_now_tai_ns = 0;
    if (udptun_->egress_active_) {
        udptun_->udptun_egress_drain_rx();
        // Tunnel playout clock = GPS-TAI derived from the gPTP master via the TAI
        // translator (NOT raw CLOCK_REALTIME): the gPTP PHC gives a smooth,
        // jitter-free rate and the Kalman offset pins it to absolute GPS-TAI, so
        // this matches the rate the far ingest stamps with -> zero buffer drift.
        // Falls back to raw CLOCK_REALTIME+offset until the translator has a sample.
        if (rate_tracker_.has_tai_sample()) {
            udptun_now_tai_ns = rate_tracker_.tai_ns(static_cast<int64_t>(base_now_ns));
        } else if (int64_t const t = realtime_tai_ns(config_.udptun_tai_offset_ns); t != 0) {
            udptun_now_tai_ns = t;
        }
    }

    // Inter-site ingest silence-source gate (decided once per wake). The silence
    // source keeps the tunnel TX (and its NAT pinhole) alive ONLY while no real
    // AVTP audio is arriving. The instant the listener source delivers packets,
    // the reactor thread feeds those frames straight into the ingest
    // (on_stream_rx_frame -> udptun_ingest_audio); this media-timer thread MUST
    // stand down, or the two threads would both submit to the same reframer --
    // double-feeding it (2x frame rate, TAI running ahead) and racing its
    // non-thread-safe state. Mirrors the keepalive `streaming` predicate so real
    // audio always wins and is forwarded cleanly to the peer.
    bool udptun_emit_silence = false;
    if (udptun_->enable_ && config_.udptun_silence_source) {
        int64_t now_tai = udptun_now_tai_ns;
        if (now_tai == 0) {
            now_tai = realtime_tai_ns(config_.udptun_tai_offset_ns);
        }
        int64_t const last_audio = udptun_->telemetry_->last_real_ingest_tai.load();
        bool const streaming = (last_audio != 0) && (now_tai != 0) && (now_tai - last_audio < 100'000'000LL);
        udptun_emit_silence = !streaming;
    }

    for (size_t p = 0; p < config_.packets_per_wake; ++p) {
        uint64_t const wake_ns = base_now_ns + (static_cast<uint64_t>(p) * PACKET_INTERVAL_NS);
        // Publish the gPTP media time so the RX thread can judge LATE/EARLY_TIMESTAMP
        // against gPTP (the reactor's own clock is monotonic, not gPTP).
        last_gptp_ns_.store(wake_ns, std::memory_order_relaxed);

        // Deterministic, GPS-rate-pinned media clock: how many samples to emit
        // this tick (nominal +/- 1, paced to GPS) and the jitter-free presentation
        // timestamp of the packet's first sample. The wake time only paces the
        // count; the timestamp does NOT carry its jitter.
        auto const tick = media_clock_.advance(wake_ns, rate_tracker_.r(), static_cast<uint32_t>(SAMPLES_PER_PACKET));
        auto const samples = static_cast<size_t>(tick.samples);
        if (samples == 0) {
            continue;
        }

        // Source: generate the per-channel sine into the interleaved buffer.
        for (size_t i = 0; i < samples; ++i) {
            for (size_t ch = 0; ch < channels_; ++ch) {
                audio_buffer_[(i * channels_) + ch] = oscillators_[ch](0.0F);
            }
        }
        // Per-channel biquad filters (interleaved layout).
        for (size_t i = 0; i < samples; ++i) {
            for (size_t ch = 0; ch < channels_; ++ch) {
                audio_buffer_[(i * channels_) + ch] = biquads_[ch](audio_buffer_[(i * channels_) + ch]);
            }
        }

        // Inter-site egress: if active, replace the oscillator content with the
        // de-tunneled audio for this packet's presentation time (now + p*125us).
        // The talkers below then emit the received stream instead of the test tone.
        if (udptun_->egress_active_ && udptun_now_tai_ns != 0) {
            int64_t const pkt_tai = udptun_now_tai_ns + (static_cast<int64_t>(p) * static_cast<int64_t>(PACKET_INTERVAL_NS));
            udptun_->udptun_egress_fill(pkt_tai, samples);
        }

        // Inter-site ingest silence source: emit zero PCM at the media cadence so
        // the entity transmits silence as if its listener source were sending
        // zeros (no real talker). Keeps the reverse tunnel + its NAT pinhole warm.
        // Gated (udptun_emit_silence, decided once per wake) to stand down the
        // instant real AVTP audio arrives -- otherwise the silence would
        // double-feed and race the reactor thread's real-audio ingest.
        if (udptun_->enable_ && config_.sweep_enable && udptun_now_tai_ns != 0) {
            // Test-signal mode: the logarithmic sweep IS the tunnel source. Pace it
            // by the GPS-TAI tunnel clock (udptun_now_tai_ns = rate_tracker_ TAI,
            // gPTP-rate / GPS-epoch) -- exactly SAMPLE_RATE frames per second of TAI.
            // Emit the cumulative frame count implied by elapsed TAI, so the ingest
            // avtp_timestamp stays locked to TAI with zero drift and the far egress
            // (playing on the same GPS-TAI clock) never under/over-runs.
            int64_t const pkt_tai = udptun_now_tai_ns + (static_cast<int64_t>(p) * static_cast<int64_t>(PACKET_INTERVAL_NS));
            if (udptun_->sweep_tai_anchor_ns_ == 0) {
                udptun_->sweep_tai_anchor_ns_ = pkt_tai;
            }
            int64_t const elapsed = pkt_tai - udptun_->sweep_tai_anchor_ns_;
            auto const target = (elapsed > 0)
                ? static_cast<uint64_t>((elapsed * static_cast<int64_t>(SAMPLE_RATE)) / 1'000'000'000LL)
                : uint64_t{0};
            if (target > udptun_->sweep_frames_emitted_) {
                size_t n = static_cast<size_t>(target - udptun_->sweep_frames_emitted_);
                size_t const cap = static_cast<size_t>(SAMPLES_PER_PACKET) * 4;  // bound catch-up bursts
                if (n > cap) {
                    n = cap;
                }
                udptun_->udptun_ingest_sweep(n);
                udptun_->sweep_frames_emitted_ += n;
            }
        } else if (udptun_emit_silence) {
            udptun_->udptun_ingest_silence(samples);
        }

        if (audio_callback_) {
            audio_callback_(std::span{audio_buffer_}.first(samples * channels_), samples);
        }

        // Transmit both formats from the same audio with the deterministic
        // presentation timestamp of this packet's first sample. We do NOT gate on
        // the announce-based supervisor lock; process_audio() only runs while the
        // PTP bridge is healthy (the media timer fires), so we have a usable clock.
        uint64_t const pts_base = media_clock_.timestamp_for(tick.first_index);
        // Gate each talker stream on a ready/connected listener (SRP): don't put
        // a stream on the wire when nobody is listening. The CRF media clock is
        // emitted whenever any audio talker is transmitting (its listeners lock
        // to it for the audio streams).
        // Steady-clock now for the MSRP-ready grace window (same clock the
        // on_talker_listener callback stamps msrp_ready_ns_ with).
        int64_t const now_steady_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        bool const tx_am824 = talker_should_transmit(AM824_STREAM_INDEX, now_steady_ns);
        bool const tx_aaf = talker_should_transmit(AAF_STREAM_INDEX, now_steady_ns);
        if (tx_am824) {
            // AM824 tolerates the GPS-paced variable block count directly.
            talker_->transmit_am824(pts_base, tick.samples);
        }
        // AAF egress (reframer owned by TalkerStreams): buffer the variable-per-wake
        // samples and emit whole SAMPLES_PER_PACKET blocks; gate closed -> clear.
        talker_->transmit_aaf_if_due(tick, tx_aaf, samples);

        // CRF media-clock PDU (decimation owned by TalkerStreams). The CRF stream
        // gates on ITS OWN ACMP connection + reservation (a listener ACMP-connects and
        // MSRP-reserves the CRF media clock as a separate stream), never on the audio
        // streams' gate.
        talker_->transmit_crf_if_due(tick, talker_should_transmit(CRF_STREAM_INDEX, now_steady_ns));
    }
}

void AvbEntityAudioIO::update_gps_ratio(uint64_t gptp_now_ns)
{
    // Sample CLOCK_REALTIME (GPS, disciplined by chrony <- the GPS grandmaster)
    // against gPTP a few times per second; the rate tracker turns the offset
    // slope into r = switch/GPS, which pins the media-clock rate, and feeds the
    // same pair to its GPS-TAI translator for the tunnel timeline. Rate-limited
    // so the CLOCK_REALTIME syscall stays off the per-packet hot path.
    if (!rate_tracker_.should_sample(gptp_now_ns)) {
        return;
    }
    timespec ts{};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return;
    }
    uint64_t const gps_ns = (static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL) + static_cast<uint64_t>(ts.tv_nsec);
    auto const est = rate_tracker_.add_sample(gptp_now_ns, gps_ns);
    // Publish the updated GPS-TAI mapping for the reactor-thread tunnel ingest,
    // which must never read the single-threaded Kalman directly (data race).
    tai_snapshot_.publish(rate_tracker_.tai_snapshot());
    if (est.valid && rate_tracker_.should_log(gptp_now_ns)) {
        // Live media-clock telemetry (rate-limited; runs on the media-timer
        // thread, so keep it to once every few seconds). Lets an operator SEE the
        // GPS-vs-switch frequency tracking and catch a clock-coupling mistake:
        //   - r is switch_rate/GPS_rate; offset-slope = (r-1) in ppm = the
        //     fractional frequency between the gPTP PHC and GPS CLOCK_REALTIME.
        //   - PHC-REALTIME is the raw instantaneous phase offset (ns).
        // A HEALTHY decoupled setup (ptp4l -> /dev/ptp0 <- switch GM; chrony ->
        // CLOCK_REALTIME <- local GPS; NO phc2sys) shows a small but NON-ZERO,
        // stable slope -- the switch GM's free-running oscillator drifting against
        // GPS. A slope pinned at ~0.000 ppm with a flat offset means the PHC and
        // CLOCK_REALTIME are coupled (e.g. phc2sys is running) and the media clock
        // is NOT actually GPS-locked -- see umbrella docs/MEDIA_CLOCK_TIMING.md.
        std::print(
            stderr,
            "[media-clock] r(switch/GPS)={:.9f}  offset-slope={:+.3f} ppm  PHC-REALTIME={} ns  unc=+/-{:.1f} ppb\n",
            est.r,
            est.ppm(),
            static_cast<long long>(est.filtered_offset_ns),
            est.freq_uncertainty_ppb);
        rate_tracker_.mark_logged(gptp_now_ns);
    }
}

auto AvbEntityAudioIO::talker_should_transmit(uint16_t const idx, int64_t const now_ns) const noexcept -> bool
{
    // In MAAP mode, never transmit until a multicast address block is defended
    // (independent of the listener gate). Static mode leaves this flag set, so this
    // is a no-op there. Acquire-load pairs with the release-store in on_acquired so
    // the freshly-published dest MACs are visible before the first packet.
    if (!maap_addresses_ready_.load(std::memory_order_acquire)) {
        return false;
    }
    return gate_.should_transmit(idx, now_ns);
}

auto AvbEntityAudioIO::fill_stream_output_counters(
    uint16_t const descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& out) const -> bool
{
    // Our talker (STREAM_OUTPUT) descriptors: 0=AM824, 1=AAF, 2=CRF. Each maps to
    // its on-wire TX packet counter (tx counters are single-threaded with the
    // media timer, read plain like print_state()).
    uint64_t frames_tx = 0;
    switch (descriptor_index) {
        case AM824_STREAM_INDEX:
            frames_tx = talker_->am824_tx_packets_;
            break;
        case AAF_STREAM_INDEX:
            frames_tx = talker_->aaf_tx_packets_;
            break;
        case CRF_STREAM_INDEX:
            frames_tx = talker_->crf_out_ ? talker_->crf_out_->packets_sent : 0;
            break;
        default:
            return false;
    }
    // IEEE 1722.1 STREAM_OUTPUT counter bit positions (Clause 7.4.43). We expose
    // FRAMES_TX (bit 6) -- total media frames this talker has put on the wire --
    // which is what tells a reader our actual transmit rate.
    valid |= (1U << 6U);
    out[6] = static_cast<uint32_t>(frames_tx);
    return true;
}

auto AvbEntityAudioIO::fill_stream_output_info(
    uint16_t const descriptor_type, uint16_t const descriptor_index, atdecc::aem::AemStreamInfoPayload& out) const -> bool
{
    // Only our talker (STREAM_OUTPUT) streams carry GET_STREAM_INFO here.
    if (descriptor_type != DESCRIPTOR_STREAM_OUTPUT) {
        return false;
    }
    auto const* stream = host_.components().acmp_talker.get_stream(descriptor_index);
    if (stream == nullptr) {
        return false;
    }

    uint32_t flags = stream_info_flags::STREAM_ID_VALID | stream_info_flags::STREAM_DEST_MAC_VALID |
        stream_info_flags::STREAM_VLAN_ID_VALID | stream_info_flags::MSRP_ACC_LAT_VALID;

    // Stream format from the STREAM_OUTPUT descriptor's current_format (8 bytes),
    // read from the blob via the symbol-aware host (the parsed model is unused now).
    if (auto const desc = host_.get_descriptor(DESCRIPTOR_STREAM_OUTPUT, descriptor_index); desc.has_value()) {
        atdecc::aem::DescriptorStream stream_desc{};
        span_load_padded(stream_desc, *desc);
        auto const fspan = stream_desc.current_format.span();
        std::copy(fspan.begin(), fspan.end(), out.stream_format.begin());
        flags |= stream_info_flags::STREAM_FORMAT_VALID;
    }

    out.stream_id = stream->stream_id;
    auto const mspan = stream->stream_dest_mac.span();
    std::copy(mspan.begin(), mspan.end(), out.stream_dest_mac.begin());
    out.stream_vlan_id = ieee::doublet_t{stream->stream_vlan_id};
    out.msrp_accumulated_latency = ieee::quadlet_t{static_cast<uint32_t>(config_.presentation_offset_ns)};

    // SR class A is the entity's only class, so CLASS_B stays clear. Report the
    // live ACMP connection state so a controller/listener sees CONNECTED.
    if (host_.components().acmp_talker.connection_count(descriptor_index) > 0) {
        flags |= stream_info_flags::CONNECTED;
    }
    out.flags = ieee::quadlet_t{flags};
    return true;
}

void AvbEntityAudioIO::configure_filter(double const freq_hz, double const gain_db, double const q)
{
    double const sample_rate_recip = 1.0 / static_cast<double>(SAMPLE_RATE);
    for (size_t ch = 0; ch < channels_; ++ch) {
        biquads_[ch].coeffs.calculate_peak(
            {.sample_rate_recip = sample_rate_recip, .frequency = freq_hz, .q = q, .gain_db = gain_db, .channel = 0});
        biquads_[ch].reset();
    }
}

}  // namespace statusbar::avb_entity
