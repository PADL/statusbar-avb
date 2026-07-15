// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity Tone Generator implementation (talker-only).
/// A blob-loaded model, a per-channel continuous sine source (white piano keys
/// by default), and N talker streams shaped by the blob's STREAM_OUTPUT
/// descriptors (Entity Construction Kit phase 1). The media clock is locked to
/// gPTP (r = 1.0); there is no listener, no GPS-rate tracking and no
/// inter-site tunnel. The control-plane wiring mirrors AvbEntityAudioIO's
/// talker half.

#include "statusbar/avb_entity/avb_entity_tone_generator.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"
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
    AvbEntityAudioIOConfig config, uint8_t base_midi_note, std::pmr::memory_resource* memory_resource)
    -> StatusValue<std::unique_ptr<AvbEntityToneGenerator>>
{
    auto storage_result = DescriptorStorage::create(std::span<uint8_t const>{config.descriptor_storage_blob});
    if (!storage_result) {
        return failure(storage_result.error());
    }

    // Stream topology from the blob (kit phase 1): kinds/formats/rates per
    // STREAM_OUTPUT descriptor. The blob and the data plane can no longer
    // silently disagree — every mismatch below is a loud create-time failure.
    auto specs_result = talker_stream_specs(*storage_result, 0);
    if (!specs_result) {
        return failure(specs_result.error());
    }
    auto const& specs = *specs_result;
    if (specs.empty()) {
        return failure(std::errc::invalid_argument);  // a talker with nothing to talk
    }
    for (auto const& spec : specs) {
        if (spec.format.kind == StreamKind::other) {
            return failure(std::errc::not_supported);  // unrecognized stream format word
        }
    }
    auto listener_specs = listener_stream_specs(*storage_result, 0);
    if (!listener_specs) {
        return failure(listener_specs.error());
    }
    if (!listener_specs->empty()) {
        return failure(std::errc::invalid_argument);  // talker-only entity, blob declares inputs
    }

    auto const rate = common_audio_sample_rate(specs, DEFAULT_SAMPLE_RATE);
    if (!rate) {
        return failure(rate.error());  // mixed audio rates need per-domain clocks (phase 3)
    }

    size_t const channels = channels_from_storage(*storage_result, 8);
    for (auto const& spec : specs) {
        bool const is_audio = spec.format.kind == StreamKind::am824 || spec.format.kind == StreamKind::aaf;
        if (is_audio && spec.format.channels != channels) {
            return failure(std::errc::invalid_argument);  // stream format vs AUDIO_CLUSTER channel mismatch
        }
    }

    auto const iface_mac = net::read_interface_mac(config.interface_name);
    auto handler = std::make_unique<EntityIdentityDescriptorHandler>(
        *storage_result,
        config.entity_id,
        config.entity_model_id,
        config.firmware_version,
        config.entity_name,
        iface_mac,
        /*patch_avb_interface=*/true);

    auto* const storage_handler = handler.get();
    std::pmr::memory_resource* const mr = memory_resource != nullptr ? memory_resource : std::pmr::get_default_resource();
    auto entity = std::make_unique<AvbEntityToneGenerator>(
        AvbEntityToneGenerator::CreateKey{},
        std::move(config),
        std::move(handler),
        storage_handler,
        specs,
        *rate,
        channels,
        base_midi_note,
        mr);
    return success(std::move(entity));
}

//
// Constructor / Destructor
//

AvbEntityToneGenerator::AvbEntityToneGenerator(
    CreateKey,
    AvbEntityAudioIOConfig config,
    std::unique_ptr<nanoavb::AemEntityHandler> handler,
    nanoavb::DescriptorStorageHandler* storage_handler,
    StreamSpecs specs,
    uint32_t sample_rate,
    size_t channels,
    uint8_t base_midi_note,
    std::pmr::memory_resource* memory_resource)
    : config_{std::move(config)}
    , specs_{specs}  // Talker stream count from the blob; 4 max listeners each, 0 listener
    // streams (talker-only).
    , host_{std::move(handler), default_adp_advertiser_config(), specs.size(), 4, 0}
    , sample_rate_{sample_rate}
    , samples_per_packet_{sample_rate / CLASS_A_PACKETS_PER_SEC}
    , channels_{channels}
    , mem_resource_{memory_resource}
    , audio_buffer_((static_cast<size_t>(samples_per_packet_) + 1) * channels, 0.0F, mem_resource_)  // +1: gPTP pacing
    , oscillators_(channels, dsp::Oscillator<float>{}, mem_resource_)
{
    // Per-channel continuous sine: each channel is the next white piano key up
    // from base_midi_note (default C4). Amplitude is shared (config tone level).
    double const sr_recip = 1.0 / static_cast<double>(sample_rate_);
    for (size_t ch = 0; ch < channels_; ++ch) {
        oscillators_[ch].state_.set_frequency(
            dsp::FrequencyParameters<double>{
                .sample_rate_recip = sr_recip, .frequency = white_key_frequency_hz(base_midi_note, ch), .phase_in_radians = 0.0},
            0);
        oscillators_[ch].coeffs_.set_amplitude(config_.tone_amplitude, 0);
    }

    storage_handler_ = storage_handler;

    // Per-control dispatch (kit phase 4): the generic CONTROL built-in calls
    // back with each accepted SET_CONTROL; bound symbols get their handler's
    // verdict, everything else is accepted (store/serve only).
    if (storage_handler_ != nullptr) {
        storage_handler_->set_on_control_changed([this](uint16_t control_index, std::span<uint8_t const> value) -> uint8_t {
            for (auto& binding : control_bindings_) {
                if (binding.resolved && binding.control_index == control_index && binding.fn) {
                    return binding.fn(value);
                }
            }
            return atdecc::AEM_STATUS_SUCCESS;
        });
        // Kit phase 5: a controller's STOP_STREAMING gates the talker slot
        // (the media thread treats it as a closed SRP gate); START reopens.
        storage_handler_->set_on_streaming_changed([this](uint16_t type, uint16_t index, bool streaming) -> uint8_t {
            if (type == DESCRIPTOR_STREAM_OUTPUT) {
                talker_->set_stream_stopped(index, !streaming);
            }
            return atdecc::AEM_STATUS_SUCCESS;
        });
    }

    // Per-stream render buffers, parallel to specs_: audio slots get an
    // interleaved tick buffer; CRF/non-audio slots an empty one.
    for (auto const& spec : specs_) {
        bool const is_audio = spec.format.kind == StreamKind::am824 || spec.format.kind == StreamKind::aaf;
        render_buffers_.emplace_back(
            is_audio ? (static_cast<size_t>(samples_per_packet_) + 1) * channels_ : 0, 0.0F, mem_resource_);
    }

    host_.components().aem_handler.set_legacy_2016(config_.atdecc_version != "2021");

    // Base the talker stream_ids on the NIC MAC (globally unique per box).
    ieee::Eui48 stream_base_mac{};
    if (auto const mac = net::read_interface_mac(config_.interface_name)) {
        stream_base_mac = *mac;
    } else {
        span_copy(stream_base_mac.span(), config_.entity_id.span().first(6));
    }

    // Static dest MACs by kind (MAAP mode reassigns after acquisition).
    for (auto const& spec : specs_) {
        ieee::Eui48 dest{};
        switch (spec.format.kind) {
            case StreamKind::am824:
                dest = config_.am824_talker_dest_mac;
                break;
            case StreamKind::aaf:
                dest = config_.aaf_talker_dest_mac;
                break;
            case StreamKind::crf:
                dest = config_.crf_talker_dest_mac;
                break;
            case StreamKind::other:
            default:
                break;
        }
        (void)host_.components().acmp_talker.configure_stream(spec.index, stream_id_for(stream_base_mac, spec.index), dest);
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
// Per-stream TX sources (kit phase 2): the code is a menu, the model is the
// selection — registrations that match nothing in the loaded model are
// recorded as inert and listed at start(); they are never an error.
//

void AvbEntityToneGenerator::set_render(uint16_t const stream_index, StreamRenderFn fn)
{
    for (size_t pos = 0; pos < specs_.size(); ++pos) {
        auto const& spec = specs_[pos];
        bool const is_audio = spec.format.kind == StreamKind::am824 || spec.format.kind == StreamKind::aaf;
        if (is_audio && spec.index == stream_index) {
            renders_[pos] = std::move(fn);
            return;
        }
    }
    if (unbound_render_indices_.size() < MAX_ENTITY_STREAMS) {
        unbound_render_indices_.push_back(stream_index);
    }
}

void AvbEntityToneGenerator::set_render_symbol(uint32_t const symbol_code, StreamRenderFn fn)
{
    for (size_t pos = 0; pos < specs_.size(); ++pos) {
        auto const& spec = specs_[pos];
        bool const is_audio = spec.format.kind == StreamKind::am824 || spec.format.kind == StreamKind::aaf;
        auto const sym = host_.symbol_of(DESCRIPTOR_STREAM_OUTPUT, spec.index);
        if (is_audio && sym.has_value() && *sym == symbol_code) {
            renders_[pos] = std::move(fn);
            return;
        }
    }
    if (unbound_render_symbols_.size() < MAX_ENTITY_STREAMS) {
        unbound_render_symbols_.push_back(symbol_code);
    }
}

void AvbEntityToneGenerator::on_control_symbol(uint32_t const symbol_code, ControlChangedFn fn)
{
    ControlBinding binding{};
    binding.symbol = symbol_code;
    binding.fn = std::move(fn);
    if (auto const entry = host_.descriptor_for_symbol(symbol_code);
        entry.has_value() && entry->descriptor_type == atdecc::aem::DESCRIPTOR_CONTROL) {
        binding.control_index = entry->descriptor_index;
        binding.resolved = true;
    }
    for (auto& existing : control_bindings_) {
        if (existing.symbol == symbol_code) {
            existing = std::move(binding);
            return;
        }
    }
    if (control_bindings_.size() < MAX_CONTROL_BINDINGS) {
        control_bindings_.push_back(std::move(binding));
    }
}

//
// Stream-specific control-plane wiring (talker-only)
//

void AvbEntityToneGenerator::wire_stream_callbacks()
{
    host_.set_advertise_streams([this](TimePoint time) { advertise_talker_streams(time); });
    host_.set_withdraw_streams([this](TimePoint time) {
        for (auto const& spec : specs_) {
            (void)host_.components().msrp_handler.talker_withdraw(make_talker_srp_info(spec).stream_id, time);
        }
    });
    host_.set_on_listener_ready(
        [this](nanoavb::StreamId const& stream_id, bool ready) { gate_.note_listener_ready(stream_id, ready); });

    host_.components().acmp_talker.set_connection_callbacks(
        [this](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            host_.ctl_log().status(
                "acmp: talker stream {} CONNECTED by listener {:012x} unique_id {}",
                stream_index,
                listener_entity_id.to_uint64(),
                listener_unique_id);
            // Publish the fresh connection count for the media-timer gate.
            gate_.note_acmp_connections(
                stream_index, static_cast<uint32_t>(host_.components().acmp_talker.connection_count(stream_index)));
        },
        [this](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            host_.ctl_log().status(
                "acmp: talker stream {} DISCONNECTED by listener {:012x} unique_id {}",
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
    if (auto const* s = host_.components().acmp_talker.get_stream(specs_.front().index); s != nullptr) {
        (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &maap_sid);
    }
    maap_handler_ = std::make_unique<avtp::MaapHandler>(our_mac, maap_sid, our_mac.to_uint64());

    auto const block_count = static_cast<uint16_t>(specs_.size());
    maap_addresses_ready_.store(false, std::memory_order_release);

    maap_handler_->set_on_acquired([this, block_count](ieee::Eui48 const& block_start, uint16_t /*count*/) {
        // Assign a block address per stream, ordered by table position.
        for (size_t pos = 0; pos < specs_.size(); ++pos) {
            auto const& spec = specs_[pos];
            ieee::Eui48 const dest = avtp::maap_block_address(block_start, static_cast<uint16_t>(pos));
            if (auto const* s = host_.components().acmp_talker.get_stream(spec.index); s != nullptr) {
                (void)host_.components().acmp_talker.configure_stream(spec.index, s->stream_id, dest);
            }
            if (auto* slot = talker_->slot_for(spec.index); slot != nullptr) {
                slot->dest_mac = dest;
            }
        }
        maap_addresses_ready_.store(true, std::memory_order_release);
        host_.ctl_log().status("maap: acquired {} stream address(es) from {:012x}", block_count, block_start.to_uint64());
        if (host_.is_ready()) {
            advertise_talker_streams(sm::Clock::now());
        }
    });

    maap_handler_->set_on_lost([this](ieee::Eui48 const& /*start*/, uint16_t /*count*/) {
        maap_addresses_ready_.store(false, std::memory_order_release);
        host_.ctl_log().warning("maap: address lost to a conflict; re-acquiring");
    });

    auto net_handler = std::make_unique<nanoavb::MaapNetHandler>(config_.interface_name, *maap_handler_);
    if (!net_handler->valid()) {
        host_.ctl_log().warning("maap: socket open failed; using static stream dest MACs");
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
    gate_.set_logger(host_.ctl_log());
    if (auto status = host_.start_control_plane(reactor, config_.interface_name); !status) {
        return status;
    }
    wire_stream_callbacks();

    if (config_.stream_address_mode == "maap") {
        if (auto status = acquire_maap_addresses(reactor); !status) {
            return status;
        }
    }

    // Deterministic media clock owns the presentation offset; the stream-output
    // contexts add zero extra offset.
    media_clock_ = ptpclient::MediaClockGenerator{ptpclient::MediaClockGenerator::Config{
        .sample_rate_hz = static_cast<double>(sample_rate_), .presentation_offset_ns = config_.presentation_offset_ns}};

    // One TX slot per blob-declared stream, shaped by its format word.
    for (auto const& spec : specs_) {
        statusbar::tsn::StreamId sid{};
        ieee::Eui48 dest{};
        if (auto const* s = host_.components().acmp_talker.get_stream(spec.index); s != nullptr) {
            (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &sid);
            dest = s->stream_dest_mac;
        }
        if (auto status = talker_->open_stream(spec, sid, dest); !status) {
            return status;
        }
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

    // Menu/selection diagnostics: registrations the model left inert. Info
    // level -- typo-finding, never an error.
    for (auto const code : unbound_render_symbols_) {
        host_.ctl_log().status("render: registered symbol 0x{:08x} not in this model (inert)", code);
    }
    for (auto const idx : unbound_render_indices_) {
        host_.ctl_log().status("render: registered stream index {} not an audio stream in this model (inert)", idx);
    }
    for (auto const& binding : control_bindings_) {
        if (!binding.resolved) {
            host_.ctl_log().status("control: registered symbol 0x{:08x} not a CONTROL in this model (inert)", binding.symbol);
        }
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
        "State: supervisor={} gptp={} mvrp={} channels={} rate={}",
        state_string(),
        host_.gptp_locked() ? "Locked" : "Unlocked",
        host_.mvrp_joined() ? "Joined" : "NotJoined",
        channels_,
        sample_rate_);
    for (auto const& spec : specs_) {
        auto const* slot = talker_->slot_for(spec.index);
        std::print(
            " | {}[{}] acmp={} tx={}",
            stream_kind_name(spec.format.kind),
            spec.index,
            host_.components().acmp_talker.connection_count(spec.index),
            slot != nullptr ? slot->tx_packets : 0);
    }
    std::print("\n");
}

//
// MSRP talker reservation
//

auto AvbEntityToneGenerator::make_talker_srp_info(StreamSpec const& spec) const -> nanoavb::TalkerStreamSrpInfo
{
    nanoavb::TalkerStreamSrpInfo info{};
    if (auto const* stream = host_.components().acmp_talker.get_stream(spec.index); stream != nullptr) {
        (void)statusbar::tsn::load_unchecked(stream->stream_id.span(), &info.stream_id);
        info.dest_address = stream->stream_dest_mac;
        info.vlan_id = stream->stream_vlan_id;
    } else {
        info.vlan_id = config_.vlan_id;
    }
    info.max_interval_frames = 1;
    // TSpec frame size from the blob's format word (kit phase 1) — no more
    // per-entity byte math to keep in step with the model.
    info.max_frame_size = srp_max_frame_size(spec.format);
    info.accumulated_latency = 0;
    return info;
}

void AvbEntityToneGenerator::advertise_talker_streams(TimePoint const time)
{
    if (!maap_addresses_ready_.load(std::memory_order_acquire)) {
        return;
    }
    for (auto const& spec : specs_) {
        auto result = host_.components().msrp_handler.talker_advertise(make_talker_srp_info(spec), time);
        if (!result) {
            host_.ctl_log().warning("msrp: talker_advertise (stream {}) failed: errno {}", spec.index, result.error().value());
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
    uint64_t const packet_interval_ns = (static_cast<uint64_t>(samples_per_packet_) * 1'000'000'000ULL) / sample_rate_;

    for (size_t p = 0; p < config_.packets_per_wake; ++p) {
        uint64_t const wake_ns = base_now_ns + (static_cast<uint64_t>(p) * packet_interval_ns);
        last_gptp_ns_.store(wake_ns, std::memory_order_relaxed);

        // Media clock locked to gPTP: r = 1.0 (no GPS-rate tracking). The
        // avtp_timestamp comes from the deterministic generator, not the jittery
        // wake time, so a recovering listener stays steady.
        auto const tick = media_clock_.advance(wake_ns, /*r=*/1.0, samples_per_packet_);
        auto const samples = static_cast<size_t>(tick.samples);
        if (samples == 0) {
            continue;
        }

        // Default source: the built-in white-key tone fills the shared buffer
        // (oscillators advance exactly once per tick for phase continuity).
        for (size_t i = 0; i < samples; ++i) {
            for (size_t ch = 0; ch < channels_; ++ch) {
                audio_buffer_[(i * channels_) + ch] = oscillators_[ch](0.0F);
            }
        }

        uint64_t const pts = media_clock_.timestamp_for(tick.first_index);
        int64_t const now_steady_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        // Each stream renders its own source (bound render callback, else the
        // shared tone), gates on ITS OWN ACMP connection + reservation, and
        // emits per its kind (AM824 direct, AAF reframed, CRF decimated).
        for (size_t pos = 0; pos < specs_.size(); ++pos) {
            std::span<float const> src{audio_buffer_.data(), samples * channels_};
            if (renders_[pos]) {
                auto& buf = render_buffers_[pos];
                std::span<float> const dest{buf.data(), samples * channels_};
                renders_[pos](dest, tick.samples, tick.first_index, pts);
                src = dest;
            }
            if (auto* slot = talker_->slot_for(specs_[pos].index); slot != nullptr) {
                talker_->transmit_if_due(*slot, tick, talker_should_transmit(slot->spec.index, now_steady_ns), samples, src);
            }
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
    auto const* slot = talker_->slot_for(descriptor_index);
    if (slot == nullptr) {
        return false;
    }
    valid |= (1U << 6U);  // FRAMES_TX (IEEE 1722.1 Clause 7.4.43)
    out[6] = static_cast<uint32_t>(slot->tx_packets);
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
        span_copy(make_span(out.stream_format), stream_desc.current_format.span());
        flags |= stream_info_flags::STREAM_FORMAT_VALID;
    }

    out.stream_id = stream->stream_id;
    span_copy(make_span(out.stream_dest_mac), stream->stream_dest_mac.span());
    out.stream_vlan_id = ieee::doublet_t{stream->stream_vlan_id};
    out.msrp_accumulated_latency = ieee::quadlet_t{static_cast<uint32_t>(config_.presentation_offset_ns)};

    if (host_.components().acmp_talker.connection_count(descriptor_index) > 0) {
        flags |= stream_info_flags::CONNECTED;
    }
    out.flags = ieee::quadlet_t{flags};
    return true;
}

}  // namespace statusbar::avb_entity
