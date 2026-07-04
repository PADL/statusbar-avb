#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_talker_streams.hpp
/// @brief TalkerStreams — the local AVB stream TX path, extracted from
/// AvbEntityAudioIO (god-object phase 3).
///
/// Owns the qdisc-bypass TX socket, the AM824/AAF/CRF stream serializers, the
/// destination MACs, the TX packet counters, and the TX-capture recorder, plus
/// the three transmit_* methods that serialize one packet and put it on the
/// wire. Runs on the SCHED_FIFO media-timer thread (called from
/// AvbEntityAudioIO::process_audio). Reads the entity's config / media clock /
/// audio buffer / gPTP-now through references bound at construction (same names
/// as the entity's members, so the moved method bodies are unchanged).

#include "statusbar/avb_entity/avb_entity_aaf_reframe.hpp"
#include "statusbar/avb_entity/tx_pcap_recorder.hpp"
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_am824_stream_output.hpp"
#include "statusbar/avtp/avtp_crf_stream_output.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>

namespace statusbar::avb_entity {

/// The subset of an entity config that TalkerStreams needs, passed by value so any
/// talker entity (96 kHz or 48 kHz) can drive the shared TX path without depending on
/// a concrete entity config. Destructured at the call site (mirrors ListenerStreams).
struct TalkerStreamsConfig
{
    uint32_t sample_rate;                ///< stream sample rate (Hz), e.g. 96000 or 48000
    uint16_t crf_timestamp_interval;     ///< CRF events per timestamp (CRF base-freq domain)
    uint16_t crf_timestamps_per_packet;  ///< CRF timestamps per CRF PDU
    uint16_t vlan_id;                    ///< AVB VLAN id
    uint8_t stream_pcp;                  ///< AVB priority code point
};

struct TalkerStreams
{
    TalkerStreams(
        TalkerStreamsConfig const& config,
        ptpclient::MediaClockGenerator const& media_clock,
        std::pmr::vector<float>& audio_buffer,
        size_t const& channels,
        std::atomic<uint64_t> const& last_gptp_ns,
        std::pmr::memory_resource* memory_resource) noexcept
        : config_{config}
        , media_clock_{media_clock}
        , audio_buffer_{audio_buffer}
        , channels_{channels}
        , last_gptp_ns_{last_gptp_ns}
        , samples_per_packet_{config.sample_rate / CLASS_A_PACKETS_PER_SEC}
        , aaf_reframer_{channels, samples_per_packet_, 4, memory_resource}
    {}

    /// Serialize + send one packet of each stream. Media-timer (SCHED_FIFO) thread.
    void transmit_am824(uint64_t now_ns, uint32_t samples);
    void transmit_aaf(uint64_t now_ns, uint16_t samples, std::span<float const> src);
    /// Serialize + send one CRF PDU whose timestamps start at `base_index` in the
    /// media-clock (SAMPLE_RATE) sample-index domain. The caller passes the LIVE
    /// media-clock position (see crf_aligned_base) so the CRF conveys gPTP-now and
    /// not the anchor, regardless of when transmission (re)starts.
    void transmit_crf(uint64_t base_index);

    /// Emit this tick's CRF PDU when @p gate_open, decimated to the declared CRF rate
    /// (one PDU per pkts_per_crf audio packets). Re-bases to the live media-clock
    /// position each PDU so timestamps track gPTP-now. When @p gate_open is false the
    /// decimation phase resets so the next emission starts a fresh PDU. Owns the
    /// decimation counter so both entities share one copy of this logic; the caller
    /// supplies only the per-stream gate decision (and, for the tone generator, skips
    /// the call entirely when it has no CRF stream).
    void transmit_crf_if_due(ptpclient::MediaClockGenerator::Emit const& tick, bool gate_open);

    /// Push this tick's @p samples of interleaved audio through the AAF reframer and
    /// emit whole SAMPLES_PER_PACKET blocks (the variable-per-wake sample count is
    /// buffered to a constant on-wire cadence). When @p gate_open is false the reframer
    /// is cleared so a reconnect starts from a clean block boundary. Owns the reframer
    /// so both entities share one copy of the AAF egress logic.
    void transmit_aaf_if_due(ptpclient::MediaClockGenerator::Emit const& tick, bool gate_open, size_t samples);

    /// Class A wire cadence is fixed at 8000 pkt/s/stream; samples-per-packet is then
    /// sample_rate/8000 (12 @ 96k, 6 @ 48k) -- a runtime value now (samples_per_packet_),
    /// not a compile-time constant, so the block reframes correctly at either rate.
    static constexpr uint32_t CLASS_A_PACKETS_PER_SEC = 8000;
    static constexpr uint32_t CRF_BASE_FREQUENCY = 48000;

    // References / values (bound at construction).
    TalkerStreamsConfig config_;  ///< destructured entity config (rate + CRF/VLAN/PCP), by value
    ptpclient::MediaClockGenerator const& media_clock_;
    std::pmr::vector<float>& audio_buffer_;
    size_t const& channels_;
    std::atomic<uint64_t> const& last_gptp_ns_;
    uint32_t samples_per_packet_;  ///< sample_rate/CLASS_A_PACKETS_PER_SEC: 12 @ 96k, 6 @ 48k

    // Owned TX state.
    /// AAF reframe FIFO: the media clock is gPTP-paced, so a wake yields a variable
    /// sample count; AM824 sends it directly, AAF must emit constant-size blocks.
    AafReframer aaf_reframer_;
    net::RawnetContext stream_tx_{};
    std::optional<avtp::Am824StreamOutputContext> am824_out_{};
    ieee::Eui48 am824_dest_mac_{};
    std::optional<avtp::AafStreamOutputContext> aaf_out_{};
    ieee::Eui48 aaf_dest_mac_{};
    std::optional<avtp::CrfStreamOutputContext> crf_out_{};
    ieee::Eui48 crf_dest_mac_{};
    uint64_t am824_tx_packets_{0};
    uint64_t aaf_tx_packets_{0};
    /// CRF decimation phase: transmit one PDU every pkts_per_crf audio packets. Reset
    /// to 0 whenever the CRF gate is closed so a reconnect starts a fresh PDU.
    uint16_t crf_decim_{0};
    /// TX stream capture (diagnostic). last_tx_gptp_ns_ is set just before each
    /// send so the socket egress tap can stamp the captured frame with the gPTP TX time.
    TxPcapRecorder tx_pcap_recorder_{};
    uint64_t last_tx_gptp_ns_{0};
};

// ---------------------------------------------------------------------------
// CRF timestamp math (pure, socket-free -- unit-testable in isolation).
// ---------------------------------------------------------------------------

/// Spacing, in media-clock (@p sample_rate) samples, between consecutive CRF
/// timestamps. The DECLARED interval is in CRF base-frequency events; one such
/// event spans sample_rate/base_frequency audio samples. Returns 0 iff
/// base_frequency is 0 (degenerate config).
[[nodiscard]] constexpr auto crf_sample_stride(uint16_t interval, uint32_t base_frequency, uint32_t sample_rate) noexcept
    -> uint32_t
{
    return base_frequency == 0 ? 0U : static_cast<uint32_t>(interval) * sample_rate / base_frequency;
}

/// The media-clock base index for the next CRF PDU: the live media-clock
/// position (tick.first_index) floored to a sample_stride boundary so
/// successive PDUs stay contiguous. Re-basing to the live index each PDU is
/// what keeps CRF timestamps tracking gPTP-now instead of the anchor.
[[nodiscard]] constexpr auto crf_aligned_base(uint64_t first_index, uint32_t sample_stride) noexcept -> uint64_t
{
    return sample_stride == 0 ? first_index : (first_index / sample_stride) * sample_stride;
}

/// Fill `n_ts` CRF timestamps into `ts_data` (each avtp::CrfPdu::TIMESTAMP_SIZE
/// bytes) from `media_clock`, starting at `base_index` and spaced by
/// `sample_stride` in the media-clock sample-index domain. Pure: no socket,
/// no mutable state -- the timestamp VALUES are exactly the media clock's
/// presentation times for those sample indices.
void fill_crf_timestamps(
    ptpclient::MediaClockGenerator const& media_clock,
    uint64_t base_index,
    uint32_t sample_stride,
    uint16_t n_ts,
    std::span<uint8_t> ts_data) noexcept;

}  // namespace statusbar::avb_entity
