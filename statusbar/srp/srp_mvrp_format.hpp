#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/srp/srp_mrp.hpp"
#include "statusbar/srp/srp_mrp_format.hpp"
#include "statusbar/srp/srp_mvrp.hpp"

#include <cstdint>
#include <format>
#include <span>

namespace statusbar::srp::mvrp {

/// Parse and format one MVRP VectorAttribute, advancing pos
/// @return output iterator after formatting, or nullopt if truncated
template <typename OutputIt>
auto format_mvrp_vector(OutputIt out, std::span<uint8_t const> payload, size_t& pos, AttributeType attr_type, uint8_t attr_length)
    -> std::optional<OutputIt>
{
    using namespace statusbar::srp::mrp;

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
        return std::format_to(out, "    MVRP truncated at FirstValue\n");
    }

    if (attr_type == AttributeType::VlanIdentifier && attr_length >= VlanIdentifierFirstValue::LENGTH) {
        VlanIdentifierFirstValue vlan;
        (void)load_unchecked(payload.subspan(pos), &vlan);
        out = std::format_to(out, "      VlanIdentifier: vid={}\n", vlan.get_vid());
    } else {
        out = std::format_to(out, "      Unknown attribute type\n");
    }
    pos += attr_length;

    size_t const num_event_octets = threepacked_octet_count(num_values);
    if (pos + num_event_octets > payload.size()) {
        return std::format_to(out, "    MVRP truncated at events\n");
    }

    if (num_values > 0) {
        out = format_mrp_events(out, num_values, payload.subspan(pos, num_event_octets), false);
    }
    pos += num_event_octets;

    return out;
}

/// Format an MVRP payload to an output iterator
template <typename OutputIt>
auto format_mvrp(OutputIt out, std::span<uint8_t const> payload) -> OutputIt
{
    using namespace statusbar::srp::mrp;

    if (payload.empty()) {
        return std::format_to(out, "  MVRP payload empty\n");
    }

    // Protocol version (1 byte)
    uint8_t const version = payload[0];
    out = std::format_to(out, "  MVRP version={}\n", version);

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
            return std::format_to(out, "  MVRP truncated at AttributeListHeader\n");
        }

        AttributeListHeader attr_list_hdr;
        (void)load_unchecked(payload.subspan(pos), &attr_list_hdr);
        pos += AttributeListHeader::LENGTH;

        auto const attr_type = static_cast<AttributeType>(attr_list_hdr.attribute_type.get());
        uint8_t const attr_length = attr_list_hdr.attribute_length.get();

        out = std::format_to(
            out, "  Message: type={} ({}) length={}\n", attribute_type_name(attr_type), static_cast<int>(attr_type), attr_length);

        // Parse VectorAttributes until EndMark
        while (pos + 2 <= payload.size()) {
            uint16_t const vec_end_check = mrp::read_doublet_at(payload, pos);
            if (vec_end_check == END_MARK) {
                pos += 2;
                break;
            }

            auto result = format_mvrp_vector(out, payload, pos, attr_type, attr_length);
            if (!result) {
                return std::format_to(out, "    MVRP truncated at VectorAttributeHeader\n");
            }
            out = *result;
        }
    }

    return out;
}

}  // namespace statusbar::srp::mvrp
