// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/bpf/bpf.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/netdump/netdump.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/status/throw_or_abort.hpp"

#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <functional>
#include <iterator>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

struct Config
{
    std::string device;
    uint16_t ethertype{0};
};

std::atomic<bool> sigint_received{false};

void handle_sigint(int /* unused */)
{
    sigint_received.store(true);
}

void print_frame(std::span<uint8_t const> frame)
{
    std::string output;
    statusbar::netdump::format_frame(std::back_inserter(output), frame);
    std::print("{}", output);
}

auto build_arg_specs(Config& config) -> statusbar::args::ArgumentSpecs
{
    statusbar::args::ArgumentSpecs specs;

    specs.add_device("device", "Network interface name (e.g., en0, eth0)", "", [&](auto v) { config.device = std::string{v}; });

    specs.add<std::string_view>("ethertype", "EtherType filter in hexadecimal (e.g., 0x0800)", "", [&](auto v) {
        if (v.empty()) {
            std::print(stderr, "Error: --ethertype is required\n");
            statusbar::throw_or_abort(std::errc::invalid_argument);
        }
        // std::stoul throws (std::invalid_argument / std::out_of_range) on garbage,
        // which would std::terminate the tool. Catch it and exit with a clear error.
        try {
            unsigned long const parsed = std::stoul(std::string{v}, nullptr, 0);
            if (parsed > 0xFFFFU) {
                throw std::out_of_range{"ethertype"};
            }
            config.ethertype = static_cast<uint16_t>(parsed);
        } catch (std::exception const&) {
            std::print(stderr, "Error: --ethertype must be a number in 0..65535 (e.g. 0x0800)\n");
            statusbar::throw_or_abort(std::errc::invalid_argument);
        }
    });

    return specs;
}

void print_usage(char const* program_name, statusbar::args::ArgumentSpecs const& specs)
{
    static constexpr std::array<std::string_view, 2> examples{
        "--device=eth0 --ethertype=0x0800    # Capture IPv4 packets",
        "--device=en0 --ethertype=0x22f0     # Capture AVTP packets",
    };
    statusbar::config::default_print_usage(
        program_name,
        specs,
        "Captures frames on a network device given by --device, filtered by --ethertype (both required).",
        examples);
    std::print(stderr, "\nConfiguration file example:\n");
    std::print(stderr, "  device = \"eth0\"\n");
    std::print(stderr, "  ethertype = \"0x0800\"\n");
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    // Build config and specs with bindings
    Config config;
    auto specs = build_arg_specs(config);

    // Parse CLI arguments, handle --completion, --help, --config-load, --config-save, apply bindings
    auto cli_result = statusbar::config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-bpf-dump");
    if (!cli_result) {
        return statusbar::config::handled_builtin_command(cli_result) ? 0 : 1;
    }

    // Validate required options
    if (config.device.empty()) {
        std::print(stderr, "Error: --device is required (network interface name like eth0, en0, wlan0)\n\n");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }

    if (config.ethertype == 0) {
        std::print(stderr, "Error: --ethertype is required (hexadecimal value like 0x0800 for IPv4, 0x22f0 for AVTP)\n\n");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }

    struct sigaction sa{};
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART — recv() must return EINTR so the loop can exit
    sigaction(SIGINT, &sa, nullptr);

    statusbar::bpf::BpfDevice bpf_device(config.device, {.ethertype = config.ethertype, .promiscuous = true});

    if (bpf_device.file_descriptor() == -1) {
        std::print(stderr, "Error: Failed to open BPF device on interface '{}'\n", config.device);
        std::print(stderr, "  Possible causes:\n");
        std::print(stderr, "  - Interface does not exist (try 'ifconfig' or 'ip link show')\n");
        std::print(stderr, "  - Insufficient permissions (try 'sudo')\n");
        std::print(stderr, "  - No BPF support on this platform\n");
        return EXIT_FAILURE;
    }

    bpf_device.set_callback([](std::span<uint8_t const> frame, statusbar::bpf::AcquisitionTimeAssociation const aquisition_time) {
        std::print(
            "Frame: {} bytes, bpf_time_ns: {} monotonic_clock_time_ns: {}\n",
            frame.size(),
            aquisition_time.bpf_time_ns,
            aquisition_time.monotonic_clock_time_ns);

        print_frame(frame);
        std::print("\n");
    });

    while (!sigint_received.load()) {
        auto status = bpf_device.receive_pdus();
        if (!status) {
            std::print(stderr, "Error receiving PDUs from interface '{}': {}\n", config.device, status.error().message());
            std::print(stderr, "  Interface may have been disconnected or removed\n");
            break;
        }
    }

    return EXIT_SUCCESS;
}