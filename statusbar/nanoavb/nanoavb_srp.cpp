// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_srp.hpp"

#include "statusbar/srp/srp_msrp.hpp"
#include "statusbar/srp/srp_msrp_participant.hpp"
#include "statusbar/srp/srp_mvrp.hpp"
#include "statusbar/srp/srp_mvrp_participant.hpp"

#include <algorithm>

namespace statusbar::nanoavb {

//
// Construction helpers — ensure the underlying participant is started
// (BEGIN! fired on LeaveAll/Periodic FSMs, timers armed) on first use.
// We do this lazily so simple tests that only check getters without
// calling tick() still behave.
//
// ============================================================
// MvrpHandler
// ============================================================

auto MvrpHandler::register_vlan(uint16_t vlan_id, TimePoint now) -> Status
{
    if (vlan_id == 0 || vlan_id > 4094) {
        return failure(make_error_code(NanoAvbError::InvalidVlanId));
    }

    for (auto& vlan : vlans_) {
        if (vlan.vlan_id == vlan_id) {
            vlan.state = VlanState::Pending;
            if (!started_) {
                participant_.start(now);
                started_ = true;
            }
            (void)participant_.declare_vlan(vlan_id, now);
            return success();
        }
    }

    // New VLAN. vlans_ is fixed-capacity (no heap): take a free slot if one
    // exists, otherwise reclaim a slot held by a withdrawn (Unregistered)
    // VLAN -- a live VLAN never loses its slot to a newcomer. Only when every
    // slot holds an actively-registered VLAN do we reject (effectively
    // unreachable on a real AVB network: typically 1-2 VLANs).
    VlanInfo const rec{.vlan_id = vlan_id, .state = VlanState::Pending};
    if (vlans_.try_push_back(rec) == nullptr) {
        auto* const slot = std::ranges::find(vlans_, VlanState::Unregistered, &VlanInfo::state);
        if (slot == vlans_.end()) {
            return failure(make_error_code(NanoAvbError::VlanTableFull));
        }
        *slot = rec;
    }

    if (!started_) {
        participant_.start(now);
        started_ = true;
    }
    (void)participant_.declare_vlan(vlan_id, now);
    return success();
}

auto MvrpHandler::withdraw_vlan(uint16_t vlan_id, TimePoint now) -> Status
{
    for (auto& vlan : vlans_) {
        if (vlan.vlan_id == vlan_id) {
            vlan.state = VlanState::Unregistered;
            (void)participant_.withdraw_vlan(vlan_id, now);
            return success();
        }
    }
    return success();
}

auto MvrpHandler::get_vlan_state(uint16_t vlan_id) const noexcept -> VlanState
{
    for (auto const& vlan : vlans_) {
        if (vlan.vlan_id == vlan_id) {
            return vlan.state;
        }
    }
    return VlanState::Unregistered;
}

auto MvrpHandler::receive_packet(std::span<uint8_t const> packet, TimePoint now) -> void
{
    // Minimal skeleton validation of the MVRP PDU header.
    if (packet.size() < 3) {
        return;
    }
    if (!started_) {
        participant_.start(now);
        started_ = true;
    }
    participant_.receive_pdu(packet, now);
}

auto MvrpHandler::tick(TimePoint now) -> void
{
    if (!started_) {
        participant_.start(now);
        started_ = true;
    }
    participant_.tick(now);

    // If the protocol engine wants to transmit a PDU, the send_packet
    // callback wraps it and forwards via the nanoavb callback path.
    // We install a one-shot adapter the first time tick/receive runs
    // by configuring the participant's send_pdu callback here.
    // (Done via set_callbacks below; harmless to re-set each tick.)
    if (callbacks_.send_packet) {
        participant_.set_send_pdu(callbacks_.send_packet);
    }
}

}  // namespace statusbar::nanoavb
