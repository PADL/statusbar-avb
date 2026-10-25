#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AM824 stream input (listener/receiver) — callback-based deserialization
///
/// Provides Am824StreamInputContext for per-stream receive state (DBC unwrapping,
/// timestamp reconstruction) and callback-based deserializers for MBLA audio,
/// MIDI, and SMPTE.

#include "statusbar/avtp/avtp_am824_stream.hpp"
#include "statusbar/sg14/inplace_vector.h"

namespace statusbar::avtp {

/// Check if AM824 deserialize frame parameters are valid
template <size_t MaxChannels, size_t MaxSamples>
[[nodiscard]] constexpr auto is_valid_am824_deserialize_params(uint8_t samples, uint8_t channels) noexcept -> bool
{
    return samples != 0 && channels != 0 && channels <= MaxChannels && samples <= MaxSamples;
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
    uint8_t last_sample_count{0};  ///< data blocks in the last packet
    bool has_valid_anchor{false};

    // Statistics
    uint32_t packets_received{0};
    uint32_t timestamp_updates{0};
    uint32_t sequence_gaps{0};
    uint32_t dbc_discontinuities{0};  ///< packets whose DBC is not the last DBC plus its block count

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
    /// @param sample_count Number of samples in this packet
    /// @param gptp_now_ns  Approximate gPTP time when the packet was received (full 64-bit)
    void process_packet_header(Am824Pdu const& pdu, uint8_t sample_count, uint64_t gptp_now_ns) noexcept;
};

/// Deserialize MBLA-only AM824 stream — no label checking (hot path)
///
/// Assumes all channel slots contain MBLA audio (label 0x40).
/// Calls on_audio once per channel with decoded float samples and presentation time.
///
/// @tparam MaxChannels   Maximum channel count for scratch allocation (default: Am824Pdu::MAX_CHANNELS)
/// @tparam MaxSamples    Maximum samples per packet for scratch allocation (default: Am824Pdu::MAX_SAMPLES_PER_PACKET)
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
    uint8_t const channels = ctx.config.channel_count;
    if (channels == 0) {
        return;
    }
    size_t const blocks = payload.size() / (size_t{4} * channels);
    uint8_t const samples = blocks > MaxSamples ? 0 : static_cast<uint8_t>(blocks);
    if (!is_valid_am824_deserialize_params<MaxChannels, MaxSamples>(samples, channels)) {
        return;
    }

    ctx.process_packet_header(pdu, samples, gptp_now_ns);

    uint64_t const base_pts = ctx.compute_base_pts_ns();
    uint64_t const period = ctx.config.sample_period_ns;

    // Decode quadlets into per-channel scratch buffers
    // Layout: data blocks are channel-interleaved
    //   [ch0_s0, ch1_s0, ..., chN_s0, ch0_s1, ch1_s1, ..., chN_s1, ...]
    // Not zeroed (MaxChannels x MaxSamples floats on the real-time stack, 64 KB
    // at the AAF defaults): the loop below writes [0, channels) x [0, samples)
    // and only that region is handed on.
    std::array<std::array<float, MaxSamples>, MaxChannels> scratch;  // NOLINT(cppcoreguidelines-pro-type-member-init)

    for (uint8_t s = 0; s < samples; ++s) {
        for (uint8_t ch = 0; ch < channels; ++ch) {
            ieee::quadlet_t q{};
            span_load(q, payload.subspan(static_cast<size_t>((s * channels) + ch) * 4));
            scratch[ch][s] = am824_sample_to_float(parse_am824_quadlet(q));
        }
    }

    for (uint8_t ch = 0; ch < channels; ++ch) {
        on_audio(ch, std::span<float>{scratch[ch].data(), samples}, base_pts, period);
    }
}

/// Deserialize mixed AM824 stream — dispatches by label per channel
///
/// Walks each quadlet, checks label, dispatches to typed callbacks:
///   MBLA (0x40-0x4F) → on_audio: aggregated float samples per channel
///   MIDI (0x80-0x83) → on_midi: aggregated valid MIDI bytes per channel
///   SMPTE (0x88-0x8B) → on_smpte: called per non-empty quadlet with part number
///   Other labels → silently skipped
///
/// @tparam MaxChannels   Maximum channel count for scratch allocation (default: Am824Pdu::MAX_CHANNELS)
/// @tparam MaxSamples    Maximum samples per packet for scratch allocation (default: Am824Pdu::MAX_SAMPLES_PER_PACKET)
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
    uint8_t const channels = ctx.config.channel_count;
    if (channels == 0) {
        return;
    }
    size_t const blocks = payload.size() / (size_t{4} * channels);
    uint8_t const samples = blocks > MaxSamples ? 0 : static_cast<uint8_t>(blocks);
    if (!is_valid_am824_deserialize_params<MaxChannels, MaxSamples>(samples, channels)) {
        return;
    }

    ctx.process_packet_header(pdu, samples, gptp_now_ns);

    uint64_t const base_pts = ctx.compute_base_pts_ns();
    uint64_t const period = ctx.config.sample_period_ns;

    // Per-channel scratch buffers
    std::array<statusbar::sg14::inplace_vector<float, MaxSamples>, MaxChannels> audio_scratch{};

    constexpr size_t max_midi_bytes = MaxSamples * 3;
    std::array<std::array<uint8_t, max_midi_bytes>, MaxChannels> midi_scratch{};
    std::array<size_t, MaxChannels> midi_count{};

    for (uint8_t s = 0; s < samples; ++s) {
        for (uint8_t ch = 0; ch < channels; ++ch) {
            ieee::quadlet_t q{};
            span_load(q, payload.subspan(static_cast<size_t>((s * channels) + ch) * 4));
            uint32_t const quadlet = q;

            uint8_t const label = static_cast<uint8_t>((quadlet >> 24) & 0xFFU);
            uint8_t const label_upper = label & 0xF0U;

            if (label_upper == 0x40U) {
                // MBLA audio (0x40-0x4F)
                audio_scratch[ch].push_back(am824_sample_to_float(parse_am824_quadlet(quadlet)));
            } else if (label_upper == 0x80U && label <= 0x83U) {
                // MIDI conformant (0x80-0x83)
                uint8_t const n = am824_extract_midi_bytes(quadlet, midi_scratch[ch].data() + midi_count[ch]);
                midi_count[ch] += n;
            } else if (label >= 0x88U && label <= 0x8BU) {
                // SMPTE time code conformant (0x88-0x8B)
                std::array<uint8_t, 3> smpte_bytes{};
                uint8_t const part = am824_extract_smpte_part(quadlet, smpte_bytes);
                if (part > 0) {
                    on_smpte(ch, part, std::span<uint8_t const, 3>{smpte_bytes}, base_pts, period);
                }
            }
        }
    }

    for (uint8_t ch = 0; ch < channels; ++ch) {
        if (!audio_scratch[ch].empty()) {
            on_audio(ch, std::span<float>{audio_scratch[ch]}, base_pts, period);
        }
    }

    for (uint8_t ch = 0; ch < channels; ++ch) {
        if (midi_count[ch] > 0) {
            on_midi(ch, std::span<uint8_t const>{midi_scratch[ch].data(), midi_count[ch]}, base_pts, period);
        }
    }
}

}  // namespace statusbar::avtp
