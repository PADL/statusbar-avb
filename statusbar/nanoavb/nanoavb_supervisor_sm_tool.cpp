// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// NanoAVB State Machine Documentation Tool
// Generates DOT (Graphviz) or Markdown documentation for NanoAVB state machines

#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/sm/sm.hpp"

#include <array>
#include <span>
#include <string_view>

using statusbar::sm::make_sm_info;

/// Registry of all NanoAVB state machines
/// To add a new state machine, add a make_sm_info<namespace::Machine>() entry below
constexpr auto state_machines = std::array{
    make_sm_info<statusbar::nanoavb::supervisor_sm::Machine>(
        "supervisor_sm", "Device Supervisor", "(no standard — application-level orchestrator over the protocol SMs below)"),
    make_sm_info<statusbar::nanoavb::gptp_sm::Machine>(
        "gptp_sm", "gPTP Timebase", "IEEE 802.1AS-2020 Clause 10 (minimal slave-port lifecycle)"),
    make_sm_info<statusbar::nanoavb::mvrp_sm::Machine>(
        "mvrp_sm", "MVRP VLAN", "IEEE 802.1Q-2014 Clause 11 (MVRP — Multiple VLAN Registration Protocol)"),
    make_sm_info<statusbar::nanoavb::adp_adv_sm::Machine>(
        "adp_adv_sm", "ADP Advertise", "IEEE 1722.1-2021 Clause 6.2.5 (ADP Advertise / entity-side)"),
    make_sm_info<statusbar::nanoavb::acmp_talker_sm::Machine>(
        "acmp_talker_sm", "ACMP Talker", "IEEE 1722.1-2021 Clause 8.2.4 (minimal entity-side connection-state tracker)"),
    make_sm_info<statusbar::nanoavb::acmp_listener_sm::Machine>(
        "acmp_listener_sm", "ACMP Listener", "IEEE 1722.1-2021 Clause 8.2.5 (minimal entity-side connection-state tracker)"),
    make_sm_info<statusbar::nanoavb::msrp_talker_sm::Machine>(
        "msrp_talker_sm", "MSRP Talker", "IEEE 802.1Q-2014 Clause 35 (MSRP — Multiple Stream Reservation Protocol, minimal)"),
    make_sm_info<statusbar::nanoavb::msrp_listener_sm::Machine>(
        "msrp_listener_sm", "MSRP Listener", "IEEE 802.1Q-2014 Clause 35 (MSRP — Multiple Stream Reservation Protocol, minimal)"),
    make_sm_info<statusbar::nanoavb::talker_engine_sm::Machine>(
        "talker_engine_sm", "Talker Engine", "(no standard — application-level audio TX pipeline lifecycle)"),
    make_sm_info<statusbar::nanoavb::listener_engine_sm::Machine>(
        "listener_engine_sm", "Listener Engine", "(no standard — application-level audio RX pipeline lifecycle)"),
};

int main(int argc, char* argv[])
{
    return statusbar::sm::sm_tool(argc, argv, state_machines);
}