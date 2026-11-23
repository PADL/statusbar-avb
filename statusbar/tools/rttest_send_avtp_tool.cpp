// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/net/net.hpp"
#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/realtime/realtime.hpp"
#include "statusbar/rttest/rttest.hpp"
#include "statusbar/status/catch_or_status.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/status/throw_or_abort.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <functional>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using namespace statusbar;

namespace {

// --- Symbolic constants ---

/// Number of audio samples per AVTP packet (AAF stream format)
constexpr int samples_per_packet = 12;

/// Presentation time offset from scheduled wake time (1.6 ms in nanoseconds)
constexpr int64_t presentation_time_offset_ns = 1'600'000;

/// Timestamp increment between consecutive TX ops within one tick (125 us = 8 kHz class A interval)
constexpr int64_t tx_op_timestamp_step_ns = 125'000;

/// Audio ramp normalization scale (1/7, maps integer range to approx [-1..+1])
constexpr float audio_ramp_scale = 1.0F / 7.0F;

/// Send a JDKS status message every this many timer wakes
constexpr uint64_t jdks_status_send_interval = 8192;

/// Maximum Ethernet header size in bytes (dest + src + VLAN tag + ethertype = 6+6+4+2)
constexpr size_t max_ethernet_header_bytes = 18;

/// Scratch space for AVTP payload serialization (bytes)
constexpr size_t avtp_payload_buffer_bytes = 1000;

/// AVB Class A traffic priority (PCP value per IEEE 802.1Q)
constexpr int avb_class_a_priority = 6;

/// Network queue depth for packet_mmap TX and RX rings
constexpr int mmap_queue_size = 256;

/// Shutdown polling interval while waiting for signal
constexpr auto shutdown_poll_interval = std::chrono::milliseconds(100);

// NOTE: MAC addresses below use the IANA documentation OUI 00:00:5E.
// These are placeholders for documentation/example purposes and MUST
// be replaced with real values for any actual deployment.
struct AvbStreamTxConfig
{
    ieee::Eui48 dest_mac{0x00, 0x00, 0x5E, 0x00, 0x53, 0x00};
    ieee::VlanTag vlan_tag;
    avtp::AafStreamContext aaf_ctx{
        .stream_id = {{0x00, 0x00, 0x5E, 0x00, 0x53, 0x00}, 0x0000},
        .sequence_num = 0,
        .format = avtp::AafFormat::float_32bit,
        .sample_rate = avtp::AafSampleRate::rate_96_khz,
        .bit_depth = 32,
        .channel_count = 1,
        .sample_count = samples_per_packet,
    };
};

struct Config
{
    realtime::TimerConfig packet_timer{.name = "Packet Timer"};
    realtime::TraceConfig trace;
    // Documentation-range MACs (IANA OUI 00:00:5E); replace for real deployments.
    AvbStreamTxConfig avb_tx0{
        .dest_mac = {0x00, 0x00, 0x5E, 0x00, 0x53, 0x00},
        .vlan_tag = {},
        .aaf_ctx = {.stream_id = {{0x00, 0x00, 0x5E, 0x00, 0x53, 0x00}, 0x0000}},
    };
    AvbStreamTxConfig avb_tx1{
        .dest_mac = {0x00, 0x00, 0x5E, 0x00, 0x53, 0x01},
        .vlan_tag = {},
        .aaf_ctx = {.stream_id = {{0x00, 0x00, 0x5E, 0x00, 0x53, 0x01}, 0x0001}},
    };

    bool quiet{false};
    bool verbose{true};
    bool dump_stats_on_exit{true};
    bool dump_avtp_only{false};
    std::string interface{"eth0"};
    int tx_ops_per_tick{1};

    // Backend selection
    std::string backend{"mmap"};
    std::string tap_device;
    uint32_t xdp_queue_id{0};

    // PTP clock configuration (for packet timer only)
    bool use_ptp_clock{false};            ///< Use PTP clock for packet timer scheduling
    std::string ptp_driver{"linuxptp"};   ///< PTP driver: linuxptp, ntpshm, system
    std::string ptp_device{"/dev/ptp0"};  ///< PTP device path or segment number
    ptpclient::BridgeSamplingParams ptp_sampling_params{};
};

auto build_arg_specs(AvbStreamTxConfig& config) -> args::ArgumentSpecs
{
    args::ArgumentSpecs specs;

    specs.add<ieee::Eui48>(
        "dest_mac", "Destination MAC address for AVTP frames", config.dest_mac, [&](auto v) -> void { config.dest_mac = v; });
    specs.add<tsn::StreamId>(
        "stream_id", "AVB Stream ID for transmitted AVTP frames", config.aaf_ctx.stream_id, [&](auto v) -> void {
            config.aaf_ctx.stream_id = v;
        });
    specs.add<uint16_t>("vlan_id", "VLAN ID for transmitted AVTP frames", config.vlan_tag.get_vid(), [&](auto v) -> void {
        config.vlan_tag.set_vid(v);
    });
    specs.add<uint8_t>(
        "pcp", "Priority Code Point (PCP) for transmitted AVTP frames", config.vlan_tag.get_pcp(), [&](auto v) -> void {
            config.vlan_tag.set_pcp(v);
        });

    return specs;
}

auto build_ptp_sampling_arg_specs(ptpclient::BridgeSamplingParams& params) -> args::ArgumentSpecs
{
    args::ArgumentSpecs specs;

    specs.add<int>("sample_hz", "PTP sampling frequency (Hz)", params.sample_hz, [&](auto v) -> void { params.sample_hz = v; });
    specs.add<int>(
        "window_size", "Regression window size (samples)", params.window_size, [&](auto v) -> void { params.window_size = v; });
    specs.add<int>("min_samples_for_healthy", "Min samples before healthy", params.min_samples_for_healthy, [&](auto v) -> void {
        params.min_samples_for_healthy = v;
    });
    specs.add<int64_t>("max_bracket_ns", "Reject samples with bracket > this (ns)", params.max_bracket_ns, [&](auto v) -> void {
        params.max_bracket_ns = v;
    });
    specs.add<int64_t>("step_threshold_ns", "Step detection threshold (ns)", params.step_threshold_ns, [&](auto v) -> void {
        params.step_threshold_ns = v;
    });
    specs.add<int64_t>(
        "degrade_threshold_ns", "Mark unhealthy if RMS > this (ns)", params.degrade_threshold_ns, [&](auto v) -> void {
            params.degrade_threshold_ns = v;
        });
    specs.add<double>(
        "max_rate_ppm", "Max rate deviation from 1.0 (ppm)", params.max_rate_ppm, [&](auto v) -> void { params.max_rate_ppm = v; });
    specs.add<int64_t>(
        "wake_refine_spin_ns", "Final spin refinement for sleep (ns)", params.wake_refine_spin_ns, [&](auto v) -> void {
            params.wake_refine_spin_ns = v;
        });

    return specs;
}

auto build_arg_specs(Config& config) -> args::ArgumentSpecs
{
    args::ArgumentSpecs specs;

    // Merge timer specs with "packet_timer." prefix
    specs.merge(realtime::build_timer_arg_specs(config.packet_timer), "packet_timer.");

    // Merge trace specs with "trace." prefix
    specs.merge(realtime::build_trace_arg_specs(config.trace), "trace.");

    specs.merge(build_arg_specs(config.avb_tx0), "avb_tx0.");
    specs.merge(build_arg_specs(config.avb_tx1), "avb_tx1.");

    specs.add<bool>(
        "quiet", "Suppress per-wake output, only show summary on exit", config.quiet, [&](auto v) -> void { config.quiet = v; });
    specs.add<bool>("verbose", "Enable verbose output during setup", config.verbose, [&](auto v) -> void { config.verbose = v; });
    specs.add<bool>("dump_stats_on_exit", "Dump timer statistics on exit", config.dump_stats_on_exit, [&](auto v) -> void {
        config.dump_stats_on_exit = v;
    });
    specs.add<bool>("dump_avtp_only", "Dump AVTP frame contents only, do not send", config.dump_avtp_only, [&](auto v) -> void {
        config.dump_avtp_only = v;
    });
    specs.add<std::string>(
        "interface", "Network interface to send AVTP frames on", config.interface, [&](auto v) -> void { config.interface = v; });
    specs.add<int>(
        "tx_ops_per_tick", "Number of transmit opportunities per timer interval", config.tx_ops_per_tick, [&](auto v) -> void {
            config.tx_ops_per_tick = v;
        });

    // Backend selection
    specs.add_choice(
        "backend",
        "Network port backend",
#ifdef __linux__
        {"mmap", "bpf", "xdp"},
#else
        {"bpf"},
#endif
        config.backend,
        [&](auto v) { config.backend = std::string{v}; });
    specs.add<std::string>("tap", "TAP device name for XDP bridge mode", config.tap_device, [&](auto v) -> void {
        config.tap_device = std::string{v};
    });
    specs.add<uint32_t>(
        "xdp_queue_id", "XDP RX queue index", config.xdp_queue_id, [&](auto v) -> void { config.xdp_queue_id = v; });

    // PTP clock options (for packet timer only)
    specs.add<bool>(
        "packet_timer.use_ptp_clock", "Use PTP clock for packet timer scheduling", config.use_ptp_clock, [&](auto v) -> void {
            config.use_ptp_clock = v;
        });
    specs.add_choice(
        "packet_timer.ptp_driver",
        "PTP driver",
        {"linuxptp", "ntpshm", "system"},
        config.ptp_driver,
        [&](std::string_view v) -> void { config.ptp_driver = std::string{v}; });
    specs.add<std::string>(
        "packet_timer.ptp_device", "PTP device path or NTP SHM segment number", config.ptp_device, [&](auto v) -> void {
            config.ptp_device = v;
        });
    specs.merge(build_ptp_sampling_arg_specs(config.ptp_sampling_params), "packet_timer.ptp_sampling.");

    return specs;
}

void print_usage(char const* prog, args::ArgumentSpecs const& specs)
{
    static constexpr std::array<std::string_view, 3> examples{
        "--interface=eth0 --backend=mmap",
        "--interface=eth0 --backend=xdp --xdp_queue_id=1 --packet_timer.cpu=3 --dump_stats_on_exit",
        "--interface=eth0 --packet_timer.use_ptp_clock --packet_timer.ptp_driver=linuxptp --packet_timer.ptp_device=/dev/ptp0",
    };
    config::default_print_usage(
        prog,
        specs,
        "Real-time AVTP/AAF transmitter for TSN egress testing. A realtime timer wakes every\n"
        "packet_timer.period_ns and sends tx_ops_per_tick frames on each of two AAF streams\n"
        "(avb_tx0.*, avb_tx1.*) through the mmap, bpf or xdp backend, optionally scheduled on a\n"
        "PTP clock; the wake-error and duration histograms and the tracer report what the host\n"
        "did with the deadline. --dump_avtp_only prints the frames and sends nothing.",
        examples);
}

auto parse_config(int argc, char** argv) -> StatusValue<Config>
{
    Config config;
    auto specs = build_arg_specs(config);

    // Parse CLI arguments, handle --completion, --help, --config-load, --config-save, apply bindings
    auto cli_result = config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-rttest-send-avtp-tool");
    if (!cli_result) {
        return failure(cli_result.error());
    }

    // Validate timer config
    if (!realtime::validate_timer_config(config.packet_timer, "packet_timer.")) {
        return failure(std::make_error_code(std::errc::invalid_argument));
    }

    return config;
}

/// Form an AVTP audio packet directly into the provided buffer (zero-copy)
/// Returns the total frame length written
auto form_avtp_audio_packet_into(
    AvbStreamTxConfig& config,
    ieee::Eui48 const& src_mac,
    int64_t timestamp_ns,
    std::span<float const, samples_per_packet> audio,
    std::span<uint8_t> buffer) -> size_t
{
    // Build ethernet frame
    ieee::EthernetFrame eth_frame{};
    eth_frame.dest_mac = config.dest_mac;
    eth_frame.src_mac = src_mac;
    if (config.vlan_tag.get_vid() != 0) {
        eth_frame.vlan_tag = config.vlan_tag;
    }
    eth_frame.ethertype = avtp::AVTP_ETHERTYPE;

    // Serialize Ethernet header directly into buffer
    auto const eth_len = store_unchecked(buffer.subspan(0, max_ethernet_header_bytes), eth_frame);
    std::span<uint8_t> const avtp_payload_space = buffer.subspan(eth_len, avtp_payload_buffer_bytes);

    // Build AVTP frame using AafStreamContext
    auto const aaf_len = avtp::aaf_create_packet(config.aaf_ctx, timestamp_ns, std::span{audio}, avtp_payload_space);

    return eth_len + aaf_len;
}

/// Send an AVTP audio packet using zero-copy TX (does not flush)
/// @return true if packet was successfully queued, false if TX ring is full
template <net::EthernetPort Port>
auto send_avtp_stream(
    Port& port, AvbStreamTxConfig& stream_config, int64_t timestamp_ns, std::span<float const, samples_per_packet> audio) -> bool
{
    auto status = net::send_frame(port, 0, [&](std::span<uint8_t> buffer) -> size_t {
        return form_avtp_audio_packet_into(stream_config, port.hardware_address(), timestamp_ns, audio, buffer);
    });
    return status.has_value();
}

template <net::EthernetPort Port>
auto send_jdks_status(Port& port, int64_t timestamp_ns) -> bool
{
    using namespace atdecc::jdks;

    auto status = net::send_frame(port, 0, [&](std::span<uint8_t> buffer) -> size_t {
        // Convert timestamp to ASCII string
        std::array<char, 32> text_buf{};
        auto const text_len =
            std::snprintf(text_buf.data(), text_buf.size(), "gPTP_time = %lld", static_cast<long long>(timestamp_ns));
        std::string_view const text(text_buf.data(), static_cast<size_t>(text_len));

        // Set up context for log response
        static uint16_t sequence_id = 0;
        ConsoleCommandContext ctx{};
        ctx.src_mac = port.hardware_address();
        ctx.my_entity_id =
            ieee::Eui64(0x00, 0x00, 0x5E, 0x00, 0x53, 0x00, 0x00, 0x00);  // Use stream ID base as entity ID (documentation OUI)
        ctx.descriptor_index = 0;

        // Generate JDKS log PDU — returns 0 on failure, which auto-cancels the guard
        return generate_log_response(buffer, ctx, sequence_id, log_priority::INFO, text);
    });
    return status.has_value();
}

/// Common packet timer callback logic — performs full TX cycle per tick:
/// begin_cycle → send packets → TAP drain → flush → end_cycle
/// @tparam ClockT The clock type for the timer event
/// @tparam Port The EthernetPort backend type
template <realtime::ClockType ClockT, net::EthernetPort Port>
auto make_packet_timer_callback(realtime::TripwireMonitor& tripwire_monitor, Config& config, Port& port, net::TapBridge* tap)
{
    return [&tripwire_monitor, &config, &port, tap](
               realtime::TimerEvent<ClockT> const& event, stats::AtomicWakeStats::Snapshot const& stats) -> void {
        realtime::ScopedTripwireObserver const tripwire_guard(tripwire_monitor, event);

        port.begin_cycle();

        int64_t timestamp_offset = presentation_time_offset_ns;
        for (int i = 0; i < config.tx_ops_per_tick; ++i, timestamp_offset += tx_op_timestamp_step_ns) {
            int64_t const timestamp_ns = event.scheduled_time_ns() + timestamp_offset;

            constexpr int half_packet = samples_per_packet / 2;
            std::array<float, samples_per_packet> audio0;
            for (int j = 0; j < samples_per_packet; ++j) {
                audio0[j] = static_cast<float>(j - half_packet) * audio_ramp_scale;
            }
            std::array<float, samples_per_packet> audio1;
            for (int j = 0; j < samples_per_packet; ++j) {
                audio1[j] = static_cast<float>(samples_per_packet - (j - half_packet)) * audio_ramp_scale;
            }

            if ((event.wake_count & (jdks_status_send_interval - 1)) == 0) {
                (void)send_jdks_status(port, timestamp_ns);
            }

            // Send both AVTP streams (zero-copy, queues packets without flushing)
            (void)send_avtp_stream(port, config.avb_tx0, timestamp_ns, audio0);
            (void)send_avtp_stream(port, config.avb_tx1, timestamp_ns, audio1);
        }

        // Drain non-RT frames from TAP to wire (XDP bridge mode)
        if (tap && tap->valid()) {
            (void)tap->drain_to_wire(port);
        }

        (void)port.tx_flush();
        (void)port.end_cycle();
    };
}

/// Create packet timer with appropriate clock based on config
/// Throws std::system_error on failure
template <net::EthernetPort Port>
auto create_packet_timer(realtime::TripwireMonitor& tripwire_monitor, Config& config, Port& port, net::TapBridge* tap)
    -> realtime::AnyTimer
{
#if defined(__linux__)
    if (config.use_ptp_clock) {
        if (config.verbose) {
            std::print("Using PTP clock: driver={}, device={}\n", config.ptp_driver, config.ptp_device);
        }

        // Set up gPTP context (PTP client, bridge, and clock adapter)
        ptpclient::PtpAppConfig ptp_config;
        ptp_config.driver_name = config.ptp_driver;
        ptp_config.device_path = config.ptp_device;
        ptp_config.sampling = config.ptp_sampling_params;
        ptp_config.verbose = config.verbose;
        ptp_config.lock_memory = false;  // Memory locking handled separately

        auto gptp_result = ptpclient::setup_gptp_app<0>(ptp_config, realtime::is_shutdown_requested);
        if (!gptp_result) {
            statusbar::throw_or_abort(gptp_result.error());
        }
        auto gptp_ctx = std::move(*gptp_result);

        if (config.verbose) {
            auto mapping = gptp_ctx.ptp.bridge->get_mapping();
            std::print(
                "PTP bridge healthy (rate={:.6F}, offset={}ns, RMS={}ns)\n",
                mapping.rate,
                mapping.offset_ns,
                mapping.rms_residual_ns);
        }

        // Create packet timer with gPTP clock adapter from context
        // Note: gptp_ctx must be kept alive - we capture it in a shared_ptr
        auto gptp_ctx_ptr = std::make_shared<ptpclient::GptpContext<0>>(std::move(gptp_ctx));
        auto callback = make_packet_timer_callback<realtime::GptpClock<0>>(tripwire_monitor, config, port, tap);

        // Wrap callback to capture gptp_ctx_ptr (keeping context alive)
        return realtime::AnyTimer::create(
            config.packet_timer,
            std::move(gptp_ctx_ptr->adapter),
            [gptp_ctx_ptr, cb = std::move(callback)](
                realtime::TimerEvent<realtime::GptpClock<0>> const& event, stats::AtomicWakeStats::Snapshot const& stats) {
                cb(event, stats);
            });
    }
#else
    if (config.use_ptp_clock) {
        statusbar::throw_or_abort(std::errc::not_supported);
    }
#endif

    // Use the raw monotonic clock for packet timer (default)
    return realtime::AnyTimer::create(
        config.packet_timer, make_packet_timer_callback<realtime::MonotonicRawClock>(tripwire_monitor, config, port, tap));
}

/// Run the main timer loop with type-erased timers
auto run_timers(std::vector<realtime::AnyTimer>& timers, Config const& config) -> int
{
    // Print timer configs
    if (config.verbose) {
        realtime::print_timer_configs(timers);
    }

    {
        // Start all timers (RAII - will stop on scope exit)
        auto timer_guards = realtime::start_timers(timers, &realtime::shutdown_token());

        // Wait for shutdown signal
        while (!realtime::is_shutdown_requested()) {
            std::this_thread::sleep_for(shutdown_poll_interval);
        }
    }

    // Stop timers before printing stats
    if (config.verbose) {
        std::print("\n\nShutting down...\n");
    }
    // Print final statistics
    if (config.dump_stats_on_exit) {
        realtime::print_timer_stats(timers);
    }

    return EXIT_SUCCESS;
}

/// Set up port and timers, run until shutdown
template <net::EthernetPort Port>
auto run_with_port(Port& port, net::TapBridge* tap, Config& config) -> int
{
    // Check realtime capabilities and prepare system
    (void)realtime::print_realtime_diagnostics_and_prepare(config.verbose);

    // Use packet timer CPU for trace CPU
    config.trace.cpu = config.packet_timer.cpu;

    // Create and start TripwireMonitor
    realtime::TripwireMonitor tripwire_monitor(config.trace);
    realtime::TripwireMonitorStart const tripwire_start{tripwire_monitor, &realtime::shutdown_token()};

    // Create timer (single timer handles full TX cycle)
    std::vector<realtime::AnyTimer> timers;
    timers.emplace_back(create_packet_timer(tripwire_monitor, config, port, tap));

    return run_timers(timers, config);
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    auto const result = statusbar::catch_or_status(
        [&]() -> statusbar::StatusValue<int> {
            // Ensure stdout is line-buffered so output appears promptly in terminals
            setvbuf(stdout, nullptr, _IOLBF, 0);

            Config config = config::parse_config_or_exit(parse_config, argc, argv);

            realtime::setup_shutdown_signal_handlers();

            if (config.verbose) {
                std::print("Backend: {}, interface: {}\n", config.backend, config.interface);
            }

            net::TapBridge tap;

            if (config.backend == "bpf") {
                net::BpfPortContext port;
                net::BpfPortConfig const port_config{
                    .interface_name = config.interface,
                    .ethertype = avtp::AVTP_ETHERTYPE,
                };
                if (auto s = port.open(port_config); !s) {
                    statusbar::throw_or_abort(s.error(), "Failed to open BPF port");
                }
                return run_with_port(port, &tap, config);
            }

#ifdef __linux__
            if (config.backend == "mmap") {
                net::MmapContext port;
                net::MmapContextOpen const open{
                    port,
                    {
                        .interface_name = config.interface,
                        .ethertype = avtp::AVTP_ETHERTYPE,
                        .priority = avb_class_a_priority,
                        .rx_queue_size = mmap_queue_size,
                        .tx_queue_size = mmap_queue_size,
                    }};
                return run_with_port(port, &tap, config);
            }

            if (config.backend == "xdp") {
                // Create TAP bridge if requested
                if (!config.tap_device.empty()) {
                    if (auto s = tap.open(net::TapBridgeConfig{.device_name = config.tap_device}); !s) {
                        statusbar::throw_or_abort(s.error(), "Failed to open TAP device '" + config.tap_device + "'");
                    }
                    if (config.verbose) {
                        std::print("TAP device: {} (ifindex={})\n", config.tap_device, tap.ifindex());
                    }
                }

                // XDP filter: redirect AVTP ethertype to XSK, pass everything else
                auto const rules =
                    net::ClassifierRuleBuilder<32>{}.ethertype(avtp::AVTP_ETHERTYPE).vlan_any().result(net::flag::process).build();
                net::XdpContext port;
                net::XdpContextOpen const open{
                    port,
                    {
                        .interface_name = config.interface,
                        .filter_rules = rules,
                        .default_action = tap.valid() ? net::XdpAction::redirect_tap : net::XdpAction::pass_kernel,
                        .queue_id = config.xdp_queue_id,
                        .tap_ifindex = tap.ifindex(),
                    }};
                return run_with_port(port, &tap, config);
            }
#endif

            std::print(stderr, "Error: Unknown backend '{}'\n", config.backend);
            return EXIT_FAILURE;
        },
        std::errc::io_error);
    if (!result) {
        std::print(stderr, "Fatal error: {}\n", result.error().message());
        return EXIT_FAILURE;
    }
    return *result;
}
