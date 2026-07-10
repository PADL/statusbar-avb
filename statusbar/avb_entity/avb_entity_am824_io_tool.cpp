// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AVB Entity AM824 I/O Tool - Runs an AVB entity with N-channel AM824 audio and DSP
//
// Usage: avb_entity_am824_io_tool [options]
//
// On Linux with hardware PTP:
//   avb_entity_am824_io_tool --ptp.driver=linuxptp --ptp.device=/dev/ptp0 --descriptor-storage=entity.bin
//
// On any platform (uses system clock):
//   avb_entity_am824_io_tool --ptp.driver=system --descriptor-storage=entity.bin
//
// This tool instantiates AvbEntityAm824IO which provides:
// - One N-channel 48 kHz AM824 listener stream (input)
// - One N-channel 48 kHz AM824 talker stream (output)
// - Configurable DSP biquad filter (peak EQ)
// - Full ATDECC support (ADP, ACMP, AECP/AEM)
// - gPTP synchronization
// - MVRP/MSRP stream reservation

#include "statusbar/avb_entity/avb_entity_am824_io.hpp"
#include "statusbar/avb_entity/avb_entity_identity.hpp"
#include "statusbar/buffer/stream_utils.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/gptp/gptp_format.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_published.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/itc/itc_telemetry_counter.hpp"
#include "statusbar/logging/logging_collector.hpp"
#include "statusbar/logging/logging_sink.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/realtime/realtime.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace statusbar;
using TimePoint = sm::TimePoint;

//
// File loading helper
//

auto load_file(std::filesystem::path const& path) -> std::vector<uint8_t>
{
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file) {
        return {};
    }
    auto const size = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    statusbar::stream_read(file, data);
    return data;
}

//
// Configuration
//

struct Config
{
    // PTP timer configuration
    ptpclient::PtpAppConfig ptp_app;

    // AVB Entity configuration
    avb_entity::AvbEntityAm824IOConfig entity{
        // entity_id unset → derived per-node from the NIC MAC at startup (so two
        // nodes don't collide); overridable with --entity.id.
        .entity_id = ieee::Eui64{},
        .entity_model_id = ieee::Eui64{0x70, 0xB3, 0xD5, 0xED, 0xCF, 0x00, 0x00, 0x00},
        // interface_name has no default — --interface is required.
        // JDKS OUI-36 multicast (NOT the 91:E0:F0 MAAP pool, which must be claimed
        // via MAAP before use); like the audio_io tool's range.
        .talker_dest_mac = {0x71, 0xB3, 0xD5, 0xED, 0xCF, 0xF1},
        .vlan_id = 2,
        .filter_freq_hz = 1000.0,
        .filter_gain_db = 0.0,  // Unity gain by default
        .filter_q = 0.707,      // Butterworth Q
        .entity_name = "AVB AM824 IO Tool",
        .firmware_version = "1.0.0",
    };

    // Descriptor storage path
    std::string descriptor_storage_path;

    // Runtime options
    bool verbose{true};
    bool dump_stats_on_exit{true};
};

//
// Argument Specifications
//

auto build_arg_specs(Config& config) -> args::ArgumentSpecs
{
    args::ArgumentSpecs specs;

    // Add PTP options
    ptpclient::add_ptp_arg_specs(specs, config.ptp_app);

    // Entity configuration - use native EUI types
    specs.add<ieee::Eui64>(
        "entity.id", "Entity ID (EUI-64)", config.entity.entity_id, [&](auto v) { config.entity.entity_id = v; });

    specs.add<ieee::Eui64>("entity.model_id", "Entity Model ID (EUI-64)", config.entity.entity_model_id, [&](auto v) {
        config.entity.entity_model_id = v;
    });

    specs.add_device(
        "interface", "Network interface (required, e.g. eth0)", "", [&](auto v) { config.entity.interface_name = std::string{v}; });

    specs.add<std::string>("entity.name", "Entity name (shown in ATDECC controllers)", config.entity.entity_name, [&](auto v) {
        config.entity.entity_name = v;
    });

    specs.add<uint16_t>("vlan_id", "VLAN ID for AVB streams", config.entity.vlan_id, [&](auto v) { config.entity.vlan_id = v; });

    specs.add<ieee::Eui48>("talker.dest_mac", "Talker destination multicast MAC", config.entity.talker_dest_mac, [&](auto v) {
        config.entity.talker_dest_mac = v;
    });

    // DSP filter configuration
    specs.add<double>("filter.freq_hz", "Filter center frequency (Hz)", config.entity.filter_freq_hz, [&](auto v) {
        config.entity.filter_freq_hz = v;
    });

    specs.add<double>(
        "filter.gain_db", "Filter gain (dB, negative=cut, positive=boost)", config.entity.filter_gain_db, [&](auto v) {
            config.entity.filter_gain_db = v;
        });

    specs.add<double>("filter.q", "Filter Q factor", config.entity.filter_q, [&](auto v) { config.entity.filter_q = v; });

    // Descriptor storage path
    specs.add<std::string>(
        "descriptor-storage", "Path to descriptor storage .bin file (required)", config.descriptor_storage_path, [&](auto v) {
            config.descriptor_storage_path = v;
        });

    // Runtime options
    specs.add<bool>("verbose", "Enable verbose output", config.verbose, [&](auto v) { config.verbose = v; });
    specs.add<bool>("dump_stats_on_exit", "Dump timer statistics on exit", config.dump_stats_on_exit, [&](auto v) {
        config.dump_stats_on_exit = v;
    });
    specs.add<bool>(
        "stream_rx.rt_timer",
        "Drain the AM824 stream RX on a dedicated SCHED_FIFO timer (its own isolated core) instead of the shared "
        "reactor -- deterministic low-latency draining stamps ingress against a fresh gPTP wake time, eliminating "
        "spurious LATE_TIMESTAMP from reactor scheduling jitter. Default false (reactor); needs a spare isolated core",
        config.entity.stream_rx_rt_timer,
        [&](auto v) { config.entity.stream_rx_rt_timer = v; });
    specs.add<int64_t>(
        "stream_rx.cpu",
        "CPU core for the RX timer thread (stream_rx.rt_timer); isolated + distinct from the media timer",
        static_cast<int64_t>(config.entity.stream_rx_cpu_affinity),
        [&](auto v) { config.entity.stream_rx_cpu_affinity = static_cast<int>(v); });
    specs.add<uint64_t>(
        "stream_rx.period_us",
        "RX drain tick period in microseconds (stream_rx.rt_timer). Default 50",
        static_cast<uint64_t>(config.entity.stream_rx_period_us),
        [&](auto v) { config.entity.stream_rx_period_us = static_cast<uint32_t>((v < 5) ? 5 : ((v > 1000) ? 1000 : v)); });

    return specs;
}

//
// Help and Usage
//

void print_usage(char const* program_name, args::ArgumentSpecs const& specs)
{
#if defined(__linux__)
    static constexpr std::array<std::string_view, 3> examples{
        "--ptp.driver=linuxptp --ptp.device=/dev/ptp0 --descriptor-storage=entity.bin",
        "--interface=eth0 --filter.gain_db=-6 --descriptor-storage=entity.bin",
        "--ptp.driver=system --descriptor-storage=entity.bin",
    };
#else
    static constexpr std::array<std::string_view, 2> examples{
        "--interface=eth0 --filter.gain_db=-6 --descriptor-storage=entity.bin",
        "--ptp.driver=system --descriptor-storage=entity.bin",
    };
#endif
    config::default_print_usage(
        program_name,
        specs,
        "AVB Entity AM824 I/O Tool\n"
        "Runs an AVB entity with N-channel AM824 audio passthrough and DSP processing.",
        examples);

    std::print(stderr, "\nPress Ctrl-C to stop.\n");
}

//
// Configuration Display
//

void print_entity_config(Config const& config, avb_entity::AvbEntityAm824IO const& entity)
{
    std::print("AVB Entity AM824 I/O Tool\n");
    std::print("=========================\n");
    std::print("Descriptor Storage: {}\n", config.descriptor_storage_path);
    std::print("Entity ID:        {}\n", ieee::to_string(config.entity.entity_id).view());
    std::print("Entity Model ID:  {}\n", ieee::to_string(config.entity.entity_model_id).view());
    std::print("Entity Name:      {}\n", config.entity.entity_name);
    std::print("Interface:        {}\n", config.entity.interface_name);
    std::print("VLAN ID:          {}\n", config.entity.vlan_id);
    std::print("Talker Dest MAC:  {}\n", ieee::to_string(config.entity.talker_dest_mac).view());
    std::print("Channels:         {}\n", entity.channels());
    std::print("\nDSP Filter:\n");
    std::print("  Frequency:      {:.1f} Hz\n", config.entity.filter_freq_hz);
    std::print("  Gain:           {:.1f} dB\n", config.entity.filter_gain_db);
    std::print("  Q:              {:.3f}\n", config.entity.filter_q);
    std::print("\nAudio Format:\n");
    std::print("  Sample Rate:    {} Hz\n", avb_entity::AvbEntityAm824IO::SAMPLE_RATE);
    std::print("  Channels:       {}\n", entity.channels());
    std::print("  Samples/Packet: {}\n", avb_entity::AvbEntityAm824IO::SAMPLES_PER_PACKET);
}

//
// Main Loop Result
//

struct MainLoopResult
{
    int exit_code{EXIT_SUCCESS};
    stats::AtomicWakeStats::Snapshot wake_stats{};
    int64_t compensation_ns{0};
    int64_t recovery_count{0};
    int64_t missed_cycles{0};
};

//
// Main Loop with PTP Timer
//

MainLoopResult run_main_loop(
    net::MessageReactor& reactor, ptpclient::PtpAppContext& ctx, avb_entity::AvbEntityAm824IO& entity, Config const& config)
{
    MainLoopResult result;
    result.compensation_ns = ctx.compensation_ns;

    // Packet rate timer period: 6 samples @ 48 kHz = 125 µs
    constexpr int64_t PACKET_PERIOD_NS = 125'000;

    // Track gPTP announce status
    auto* net_handlers = entity.net_handlers();
    bool had_grandmaster = net_handlers != nullptr ? net_handlers->gptp_handler().has_grandmaster() : false;

    // The media-timer callback runs on a SCHED_FIFO RT thread, so it must NOT
    // std::print (alloc/throw/block) — it publishes to these lock-free itc
    // channels and the control loop below surfaces them off-thread.
    itc::TelemetryCounter<int64_t> timer_error_count;
    itc::Published<int64_t> last_wake_count;
    itc::Published<int64_t> last_wake_error_ns;

    // Create PTP timer for realtime audio packet handling
    auto timer = ptpclient::make_ptp_timer(
        *ctx.bridge,
        PACKET_PERIOD_NS,
        [&](StatusValue<ptpclient::TimerWakeInfo> const& wake_info) {
            if (!wake_info) {
                timer_error_count.add(1);
                return;
            }

            auto const now = TimePoint{std::chrono::nanoseconds{wake_info->actual_time_ns}};

            // Process audio (DSP and packet handling)
            entity.process_audio(now);

            last_wake_count.publish(wake_info->wake_count);
            last_wake_error_ns.publish(wake_info->error_ns);
        },
        ctx.compensation_ns,
        ctx.enable_realtime,
        ctx.cpu_affinity);

    // Start PTP timer
    auto start_result = timer.start();
    if (!start_result) {
        std::print(stderr, "Error: Failed to start timer: {}\n", start_result.error().message());
        result.exit_code = EXIT_FAILURE;
        return result;
    }

    // ---- Optional dedicated RT RX timer (stream_rx.rt_timer) -------------------
    // Drain AM824 on its own SCHED_FIFO core so the reactor/control-plane can't delay
    // RX (which otherwise batch-tallies queued frames against a now-later clock and
    // reads as spurious LATE_TIMESTAMP). Stats mirror the media timer.
    itc::TelemetryCounter<int64_t> rx_timer_error_count;
    itc::TelemetryCounter<uint64_t> rx_frames_drained;
    itc::Published<int64_t> rx_last_wake_error_ns;
    itc::Published<int64_t> rx_last_batch;

    int64_t const RX_PERIOD_NS = static_cast<int64_t>(config.entity.stream_rx_period_us) * 1'000;
    auto rx_timer = ptpclient::make_ptp_timer(
        *ctx.bridge,
        RX_PERIOD_NS,
        [&](StatusValue<ptpclient::TimerWakeInfo> const& wake_info) {
            if (!wake_info) {
                rx_timer_error_count.add(1);
                return;
            }
            size_t const n = entity.drain_stream_rx(wake_info->actual_time_ns);
            rx_frames_drained.add(n);
            rx_last_batch.publish(static_cast<int64_t>(n));
            rx_last_wake_error_ns.publish(wake_info->error_ns);
        },
        ctx.compensation_ns,
        ctx.enable_realtime,
        config.entity.stream_rx_cpu_affinity);

    if (config.entity.stream_rx_rt_timer) {
        if (auto const rs = rx_timer.start(); !rs) {
            std::print(stderr, "Warning: stream RX timer failed to start ({}); RX stays on the reactor\n", rs.error().message());
        } else {
            std::print(
                "Stream RX: dedicated SCHED_FIFO timer on core {} @ {} us tick\n",
                config.entity.stream_rx_cpu_affinity,
                config.entity.stream_rx_period_us);
        }
    }

    // Simulate link up on startup
    auto startup_time = TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    entity.on_link_up(startup_time);

    // Watchdog timeout configuration (gPTP lock only; there is no VLAN gate).
    constexpr auto GPTP_LOCK_TIMEOUT = std::chrono::seconds{10};
    auto last_state_change_time = std::chrono::steady_clock::now();
    auto last_state = entity.state_string();
    auto last_telemetry_time = std::chrono::steady_clock::now();

    // Wire up gPTP announce callback to update ADP advertiser
    if (net_handlers != nullptr) {
        net_handlers->gptp_handler().set_callbacks(nanoavb::GptpAnnounceCallbacks{
            .grandmaster_id_changed =
                [&entity](int64_t now_ns, gptp::ClockIdentity const& grandmaster_id, gptp::AnnounceMessage const& announce) {
                    std::string gm_str;
                    gptp::format_to(std::back_inserter(gm_str), grandmaster_id);
                    std::print(
                        "gPTP: Grandmaster changed to {} (priority1={}, priority2={})\n",
                        gm_str,
                        announce.grandmaster_priority1.get(),
                        announce.grandmaster_priority2.get());

                    // Update ADP advertiser with new grandmaster info
                    entity.components().adp_advertiser.set_gptp_info(grandmaster_id, 0);
                    entity.components().adp_advertiser.notify_entity_changed();

                    // Notify entity state machine
                    (void)now_ns;
                    auto const sm_now = std::chrono::steady_clock::now();
                    entity.on_gptp_announce(sm_now, true);
                }});
    }

    // Poll reactor for network I/O until shutdown
    // Drain the entity's SPSC log channels (ctl = reactor thread, media = RT
    // media timer) here on the main thread — the entities/libraries only ever
    // enqueue; formatting and stderr I/O happen in this loop.
    logging::LogCollector<4> log_collector;
    logging::StderrSink log_sink;
    (void)log_collector.add(entity.ctl_log_channel());
    (void)log_collector.add(entity.media_log_channel());
    entity.set_log_verbosity(config.verbose ? logging::LogLevel::Debug : logging::LogLevel::Status);

    while (!realtime::is_shutdown_requested()) {
        (void)log_collector.poll(log_sink);
        (void)reactor.poll_once(100);

        // Check for state changes to reset watchdog
        auto const current_state = entity.state_string();
        if (current_state != last_state) {
            if (config.verbose) {
                std::print("State: {} -> {}\n", last_state, current_state);
            }
            last_state_change_time = std::chrono::steady_clock::now();
            last_state = current_state;
        }

        // Check for watchdog timeout in waiting states
        auto const now = std::chrono::steady_clock::now();
        auto const time_in_state = now - last_state_change_time;
        auto const sm_now = TimePoint{now.time_since_epoch()};

        // Grandmaster edge-detect runs here on the reactor/main thread (NOT the
        // SCHED_FIFO media-timer callback) so on_gptp_announce -> the non-atomic
        // SM/MSRP stack is only ever driven from one thread. First-acquire is
        // also handled promptly by the grandmaster_id_changed reactor callback;
        // this poll additionally covers GM regained with an unchanged identity.
        if (net_handlers != nullptr) {
            bool const has_gm = net_handlers->gptp_handler().has_grandmaster();
            if (has_gm && !had_grandmaster) {
                entity.on_gptp_announce(sm_now, has_gm);
            }
            had_grandmaster = has_gm;
        }

        if (current_state == "Init" && time_in_state > GPTP_LOCK_TIMEOUT) {
            entity.on_timeout(sm_now);
            last_state_change_time = now;
        }

        // Periodic PTP-bridge / timer telemetry on the main thread, so it keeps
        // reporting even when the media timer stalls (that's the diagnostic
        // point). stderr is unbuffered → visible immediately under systemd/nohup.
        if (config.verbose && ctx.bridge && now - last_telemetry_time >= std::chrono::seconds(1)) {
            last_telemetry_time = now;
            auto const bt = ctx.bridge->telemetry();
            std::print(
                stderr,
                "[bridge] healthy={} epoch={} samp={} rms={}ns bracket(min/mean/max)={}/{}/{}ns "
                "reject={} step={} last_step_resid={}ns overruns={} | timer recovery={} missed={}\n",
                bt.healthy,
                bt.epoch,
                bt.sample_count,
                bt.rms_residual_ns,
                bt.bracket_min_ns,
                bt.bracket_mean_ns,
                bt.bracket_max_ns,
                bt.reject_count,
                bt.step_count,
                bt.last_step_residual_ns,
                bt.regression_overruns,
                timer.recovery_count(),
                timer.missed_cycles());
            std::print(
                stderr,
                "[media] wake={} err={:+}ns state={} timer_errors={}\n",
                last_wake_count.load(),
                last_wake_error_ns.load(),
                entity.state_string(),
                timer_error_count.load());
            if (config.entity.stream_rx_rt_timer) {
                std::print(
                    stderr,
                    "[rx-timer] wake_err={:+}ns last_batch={} frames={} recovery={} missed={} errors={}\n",
                    rx_last_wake_error_ns.load(),
                    rx_last_batch.load(),
                    rx_frames_drained.load(),
                    rx_timer.recovery_count(),
                    rx_timer.missed_cycles(),
                    rx_timer_error_count.load());
            }
        }
    }

    if (config.entity.stream_rx_rt_timer) {
        rx_timer.stop();
        std::print(
            stderr,
            "[rx-timer] final: frames_drained={} wake_errors={} recovery={} missed={}\n",
            rx_frames_drained.load(),
            rx_timer_error_count.load(),
            rx_timer.recovery_count(),
            rx_timer.missed_cycles());
    }

    // Final drain: flush log entries produced during shutdown.
    (void)log_collector.poll(log_sink);

    // Stop timer and capture statistics
    timer.stop();
    if (auto const errs = timer_error_count.load(); errs > 0) {
        std::print(stderr, "Warning: media timer had {} wake error(s) (likely gPTP sync loss)\n", errs);
    }
    result.wake_stats = timer.stats();
    result.recovery_count = timer.recovery_count();
    result.missed_cycles = timer.missed_cycles();
    ctx.guard.stop();

    return result;
}

//
// Final Status Display
//

void print_final_status(avb_entity::AvbEntityAm824IO const& entity, MainLoopResult const& result, bool dump_stats)
{
    std::print("\n=== Final Status ===\n");
    std::print("  Entity State:    {}\n", entity.state_string());
    std::print("  Timer Recovery:  {}\n", result.recovery_count);
    std::print("  Missed Cycles:   {}\n", result.missed_cycles);
    entity.print_state();
    std::print("====================\n");

    if (dump_stats) {
        std::string stats_report;
        result.wake_stats.format_report_to(std::back_inserter(stats_report), result.compensation_ns);
        std::print("{}", stats_report);
    }
}

}  // namespace

//
// Main Entry Point
//

auto main(int argc, char** argv) -> int
{
    using namespace statusbar;

    // Build config and specs with bindings
    Config config;
    auto specs = build_arg_specs(config);

    // Parse CLI arguments
    auto cli_result = config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-avb-am824-io");
    if (!cli_result) {
        return config::handled_builtin_command(cli_result) ? 0 : 1;
    }

    if (config.entity.interface_name.empty()) {
        std::print(stderr, "Error: --interface=<iface> is required\n");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }

    // Validate required --descriptor-storage argument
    if (config.descriptor_storage_path.empty()) {
        std::print(stderr, "Error: --descriptor-storage=<path> is required\n");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }

    // Per-node-unique entity_id (NIC MAC → modified EUI-64) + hostname name,
    // unless overridden by --entity.id / --entity.name.
    avb_entity::apply_node_identity_defaults(
        config.entity.interface_name,
        config.entity.entity_id,
        config.entity.entity_name,
        ieee::Eui64{0x70, 0xB3, 0xD5, 0xED, 0xCF, 0x00, 0x00, 0x02},
        "AVB AM824 IO Tool");

    // Load the descriptor storage blob
    auto blob = load_file(config.descriptor_storage_path);
    if (blob.empty()) {
        std::print(stderr, "Error: Failed to load descriptor storage file: {}\n", config.descriptor_storage_path);
        return EXIT_FAILURE;
    }
    config.entity.descriptor_storage_blob = std::move(blob);

    // Set up signal handlers for clean shutdown
    realtime::setup_shutdown_signal_handlers();

    // Create the AVB entity via factory
    auto entity_result = avb_entity::AvbEntityAm824IO::create(std::move(config.entity));
    if (!entity_result) {
        std::print(stderr, "Error: Failed to create entity: {}\n", entity_result.error().message());
        return EXIT_FAILURE;
    }
    auto& entity = **entity_result;

    // Print configuration
    print_entity_config(config, entity);

    // Print PTP configuration
    std::string ptp_summary;
    ptpclient::format_ptp_config_to(std::back_inserter(ptp_summary), config.ptp_app, "\nPTP Configuration");
    std::print("{}\n", ptp_summary);

    // Create message reactor with shutdown flag and monotonic clock
    itc::StopToken shutdown_flag{};
    net::MessageReactor reactor{shutdown_flag, net::monotonic_ns, 10};

    // Start the entity (adds handlers to reactor)
    auto start_result = entity.start(reactor);
    if (!start_result) {
        std::print(stderr, "Error: Failed to start entity: {}\n", start_result.error().message());
        return EXIT_FAILURE;
    }

    std::print("\nEntity started. Network handlers registered.\n");

    // Setup PTP application with automatic fallback to system clock
    auto is_shutdown = [] { return realtime::is_shutdown_requested(); };
    auto ctx_result = ptpclient::setup_ptp_app_with_fallback(config.ptp_app, is_shutdown);

    if (!ctx_result) {
        if (realtime::is_shutdown_requested()) {
            std::print("Shutdown requested during setup\n");
            (void)entity.stop();
            return EXIT_SUCCESS;
        }
        std::print(stderr, "Error: Failed to setup PTP: {}\n", ctx_result.error().message());
        (void)entity.stop();
        return EXIT_FAILURE;
    }

    auto& ctx = *ctx_result;

    std::print("\nStarting main loop (Ctrl-C to stop)...\n\n");

    // Run main loop with PTP timer
    auto loop_result = run_main_loop(reactor, ctx, entity, config);

    // Clean shutdown
    std::print("\n\nShutting down...\n");

    // Notify entity of shutdown
    auto stop_time = TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    entity.on_link_down(stop_time);

    // Stop entity
    (void)entity.stop();
    std::print("Entity stopped.\n");

    // Print final status
    print_final_status(entity, loop_result, config.dump_stats_on_exit);

    return loop_result.exit_code;
}
