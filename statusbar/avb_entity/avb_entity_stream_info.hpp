#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Shared AEM GET_STREAM_INFO / GET_COUNTERS fills for kit entities
/// (refactor phase A — extracted from AvbEntityAudioIO /
/// AvbEntityToneGenerator, which carried line-for-line copies). Each
/// entity's aem_handler callback delegates here; the entity keeps only
/// what is genuinely per-entity (which descriptor types it serves).

#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/avb_entity/avb_entity_host.hpp"
#include "statusbar/avb_entity/avb_entity_stream_counters.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/buffer/span_utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace statusbar::avb_entity {

/// IEEE 1722.1-2021 Table 7-158: the STREAM_OUTPUT FRAMES_TX counter is bit 7
/// of counters_valid (bits 0-6: STREAM_START, STREAM_STOP, STREAM_INTERRUPTED,
/// MEDIA_RESET, TIMESTAMP_UNCERTAIN, TIMESTAMP_VALID, TIMESTAMP_NOT_VALID).
inline constexpr unsigned stream_output_frames_tx_bit = 7;
/// The other STREAM_OUTPUT counters a talker counts, by bit (Table 7-158;
/// Milan v1.3 Table 5.14 has the same).
inline constexpr unsigned stream_output_stream_start_bit = 0;
inline constexpr unsigned stream_output_stream_stop_bit = 1;
inline constexpr unsigned stream_output_media_reset_bit = 3;
inline constexpr unsigned stream_output_timestamp_uncertain_bit = 4;
/// Where a Milan STREAM_OUTPUT reports signal presence (Milan v1.3 Section 5.3.7.7.1):
/// ENTITY_SPECIFIC_9 for channels 0-31, ENTITY_SPECIFIC_10 for 32-59.
inline constexpr unsigned stream_output_signal_presence_0_31_bit = 23;
inline constexpr unsigned stream_output_signal_presence_32_59_bit = 22;

/// Copy a stream descriptor's current_format into @p out (read from the blob
/// via the symbol-aware host); true when the descriptor exists.
[[nodiscard]] inline auto fill_stream_format(
    AvbEntityHost const& host,
    uint16_t const descriptor_type,
    uint16_t const descriptor_index,
    atdecc::aem::AemStreamInfoPayload& out) -> bool
{
    auto const desc = host.get_descriptor(descriptor_type, descriptor_index);
    if (!desc.has_value()) {
        return false;
    }
    atdecc::aem::DescriptorStream stream_desc{};
    span_load_padded(stream_desc, *desc);
    span_copy(make_span(out.stream_format), stream_desc.current_format.span());
    return true;
}

/// IEEE 1722.1-2021 Table 7-145 flags for a talker stream: the identity
/// fields a talker always knows, the format when its descriptor exists, the
/// live ACMP connection state, and STREAMING_WAIT for a stream "stopped with
/// STOP_STREAMING command". SR class A is the entities' only class, so
/// CLASS_B stays clear.
[[nodiscard]] inline auto talker_stream_info_flags(
    AvbEntityHost const& host, uint16_t const descriptor_index, bool const format_valid, bool const stopped) -> uint32_t
{
    namespace stream_info_flags = atdecc::aem::stream_info_flags;
    uint32_t flags = stream_info_flags::STREAM_ID_VALID | stream_info_flags::STREAM_DEST_MAC_VALID |
        stream_info_flags::STREAM_VLAN_ID_VALID | stream_info_flags::MSRP_ACC_LAT_VALID;
    if (format_valid) {
        flags |= stream_info_flags::STREAM_FORMAT_VALID;
    }
    if (host.components().acmp_talker.connection_count(descriptor_index) > 0) {
        flags |= stream_info_flags::CONNECTED;
    }
    if (stopped) {
        flags |= stream_info_flags::STREAMING_WAIT;
    }
    return flags;
}

/// The identity fields a talker always knows: its stream_id, destination
/// MAC and VLAN from the ACMP talker state, and the accumulated latency it
/// advertises (the entity's presentation offset).
template <typename TalkerStream>
inline void copy_talker_identity(
    TalkerStream const& stream, uint64_t const presentation_offset_ns, atdecc::aem::AemStreamInfoPayload& out)
{
    out.stream_id = stream.stream_id;
    out.stream_dest_mac = stream.stream_dest_mac;
    out.stream_vlan_id = ieee::doublet_t{stream.stream_vlan_id};
    out.msrp_accumulated_latency = ieee::quadlet_t{static_cast<uint32_t>(presentation_offset_ns)};
}

/// Fill the GET_STREAM_INFO response for a STREAM_OUTPUT (talker) index
/// from the live ACMP stream identity + the descriptor's current_format
/// (true if it is one of the host's talker streams). A Milan listener
/// queries this to verify the stream before sustaining a connection.
/// @p stopped is the talker's STOP_STREAMING latch: IEEE 1722.1-2021 Table
/// 7-145 reports a stream "stopped with STOP_STREAMING command" as
/// STREAMING_WAIT, the only way a controller can see that an output is stopped.
[[nodiscard]] inline auto fill_talker_stream_info(
    AvbEntityHost const& host,
    uint16_t const descriptor_index,
    uint64_t const presentation_offset_ns,
    bool const stopped,
    atdecc::aem::AemStreamInfoPayload& out) -> bool
{
    auto const stream = host.components().talker_stream(descriptor_index);
    if (!stream.has_value()) {
        return false;
    }
    bool const format_valid = fill_stream_format(host, atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, descriptor_index, out);
    copy_talker_identity(*stream, presentation_offset_ns, out);
    out.flags = ieee::quadlet_t{talker_stream_info_flags(host, descriptor_index, format_valid, stopped)};
    return true;
}

/// A connected sink knows the talker's stream identity: copy it into @p out
/// and return the Table 7-145 flags it validates (none while unconnected).
template <typename ListenerSink>
[[nodiscard]] inline auto copy_connected_sink_identity(ListenerSink const& sink, atdecc::aem::AemStreamInfoPayload& out) -> uint32_t
{
    if (!sink.connected) {
        return 0;
    }
    uint32_t flags = atdecc::aem::stream_info_flags::CONNECTED | atdecc::aem::stream_info_flags::STREAM_ID_VALID |
        atdecc::aem::stream_info_flags::STREAM_DEST_MAC_VALID;
    out.stream_id = sink.stream_id;
    out.stream_dest_mac = sink.stream_dest_mac;
    if (sink.stream_vlan_id != 0) {
        flags |= atdecc::aem::stream_info_flags::STREAM_VLAN_ID_VALID;
        out.stream_vlan_id = ieee::doublet_t{sink.stream_vlan_id};
    }
    return flags;
}

/// An IEEE 1722.1 listener's sink into @p out: copy_connected_sink_identity(), and STREAMING_WAIT
/// for an input that is stopped. Returns the flags.
template <typename ListenerSink>
[[nodiscard]] inline auto fill_ieee_sink_stream_info(
    ListenerSink const& sink, bool const stopped, atdecc::aem::AemStreamInfoPayload& out) -> uint32_t
{
    return copy_connected_sink_identity(sink, out) | (stopped ? atdecc::aem::stream_info_flags::STREAMING_WAIT : 0U);
}

/// What Milan v1.3 Table 5.8 reads of a stream input: whether its sink is bound and settled, its
/// started/stopped state, the SRP parameters its talker's PROBE_TX_RESPONSE gave while it is
/// settled, and the matching Talker attribute the port registers then (5.3.8.8), if any.
struct MilanSinkStreamInfo
{
    bool bound{false};
    bool settled{false};
    bool stopped{false};
    atdecc::MilanSrpParameters srp{};
    std::optional<nanoavb::RegisteredTalker> talker{};

    /// "Registering either a matching Talker Advertise or a matching Talker Failed attribute".
    [[nodiscard]] constexpr auto registering() const noexcept -> bool { return settled && talker.has_value(); }
    /// "Registering a matching Talker Failed attribute".
    [[nodiscard]] constexpr auto registering_failed() const noexcept -> bool { return registering() && talker->failed; }
};

/// The flags Table 5.8 sets for a stream input that is bound: FAST_CONNECT, SAVED_STATE and
/// CONNECTED, STREAMING_WAIT while it is stopped, and NOT_REGISTERING_SRP while it registers
/// no matching Talker attribute. None for one that is not bound.
[[nodiscard]] constexpr auto milan_bound_sink_flags(MilanSinkStreamInfo const& sink) noexcept -> uint32_t
{
    namespace stream_info_flags = atdecc::aem::stream_info_flags;
    if (!sink.bound) {
        return 0;
    }
    uint32_t const wait = sink.stopped ? stream_info_flags::STREAMING_WAIT : 0U;
    uint32_t const srp = sink.registering() ? 0U : stream_info_flags::NOT_REGISTERING_SRP;
    return stream_info_flags::FAST_CONNECT | stream_info_flags::SAVED_STATE | stream_info_flags::CONNECTED | wait | srp;
}

/// Every flag of Table 5.8 but STREAM_FORMAT_VALID: those of a bound input, the three that mark
/// the stream's id, address and VLAN valid "if, and only if, the Stream Input is settled",
/// MSRP_ACC_LAT_VALID while it registers a matching Talker attribute, and
/// SRP_REGISTRATION_FAILED with MSRP_FAILURE_VALID while that attribute is a Talker Failed.
[[nodiscard]] constexpr auto milan_sink_stream_info_flags(MilanSinkStreamInfo const& sink) noexcept -> uint32_t
{
    namespace stream_info_flags = atdecc::aem::stream_info_flags;
    uint32_t const settled = sink.settled
        ? stream_info_flags::STREAM_ID_VALID | stream_info_flags::STREAM_DEST_MAC_VALID | stream_info_flags::STREAM_VLAN_ID_VALID
        : 0U;
    uint32_t const latency = sink.registering() ? stream_info_flags::MSRP_ACC_LAT_VALID : 0U;
    uint32_t const failure =
        sink.registering_failed() ? stream_info_flags::SRP_REGISTRATION_FAILED | stream_info_flags::MSRP_FAILURE_VALID : 0U;
    return milan_bound_sink_flags(sink) | settled | latency | failure;
}

/// The stream's id, destination address and VLAN into @p out as the PROBE_TX_RESPONSE gave them
/// while the input is settled, zero otherwise (Milan v1.3 5.4.2.10.1).
inline void copy_milan_sink_stream(MilanSinkStreamInfo const& sink, atdecc::aem::AemStreamInfoPayload& out) noexcept
{
    out.stream_id = sink.settled ? sink.srp.stream_id : ieee::Eui64{};
    out.stream_dest_mac = sink.settled ? sink.srp.stream_dest_mac : ieee::Eui48{};
    out.stream_vlan_id = ieee::doublet_t{sink.settled ? sink.srp.stream_vlan_id : uint16_t{0}};
}

/// The fields of Milan v1.3 5.4.2.10.1 into @p out, each zero where Table 5.8 does not mark it
/// valid: the stream's id, destination address and VLAN as the PROBE_TX_RESPONSE gave them, the
/// accumulated latency of the registered Talker attribute, and a Talker Failed's bridge and code.
/// The entity adds no delay of its own to the latency: it knows none between the wire and its
/// MAC. Returns milan_sink_stream_info_flags().
inline auto fill_milan_sink_stream_info(MilanSinkStreamInfo const& sink, atdecc::aem::AemStreamInfoPayload& out) noexcept
    -> uint32_t
{
    copy_milan_sink_stream(sink, out);
    out.msrp_accumulated_latency = ieee::quadlet_t{sink.registering() ? sink.talker->accumulated_latency : 0U};
    out.msrp_failure_code = ieee::octet_t{sink.registering_failed() ? sink.talker->failure_code : uint8_t{0}};
    out.msrp_failure_bridge_id = sink.registering_failed() ? sink.talker->failure_bridge_id : ieee::Eui64{};
    return milan_sink_stream_info_flags(sink);
}

/// Fill the GET_STREAM_INFO response for a STREAM_INPUT (listener) index
/// from the ACMP listener sink state: the connected talker's stream_id /
/// dest MAC when bound, plus the descriptor's current_format — so a
/// controller can read what an input is connected to. @p stopped is the
/// input's stopped state (ListenerStreams::stream_stopped), reported as
/// STREAMING_WAIT: "1 if the Stream Input is bound and stopped" (Milan v1.3
/// Table 5.8), and IEEE 1722.1-2021 Table 7-145's stream "stopped with
/// STOP_STREAMING command".
///
/// @p milan is the input as a Milan sink has it (MilanSinkStreamInfo), for an
/// entity whose connection management is Milan's: the flags and fields are
/// then Table 5.8's and @p stopped is not read. nullopt for an IEEE 1722.1
/// listener.
[[nodiscard]] inline auto fill_listener_stream_info(
    AvbEntityHost const& host,
    uint16_t const descriptor_index,
    bool const stopped,
    std::optional<MilanSinkStreamInfo> const& milan,
    atdecc::aem::AemStreamInfoPayload& out) -> bool
{
    auto const sink = host.components().listener_sink(descriptor_index);
    if (!sink.has_value()) {
        return false;
    }
    uint32_t const format = fill_stream_format(host, atdecc::aem::DESCRIPTOR_STREAM_INPUT, descriptor_index, out)
        ? atdecc::aem::stream_info_flags::STREAM_FORMAT_VALID
        : 0U;
    uint32_t const state =
        milan.has_value() ? fill_milan_sink_stream_info(*milan, out) : fill_ieee_sink_stream_info(*sink, stopped, out);
    out.flags = ieee::quadlet_t{format | state};
    return true;
}

/// As above for an IEEE 1722.1 listener.
[[nodiscard]] inline auto fill_listener_stream_info(
    AvbEntityHost const& host, uint16_t const descriptor_index, bool const stopped, atdecc::aem::AemStreamInfoPayload& out) -> bool
{
    return fill_listener_stream_info(host, descriptor_index, stopped, std::nullopt, out);
}

/// What STREAM_OUTPUT @p descriptor_index has sent (TalkerTxCounters) as
/// StreamOutputTotals, its starts and stops of one instant; zeros for an
/// index past MAX_ENTITY_STREAMS. Reactor thread.
[[nodiscard]] inline auto talker_stream_totals(TalkerStreams const& talker, uint16_t const descriptor_index) -> StreamOutputTotals
{
    auto const* const c = talker.tx_counters(descriptor_index);
    if (c == nullptr) {
        return {};
    }
    auto const relaxed = std::memory_order_relaxed;
    uint32_t const changes = c->streaming_changes.load(relaxed);
    return {
        .starts = stream_starts(changes),
        .stops = stream_stops(changes),
        .pdus = c->pdus.load(relaxed),
        .tu_pdus = c->tu_pdus.load(relaxed),
        .mr_toggles = c->mr_toggles.load(relaxed)};
}

/// The counters_valid bits of the STREAM_OUTPUT counters a talker counts.
inline constexpr uint32_t stream_output_counters_valid = (1U << stream_output_stream_start_bit) |
    (1U << stream_output_stream_stop_bit) | (1U << stream_output_media_reset_bit) | (1U << stream_output_timestamp_uncertain_bit) |
    (1U << stream_output_frames_tx_bit);

/// @p totals as running counts into the GET_COUNTERS array @p out, each at its bit.
inline void store_stream_output_totals(StreamOutputTotals const& totals, std::array<uint32_t, 32>& out) noexcept
{
    out[stream_output_stream_start_bit] = totals.starts;
    out[stream_output_stream_stop_bit] = totals.stops;
    out[stream_output_media_reset_bit] = totals.mr_toggles;
    out[stream_output_timestamp_uncertain_bit] = totals.tu_pdus;
    out[stream_output_frames_tx_bit] = totals.pdus;
}

/// Fill the GET_COUNTERS bitmap for a STREAM_OUTPUT index (IEEE 1722.1-2021
/// Clause 7.4.42, Table 7-158) from what the talker sent (TalkerTxCounters):
/// STREAM_START, STREAM_STOP, MEDIA_RESET, TIMESTAMP_UNCERTAIN and FRAMES_TX,
/// as running counts. False for a stream the talker has no slot for.
[[nodiscard]] inline auto fill_talker_stream_counters(
    TalkerStreams const& talker, uint16_t const descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& out) -> bool
{
    if (talker.slot_for(descriptor_index) == nullptr || talker.tx_counters(descriptor_index) == nullptr) {
        return false;
    }
    valid |= stream_output_counters_valid;
    store_stream_output_totals(talker_stream_totals(talker, descriptor_index), out);
    return true;
}

/// The signal presence @p channels (bit k channel k) a STREAM_OUTPUT of @p stream_channels
/// reports: the bits past its channels, and past TalkerStreams::SIGNAL_PRESENCE_CHANNELS, clear
/// (Milan v1.3 Section 5.3.7.7.1).
[[nodiscard]] constexpr auto reported_signal_presence(uint64_t const channels, size_t const stream_channels) noexcept -> uint64_t
{
    size_t const reported = std::min(stream_channels, TalkerStreams::SIGNAL_PRESENCE_CHANNELS);
    return channels & ((uint64_t{1} << reported) - 1U);
}

/// The signal presence of channels @p first to @p first + 31 of @p channels (bit k channel k) as
/// one counter: channel first + i at bit i counted from the MSB, channels past 59 clear (Milan v1.3
/// Section 5.3.7.7.1).
[[nodiscard]] constexpr auto signal_presence_quadlet(uint64_t const channels, size_t const first) noexcept -> uint32_t
{
    uint32_t quadlet = 0;
    for (size_t i = 0; i < 32 && first + i < TalkerStreams::SIGNAL_PRESENCE_CHANNELS; ++i) {
        quadlet |= ((channels >> (first + i)) & 1U) != 0 ? 0x8000'0000U >> i : 0U;
    }
    return quadlet;
}

/// @p presence, within a stream of @p stream_channels, into its two counters of @p out; the
/// counters_valid bits that makes valid, none while the output reports no signal presence.
[[nodiscard]] inline auto store_signal_presence(
    std::optional<uint64_t> const presence, size_t const stream_channels, std::array<uint32_t, 32>& out) noexcept -> uint32_t
{
    if (!presence) {
        return 0;
    }
    uint64_t const reported = reported_signal_presence(*presence, stream_channels);
    out[stream_output_signal_presence_0_31_bit] = signal_presence_quadlet(reported, 0);
    out[stream_output_signal_presence_32_59_bit] = signal_presence_quadlet(reported, 32);
    return (1U << stream_output_signal_presence_0_31_bit) | (1U << stream_output_signal_presence_32_59_bit);
}

/// Fill the GET_COUNTERS bitmap for a STREAM_OUTPUT index as Milan v1.3 Table
/// 5.14 has them: @p intervals, the stream's StreamOutputIntervals, current()
/// at what the talker has sent, and the signal presence the talker holds now within the stream's
/// @p stream_channels, once the output reports it. False for a stream the talker has no slot for.
/// Reactor thread.
[[nodiscard]] inline auto fill_interval_talker_stream_counters(
    TalkerStreams const& talker,
    uint16_t const descriptor_index,
    StreamOutputIntervals const& intervals,
    size_t const stream_channels,
    uint32_t& valid,
    std::array<uint32_t, 32>& out) -> bool
{
    if (talker.slot_for(descriptor_index) == nullptr) {
        return false;
    }
    valid |= StreamOutputIntervals::VALID;
    auto const values = intervals.current(talker_stream_totals(talker, descriptor_index));
    for (size_t i = 0; i < values.size(); ++i) {
        if ((StreamOutputIntervals::VALID & (1U << i)) != 0) {
            out[i] = values[i];
        }
    }
    valid |= store_signal_presence(talker.signal_presence(descriptor_index), stream_channels, out);
    return true;
}

}  // namespace statusbar::avb_entity
