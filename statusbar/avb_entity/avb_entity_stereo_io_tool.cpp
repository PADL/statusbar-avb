// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AVB Entity Stereo I/O Tool - Runs an AVB entity with stereo audio passthrough and DSP
//
// Usage: avb_entity_stereo_io_tool [options]
//
// On Linux with hardware PTP:
//   avb_entity_stereo_io_tool --ptp.driver=linuxptp --ptp.device=/dev/ptp0
//
// On any platform (uses system clock):
//   avb_entity_stereo_io_tool --ptp.driver=system
//
// This tool instantiates AvbEntityStereoIO which provides:
// - One stereo 48 kHz AM824 listener stream (input)
// - One stereo 48 kHz AM824 talker stream (output)
// - Configurable DSP biquad filter (peak EQ)
// - Full ATDECC support (ADP, ACMP, AECP/AEM)
// - gPTP synchronization
// - MVRP/MSRP stream reservation

#include "statusbar/avb_entity/avb_entity_identity.hpp"
#include "statusbar/avb_entity/avb_entity_runner.hpp"
#include "statusbar/avb_entity/avb_entity_stereo_io.hpp"
#include "statusbar/avb_entity/avb_entity_version.hpp"
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
#include <functional>
#include <iterator>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using namespace statusbar;
using TimePoint = sm::TimePoint;

//
// Configuration
//

struct Config
{
    // PTP timer configuration
    ptpclient::PtpAppConfig ptp_app;

    // AVB Entity configuration
    avb_entity::AvbEntityStereoIOConfig entity{
        // entity_id unset → derived per-node from the NIC MAC at startup (so two
        // nodes don't collide); overridable with --entity.id.
        .entity_id = ieee::Eui64{},
        .entity_model_id = ieee::Eui64{0x70, 0xB3, 0xD5, 0xED, 0xCF, 0x00, 0x00, 0x00},
        // interface_name has no default — --interface is required.
        // JDKS OUI-36 multicast (NOT the 91:E0:F0 MAAP pool, which must be claimed
        // via MAAP before use); like the audio_io tool's range.
        .talker_dest_mac = {0x71, 0xB3, 0xD5, 0xED, 0xCF, 0xF2},
        .vlan_id = 2,
        .filter_freq_hz = 1000.0,
        .filter_gain_db = 0.0,  // Unity gain by default
        .filter_q = 0.707,      // Butterworth Q
        .entity_name = "AVB Stereo IO Tool",
        .firmware_version = std::string{statusbar::avb_entity::build_version},
    };

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
    avb_entity::add_stream_transport_arg_specs(specs, config.entity.stream_transport, config.entity.stream_xdp_mode);

    return specs;
}

//
// Help and Usage
//

void print_usage(char const* program_name, args::ArgumentSpecs const& specs)
{
#if defined(__linux__)
    static constexpr std::array<std::string_view, 3> examples{
        "--ptp.driver=linuxptp --ptp.device=/dev/ptp0",
        "--interface=eth0 --filter.gain_db=-6",
        "--ptp.driver=system",
    };
#else
    static constexpr std::array<std::string_view, 2> examples{
        "--interface=eth0 --filter.gain_db=-6",
        "--ptp.driver=system",
    };
#endif
    config::default_print_usage(
        program_name,
        specs,
        "AVB Entity Stereo I/O Tool\n"
        "Runs an AVB entity with stereo audio passthrough and DSP processing.",
        examples);

    std::print(stderr, "\nPress Ctrl-C to stop.\n");
}

//
// Configuration Display
//

void print_entity_config(Config const& config)
{
    std::print("AVB Entity Stereo I/O Tool\n");
    std::print("==========================\n");
    std::print("Entity ID:        {}\n", ieee::to_string(config.entity.entity_id).view());
    std::print("Entity Model ID:  {}\n", ieee::to_string(config.entity.entity_model_id).view());
    std::print("Entity Name:      {}\n", config.entity.entity_name);
    std::print("Interface:        {}\n", config.entity.interface_name);
    std::print("VLAN ID:          {}\n", config.entity.vlan_id);
    std::print("Talker Dest MAC:  {}\n", ieee::to_string(config.entity.talker_dest_mac).view());
    std::print("\nDSP Filter:\n");
    std::print("  Frequency:      {:.1f} Hz\n", config.entity.filter_freq_hz);
    std::print("  Gain:           {:.1f} dB\n", config.entity.filter_gain_db);
    std::print("  Q:              {:.3f}\n", config.entity.filter_q);
    std::print("\nAudio Format:\n");
    std::print("  Sample Rate:    {} Hz\n", avb_entity::AvbEntityStereoIO::SAMPLE_RATE);
    std::print("  Channels:       {} (stereo)\n", avb_entity::AvbEntityStereoIO::CHANNELS);
    std::print("  Samples/Packet: {}\n", avb_entity::AvbEntityStereoIO::SAMPLES_PER_PACKET);
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

/// What the optional RT RX timer publishes for the control loop.
struct RxTimerTelemetry
{
    itc::TelemetryCounter<int64_t> errors;
    itc::TelemetryCounter<uint64_t> frames_drained;
    itc::Published<int64_t> last_wake_error_ns;
    itc::Published<int64_t> last_batch;
};

/// Everything the RT timers publish (they must NOT std::print: alloc/throw/block).
struct ToolTelemetry
{
    avb_entity::MediaTimerTelemetry media{};
    RxTimerTelemetry rx{};
};

/// The control loop's watch on the entity (main thread only).
struct ToolWatch
{
    std::string_view last_state;
    std::chrono::steady_clock::time_point last_state_change{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_telemetry{std::chrono::steady_clock::now()};
    bool had_grandmaster{false};
};

/// The PTP media timer: process_audio (DSP and packet handling) every 125 us, 6 samples at 48 kHz.
auto make_media_timer(ptpclient::PtpAppContext& ctx, avb_entity::AvbEntityStereoIO& entity, avb_entity::MediaTimerTelemetry& media)
{
    constexpr int64_t PACKET_PERIOD_NS = 125'000;
    return ptpclient::make_ptp_timer(
        *ctx.bridge,
        PACKET_PERIOD_NS,
        [&entity, &media](StatusValue<ptpclient::TimerWakeInfo> const& wake_info) {
            if (!wake_info) {
                media.errors.add(1);
                return;
            }
            entity.process_audio(TimePoint{std::chrono::nanoseconds{wake_info->actual_time_ns}});
            media.wake_count.publish(wake_info->wake_count);
            media.wake_error_ns.publish(wake_info->error_ns);
        },
        ctx.compensation_ns,
        ctx.enable_realtime,
        ctx.cpu_affinity);
}

/// The optional dedicated RT RX timer (stream_rx.rt_timer) drains AM824 on its own SCHED_FIFO
/// core, so the reactor cannot delay RX (which otherwise batch-tallies queued frames against a
/// now-later clock and reads as spurious LATE_TIMESTAMP). Stats mirror the media timer.
auto make_rx_timer(ptpclient::PtpAppContext& ctx, avb_entity::AvbEntityStereoIO& entity, Config const& config, RxTimerTelemetry& rx)
{
    int64_t const RX_PERIOD_NS = static_cast<int64_t>(config.entity.stream_rx_period_us) * 1'000;
    return ptpclient::make_ptp_timer(
        *ctx.bridge,
        RX_PERIOD_NS,
        [&entity, &rx](StatusValue<ptpclient::TimerWakeInfo> const& wake_info) {
            if (!wake_info) {
                rx.errors.add(1);
                return;
            }
            size_t const n = entity.drain_stream_rx(wake_info->actual_time_ns);
            rx.frames_drained.add(n);
            rx.last_batch.publish(static_cast<int64_t>(n));
            rx.last_wake_error_ns.publish(wake_info->error_ns);
        },
        ctx.compensation_ns,
        ctx.enable_realtime,
        config.entity.stream_rx_cpu_affinity);
}

/// Start the RX timer when configured; on failure RX stays on the reactor.
template <typename Timer>
void start_rx_timer(Timer& rx_timer, Config const& config)
{
    if (!config.entity.stream_rx_rt_timer) {
        return;
    }
    rx_timer.set_thread_name("stream-rx");
    if (auto const rs = rx_timer.start(); !rs) {
        std::print(stderr, "Warning: stream RX timer failed to start ({}); RX stays on the reactor\n", rs.error().message());
    } else {
        std::print(
            "Stream RX: dedicated SCHED_FIFO timer on core {} @ {} us tick\n",
            config.entity.stream_rx_cpu_affinity,
            config.entity.stream_rx_period_us);
    }
}

/// Stop the RX timer, if it ran, and report its totals.
template <typename Timer>
void stop_rx_timer(Timer& rx_timer, Config const& config, RxTimerTelemetry const& rx)
{
    if (!config.entity.stream_rx_rt_timer) {
        return;
    }
    rx_timer.stop();
    std::print(
        stderr,
        "[rx-timer] final: frames_drained={} wake_errors={} recovery={} missed={}\n",
        rx.frames_drained.load(),
        rx.errors.load(),
        rx_timer.recovery_count(),
        rx_timer.missed_cycles());
}

/// Note a state change: it resets the watchdog.
void observe_state(ToolWatch& watch, avb_entity::AvbEntityStereoIO const& entity, bool const verbose)
{
    auto const current_state = entity.state_string();
    if (current_state == watch.last_state) {
        return;
    }
    if (verbose) {
        std::print("State: {} -> {}\n", watch.last_state, current_state);
    }
    watch.last_state_change = std::chrono::steady_clock::now();
    watch.last_state = current_state;
}

/// Grandmaster edge-detect, then the watchdog (gPTP lock only; there is no VLAN gate).
void supervise(ToolWatch& watch, avb_entity::AvbEntityStereoIO& entity, std::chrono::steady_clock::time_point const now)
{
    constexpr auto GPTP_LOCK_TIMEOUT = std::chrono::seconds{10};
    auto const sm_now = TimePoint{now.time_since_epoch()};
    avb_entity::detect_grandmaster_edge(entity, watch.had_grandmaster, sm_now);
    if (watch.last_state == "Init" && now - watch.last_state_change > GPTP_LOCK_TIMEOUT) {
        entity.on_timeout(sm_now);
        watch.last_state_change = now;
    }
}

/// Report the RX timer's counters (when it runs).
template <typename Timer>
void report_rx_timer(Timer const& rx_timer, Config const& config, RxTimerTelemetry const& rx)
{
    if (!config.entity.stream_rx_rt_timer) {
        return;
    }
    std::print(
        stderr,
        "[rx-timer] wake_err={:+}ns last_batch={} frames={} recovery={} missed={} errors={}\n",
        rx.last_wake_error_ns.load(),
        rx.last_batch.load(),
        rx.frames_drained.load(),
        rx_timer.recovery_count(),
        rx_timer.missed_cycles(),
        rx.errors.load());
}

/// Once a second: surface the media timer's published wake stats off the RT thread.
template <typename Timer, typename RxTimer>
void report_telemetry(
    ToolWatch& watch,
    std::chrono::steady_clock::time_point const now,
    ptpclient::PtpAppContext& /*ctx*/,
    Timer const& timer,
    RxTimer const& rx_timer,
    ToolTelemetry const& telemetry,
    Config const& config)
{
    if (now - watch.last_telemetry < std::chrono::seconds(1)) {
        return;
    }
    watch.last_telemetry = now;
    std::print(
        stderr,
        "[media] wake={} err={:+}ns state={} timer_errors={} recovery={} missed={}\n",
        telemetry.media.wake_count.load(),
        telemetry.media.wake_error_ns.load(),
        watch.last_state,
        telemetry.media.errors.load(),
        timer.recovery_count(),
        timer.missed_cycles());
    report_rx_timer(rx_timer, config, telemetry.rx);
}

/// Whether the entity has a grandmaster now (false without network handlers).
[[nodiscard]] auto had_grandmaster(avb_entity::AvbEntityStereoIO& entity) -> bool
{
    auto* const net_handlers = entity.net_handlers();
    return net_handlers != nullptr && net_handlers->gptp_handler().has_grandmaster();
}

/// One pass of the control loop: logs, reactor, state, watchdog and (verbose) telemetry.
template <typename Timer, typename RxTimer>
void poll_once(
    ToolWatch& watch,
    net::MessageReactor& reactor,
    ptpclient::PtpAppContext& ctx,
    avb_entity::AvbEntityStereoIO& entity,
    Config const& config,
    Timer const& timer,
    RxTimer const& rx_timer,
    ToolTelemetry const& telemetry,
    avb_entity::LogDrain& logs)
{
    logs.poll();
    (void)reactor.poll_once(100);
    observe_state(watch, entity, config.verbose);
    auto const now = std::chrono::steady_clock::now();
    supervise(watch, entity, now);
    if (config.verbose) {
        report_telemetry(watch, now, ctx, timer, rx_timer, telemetry, config);
    }
}

/// Poll the logs and the reactor until shutdown is requested, then stop the RX timer and
/// drain the logs once more.
template <typename Timer, typename RxTimer>
void run_control_loop(
    net::MessageReactor& reactor,
    ptpclient::PtpAppContext& ctx,
    avb_entity::AvbEntityStereoIO& entity,
    Config const& config,
    Timer const& timer,
    RxTimer& rx_timer,
    ToolTelemetry const& telemetry)
{
    ToolWatch watch{.last_state = entity.state_string(), .had_grandmaster = had_grandmaster(entity)};
    avb_entity::LogDrain logs;
    avb_entity::attach_log_channels(logs, entity, config.verbose);
    while (!realtime::is_shutdown_requested()) {
        poll_once(watch, reactor, ctx, entity, config, timer, rx_timer, telemetry, logs);
    }
    stop_rx_timer(rx_timer, config, telemetry.rx);
    logs.poll();  // flush log entries produced during shutdown
}

/// Run the RX timer, the simulated link-up and the grandmaster announce around the control loop.
template <typename Timer>
void run_with_rx_timer(
    net::MessageReactor& reactor,
    ptpclient::PtpAppContext& ctx,
    avb_entity::AvbEntityStereoIO& entity,
    Config const& config,
    Timer const& timer,
    ToolTelemetry& telemetry)
{
    auto rx_timer = make_rx_timer(ctx, entity, config, telemetry.rx);
    start_rx_timer(rx_timer, config);
    entity.on_link_up(TimePoint{std::chrono::steady_clock::now().time_since_epoch()});  // simulated
    avb_entity::wire_grandmaster_announce(entity);
    run_control_loop(reactor, ctx, entity, config, timer, rx_timer, telemetry);
}

/// Stop the media timer and PTP sampling and collect the timer's statistics.
template <typename Timer>
auto stop_media_timer(Timer& timer, avb_entity::MediaTimerTelemetry const& media, ptpclient::PtpAppContext& ctx) -> MainLoopResult
{
    timer.stop();
    if (auto const errs = media.errors.load(); errs > 0) {
        std::print(stderr, "Warning: media timer had {} wake error(s) (likely gPTP sync loss)\n", errs);
    }
    MainLoopResult result{
        .wake_stats = timer.stats(),
        .compensation_ns = ctx.compensation_ns,
        .recovery_count = timer.recovery_count(),
        .missed_cycles = timer.missed_cycles()};
    ctx.guard.stop();
    return result;
}

MainLoopResult run_main_loop(
    net::MessageReactor& reactor, ptpclient::PtpAppContext& ctx, avb_entity::AvbEntityStereoIO& entity, Config const& config)
{
    ToolTelemetry telemetry{};
    auto timer = make_media_timer(ctx, entity, telemetry.media);
    if (auto const started = timer.start(); !started) {
        std::print(stderr, "Error: Failed to start timer: {}\n", started.error().message());
        return MainLoopResult{.exit_code = EXIT_FAILURE, .compensation_ns = ctx.compensation_ns};
    }
    run_with_rx_timer(reactor, ctx, entity, config, timer, telemetry);
    return stop_media_timer(timer, telemetry.media, ctx);
}

//
// Final Status Display
//

void print_final_status(avb_entity::AvbEntityStereoIO const& entity, MainLoopResult const& result, bool dump_stats)
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
    auto cli_result = config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-avb-stereo-io");
    if (!cli_result) {
        return config::handled_builtin_command(cli_result) ? 0 : 1;
    }

    if (config.entity.interface_name.empty()) {
        std::print(stderr, "Error: --interface=<iface> is required\n");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }

    // Per-node-unique entity_id (NIC MAC → modified EUI-64) + hostname name,
    // unless overridden by --entity.id / --entity.name.
    avb_entity::apply_node_identity_defaults(
        config.entity.interface_name,
        config.entity.entity_id,
        config.entity.entity_name,
        ieee::Eui64{0x70, 0xB3, 0xD5, 0xED, 0xCF, 0x00, 0x00, 0x01},
        "AVB Stereo IO Tool");

    // Set up signal handlers for clean shutdown
    realtime::setup_shutdown_signal_handlers();

    // Print configuration
    print_entity_config(config);

    // Print PTP configuration
    std::string ptp_summary;
    ptpclient::format_ptp_config_to(std::back_inserter(ptp_summary), config.ptp_app, "\nPTP Configuration");
    std::print("{}\n", ptp_summary);

    // Create the AVB entity
    avb_entity::AvbEntityStereoIO entity{config.entity};

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