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

/// The shape of a packet the deserializer accepts: its samples per channel, 0 when it
/// is empty, malformed for the context or over the scratch's capacity. The scratch is
/// one block of MaxChannels * MaxSamples floats shared by all channels, so the bound is
/// on the packet's sample total, not on samples per channel: a mono int16 packet filling
/// an Ethernet frame (738 samples) fits as well as 64 channels of 11.
template <size_t MaxChannels, size_t MaxSamples>
[[nodiscard]] constexpr auto aaf_samples_in_packet(AafStreamInputContext const& ctx, size_t payload_size) noexcept -> size_t
{
    size_t const bps = aaf_bytes_per_sample(ctx.format);
    if (!is_valid_aaf_deserialize_params<MaxChannels>(ctx.channel_count, bps)) {
        return 0;
    }
    size_t const samples = payload_size / (bps * ctx.channel_count);
    return samples * ctx.channel_count <= MaxChannels * MaxSamples ? samples : 0;
}

/// Account one accepted packet in the context and return its first sample's PTS:
/// the header first, the PTS while it is this packet's, then the advance.
[[nodiscard]] inline auto aaf_account_packet(
    AafStreamInputContext& ctx, AafPdu const& pdu, uint64_t gptp_now_ns, size_t samples) noexcept -> uint64_t
{
    ctx.process_packet_header(pdu, gptp_now_ns);
    uint64_t const pts = ctx.compute_pts_ns();
    ctx.advance_samples(static_cast<uint16_t>(samples));
    return pts;
}

/// Decode the payload into lanes: lane ch (at ch * samples) holds channel ch's samples
/// in order. AAF wire layout: channel-interleaved, big-endian
///   [ch0_s0, ch1_s0, ..., chN_s0, ch0_s1, ch1_s1, ..., chN_s1, ...]
inline void aaf_decode_lanes(
    std::span<uint8_t const> payload, AafFormat format, uint16_t channels, size_t samples, std::span<float> lanes) noexcept
{
    size_t const bps = aaf_bytes_per_sample(format);
    size_t offset = 0;
    for (size_t s = 0; s < samples; ++s) {
        for (uint16_t ch = 0; ch < channels; ++ch) {
            lanes[(ch * samples) + s] = decode_aaf_sample(payload, offset, format);
            offset += bps;
        }
    }
}

/// Deserialize AAF audio stream — decodes any PCM format to float per-channel
///
/// Calls on_audio once per channel with decoded float samples and presentation time.
/// Supports int16, int24, int32, and float32 formats.
///
/// @tparam MaxChannels   Maximum channel count
/// @tparam MaxSamples    Scratch size per channel lane; the scratch (MaxChannels * MaxSamples floats) is
///                      shared by all channels, so a packet may carry up to that many samples in all
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
    size_t const samples = aaf_samples_in_packet<MaxChannels, MaxSamples>(ctx, payload.size());
    if (samples == 0) {
        return;
    }
    uint64_t const pts = aaf_account_packet(ctx, pdu, gptp_now_ns, samples);
    std::array<float, MaxChannels * MaxSamples> scratch{};
    aaf_decode_lanes(payload, ctx.format, ctx.channel_count, samples, scratch);
    for (uint16_t ch = 0; ch < ctx.channel_count; ++ch) {
        on_audio(static_cast<uint8_t>(ch), std::span<float>{scratch.data() + (ch * samples), samples}, pts, ctx.sample_period_ns);
    }
}

}  // namespace statusbar::avtp
