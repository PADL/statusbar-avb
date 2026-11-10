#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Test support: crafted MSRPDUs for the tests of MSRP peers (srp, the AVB entity, the AES67
/// SRP port).

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/srp/srp_mrp.hpp"
#include "statusbar/srp/srp_msrp.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace statusbar::srp::msrp {

/// An MSRPDU of one Listener message whose one VectorAttribute carries LeaveAll and no values
/// (IEEE 802.1Q-2022 Clause 10.8.1, Figure 10-5).
struct ListenerLeaveAllPdu
{
    mrp::octet_t protocol_version{};             // 0
    mrp::AttributeListHeader header{};           // 1
    mrp::doublet_t attribute_list_length{};      // 3: the VectorAttribute and the list's EndMark
    mrp::VectorAttributeHeader vector_header{};  // 5
    mrp::doublet_t list_end_mark{};              // 7
    mrp::doublet_t pdu_end_mark{};               // 9
};

static_assert(sizeof(ListenerLeaveAllPdu) == 11);
static_assert(offsetof(ListenerLeaveAllPdu, attribute_list_length) == 3);
static_assert(offsetof(ListenerLeaveAllPdu, vector_header) == 5);
static_assert(offsetof(ListenerLeaveAllPdu, pdu_end_mark) == 9);

/// The Listener LeaveAll re-declaring nothing that a bridge port sends at its LeaveAll once its
/// Listener has left without an Lv.
[[nodiscard]] inline auto listener_leave_all_pdu() noexcept -> std::array<uint8_t, sizeof(ListenerLeaveAllPdu)>
{
    ListenerLeaveAllPdu const fields{
        .protocol_version = PROTOCOL_VERSION,
        .header =
            {.attribute_type = static_cast<uint8_t>(AttributeType::Listener),
             .attribute_length = static_cast<uint8_t>(AttributeLength::Listener)},
        .attribute_list_length = static_cast<uint16_t>(mrp::VectorAttributeHeader::LENGTH + sizeof(mrp::doublet_t)),
        .vector_header = {.vector_header = mrp::calculate_vector_header(true, 0)},
        .list_end_mark = mrp::END_MARK,
        .pdu_end_mark = mrp::END_MARK};
    std::array<uint8_t, sizeof(ListenerLeaveAllPdu)> pdu{};
    span_store(pdu, fields);
    return pdu;
}

}  // namespace statusbar::srp::msrp
