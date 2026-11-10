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

/// IEEE 1722.1-2021 Table 7-158: the STREAM_OUTPUT FRAMES_TX counter is bit 7
/// of counters_valid (bits 0-6: STREAM_START, STREAM_STOP, STREAM_INTERRUPTED,
/// MEDIA_RESET, TIMESTAMP_UNCERTAIN, TIMESTAMP_VALID, TIMESTAMP_NOT_VALID).
inline constexpr unsigned stream_output_frames_tx_bit = 7;

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
    auto const* stream = host.components().acmp_talker.get_stream(descriptor_index);
    if (stream == nullptr) {
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

/// Fill the GET_STREAM_INFO response for a STREAM_INPUT (listener) index
/// from the ACMP listener sink state: the connected talker's stream_id /
/// dest MAC when bound, plus the descriptor's current_format — so a
/// controller can read what an input is connected to.
[[nodiscard]] inline auto fill_listener_stream_info(
    AvbEntityHost const& host, uint16_t const descriptor_index, atdecc::aem::AemStreamInfoPayload& out) -> bool
{
    auto const* sink = host.components().acmp_listener.get_stream(descriptor_index);
    if (sink == nullptr) {
        return false;
    }
    uint32_t flags = fill_stream_format(host, atdecc::aem::DESCRIPTOR_STREAM_INPUT, descriptor_index, out)
        ? atdecc::aem::stream_info_flags::STREAM_FORMAT_VALID
        : 0U;
    flags |= copy_connected_sink_identity(*sink, out);
    out.flags = ieee::quadlet_t{flags};
    return true;
}

/// Fill the GET_COUNTERS bitmap for a STREAM_OUTPUT index: FRAMES_TX
/// (IEEE 1722.1-2021 Clause 7.4.42, Table 7-158 bit 7) from the talker
/// slot's TX packet count — what tells a reader our actual transmit rate.
/// The tx counter is single-threaded with the media timer, read plain.
[[nodiscard]] inline auto fill_talker_stream_counters(
    TalkerStreams const& talker, uint16_t const descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& out) -> bool
{
    auto const* slot = talker.slot_for(descriptor_index);
    if (slot == nullptr) {
        return false;
    }
    valid |= (1U << stream_output_frames_tx_bit);
    out[stream_output_frames_tx_bit] = static_cast<uint32_t>(slot->tx_packets);
    return true;
}

}  // namespace statusbar::avb_entity
