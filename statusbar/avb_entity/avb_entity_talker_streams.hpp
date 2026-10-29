#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_talker_streams.hpp
/// @brief TalkerStreams — the local AVB stream TX path.
///
/// Borrows the StreamPort it transmits on and owns N per-stream TX slots
/// (each holding the kind-matching AM824/AAF/CRF serializer shaped by its
/// StreamSpec), the TX
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
#include "statusbar/avb_entity/avb_entity_slot_guard.hpp"
#include "statusbar/avb_entity/avb_entity_stream_port.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/tx_pcap_recorder.hpp"
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_am824_stream_output.hpp"
#include "statusbar/avtp/avtp_crf_stream_output.hpp"
#include "statusbar/ieee/ieee.hpp"
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
#include <system_error>

namespace statusbar::avb_entity {

/// The subset of an entity config that TalkerStreams needs, passed by value so any
/// talker entity (96 kHz or 48 kHz) can drive the shared TX path without depending on
/// a concrete entity config. Destructured at the call site (mirrors ListenerStreams).
struct TalkerStreamsConfig
{
    uint32_t sample_rate;  ///< the media clock's rate (Hz), e.g. 96000 or 48000: transmit_if_due's streams'
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
    /// The CRF mr bit sent (IEEE 1722-2016 Clause 10.4). Media thread.
    bool crf_mr{false};
    std::optional<avtp::Am824StreamOutputContext> am824{};
    std::optional<avtp::AafStreamOutputContext> aaf{};
    std::optional<avtp::CrfStreamOutputContext> crf{};
    std::optional<AafReframer> reframer{};
};

/// One STREAM_OUTPUT's transmitted AVTPDUs for GET_COUNTERS: how many, how many
/// with tu set, how many toggled mr. The media thread is the one writer (a
/// relaxed load and store, no read-modify-write); the reactor reads.
struct TalkerTxCounters
{
    std::atomic<uint32_t> pdus{0};
    std::atomic<uint32_t> tu_pdus{0};
    std::atomic<uint32_t> mr_toggles{0};
    bool last_mr{false};  ///< media thread only
    bool have_mr{false};  ///< media thread only
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
        , vlan_id_{config.vlan_id}
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

    /// Transmit on @p port from now on and (optionally) arm the TX pcap capture
    /// as its egress tap. @p port must outlive this object, or a later open_tx.
    /// Reactor thread, before the media thread transmits. Errors: none today.
    [[nodiscard]] auto open_tx(StreamPort& port, TxPcapConfig const& pcap) -> Status;
    [[nodiscard]] auto open_tx(StreamPort& port) -> Status { return open_tx(port, TxPcapConfig{}); }

    /// Hand this tick's committed frames to the link (StreamPort::tx_flush):
    /// once per media tick, after the tick's last transmit. Media thread.
    /// Errors: the port's; success before open_tx.
    [[nodiscard]] auto flush() noexcept -> Status { return port_ != nullptr ? port_->tx_flush() : Status{}; }

    /// TX pcap forwarding (see TxPcapRecorder): the capture window closed and
    /// the file awaits the non-RT flush.
    [[nodiscard]] auto tx_pcap_ready_to_write() const noexcept -> bool { return tx_pcap_recorder_.ready_to_write(); }
    [[nodiscard]] auto flush_tx_pcap() -> Status { return tx_pcap_recorder_.write_to_file(); }
    [[nodiscard]] auto tx_pcap_frame_count() const noexcept -> size_t { return tx_pcap_recorder_.frame_count(); }

    /// Add a TX slot shaped by @p spec, constructing the kind-matching
    /// serializer for @p stream_id. CRF timing (base frequency, interval,
    /// timestamps per PDU) comes from the spec's decoded format word — the
    /// blob is the single source. Errors: unknown/unsupported format kind or
    /// rate, a CRF format with no timestamps or more than
    /// MAX_CRF_TIMESTAMPS_PER_PDU, or more than MAX_ENTITY_STREAMS slots.
    auto open_stream(StreamSpec const& spec, tsn::StreamId stream_id, ieee::Eui48 dest_mac) -> Status;

    /// Re-shape the slot of @p spec.index for @p spec (new format, AAF or AM824, same stream id,
    /// destination, sequence and sample count). Reactor thread; the slot is locked (SlotGuard) so the
    /// media thread skips it. Errors (slot unchanged): no such slot, a kind change to or from CRF, or
    /// a format open_stream would refuse.
    auto reformat_stream(StreamSpec const& spec) -> Status;

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

    /// How long AVTP tu stays set after a grandmaster change (Milan v1.2 Section 4.3.5.2, Annex
    /// B.1.1); tu is also set while no grandmaster is known. Reactor thread writes, media thread
    /// reads; an entity that never reports gPTP state never sets tu.
    static constexpr int64_t TU_AFTER_GM_CHANGE_NS = 250'000'000;
    /// A grandmaster was found or changed at steady-clock time @p now_ns.
    void note_grandmaster_changed(int64_t const now_ns) noexcept
    {
        tu_until_ns_.store(now_ns + TU_AFTER_GM_CHANGE_NS, std::memory_order_relaxed);
        gptp_lost_.store(false, std::memory_order_relaxed);
    }
    /// No grandmaster is known (the Announces stopped).
    void note_grandmaster_lost() noexcept { gptp_lost_.store(true, std::memory_order_relaxed); }
    /// Whether a PDU stamped at steady-clock time @p now_ns carries tu = 1.
    [[nodiscard]] auto timestamp_uncertain(int64_t const now_ns) const noexcept -> bool
    {
        return gptp_lost_.load(std::memory_order_relaxed) || now_ns < tu_until_ns_.load(std::memory_order_relaxed);
    }

    /// A media clock restart on CRF STREAM_OUTPUT @p stream_index: mr toggles on its next PDU and stays
    /// (IEEE 1722-2016 Clause 10.4); requests before that PDU toggle it once. Any thread: two lock-free
    /// atomic accesses, no allocation. invalid_argument for no CRF slot of that index.
    [[nodiscard]] auto toggle_media_clock_restart(uint16_t stream_index) noexcept -> Status;

    /// VLAN ID the stream frames are tagged with: config.vlan_id until the
    /// Class A VID in use changes (adopted from the bridge's MSRP Domain,
    /// Milan v1.2 Section 4.2.7.2.1). Reactor thread writes, media thread reads.
    void set_vlan_id(uint16_t const vlan_id) noexcept { vlan_id_.store(vlan_id, std::memory_order_relaxed); }
    [[nodiscard]] auto vlan_id() const noexcept -> uint16_t { return vlan_id_.load(std::memory_order_relaxed); }

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
    /// Send one AAF packet of @p frames frames presented at @p pts_ns (no offset added), its payload raw wire bytes
    /// from @p render, due at @p launch_time_ns (StreamPort::tx_commit). Media-timer thread; true if sent. Nothing
    /// is sent, nor the sequence advanced, for a non-AAF slot, no port frame, or @p render false.
    auto transmit_aaf_raw(
        TalkerStreamSlot& slot,
        uint64_t first_index,
        uint64_t pts_ns,
        uint16_t frames,
        StreamRawRenderFn const& render,
        uint64_t launch_time_ns = 0) -> bool;
    /// Send one IEC 61883-6 AM824 packet of @p frames data blocks, the first at sample index @p first_index
    /// presented at @p pts_ns, its payload filled by @p render (see serialize_am824_raw), due at
    /// @p launch_time_ns. Media-timer thread; true if sent. As transmit_aaf_raw otherwise.
    auto transmit_am824_raw(
        TalkerStreamSlot& slot,
        uint64_t first_index,
        uint64_t pts_ns,
        uint16_t frames,
        StreamRawRenderFn const& render,
        uint64_t launch_time_ns = 0) -> bool;
    /// Commit @p frame, a prefix of the port's tx_begin window, on @p slot (VLAN-tagged, pcap-stamped,
    /// due at @p launch_time_ns) and count it; false, counting nothing, when the port refuses it (no
    /// port: only counted). Media-timer thread.
    auto send_stream_frame(TalkerStreamSlot& slot, std::span<uint8_t const> frame, uint64_t launch_time_ns = 0) -> bool;

    /// The transmit counters of STREAM_OUTPUT @p stream_index (nullptr past
    /// MAX_ENTITY_STREAMS). Any thread.
    [[nodiscard]] auto tx_counters(uint16_t const stream_index) const noexcept -> TalkerTxCounters const*
    {
        return stream_index < tx_counters_.size() ? &tx_counters_[stream_index] : nullptr;
    }

    /// One CRF PDU whose timestamps start at `base_index` in the media-clock
    /// sample-index domain (callers pass the LIVE position; see crf_aligned_base).
    /// Fills the timestamps from the media clock (fill_crf_timestamps) and sends
    /// them with transmit_crf_raw; nothing is sent before the media clock is anchored.
    void transmit_crf(TalkerStreamSlot& slot, uint64_t base_index);
    /// Send one CRF PDU on @p slot carrying exactly @p timestamps_ns (64-bit gPTP times, offset
    /// applied), due at @p launch_time_ns. Media-timer thread; true if sent. Nothing is sent for a
    /// non-CRF or stopped slot, no port frame, or a count other than timestamps_per_packet.
    auto transmit_crf_raw(TalkerStreamSlot& slot, std::span<uint64_t const> timestamps_ns, uint64_t launch_time_ns = 0) -> bool;

    /// Class A wire cadence is fixed at 8000 pkt/s/stream; samples-per-packet is then
    /// sample_rate/8000 (12 @ 96k, 6 @ 48k).
    static constexpr uint32_t CLASS_A_PACKETS_PER_SEC = avb_entity::CLASS_A_PACKETS_PER_SEC;

    /// The most timestamps one CRF PDU carries: open_stream refuses a CRF
    /// format declaring more (or none), so the TX paths size their buffers by it.
    static constexpr uint16_t MAX_CRF_TIMESTAMPS_PER_PDU = 64;

    // References / values (bound at construction).
    TalkerStreamsConfig config_;           ///< destructured entity config (rate + VLAN/PCP), by value
    std::atomic<uint16_t> vlan_id_;        ///< VLAN tag for stream frames (config_.vlan_id, or the adopted Domain VID)
    std::atomic<int64_t> tu_until_ns_{0};  ///< steady-clock end of the post-GM-change tu window
    std::atomic<bool> gptp_lost_{false};   ///< no grandmaster known: tu on every PDU
    ptpclient::MediaClockGenerator const& media_clock_;
    std::atomic<uint64_t> const& last_gptp_ns_;
    uint32_t samples_per_packet_;  ///< the media clock's per class A packet: 12 @ 96k, 6 @ 48k
    std::pmr::memory_resource* mem_resource_;

    /// Build @p slot's serializer (and AAF reframer) for @p spec, its stream
    /// @p stream_id: what open_stream and reformat_stream share.
    auto shape(TalkerStreamSlot& slot, StreamSpec const& spec, tsn::StreamId stream_id) -> Status;
    /// Re-shape @p slot for @p spec through a trial, swapped in under the slot's guard.
    auto reshape(TalkerStreamSlot& slot, StreamSpec const& spec) -> Status;
    /// Take @p trial's serializers into @p slot, carrying the stream's counts over. Under the guard.
    static void adopt_shape(TalkerStreamSlot& slot, TalkerStreamSlot&& trial, StreamSpec const& spec) noexcept;
    /// Send the frame @p serialize writes into a tx_begin window of @p max_frame octets (its length,
    /// 0: none), due at @p launch_time_ns, restoring @p ctx when the port refuses it. Media thread.
    template <typename Context, typename Serialize>
    auto send_serialized(
        TalkerStreamSlot& slot, Context& ctx, size_t max_frame, uint64_t launch_time_ns, Serialize const& serialize) -> bool;

    // Owned TX state.
    sg14::inplace_vector<TalkerStreamSlot, MAX_ENTITY_STREAMS> slots_{};
    /// Per-stream guards between the media thread's transmits and the
    /// reactor's reformat_stream, by STREAM_OUTPUT index.
    std::array<SlotGuard, MAX_ENTITY_STREAMS> guards_{};
    /// Per-stream STOP_STREAMING latches (see set_stream_stopped).
    std::array<std::atomic<bool>, MAX_ENTITY_STREAMS> stream_stopped_{};
    /// Per-stream transmit counters, by STREAM_OUTPUT index (see tx_counters).
    std::array<TalkerTxCounters, MAX_ENTITY_STREAMS> tx_counters_{};
    /// Per-stream media clock restart pending (toggle_media_clock_restart), by STREAM_OUTPUT index.
    std::array<std::atomic<bool>, MAX_ENTITY_STREAMS> mr_pending_{};
    /// A CRF slot is open at this STREAM_OUTPUT index (open_stream), for toggle_media_clock_restart.
    std::array<std::atomic<bool>, MAX_ENTITY_STREAMS> crf_open_{};
    static_assert(std::atomic<bool>::is_always_lock_free);
    StreamPort* port_{nullptr};  ///< the link (open_tx); borrowed
    /// TX stream capture (diagnostic). last_tx_gptp_ns_ is set just before each
    /// send so the port egress tap can stamp the captured frame with the gPTP TX time.
    TxPcapRecorder tx_pcap_recorder_{};
    uint64_t last_tx_gptp_ns_{0};
};

// ---------------------------------------------------------------------------
// Raw AAF serialization (pure, socket-free -- unit-testable in isolation).
// ---------------------------------------------------------------------------

/// Build one AAF frame into @p frame for @p frames frames, AVTP timestamp the low 32 bits of
/// @p pts_ns (no offset added), payload filled by @p render. Returns the length (sequence advanced),
/// or 0 with @p ctx untouched for a format with no byte layout, a short @p frame or @p render false.
[[nodiscard]] auto serialize_aaf_raw(
    avtp::AafStreamOutputContext& ctx,
    std::span<uint8_t> frame,
    uint16_t frames,
    uint64_t first_index,
    uint64_t pts_ns,
    StreamRawRenderFn const& render,
    bool timestamp_uncertain = false) -> size_t;

/// Build one IEC 61883-6 AM824 frame of @p frames blocks from sample index @p first_index (the DBC, mod 256)
/// at @p pts_ns (no offset added); @p render writes frames x channels big-endian quadlets. Returns the
/// length (sequence advanced), or 0 with @p ctx untouched for > 32 blocks, a short @p frame or @p render false.
[[nodiscard]] auto serialize_am824_raw(
    avtp::Am824StreamOutputContext& ctx,
    std::span<uint8_t> frame,
    uint16_t frames,
    uint64_t first_index,
    uint64_t pts_ns,
    StreamRawRenderFn const& render,
    bool timestamp_uncertain = false) -> size_t;

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

/// Fill @p timestamps with @p media_clock's presentation times (offset included) for sample indices
/// from @p base_index spaced by @p sample_stride. Pure.
void fill_crf_timestamps(
    ptpclient::MediaClockGenerator const& media_clock,
    uint64_t base_index,
    uint32_t sample_stride,
    std::span<uint64_t> timestamps) noexcept;

/// Build one CRF frame into @p frame carrying @p timestamps verbatim with @p ctx's stream id, type
/// and sequence, and the tu and mr bits given. Returns the length (counters advanced), or 0 with @p ctx
/// untouched for a count other than ctx.timestamps_per_packet (or zero) or a short @p frame. Pure.
[[nodiscard]] auto serialize_crf_raw(
    avtp::CrfStreamOutputContext& ctx,
    std::span<uint8_t> frame,
    std::span<uint64_t const> timestamps,
    bool timestamp_uncertain = false,
    bool media_clock_restart = false) -> size_t;

}  // namespace statusbar::avb_entity
