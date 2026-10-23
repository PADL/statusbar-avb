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

/// Fill the GET_STREAM_INFO response for a STREAM_INPUT index from the ACMP listener: the
/// connected talker's stream_id and destination, the current format; @p milan adds Milan v1.2
/// Section 5.4.2.10.1's fields, mapped from the classic listener and MSRP.
[[nodiscard]] inline auto fill_listener_stream_info(
    AvbEntityHost const& host,
    uint16_t const descriptor_index,
    bool const milan,
    atdecc::aem::AemStreamInfoPayload& out,
    atdecc::aem::AemStreamInfoMilanExtension& ext) -> bool
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
        ext.set_probing(
            sink->connected ? probing_status::COMPLETED : (bound ? probing_status::ACTIVE : probing_status::DISABLED), 0);
        bool registering = false;
        if (sink->connected) {
            statusbar::tsn::StreamId sid{};
            (void)statusbar::tsn::load_unchecked(sink->stream_id.span(), &sid);
            auto const& msrp = host.components().msrp_handler;
            if (auto const* failed = msrp.registered_talker_failed(sid); failed != nullptr) {
                registering = true;
                flags |= stream_info_flags::REGISTERING_FAILED | stream_info_flags::MSRP_FAILURE_VALID;
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
