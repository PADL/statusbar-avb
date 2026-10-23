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
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/srp/srp_msrp.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace statusbar::avb_entity {

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

/// Fill the GET_COUNTERS bitmap for a STREAM_OUTPUT index: FRAMES_TX
/// (IEEE 1722.1 Clause 7.4.43 bit 6) from the talker slot's TX packet
/// count — what tells a reader our actual transmit rate. The tx counter
/// is single-threaded with the media timer, read plain.
[[nodiscard]] inline auto fill_talker_stream_counters(
    TalkerStreams const& talker, uint16_t const descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& out) -> bool
{
    auto const* slot = talker.slot_for(descriptor_index);
    if (slot == nullptr) {
        return false;
    }
    valid |= (1U << 6U);
    out[6] = static_cast<uint32_t>(slot->tx_packets);
    return true;
}

}  // namespace statusbar::avb_entity
