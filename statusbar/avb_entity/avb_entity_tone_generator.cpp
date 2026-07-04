// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity Tone Generator implementation (talker-only AM824 + AAF + CRF).
/// A blob-loaded model, a per-channel continuous sine source (white piano keys
/// by default), and three talker streams. The media clock is locked to gPTP
/// (r = 1.0); there is no listener, no GPS-rate tracking and no inter-site
/// tunnel. The control-plane wiring mirrors AvbEntityAudioIO's talker half.

#include "statusbar/avb_entity/avb_entity_tone_generator.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/avtp/avtp_crf.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/dsp/dsp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/nanoavb/nanoavb_acmp.hpp"
#include "statusbar/nanoavb/nanoavb_adp.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_posix_util.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/catch_or_status.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <print>
#include <span>
#include <string_view>
#include <utility>

namespace statusbar::avb_entity {

using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::nanoavb;

auto white_key_frequency_hz(uint8_t const base_midi_note, size_t const white_index) noexcept -> double
{
    // White keys are the natural notes; semitone offsets within an octave.
    static constexpr std::array<int, 7> kWhiteSemis{0, 2, 4, 5, 7, 9, 11};
    auto const octave = static_cast<int>(white_index / 7);
    auto const within = static_cast<size_t>(white_index % 7);
    int const midi = static_cast<int>(base_midi_note) + (octave * 12) + kWhiteSemis[within];
    return 440.0 * std::pow(2.0, (static_cast<double>(midi) - 69.0) / 12.0);
}

namespace {

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

/// Channel count from the first AUDIO_CLUSTER descriptor (default 8). In the
/// tone-generator model AUDIO_CLUSTER 0 is the first OUTPUT cluster.
auto channels_from_storage(DescriptorStorage const& storage) -> size_t
{
    size_t channels = 8;
    if (auto r = load_descriptor<DescriptorAudioCluster>(storage, 0, DESCRIPTOR_AUDIO_CLUSTER, 0)) {
        auto const ch = static_cast<uint16_t>(r->channel_count);
        if (ch >= 1) {
            channels = static_cast<size_t>(ch);
        }
    }
    return channels;
}

/// Serves the entity's descriptors from its .aem blob (symbol-aware), patching
/// the runtime-only ENTITY identity and AVB_INTERFACE network/gPTP fields.
class ToneGenDescriptorHandler : public nanoavb::DescriptorStorageHandler
{
  public:
    ToneGenDescriptorHandler(DescriptorStorage storage, AvbEntityAudioIOConfig const& config, std::optional<ieee::Eui48> iface_mac)
        : DescriptorStorageHandler{storage}
        , entity_id_{config.entity_id}
        , entity_model_id_{config.entity_model_id}
        , firmware_version_{config.firmware_version}
        , iface_mac_{iface_mac}
    {
        manage_entity_name(AtdeccString{config.entity_name.c_str()});
    }

    auto on_get_entity(DescriptorRef ref, uint32_t symbol, DescriptorEntity& desc) -> bool override
    {
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
        desc.priority1 = 248;
        desc.clock_class = 248;  // slave-only (not grandmaster-capable)
        desc.offset_scaled_log_variance = 0x436A;
        desc.clock_accuracy = 0xFE;
        desc.priority2 = 248;
        desc.domain_number = 0;
        desc.log_sync_interval = static_cast<uint8_t>(static_cast<int8_t>(-3));  // 125 ms
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

/// Globally-unique IEEE 1722 stream_id from the talker's NIC MAC (high 6 bytes)
/// plus a per-stream index in the low byte.
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
// Factory
//

auto AvbEntityToneGenerator::create(
    AvbEntityAudioIOConfig config, uint8_t base_midi_note, StreamSet streams, std::pmr::memory_resource* memory_resource)
    -> StatusValue<std::unique_ptr<AvbEntityToneGenerator>>
{
    auto storage_result = DescriptorStorage::create(std::span<uint8_t const>{config.descriptor_storage_blob});
    if (!storage_result) {
        return failure(storage_result.error());
    }

    size_t const channels = channels_from_storage(*storage_result);
    auto const iface_mac = net::read_interface_mac(config.interface_name);
    auto handler = std::make_unique<ToneGenDescriptorHandler>(*storage_result, config, iface_mac);

    std::pmr::memory_resource* const mr = memory_resource != nullptr ? memory_resource : std::pmr::get_default_resource();
    auto entity = std::make_unique<AvbEntityToneGenerator>(
        AvbEntityToneGenerator::CreateKey{}, std::move(config), std::move(handler), channels, base_midi_note, streams, mr);
    return success(std::move(entity));
}

//
// Constructor / Destructor
//

AvbEntityToneGenerator::AvbEntityToneGenerator(
    CreateKey,
    AvbEntityAudioIOConfig config,
    std::unique_ptr<nanoavb::AemEntityHandler> handler,
    size_t channels,
    uint8_t base_midi_note,
    StreamSet streams,
    std::pmr::memory_resource* memory_resource)
    : config_{std::move(config)}  // Talker streams: 3 (AM824+AAF+CRF) for All, 2 (AAF+CRF) for AafCrf, 1 (AAF)
    // for AafOnly. 4 max listeners each, 0 listener streams (talker-only).
    , host_{std::move(handler), make_adp_config(), (streams == StreamSet::All ? size_t{3} : (streams == StreamSet::AafCrf ? size_t{2} : size_t{1})), 4, 0}
    , has_am824_{streams == StreamSet::All}
    , has_crf_{streams == StreamSet::All || streams == StreamSet::AafCrf}
    , aaf_idx_{streams == StreamSet::All ? AAF_STREAM_INDEX : uint16_t{0}}
    , crf_idx_{streams == StreamSet::All ? CRF_STREAM_INDEX : uint16_t{1}}
    , channels_{channels}
    , mem_resource_{memory_resource}
    , audio_buffer_((SAMPLES_PER_PACKET + 1) * channels, 0.0F, mem_resource_)  // +1: gPTP pacing may emit nominal+1
    , oscillators_(channels, dsp::Oscillator<float>{}, mem_resource_)
{
    // Per-channel continuous sine: each channel is the next white piano key up
    // from base_midi_note (default C2). Amplitude is shared (config tone level).
    double const sr_recip = 1.0 / static_cast<double>(SAMPLE_RATE);
    for (size_t ch = 0; ch < channels_; ++ch) {
        oscillators_[ch].state_.set_frequency(
            dsp::FrequencyParameters<double>{
                .sample_rate_recip = sr_recip, .frequency = white_key_frequency_hz(base_midi_note, ch), .phase_in_radians = 0.0},
            0);
        oscillators_[ch].coeffs_.set_amplitude(config_.tone_amplitude, 0);
    }

    host_.components().aem_handler.set_legacy_2016(config_.atdecc_version != "2021");

    // Base the talker stream_ids on the NIC MAC (globally unique per box).
    ieee::Eui48 stream_base_mac{};
    if (auto const mac = net::read_interface_mac(config_.interface_name)) {
        stream_base_mac = *mac;
    } else {
        span_copy(stream_base_mac.span(), config_.entity_id.span().first(6));
    }

    // AAF is always present; AM824 / CRF per the stream set.
    if (has_am824_) {
        (void)host_.components().acmp_talker.configure_stream(
            am824_idx_, stream_id_for(stream_base_mac, am824_idx_), config_.am824_talker_dest_mac);
    }
    (void)host_.components().acmp_talker.configure_stream(
        aaf_idx_, stream_id_for(stream_base_mac, aaf_idx_), config_.aaf_talker_dest_mac);
    if (has_crf_) {
        (void)host_.components().acmp_talker.configure_stream(
            crf_idx_, stream_id_for(stream_base_mac, crf_idx_), config_.crf_talker_dest_mac);
    }

    (void)host_.components().mvrp_handler.register_vlan(config_.vlan_id, sm::Clock::now());
    host_.components().msrp_handler.set_domain(
        DomainInfo{.sr_class_id = 6, .sr_class_priority = 3, .sr_class_vid = config_.vlan_id});
    host_.components().msrp_handler.set_redeclare_registered_listeners(config_.redeclare_registered_listeners);
    host_.components().msrp_handler.set_suppress_leaveall(config_.suppress_leaveall);
}

AvbEntityToneGenerator::~AvbEntityToneGenerator()
{
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
// Stream-specific control-plane wiring (talker-only)
//

void AvbEntityToneGenerator::wire_stream_callbacks()
{
    host_.set_advertise_streams([this](TimePoint time) { advertise_talker_streams(time); });
    host_.set_withdraw_streams([this](TimePoint time) {
        for (uint16_t const idx : active_stream_indices()) {
            (void)host_.components().msrp_handler.talker_withdraw(make_talker_srp_info(idx).stream_id, time);
        }
    });
    host_.set_on_listener_ready(
        [this](nanoavb::StreamId const& stream_id, bool ready) { gate_.note_listener_ready(stream_id, ready); });

    host_.components().acmp_talker.set_connection_callbacks(
        [this](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            std::print(
                "[acmp] talker stream {} CONNECTED by listener {:012x} unique_id {}\n",
                stream_index,
                listener_entity_id.to_uint64(),
                listener_unique_id);
            // Publish the fresh connection count for the media-timer gate.
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

    // AECP GET_COUNTERS (STREAM_OUTPUT talker rate) + GET_STREAM_INFO. No
    // STREAM_INPUT branch: this entity has no listener sinks.
    host_.components().aem_handler.set_get_counters(
        [this](uint16_t descriptor_type, uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& counters) -> bool {
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

auto AvbEntityToneGenerator::acquire_maap_addresses(net::MessageReactor& reactor) -> Status
{
    ieee::Eui48 our_mac{};
    if (auto const mac = net::read_interface_mac(config_.interface_name)) {
        our_mac = *mac;
    } else {
        span_copy(our_mac.span(), config_.entity_id.span().first(6));
    }

    statusbar::tsn::StreamId maap_sid{};
    if (auto const* s = host_.components().acmp_talker.get_stream(active_stream_indices().front()); s != nullptr) {
        (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &maap_sid);
    }
    maap_handler_ = std::make_unique<avtp::MaapHandler>(our_mac, maap_sid, our_mac.to_uint64());

    auto const block_count = static_cast<uint16_t>(active_stream_indices().size());  // 1 (AAF), 2 (AAF+CRF) or 3
    maap_addresses_ready_.store(false, std::memory_order_release);

    maap_handler_->set_on_acquired([this, block_count](ieee::Eui48 const& block_start, uint16_t /*count*/) {
        auto assign = [this, &block_start](uint16_t idx, uint16_t offset, ieee::Eui48& tx_cache) {
            ieee::Eui48 const dest = avtp::maap_block_address(block_start, offset);
            if (auto const* s = host_.components().acmp_talker.get_stream(idx); s != nullptr) {
                (void)host_.components().acmp_talker.configure_stream(idx, s->stream_id, dest);
            }
            tx_cache = dest;
        };
        // Assign a block address per active stream, ordered by ascending index.
        if (has_am824_) {
            assign(am824_idx_, am824_idx_, talker_->am824_dest_mac_);
        }
        assign(aaf_idx_, aaf_idx_, talker_->aaf_dest_mac_);
        if (has_crf_) {
            assign(crf_idx_, crf_idx_, talker_->crf_dest_mac_);
        }
        maap_addresses_ready_.store(true, std::memory_order_release);
        std::print(stderr, "MAAP: acquired {} stream address(es) from {}\n", block_count, ieee::to_string(block_start).view());
        if (host_.is_ready()) {
            advertise_talker_streams(sm::Clock::now());
        }
    });

    maap_handler_->set_on_lost([this](ieee::Eui48 const& /*start*/, uint16_t /*count*/) {
        maap_addresses_ready_.store(false, std::memory_order_release);
        std::print(stderr, "Warning: MAAP address lost to a conflict; re-acquiring\n");
    });

    auto net_handler = std::make_unique<nanoavb::MaapNetHandler>(config_.interface_name, *maap_handler_);
    if (!net_handler->valid()) {
        std::print(stderr, "Warning: MAAP socket open failed on {}; using static stream dest MACs\n", config_.interface_name);
        maap_handler_.reset();
        maap_addresses_ready_.store(true, std::memory_order_release);
        return {};
    }

    reactor.add(std::move(net_handler));
    maap_handler_->acquire(block_count, net::monotonic_ns());
    return {};
}

auto AvbEntityToneGenerator::start(net::MessageReactor& reactor) -> Status
{
    if (auto status = host_.start_control_plane(reactor, config_.interface_name); !status) {
        return status;
    }
    wire_stream_callbacks();

    if (config_.stream_address_mode == "maap") {
        if (auto status = acquire_maap_addresses(reactor); !status) {
            return status;
        }
    }

    // Resolve the talker stream id (+ dest MAC) for the AAF stream (always present).
    statusbar::tsn::StreamId aaf_sid{};
    if (auto const* s = host_.components().acmp_talker.get_stream(aaf_idx_); s != nullptr) {
        (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &aaf_sid);
        talker_->aaf_dest_mac_ = s->stream_dest_mac;
    }

    // Deterministic media clock owns the presentation offset; the stream-output
    // contexts add zero extra offset.
    media_clock_ = ptpclient::MediaClockGenerator{ptpclient::MediaClockGenerator::Config{
        .sample_rate_hz = static_cast<double>(SAMPLE_RATE), .presentation_offset_ns = config_.presentation_offset_ns}};

    talker_->aaf_out_.emplace(
        aaf_sid, AAF_FORMAT, AAF_SAMPLE_RATE, static_cast<uint16_t>(channels_), AAF_BIT_DEPTH, /*presentation_offset_ns=*/0);

    if (has_am824_) {
        statusbar::tsn::StreamId am824_sid{};
        if (auto const* s = host_.components().acmp_talker.get_stream(am824_idx_); s != nullptr) {
            (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &am824_sid);
            talker_->am824_dest_mac_ = s->stream_dest_mac;
        }
        talker_->am824_out_.emplace(
            am824_sid, avtp::Am824SampleRate::rate_96_khz, static_cast<uint8_t>(channels_), /*presentation_offset_ns=*/0);
    }
    if (has_crf_) {
        statusbar::tsn::StreamId crf_sid{};
        if (auto const* s = host_.components().acmp_talker.get_stream(crf_idx_); s != nullptr) {
            (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &crf_sid);
            talker_->crf_dest_mac_ = s->stream_dest_mac;
        }
        talker_->crf_out_.emplace(
            crf_sid,
            avtp::CrfType::audio_sample,
            CRF_BASE_FREQUENCY,
            avtp::CrfPull::multiply_1_0,
            config_.crf_timestamp_interval,
            config_.crf_timestamps_per_packet);
    }

    // One TX socket (qdisc-bypass so our own egress is not re-received).
    (void)talker_->stream_tx_.open(config_.interface_name, avtp::AVTP_ETHERTYPE, nullptr, /*qdisc_bypass=*/true);

    if (!config_.tx_pcap_path.empty()) {
        talker_->tx_pcap_recorder_.configure(
            config_.tx_pcap_path,
            config_.tx_pcap_max_bytes,
            /*snaplen=*/1522,
            static_cast<uint64_t>(config_.tx_pcap_seconds) * 1'000'000'000ULL);
        talker_->stream_tx_.set_tx_tap(
            [this](std::span<uint8_t const> frame) { talker_->tx_pcap_recorder_.record(frame, talker_->last_tx_gptp_ns_); });
    }

    return success();
}

auto AvbEntityToneGenerator::stop() -> Status
{
    if (!host_.is_running()) {
        return failure(std::make_error_code(std::errc::not_connected));
    }
    auto const now = TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    return host_.stop_control_plane(now);
}

void AvbEntityToneGenerator::print_state() const
{
    std::print(
        "State: supervisor={} gptp={} mvrp={} acmp[am824={} aaf={} crf={}] channels={} | "
        "AM824 tx={} | AAF tx={} | CRF tx={}\n",
        state_string(),
        host_.gptp_locked() ? "Locked" : "Unlocked",
        host_.mvrp_joined() ? "Joined" : "NotJoined",
        host_.components().acmp_talker.connection_count(AM824_STREAM_INDEX),
        host_.components().acmp_talker.connection_count(AAF_STREAM_INDEX),
        host_.components().acmp_talker.connection_count(CRF_STREAM_INDEX),
        channels_,
        talker_->am824_tx_packets_,
        talker_->aaf_tx_packets_,
        talker_->crf_out_ ? talker_->crf_out_->packets_sent : 0);
}

//
// MSRP talker reservation
//

auto AvbEntityToneGenerator::active_stream_indices() const -> std::vector<uint16_t>
{
    std::vector<uint16_t> idx;
    if (has_am824_) {
        idx.push_back(am824_idx_);
    }
    idx.push_back(aaf_idx_);
    if (has_crf_) {
        idx.push_back(crf_idx_);
    }
    return idx;
}

auto AvbEntityToneGenerator::make_talker_srp_info(uint16_t stream_index) const -> nanoavb::TalkerStreamSrpInfo
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
    bool const is_crf = has_crf_ && stream_index == crf_idx_;
    bool const is_aaf = stream_index == aaf_idx_;
    if (is_crf) {
        info.max_frame_size =
            static_cast<uint16_t>(avtp::CrfPdu::HEADER_LENGTH + (config_.crf_timestamps_per_packet * avtp::CrfPdu::TIMESTAMP_SIZE));
    } else {
        uint16_t const header_bytes = is_aaf ? avtp::AafPdu::HEADER_LENGTH : uint16_t{32};
        info.max_frame_size = static_cast<uint16_t>(header_bytes + ((SAMPLES_PER_PACKET + 1) * channels_ * QUADLET_BYTES));
    }
    info.accumulated_latency = 0;
    return info;
}

void AvbEntityToneGenerator::advertise_talker_streams(TimePoint const time)
{
    if (!maap_addresses_ready_.load(std::memory_order_acquire)) {
        return;
    }
    for (uint16_t const idx : active_stream_indices()) {
        auto result = host_.components().msrp_handler.talker_advertise(make_talker_srp_info(idx), time);
        if (!result) {
            std::print(stderr, "Warning: MSRP talker_advertise (stream {}) failed: {}\n", idx, result.error().message());
        }
    }
}

//
// Event handlers (drive the shared SM stack)
//

void AvbEntityToneGenerator::on_link_up(TimePoint time)
{
    host_.on_link_up(time);
}
void AvbEntityToneGenerator::on_link_down(TimePoint time)
{
    host_.on_link_down(time);
}
void AvbEntityToneGenerator::on_gptp_announce(TimePoint time, bool has_grandmaster)
{
    host_.on_gptp_announce(time, has_grandmaster);
}
void AvbEntityToneGenerator::on_timeout(TimePoint time)
{
    host_.on_timeout(time);
}

//
// Audio processing
//

void AvbEntityToneGenerator::process_audio(TimePoint time)
{
    auto const base_now_ns = static_cast<uint64_t>(time.time_since_epoch().count());
    constexpr uint64_t PACKET_INTERVAL_NS = (static_cast<uint64_t>(SAMPLES_PER_PACKET) * 1'000'000'000ULL) / SAMPLE_RATE;

    for (size_t p = 0; p < config_.packets_per_wake; ++p) {
        uint64_t const wake_ns = base_now_ns + (static_cast<uint64_t>(p) * PACKET_INTERVAL_NS);
        last_gptp_ns_.store(wake_ns, std::memory_order_relaxed);

        // Media clock locked to gPTP: r = 1.0 (no GPS-rate tracking). The
        // avtp_timestamp comes from the deterministic generator, not the jittery
        // wake time, so a recovering listener stays steady.
        auto const tick = media_clock_.advance(wake_ns, /*r=*/1.0, static_cast<uint32_t>(SAMPLES_PER_PACKET));
        auto const samples = static_cast<size_t>(tick.samples);
        if (samples == 0) {
            continue;
        }

        for (size_t i = 0; i < samples; ++i) {
            for (size_t ch = 0; ch < channels_; ++ch) {
                audio_buffer_[(i * channels_) + ch] = oscillators_[ch](0.0F);
            }
        }

        uint64_t const pts_base = media_clock_.timestamp_for(tick.first_index);
        int64_t const now_steady_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        bool const tx_aaf = talker_should_transmit(aaf_idx_, now_steady_ns);
        bool const tx_am824 = has_am824_ && talker_should_transmit(am824_idx_, now_steady_ns);
        if (tx_am824) {
            talker_->transmit_am824(pts_base, tick.samples);
        }
        // AAF egress (reframer owned by TalkerStreams): buffer the variable-per-wake
        // samples and emit whole SAMPLES_PER_PACKET blocks; gate closed -> clear.
        talker_->transmit_aaf_if_due(tick, tx_aaf, samples);

        // CRF media-clock PDU (decimation owned by TalkerStreams). The CRF stream is a
        // first-class stream that gates on ITS OWN ACMP connection + reservation, never
        // on the audio streams' gate; skipped entirely when this set has no CRF stream.
        if (has_crf_) {
            talker_->transmit_crf_if_due(tick, talker_should_transmit(crf_idx_, now_steady_ns));
        }
    }
}

auto AvbEntityToneGenerator::talker_should_transmit(uint16_t const idx, int64_t const now_ns) const noexcept -> bool
{
    if (!maap_addresses_ready_.load(std::memory_order_acquire)) {
        return false;
    }
    return gate_.should_transmit(idx, now_ns);
}

auto AvbEntityToneGenerator::fill_stream_output_counters(
    uint16_t const descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& out) const -> bool
{
    uint64_t frames_tx = 0;
    if (descriptor_index == aaf_idx_) {
        frames_tx = talker_->aaf_tx_packets_;
    } else if (has_am824_ && descriptor_index == am824_idx_) {
        frames_tx = talker_->am824_tx_packets_;
    } else if (has_crf_ && descriptor_index == crf_idx_) {
        frames_tx = talker_->crf_out_ ? talker_->crf_out_->packets_sent : 0;
    } else {
        return false;
    }
    valid |= (1U << 6U);  // FRAMES_TX (IEEE 1722.1 Clause 7.4.43)
    out[6] = static_cast<uint32_t>(frames_tx);
    return true;
}

auto AvbEntityToneGenerator::fill_stream_output_info(
    uint16_t const descriptor_type, uint16_t const descriptor_index, atdecc::aem::AemStreamInfoPayload& out) const -> bool
{
    if (descriptor_type != DESCRIPTOR_STREAM_OUTPUT) {
        return false;
    }
    auto const* stream = host_.components().acmp_talker.get_stream(descriptor_index);
    if (stream == nullptr) {
        return false;
    }

    uint32_t flags = stream_info_flags::STREAM_ID_VALID | stream_info_flags::STREAM_DEST_MAC_VALID |
        stream_info_flags::STREAM_VLAN_ID_VALID | stream_info_flags::MSRP_ACC_LAT_VALID;

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

    if (host_.components().acmp_talker.connection_count(descriptor_index) > 0) {
        flags |= stream_info_flags::CONNECTED;
    }
    out.flags = ieee::quadlet_t{flags};
    return true;
}

}  // namespace statusbar::avb_entity
