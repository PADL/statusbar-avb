#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_talker_streams.hpp
/// @brief TalkerStreams — the local AVB stream TX path.
///
/// Sends on the entity's StreamPort, owns N per-stream TX slots (each holding the
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
#include "statusbar/avb_entity/avb_entity_stream_port.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/tx_pcap_recorder.hpp"
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_am824_stream_output.hpp"
#include "statusbar/avtp/avtp_crf_stream_output.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/ieee/ieee_protocols.hpp"
#include "statusbar/itc/itc_seqlock_value.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/realtime/realtime_clock.hpp"
#include "statusbar/sg14/inplace_function.h"
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
    /// AAF: sample frames per AVTPDU, the block the reframer emits and the
    /// count every frame carries (aaf_samples_per_pdu of the spec).
    uint16_t aaf_samples{0};
    /// CRF: the media-clock sample index the next PDU's first timestamp is
    /// owed at, or nothing while none is scheduled (gate closed, or not yet
    /// opened). PDUs are spaced timestamps_per_packet x stride samples apart
    /// in the media clock, whatever the wake cadence (crf_schedule).
    std::optional<uint64_t> crf_due{};
    std::optional<avtp::Am824StreamOutputContext> am824{};
    std::optional<avtp::AafStreamOutputContext> aaf{};
    std::optional<avtp::CrfStreamOutputContext> crf{};
    std::optional<AafReframer> reframer{};
    /// The mr bit the stream sends (IEEE 1722-2016 Clauses 4.4.4.3, 10.4.3). Media thread.
    bool mr{false};
    /// AVTPDUs still to send before mr may toggle again (TalkerStreams::MR_HOLD_PDUS). Media thread.
    uint8_t mr_hold{0};
    /// The media clock restart requests mr has followed (toggle_media_clock_restart). Media thread.
    uint32_t mr_requests_seen{0};
    /// The media clock's re-anchors mr has followed (MediaClockGenerator::step_count), from the
    /// first AVTPDU on. Media thread.
    std::optional<uint64_t> media_clock_steps{};
};

/// What one STREAM_OUTPUT has sent, for GET_COUNTERS: its AVTPDUs, those with
/// tu set, the mr toggles among them, and each start and stop of the stream.
/// The kit's sender (the media thread) is the one writer, a relaxed load and
/// store, no read-modify-write; the reactor thread reads.
struct TalkerTxCounters
{
    std::atomic<uint32_t> pdus{0};
    std::atomic<uint32_t> tu_pdus{0};
    std::atomic<uint32_t> mr_toggles{0};
    /// Each start and each stop, in turn: odd while the stream streams. One
    /// count, so a reader sees STREAM_START and STREAM_STOP of one instant.
    std::atomic<uint32_t> streaming_changes{0};
    bool last_mr{false};  ///< writer only
    bool have_mr{false};  ///< writer only
};

/// STREAM_START of @p streaming_changes (TalkerTxCounters): one per start.
[[nodiscard]] constexpr auto stream_starts(uint32_t const streaming_changes) noexcept -> uint32_t
{
    return (streaming_changes / 2U) + (streaming_changes % 2U);
}
/// STREAM_STOP of @p streaming_changes: one per stop.
[[nodiscard]] constexpr auto stream_stops(uint32_t const streaming_changes) noexcept -> uint32_t
{
    return streaming_changes / 2U;
}

/// The bytes every frame of one talker stream starts with, its 802.1Q-tagged Ethernet header and AVTPDU header, with
/// sequence_num, avtp_timestamp, mr and tu zero, and where those four go: write() makes the headers of one frame.
struct StreamFrameTemplate
{
    /// The most header bytes: a tagged AAF frame's.
    static constexpr size_t CAPACITY = ieee::protocols::ETHERNET_HEADER_SIZE + ieee::VlanTag::LENGTH + avtp::AafPdu::HEADER_LENGTH;
    /// mr's bit in its octet, and tu's in its (IEEE 1722-2016 Clauses 4.4.3, 7.2, 10.4).
    static constexpr uint8_t MR_BIT = 0x08U;
    static constexpr uint8_t TU_BIT = 0x01U;

    std::array<uint8_t, CAPACITY> bytes{};
    size_t length{0};            ///< header bytes before the payload: 42 for AAF, 38 for CRF
    size_t sequence_offset{0};   ///< sequence_num's octet
    size_t timestamp_offset{0};  ///< avtp_timestamp's four octets, big-endian; 0 for CRF, whose timestamps are its payload
    size_t mr_offset{0};         ///< the octet whose MR_BIT is mr
    size_t tu_offset{0};         ///< the octet whose TU_BIT is tu

    /// The header bytes, `length` of them.
    [[nodiscard]] auto header() const noexcept -> std::span<uint8_t const> { return std::span{bytes}.first(length); }
    /// The headers of the frame with @p sequence, @p timestamp (ignored for CRF), @p mr and @p tu to the start of
    /// @p out; false, writing nothing, when @p out is shorter than `length`.
    [[nodiscard]] auto write(uint8_t sequence, uint32_t timestamp, bool mr, bool tu, std::span<uint8_t> out) const noexcept -> bool;
};

struct TalkerStreams
{
    TalkerStreams(
        TalkerStreamsConfig const& config,
        ptpclient::MediaClockGenerator const& media_clock,
        std::atomic<int64_t> const& last_gptp_ns,
        std::pmr::memory_resource* memory_resource) noexcept
        : config_{config}
        , media_clock_{media_clock}
        , last_gptp_ns_{last_gptp_ns}
        , mem_resource_{memory_resource}
    {}

    /// Tag the stream frames with @p vlan_id and priority @p pcp from now on (the SR class Domain adopted from the
    /// wire), told to the frames hook. Reactor thread, as it reads the slots; the media thread reads the tag per frame.
    void set_vlan(uint16_t vlan_id, uint8_t pcp) noexcept;
    [[nodiscard]] auto vlan_id() const noexcept -> uint16_t { return vlan_id_.load(std::memory_order_relaxed); }
    [[nodiscard]] auto stream_pcp() const noexcept -> uint8_t { return stream_pcp_.load(std::memory_order_relaxed); }

    //
    // AVTP mr - IEEE 1722-2016 Clauses 4.4.4.3, 10.4.3
    //

    /// The AVTPDUs a toggled mr keeps its new state for, at least (IEEE 1722-2016 Clause 4.4.4.3).
    static constexpr uint8_t MR_HOLD_PDUS = 8;
    /// The most channels of a STREAM_OUTPUT whose signal presence GET_COUNTERS reports: 32 in
    /// ENTITY_SPECIFIC_9, 28 in ENTITY_SPECIFIC_10 (Milan v1.3 Section 5.3.7.7.1).
    static constexpr size_t SIGNAL_PRESENCE_CHANNELS = 60;
    /// Report the signal presence of STREAM_OUTPUT @p stream_index, bit k for channel k: bits past
    /// SIGNAL_PRESENCE_CHANNELS are dropped here, bits past the stream's channels by the kit. Until
    /// the first report the output reports none. Any one thread per output, a real-time one too: a
    /// store, no clock, no lock. invalid_argument past MAX_ENTITY_STREAMS.
    [[nodiscard]] auto set_signal_presence(uint16_t stream_index, uint64_t channels) noexcept -> Status;
    /// The signal presence last reported for STREAM_OUTPUT @p stream_index, nullopt before the first
    /// report or past MAX_ENTITY_STREAMS. Any thread.
    [[nodiscard]] auto signal_presence(uint16_t stream_index) const noexcept -> std::optional<uint64_t>;

    /// A media clock restart on open STREAM_OUTPUT @p stream_index: mr toggles on its next AVTPDU,
    /// or the first after MR_HOLD_PDUS since the last toggle, and keeps its new state; requests
    /// before then toggle it once. invalid_argument for a stream not open.
    ///
    /// Any thread, and several at once, a real-time one too: one atomic load and one atomic add,
    /// no lock, no clock, no system call. So a data plane that re-steps a media clock on its own
    /// thread asks here at the step, and mr goes out with the first AVTPDU after it. A request
    /// that meets the stream's close is refused or counted for the run that ends; the next run
    /// starts clear either way (open_stream()).
    [[nodiscard]] auto toggle_media_clock_restart(uint16_t stream_index) noexcept -> Status;
    /// The restarts asked of STREAM_OUTPUT @p stream_index since this object was made, counted
    /// over all its runs; 0 past MAX_ENTITY_STREAMS. Any thread.
    [[nodiscard]] auto media_clock_restart_requests(uint16_t stream_index) const noexcept -> uint32_t;
    /// The mr bit @p slot's next AVTPDU carries, toggled when a restart is owed (a request, or the
    /// media clock re-anchored on a stepped gPTP timeline) and the hold is over. Media thread; call
    /// once per AVTPDU sent (count_sent runs the hold down).
    [[nodiscard]] auto next_mr(TalkerStreamSlot& slot) noexcept -> bool;

    //
    // AVTP tu - IEEE 1722-2016 Clause 4.4.4.7, Milan v1.3 Section 4.3.5.2
    //

    /// How long every AVTPDU carries tu after a change of grandmaster (Milan v1.3 Annex B.1.1).
    static constexpr int64_t TU_AFTER_GM_CHANGE_NS = 250'000'000;
    /// The clock tu is timed on, CLOCK_MONOTONIC_RAW: the one a media wake is told on, and not
    /// gPTP time, which may step at the change. The talker reads no clock: each instant is told
    /// it, the reactor cycle's by notify_grandmaster_changed() and the media wake's by
    /// tell_wake_time().
    using RawTimePoint = realtime::MonotonicRawClock::time_point;
    /// When the AVTPDUs carry tu: while no grandmaster is in force, and before RAW until_ns.
    struct TimestampUncertainty
    {
        bool grandmaster_lost{false};
        int64_t until_ns{0};
    };
    /// A grandmaster came into force at @p now_ns, another or the first after none: tu for
    /// TU_AFTER_GM_CHANGE_NS. @p now_ns is nanoseconds on the clock tu is timed on, the one the
    /// wakes are told on; 0 is an instant never told and opens no window. Reactor thread, the
    /// one writer.
    void notify_grandmaster_changed(int64_t const now_ns) noexcept
    {
        uncertainty_.store({.grandmaster_lost = false, .until_ns = now_ns == 0 ? 0 : now_ns + TU_AFTER_GM_CHANGE_NS});
    }
    /// The same, told the instant as a RawTimePoint.
    void notify_grandmaster_changed(RawTimePoint const now) noexcept
    {
        notify_grandmaster_changed(RawTimePoint::clock::to_ns(now));
    }
    /// No grandmaster is in force: tu until one is. Reactor thread, the one writer.
    void notify_grandmaster_lost() noexcept { uncertainty_.store({.grandmaster_lost = true}); }
    /// Whether an AVTPDU sent at @p now_ns, on the same clock, carries tu: never before the first
    /// notify_grandmaster_*, and 0, an instant never told, is in no window, so a talker that is
    /// told no time sends tu only while no grandmaster is in force. Any thread; lock-free
    /// (itc::SeqlockValue), so the media thread too.
    [[nodiscard]] auto timestamp_uncertain(int64_t const now_ns) const noexcept -> bool
    {
        auto const uncertainty = uncertainty_.load();
        return uncertainty.grandmaster_lost || (now_ns != 0 && now_ns < uncertainty.until_ns);
    }
    /// The same, asked with a RawTimePoint.
    [[nodiscard]] auto timestamp_uncertain(RawTimePoint const now) const noexcept -> bool
    {
        return timestamp_uncertain(RawTimePoint::clock::to_ns(now));
    }
    /// Media thread: the RAW instant of the media wake now being served, as the wake was told
    /// it. Every AVTPDU sent in this wake takes its tu at that instant (wake_timestamp_uncertain()).
    /// The kit tells it from talker_should_transmit(), which an entity calls with the wake's
    /// instant before each transmit_if_due(); an entity that drives a TalkerStreams itself and
    /// tells it of grandmaster changes calls this once per wake.
    void tell_wake_time(RawTimePoint const now) noexcept { wake_now_ = now; }
    /// The tu of an AVTPDU sent in the wake last told (tell_wake_time). Media thread.
    [[nodiscard]] auto wake_timestamp_uncertain() const noexcept -> bool { return timestamp_uncertain(wake_now_); }

    /// TX pcap capture knobs for open_tx (see TxPcapRecorder). Empty path =
    /// capture disabled.
    struct TxPcapConfig
    {
        std::string_view path{};
        size_t max_bytes{0};
        uint32_t seconds{0};
    };

    /// Send through @p port - the entity's stream port, already open - and
    /// (optionally) arm the TX pcap capture. One call from the owning
    /// entity's start(). The port outlives this class; the recorder is this
    /// class's state.
    auto open_tx(StreamPort& port, TxPcapConfig const& pcap) -> Status;
    auto open_tx(StreamPort& port) -> Status { return open_tx(port, TxPcapConfig{}); }

    /// The reverse of open_stream and open_tx: no slots, no port, the kick
    /// back in flush_tx, every stream streaming again. One call from the
    /// owning entity's stop(), once the media timer that transmits has
    /// stopped; the entity's next start() opens the streams afresh, as a
    /// first start does, instead of over the ones a previous run left. A
    /// stream that was streaming counts a stop (TalkerTxCounters).
    void close_tx() noexcept;

    /// Push this tick's frames to the wire. The ring backends batch what
    /// transmit_* committed until this kick; rawnet sent each frame as it
    /// was committed and this is free. Once per media tick, after the slots.
    void flush_tx();

    /// Hand the TX kick to another thread (see kick_tx): flush_tx then only
    /// closes the cycle. Returns false, and leaves the kick in flush_tx, when
    /// there is no port to kick.
    auto set_external_kick(bool enable) noexcept -> bool;
    [[nodiscard]] auto external_kick() const noexcept -> bool { return external_kick_; }

    /// The TX kick alone, from the kicker thread: the wake-up syscall over the
    /// ring the media thread fills. Taken without the port guard on purpose:
    /// the mmap kick touches no user-space state and the xdp kick only its
    /// atomic pending flag (the kernel's ring protocol is built for a filler
    /// and a kicker), and holding the guard would make the media thread spin
    /// for the length of the syscall it was spared.
    void kick_tx();

    /// TX pcap forwarding (see TxPcapRecorder): the capture window closed and
    /// the file awaits the non-RT flush.
    [[nodiscard]] auto tx_pcap_ready_to_write() const noexcept -> bool { return tx_pcap_recorder_.ready_to_write(); }
    [[nodiscard]] auto flush_tx_pcap() -> Status { return tx_pcap_recorder_.write_to_file(); }
    [[nodiscard]] auto tx_pcap_frame_count() const noexcept -> size_t { return tx_pcap_recorder_.frame_count(); }

    /// Add a TX slot shaped by @p spec, constructing the kind-matching
    /// serializer for @p stream_id. CRF timing (base frequency, interval,
    /// timestamps per PDU) comes from the spec's decoded format word — the
    /// blob is the single source. Errors: unknown/unsupported format kind or
    /// rate, a CRF format with no timestamp per AVTPDU or more than
    /// MAX_CRF_TIMESTAMPS_PER_PDU, or more than MAX_ENTITY_STREAMS slots.
    auto open_stream(StreamSpec const& spec, tsn::StreamId stream_id, ieee::Eui48 dest_mac) -> Status;

    /// The most timestamps one CRF AVTPDU of this talker carries: the frame
    /// transmit_crf() builds has room for these and no more. A stream format
    /// can ask for up to 255.
    static constexpr uint16_t MAX_CRF_TIMESTAMPS_PER_PDU = 64;

    /// The slot whose spec.index == @p stream_index (the STREAM_OUTPUT
    /// descriptor index == ACMP talker unique id), or nullptr.
    [[nodiscard]] auto slot_for(uint16_t stream_index) noexcept -> TalkerStreamSlot*;
    [[nodiscard]] auto slot_for(uint16_t stream_index) const noexcept -> TalkerStreamSlot const*;
    /// The first slot of @p kind, or nullptr.
    [[nodiscard]] auto slot_of(StreamKind kind) noexcept -> TalkerStreamSlot*;
    [[nodiscard]] auto slot_of(StreamKind kind) const noexcept -> TalkerStreamSlot const*;

    /// The transmit counters of STREAM_OUTPUT @p stream_index, kept across
    /// close_tx() and open_stream(); nullptr past MAX_ENTITY_STREAMS. Any thread.
    [[nodiscard]] auto tx_counters(uint16_t const stream_index) const noexcept -> TalkerTxCounters const*
    {
        return stream_index < tx_counters_.size() ? &tx_counters_[stream_index] : nullptr;
    }

    /// STREAM_OUTPUT @p stream_index's frames as this talker sends them now, from @p src_mac: its destination, the
    /// VID and PCP in force, its stream ID and format. nullopt for no AAF or CRF stream there, or a frame the talker
    /// would not send. It describes the frames and starts none: a data plane sends a stream only while its gate is open
    /// (AvbEntityKit::talker_should_transmit()). Reactor thread.
    [[nodiscard]] auto frame_template(uint16_t stream_index, ieee::Eui48 const& src_mac) const noexcept
        -> std::optional<StreamFrameTemplate>;
    /// Called with an open stream's index at each change of its frame_template(): a VID or PCP set_vlan() moves (every
    /// open stream), a destination set_destination() moves. The thread that made the change calls it, the kit's
    /// reactor; not at open_stream(). A later install replaces an earlier one; it must not throw.
    using FramesChangedFn = sg14::inplace_function<void(uint16_t stream_index), 32>;
    void set_on_frames_changed(FramesChangedFn fn) noexcept { on_frames_changed_ = std::move(fn); }
    /// STREAM_OUTPUT @p stream_index's frames go to @p dest_mac from now on (a MAAP block), told to the frames hook
    /// when that moves them. Reactor thread; the media thread reads it per frame, as it always has.
    void set_destination(uint16_t stream_index, ieee::Eui48 const& dest_mac) noexcept;

    /// Emit this tick's traffic for @p slot from @p src (this stream's
    /// interleaved audio for the tick — per-stream sources are the entity's
    /// business; CRF slots ignore it): AM824 sends the tick's samples
    /// directly; AAF reframes to constant blocks of the format's samples per
    /// AVTPDU (gate closed clears the FIFO); CRF emits a PDU whenever the
    /// media clock reaches the one it owes (gate closed forgets the schedule,
    /// so the next PDU starts from the live position). The stream streams
    /// while @p gate_open and not stopped: a change counts a start or a stop.
    /// Media-timer (SCHED_FIFO) thread.
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

    /// The media clock's presentation time for @p sample_index, typed as the
    /// gPTP instant it is: what a packet whose first sample is that index
    /// carries as its avtp_timestamp.
    [[nodiscard]] auto presentation_for(uint64_t const sample_index) const noexcept -> realtime::GptpClock0::time_point
    {
        return realtime::GptpClock0::from_ns(static_cast<int64_t>(media_clock_.timestamp_for(sample_index)));
    }

    /// Serialize + send one packet on a specific slot from @p src (interleaved,
    /// slot.spec.format.channels stride), its avtp_timestamp @p presentation
    /// (presentation_for the packet's first sample). Media-timer thread.
    /// (Public for entities that drive their own cadence, e.g. the pipe-fed
    /// AM824 loopback entities.)
    void transmit_am824(
        TalkerStreamSlot& slot, realtime::GptpClock0::time_point presentation, uint32_t samples, std::span<float const> src);
    void transmit_aaf(
        TalkerStreamSlot& slot, realtime::GptpClock0::time_point presentation, uint16_t samples, std::span<float const> src);
    /// One CRF PDU whose timestamps start at `base_index` in the media-clock
    /// sample-index domain (callers pass the LIVE position; see crf_aligned_base).
    void transmit_crf(TalkerStreamSlot& slot, uint64_t base_index);

    /// One AVTPDU of @p slot sent, its tu and mr as given: tx_packets and the
    /// stream's TalkerTxCounters. The sender's thread.
    void count_sent(TalkerStreamSlot& slot, bool tu, bool mr) noexcept;
    /// STREAM_OUTPUT @p stream_index streams (@p on) or not from this tick:
    /// a change counts a start or a stop. The sender's thread.
    void follow_streaming(uint16_t stream_index, bool on) noexcept;

    //
    // A talker stream sent beside the stream port (docs/ENTITY_CONSTRUCTION_KIT.md)
    //

    /// The varying header fields a data plane beside the port gives the AVTPDUs it sends of a stream from now on.
    struct OffloadHeader
    {
        uint8_t sequence_num{0};  ///< the next AVTPDU's
        bool mr{false};
        bool tu{false};
    };
    /// What such a data plane sent of a stream since it last said.
    struct OffloadSent
    {
        uint32_t pdus{0};          ///< AVTPDUs sent
        uint32_t tu_pdus{0};       ///< of them, those with tu set; counted at most pdus
        uint8_t next_sequence{0};  ///< the sequence_num of the next it sends; read when pdus is not 0
        /// The mr of the last AVTPDU it sent, when it knows it: a toggle is then counted, and its hold run down, only
        /// once the wire carries it. Without it every AVTPDU reported counts with the mr last answered.
        std::optional<bool> last_mr{};
    };
    /// The header @p slot's next AVTPDU carries as it starts sending: its sequence, a pending restart applied as for
    /// an AVTPDU the kit sends (next_mr()), tu at the media wake last told (wake_timestamp_uncertain()). The data plane
    /// is told the wake as the port's sender is: through AvbEntityKit::talker_should_transmit(), or tell_wake_time()
    /// if it does not ask the gate. Media thread, in transmit_if_due()'s place for a stream a data plane beside the
    /// port sends, so the slot's mr, hold and counters keep their one writer.
    [[nodiscard]] auto offload_header(TalkerStreamSlot& slot) noexcept -> OffloadHeader;
    /// transmit_if_due() for a stream a data plane beside the port sends: @p sent counted as the kit's own AVTPDUs,
    /// a start or a stop as @p gate_open and STOP_STREAMING have it; then offload_header() while the stream streams,
    /// else the header with mr as it stands, as no AVTPDU goes. Media thread, in transmit_if_due()'s place, the wake
    /// told first as for offload_header().
    [[nodiscard]] auto notify_offload_sent(TalkerStreamSlot& slot, bool gate_open, OffloadSent const& sent) noexcept
        -> OffloadHeader;
    /// @p sent counted on @p slot and its stream's TalkerTxCounters, its mr hold run down, its sequence moved on.
    void count_offload_sent(TalkerStreamSlot& slot, OffloadSent const& sent) noexcept;
    /// @p slot's header with mr @p mr: its next sequence_num, tu at the wake last told.
    [[nodiscard]] auto offload_header_for(TalkerStreamSlot const& slot, bool mr) const noexcept -> OffloadHeader;
    /// @p slot's AAF reframer and CRF schedule emptied, as after a closed gate. Media thread.
    static void empty_own_schedule(TalkerStreamSlot& slot) noexcept;

    /// The AAF and CRF halves of transmit_if_due.
    void serve_aaf(
        TalkerStreamSlot& slot,
        ptpclient::MediaClockGenerator::Emit const& tick,
        bool gate_open,
        size_t samples,
        std::span<float const> src);
    void serve_crf(TalkerStreamSlot& slot, ptpclient::MediaClockGenerator::Emit const& tick, bool gate_open, size_t samples);

    // References / values (bound at construction).
    TalkerStreamsConfig config_;  ///< destructured entity config (rate + VLAN/PCP), by value
    /// The VLAN tag every stream frame carries: the configured one until
    /// set_vlan() moves it (a Domain adopted from the wire, Milan 4.2.7.2.1).
    /// Atomics: written from the control thread, read per frame on the media thread.
    std::atomic<uint16_t> vlan_id_{config_.vlan_id};
    std::atomic<uint8_t> stream_pcp_{config_.stream_pcp};
    FramesChangedFn on_frames_changed_{};
    /// When the AVTPDUs carry tu (notify_grandmaster_*): the reactor writes, the media thread reads.
    itc::SeqlockValue<TimestampUncertainty> uncertainty_{};
    /// The media wake now being served (tell_wake_time). Media thread only.
    RawTimePoint wake_now_{};
    ptpclient::MediaClockGenerator const& media_clock_;
    std::atomic<int64_t> const& last_gptp_ns_;
    std::pmr::memory_resource* mem_resource_;

    // Owned TX state.
    sg14::inplace_vector<TalkerStreamSlot, MAX_ENTITY_STREAMS> slots_{};
    /// Per-stream STOP_STREAMING latches (see set_stream_stopped).
    std::array<std::atomic<bool>, MAX_ENTITY_STREAMS> stream_stopped_{};
    /// Per-stream transmit counters, by STREAM_OUTPUT index (see tx_counters).
    std::array<TalkerTxCounters, MAX_ENTITY_STREAMS> tx_counters_{};
    /// Per-stream signal presence (set_signal_presence), SIGNAL_PRESENCE_REPORTED set once reported.
    std::array<std::atomic<uint64_t>, MAX_ENTITY_STREAMS> signal_presence_{};
    static constexpr uint64_t SIGNAL_PRESENCE_REPORTED = uint64_t{1} << 63U;
    /// Per-stream media clock restart requests, counted by whichever thread asks (toggle_media_clock_restart).
    std::array<std::atomic<uint32_t>, MAX_ENTITY_STREAMS> mr_requests_{};
    /// Whether STREAM_OUTPUT index is open: what toggle_media_clock_restart() may read on any
    /// thread, where slots_ is the opening thread's. Set by open_stream(), cleared by close_tx().
    std::array<std::atomic<bool>, MAX_ENTITY_STREAMS> stream_open_{};
    /// STREAM_OUTPUT @p stream_index is open, or no longer; nothing past MAX_ENTITY_STREAMS.
    void mark_open(uint16_t stream_index, bool open) noexcept;
    /// @p slot, opened, joins the slots: the restart requests so far are not its own, and it is
    /// marked open once it is among them.
    void keep_opened(TalkerStreamSlot slot);
    /// The requests counted for STREAM_OUTPUT @p stream_index, 0 past MAX_ENTITY_STREAMS.
    [[nodiscard]] auto mr_requests(uint16_t stream_index) const noexcept -> uint32_t;
    /// Whether @p slot owes a toggle of mr: a request or a re-anchor it has not followed. Media thread.
    [[nodiscard]] auto restart_owed(TalkerStreamSlot const& slot) const noexcept -> bool;
    /// One AVTP stream frame to @p dest_mac: VLAN tag from config_, source
    /// the port's address, recorded to the pcap tap when that is armed.
    void send_stream_frame(ieee::Eui48 const& dest_mac, std::span<uint8_t const> payload);
    /// Record @p length bytes of @p slot to the pcap tap when armed, then commit. Under the guard.
    void commit_stream_frame(net::EthernetTxSlot const& slot, size_t length);

    StreamPort* port_{nullptr};
    /// The TX kick lives on another thread (set_external_kick).
    bool external_kick_{false};
    /// TX stream capture (diagnostic). last_tx_gptp_ns_ is set just before each
    /// send so the captured frame carries the gPTP TX time.
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

/// The media-clock index a CRF schedule starts at: the first sample_stride
/// boundary at or after the live position. The first PDU is then sent by the
/// wake that covers its index, as every later one is, and leaves as far ahead
/// of its timestamp as they do. The boundary before the live position
/// (crf_aligned_base) is already up to a stride behind: a PDU sent for it
/// goes out that much after the moment it was owed, and with a presentation
/// offset shorter than the stride a listener receives it past its timestamp
/// (LATE_TIMESTAMP, Milan v1.3 Table 5.6).
[[nodiscard]] constexpr auto crf_start_base(uint64_t const live, uint32_t const sample_stride) noexcept -> uint64_t
{
    uint64_t const before = crf_aligned_base(live, sample_stride);
    return before == live ? live : before + sample_stride;
}

/// What crf_schedule decided for one wake: the base index of the PDU to send
/// now, if one is owed, and the schedule to carry to the next wake.
struct CrfSchedule
{
    std::optional<uint64_t> send{};
    std::optional<uint64_t> due{};
};

/// Whether this wake owes a CRF PDU. A PDU covers @p span media-clock samples
/// (timestamps_per_packet x stride), so PDUs are owed at every multiple of
/// span from the first one, however many wakes that takes: the count per
/// second is the sample rate over span (459.375 at 44.1 kHz for a span of
/// 96), not a count of wakes. @p due is the index the next PDU is owed at,
/// nullopt when nothing is scheduled; the wake covers [@p live, @p live +
/// @p samples). Nothing scheduled, or a due index more than a span away from
/// the live position (the gate just opened, the media clock stepped), starts
/// again at the first stride boundary at or after the live position
/// (crf_start_base): no PDU is sent for an index a wake before this one
/// covered.
[[nodiscard]] constexpr auto crf_schedule(
    std::optional<uint64_t> due, uint64_t const live, uint32_t const samples, uint32_t const stride, uint64_t const span) noexcept
    -> CrfSchedule
{
    if (!due || *due + span <= live || live + span < *due) {
        due = crf_start_base(live, stride);
    }
    if (span == 0 || *due >= live + samples) {
        return {.send = std::nullopt, .due = due};
    }
    return {.send = *due, .due = *due + span};
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

//
// Stream frame templates (pure, socket-free)
//

/// The template of an AAF stream's frames of @p frames frames behind @p header (stream_frame_header()): the AVTPDU
/// header transmit_aaf() sends for @p ctx's stream, format and channels. nullopt for a format with no byte layout,
/// no frames, or a frame past an Ethernet payload.
[[nodiscard]] auto aaf_frame_template(
    avtp::AafStreamOutputContext const& ctx, uint16_t frames, ieee::EthernetFrame const& header) noexcept
    -> std::optional<StreamFrameTemplate>;
/// The template of a CRF stream's frames behind @p header: the AVTPDU header transmit_crf() sends for @p ctx's
/// stream, its timestamps the payload. nullopt for none, or more than MAX_CRF_TIMESTAMPS_PER_PDU.
[[nodiscard]] auto crf_frame_template(avtp::CrfStreamOutputContext const& ctx, ieee::EthernetFrame const& header) noexcept
    -> std::optional<StreamFrameTemplate>;

}  // namespace statusbar::avb_entity
