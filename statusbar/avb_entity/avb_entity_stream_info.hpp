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

#include <array>
#include <cstdint>

namespace statusbar::avb_entity {

/// Fill the GET_STREAM_INFO response for a STREAM_OUTPUT (talker) index
/// from the live ACMP stream identity + the descriptor's current_format
/// (true if it is one of the host's talker streams). A Milan listener
/// queries this to verify the stream before sustaining a connection.
[[nodiscard]] inline auto fill_talker_stream_info(
    AvbEntityHost const& host,
    uint16_t const descriptor_index,
    uint64_t const presentation_offset_ns,
    atdecc::aem::AemStreamInfoPayload& out) -> bool
{
    using atdecc::aem::DESCRIPTOR_STREAM_OUTPUT;
    namespace stream_info_flags = atdecc::aem::stream_info_flags;

    auto const* stream = host.components().acmp_talker.get_stream(descriptor_index);
    if (stream == nullptr) {
        return false;
    }

    uint32_t flags = stream_info_flags::STREAM_ID_VALID | stream_info_flags::STREAM_DEST_MAC_VALID |
        stream_info_flags::STREAM_VLAN_ID_VALID | stream_info_flags::MSRP_ACC_LAT_VALID;

    // Stream format from the STREAM_OUTPUT descriptor's current_format,
    // read from the blob via the symbol-aware host.
    if (auto const desc = host.get_descriptor(DESCRIPTOR_STREAM_OUTPUT, descriptor_index); desc.has_value()) {
        atdecc::aem::DescriptorStream stream_desc{};
        span_load_padded(stream_desc, *desc);
        span_copy(make_span(out.stream_format), stream_desc.current_format.span());
        flags |= stream_info_flags::STREAM_FORMAT_VALID;
    }

    out.stream_id = stream->stream_id;
    span_copy(make_span(out.stream_dest_mac), stream->stream_dest_mac.span());
    out.stream_vlan_id = ieee::doublet_t{stream->stream_vlan_id};
    out.msrp_accumulated_latency = ieee::quadlet_t{static_cast<uint32_t>(presentation_offset_ns)};

    // SR class A is the entities' only class, so CLASS_B stays clear. Report
    // the live ACMP connection state so a controller/listener sees CONNECTED.
    if (host.components().acmp_talker.connection_count(descriptor_index) > 0) {
        flags |= stream_info_flags::CONNECTED;
    }
    out.flags = ieee::quadlet_t{flags};
    return true;
}

/// Fill the GET_STREAM_INFO response for a STREAM_INPUT (listener) index
/// from the ACMP listener sink state: the connected talker's stream_id /
/// dest MAC when bound, plus the descriptor's current_format — so a
/// controller can read what an input is connected to.
[[nodiscard]] inline auto fill_listener_stream_info(
    AvbEntityHost const& host, uint16_t const descriptor_index, atdecc::aem::AemStreamInfoPayload& out) -> bool
{
    using atdecc::aem::DESCRIPTOR_STREAM_INPUT;
    namespace stream_info_flags = atdecc::aem::stream_info_flags;

    auto const* sink = host.components().acmp_listener.get_stream(descriptor_index);
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
        span_copy(make_span(out.stream_dest_mac), sink->stream_dest_mac.span());
        if (sink->stream_vlan_id != 0) {
            flags |= stream_info_flags::STREAM_VLAN_ID_VALID;
            out.stream_vlan_id = ieee::doublet_t{sink->stream_vlan_id};
        }
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
