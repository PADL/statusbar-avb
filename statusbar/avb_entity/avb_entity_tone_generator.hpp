#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity Tone Generator (talker-only)
/// A standalone AVB talker entity that transmits three stream sources, no
/// listeners and no inter-site tunnel:
///   - stream 0: AM824 (IEC 61883-6, MBLA 24-in-32), N-channel 96 kHz
///   - stream 1: AAF (IEEE 1722 AVTP Audio Format, 32-bit PCM), N-channel 96 kHz
///   - stream 2: CRF (Clock Reference Format, Milan 48 kHz media clock)
/// Each audio channel carries a continuous sine tone; by default the 8 channels
/// are the white piano keys C4..C5 (C4, D4, E4, F4, G4, A4, B4, C5). The media
/// clock is locked to gPTP at ratio r = 1.0 (no GPS-rate tracking). The entity
/// model is loaded from a descriptor-storage blob declaring 0 stream inputs and
/// 3 stream outputs (examples/tone.json, compiled by aemxml json2bin).
///
/// This reuses the shared AVB control plane (AvbEntityHost), the stream TX path
/// (TalkerStreams: AM824 + AAF + CRF), the per-stream transmit gate (TalkerGate),
/// the deterministic presentation-timestamp generator (MediaClockGenerator) and
/// the AAF reframer. It deliberately omits the listener / UDPTUN halves of
/// AvbEntityAudioIO.

#include "statusbar/avb_entity/avb_entity_aaf_reframe.hpp"
#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/avb_entity/avb_entity_host.hpp"
#include "statusbar/avb_entity/avb_entity_talker_gate.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/avtp/avtp_maap_handler.hpp"
#include "statusbar/dsp/dsp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
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

/// Talker-only AVB tone generator (AM824 + AAF audio + CRF media clock).
class AvbEntityToneGenerator
{
  public:
    using TimePoint = sm::TimePoint;

    /// 96 kHz, SR class A (125 us interval = 8000 packets/s) -> 12 samples/packet.
    static constexpr uint32_t CLASS_A_PACKETS_PER_SEC = 8000;
    static constexpr uint32_t SAMPLE_RATE = 96000;
    static constexpr size_t SAMPLES_PER_PACKET = SAMPLE_RATE / CLASS_A_PACKETS_PER_SEC;

    /// CRF base frequency. Milan mandates a 48 kHz CRF media clock; both 48 kHz
    /// and 96 kHz clients lock to it. Must divide SAMPLE_RATE evenly.
    static constexpr uint32_t CRF_BASE_FREQUENCY = 48000;
    static_assert(SAMPLE_RATE % CRF_BASE_FREQUENCY == 0, "CRF base must divide the audio sample rate");

    /// AAF wire format: 32-bit signed PCM at 96 kHz.
    static constexpr avtp::AafFormat AAF_FORMAT = avtp::AafFormat::int_32bit;
    static constexpr avtp::AafSampleRate AAF_SAMPLE_RATE = avtp::AafSampleRate::rate_96_khz;
    static constexpr uint8_t AAF_BIT_DEPTH = 32;

    /// Stream (STREAM_OUTPUT) descriptor indices for the full (All) model.
    static constexpr uint16_t AM824_STREAM_INDEX = 0;
    static constexpr uint16_t AAF_STREAM_INDEX = 1;
    static constexpr uint16_t CRF_STREAM_INDEX = 2;

    /// Which stream set this entity exposes + transmits.
    ///   All     = AM824 (idx 0) + AAF (idx 1) + CRF (idx 2) — the full generator.
    ///   AafOnly = a single AAF stream at index 0 — a clean 8-ch AAF device, for a
    ///             listener that stalls on the mixed AM824+AAF 16-ch aggregate.
    ///             Pairs with the examples/tone-aaf.json model.
    ///   AafCrf  = AAF (idx 0) + CRF (idx 1) — a clean 8-ch AAF device PLUS a CRF
    ///             media-clock stream, so a listener (e.g. macOS) has a clock
    ///             reference to recover. Pairs with examples/tone-aaf-crf.json.
    enum class StreamSet
    {
        All,
        AafOnly,
        AafCrf
    };

    /// Factory — constructs and validates the entity from configuration. Parses
    /// the descriptor-storage blob (channel count + model) and exposes the
    /// requested `streams` set. `base_midi_note` selects the lowest white-key tone
    /// (default C2); each channel takes the next white key up.
    [[nodiscard]] static auto create(
        AvbEntityAudioIOConfig config,
        uint8_t base_midi_note = TONE_DEFAULT_BASE_MIDI_NOTE,
        StreamSet streams = StreamSet::All,
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
        size_t channels,
        uint8_t base_midi_note,
        StreamSet streams,
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
    /// transmit an AM824 + AAF packet (gated), plus the decimated CRF media clock.
    auto process_audio(TimePoint time) -> void;

    /// Whether talker stream `idx` (0=AM824, 1=AAF, 2=CRF) should put its AVTP
    /// stream on the wire this tick (gating disabled, or a downstream listener is
    /// ready/connected). Also gated on MAAP-address readiness in "maap" mode.
    [[nodiscard]] auto talker_should_transmit(uint16_t idx, int64_t now_ns) const noexcept -> bool;

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
    /// The active STREAM_OUTPUT indices: {0,1,2} in All mode, {0} (AAF) in AafOnly.
    [[nodiscard]] auto active_stream_indices() const -> std::vector<uint16_t>;
    [[nodiscard]] auto make_talker_srp_info(uint16_t stream_index) const -> nanoavb::TalkerStreamSrpInfo;
    void advertise_talker_streams(TimePoint time);
    [[nodiscard]] auto acquire_maap_addresses(net::MessageReactor& reactor) -> Status;
    [[nodiscard]] auto fill_stream_output_counters(uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& out) const
        -> bool;
    [[nodiscard]] auto fill_stream_output_info(
        uint16_t descriptor_type, uint16_t descriptor_index, atdecc::aem::AemStreamInfoPayload& out) const -> bool;

    AvbEntityAudioIOConfig config_;

    /// Reusable AVB control plane: N talker streams (3 for All, 1 for AafOnly),
    /// 4 max listeners each, 0 listener streams (talker-only).
    AvbEntityHost host_;

    /// Active stream kinds + their descriptor/ACMP indices. AAF is always present.
    ///   All:     AM824@0, AAF@1, CRF@2.   AafCrf: AAF@0, CRF@1.   AafOnly: AAF@0.
    bool has_am824_{true};
    bool has_crf_{true};
    uint16_t am824_idx_{AM824_STREAM_INDEX};
    uint16_t aaf_idx_{AAF_STREAM_INDEX};
    uint16_t crf_idx_{CRF_STREAM_INDEX};

    /// Per-stream transmit gate (ACMP-AND-MSRP + grace). Binds config_ + components.
    TalkerGate gate_{config_.gate_talker_on_listener, host_.components()};

    size_t channels_{0};

    //
    // DSP source (per-channel continuous sine), interleaved output buffer.
    //
    std::pmr::memory_resource* mem_resource_{std::pmr::get_default_resource()};
    std::pmr::vector<float> audio_buffer_;
    std::pmr::vector<dsp::Oscillator<float>> oscillators_;

    /// Latest gPTP time (ns) seen by the media timer (TalkerStreams reads it).
    std::atomic<uint64_t> last_gptp_ns_{0};

    /// Deterministic presentation-timestamp generator (r is pinned to 1.0: the
    /// media clock is locked to gPTP, no GPS-rate tracking).
    ptpclient::MediaClockGenerator media_clock_;

    /// Stream TX path: qdisc-bypass socket + AM824/AAF/CRF serializers + dest MACs
    /// + TX counters + capture recorder. Declared after the members it references.
    std::unique_ptr<TalkerStreams> talker_{std::make_unique<TalkerStreams>(
        TalkerStreamsConfig{
            .sample_rate = SAMPLE_RATE,
            .crf_timestamp_interval = config_.crf_timestamp_interval,
            .crf_timestamps_per_packet = config_.crf_timestamps_per_packet,
            .vlan_id = config_.vlan_id,
            .stream_pcp = config_.stream_pcp},
        media_clock_,
        audio_buffer_,
        channels_,
        last_gptp_ns_,
        mem_resource_)};

    /// MAAP handler, allocated only in "maap" stream_address_mode (null otherwise).
    std::unique_ptr<avtp::MaapHandler> maap_handler_;

    /// TX-address readiness gate. true (default/static). In "maap" mode it is held
    /// false until the block is defended.
    std::atomic<bool> maap_addresses_ready_{true};
};

}  // namespace statusbar::avb_entity
