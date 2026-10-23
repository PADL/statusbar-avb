#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AAF V1 stream output (talker/transmitter) — callback-based serialization
///
/// Version 1 simplifications over V0:
///   - 64-bit avtp_timestamp: full PTP time set directly (V0 truncated to 32 bits)
///   - 32-bit sequence_num: no 256-wrap (V0 wrapped at 0xFF)
///   - ptp_grandmaster_identity: set per-packet for clock domain identification
///
/// References:
///   IEEE Std 1722-2025 Clause 7, Section 4.7.4 (version 1 common stream header)

#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_aaf_v1.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace statusbar::avtp {

/// Per-stream state for AAF V1 serialization (talker/transmitter side).
/// One instance per transmitted AAF V1 stream.
struct AafV1StreamOutputContext
{
    // Config
    AafFormat format;
    AafSampleRate sample_rate;
    uint16_t channel_count;
    uint8_t bit_depth;
    uint64_t sample_period_ns;
    StreamId stream_id;
    uint64_t presentation_offset_ns;     ///< How far ahead of gptp_now to set presentation time
    ClockIdentity grandmaster_identity;  ///< PTP grandmaster to stamp into packets

    // Running state
    uint32_t sequence_num{0};
    uint32_t running_sample_count{0};

    // Statistics
    uint32_t packets_sent{0};

    /// Construct with stream parameters
    AafV1StreamOutputContext(
        StreamId sid,
        AafFormat fmt,
        AafSampleRate rate,
        uint16_t channels,
        uint8_t depth,
        uint64_t pres_offset,
        ClockIdentity gm = ClockIdentity{});

    /// Reset running state while preserving configuration
    void reset() noexcept;

    /// Build the V1 packet header for the next packet.
    void build_packet_header(AafV1Pdu& pdu, uint16_t samples_per_channel, uint64_t gptp_now_ns) noexcept;
};

/// Serialize AAF V1 audio stream — encodes float audio to any PCM format
///
/// Same callback pattern as V0 but uses AafV1Pdu and AafV1StreamOutputContext.
///
/// @tparam MaxChannels   Maximum channel count for scratch allocation
/// @tparam MaxSamples    Maximum samples per packet for scratch allocation
/// @param ctx                  Stream output context (updated with sequence state)
/// @param pdu                  V1 PDU header to fill
/// @param payload              Output buffer for encoded audio samples
/// @param samples_per_channel  Number of samples to serialize (runtime)
/// @param gptp_now_ns          Current gPTP time (for timestamp generation)
/// @param get_audio            Callback: void(uint8_t channel, std::span<float> dest)
/// @return Number of bytes written to payload
template <size_t MaxChannels = 64, size_t MaxSamples = 256, typename AudioSource>
auto aaf_v1_stream_serialize(
    AafV1StreamOutputContext& ctx,
    AafV1Pdu& pdu,
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
