// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// gptp_slave_linux_tool — gPTP slave-only follower daemon for Linux
//
// Usage: statusbar-gptp-slave [options]   (see --help for the full list)
//   --interface=<iface>         Network interface (default: eth0)
//   --profile=standard|automotive   gPTP profile (default: standard)
//   --manual-peer-delay=<ns>    Manual peer delay (disables Pdelay exchange)
//   --verbose                   Print every sync update
//
// Requires CAP_NET_RAW + CAP_NET_ADMIN, or root.
// After building, grant capabilities with:
//   sudo setcap cap_net_raw,cap_net_admin=ep statusbar-gptp-slave
// Or use: ./scripts/device/setcaps.sh statusbar-gptp-slave

#if defined(__linux__)

#    include "statusbar/config/config.hpp"
#    include "statusbar/gptp/gptp_slave_session.hpp"

#    include <poll.h>

#    include <array>
#    include <chrono>
#    include <csignal>
#    include <cstdio>
#    include <cstdlib>
#    include <iterator>
#    include <print>
#    include <span>
#    include <string>

using namespace statusbar;
using namespace statusbar::gptp;
using Clock = std::chrono::steady_clock;

namespace {

sig_atomic_t volatile g_running = 1;

void signal_handler(int /*sig*/)
{
    g_running = 0;
}

void install_signal_handlers()
{
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

struct Config
{
    std::string interface{"eth0"};
    std::string profile_str{"standard"};
    int64_t manual_peer_delay_ns{-1};     // -1 = use live Pdelay
    int64_t phase_jump_threshold_ns{-1};  // -1 = use config default
    bool verbose{false};
    bool software_timestamping{false};
    double servo_ki{-1.0};
    double servo_kp{-1.0};
    bool no_bridge{false};

    // Derived (resolved after parsing)
    Profile profile{Profile::Standard};
};

auto build_arg_specs(Config& config) -> statusbar::args::ArgumentSpecs
{
    statusbar::args::ArgumentSpecs specs;

    specs.add_device("interface", "Network interface", "eth0", [&](auto v) { config.interface = std::string{v}; });

    specs.add_choice(
        "profile", "gPTP profile", {"standard", "automotive"}, "standard", [&](auto v) { config.profile_str = std::string{v}; });

    specs.add<int64_t>("manual-peer-delay", "Manual peer delay in ns (disables Pdelay exchange)", -1, [&](auto v) {
        config.manual_peer_delay_ns = v;
    });

    specs.add<int64_t>("phase-jump-threshold", "Phase jump threshold in ns (default: from profile)", -1, [&](auto v) {
        config.phase_jump_threshold_ns = v;
    });

    specs.add<double>("servo-ki", "Servo PI integral gain (default: 0.0003)", -1.0, [&](auto v) { config.servo_ki = v; });

    specs.add<double>("servo-kp", "Servo PI proportional gain (default: 1.0)", -1.0, [&](auto v) { config.servo_kp = v; });

    specs.add_flag("software", "Use software timestamping (no PHC required)", [&](auto v) { config.software_timestamping = v; });

    specs.add_flag(
        "no-bridge",
        "Do not keep the gPTP <-> CLOCK_MONOTONIC_RAW bridge. With it (the default) the per-sync line "
        "and the exit summary include the bridge's offset and rate.",
        [&](auto v) { config.no_bridge = v; });

    specs.add_flag("verbose", "Print every sync update", [&](auto v) { config.verbose = v; });

    return specs;
}

void print_usage(char const* program_name, statusbar::args::ArgumentSpecs const& specs)
{
    static constexpr std::array<std::string_view, 4> examples{
        "--interface=eth0 --profile=standard --verbose",
        "--interface=eth0 --profile=automotive --manual-peer-delay=500000 --software --verbose",
        "--interface=eth0 --software --servo-ki=0.0001 --verbose",
        "--config=gptp.toml --verbose",
    };
    config::default_print_usage(
        program_name,
        specs,
        "gPTP slave-only follower daemon for Linux.\nRequires CAP_NET_RAW + CAP_NET_ADMIN, or root.",
        examples);

    std::println(stderr, "\nConfiguration File:");
    std::println(stderr, "  Settings can be stored in a TOML file and loaded with --config=FILE.");
    std::println(stderr, "  CLI arguments override config file values. Save current config with --config-save=FILE.");
}

/// Resolve derived fields from the parsed string values.
void resolve_config(Config& config)
{
    config.profile = (config.profile_str == "automotive") ? Profile::AvnuAutomotive : Profile::Standard;
}

}  // namespace

int main(int argc, char** argv)
{
    Config cli;
    auto specs = build_arg_specs(cli);
    auto cli_result = statusbar::config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-gptp-slave");
    if (!cli_result) {
        return statusbar::config::handled_builtin_command(cli_result) ? 0 : 1;
    }
    resolve_config(cli);

    install_signal_handlers();

    SlaveSessionConfig sc{};
    sc.interface = cli.interface;
    sc.profile = cli.profile;
    sc.software_timestamping = cli.software_timestamping;
    sc.manual_peer_delay_ns = cli.manual_peer_delay_ns;
    sc.phase_jump_threshold_ns = cli.phase_jump_threshold_ns;
    sc.servo_kp = cli.servo_kp;
    sc.servo_ki = cli.servo_ki;
    sc.bridge = !cli.no_bridge;
    sc.verbose = cli.verbose;

    SlaveSession session{sc};
    if (!session.start()) {
        return 1;
    }

    while (g_running != 0) {
        struct pollfd pfd{};
        pfd.fd = session.poll_fds()[0];
        pfd.events = POLLIN;
        int const ready = ::poll(&pfd, 1, /*timeout_ms*/ 100);
        auto const now = Clock::now();
        if (ready > 0 && (pfd.revents & POLLIN) != 0) {
            session.dispatch(pfd.fd, now);
        }
        session.tick(now);
    }

    session.stop();
    return 0;
}

#else

#    include <print>

int main()
{
    std::println(stderr, "gptp_slave_linux_tool requires Linux with hardware timestamping support.");
    return 1;
}

#endif
