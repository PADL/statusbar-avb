// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_srp.hpp"

#include "statusbar/srp/srp_msrp.hpp"
#include "statusbar/srp/srp_msrp_participant.hpp"
#include "statusbar/srp/srp_mvrp.hpp"
#include "statusbar/srp/srp_mvrp_participant.hpp"

#include <algorithm>
#include <cstdint>
#include <span>

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

namespace {

[[nodiscard]] auto holds(std::span<uint16_t const> const vlans, uint16_t const vlan_id) noexcept -> bool
{
    return std::ranges::find(vlans, vlan_id) != vlans.end();
}

}  // namespace

auto MvrpHandler::register_vlan(uint16_t const vlan_id, TimePoint const now) -> Status
{
    auto declared = declare(vlan_id, now);
    if (declared.has_value() && !holds(entity_vlans_, vlan_id)) {
        (void)entity_vlans_.try_push_back(vlan_id);
    }
    return declared;
}

auto MvrpHandler::withdraw_vlan(uint16_t const vlan_id, TimePoint const now) -> Status
{
    if (auto* const held = std::ranges::find(entity_vlans_, vlan_id); held != entity_vlans_.end()) {
        entity_vlans_.erase(held);
    }
    // Milan v1.3 4.4.1: a settled sink on the VLAN keeps it declared.
    if (holds(sink_vlans_, vlan_id)) {
        return success();
    }
    return undeclare(vlan_id, now);
}

void MvrpHandler::set_sink_vlans(std::span<uint16_t const> const vlans, TimePoint const now)
{
    VlanIds next{};
    for (uint16_t const vlan_id : vlans) {
        take_sink_vlan(next, vlan_id, now);
    }
    for (uint16_t const vlan_id : sink_vlans_) {
        if (!holds(next, vlan_id) && !holds(entity_vlans_, vlan_id)) {
            (void)undeclare(vlan_id, now);
        }
    }
    sink_vlans_ = next;
}

void MvrpHandler::take_sink_vlan(VlanIds& next, uint16_t const vlan_id, TimePoint const now)
{
    if (holds(next, vlan_id)) {
        return;
    }
    bool const declared = holds(sink_vlans_, vlan_id) || holds(entity_vlans_, vlan_id);
    if (declared || declare(vlan_id, now).has_value()) {
        (void)next.try_push_back(vlan_id);
    }
}

auto MvrpHandler::declare(uint16_t vlan_id, TimePoint now) -> Status
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

auto MvrpHandler::undeclare(uint16_t vlan_id, TimePoint now) -> Status
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

void MvrpHandler::withdraw_all(TimePoint now)
{
    if (!started_) {
        return;
    }
    for (auto& vlan : vlans_) {
        if (vlan.state != VlanState::Unregistered) {
            vlan.state = VlanState::Unregistered;
            (void)participant_.withdraw_vlan(vlan.vlan_id, now);
        }
    }
    participant_.transmit_pending(now);
    // The end of the run: the next use starts the participant again (BEGIN!).
    participant_.stop();
    started_ = false;
    release_holds();
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
}

}  // namespace statusbar::nanoavb
