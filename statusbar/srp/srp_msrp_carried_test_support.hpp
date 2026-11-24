#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// For tests that look at what an MSRP participant put on the wire: the
/// Listener values of an MSRPDU, each with the event and the Declaration
/// Type the sender attached to it.

#include "statusbar/srp/srp_mrp.hpp"
#include "statusbar/srp/srp_msrp.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace statusbar::srp::msrp::test_support {

/// One Listener value as a PDU carried it: the unique_id of its StreamID,
/// its three-packed AttributeEvent and its four-packed Declaration Type.
struct CarriedListener
{
    uint16_t unique_id{0};
    mrp::AttributeEvent event{};
    ListenerDeclaration declaration{ListenerDeclaration::Ignore};
};

/// The octets of one Listener vector after its header: the FirstValue, the
/// ThreePackedEvents and the FourPackedEvents of @p num_values values.
[[nodiscard]] inline auto listener_vector_size(uint8_t const attr_len, uint16_t const num_values) noexcept -> size_t
{
    return attr_len + ((static_cast<size_t>(num_values) + 2) / 3) + ((static_cast<size_t>(num_values) + 3) / 4);
}

/// The values of one Listener vector, @p vector starting at its FirstValue.
inline void append_listener_vector(
    std::vector<CarriedListener>& out, std::span<uint8_t const> const vector, uint8_t const attr_len, uint16_t const num_values)
{
    auto const base_uid = static_cast<uint16_t>((static_cast<uint16_t>(vector[6]) << 8) | vector[7]);
    auto const events = vector.subspan(attr_len, (static_cast<size_t>(num_values) + 2) / 3);
    auto const declarations = vector.subspan(attr_len + events.size());
    for (uint16_t i = 0; i < num_values; ++i) {
        auto const declaration = mrp::nth_of_four(mrp::unpack4_declarations(declarations[i / 4]), i);
        out.push_back(
            {.unique_id = static_cast<uint16_t>(base_uid + i),
             .event = mrp::nth_of_three(mrp::unpack3_events(events[i / 3]), i),
             .declaration = static_cast<ListenerDeclaration>(declaration)});
    }
}

/// The values of one Listener message, @p list being its AttributeList. A
/// vector with no values (a LeaveAll with nothing to carry) has no FirstValue.
inline void append_listener_message(std::vector<CarriedListener>& out, std::span<uint8_t const> const list, uint8_t const attr_len)
{
    size_t at = 0;
    while (at + 2 <= list.size() && (list[at] != 0 || list[at + 1] != 0)) {
        auto const num_values = static_cast<uint16_t>(((static_cast<uint16_t>(list[at]) << 8) | list[at + 1]) & 0x1FFF);
        size_t const size = (num_values == 0) ? 0 : listener_vector_size(attr_len, num_values);
        if (at + 2 + size > list.size()) {
            break;
        }
        if (num_values != 0) {
            append_listener_vector(out, list.subspan(at + 2, size), attr_len, num_values);
        }
        at += 2 + size;
    }
}

/// Every Listener value the MSRPDU @p pdu carries, in wire order.
[[nodiscard]] inline auto listener_carried(std::span<uint8_t const> const pdu) -> std::vector<CarriedListener>
{
    std::vector<CarriedListener> out{};
    size_t at = pdu.empty() ? pdu.size() : 1;  // past the ProtocolVersion
    while (at + 4 <= pdu.size() && pdu[at] != 0) {
        size_t const list_end = at + 4 + ((static_cast<size_t>(pdu[at + 2]) << 8) | pdu[at + 3]);
        if (list_end > pdu.size()) {
            break;
        }
        if (pdu[at] == static_cast<uint8_t>(AttributeType::Listener)) {
            append_listener_message(out, pdu.subspan(at + 4, list_end - at - 4), pdu[at + 1]);
        }
        at = list_end;
    }
    return out;
}

/// Every Listener value a run of MSRPDUs carries, in the order they went out.
[[nodiscard]] inline auto listener_carried(std::vector<std::vector<uint8_t>> const& pdus) -> std::vector<CarriedListener>
{
    std::vector<CarriedListener> out{};
    for (auto const& pdu : pdus) {
        auto const carried = listener_carried(std::span<uint8_t const>(pdu));
        out.insert(out.end(), carried.begin(), carried.end());
    }
    return out;
}

}  // namespace statusbar::srp::msrp::test_support
