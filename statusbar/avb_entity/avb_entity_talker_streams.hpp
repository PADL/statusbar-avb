#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_talker_streams.hpp
/// @brief TalkerStreams — the local AVB stream TX path.
///
/// Owns the qdisc-bypass TX socket, N per-stream TX slots (each holding the
/// kind-matching AM824/AAF/CRF serializer shaped by its StreamSpec), the TX
/// packet counters, and the TX-capture recorder. Runs on the SCHED_FIFO
/// media-timer thread (called from the entities' process_audio). Reads the
/// entity's config / media clock / audio buffer / gPTP-now through references
/// bound at construction.
///
/// Entity Construction Kit phase 1: the stream table is no longer three named
/// AM824/AAF/CRF slots — it is a vector of slots derived from the blob's
/// STREAM_OUTPUT descriptors (see avb_entity_stream_spec.hpp), so an entity's
/// TX shape follows its declarative model instead of per-entity C++ enums.

#include "statusbar/avb_entity/avb_entity_aaf_reframe.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/tx_pcap_recorder.hpp"
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_am824_stream_output.hpp"
#include "statusbar/avtp/avtp_crf_stream_output.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
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
    uint32_t sample_rate;  ///< stream sample rate (Hz), e.g. 96000 or 48000
    uint16_t vlan_id;      ///< AVB VLAN id
    uint8_t stream_pcp;    ///< AVB priority code point
};

/// One TX stream slot: the StreamSpec that shaped it, its destination MAC,
/// its serializer (exactly one of am824/aaf/crf engaged, per spec.format.kind),
/// and its counters. AAF slots also own their reframe FIFO (the media clock is
/// gPTP-paced, so a wake yields a variable sample count; AAF must emit
/// constant-size blocks).
struct TalkerStreamSlot
{
    StreamSpec spec{};
    ieee::Eui48 dest_mac{};
    uint64_t tx_packets{0};
    /// CRF decimation phase: transmit one PDU every pkts_per_crf audio packets.
    /// Reset to 0 whenever the CRF gate is closed so a reconnect starts fresh.
    uint16_t crf_decim{0};
    std::optional<avtp::Am824StreamOutputContext> am824{};
    std::optional<avtp::AafStreamOutputContext> aaf{};
    std::optional<avtp::CrfStreamOutputContext> crf{};
    std::optional<AafReframer> reframer{};
};

struct TalkerStreams
{
    TalkerStreams(
        TalkerStreamsConfig const& config,
        ptpclient::MediaClockGenerator const& media_clock,
        std::atomic<uint64_t> const& last_gptp_ns,
        std::pmr::memory_resource* memory_resource) noexcept
        : config_{config}
        , media_clock_{media_clock}
        , last_gptp_ns_{last_gptp_ns}
        , samples_per_packet_{config.sample_rate / CLASS_A_PACKETS_PER_SEC}
        , mem_resource_{memory_resource}
    {}

    /// TX pcap capture knobs for open_tx (see TxPcapRecorder). Empty path =
    /// capture disabled.
    struct TxPcapConfig
    {
        std::string_view path{};
        size_t max_bytes{0};
        uint32_t seconds{0};
    };

    /// Open the qdisc-bypass TX socket on @p interface_name and (optionally)
    /// arm the TX pcap capture with its socket egress tap. One call from the
    /// owning entity's start() — the socket, the recorder and the tap are
    /// THIS class's state, so this class assembles them (refactor phase A:
    /// entities no longer reach into stream_tx_/tx_pcap_recorder_).
    auto open_tx(std::string_view interface_name, TxPcapConfig const& pcap) -> Status;
    auto open_tx(std::string_view const interface_name) -> Status { return open_tx(interface_name, TxPcapConfig{}); }

    /// TX pcap forwarding (see TxPcapRecorder): the capture window closed and
    /// the file awaits the non-RT flush.
    [[nodiscard]] auto tx_pcap_ready_to_write() const noexcept -> bool { return tx_pcap_recorder_.ready_to_write(); }
    [[nodiscard]] auto flush_tx_pcap() -> Status { return tx_pcap_recorder_.write_to_file(); }
    [[nodiscard]] auto tx_pcap_frame_count() const noexcept -> size_t { return tx_pcap_recorder_.frame_count(); }

    /// Add a TX slot shaped by @p spec, constructing the kind-matching
    /// serializer for @p stream_id. CRF timing (base frequency, interval,
    /// timestamps per PDU) comes from the spec's decoded format word — the
    /// blob is the single source. Errors: unknown/unsupported format kind or
    /// rate, or more than MAX_ENTITY_STREAMS slots.
    auto open_stream(StreamSpec const& spec, tsn::StreamId stream_id, ieee::Eui48 dest_mac) -> Status;

    /// The slot whose spec.index == @p stream_index (the STREAM_OUTPUT
    /// descriptor index == ACMP talker unique id), or nullptr.
    [[nodiscard]] auto slot_for(uint16_t stream_index) noexcept -> TalkerStreamSlot*;
    [[nodiscard]] auto slot_for(uint16_t stream_index) const noexcept -> TalkerStreamSlot const*;
    /// The first slot of @p kind, or nullptr.
    [[nodiscard]] auto slot_of(StreamKind kind) noexcept -> TalkerStreamSlot*;
    [[nodiscard]] auto slot_of(StreamKind kind) const noexcept -> TalkerStreamSlot const*;

    /// Emit this tick's traffic for @p slot from @p src (this stream's
    /// interleaved audio for the tick — per-stream sources are the entity's
    /// business; CRF slots ignore it): AM824 sends the tick's samples
    /// directly; AAF reframes to constant blocks (gate closed clears the
    /// FIFO); CRF emits one decimated PDU re-based to the live media-clock
    /// position (gate closed resets the phase). Media-timer (SCHED_FIFO) thread.
    void transmit_if_due(
        TalkerStreamSlot& slot,
        ptpclient::MediaClockGenerator::Emit const& tick,
        bool gate_open,
        size_t samples,
        std::span<float const> src);

    /// STOP_STREAMING gate for one STREAM_OUTPUT descriptor index (kit
    /// phase 5): a stopped stream is treated as gate-closed by
    /// transmit_if_due (AAF FIFO cleared, CRF phase reset). Reactor thread
    /// writes, media thread reads; streams default to streaming.
    void set_stream_stopped(uint16_t const stream_index, bool const stopped) noexcept
    {
        if (stream_index < stream_stopped_.size()) {
            stream_stopped_[stream_index].store(stopped, std::memory_order_release);
        }
    }
    [[nodiscard]] auto stream_stopped(uint16_t const stream_index) const noexcept -> bool
    {
        return stream_index < stream_stopped_.size() && stream_stopped_[stream_index].load(std::memory_order_acquire);
    }

    /// Serialize + send one packet on a specific slot from @p src (interleaved,
    /// slot.spec.format.channels stride). Media-timer thread. (Public for
    /// entities that drive their own cadence, e.g. the pipe-fed AM824 loopback
    /// entities.)
    void transmit_am824(TalkerStreamSlot& slot, uint64_t now_ns, uint32_t samples, std::span<float const> src);
    void transmit_aaf(TalkerStreamSlot& slot, uint64_t now_ns, uint16_t samples, std::span<float const> src);
    /// One CRF PDU whose timestamps start at `base_index` in the media-clock
    /// sample-index domain (callers pass the LIVE position; see crf_aligned_base).
    void transmit_crf(TalkerStreamSlot& slot, uint64_t base_index);

    /// Class A wire cadence is fixed at 8000 pkt/s/stream; samples-per-packet is then
    /// sample_rate/8000 (12 @ 96k, 6 @ 48k).
    static constexpr uint32_t CLASS_A_PACKETS_PER_SEC = avb_entity::CLASS_A_PACKETS_PER_SEC;

    // References / values (bound at construction).
    TalkerStreamsConfig config_;  ///< destructured entity config (rate + VLAN/PCP), by value
    ptpclient::MediaClockGenerator const& media_clock_;
    std::atomic<uint64_t> const& last_gptp_ns_;
    uint32_t samples_per_packet_;  ///< sample_rate/CLASS_A_PACKETS_PER_SEC: 12 @ 96k, 6 @ 48k
    std::pmr::memory_resource* mem_resource_;

    // Owned TX state.
    sg14::inplace_vector<TalkerStreamSlot, MAX_ENTITY_STREAMS> slots_{};
    /// Per-stream STOP_STREAMING latches (see set_stream_stopped).
    std::array<std::atomic<bool>, MAX_ENTITY_STREAMS> stream_stopped_{};
    net::RawnetContext stream_tx_{};
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
