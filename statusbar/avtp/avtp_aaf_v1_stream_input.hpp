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
    int64_t last_pts_ns{0};        ///< Last valid 64-bit presentation timestamp (direct from packet; signed, see Nanoseconds)
    int64_t last_gptp_time_ns{0};  ///< Approximate time of the last received packet, on the stream's timeline
    uint32_t samples_since_ts{0};  ///< Samples received since last valid timestamp
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

    /// process_packet_header() told the time as a signed Nanoseconds value.
    void process_packet_header(AafV1Pdu const& pdu, Nanoseconds now) noexcept;

    /// The steps of process_packet_header(): the 32-bit sequence number and
    /// its gaps, the timestamp of a packet that carries one, and the
    /// grandmaster it names.
    void update_sequence_num(uint32_t seq_num) noexcept;
    void take_timestamp(AafV1Pdu const& pdu) noexcept;
    void note_grandmaster(ClockIdentity const& gm) noexcept;

    /// Compute presentation time of the first sample in the current packet.
    [[nodiscard]] auto compute_pts_ns() const noexcept -> uint64_t;

    /// compute_pts_ns() as a signed Nanoseconds value, zero before any timestamp.
    [[nodiscard]] auto compute_pts() const noexcept -> Nanoseconds;

    /// The time between two samples.
    [[nodiscard]] auto sample_period() const noexcept -> Nanoseconds { return Nanoseconds{static_cast<int64_t>(sample_period_ns)}; }

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
{  // NOLINT(statusbar-block-statements) one PDU decoded in order: checks, header, lanes, hand-off
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

/// The samples per channel of a packet the deserializer accepts, 0 when it is
/// empty, malformed for the context or over MaxSamples.
template <size_t MaxChannels, size_t MaxSamples>
[[nodiscard]] constexpr auto aaf_v1_samples_in_packet(AafV1StreamInputContext const& ctx, size_t const payload_size) noexcept
    -> size_t
{
    size_t const bps = aaf_bytes_per_sample(ctx.format);
    if (!is_valid_aaf_deserialize_params<MaxChannels>(ctx.channel_count, bps)) {
        return 0;
    }
    size_t const samples = payload_size / (bps * ctx.channel_count);
    return samples <= MaxSamples ? samples : 0;
}

/// Account one accepted packet in the context and return its first sample's
/// presentation time: the header first, the time while it is this packet's,
/// then the advance.
[[nodiscard]] inline auto aaf_v1_account_packet(
    AafV1StreamInputContext& ctx, AafV1Pdu const& pdu, Nanoseconds const now, size_t const samples) noexcept -> Nanoseconds
{
    ctx.process_packet_header(pdu, now);
    Nanoseconds const pts = ctx.compute_pts();
    ctx.advance_samples(static_cast<uint16_t>(samples));
    return pts;
}

/// aaf_v1_stream_deserialize() on the signed time: told @p now as Nanoseconds,
/// it hands each channel's presentation time and sample period on as Nanoseconds.
///
/// @param now       Approximate time on the stream's timeline when the packet was received
/// @param on_audio  Callback: void(uint8_t channel, std::span<float> samples,
///                             Nanoseconds pts, Nanoseconds sample_period)
template <size_t MaxChannels = 64, size_t MaxSamples = 256, typename AudioCallback>
void aaf_v1_stream_deserialize(
    AafV1StreamInputContext& ctx,
    AafV1Pdu const& pdu,
    std::span<uint8_t const> payload,
    Nanoseconds const now,
    AudioCallback const& on_audio)
{
    size_t const samples = aaf_v1_samples_in_packet<MaxChannels, MaxSamples>(ctx, payload.size());
    if (samples == 0) {
        return;
    }
    Nanoseconds const pts = aaf_v1_account_packet(ctx, pdu, now, samples);
    std::array<float, MaxChannels * MaxSamples> scratch{};
    aaf_decode_lanes(payload, ctx.format, ctx.channel_count, samples, scratch);
    for (uint16_t ch = 0; ch < ctx.channel_count; ++ch) {
        on_audio(static_cast<uint8_t>(ch), std::span<float>{scratch.data() + (ch * samples), samples}, pts, ctx.sample_period());
    }
}

}  // namespace statusbar::avtp
