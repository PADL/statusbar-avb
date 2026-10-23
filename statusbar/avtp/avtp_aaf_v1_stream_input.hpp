#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AAF V1 stream input (listener/receiver) — callback-based deserialization
///
/// Version 1 simplifications over V0:
///   - 64-bit avtp_timestamp: no reconstruction needed (V0 needed 32→64 bit reconstruction)
///   - 32-bit sequence_num: reliable gap detection (V0 wrapped at 256)
///   - ptp_grandmaster_identity: clock domain tracking
///
/// References:
///   IEEE Std 1722-2025 Clause 7, Section 4.7.4 (version 1 common stream header)

#include "statusbar/avtp/avtp_aaf_stream_input.hpp"
#include "statusbar/avtp/avtp_aaf_v1.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace statusbar::avtp {

/// Per-stream state for AAF V1 deserialization (listener/receiver side).
/// One instance per received AAF V1 stream.
///
/// Simpler than V0: the 64-bit timestamp is used directly (no reconstruction),
/// and the 32-bit sequence number provides reliable gap detection.
struct AafV1StreamInputContext
{
    // Config
    AafFormat format;
    AafSampleRate sample_rate;
    uint16_t channel_count;
    uint8_t bit_depth;
    uint64_t sample_period_ns;

    // Running state
    uint32_t last_sequence_num{0};
    uint64_t last_pts_ns{0};        ///< Last valid 64-bit presentation timestamp (direct from packet)
    uint64_t last_gptp_time_ns{0};  ///< Approximate gPTP time of last received packet
    uint32_t samples_since_ts{0};   ///< Samples received since last valid timestamp
    bool has_valid_timestamp{false};
    ClockIdentity last_grandmaster{};  ///< Last seen PTP grandmaster identity
    bool grandmaster_changed{false};   ///< Set when grandmaster changes between packets

    // Statistics
    uint32_t packets_received{0};
    uint32_t timestamp_updates{0};
    uint32_t sequence_gaps{0};

    /// Construct with AAF format parameters
    AafV1StreamInputContext(AafFormat fmt, AafSampleRate rate, uint16_t channels, uint8_t depth);

    /// Reset running state while preserving configuration
    void reset() noexcept;

    /// Process a received V1 packet header — updates timestamp, sequence, and grandmaster state.
    /// Call compute_pts_ns() after this but before advance_samples().
    void process_packet_header(AafV1Pdu const& pdu, uint64_t gptp_now_ns) noexcept;

    /// Compute presentation time of the first sample in the current packet.
    [[nodiscard]] auto compute_pts_ns() const noexcept -> uint64_t;

    /// Advance the sample counter after processing a packet's audio data.
    void advance_samples(uint16_t sample_count) noexcept;
};

/// Deserialize AAF V1 audio stream — decodes any PCM format to float per-channel
///
/// Same callback pattern as V0 but uses AafV1Pdu and AafV1StreamInputContext.
///
/// @tparam MaxChannels   Maximum channel count for scratch allocation
/// @tparam MaxSamples    Maximum samples per packet for scratch allocation
/// @param ctx         Stream input context (updated with sequence/timestamp state)
/// @param pdu         Parsed AAF V1 PDU (header already validated)
/// @param payload     Audio payload bytes (after the 40-byte AafV1Pdu header)
/// @param gptp_now_ns Approximate gPTP time when the packet was received (full 64-bit)
/// @param on_audio    Callback: void(uint8_t channel, std::span<float> samples,
///                               uint64_t pts_ns, uint64_t sample_period_ns)
template <size_t MaxChannels = 64, size_t MaxSamples = 256, typename AudioCallback>
void aaf_v1_stream_deserialize(
    AafV1StreamInputContext& ctx,
    AafV1Pdu const& pdu,
    std::span<uint8_t const> payload,
    uint64_t gptp_now_ns,
    AudioCallback const& on_audio)
{
    uint16_t const channels = ctx.channel_count;
    size_t const bps = aaf_bytes_per_sample(ctx.format);
    if (!is_valid_aaf_deserialize_params<MaxChannels>(channels, bps)) {
        return;
    }

    uint16_t const samples = static_cast<uint16_t>(payload.size() / (bps * channels));
    if (samples == 0 || samples > MaxSamples) {
        return;
    }

    ctx.process_packet_header(pdu, gptp_now_ns);

    uint64_t const pts = ctx.compute_pts_ns();
    uint64_t const period = ctx.sample_period_ns;

    ctx.advance_samples(samples);

    // Decode the channel-interleaved, big-endian payload into per-channel scratch buffers. Not
    // zeroed (64 KB on the real-time stack at the defaults): only [0, channels) x [0, samples) is
    // written and handed on.
    std::array<std::array<float, MaxSamples>, MaxChannels> scratch;  // NOLINT(cppcoreguidelines-pro-type-member-init)

    size_t offset = 0;
    for (uint16_t s = 0; s < samples; ++s) {
        for (uint16_t ch = 0; ch < channels; ++ch) {
            scratch[ch][s] = decode_aaf_sample(payload, offset, ctx.format);
            offset += bps;
        }
    }

    for (uint16_t ch = 0; ch < channels; ++ch) {
        on_audio(static_cast<uint8_t>(ch), std::span<float>{scratch[ch].data(), samples}, pts, period);
    }
}

}  // namespace statusbar::avtp
