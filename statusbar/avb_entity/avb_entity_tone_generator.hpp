#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity Tone Generator
/// A standalone AVB talker entity whose stream topology is derived from its
/// descriptor-storage blob (Entity Construction Kit phase 1): each
/// STREAM_OUTPUT descriptor's current_format decides the stream kind — AM824
/// (IEC 61883-6 MBLA), AAF (32-bit PCM), or CRF (media clock) — so the same
/// C++ serves examples/tone.json (AM824+AAF+CRF), tone-aaf.json (AAF only),
/// tone-aaf-crf.json (AAF+CRF), or any other talker-only model without code
/// changes. Each audio channel carries a continuous sine tone; by default the
/// channels are the white piano keys upward from the base MIDI note.
///
/// The media clock is locked to gPTP at ratio r = 1.0 by default. When the
/// blob additionally declares a CRF STREAM_INPUT with a matching
/// INPUT_STREAM CLOCK_SOURCE (the menu/selection pattern: the code supports
/// it, the model opts in), a controller can SET_CLOCK_SOURCE onto it and the
/// media clock slaves to the recovered remote CRF rate instead — so a tone
/// generator can phase-follow another node's media clock. Audio stream
/// inputs remain unsupported (a tone generator has no audio RX path).
///
/// This reuses the shared AVB control plane (AvbEntityHost), the spec-driven
/// stream TX path (TalkerStreams), the CRF listener slot + clock recovery
/// (ListenerStreams + CrfClockRecovery, kit phase 3), the per-stream
/// transmit gate (TalkerGate), and the deterministic
/// presentation-timestamp generator (MediaClockGenerator). It deliberately
/// omits the audio-listener / UDPTUN halves of AvbEntityAudioIO.

#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/avb_entity/avb_entity_crf_clock_recovery.hpp"
#include "statusbar/avb_entity/avb_entity_host.hpp"
#include "statusbar/avb_entity/avb_entity_listener_streams.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/avb_entity_talker_gate.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/avtp/avtp_maap_handler.hpp"
#include "statusbar/dsp/dsp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace statusbar::avb_entity {

using statusbar::failure;
using statusbar::Status;
using statusbar::StatusValue;
using statusbar::success;

/// MIDI note number of C4 (= 60); the default base for the white-key tone set.
inline constexpr uint8_t TONE_DEFAULT_BASE_MIDI_NOTE = 60;

/// Frequency (Hz, equal temperament, A4 = 440) of the `white_index`-th white
/// piano key at or above `base_midi_note`. White keys are the natural notes; the
/// semitone offsets within an octave are {0,2,4,5,7,9,11}. With base C2 (36),
/// white_index 0..7 yields C2, D2, E2, F2, G2, A2, B2, C3.
[[nodiscard]] auto white_key_frequency_hz(uint8_t base_midi_note, size_t white_index) noexcept -> double;

/// Talker-only AVB tone generator; stream kinds/count/rate from the blob.
class AvbEntityToneGenerator
{
  public:
    using TimePoint = sm::TimePoint;

    /// SR class A wire cadence (125 us interval = 8000 packets/s/stream). The
    /// samples-per-packet follows the blob's audio rate (12 @ 96k, 6 @ 48k).
    static constexpr uint32_t CLASS_A_PACKETS_PER_SEC = avb_entity::CLASS_A_PACKETS_PER_SEC;

    /// Audio sample rate assumed when the blob declares no audio stream (a
    /// CRF-only model still needs a media-clock rate to pace itself).
    static constexpr uint32_t DEFAULT_SAMPLE_RATE = 96000;

    /// Factory — constructs and validates the entity from configuration. Parses
    /// the descriptor-storage blob: the STREAM_OUTPUT descriptors shape the
    /// stream slots (kind/format/rate per current_format), the first
    /// AUDIO_CLUSTER supplies the channel count, and every audio stream must
    /// agree on one sample rate and the cluster channel count (loud create-time
    /// failure instead of silent blob/C++ divergence). `base_midi_note` selects
    /// the lowest white-key tone (default C4); each channel takes the next
    /// white key up.
    [[nodiscard]] static auto create(
        AvbEntityAudioIOConfig config,
        uint8_t base_midi_note = TONE_DEFAULT_BASE_MIDI_NOTE,
        std::pmr::memory_resource* memory_resource = nullptr) -> StatusValue<std::unique_ptr<AvbEntityToneGenerator>>;

    ~AvbEntityToneGenerator();

    AvbEntityToneGenerator(AvbEntityToneGenerator const&) = delete;
    auto operator=(AvbEntityToneGenerator const&) -> AvbEntityToneGenerator& = delete;
    AvbEntityToneGenerator(AvbEntityToneGenerator&&) = delete;
    auto operator=(AvbEntityToneGenerator&&) -> AvbEntityToneGenerator& = delete;

    /// Passkey gating the public constructor (use create()).
    class CreateKey
    {
        CreateKey() = default;
        friend class AvbEntityToneGenerator;
    };

    AvbEntityToneGenerator(
        CreateKey,
        AvbEntityAudioIOConfig config,
        std::unique_ptr<nanoavb::AemEntityHandler> handler,
        nanoavb::DescriptorStorageHandler* storage_handler,
        StreamSpecs specs,
        StreamSpecs listener_specs,
        uint16_t initial_clock_source,
        std::optional<uint16_t> crf_clock_source_index,
        uint32_t sample_rate,
        size_t channels,
        uint8_t base_midi_note,
        std::pmr::memory_resource* memory_resource);

    [[nodiscard]] auto start(net::MessageReactor& reactor) -> Status;
    [[nodiscard]] auto stop() -> Status;

    [[nodiscard]] auto is_running() const noexcept -> bool { return host_.is_running(); }
    [[nodiscard]] auto is_ready() const noexcept -> bool { return host_.is_ready(); }
    [[nodiscard]] auto state_string() const -> std::string_view { return host_.state_string(); }
    auto print_state() const -> void;

    auto on_link_up(TimePoint time) -> void;
    auto on_link_down(TimePoint time) -> void;
    auto on_gptp_announce(TimePoint time, bool has_grandmaster) -> void;
    auto on_timeout(TimePoint time) -> void;

    /// Process one media-timer wake (8000 Hz): generate the per-channel tones and
    /// transmit each blob-declared stream (gated per stream).
    auto process_audio(TimePoint time) -> void;

    /// Whether talker stream `idx` (a STREAM_OUTPUT descriptor index) should put
    /// its AVTP stream on the wire this tick (gating disabled, or a downstream
    /// listener is ready/connected). Also gated on MAAP-address readiness in
    /// "maap" mode.
    [[nodiscard]] auto talker_should_transmit(uint16_t idx, int64_t now_ns) const noexcept -> bool;

    /// The blob-derived stream table (kind/format/rate per STREAM_OUTPUT).
    [[nodiscard]] auto stream_specs() const noexcept -> StreamSpecs const& { return specs_; }
    [[nodiscard]] auto sample_rate() const noexcept -> uint32_t { return sample_rate_; }

    // --- CRF-input media-clock slaving (kit phase 3c pattern) ----------------
    /// The CLOCK_DOMAIN's active clock-source index (runtime SET_CLOCK_SOURCE
    /// state; the blob's authored default until a controller changes it).
    [[nodiscard]] auto active_clock_source() const noexcept -> uint16_t
    {
        return active_clock_source_.load(std::memory_order_acquire);
    }
    /// The clock-source index backed by the blob's CRF stream input, or
    /// nullopt when the model declares none (gPTP-only pacing).
    [[nodiscard]] auto crf_clock_source_index() const noexcept -> std::optional<uint16_t> { return crf_clock_source_index_; }
    /// The CRF media-clock recovery (rate/lock telemetry).
    [[nodiscard]] auto crf_recovery() noexcept -> CrfClockRecovery& { return crf_recovery_; }
    /// The blob-derived listener stream table (CRF inputs only).
    [[nodiscard]] auto listener_specs() const noexcept -> StreamSpecs const& { return listener_specs_; }

    // --- Per-stream TX sources (kit phase 2) -------------------------------------
    /// Register a render callback for the audio stream whose blob symbol is
    /// @p symbol (e.g. "aaf_out"). The code is a menu, the model is the
    /// selection: a registration whose symbol/index the loaded model does not
    /// declare is recorded but INERT (listed at start(), never an error), so
    /// one binary can implement every option and a minimal model activates a
    /// subset. Unbound audio streams fall back to the built-in white-key tone.
    /// Call before start(); the callback runs on the media-timer RT thread
    /// (no allocation, no blocking).
    void set_render(std::string_view symbol, StreamRenderFn fn) { set_render_symbol(symbol_code(symbol), std::move(fn)); }
    void set_render_symbol(uint32_t symbol_code, StreamRenderFn fn);
    /// Same, addressed by STREAM_OUTPUT descriptor index.
    void set_render(uint16_t stream_index, StreamRenderFn fn);

    // --- Per-control value handlers (kit phase 4) ---------------------------
    /// A bound control's value-changed handler: receives the SET_CONTROL
    /// current-values payload (size-validated for linear types). Return
    /// AEM_STATUS_SUCCESS to accept (the built-in stores + notifies), any
    /// other AEM_STATUS_* to reject. Reactor thread.
    using ControlChangedFn = sg14::inplace_function<uint8_t(std::span<uint8_t const> value), 64>;

    /// Register a handler for the CONTROL whose blob symbol is @p symbol.
    /// The code is a menu, the model is the selection: an unknown symbol is
    /// recorded but inert (listed at start()); controls with no handler still
    /// get the generic store/serve built-in.
    void on_control(std::string_view symbol, ControlChangedFn fn) { on_control_symbol(symbol_code(symbol), std::move(fn)); }
    void on_control_symbol(uint32_t symbol_code, ControlChangedFn fn);

    [[nodiscard]] auto components() -> nanoavb::NanoAvbComponents& { return host_.components(); }
    [[nodiscard]] auto components() const -> nanoavb::NanoAvbComponents const& { return host_.components(); }
    [[nodiscard]] auto net_handlers() -> nanoavb::NanoAvbNetHandlers* { return host_.net_handlers(); }

    // --- Logging (see AvbEntityHost) --------------------------------------------
    [[nodiscard]] auto ctl_log_channel() noexcept -> logging::LogChannelBase& { return host_.ctl_log_channel(); }
    [[nodiscard]] auto media_log_channel() noexcept -> logging::LogChannelBase& { return host_.media_log_channel(); }
    void set_log_verbosity(logging::LogLevel const v) noexcept { host_.set_log_verbosity(v); }
    [[nodiscard]] auto config() const noexcept -> AvbEntityAudioIOConfig const& { return config_; }
    [[nodiscard]] auto channels() const noexcept -> size_t { return channels_; }

    /// TX stream capture (diagnostic; see AvbEntityAudioIOConfig::tx_pcap_path).
    [[nodiscard]] auto tx_pcap_ready_to_write() const noexcept -> bool { return talker_->tx_pcap_recorder_.ready_to_write(); }
    [[nodiscard]] auto flush_tx_pcap() -> Status { return talker_->tx_pcap_recorder_.write_to_file(); }
    [[nodiscard]] auto tx_pcap_frame_count() const noexcept -> size_t { return talker_->tx_pcap_recorder_.frame_count(); }

  private:
    auto wire_stream_callbacks() -> void;
    [[nodiscard]] auto make_talker_srp_info(StreamSpec const& spec) const -> nanoavb::TalkerStreamSrpInfo;
    void advertise_talker_streams(TimePoint time);
    [[nodiscard]] auto acquire_maap_addresses(net::MessageReactor& reactor) -> Status;
    [[nodiscard]] auto fill_stream_output_counters(uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& out) const
        -> bool;
    [[nodiscard]] auto fill_stream_output_info(
        uint16_t descriptor_type, uint16_t descriptor_index, atdecc::aem::AemStreamInfoPayload& out) const -> bool;

    AvbEntityAudioIOConfig config_;

    /// Blob-derived stream tables; drive every per-stream decision below.
    StreamSpecs specs_;
    StreamSpecs listener_specs_;  ///< CRF inputs only (audio inputs rejected at create)

    /// Reusable AVB control plane: talker/listener stream counts from the
    /// blob, 4 max listeners per talker stream.
    AvbEntityHost host_;

    /// Per-stream TX render bindings + their interleaved buffers, parallel to
    /// specs_ (empty function = default tone; empty buffer = non-audio slot).
    std::array<StreamRenderFn, MAX_ENTITY_STREAMS> renders_{};
    sg14::inplace_vector<std::pmr::vector<float>, MAX_ENTITY_STREAMS> render_buffers_{};
    /// Registrations that matched nothing in the model (menu/selection:
    /// inert; listed at start() for typo-finding).
    sg14::inplace_vector<uint32_t, MAX_ENTITY_STREAMS> unbound_render_symbols_{};
    sg14::inplace_vector<uint16_t, MAX_ENTITY_STREAMS> unbound_render_indices_{};

    /// Symbol-bound control handlers (kit phase 4): resolved bindings carry
    /// the CONTROL descriptor index; unresolved ones stay inert (diag at start).
    struct ControlBinding
    {
        uint32_t symbol{0};
        uint16_t control_index{0};
        bool resolved{false};
        ControlChangedFn fn{};
    };
    static constexpr size_t MAX_CONTROL_BINDINGS = 8;
    sg14::inplace_vector<ControlBinding, MAX_CONTROL_BINDINGS> control_bindings_{};
    /// The blob-backed descriptor handler (owned by host_); carries the
    /// generic CONTROL built-in this registry dispatches from.
    nanoavb::DescriptorStorageHandler* storage_handler_{nullptr};

    /// Per-stream transmit gate (ACMP-AND-MSRP + grace). Binds config_ + components.
    TalkerGate gate_{config_.gate_talker_on_listener, host_.components()};

    uint32_t sample_rate_{DEFAULT_SAMPLE_RATE};  ///< from the blob's audio formats
    uint32_t samples_per_packet_{DEFAULT_SAMPLE_RATE / CLASS_A_PACKETS_PER_SEC};
    size_t channels_{0};

    //
    // DSP source (per-channel continuous sine), interleaved output buffer.
    //
    std::pmr::memory_resource* mem_resource_{std::pmr::get_default_resource()};
    std::pmr::vector<float> audio_buffer_;
    std::pmr::vector<dsp::Oscillator<float>> oscillators_;

    /// Latest gPTP time (ns) seen by the media timer (TalkerStreams reads it).
    std::atomic<uint64_t> last_gptp_ns_{0};

    /// Deterministic presentation-timestamp generator. r is 1.0 (locked to
    /// gPTP) unless the CRF-input clock source is active and locked, in
    /// which case it follows the recovered remote rate (see process_audio).
    ptpclient::MediaClockGenerator media_clock_;

    /// CRF-input media-clock recovery: fed by the CRF stream input's
    /// timestamps on the RX/reactor thread; consulted for the media-clock
    /// rate on the media thread when the CRF clock source is active.
    CrfClockRecovery crf_recovery_{};
    /// The CLOCK_DOMAIN's active clock-source index. Written by the
    /// SET_CLOCK_SOURCE apply callback (reactor thread), read per tick by
    /// the media thread. Assigned in the ctor body (declaration order).
    std::atomic<uint16_t> active_clock_source_{0};
    /// The clock-source index whose CLOCK_SOURCE descriptor is the CRF
    /// stream input (resolved from the blob at create; nullopt = not modeled).
    std::optional<uint16_t> crf_clock_source_index_{};

    /// The CRF listener RX path (only allocated when the blob declares a CRF
    /// input): the CRF deserialize slot + counters + the borrowed RX socket
    /// (the socket's handler is owned by the reactor after start()).
    /// Declared after host_/config_/last_gptp_ns_ (binds components).
    std::unique_ptr<ListenerStreams> listener_{};

    /// Stream TX path: qdisc-bypass socket + spec-shaped serializer slots + TX
    /// counters + capture recorder. Declared after the members it references.
    std::unique_ptr<TalkerStreams> talker_{std::make_unique<TalkerStreams>(
        TalkerStreamsConfig{.sample_rate = sample_rate_, .vlan_id = config_.vlan_id, .stream_pcp = config_.stream_pcp},
        media_clock_,
        last_gptp_ns_,
        mem_resource_)};

    /// MAAP handler, allocated only in "maap" stream_address_mode (null otherwise).
    std::unique_ptr<avtp::MaapHandler> maap_handler_;

    /// TX-address readiness gate. true (default/static). In "maap" mode it is held
    /// false until the block is defended.
    std::atomic<bool> maap_addresses_ready_{true};
};

}  // namespace statusbar::avb_entity
