#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AAF stream output (talker/transmitter) — callback-based serialization
///
/// Provides AafStreamOutputContext for per-stream transmit state (sequence numbering,
/// timestamp generation) and a callback-based serializer that encodes float audio
/// to any AAF PCM format (int16, int24, int32, float32).
///
/// The sample count per packet is a runtime parameter — at 48kHz with class C
/// (125μs interval), the pattern is typically 6,6,6,6,6,6,5,6... samples.

#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_stream_common.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace statusbar::avtp {

/// Check if AAF serialize frame parameters are valid
template <size_t MaxChannels, size_t MaxSamples>
[[nodiscard]] constexpr auto is_valid_aaf_serialize_params(uint16_t samples_per_channel, uint16_t channels, size_t bps) noexcept
    -> bool
{
    return samples_per_channel != 0 && channels != 0 && bps != 0 && channels <= MaxChannels && samples_per_channel <= MaxSamples;
}

/// Per-stream state for AAF serialization (talker/transmitter side).
/// One instance per transmitted AAF stream.
///
/// AAF timestamp mode is controlled by `sparse_timestamp`:
///   false (sp=0): every packet gets tv=1 with a valid AVTP timestamp
///   true  (sp=1): only every Nth packet gets tv=1 (not yet implemented — always non-sparse)
struct AafStreamOutputContext
{
    // Config
    AafFormat format;
    AafSampleRate sample_rate;
    uint16_t channel_count;
    uint8_t bit_depth;
    uint64_t sample_period_ns;
    StreamId stream_id;
    uint64_t presentation_offset_ns;  ///< How far ahead of gptp_now to set presentation time

    // Running state
    uint8_t sequence_num{0};
    uint32_t running_sample_count{0};  ///< Total samples sent (for PTS extrapolation)

    // Statistics
    uint32_t packets_sent{0};

    /// Construct with stream parameters
    /// @param sid          The stream ID for this talker stream
    /// @param fmt          AAF sample format (int16, int24, int32, float32)
    /// @param rate         Nominal sample rate
    /// @param channels     Number of audio channels
    /// @param depth        Bit depth of audio samples
    /// @param pres_offset  Presentation time offset in nanoseconds (typically 1-4ms)
    AafStreamOutputContext(StreamId sid, AafFormat fmt, AafSampleRate rate, uint16_t channels, uint8_t depth, uint64_t pres_offset);

    /// Reset running state while preserving configuration
    void reset() noexcept;

    /// Build the packet header for the next packet.
    ///
    /// Fills in stream_id, sequence number, format fields, and dimensions.
    /// Sets tv=1 and computes the AVTP timestamp (lower 32 bits of presentation time).
    /// AAF non-sparse mode: every packet gets a valid timestamp.
    ///
    /// @param pdu                  PDU to fill
    /// @param samples_per_channel  Number of samples in this packet
    /// @param gptp_now_ns          Current gPTP time
    void build_packet_header(AafPdu& pdu, uint16_t samples_per_channel, uint64_t gptp_now_ns) noexcept;
};

/// Serialize AAF audio stream — encodes float audio to any PCM format
///
/// Calls get_audio once per channel to obtain float samples, then encodes them
/// to the configured AAF format in channel-interleaved big-endian wire order.
///
/// @tparam MaxChannels   Maximum channel count for scratch allocation
/// @tparam MaxSamples    Maximum samples per packet for scratch allocation
/// @param ctx                  Stream output context (updated with sequence state)
/// @param pdu                  PDU header to fill
/// @param payload              Output buffer for encoded audio samples
/// @param samples_per_channel  Number of samples to serialize (runtime)
/// @param gptp_now_ns          Current gPTP time (for timestamp generation)
/// @param get_audio            Callback: void(uint8_t channel, std::span<float> dest)
///                             Called once per channel. Caller fills dest with samples_per_channel floats.
/// @return Number of bytes written to payload
template <size_t MaxChannels = 64, size_t MaxSamples = 256, typename AudioSource>
auto aaf_stream_serialize(
    AafStreamOutputContext& ctx,
    AafPdu& pdu,
    std::span<uint8_t> payload,
    uint16_t samples_per_channel,
    uint64_t gptp_now_ns,
    AudioSource const& get_audio) -> size_t
{
    uint16_t const channels = ctx.channel_count;
    size_t const bps = aaf_bytes_per_sample(ctx.format);
    if (!is_valid_aaf_serialize_params<MaxChannels, MaxSamples>(samples_per_channel, channels, bps)) {
        return 0;
    }

    size_t const required = static_cast<size_t>(samples_per_channel) * channels * bps;
    if (payload.size() < required) {
        return 0;
    }

    ctx.build_packet_header(pdu, samples_per_channel, gptp_now_ns);

    // Collect float samples from source into per-channel scratch buffers. Only the region the
    // encoder reads is zeroed (the array lives on the real-time stack), so unfilled dest is silence.
    std::array<std::array<float, MaxSamples>, MaxChannels> scratch;  // NOLINT(cppcoreguidelines-pro-type-member-init)
    for (uint16_t ch = 0; ch < channels; ++ch) {
        std::fill_n(scratch[ch].data(), samples_per_channel, 0.0F);
        get_audio(static_cast<uint8_t>(ch), std::span<float>{scratch[ch].data(), samples_per_channel});
    }

    // Encode as channel-interleaved big-endian samples
    size_t offset = 0;
    for (uint16_t s = 0; s < samples_per_channel; ++s) {
        for (uint16_t ch = 0; ch < channels; ++ch) {
            (void)encode_aaf_sample(scratch[ch][s], payload, offset, ctx.format);
            offset += bps;
        }
    }

    return required;
}

}  // namespace statusbar::avtp
