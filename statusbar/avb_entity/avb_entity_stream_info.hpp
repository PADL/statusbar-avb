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
#include "statusbar/srp/srp_msrp.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <algorithm>
#include <array>
#include <atomic>
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
    /// Declaring its Talker Advertise.
    bool declaring{false};
    /// The Listener declaration registered for it (Ignore: none).
    srp::msrp::ListenerDeclaration listener{srp::msrp::ListenerDeclaration::Ignore};
    /// Milan v1.2 Section 5.4.2.10.2: an output's msrp_accumulated_latency is
    /// its presentation time offset.
    uint32_t presentation_offset_ns{2'000'000};
    /// Stopped by STOP_STREAMING: STREAMING_WAIT (IEEE 1722.1-2021 Table 7-145).
    bool stopped{false};
};

/// Fill the GET_STREAM_INFO response for STREAM_OUTPUT @p index from the live ACMP
/// stream identity and current_format (Milan v1.2 Section 5.4.2.10.2; flag rules
/// in docs/AVB_ENTITY_KIT.md). False if it is not one of the host's talker streams.
[[nodiscard]] inline auto fill_talker_stream_info(
    AvbEntityHost const& host,
    uint16_t const descriptor_index,
    TalkerStreamStatus const& status,
    atdecc::aem::AemStreamInfoPayload& out,
    atdecc::aem::AemStreamInfoMilanExtension& ext) -> bool
{
    using atdecc::aem::DESCRIPTOR_STREAM_OUTPUT;
    namespace stream_info_flags = atdecc::aem::stream_info_flags;
    using srp::msrp::ListenerDeclaration;

    auto const* stream = host.components().acmp_talker.get_stream(descriptor_index);
    if (stream == nullptr) {
        return false;
    }

    uint32_t flags =
        stream_info_flags::STREAM_ID_VALID | stream_info_flags::STREAM_VLAN_ID_VALID | stream_info_flags::MSRP_ACC_LAT_VALID;

    // Stream format from the STREAM_OUTPUT descriptor's current_format,
    // read from the blob via the symbol-aware host.
    if (auto const desc = host.get_descriptor(DESCRIPTOR_STREAM_OUTPUT, descriptor_index); desc.has_value()) {
        atdecc::aem::DescriptorStream stream_desc{};
        span_load_padded(stream_desc, *desc);
        span_copy(make_span(out.stream_format), stream_desc.current_format.span());
        flags |= stream_info_flags::STREAM_FORMAT_VALID;
    }

    out.stream_id = stream->stream_id;
    out.stream_dest_mac = stream->stream_dest_mac;
    out.stream_vlan_id = ieee::doublet_t{stream->stream_vlan_id};
    out.msrp_accumulated_latency = ieee::quadlet_t{status.presentation_offset_ns};

    if (status.milan) {
        // Zero while MAAP has no address (Milan v1.2 Table 5.3).
        if (stream->stream_dest_mac != ieee::Eui48{}) {
            flags |= stream_info_flags::STREAM_DEST_MAC_VALID;
        }
        bool const registering = status.declaring && status.listener != ListenerDeclaration::Ignore;
        if (registering && status.listener == ListenerDeclaration::AskingFailed) {
            flags |= stream_info_flags::REGISTERING_FAILED;
        }
        ext.flags_ex = ieee::quadlet_t{registering ? atdecc::aem::stream_info_flags_ex::REGISTERING : 0U};
        ext.set_probing(atdecc::aem::probing_status::DISABLED, 0);
    } else {
        // SR class A is the entities' only class, so CLASS_B stays clear. Report
        // the live ACMP connection state so a controller/listener sees CONNECTED.
        if (stream->stream_dest_mac != ieee::Eui48{}) {
            flags |= stream_info_flags::STREAM_DEST_MAC_VALID;  // zero until MAAP has one
        }
        if (host.components().acmp_talker.connection_count(descriptor_index) > 0) {
            flags |= stream_info_flags::CONNECTED;
        }
        if (status.stopped) {
            flags |= stream_info_flags::STREAMING_WAIT;
        }
    }
    out.flags = ieee::quadlet_t{flags};
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
    using atdecc::aem::DESCRIPTOR_STREAM_INPUT;
    namespace stream_info_flags = atdecc::aem::stream_info_flags;
    namespace probing_status = atdecc::aem::probing_status;

    auto const& listener = host.components().acmp_listener;
    auto const* sink = listener.get_stream(descriptor_index);
    if (sink == nullptr) {
        return false;
    }

    uint32_t flags = 0;

    if (auto const desc = host.get_descriptor(DESCRIPTOR_STREAM_INPUT, descriptor_index); desc.has_value()) {
        atdecc::aem::DescriptorStream stream_desc{};
        span_load_padded(stream_desc, *desc);
        span_copy(make_span(out.stream_format), stream_desc.current_format.span());
        flags |= stream_info_flags::STREAM_FORMAT_VALID;
    }

    // A connected sink knows the talker's stream identity.
    if (sink->connected) {
        flags |= stream_info_flags::CONNECTED | stream_info_flags::STREAM_ID_VALID | stream_info_flags::STREAM_DEST_MAC_VALID;
        out.stream_id = sink->stream_id;
        out.stream_dest_mac = sink->stream_dest_mac;
        if (sink->stream_vlan_id != 0) {
            flags |= stream_info_flags::STREAM_VLAN_ID_VALID;
            out.stream_vlan_id = ieee::doublet_t{sink->stream_vlan_id};
        }
    }

    if (milan) {
        auto const goals = listener.fast_connect_goals();
        bool const goal =
            std::ranges::any_of(goals, [descriptor_index](auto const& g) { return g.listener_unique_id == descriptor_index; });
        bool const bound = sink->connected || goal;
        if (bound) {
            flags |= stream_info_flags::BOUND | stream_info_flags::FAST_CONNECT | stream_info_flags::SAVED_STATE;
        }
    }
    if (sink->settled()) {
        flags |=
            stream_info_flags::STREAM_ID_VALID | stream_info_flags::STREAM_DEST_MAC_VALID | stream_info_flags::STREAM_VLAN_ID_VALID;
        out.stream_id = sink->stream.stream_id;
        out.stream_dest_mac = sink->stream.dest_mac;
        out.stream_vlan_id = ieee::doublet_t{sink->stream.vlan_id};
    }
    if (sink->registering()) {
        statusbar::tsn::StreamId sid{};
        (void)statusbar::tsn::load_unchecked(sink->stream.stream_id.span(), &sid);
        auto const& msrp = host.components().msrp_handler;
        flags |= stream_info_flags::MSRP_ACC_LAT_VALID;
        if (sink->registration == MilanTalkerRegistration::failed) {
            flags |= stream_info_flags::REGISTERING_FAILED | stream_info_flags::MSRP_FAILURE_VALID;
            if (auto const* failed = msrp.peer_talker_failed(sid); failed != nullptr) {
                out.msrp_failure_code = failed->failure_code;
                out.msrp_failure_bridge_id = failed->failure_bridge_id;
                out.msrp_accumulated_latency = failed->advertise.accumulated_latency;
            } else if (auto const* advertise = msrp.registered_talker_advertise(sid); advertise != nullptr) {
                registering = true;
                out.msrp_accumulated_latency = advertise->accumulated_latency;
            }
        }
        if (registering) {
            flags |= stream_info_flags::MSRP_ACC_LAT_VALID;
        }
        ext.flags_ex = ieee::quadlet_t{registering ? atdecc::aem::stream_info_flags_ex::REGISTERING : 0U};
    }
    out.flags = ieee::quadlet_t{flags};
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
    auto const* c = talker.tx_counters(descriptor_index);
    if (c == nullptr) {
        return {};
    }
    auto const relaxed = std::memory_order_relaxed;
    return {.pdus = c->pdus.load(relaxed), .tu_pdus = c->tu_pdus.load(relaxed), .mr_toggles = c->mr_toggles.load(relaxed)};
}

/// Fill the GET_COUNTERS bitmap for a STREAM_OUTPUT index: Milan v1.2 Section
/// 5.4.2.25 Table 5.17, or classic IEEE 1722.1-2021 Clause 7.4.42.2.5 totals.
/// False when the talker has no such stream.
[[nodiscard]] inline auto fill_talker_stream_counters(
    TalkerStreams const& talker,
    uint16_t const descriptor_index,
    StreamOutputIntervals const& intervals,
    bool const milan,
    uint32_t& valid,
    std::array<uint32_t, 32>& out) -> bool
{
    if (talker.slot_for(descriptor_index) == nullptr) {
        return false;
    }
    using I = StreamOutputIntervals;
    if (milan) {
        valid |= I::VALID;
        std::copy(intervals.values.begin(), intervals.values.end(), out.begin());
        return true;
    }
    auto const totals = talker_stream_totals(talker, descriptor_index);
    valid |= (1U << 0U) | (1U << 1U) | (1U << 3U) | (1U << 4U) | (1U << stream_output_frames_tx_bit);
    out[0] = intervals.values[I::STREAM_START];
    out[1] = intervals.values[I::STREAM_STOP];
    out[3] = totals.mr_toggles;
    out[4] = totals.tu_pdus;
    out[stream_output_frames_tx_bit] = totals.pdus;
    return true;
}

}  // namespace statusbar::avb_entity
