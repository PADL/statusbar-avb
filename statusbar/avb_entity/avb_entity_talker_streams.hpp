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
#include "statusbar/avb_entity/avb_entity_stream_port.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/tx_pcap_recorder.hpp"
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_am824_stream_output.hpp"
#include "statusbar/avtp/avtp_crf_stream_output.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_spin_lock.hpp"
#include "statusbar/net/net_rawnet.hpp"
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
    /// AAF: sample frames per AVTPDU, the block the reframer emits and the
    /// count every frame carries (aaf_samples_per_pdu of the spec).
    uint16_t aaf_samples{0};
    /// CRF: the media-clock sample index the next PDU's first timestamp is
    /// owed at, or nothing while none is scheduled (gate closed, or not yet
    /// opened). PDUs are spaced timestamps_per_packet x stride samples apart
    /// in the media clock, whatever the wake cadence (crf_schedule).
    std::optional<uint64_t> crf_due{};
    /// The mr bit the stream sends (IEEE 1722-2016 Clauses 4.4.4.3, 10.4.3). Media thread.
    bool mr{false};
    /// PDUs still to send with mr as it is before a toggle may change it again. Media thread.
    uint8_t mr_hold{0};
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
    /// before then toggle it once. invalid_argument for a stream not open. Reactor thread, the one writer.
    [[nodiscard]] auto toggle_media_clock_restart(uint16_t stream_index) noexcept -> Status;
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

    /// Push this tick's frames to the wire (the ring backends batch until this kick; rawnet sent
    /// each as committed). Once per media tick, after the slots. Errors: the kick's, else the
    /// port cycle's.
    [[nodiscard]] auto flush_tx() -> Status;

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
    /// for the length of the syscall it was spared. Errors: the kick's.
    [[nodiscard]] auto kick_tx() -> Status;

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
    [[nodiscard]] auto open_stream(StreamSpec const& spec, tsn::StreamId stream_id, ieee::Eui48 dest_mac) -> Status;

    /// Re-shape @p spec.index's slot for @p spec, its stream id, destination, sequence and sample count
    /// kept. Reactor thread, under the slot's lock. Errors (slot unchanged): no such slot, a change to or
    /// from CRF, a format open_stream() refuses.
    [[nodiscard]] auto reformat_stream(StreamSpec const& spec) -> Status;

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

    /// How long AVTP tu stays set after a grandmaster change (Milan v1.2 Section 4.3.5.2, Annex
    /// B.1.1); tu is also set while no grandmaster is known. Reactor thread writes, media thread
    /// reads; an entity that never reports gPTP state never sets tu.
    static constexpr int64_t TU_AFTER_GM_CHANGE_NS = 250'000'000;
    /// A grandmaster was found or changed at steady-clock time @p now_ns.
    void notify_grandmaster_changed(int64_t const now_ns) noexcept
    {
        tu_until_ns_.store(now_ns + TU_AFTER_GM_CHANGE_NS, std::memory_order_relaxed);
        gptp_lost_.store(false, std::memory_order_relaxed);
    }
    /// No grandmaster is known (the Announces stopped).
    void notify_grandmaster_lost() noexcept { gptp_lost_.store(true, std::memory_order_relaxed); }
    /// Whether a PDU stamped at steady-clock time @p now_ns carries tu = 1.
    [[nodiscard]] auto timestamp_uncertain(int64_t const now_ns) const noexcept -> bool
    {
        return gptp_lost_.load(std::memory_order_relaxed) || now_ns < tu_until_ns_.load(std::memory_order_relaxed);
    }

    /// A media clock restart on STREAM_OUTPUT @p stream_index: mr toggles on its next PDU, at least MR_HOLD_PDUS after
    /// the last toggle (IEEE 1722-2016 Clause 4.4.4.3), and stays; requests before then toggle it once. Any thread,
    /// lock-free, no allocation. invalid_argument for no open stream of that index.
    [[nodiscard]] auto toggle_media_clock_restart(uint16_t stream_index) noexcept -> Status;

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
    /// Send one AAF packet of @p frames frames presented at @p pts_ns (no offset added), its payload raw wire bytes
    /// from @p render, due at @p launch_time_ns (StreamPort::launch_time). Media-timer thread; true if sent. Nothing
    /// is sent, nor the sequence advanced, for a non-AAF slot, no port frame, or @p render false.
    [[nodiscard]] auto transmit_aaf_raw(
        TalkerStreamSlot& slot,
        uint64_t first_index,
        uint64_t pts_ns,
        uint16_t frames,
        StreamRawRenderFn const& render,
        uint64_t launch_time_ns = 0) -> bool;
    /// Send one IEC 61883-6 AM824 packet of @p frames data blocks, the first at sample index @p first_index
    /// presented at @p pts_ns, its payload filled by @p render (see serialize_am824_raw), due at
    /// @p launch_time_ns. Media-timer thread; true if sent. As transmit_aaf_raw otherwise.
    [[nodiscard]] auto transmit_am824_raw(
        TalkerStreamSlot& slot,
        uint64_t first_index,
        uint64_t pts_ns,
        uint16_t frames,
        StreamRawRenderFn const& render,
        uint64_t launch_time_ns = 0) -> bool;
    /// Send the AVTPDU @p frame on @p slot (VLAN-tagged, pcap-stamped,
    /// due at @p launch_time_ns) and count it; false, counting nothing, when the port refuses it (no
    /// port: only counted). Media-timer thread.
    [[nodiscard]] auto send_stream_frame(TalkerStreamSlot& slot, std::span<uint8_t const> frame, uint64_t launch_time_ns = 0)
        -> bool;
    /// One AVTP stream frame to @p dest_mac, untimed and uncounted, tagged with the current VID and
    /// recorded to the pcap tap when armed; a frame the port refuses is dropped. Media-timer thread.
    void send_stream_frame(ieee::Eui48 const& dest_mac, std::span<uint8_t const> payload)
    {
        if (port_ != nullptr) {
            (void)send_to_port(dest_mac, payload, 0);
        }
    }

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
    [[nodiscard]] auto transmit_crf_raw(
        TalkerStreamSlot& slot, std::span<uint64_t const> timestamps_ns, uint64_t launch_time_ns = 0) -> bool;

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

    /// The AVTPDUs a toggled mr stays in its new state for, at least (IEEE 1722-2016 Clauses 4.4.4.3, 10.4.3).
    static constexpr uint8_t MR_HOLD_PDUS = 8;

    /// The most timestamps one CRF PDU carries: open_stream refuses a CRF
    /// format declaring more (or none), so the TX paths size their buffers by it.
    static constexpr uint16_t MAX_CRF_TIMESTAMPS_PER_PDU = 64;

    // References / values (bound at construction).
    TalkerStreamsConfig config_;  ///< destructured entity config (rate + VLAN/PCP), by value
    /// The VLAN tag every stream frame carries: the configured one until
    /// set_vlan() moves it (a Domain adopted from the wire, Milan 4.2.7.2.1).
    /// Atomics: written from the control thread, read per frame on the media thread.
    std::atomic<uint16_t> vlan_id_{config_.vlan_id};
    std::atomic<uint8_t> stream_pcp_{config_.stream_pcp};
    std::atomic<int64_t> tu_until_ns_{0};  ///< steady-clock end of the post-GM-change tu window
    std::atomic<bool> gptp_lost_{false};   ///< no grandmaster known: tu on every PDU
    ptpclient::MediaClockGenerator const& media_clock_;
    std::atomic<int64_t> const& last_gptp_ns_;
    std::pmr::memory_resource* mem_resource_;

    /// STREAM_OUTPUT @p stream_index opened: toggles accepted from now on, none pending.
    void mark_open(uint16_t stream_index) noexcept;
    /// The mr bit @p slot's next PDU carries: toggled once for the restarts requested since its last. Media thread.
    [[nodiscard]] auto next_mr(TalkerStreamSlot& slot) noexcept -> bool;
    /// Build @p slot's serializer (and AAF reframer) for @p spec, its stream
    /// @p stream_id: what open_stream and reformat_stream share.
    [[nodiscard]] auto shape(TalkerStreamSlot& slot, StreamSpec const& spec, tsn::StreamId stream_id) -> Status;
    /// Re-shape @p slot for @p spec through a trial, swapped in under the slot's guard.
    [[nodiscard]] auto reshape(TalkerStreamSlot& slot, StreamSpec const& spec) -> Status;
    /// Move @p trial's serializers into @p slot, carrying the stream's counts over. Under the guard.
    static void adopt_shape(TalkerStreamSlot& slot, TalkerStreamSlot&& trial, StreamSpec const& spec) noexcept;
    /// Send the AVTPDU @p serialize writes into the port's frame, in a window of at most @p max_frame
    /// octets (its length, 0: none), due at @p launch_time_ns, restoring @p ctx when the port refuses it.
    template <typename Context, typename Serialize>
    [[nodiscard]] auto send_serialized(
        TalkerStreamSlot& slot, Context& ctx, size_t max_frame, uint64_t launch_time_ns, Serialize const& serialize) -> bool;
    /// The window an AVTPDU is built in before it goes to the port: @p length octets of tx_buf_,
    /// empty with no port or for a length past it. Media thread.
    [[nodiscard]] auto tx_begin(size_t length) noexcept -> std::span<uint8_t>;
    /// One AVTP stream frame to @p dest_mac on the port, due at @p launch_time_ns: VLAN tag from
    /// vlan_id_, source the port's address, recorded to the pcap tap. False when the port refuses it.
    [[nodiscard]] auto send_to_port(ieee::Eui48 const& dest_mac, std::span<uint8_t const> payload, uint64_t launch_time_ns) -> bool;
    /// send_serialized() on a port only this thread drives: the AVTPDU rendered into the port's slot.
    template <typename Context, typename Serialize>
    [[nodiscard]] auto send_in_place(
        TalkerStreamSlot& slot, Context& ctx, size_t max_frame, uint64_t launch_time_ns, Serialize const& serialize) -> bool;
    /// send_serialized() on a serialized port: rendered into tx_buf_, then copied into a slot under the guard.
    template <typename Context, typename Serialize>
    [[nodiscard]] auto send_staged(
        TalkerStreamSlot& slot, Context& ctx, size_t max_frame, uint64_t launch_time_ns, Serialize const& serialize) -> bool;
    /// @p serialize's AVTPDU into the port's @p frame and committed due at @p launch_time_ns, or the frame given back.
    template <typename Context, typename Serialize>
    [[nodiscard]] auto serialize_into(
        TalkerStreamSlot& slot,
        Context& ctx,
        net::EthernetTxSlot const& frame,
        size_t max_frame,
        uint64_t launch_time_ns,
        Serialize const& serialize) -> bool;
    /// @p slot's 802.1Q-tagged stream header at the start of @p buffer: its length, 0 when it does not fit.
    [[nodiscard]] auto store_stream_header(TalkerStreamSlot const& slot, std::span<uint8_t> buffer) const noexcept -> size_t;
    /// Up to @p length octets of @p buffer after its @p header octets: where the AVTPDU is serialized.
    [[nodiscard]] static auto payload_window(std::span<uint8_t> buffer, size_t header, size_t length) noexcept
        -> std::span<uint8_t>;
    /// Commit @p frame, its @p header octets then its @p length-octet AVTPDU, due at @p launch_time_ns (its
    /// launch time fixed now, StreamPort::stamp_launch_time), counted on @p slot.
    [[nodiscard]] auto commit_frame(
        TalkerStreamSlot& slot, net::EthernetTxSlot const& frame, size_t header, size_t length, uint64_t launch_time_ns) -> bool;
    /// One sent AVTPDU @p frame counted on @p slot.
    void count_sent(TalkerStreamSlot& slot, std::span<uint8_t const> frame) noexcept;
    /// Record @p length bytes of @p slot to the pcap tap when armed, then commit. Under the guard.
    [[nodiscard]] auto commit_stream_frame(net::EthernetTxSlot const& slot, size_t length) -> bool;

    // Owned TX state.
    sg14::inplace_vector<TalkerStreamSlot, MAX_ENTITY_STREAMS> slots_{};
    /// Per-stream locks between the media thread's transmits (try_lock, never waits) and the
    /// reactor's reformat_stream (lock), by STREAM_OUTPUT index.
    std::array<itc::SpinLock, MAX_ENTITY_STREAMS> guards_{};
    /// Per-stream STOP_STREAMING latches (see set_stream_stopped).
    std::array<std::atomic<bool>, MAX_ENTITY_STREAMS> stream_stopped_{};
    /// Per-stream transmit counters, by STREAM_OUTPUT index (see tx_counters).
    std::array<TalkerTxCounters, MAX_ENTITY_STREAMS> tx_counters_{};
    /// Per-stream media clock restart pending (toggle_media_clock_restart): the open generation it was asked of, 0
    /// for none; it fires only on that generation. By STREAM_OUTPUT index.
    std::array<std::atomic<uint32_t>, MAX_ENTITY_STREAMS> mr_pending_{};
    /// Each STREAM_OUTPUT index's open generation, bumped by every open_stream (mark_open); 0 before the first.
    std::array<std::atomic<uint32_t>, MAX_ENTITY_STREAMS> open_generation_{};
    /// A slot is open at this STREAM_OUTPUT index (open_stream), for toggle_media_clock_restart.
    std::array<std::atomic<bool>, MAX_ENTITY_STREAMS> open_{};
    static_assert(std::atomic<bool>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free);
    StreamPort* port_{nullptr};  ///< the link (open_tx); borrowed
    /// The TX kick lives on another thread (set_external_kick).
    bool external_kick_{false};
    /// An AVTPDU being built (tx_begin): media thread only.
    std::array<uint8_t, net::MAX_ETHERNET_PAYLOAD> tx_buf_{};
    /// TX stream capture (diagnostic). last_tx_gptp_ns_ is set just before each
    /// send so the captured frame carries the gPTP TX time.
    TxPcapRecorder tx_pcap_recorder_{};
    uint64_t last_tx_gptp_ns_{0};
};

// ---------------------------------------------------------------------------
// Raw AAF serialization (pure, socket-free -- unit-testable in isolation).
// ---------------------------------------------------------------------------

/// Build one AAF frame into @p frame for @p frames frames, AVTP timestamp the low 32 bits of
/// @p pts_ns (no offset added), payload filled by @p render, with the tu and mr bits given. Returns the length
/// (sequence advanced), or 0 with @p ctx untouched for a format with no byte layout, a short @p frame or @p render false.
[[nodiscard]] auto serialize_aaf_raw(
    avtp::AafStreamOutputContext& ctx,
    std::span<uint8_t> frame,
    uint16_t frames,
    uint64_t first_index,
    uint64_t pts_ns,
    StreamRawRenderFn const& render,
    bool timestamp_uncertain = false,
    bool media_clock_restart = false) -> size_t;

/// Build one AM824 frame from float samples into @p frame (zeroed first): @p samples per channel of the
/// interleaved @p src, presented at @p now_ns plus the context's offset, tu and mr as given. The length, or 0 for none.
[[nodiscard]] auto serialize_am824_float(
    avtp::Am824StreamOutputContext& ctx,
    std::span<uint8_t> frame,
    uint32_t samples,
    uint64_t now_ns,
    std::span<float const> src,
    bool timestamp_uncertain,
    bool media_clock_restart) -> size_t;
/// As serialize_am824_float, one AAF frame.
[[nodiscard]] auto serialize_aaf_float(
    avtp::AafStreamOutputContext& ctx,
    std::span<uint8_t> frame,
    uint16_t samples,
    uint64_t now_ns,
    std::span<float const> src,
    bool timestamp_uncertain,
    bool media_clock_restart) -> size_t;

/// One IEC 61883-6 AM824 frame of @p frames blocks from sample @p first_index (the DBC, mod 256) at @p pts_ns (no
/// offset added), tu and mr as given; @p render writes frames x channels big-endian quadlets. The length (sequence
/// advanced), or 0 with @p ctx untouched for > 32 blocks, a short @p frame or @p render false.
[[nodiscard]] auto serialize_am824_raw(
    avtp::Am824StreamOutputContext& ctx,
    std::span<uint8_t> frame,
    uint16_t frames,
    uint64_t first_index,
    uint64_t pts_ns,
    StreamRawRenderFn const& render,
    bool timestamp_uncertain = false,
    bool media_clock_restart = false) -> size_t;

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
/// again from the stride-aligned live position, as crf_aligned_base did
/// every PDU.
[[nodiscard]] constexpr auto crf_schedule(
    std::optional<uint64_t> due, uint64_t const live, uint32_t const samples, uint32_t const stride, uint64_t const span) noexcept
    -> CrfSchedule
{
    if (!due || *due + span <= live || live + span < *due) {
        due = crf_aligned_base(live, stride);
    }
    if (span == 0 || *due >= live + samples) {
        return {.send = std::nullopt, .due = due};
    }
    return {.send = *due, .due = *due + span};
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
