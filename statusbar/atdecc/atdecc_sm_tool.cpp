// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// ATDECC State Machine Documentation Tool
// Generates DOT (Graphviz) or Markdown documentation for ATDECC state machines

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_adp_discovery.hpp"
#include "statusbar/atdecc/atdecc_aecp_aem_controller.hpp"
#include "statusbar/sm/sm.hpp"

#include <array>
#include <span>
#include <string_view>

using statusbar::sm::make_sm_info;

/// Registry of all ATDECC state machines
/// To add a new state machine, add a make_sm_info<Type>() entry below.
constexpr auto state_machines = std::array{
    make_sm_info<statusbar::atdecc::AcmpControllerStateMachine<>>(
        "acmp_controller_sm", "ACMP Controller", "IEEE 1722.1-2021 Clause 8.2.3"),
    make_sm_info<statusbar::atdecc::AcmpTalkerStateMachine<>>("acmp_talker_sm", "ACMP Talker", "IEEE 1722.1-2021 Clause 8.2.4"),
    make_sm_info<statusbar::atdecc::AcmpListenerStateMachine<>>(
        "acmp_listener_sm", "ACMP Listener", "IEEE 1722.1-2021 Clause 8.2.5"),
    make_sm_info<statusbar::atdecc::AdpDiscoveryStateMachine<>>(
        "adp_discovery_sm", "ADP Discovery", "IEEE 1722.1-2021 Clause 6.2.4"),
    make_sm_info<statusbar::atdecc::AemControllerStateMachine<>>(
        "aem_controller_sm", "AECP AEM Controller", "IEEE 1722.1-2021 Clause 9.2.1.2.5"),
    make_sm_info<statusbar::atdecc::MilanSinkStateMachine<>>(
        "milan_acmp_sink_sm", "Milan ACMP Listener Sink", "Milan v1.3 Section 5.5.3"),
    make_sm_info<statusbar::atdecc::MilanDiscoveryStateMachine<>>(
        "milan_adp_discovery_sm", "Milan Listener Discovery", "Milan v1.3 Section 5.6.4"),
};

int main(int argc, char* argv[])
{
    return statusbar::sm::sm_tool(argc, argv, state_machines);
}
