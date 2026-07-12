// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// ATDECC Controller TUI Tool — interactive controller for ATDECC entities
// Discovers entities via ADP, reads ENTITY descriptors via AEM,
// and manages stream connections via ACMP.
// Usage: statusbar-atdecc-controller --interface=eth0 [--instance=N] [--controller-entity-id=EUI64]

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc_tools/atdecc_controller_service.hpp"
#include "statusbar/atdecc_tools/atdecc_controller_simple.hpp"
#include "statusbar/atdecc_tools/atdecc_controller_tui.hpp"
#include "statusbar/atdecc_tools/atdecc_tools_common.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/tui/tui.hpp"

#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
#include <print>
#include <string>
#include <utility>

namespace {

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::ieee;
using namespace statusbar::net;

using atdecc_tools::CommonConfig;
using atdecc_tools::ControllerTuiApp;
using atdecc_tools::StreamRequest;

// ---------------------------------------------------------------------------
// Timestamps
// ---------------------------------------------------------------------------

auto const g_start_time = std::chrono::steady_clock::now();

auto elapsed_ns() -> int64_t
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_start_time).count();
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

std::string g_backend{atdecc_tools::default_controller_backend()};

auto build_arg_specs(CommonConfig& config) -> args::ArgumentSpecs
{
    args::ArgumentSpecs specs;
    atdecc_tools::add_common_arg_specs(specs, config);
    specs.add_choice(
        "backend",
        "Controller backend: raw (own state machines over the 0x22F0 raw socket) or avb (macOS AVB framework; required "
        "to reach the Mac's own virtual entity). Defaults to avb on macOS, raw elsewhere.",
        {"raw", "avb"},
        atdecc_tools::default_controller_backend(),
        [](auto v) { g_backend = std::string{v}; });
    return specs;
}

void print_usage(char const* prog, args::ArgumentSpecs const& specs)
{
    config::default_print_usage(prog, specs, "Interactive ATDECC controller with TUI.");
}

}  // namespace

int main(int argc, char* argv[])
{
    using namespace statusbar;

    CommonConfig config;
    auto specs = build_arg_specs(config);

    auto cli_result = config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-atdecc-controller");
    if (!cli_result) {
        return config::handled_builtin_command(cli_result) ? 0 : 1;
    }
    if (config.interface_name.empty()) {
        std::println(stderr, "Error: --interface is required");
        return 1;
    }

    auto id_result = atdecc_tools::resolve_controller_id(config, atdecc_tools::TOOL_ID_CONTROLLER);
    if (!id_result.has_value()) {
        std::println(stderr, "Error: cannot open interface '{}': {}", config.interface_name, id_result.error().message());
        return 1;
    }
    Eui64 const controller_id = *id_result;

    // Create the controller backend (platform default: AVB framework on
    // macOS, raw socket elsewhere) and the controller pollable on it.
    std::string backend_error;
    auto service = atdecc_tools::make_controller_service(g_backend, config.interface_name, controller_id, backend_error);
    if (service == nullptr) {
        std::println(stderr, "Error: {}", backend_error);
        return 1;
    }
    auto pollable = std::make_unique<atdecc_tools::ControllerSimple>(std::move(service));
    auto* pollable_ptr = pollable.get();
    // Interactive UI: keep live RX-state fresh by auto-probing GET_RX_STATE on
    // discovery. (One-shot tools leave this off to avoid in-flight starvation.)
    pollable_ptr->set_auto_probe_rx_state(true);

    // Create TUI app (presentation layer). It subscribes to events from
    // the business layer (ControllerSimple) and emits actions back.
    ControllerTuiApp app{};
    app.set_interface_name(config.interface_name);

    // Set up terminal
    tui::TerminalTui tui;
    tui.hide_cursor();
    tui.clear();

    // Set up reactor
    auto& stop = statusbar::itc::install_stop_signal();
    MessageReactor reactor{stop, elapsed_ns, 10};
    reactor.add(std::move(pollable));

    // Main loop
    while (!stop.stop_requested()) {
        reactor.poll_once(50);

        int key = tui.read_key();
        if (key != tui::KEY_NONE) {
            if (!app.handle_key(key)) {
                break;
            }
        }

        // Dispatch actions emitted by the TUI to the business layer
        auto const now_ns = elapsed_ns();
        for (auto const& action : app.take_pending_actions()) {
            pollable_ptr->dispatch(action, now_ns);
        }

        // Drain events from the business layer into the TUI
        for (auto const& event : pollable_ptr->drain_events()) {
            app.handle_event(event);
        }

        // Check for terminal resize
        if (tui.resize_pending()) {
            tui.clear();
            app.force_redraw();
        }

        app.update_entities(pollable_ptr->get_display_entities());

        if (app.needs_render()) {
            app.render(tui);
        }
    }

    return 0;
}
