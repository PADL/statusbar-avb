#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/ieee/ieee_ethernet_format.hpp"
#include "statusbar/srp/srp_mrp.hpp"
#include "statusbar/srp/srp_mrp_format.hpp"
#include "statusbar/srp/srp_msrp.hpp"
#include "statusbar/tsn/tsn_stream_id.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <span>
#include <string>

namespace statusbar::srp::msrp::detail {

/// Format a tsn::StreamId to an output iterator (local helper to avoid avtp dependency)
template <typename OutputIt>
auto format_stream_id(OutputIt out, tsn::StreamId const& sid) -> OutputIt
{
    out = ieee::format_to(out, sid.get_system_address());
    out = std::format_to(out, ":{:04x}", sid.get_unique_id());
    return out;
}

}  // namespace statusbar::srp::msrp::detail

namespace statusbar::srp::msrp {

/// Format an MSRP FirstValue based on attribute type
template <typename OutputIt>
auto format_first_value(OutputIt out, AttributeType attr_type, std::span<uint8_t const> data, uint8_t attr_length) -> OutputIt
{
    using namespace mrp;

    switch (attr_type) {
        case AttributeType::Domain: {
            if (attr_length >= DomainFirstValue::LENGTH) {
                DomainFirstValue domain;
                (void)load_unchecked(data, &domain);
                out = std::format_to(
                    out,
                    "      Domain: sr_class={} priority={} vid={}\n",
                    static_cast<int>(domain.sr_class_id.get()),
                    static_cast<int>(domain.sr_class_priority.get()),
                    domain.sr_class_vid.get());
            }
            break;
        }
        case AttributeType::Listener: {
            if (attr_length >= ListenerFirstValue::LENGTH) {
                ListenerFirstValue listener;
                (void)load_unchecked(data, &listener);
                std::string stream_str;
                detail::format_stream_id(std::back_inserter(stream_str), listener.stream_id);
                out = std::format_to(out, "      Listener: stream_id={}\n", stream_str);
            }
            break;
        }
        case AttributeType::TalkerAdvertise: {
            if (attr_length >= TalkerAdvertiseFirstValue::LENGTH) {
                TalkerAdvertiseFirstValue talker;
                (void)load_unchecked(data, &talker);
                std::string stream_str;
                detail::format_stream_id(std::back_inserter(stream_str), talker.stream_id);
                std::string dest_str;
                ieee::format_to(std::back_inserter(dest_str), talker.destination_address);
                out = std::format_to(
                    out,
                    "      TalkerAdvertise: stream_id={} dest={} vid={} max_frame={} max_interval={} priority={} rank={} "
                    "latency={}\n",
                    stream_str,
                    dest_str,
                    talker.vlan_identifier.get(),
                    talker.max_frame_size.get(),
                    talker.max_interval_frames.get(),
                    talker.get_priority(),
                    talker.get_rank(),
                    talker.accumulated_latency.get());
            }
            break;
        }
        case AttributeType::TalkerFailed: {
            if (attr_length >= TalkerFailedFirstValue::LENGTH) {
                TalkerFailedFirstValue talker;
                (void)load_unchecked(data, &talker);
                std::string stream_str;
                detail::format_stream_id(std::back_inserter(stream_str), talker.advertise.stream_id);
                std::string dest_str;
                ieee::format_to(std::back_inserter(dest_str), talker.advertise.destination_address);
                std::string bridge_str;
                ieee::format_to(std::back_inserter(bridge_str), talker.failure_bridge_id);
                out = std::format_to(
                    out,
                    "      TalkerFailed: stream_id={} dest={} vid={} failure_bridge={} failure_code={} ({})\n",
                    stream_str,
                    dest_str,
                    talker.advertise.vlan_identifier.get(),
                    bridge_str,
                    failure_code_name(talker.get_failure_code()),
                    static_cast<int>(talker.failure_code.get()));
            }
            break;
        }
        default:
            out = std::format_to(out, "      Unknown attribute type\n");
            break;
    }
    return out;
}

/// Parse and format one MSRP VectorAttribute, advancing pos
/// @return output iterator after formatting, or nullopt if truncated
template <typename OutputIt>
auto format_msrp_vector(OutputIt out, std::span<uint8_t const> payload, size_t& pos, AttributeType attr_type, uint8_t attr_length)
    -> std::optional<OutputIt>
{
    using namespace mrp;

    if (pos + VectorAttributeHeader::LENGTH > payload.size()) {
        return std::nullopt;
    }

    VectorAttributeHeader vec_hdr;
    (void)load_unchecked(payload.subspan(pos), &vec_hdr);
    pos += VectorAttributeHeader::LENGTH;

    bool const leave_all = vec_hdr.get_leave_all();
    uint16_t const num_values = vec_hdr.get_number_of_values();

    out = std::format_to(out, "    Vector: leave_all={} num_values={}\n", leave_all, num_values);

    if (pos + attr_length > payload.size()) {
        return std::format_to(out, "    MSRP truncated at FirstValue\n");
    }

    out = format_first_value(out, attr_type, payload.subspan(pos), attr_length);
    pos += attr_length;

    size_t const num_event_octets = threepacked_octet_count(num_values);
    bool const has_declarations = (attr_type == AttributeType::Listener);
    size_t const num_decl_octets = has_declarations ? fourpacked_octet_count(num_values) : 0;
    size_t const total_event_bytes = num_event_octets + num_decl_octets;

    if (pos + total_event_bytes > payload.size()) {
        return std::format_to(out, "    MSRP truncated at events\n");
    }

    if (num_values > 0) {
        out = format_mrp_events(out, num_values, payload.subspan(pos, total_event_bytes), has_declarations);
    }
    pos += total_event_bytes;

    return out;
}

/// Format an MSRP payload to an output iterator
template <typename OutputIt>
auto format_msrp(OutputIt out, std::span<uint8_t const> payload) -> OutputIt
{
    using namespace mrp;

    if (payload.empty()) {
        return std::format_to(out, "  MSRP payload empty\n");
    }

    // Protocol version (1 byte)
    uint8_t const version = payload[0];
    out = std::format_to(out, "  MSRP version={}\n", version);

    size_t pos = 1;

    // Parse messages until we hit the final EndMark
    while (pos + 2 <= payload.size()) {
        // Check for EndMark (end of all messages)
        uint16_t const end_check = mrp::read_doublet_at(payload, pos);
        if (end_check == END_MARK) {
            out = std::format_to(out, "  [EndMark]\n");
            break;
        }

        // AttributeListHeader (2 bytes)
        if (pos + AttributeListHeader::LENGTH > payload.size()) {
            return std::format_to(out, "  MSRP truncated at AttributeListHeader\n");
        }

        if (pos + AttributeListHeader::LENGTH + 2 > payload.size()) {
            return std::format_to(out, "  MSRP truncated at AttributeListHeader\n");
        }

        AttributeListHeader attr_list_hdr;
        (void)load_unchecked(payload.subspan(pos), &attr_list_hdr);
        pos += AttributeListHeader::LENGTH;

        // AttributeListLength (IEEE 802.1Q-2014 Clause 10.8.2.3): octet length of
        // the AttributeList (vectors + trailing EndMark) that follows. It sits
        // between AttributeLength and the first VectorHeader; skipping it shifts
        // every subsequent field by two bytes and turns the whole decode to
        // garbage. Consume it and use it to bound this message's vector walk.
        uint16_t const attr_list_length = mrp::read_doublet_at(payload, pos);
        pos += 2;
        size_t const attr_list_end = std::min(pos + attr_list_length, payload.size());

        auto const attr_type = static_cast<AttributeType>(attr_list_hdr.attribute_type.get());
        uint8_t const attr_length = attr_list_hdr.attribute_length.get();

        out = std::format_to(
            out, "  Message: type={} ({}) length={}\n", attribute_type_name(attr_type), static_cast<int>(attr_type), attr_length);

        // Parse VectorAttributes until EndMark (bounded by AttributeListLength)
        while (pos + 2 <= attr_list_end) {
            uint16_t const vec_end_check = mrp::read_doublet_at(payload, pos);
            if (vec_end_check == END_MARK) {
                pos += 2;
                break;
            }

            auto result = format_msrp_vector(out, payload, pos, attr_type, attr_length);
            if (!result) {
                return std::format_to(out, "    MSRP truncated at VectorAttributeHeader\n");
            }
            out = *result;
        }

        // Realign to the message boundary the sender declared, so an under- or
        // over-reading vector decode can't desync the outer message walk.
        pos = attr_list_end;
    }

    return out;
}

}  // namespace statusbar::srp::msrp
