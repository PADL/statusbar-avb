// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity Tone Generator implementation (talker-only).
/// A blob-loaded model, a per-channel continuous sine source (white piano keys
/// by default), and N talker streams shaped by the blob's STREAM_OUTPUT
/// descriptors (Entity Construction Kit phase 1). All control-plane wiring
/// lives in AvbEntityKit (refactor phase D); this file is blob validation,
/// the sine source, the render bindings, and process_audio().

#include "statusbar/avb_entity/avb_entity_tone_generator.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"
#include "statusbar/net/net_posix_util.hpp"
#include "statusbar/status/catch_or_status.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <print>
#include <span>
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
    // Stream inputs: a tone generator has no audio RX path, but a CRF input
    // is welcome — it lets the media clock slave to a remote CRF (the code is
    // a menu, the model is the selection). Any AUDIO input is still a loud
    // create-time rejection.
    auto listener_specs = listener_stream_specs(*storage_result, 0);
    if (!listener_specs) {
        return failure(listener_specs.error());
    }
    for (auto const& spec : *listener_specs) {
        if (spec.format.kind != StreamKind::crf) {
            return failure(std::errc::invalid_argument);  // audio inputs unsupported here
        }
    }

    // Kit phase 3c pattern: the clock source backed by the (first) CRF
    // stream input, and the CLOCK_DOMAIN's authored default selection.
    std::optional<uint16_t> crf_clock_source{};
    if (!listener_specs->empty()) {
        crf_clock_source = find_input_stream_clock_source(*storage_result, listener_specs->front().index);
    }
    uint16_t const initial_clock_source = authored_clock_source(*storage_result);

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
    EntityIdentityDescriptorHandler handler{
        *storage_result,
        config.entity_id,
        config.entity_model_id,
        config.firmware_version,
        config.entity_name,
        iface_mac,
        /*patch_avb_interface=*/true};

    std::pmr::memory_resource* const mr = memory_resource != nullptr ? memory_resource : std::pmr::get_default_resource();
    auto entity = std::make_unique<AvbEntityToneGenerator>(
        AvbEntityToneGenerator::CreateKey{},
        std::move(config),
        std::move(handler),
        specs,
        *listener_specs,
        initial_clock_source,
        crf_clock_source,
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
    EntityIdentityDescriptorHandler handler,
    StreamSpecs specs,
    StreamSpecs listener_specs,
    uint16_t initial_clock_source,
    std::optional<uint16_t> crf_clock_source_index,
    uint32_t sample_rate,
    size_t channels,
    uint8_t base_midi_note,
    std::pmr::memory_resource* memory_resource)
    : config_{std::move(config)}
    , sample_rate_{sample_rate}
    , samples_per_packet_{sample_rate / CLASS_A_PACKETS_PER_SEC}
    , channels_{channels}
    , mem_resource_{memory_resource}
    , audio_buffer_((static_cast<size_t>(samples_per_packet_) + 1) * channels, 0.0F, mem_resource_)  // +1: gPTP pacing
    , oscillators_(channels, dsp::Oscillator<float>{}, mem_resource_)
    , handler_{std::move(handler)}
    , kit_{
          config_,
          handler_,
          specs,
          listener_specs,
          sample_rate,
          initial_clock_source,
          crf_clock_source_index,
          media_clock_,
          last_gptp_ns_,
          /*audio_sink=*/nullptr,
          mem_resource_}
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

    // Per-stream render buffers, parallel to the talker specs: audio slots get
    // an interleaved tick buffer; CRF/non-audio slots an empty one.
    for (auto const& spec : kit_.talker_specs()) {
        bool const is_audio = spec.format.kind == StreamKind::am824 || spec.format.kind == StreamKind::aaf;
        render_buffers_.emplace_back(
            is_audio ? (static_cast<size_t>(samples_per_packet_) + 1) * channels_ : 0, 0.0F, mem_resource_);
    }
}

AvbEntityToneGenerator::~AvbEntityToneGenerator()
{
    (void)statusbar::catch_or_status(
        [&]() -> statusbar::Status {
            if (is_running()) {
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
    auto const& specs = kit_.talker_specs();
    for (size_t pos = 0; pos < specs.size(); ++pos) {
        auto const& spec = specs[pos];
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
    auto const& specs = kit_.talker_specs();
    for (size_t pos = 0; pos < specs.size(); ++pos) {
        auto const& spec = specs[pos];
        bool const is_audio = spec.format.kind == StreamKind::am824 || spec.format.kind == StreamKind::aaf;
        auto const sym = kit_.host().symbol_of(DESCRIPTOR_STREAM_OUTPUT, spec.index);
        if (is_audio && sym.has_value() && *sym == symbol_code) {
            renders_[pos] = std::move(fn);
            return;
        }
    }
    if (unbound_render_symbols_.size() < MAX_ENTITY_STREAMS) {
        unbound_render_symbols_.push_back(symbol_code);
    }
}

//
// Start
//

auto AvbEntityToneGenerator::start(net::MessageReactor& reactor) -> Status
{
    // Deterministic media clock owns the presentation offset; the stream-output
    // contexts add zero extra offset. Assigned before kit_.start() opens the
    // TX slots (the kit's talker holds a reference to it).
    media_clock_ = ptpclient::MediaClockGenerator{ptpclient::MediaClockGenerator::Config{
        .sample_rate_hz = static_cast<double>(sample_rate_), .presentation_offset_ns = config_.presentation_offset_ns}};

    if (auto status = kit_.start(reactor); !status) {
        return status;
    }

    // Menu/selection diagnostics: render registrations the model left inert
    // (the kit lists inert CONTROL registrations itself). Typo-finding, never
    // an error.
    for (auto const code : unbound_render_symbols_) {
        kit_.host().ctl_log().status("render: registered symbol 0x{:08x} not in this model (inert)", code);
    }
    for (auto const idx : unbound_render_indices_) {
        kit_.host().ctl_log().status("render: registered stream index {} not an audio stream in this model (inert)", idx);
    }

    return success();
}

void AvbEntityToneGenerator::print_state() const
{
    std::print(
        "State: supervisor={} gptp={} mvrp={} channels={} rate={}",
        state_string(),
        kit_.host().gptp_locked() ? "Locked" : "Unlocked",
        kit_.host().mvrp_joined() ? "Joined" : "NotJoined",
        channels_,
        sample_rate_);
    for (auto const& spec : kit_.talker_specs()) {
        auto const* slot = kit_.talker().slot_for(spec.index);
        std::print(
            " | {}[{}] acmp={} tx={}",
            stream_kind_name(spec.format.kind),
            spec.index,
            kit_.host().components().acmp_talker.connection_count(spec.index),
            slot != nullptr ? slot->tx_packets : 0);
    }
    std::print("\n");
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

        // Media clock rate: 1.0 (locked to gPTP) unless the CRF-input clock
        // source is active and locked, in which case the recovered remote
        // rate slaves this entity's media clock to the far talker's (see
        // AvbEntityKit::media_rate). The avtp_timestamp comes from the
        // deterministic generator, not the jittery wake time.
        double const r = kit_.media_rate(1.0);
        auto const tick = media_clock_.advance(wake_ns, r, samples_per_packet_);
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
        auto const& specs = kit_.talker_specs();
        for (size_t pos = 0; pos < specs.size(); ++pos) {
            std::span<float const> src{audio_buffer_.data(), samples * channels_};
            if (renders_[pos]) {
                auto& buf = render_buffers_[pos];
                std::span<float> const dest{buf.data(), samples * channels_};
                renders_[pos](dest, tick.samples, tick.first_index, pts);
                src = dest;
            }
            if (auto* slot = kit_.talker().slot_for(specs[pos].index); slot != nullptr) {
                kit_.talker().transmit_if_due(
                    *slot, tick, kit_.talker_should_transmit(slot->spec.index, now_steady_ns), samples, src);
            }
        }
    }
}

}  // namespace statusbar::avb_entity
