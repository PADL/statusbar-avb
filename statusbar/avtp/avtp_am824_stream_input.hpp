#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AM824 stream input (listener/receiver) — callback-based deserialization
///
/// Provides Am824StreamInputContext for per-stream receive state (DBC unwrapping,
/// timestamp reconstruction) and callback-based deserializers for MBLA audio,
/// MIDI, and SMPTE.

#include "statusbar/avtp/avtp_am824_stream.hpp"

namespace statusbar::avtp {

/// Check if AM824 deserialize frame parameters are valid: at least one data block, a
/// channel count the scratch has lanes for, and a packet whose quadlets fit the decode
/// scratch. The scratch is one block of MaxChannels * MaxSamples floats shared by all
/// channels, so the bound is on the packet's quadlet count, not on samples per channel:
/// a 192 kHz class B stereo packet (48 data blocks, 96 quadlets) fits as well as a
/// 48 kHz 16-channel one (6 blocks, 96 quadlets).
template <size_t MaxChannels, size_t MaxSamples>
[[nodiscard]] constexpr auto is_valid_am824_deserialize_params(size_t samples, uint8_t channels) noexcept -> bool
{
    return samples != 0 && channels != 0 && channels <= MaxChannels && samples * channels <= MaxChannels * MaxSamples;
}

/// Per-stream state for AM824 deserialization (listener/receiver side).
/// One instance per received AM824 stream. Tracks DBC, timestamps, and statistics.
///
/// The caller must provide the approximate gPTP receive time to process_packet_header()
/// and the deserializer functions so that the 32-bit AVTP timestamp can be expanded to
/// a full 64-bit gPTP-domain presentation time.
struct Am824StreamInputContext
{
    Am824StreamConfig config;

    // Running state
    uint8_t last_dbc{0};
    uint8_t last_sequence_num{0};
    uint32_t last_avtp_timestamp{0};
    uint64_t last_anchor_pts_ns{0};  ///< Full 64-bit gPTP-domain anchor time
    uint64_t last_gptp_time_ns{0};   ///< Approximate gPTP time of last received packet
    uint32_t last_anchor_dbc{0};
    uint32_t running_dbc{0};
    bool has_valid_anchor{false};

    // Statistics
    uint32_t packets_received{0};
    uint32_t timestamp_updates{0};
    uint32_t sequence_gaps{0};

    /// Construct with sample rate and channel count
    Am824StreamInputContext(Am824SampleRate rate, uint8_t channels);

    /// Reset running state while preserving configuration
    void reset() noexcept;

    /// Update running_dbc from the 8-bit DBC field, unwrapping across 256 boundary
    void update_dbc(uint8_t dbc) noexcept;

    /// Update timestamp anchor from AVTPDU fields.
    /// @param tv             True if the avtp_timestamp field is valid
    /// @param avtp_timestamp The 32-bit AVTP presentation timestamp
    /// @param dbc            Current data block count from the AVTPDU
    /// @param gptp_now_ns    Approximate gPTP time when the packet was received (full 64-bit).
    ///                       Used to reconstruct the upper 32 bits of the presentation time.
    void update_timestamp(bool tv, uint32_t avtp_timestamp, uint8_t dbc, uint64_t gptp_now_ns) noexcept;

    /// Compute presentation time of the first data block in the current packet.
    /// Returns a full 64-bit gPTP-domain timestamp (not just the lower 32 bits).
    [[nodiscard]] auto compute_base_pts_ns() const noexcept -> uint64_t;

    /// Update sequence number and detect gaps
    void update_sequence_num(uint8_t seq_num) noexcept;

    /// Update context state from a packet header (combines sequence, DBC, and timestamp updates).
    /// @param pdu          The parsed AM824 PDU header
    /// @param sample_count Number of data blocks (samples per channel) in this packet
    /// @param gptp_now_ns  Approximate gPTP time when the packet was received (full 64-bit)
    void process_packet_header(Am824Pdu const& pdu, size_t sample_count, uint64_t gptp_now_ns) noexcept;
};

/// The data blocks in a packet the deserializers accept, with the header processed:
/// 0 when the packet is empty or over the scratch's capacity (then the header is not
/// processed either; the packet is dropped whole).
template <size_t MaxChannels, size_t MaxSamples>
[[nodiscard]] auto am824_accept_packet(
    Am824StreamInputContext& ctx, Am824Pdu const& pdu, std::span<uint8_t const> payload, uint64_t gptp_now_ns) noexcept -> size_t
{
    uint8_t const channels = ctx.config.channel_count;
    size_t const samples = channels == 0 ? 0 : payload.size() / (size_t{4} * channels);
    if (!is_valid_am824_deserialize_params<MaxChannels, MaxSamples>(samples, channels)) {
        return 0;
    }
    ctx.process_packet_header(pdu, samples, gptp_now_ns);
    return samples;
}

/// Decode MBLA quadlets into lanes: lane ch (at ch * samples) holds channel ch's
/// samples in order. Wire layout: data blocks are channel-interleaved
///   [ch0_s0, ch1_s0, ..., chN_s0, ch0_s1, ch1_s1, ..., chN_s1, ...]
inline void am824_decode_mbla_lanes(
    std::span<uint8_t const> payload, uint8_t channels, size_t samples, std::span<float> lanes) noexcept
{
    for (size_t s = 0; s < samples; ++s) {
        for (uint8_t ch = 0; ch < channels; ++ch) {
            ieee::quadlet_t q{};
            span_load(q, payload.subspan(((s * channels) + ch) * 4));
            lanes[(ch * samples) + s] = am824_sample_to_float(parse_am824_quadlet(q));
        }
    }
}

/// Deserialize MBLA-only AM824 stream — no label checking (hot path)
///
/// Assumes all channel slots contain MBLA audio (label 0x40).
/// Calls on_audio once per channel with decoded float samples and presentation time.
///
/// @tparam MaxChannels   Maximum channel count (default: Am824Pdu::MAX_CHANNELS)
/// @tparam MaxSamples    Scratch size per channel lane; the scratch (MaxChannels * MaxSamples floats) is shared
///                      by all channels, so a packet may carry up to that many quadlets (default: Am824Pdu::MAX_SAMPLES_PER_PACKET)
/// @param ctx         Stream input context (updated with DBC/timestamp/sequence state)
/// @param pdu         Parsed AM824 PDU (header already validated)
/// @param payload     Audio payload bytes (after the 32-byte Am824Pdu header)
/// @param gptp_now_ns Approximate gPTP time when the packet was received (full 64-bit).
/// @param on_audio    Callback: (uint8_t channel, std::span<float> samples,
///                               uint64_t base_pts_ns, uint64_t sample_period_ns)
template <size_t MaxChannels = Am824Pdu::MAX_CHANNELS, size_t MaxSamples = Am824Pdu::MAX_SAMPLES_PER_PACKET, typename AudioCallback>
void am824_deserialize_mbla(
    Am824StreamInputContext& ctx,
    Am824Pdu const& pdu,
    std::span<uint8_t const> payload,
    uint64_t gptp_now_ns,
    AudioCallback const& on_audio)
{
    size_t const samples = am824_accept_packet<MaxChannels, MaxSamples>(ctx, pdu, payload, gptp_now_ns);
    if (samples == 0) {
        return;
    }
    std::array<float, MaxChannels * MaxSamples> scratch{};
    am824_decode_mbla_lanes(payload, ctx.config.channel_count, samples, scratch);
    uint64_t const base_pts = ctx.compute_base_pts_ns();
    for (uint8_t ch = 0; ch < ctx.config.channel_count; ++ch) {
        on_audio(ch, std::span<float>{scratch.data() + (ch * samples), samples}, base_pts, ctx.config.sample_period_ns);
    }
}

/// The scratch of the mixed deserializer: one lane per channel in each store (lane ch
/// at ch * samples), with a fill count per lane, since a channel yields at most one
/// sample, or three MIDI bytes, per data block and not every data block carries one.
template <size_t MaxChannels, size_t MaxSamples>
struct Am824MixedLanes
{
    std::array<float, MaxChannels * MaxSamples> audio{};
    std::array<size_t, MaxChannels> audio_count{};
    std::array<uint8_t, MaxChannels * MaxSamples * 3> midi{};
    std::array<size_t, MaxChannels> midi_count{};

    [[nodiscard]] auto audio_lane(uint8_t ch, size_t samples) noexcept -> std::span<float>
    {
        return std::span<float>{audio.data() + (ch * samples), audio_count[ch]};
    }
    [[nodiscard]] auto midi_lane(uint8_t ch, size_t samples) const noexcept -> std::span<uint8_t const>
    {
        return std::span<uint8_t const>{midi.data() + (ch * samples * 3), midi_count[ch]};
    }

    /// Sort one quadlet by label into the lanes; SMPTE parts go straight to @p on_smpte.
    template <typename SmpteCb>
    void take(uint8_t ch, size_t samples, uint32_t quadlet, uint64_t base_pts, uint64_t period, SmpteCb const& on_smpte)
    {
        uint8_t const label = static_cast<uint8_t>((quadlet >> 24) & 0xFFU);
        uint8_t const label_upper = label & 0xF0U;
        if (label_upper == 0x40U) {
            // MBLA audio (0x40-0x4F)
            audio[(ch * samples) + audio_count[ch]] = am824_sample_to_float(parse_am824_quadlet(quadlet));
            ++audio_count[ch];
        } else if (label_upper == 0x80U && label <= 0x83U) {
            // MIDI conformant (0x80-0x83)
            midi_count[ch] += am824_extract_midi_bytes(quadlet, midi.data() + (ch * samples * 3) + midi_count[ch]);
        } else if (label >= 0x88U && label <= 0x8BU) {
            // SMPTE time code conformant (0x88-0x8B)
            std::array<uint8_t, 3> smpte_bytes{};
            uint8_t const part = am824_extract_smpte_part(quadlet, smpte_bytes);
            if (part > 0) {
                on_smpte(ch, part, std::span<uint8_t const, 3>{smpte_bytes}, base_pts, period);
            }
        }
    }
};

/// Deserialize mixed AM824 stream — dispatches by label per channel
///
/// Walks each quadlet, checks label, dispatches to typed callbacks:
///   MBLA (0x40-0x4F) → on_audio: aggregated float samples per channel
///   MIDI (0x80-0x83) → on_midi: aggregated valid MIDI bytes per channel
///   SMPTE (0x88-0x8B) → on_smpte: called per non-empty quadlet with part number
///   Other labels → silently skipped
///
/// @tparam MaxChannels   Maximum channel count (default: Am824Pdu::MAX_CHANNELS)
/// @tparam MaxSamples    Scratch size per channel lane; the scratch (MaxChannels * MaxSamples floats) is shared
///                      by all channels, so a packet may carry up to that many quadlets (default: Am824Pdu::MAX_SAMPLES_PER_PACKET)
/// @param gptp_now_ns Approximate gPTP time when the packet was received (full 64-bit).
template <
    size_t MaxChannels = Am824Pdu::MAX_CHANNELS,
    size_t MaxSamples = Am824Pdu::MAX_SAMPLES_PER_PACKET,
    typename AudioCb,
    typename MidiCb,
    typename SmpteCb>
void am824_deserialize_mixed(
    Am824StreamInputContext& ctx,
    Am824Pdu const& pdu,
    std::span<uint8_t const> payload,
    uint64_t gptp_now_ns,
    AudioCb const& on_audio,
    MidiCb const& on_midi,
    SmpteCb const& on_smpte)
{
    size_t const samples = am824_accept_packet<MaxChannels, MaxSamples>(ctx, pdu, payload, gptp_now_ns);
    if (samples == 0) {
        return;
    }
    uint64_t const base_pts = ctx.compute_base_pts_ns();
    Am824MixedLanes<MaxChannels, MaxSamples> lanes{};
    for (size_t s = 0; s < samples; ++s) {
        for (uint8_t ch = 0; ch < ctx.config.channel_count; ++ch) {
            ieee::quadlet_t q{};
            span_load(q, payload.subspan(((s * ctx.config.channel_count) + ch) * 4));
            lanes.take(ch, samples, q, base_pts, ctx.config.sample_period_ns, on_smpte);
        }
    }
    for (uint8_t ch = 0; ch < ctx.config.channel_count; ++ch) {
        if (lanes.audio_count[ch] > 0) {
            on_audio(ch, lanes.audio_lane(ch, samples), base_pts, ctx.config.sample_period_ns);
        }
        if (lanes.midi_count[ch] > 0) {
            on_midi(ch, lanes.midi_lane(ch, samples), base_pts, ctx.config.sample_period_ns);
        }
    }
}

}  // namespace statusbar::avtp
