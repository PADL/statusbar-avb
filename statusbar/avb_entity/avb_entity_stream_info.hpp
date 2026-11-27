#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Shared AEM GET_STREAM_INFO / GET_COUNTERS fills for kit entities
/// (refactor phase A — extracted from AvbEntityAudioIO /
/// AvbEntityToneGenerator, which carried line-for-line copies). Each
/// entity's aem_handler callback delegates here; the entity keeps only
/// what is genuinely per-entity (which descriptor types it serves).

#include "statusbar/atdecc/atdecc_acmp_pdu.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/avb_entity/avb_entity_host.hpp"
#include "statusbar/avb_entity/avb_entity_stream_counters.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/srp/srp_msrp.hpp"
#include "statusbar/tsn/tsn.hpp"

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

/// What GET_STREAM_INFO reports of a talker stream beyond its ACMP identity:
/// the entity's profile and the stream's MSRP and Milan state.
struct TalkerStreamStatus
{
    bool milan{true};
    /// The response is IEEE 1722.1-2021's 84 octets (AemCommandHandler::stream_info_length()), which
    /// gives msrp_accumulated_latency its meaning; else IEEE 1722.1-2013's 48.
    bool ieee_2021{true};
    /// Declaring its Talker Advertise.
    bool declaring{false};
    /// The Listener declaration registered for it (Ignore: none).
    srp::msrp::ListenerDeclaration listener{srp::msrp::ListenerDeclaration::Ignore};
    /// Milan v1.2 Section 5.4.2.10.2: an output's msrp_accumulated_latency is
    /// its presentation time offset.
    uint32_t presentation_offset_ns{2'000'000};
    /// The AccumulatedLatency its Talker Advertise declares, ns: the IEEE 1722.1-2021
    /// layout's msrp_accumulated_latency (Milan v1.3 Section 5.4.2.10.2).
    uint32_t accumulated_latency_ns{0};
    /// Stopped by STOP_STREAMING: STREAMING_WAIT (IEEE 1722.1-2021 Table 7-145).
    bool stopped{false};
};

/// MSRP_ACC_LAT_VALID for an output with @p status: Milan always (v1.3 Table 5.9); classic in
/// the IEEE 1722.1-2021 layout while it declares the attribute the latency is (Clause 7.4.16.2).
[[nodiscard]] constexpr auto talker_accumulated_latency_flag(TalkerStreamStatus const& status) noexcept -> uint32_t
{
    bool const valid = status.milan || status.declaring || !status.ieee_2021;
    return valid ? atdecc::aem::stream_info_flags::MSRP_ACC_LAT_VALID : 0U;
}

/// An output's msrp_accumulated_latency in @p status's layout: the declared AccumulatedLatency
/// in IEEE 1722.1-2021's (Clause 7.4.16.2), the presentation time offset in the older ones.
[[nodiscard]] constexpr auto talker_msrp_accumulated_latency(TalkerStreamStatus const& status) noexcept -> uint32_t
{
    bool const declared = status.ieee_2021;
    return declared ? status.accumulated_latency_ns : status.presentation_offset_ns;
}

/// The SRP flags of a Milan output in the IEEE 1722.1-2021 layout (Milan v1.3 Table 5.9). The
/// entity declares no Talker Failed, so MSRP_FAILURE_VALID stays clear.
[[nodiscard]] constexpr auto milan_v13_talker_srp_flags(TalkerStreamStatus const& status) noexcept -> uint32_t
{
    namespace stream_info_flags = atdecc::aem::stream_info_flags;
    using srp::msrp::ListenerDeclaration;
    if (!status.declaring) {
        return 0;
    }
    switch (status.listener) {
        case ListenerDeclaration::Ignore:
            return stream_info_flags::NOT_REGISTERING_SRP;
        case ListenerDeclaration::AskingFailed:
            return stream_info_flags::REGISTERING_FAILED;
        case ListenerDeclaration::Ready:
        case ListenerDeclaration::ReadyFailed:
        default:
            return stream_info_flags::CONNECTED;
    }
}

/// The flags of classic output @p descriptor_index: CONNECTED from the ACMP connection count,
/// STREAMING_WAIT while @p status is stopped. SR class A is the entities' only class, so CLASS_B stays clear.
[[nodiscard]] inline auto classic_talker_flags(
    AvbEntityHost const& host, uint16_t const descriptor_index, TalkerStreamStatus const& status) -> uint32_t
{
    namespace stream_info_flags = atdecc::aem::stream_info_flags;
    bool const connected = host.components().acmp_talker.connection_count(descriptor_index) > 0;
    return (connected ? stream_info_flags::CONNECTED : 0U) | (status.stopped ? stream_info_flags::STREAMING_WAIT : 0U);
}

/// The STREAM_OUTPUT descriptor's current_format into @p out: STREAM_FORMAT_VALID when the
/// model has the output, else 0.
[[nodiscard]] inline auto fill_talker_format(
    AvbEntityHost const& host, uint16_t const descriptor_index, atdecc::aem::AemStreamInfoPayload& out) -> uint32_t
{
    auto const desc = host.get_descriptor(atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, descriptor_index);
    if (!desc.has_value()) {
        return 0;
    }
    atdecc::aem::DescriptorStream stream_desc{};
    span_load_padded(stream_desc, *desc);
    span_copy(make_span(out.stream_format), stream_desc.current_format.span());
    return atdecc::aem::stream_info_flags::STREAM_FORMAT_VALID;
}

/// An output's stream ID, address, VID and msrp_accumulated_latency into @p out; the first three's flags:
/// the ID and VID always (valid while declaring, undefined otherwise), the address while MAAP
/// has one (Milan v1.2 Table 5.3).
[[nodiscard]] inline auto fill_talker_identity(
    nanoavb::NanoAvbComponents::TalkerStream const& stream,
    TalkerStreamStatus const& status,
    atdecc::aem::AemStreamInfoPayload& out) -> uint32_t
{
    using namespace atdecc::aem::stream_info_flags;
    out.stream_id = stream.stream_id;
    out.stream_dest_mac = stream.stream_dest_mac;
    out.stream_vlan_id = ieee::doublet_t{stream.stream_vlan_id};
    out.msrp_accumulated_latency = ieee::quadlet_t{talker_msrp_accumulated_latency(status)};
    return (stream.stream_dest_mac != ieee::Eui48{} ? STREAM_DEST_MAC_VALID : 0U) | STREAM_ID_VALID | STREAM_VLAN_ID_VALID;
}

/// Fill the GET_STREAM_INFO response for STREAM_OUTPUT @p index from the live ACMP
/// stream identity and current_format (Milan v1.3 Section 5.4.2.10.2 in the 2021
/// layout; flag rules in docs/AVB_ENTITY_KIT.md). False if it is not one of the host's talkers.
[[nodiscard]] inline auto fill_talker_stream_info(
    AvbEntityHost const& host,
    uint16_t const descriptor_index,
    TalkerStreamStatus const& status,
    atdecc::aem::AemStreamInfoPayload& out) -> bool
{
    auto const stream = host.components().talker_stream(descriptor_index);
    if (!stream.has_value()) {
        return false;
    }
    uint32_t const flags = fill_talker_identity(*stream, status, out) | fill_talker_format(host, descriptor_index, out) |
        talker_accumulated_latency_flag(status);
    out.flags = ieee::quadlet_t{
        flags | (status.milan ? milan_v13_talker_srp_flags(status) : classic_talker_flags(host, descriptor_index, status))};
    return true;
}

/// The STREAM_INPUT descriptor's current_format into @p out (true when the
/// model has the input).
[[nodiscard]] inline auto fill_stream_input_format(
    AvbEntityHost const& host, uint16_t const descriptor_index, atdecc::aem::AemStreamInfoPayload& out) -> bool
{
    auto const desc = host.get_descriptor(atdecc::aem::DESCRIPTOR_STREAM_INPUT, descriptor_index);
    if (!desc.has_value()) {
        return false;
    }
    atdecc::aem::DescriptorStream stream_desc{};
    span_load_padded(stream_desc, *desc);
    span_copy(make_span(out.stream_format), stream_desc.current_format.span());
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
    uint32_t const format =
        fill_stream_input_format(host, descriptor_index, out) ? atdecc::aem::stream_info_flags::STREAM_FORMAT_VALID : 0U;
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
