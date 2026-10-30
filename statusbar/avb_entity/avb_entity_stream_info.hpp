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
#include "statusbar/srp/srp_msrp.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace statusbar::avb_entity {

/// What GET_STREAM_INFO reports of a talker stream beyond its ACMP identity:
/// the entity's profile and the stream's MSRP and Milan state.
struct TalkerStreamStatus
{
    bool milan{true};
    /// The response form, which gives msrp_accumulated_latency its meaning.
    atdecc::aem::StreamInfoLayout layout{atdecc::aem::StreamInfoLayout::milan_v1_2};
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
};

/// MSRP_ACC_LAT_VALID for an output with @p status: Milan always (v1.3 Table 5.9); classic in
/// the IEEE 1722.1-2021 layout while it declares the attribute the latency is (Clause 7.4.16.2).
[[nodiscard]] constexpr auto talker_accumulated_latency_flag(TalkerStreamStatus const& status) noexcept -> uint32_t
{
    bool const valid = status.milan || status.declaring || status.layout != atdecc::aem::StreamInfoLayout::ieee_2021;
    return valid ? atdecc::aem::stream_info_flags::MSRP_ACC_LAT_VALID : 0U;
}

/// An output's msrp_accumulated_latency in @p status's layout: the declared AccumulatedLatency
/// in IEEE 1722.1-2021's (Clause 7.4.16.2), the presentation time offset in the older ones.
[[nodiscard]] constexpr auto talker_msrp_accumulated_latency(TalkerStreamStatus const& status) noexcept -> uint32_t
{
    bool const declared = status.layout == atdecc::aem::StreamInfoLayout::ieee_2021;
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

/// The flags of a Milan output with @p status (Milan v1.2 Tables 5.11-5.12, or v1.3 Table
/// 5.9 in the IEEE 1722.1-2021 layout); in Milan v1.2's, flags_ex and pbsta into @p ext.
[[nodiscard]] constexpr auto milan_talker_flags(
    TalkerStreamStatus const& status, atdecc::aem::AemStreamInfoMilanExtension& ext) noexcept -> uint32_t
{
    if (status.layout == atdecc::aem::StreamInfoLayout::ieee_2021) {
        return milan_v13_talker_srp_flags(status);
    }
    bool const registering = status.declaring && status.listener != srp::msrp::ListenerDeclaration::Ignore;
    ext.flags_ex = ieee::quadlet_t{registering ? atdecc::aem::stream_info_flags_ex::REGISTERING : 0U};
    ext.set_probing(atdecc::aem::probing_status::DISABLED, 0);
    bool const failed = registering && status.listener == srp::msrp::ListenerDeclaration::AskingFailed;
    return failed ? atdecc::aem::stream_info_flags::REGISTERING_FAILED : 0U;
}

/// The flags of classic output @p descriptor_index: CONNECTED from the ACMP connection count.
/// SR class A is the entities' only class, so CLASS_B stays clear.
[[nodiscard]] inline auto classic_talker_flags(AvbEntityHost const& host, uint16_t const descriptor_index) -> uint32_t
{
    bool const connected = host.components().acmp_talker.connection_count(descriptor_index) > 0;
    return connected ? atdecc::aem::stream_info_flags::CONNECTED : 0U;
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
template <size_t N>
[[nodiscard]] inline auto fill_talker_identity(
    atdecc::TalkerStreamInfoDynamic<N> const& stream, TalkerStreamStatus const& status, atdecc::aem::AemStreamInfoPayload& out)
    -> uint32_t
{
    using namespace atdecc::aem::stream_info_flags;
    out.stream_id = stream.stream_id;
    out.stream_dest_mac = stream.stream_dest_mac;
    out.stream_vlan_id = ieee::doublet_t{stream.stream_vlan_id};
    out.msrp_accumulated_latency = ieee::quadlet_t{talker_msrp_accumulated_latency(status)};
    return (stream.stream_dest_mac != ieee::Eui48{} ? STREAM_DEST_MAC_VALID : 0U) | STREAM_ID_VALID | STREAM_VLAN_ID_VALID;
}

/// Fill the GET_STREAM_INFO response for STREAM_OUTPUT @p index from the live ACMP
/// stream identity and current_format (Milan v1.2 Section 5.4.2.10.2, v1.3 in the 2021
/// layout; flag rules in docs/AVB_ENTITY_KIT.md). False if it is not one of the host's talkers.
[[nodiscard]] inline auto fill_talker_stream_info(
    AvbEntityHost const& host,
    uint16_t const descriptor_index,
    TalkerStreamStatus const& status,
    atdecc::aem::AemStreamInfoPayload& out,
    atdecc::aem::AemStreamInfoMilanExtension& ext) -> bool
{
    auto const* stream = host.components().acmp_talker.get_stream(descriptor_index);
    if (stream == nullptr) {
        return false;
    }
    uint32_t const flags = fill_talker_identity(*stream, status, out) | fill_talker_format(host, descriptor_index, out) |
        talker_accumulated_latency_flag(status);
    out.flags =
        ieee::quadlet_t{flags | (status.milan ? milan_talker_flags(status, ext) : classic_talker_flags(host, descriptor_index))};
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
/// when connected, plus current_format.
[[nodiscard]] inline auto fill_listener_stream_info(
    AvbEntityHost const& host, uint16_t const descriptor_index, atdecc::aem::AemStreamInfoPayload& out) -> bool
{
    namespace stream_info_flags = atdecc::aem::stream_info_flags;

    auto const* sink = host.components().acmp_listener.get_stream(descriptor_index);
    if (sink == nullptr) {
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
    out.flags = ieee::quadlet_t{flags};
    return true;
}

/// The MSRP fields of a registering Milan input's GET_STREAM_INFO from the talker's
/// declaration (Milan v1.2 Table 5.9); the flags they make valid.
[[nodiscard]] inline auto fill_milan_registration(
    AvbEntityHost const& host, atdecc::MilanSink const& sink, atdecc::aem::AemStreamInfoPayload& out) -> uint32_t
{
    namespace stream_info_flags = atdecc::aem::stream_info_flags;
    statusbar::tsn::StreamId sid{};
    (void)statusbar::tsn::load_unchecked(sink.stream.stream_id.span(), &sid);
    if (sink.registration == atdecc::MilanTalkerRegistration::failed) {
        if (auto const* failed = host.components().msrp_handler.peer_talker_failed(sid); failed != nullptr) {
            out.msrp_failure_code = failed->failure_code;
            out.msrp_failure_bridge_id = failed->failure_bridge_id;
            out.msrp_accumulated_latency = failed->advertise.accumulated_latency;
        }
        return stream_info_flags::MSRP_ACC_LAT_VALID | stream_info_flags::REGISTERING_FAILED |
            stream_info_flags::MSRP_FAILURE_VALID;
    }
    if (auto const* advertise = host.components().msrp_handler.peer_talker_advertise(sid); advertise != nullptr) {
        out.msrp_accumulated_latency = advertise->accumulated_latency;
    }
    return stream_info_flags::MSRP_ACC_LAT_VALID;
}

/// The fields of a Milan input's GET_STREAM_INFO that its format, binding, stream and
/// registration give (Milan v1.2 Table 5.9); their flags.
[[nodiscard]] inline auto fill_milan_listener_fields(
    AvbEntityHost const& host,
    uint16_t const descriptor_index,
    atdecc::MilanSink const& sink,
    atdecc::aem::AemStreamInfoPayload& out) -> uint32_t
{
    namespace stream_info_flags = atdecc::aem::stream_info_flags;
    uint32_t flags = fill_stream_input_format(host, descriptor_index, out) ? stream_info_flags::STREAM_FORMAT_VALID : 0U;
    if (sink.bound()) {
        flags |= stream_info_flags::BOUND | stream_info_flags::FAST_CONNECT | stream_info_flags::SAVED_STATE;
        if (!sink.binding.started) {
            flags |= stream_info_flags::STREAMING_WAIT;
        }
    }
    if (sink.settled()) {
        flags |=
            stream_info_flags::STREAM_ID_VALID | stream_info_flags::STREAM_DEST_MAC_VALID | stream_info_flags::STREAM_VLAN_ID_VALID;
        out.stream_id = sink.stream.stream_id;
        out.stream_dest_mac = sink.stream.dest_mac;
        out.stream_vlan_id = ieee::doublet_t{sink.stream.vlan_id};
    }
    if (sink.registering()) {
        flags |= fill_milan_registration(host, sink, out);
    }
    return flags;
}

/// Fill the Milan GET_STREAM_INFO response for a STREAM_INPUT from its sink state
/// machine (Milan v1.2 Section 5.4.2.10.1, Tables 5.9-5.10; docs/AVB_ENTITY_KIT.md).
/// False when the input or its sink does not exist.
[[nodiscard]] inline auto fill_milan_listener_stream_info(
    AvbEntityHost const& host,
    uint16_t const descriptor_index,
    atdecc::aem::AemStreamInfoPayload& out,
    atdecc::aem::AemStreamInfoMilanExtension& ext) -> bool
{
    auto const* sink = host.components().acmp_listener.milan_sinks().sink(descriptor_index);
    if (sink == nullptr) {
        return false;
    }
    out.flags = ieee::quadlet_t{fill_milan_listener_fields(host, descriptor_index, *sink, out)};
    ext.flags_ex = ieee::quadlet_t{sink->registering() ? atdecc::aem::stream_info_flags_ex::REGISTERING : 0U};
    ext.set_probing(sink->probing_status, sink->acmp_status);
    return true;
}

/// A STREAM_OUTPUT's transmit totals (TalkerTxCounters) as StreamOutputTotals.
[[nodiscard]] inline auto talker_stream_totals(TalkerStreams const& talker, uint16_t const descriptor_index) -> StreamOutputTotals
{
    auto const* c = talker.tx_counters(descriptor_index);
    if (c == nullptr) {
        return {};
    }
    auto const relaxed = std::memory_order_relaxed;
    return {.pdus = c->pdus.load(relaxed), .tu_pdus = c->tu_pdus.load(relaxed), .mr_toggles = c->mr_toggles.load(relaxed)};
}

/// The classic GET_COUNTERS totals of STREAM_OUTPUT @p descriptor_index
/// (IEEE 1722.1-2021 Clause 7.4.42.2.5) into @p out, their bits into @p valid.
inline void fill_classic_talker_counters(
    TalkerStreams const& talker,
    uint16_t const descriptor_index,
    StreamOutputIntervals const& intervals,
    uint32_t& valid,
    std::array<uint32_t, 32>& out)
{  // NOLINT(statusbar-block-statements): one line per counter of IEEE 1722.1-2021 Clause 7.4.42.2.5
    using I = StreamOutputIntervals;
    auto const totals = talker_stream_totals(talker, descriptor_index);
    valid |= (1U << 0U) | (1U << 1U) | (1U << 3U) | (1U << 4U) | (1U << 7U);
    out[0] = intervals.values[I::STREAM_START];
    out[1] = intervals.values[I::STREAM_STOP];
    out[3] = totals.mr_toggles;
    out[4] = totals.tu_pdus;
    out[7] = totals.pdus;
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
    if (milan) {
        valid |= StreamOutputIntervals::VALID;
        std::copy(intervals.values.begin(), intervals.values.end(), out.begin());
        return true;
    }
    fill_classic_talker_counters(talker, descriptor_index, intervals, valid, out);
    return true;
}

}  // namespace statusbar::avb_entity
