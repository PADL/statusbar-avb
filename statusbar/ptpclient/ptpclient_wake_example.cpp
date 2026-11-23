// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// PTP Wake Example - Demonstrates precision wake timing using PTP clock
//
// Usage: ptpclient_wake_example --ptp.driver=<name> --ptp.device=<path> --ptp.period=<ns> [options]
//
// Requires CAP_SYS_NICE capability or root for realtime thread priority.

#include "statusbar/config/config.hpp"
#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/realtime/realtime.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iterator>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

// Config contains PTP fields via composition
struct Config
{
    statusbar::ptpclient::PtpAppConfig ptp_app;
    // Add any example-specific fields here if needed
};

// Use realtime module's shutdown handling

auto build_arg_specs(Config& config) -> statusbar::args::ArgumentSpecs
{
    statusbar::args::ArgumentSpecs specs;

    // Add PTP options with lambda bindings to config
    statusbar::ptpclient::add_ptp_arg_specs(specs, config.ptp_app);

    return specs;
}

void print_usage(char const* program_name, statusbar::args::ArgumentSpecs const& specs)
{
    static constexpr std::array<std::string_view, 3> examples{
        "--ptp.driver=linuxptp --ptp.device=/dev/ptp0 --ptp.period=1000000",
        "--ptp.period=1000000 --ptp.compensation=-3000   # Wake 3us earlier",
        "--ptp.period=1000000 --ptp.no-rt                # Without RT priority",
    };
    statusbar::config::default_print_usage(program_name, specs, "", examples);
    std::print(stderr, "\nConfiguration file example:\n");
    std::print(stderr, "  [ptp]\n");
    std::print(stderr, "  driver = \"linuxptp\"\n");
    std::print(stderr, "  device = \"/dev/ptp0\"\n");
    std::print(stderr, "  period = 1000000\n");
    std::print(stderr, "  compensation = -3000\n");
    std::print(stderr, "  cpu = 3\n");
    std::print(stderr, "  no-rt = false\n");
    std::print(stderr, "\nPress Ctrl-C to stop and display statistics.\n");
}

/// Setting up PTP did not finish: a shutdown during it is a clean exit.
auto setup_failed(std::error_code const& error) -> int
{
    if (statusbar::realtime::is_shutdown_requested()) {
        std::print("Shutdown requested during setup\n");
        return EXIT_SUCCESS;
    }
    std::print(stderr, "Error: Failed to setup PTP: {}\n", error.message());
    return EXIT_FAILURE;
}

/// The example's wake: a line every 10000 wakes with the wake error.
void on_wake(statusbar::StatusValue<statusbar::ptpclient::TimerWakeInfo> const& wake_info)
{
    if (!wake_info) {
        std::print(stderr, "Warning: Timer error: {}\n", wake_info.error().message());
        return;
    }
    if (wake_info->wake_count % 10000 == 0) {
        std::print(
            "Wakes: {:8}  Error: {:+8} ns ({:+.3f} us)\n",
            wake_info->wake_count,
            wake_info->error_ns(),
            static_cast<double>(wake_info->error_ns()) / 1000.0);
    }
}

template <typename StatsT>
void print_report(StatsT const& stats, int64_t const compensation_ns)
{
    std::string report;
    stats.format_report_to(std::back_inserter(report), compensation_ns);
    std::print("{}", report);
}

/// Clean shutdown, then the timer's statistics.
template <typename TimerT>
void stop_and_report(TimerT& timer, statusbar::ptpclient::PtpAppContext& ctx)
{
    std::print("\n\nShutting down...\n");
    timer.stop();
    std::print("\n\nguard stopping...\n");
    ctx.guard.stop();
    std::print("\n\nguard stopped...\n\n");
    print_report(timer.stats(), ctx.compensation_ns);
}

/// Run the timer on the PHC until a shutdown signal.
auto run_wake_loop(statusbar::ptpclient::PtpAppContext& ctx) -> int
{
    std::print("\nStarting wake loop (Ctrl-C to stop)...\n\n");
    auto timer = statusbar::ptpclient::make_ptp_timer(
        *ctx.bridge, ctx.period_ns, on_wake, ctx.compensation_ns, ctx.enable_realtime, ctx.cpu_affinity, ctx.threshold);
    if (auto const started = timer.start(); !started) {
        std::print(stderr, "Error: Failed to start timer: {}\n", started.error().message());
        return EXIT_FAILURE;
    }
    while (!statusbar::realtime::is_shutdown_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    stop_and_report(timer, ctx);
    return EXIT_SUCCESS;
}

void print_config(Config const& config)
{
    std::string config_summary;
    statusbar::ptpclient::format_ptp_config_to(std::back_inserter(config_summary), config.ptp_app, "PTP Wake Example");
    std::print("{}", config_summary);
}

/// Set up the PTP application (client, bridge, sampling, wait for healthy)
/// and run the wake loop on it.
auto run(Config const& config) -> int
{
    print_config(config);
    statusbar::realtime::setup_shutdown_signal_handlers();
    auto ctx = statusbar::ptpclient::setup_ptp_app(config.ptp_app, [] { return statusbar::realtime::is_shutdown_requested(); });
    if (ctx) {
        return run_wake_loop(*ctx);
    }
    return setup_failed(ctx.error());
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    // Build config and specs with bindings
    Config config;
    auto specs = build_arg_specs(config);

    // Parse CLI arguments, handle --completion, --help, --config-load, --config-save, apply bindings
    auto cli_result = statusbar::config::parse_cli_args(argc, argv, specs, print_usage);
    if (!cli_result) {
        return statusbar::config::handled_builtin_command(cli_result) ? 0 : 1;
    }
    if (config.ptp_app.period_ns <= 0) {
        std::print(stderr, "Error: --ptp.period must be positive\n");
        return EXIT_FAILURE;
    }
    return run(config);
}
