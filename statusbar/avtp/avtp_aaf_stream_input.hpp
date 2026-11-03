#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AAF stream input (listener/receiver) — callback-based deserialization
///
/// Provides AafStreamInputContext for per-stream receive state (sequence tracking,
/// timestamp reconstruction) and a callback-based deserializer that decodes any
/// AAF PCM format (int16, int24, int32, float32) to float.
///
/// References:
///   IEEE Std 1722-2016 Clause 7 (AVTP Audio Format)

#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_stream_common.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace statusbar::avtp {

/// Check if AAF deserialize frame parameters are valid
template <size_t MaxChannels>
[[nodiscard]] constexpr auto is_valid_aaf_deserialize_params(uint16_t channels, size_t bps) noexcept -> bool
{
    return channels != 0 && bps != 0 && channels <= MaxChannels;
}

/// Per-stream state for AAF deserialization (listener/receiver side).
/// One instance per received AAF stream.
///
/// AAF is simpler than AM824: no DBC, no syt_interval. Each packet with tv=1
/// carries a direct presentation timestamp; packets with tv=0 have their PTS
/// extrapolated from the last valid timestamp using the sample period.
struct AafStreamInputContext
{
    // Config
    AafFormat format;
    AafSampleRate sample_rate;
    uint16_t channel_count;
    uint8_t bit_depth;
    uint64_t sample_period_ns;

    // Running state
    uint8_t last_sequence_num{0};
    uint64_t last_pts_ns{0};        ///< Last reconstructed full 64-bit presentation time
    uint64_t last_gptp_time_ns{0};  ///< Approximate gPTP time of last received packet
    uint32_t samples_since_ts{0};   ///< Samples received since last valid timestamp
    bool has_valid_timestamp{false};

    // Statistics
    uint32_t packets_received{0};
    uint32_t timestamp_updates{0};
    uint32_t sequence_gaps{0};

    /// Construct with AAF format parameters
    AafStreamInputContext(AafFormat fmt, AafSampleRate rate, uint16_t channels, uint8_t depth);

    /// Reset running state while preserving configuration
    void reset() noexcept;

    /// Update sequence number and detect gaps
    void update_sequence_num(uint8_t seq_num) noexcept;

    /// Process a received packet header — updates timestamp and sequence state.
    /// Call compute_pts_ns() after this but before advance_samples().
    /// @param pdu          The parsed AAF PDU header
    /// @param gptp_now_ns  Approximate gPTP time when the packet was received (full 64-bit)
    void process_packet_header(AafPdu const& pdu, uint64_t gptp_now_ns) noexcept;

    /// Compute presentation time of the first sample in the current packet.
    /// Must be called after process_packet_header() and before advance_samples().
    /// Returns a full 64-bit gPTP-domain timestamp.
    [[nodiscard]] auto compute_pts_ns() const noexcept -> uint64_t;

    /// Advance the sample counter after processing a packet's audio data.
    /// @param sample_count Number of samples per channel in the packet just processed
    void advance_samples(uint16_t sample_count) noexcept;
};

/// Frames per channel in an AAF payload (trailing bytes short of a frame not counted), or 0 if
/// aaf_stream_deserialize would refuse it (no channels, unsized format, more than MaxChannels or
/// MaxSamples, no whole frame): the acceptance rule every AAF receive path shares.
template <size_t MaxChannels = 64, size_t MaxSamples = 256>
[[nodiscard]] constexpr auto aaf_payload_frames(AafStreamInputContext const& ctx, std::span<uint8_t const> payload) noexcept
    -> uint16_t
{
    uint16_t const channels = ctx.channel_count;
    size_t const bps = aaf_bytes_per_sample(ctx.format);
    if (!is_valid_aaf_deserialize_params<MaxChannels>(channels, bps)) {
        return 0;
    }
    uint16_t const samples = static_cast<uint16_t>(payload.size() / (bps * channels));
    if (samples == 0 || samples > MaxSamples) {
        return 0;
    }
    return samples;
}

/// Deserialize AAF audio stream — decodes any PCM format to float per-channel
///
/// Calls on_audio once per channel with decoded float samples and presentation time.
/// Supports int16, int24, int32, and float32 formats.
///
/// @tparam MaxChannels   Maximum channel count for scratch allocation
/// @tparam MaxSamples    Maximum samples per packet for scratch allocation
/// @param ctx         Stream input context (updated with sequence/timestamp state)
/// @param pdu         Parsed AAF PDU (header already validated)
/// @param payload     Audio payload bytes (after the 24-byte AafPdu header)
/// @param gptp_now_ns Approximate gPTP time when the packet was received (full 64-bit)
/// @param on_audio    Callback: void(uint8_t channel, std::span<float> samples,
///                               uint64_t pts_ns, uint64_t sample_period_ns)
template <size_t MaxChannels = 64, size_t MaxSamples = 256, typename AudioCallback>
void aaf_stream_deserialize(
    AafStreamInputContext& ctx,
    AafPdu const& pdu,
    std::span<uint8_t const> payload,
    uint64_t gptp_now_ns,
    AudioCallback const& on_audio)
{
    uint16_t const samples = aaf_payload_frames<MaxChannels, MaxSamples>(ctx, payload);
    if (samples == 0) {
        return;
    }
    uint16_t const channels = ctx.channel_count;
    size_t const bps = aaf_bytes_per_sample(ctx.format);

    ctx.process_packet_header(pdu, gptp_now_ns);

    uint64_t const pts = ctx.compute_pts_ns();
    uint64_t const period = ctx.sample_period_ns;

    ctx.advance_samples(samples);

    // Decode payload into per-channel scratch buffers
    // AAF wire layout: channel-interleaved, big-endian
    //   [ch0_s0, ch1_s0, ..., chN_s0, ch0_s1, ch1_s1, ..., chN_s1, ...]
    std::array<std::array<float, MaxSamples>, MaxChannels> scratch{};

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
