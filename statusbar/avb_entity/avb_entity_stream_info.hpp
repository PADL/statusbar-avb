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

/// Fill the classic GET_STREAM_INFO response for STREAM_INPUT @p descriptor_index
/// from the ACMP listener sink: the connected talker's stream_id and dest MAC
/// when connected, plus current_format. @p stopped is the input's stopped state
/// (ListenerStreams::stream_stopped), reported as STREAMING_WAIT (IEEE 1722.1-2021
/// Table 7-145: a stream "stopped with STOP_STREAMING command").
[[nodiscard]] inline auto fill_listener_stream_info(
    AvbEntityHost const& host, uint16_t const descriptor_index, bool const stopped, atdecc::aem::AemStreamInfoPayload& out) -> bool
{  // NOLINT(statusbar-block-statements) one GET_STREAM_INFO field per statement
    namespace stream_info_flags = atdecc::aem::stream_info_flags;

    auto const sink = host.components().listener_sink(descriptor_index);
    if (!sink.has_value()) {
        return false;
    }
    uint32_t flags = 0;
    if (fill_stream_input_format(host, descriptor_index, out)) {
        flags |= stream_info_flags::STREAM_FORMAT_VALID;
    }
    if (sink->connected) {
        flags |= stream_info_flags::CONNECTED | stream_info_flags::STREAM_ID_VALID | stream_info_flags::STREAM_DEST_MAC_VALID;
        out.stream_id = sink->stream_id;
        out.stream_dest_mac = sink->stream_dest_mac;
        if (sink->stream_vlan_id != 0) {
            flags |= stream_info_flags::STREAM_VLAN_ID_VALID;
            out.stream_vlan_id = ieee::doublet_t{sink->stream_vlan_id};
        }
    }
    out.flags = ieee::quadlet_t{flags | (stopped ? stream_info_flags::STREAMING_WAIT : 0U)};
    return true;
}

/// The MSRP fields of a registering Milan input's GET_STREAM_INFO from the talker's
/// declaration (Milan v1.2 Table 5.9), @p srp its probed parameters; the flags they make valid.
[[nodiscard]] inline auto fill_milan_registration(
    AvbEntityHost const& host, atdecc::MilanSrpParameters const& srp, atdecc::aem::AemStreamInfoPayload& out) -> uint32_t
{
    namespace stream_info_flags = atdecc::aem::stream_info_flags;
    statusbar::tsn::StreamId sid{};
    (void)statusbar::tsn::load_unchecked(srp.stream_id.span(), &sid);
    if (auto const* failed = host.components().msrp_handler.peer_talker_failed(sid); failed != nullptr) {
        out.msrp_failure_code = failed->failure_code;
        out.msrp_failure_bridge_id = failed->failure_bridge_id;
        out.msrp_accumulated_latency = failed->advertise.accumulated_latency;
        return stream_info_flags::MSRP_ACC_LAT_VALID | stream_info_flags::REGISTERING_FAILED |
            stream_info_flags::MSRP_FAILURE_VALID;
    }
    if (auto const* advertise = host.components().msrp_handler.peer_talker_advertise(sid); advertise != nullptr) {
        out.msrp_accumulated_latency = advertise->accumulated_latency;
    }
    return stream_info_flags::MSRP_ACC_LAT_VALID;
}

/// Whether SRP registers a Talker attribute matching settled Milan input @p srp (Milan v1.3 Section 5.3.8.8).
[[nodiscard]] inline auto milan_input_registering(AvbEntityHost const& host, atdecc::MilanSrpParameters const& srp) -> bool
{
    statusbar::tsn::StreamId sid{};
    (void)statusbar::tsn::load_unchecked(srp.stream_id.span(), &sid);
    nanoavb::TalkerMatch const match{.dest_address = srp.stream_dest_mac, .vlan_id = srp.stream_vlan_id};
    return host.components().msrp_handler.registered_talker(sid, match).has_value();
}

/// The fields of a Milan input's GET_STREAM_INFO that its format, binding, stream and
/// registration give (Milan v1.2 Table 5.9); their flags.
[[nodiscard]] inline auto fill_milan_listener_fields(
    AvbEntityHost const& host,
    uint16_t const descriptor_index,
    atdecc::MilanSink const& sink,
    atdecc::aem::AemStreamInfoPayload& out) -> uint32_t
{  // NOLINT(statusbar-block-statements) one GET_STREAM_INFO field group per statement
    namespace stream_info_flags = atdecc::aem::stream_info_flags;
    auto const& ctx = sink.context();
    uint32_t flags = fill_stream_input_format(host, descriptor_index, out) ? stream_info_flags::STREAM_FORMAT_VALID : 0U;
    if (ctx.binding.has_value()) {
        flags |= stream_info_flags::BOUND | stream_info_flags::FAST_CONNECT | stream_info_flags::SAVED_STATE;
        flags |= ctx.binding->streaming_wait ? stream_info_flags::STREAMING_WAIT : 0U;
    }
    if (sink.is_settled()) {
        flags |=
            stream_info_flags::STREAM_ID_VALID | stream_info_flags::STREAM_DEST_MAC_VALID | stream_info_flags::STREAM_VLAN_ID_VALID;
        out.stream_id = ctx.srp.stream_id;
        out.stream_dest_mac = ctx.srp.stream_dest_mac;
        out.stream_vlan_id = ieee::doublet_t{ctx.srp.stream_vlan_id};
    }
    if (sink.is_settled() && milan_input_registering(host, ctx.srp)) {
        flags |= fill_milan_registration(host, ctx.srp, out);
    }
    return flags;
}

/// NOT_REGISTERING_SRP for a Milan input in IEEE 1722.1-2021's layout (@p ieee_2021) that is bound and whose
/// Talker attribute SRP does not register (Milan v1.3 Table 5.8); else 0.
[[nodiscard]] inline auto milan_listener_srp_flag(AvbEntityHost const& host, atdecc::MilanSink const& sink, bool const ieee_2021)
    -> uint32_t
{
    bool const registering = sink.is_settled() && milan_input_registering(host, sink.context().srp);
    return ieee_2021 && sink.is_bound() && !registering ? atdecc::aem::stream_info_flags::NOT_REGISTERING_SRP : 0U;
}

/// Fill the Milan GET_STREAM_INFO response for a STREAM_INPUT from its sink state machine (Milan
/// v1.3 Section 5.4.2.10.1 Table 5.8; NOT_REGISTERING_SRP in IEEE 1722.1-2021's layout, @p ieee_2021;
/// docs/AVB_ENTITY_KIT.md). False when the input or its sink does not exist.
[[nodiscard]] inline auto fill_milan_listener_stream_info(
    AvbEntityHost const& host, uint16_t const descriptor_index, bool const ieee_2021, atdecc::aem::AemStreamInfoPayload& out)
    -> bool
{
    auto const* listener = host.components().milan_acmp_listener.get();
    auto const* sink = listener != nullptr ? listener->sink(descriptor_index) : nullptr;
    if (sink == nullptr) {
        return false;
    }
    uint32_t const flags = fill_milan_listener_fields(host, descriptor_index, *sink, out);
    out.flags = ieee::quadlet_t{flags | milan_listener_srp_flag(host, *sink, ieee_2021)};
    return true;
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
