#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AM824 stream output (talker/transmitter) — callback-based serialization
///
/// Provides Am824StreamOutputContext for per-stream transmit state (DBC generation,
/// timestamp insertion, sequence numbering) and callback-based serializers for
/// MBLA audio, MIDI, and SMPTE.
///
/// The sample count per packet is a runtime parameter — at 48kHz with class C
/// (125μs interval), you get a repeating pattern of 5/6/7 samples per packet.

#include "statusbar/avtp/avtp_am824_stream.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace statusbar::avtp {

/// Check if AM824 serialize frame parameters are valid
template <size_t MaxChannels, size_t MaxSamples>
[[nodiscard]] constexpr auto is_valid_am824_serialize_params(uint8_t samples_per_channel, uint8_t channels) noexcept -> bool
{
    return samples_per_channel != 0 && channels != 0 && channels <= MaxChannels && samples_per_channel <= MaxSamples;
}

/// Per-stream state for AM824 serialization (talker/transmitter side).
/// One instance per transmitted AM824 stream. Tracks DBC, timestamps, and sequence numbers.
struct Am824StreamOutputContext
{
    Am824StreamConfig config;
    StreamId stream_id;
    uint64_t presentation_offset_ns;  ///< How far ahead of gptp_now to set presentation time

    // Running state
    uint8_t sequence_num{0};
    uint32_t running_dbc{0};  ///< Total data blocks (samples) sent

    // Statistics
    uint32_t packets_sent{0};
    uint32_t timestamp_inserts{0};

    /// Construct with stream ID, sample rate, channel count, and presentation offset.
    /// @param sid          The stream ID for this talker stream
    /// @param rate         AM824 sample rate
    /// @param channels     Number of audio channels
    /// @param pres_offset  Presentation time offset in nanoseconds (typically 1-4ms)
    Am824StreamOutputContext(StreamId sid, Am824SampleRate rate, uint8_t channels, uint64_t pres_offset);

    /// Reset running state while preserving configuration
    void reset() noexcept;

    /// Build the packet header for the next packet.
    ///
    /// Fills in the PDU's stream_id, sequence number, DBC, and dimensions.
    /// Sets tv=1 and computes the AVTP timestamp (lower 32 bits of presentation time)
    /// when the running DBC crosses a syt_interval boundary.
    ///
    /// @param pdu                  PDU to fill (must already be initialized with init())
    /// @param samples_per_channel  Number of samples in this packet (e.g. 5, 6, or 7)
    /// @param gptp_now_ns          Current gPTP time (used to compute presentation timestamp)
    void build_packet_header(Am824Pdu& pdu, uint8_t samples_per_channel, uint64_t gptp_now_ns) noexcept;
};

/// Serialize MBLA-only AM824 stream — all channels are audio (hot path)
///
/// Calls get_audio once per channel to fill a scratch buffer with float samples,
/// then encodes them as AM824 quadlets into the payload.
///
/// @tparam MaxChannels   Maximum channel count for scratch allocation (default: Am824Pdu::MAX_CHANNELS)
/// @tparam MaxSamples    Maximum samples per packet for scratch allocation (default: Am824Pdu::MAX_SAMPLES_PER_PACKET)
/// @param ctx                  Stream output context (updated with DBC/sequence state)
/// @param pdu                  PDU header to fill
/// @param payload              Output buffer for AM824 audio quadlets
/// @param samples_per_channel  Number of samples to serialize (runtime, e.g. 5, 6, or 7)
/// @param gptp_now_ns          Current gPTP time (for timestamp generation)
/// @param get_audio            Callback: void(uint8_t channel, std::span<float> dest)
///                             Called once per channel. Caller fills dest with samples_per_channel floats.
/// @return Number of bytes written to payload
template <size_t MaxChannels = Am824Pdu::MAX_CHANNELS, size_t MaxSamples = Am824Pdu::MAX_SAMPLES_PER_PACKET, typename AudioSource>
auto am824_serialize_mbla(
    Am824StreamOutputContext& ctx,
    Am824Pdu& pdu,
    std::span<uint8_t> payload,
    uint8_t samples_per_channel,
    uint64_t gptp_now_ns,
    AudioSource const& get_audio) -> size_t
{
    uint8_t const channels = ctx.config.channel_count;
    if (!is_valid_am824_serialize_params<MaxChannels, MaxSamples>(samples_per_channel, channels)) {
        return 0;
    }

    size_t const required = static_cast<size_t>(samples_per_channel) * channels * 4;
    if (payload.size() < required) {
        return 0;
    }

    ctx.build_packet_header(pdu, samples_per_channel, gptp_now_ns);

    // Collect float samples from source into per-channel scratch buffers. Only the region the
    // encoder reads is zeroed (the array lives on the real-time stack), so unfilled dest is silence.
    std::array<std::array<float, MaxSamples>, MaxChannels> scratch;  // NOLINT(cppcoreguidelines-pro-type-member-init)
    for (uint8_t ch = 0; ch < channels; ++ch) {
        std::fill_n(scratch[ch].data(), samples_per_channel, 0.0F);
        get_audio(ch, std::span<float>{scratch[ch].data(), samples_per_channel});
    }

    // Encode as channel-interleaved AM824 quadlets
    for (uint8_t s = 0; s < samples_per_channel; ++s) {
        for (uint8_t ch = 0; ch < channels; ++ch) {
            uint32_t const quadlet = create_am824_quadlet(float_to_am824_sample(scratch[ch][s]));
            ieee::quadlet_t const q{quadlet};
            span_store(payload.subspan(static_cast<size_t>((s * channels) + ch) * 4), q);
        }
    }

    return required;
}

/// Serialize mixed AM824 stream — channels may be audio, MIDI, or SMPTE
///
/// Uses per-channel type assignment to determine which source callback to use
/// for each channel slot in each data block.
///
/// @tparam MaxChannels   Maximum channel count for scratch allocation (default: Am824Pdu::MAX_CHANNELS)
/// @tparam MaxSamples    Maximum samples per packet for scratch allocation (default: Am824Pdu::MAX_SAMPLES_PER_PACKET)
/// @param ctx                  Stream output context (updated with DBC/sequence state)
/// @param pdu                  PDU header to fill
/// @param payload              Output buffer for AM824 quadlets
/// @param samples_per_channel  Number of data blocks to serialize (runtime)
/// @param gptp_now_ns          Current gPTP time (for timestamp generation)
/// @param channel_types        Per-channel type assignment (must have channel_count elements)
/// @param get_audio            Callback: void(uint8_t channel, std::span<float> dest)
///                             Called once per MBLA channel. Caller fills dest with samples_per_channel floats.
/// @param get_midi             Callback: uint8_t(uint8_t channel, uint8_t sample_index, uint8_t* out)
///                             Called per MIDI channel per data block. Returns 0-3 MIDI bytes written to out.
/// @param get_smpte            Callback: uint8_t(uint8_t channel, uint8_t sample_index, std::array<uint8_t,3>& out)
///                             Called per SMPTE channel per data block. Returns part number (0=no data, 1-3).
/// @return Number of bytes written to payload
template <
    size_t MaxChannels = Am824Pdu::MAX_CHANNELS,
    size_t MaxSamples = Am824Pdu::MAX_SAMPLES_PER_PACKET,
    typename AudioSource,
    typename MidiSource,
    typename SmpteSource>
auto am824_serialize_mixed(
    Am824StreamOutputContext& ctx,
    Am824Pdu& pdu,
    std::span<uint8_t> payload,
    uint8_t samples_per_channel,
    uint64_t gptp_now_ns,
    std::span<Am824ChannelType const> channel_types,
    AudioSource const& get_audio,
    MidiSource const& get_midi,
    SmpteSource const& get_smpte) -> size_t
{
    uint8_t const channels = ctx.config.channel_count;
    if (!is_valid_am824_serialize_params<MaxChannels, MaxSamples>(samples_per_channel, channels)) {
        return 0;
    }
    if (channel_types.size() < channels) {
        return 0;
    }

    size_t const required = static_cast<size_t>(samples_per_channel) * channels * 4;
    if (payload.size() < required) {
        return 0;
    }

    ctx.build_packet_header(pdu, samples_per_channel, gptp_now_ns);

    // Collect audio samples into scratch for MBLA channels
    // Only the MBLA channels' used region is zeroed (see am824_serialize_mbla).
    std::array<std::array<float, MaxSamples>, MaxChannels> audio_scratch;  // NOLINT(cppcoreguidelines-pro-type-member-init)
    for (uint8_t ch = 0; ch < channels; ++ch) {
        if (channel_types[ch] == Am824ChannelType::mbla) {
            std::fill_n(audio_scratch[ch].data(), samples_per_channel, 0.0F);
            get_audio(ch, std::span<float>{audio_scratch[ch].data(), samples_per_channel});
        }
    }

    // Encode data blocks — channel-interleaved quadlets
    for (uint8_t s = 0; s < samples_per_channel; ++s) {
        for (uint8_t ch = 0; ch < channels; ++ch) {
            uint32_t quadlet = 0;

            switch (channel_types[ch]) {
                case Am824ChannelType::mbla:
                    quadlet = create_am824_quadlet(float_to_am824_sample(audio_scratch[ch][s]));
                    break;
                case Am824ChannelType::midi: {
                    uint8_t midi_bytes[3]{};
                    uint8_t const n = get_midi(ch, s, midi_bytes);
                    quadlet = am824_create_midi_quadlet(n, midi_bytes);
                    break;
                }
                case Am824ChannelType::smpte: {
                    std::array<uint8_t, 3> smpte_bytes{};
                    uint8_t const part = get_smpte(ch, s, smpte_bytes);
                    quadlet = am824_create_smpte_quadlet(part, smpte_bytes);
                    break;
                }
            }

            ieee::quadlet_t const q{quadlet};
            span_store(payload.subspan(static_cast<size_t>((s * channels) + ch) * 4), q);
        }
    }

    return required;
}

}  // namespace statusbar::avtp
