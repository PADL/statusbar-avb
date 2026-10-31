#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Shared tool harness for the kit entity tools (kit phase 5d).
///
/// Every entity tool used to hand-roll the same ~400 lines: the common CLI
/// argument specs, blob loading, PTP setup with the system-clock fallback
/// warning, the PTP media timer driving Entity::process_audio on its
/// SCHED_FIFO thread, the link monitor, gPTP grandmaster edge-detect, SPSC
/// log draining, TX-pcap flushing, periodic telemetry, shutdown and the
/// exit-time histogram CSV dumps. This header owns all of that once; a
/// tool keeps only its entity-specific arguments, banner and create()
/// call, plus optional hooks for extra log channels / telemetry / polling.
///
/// The Entity template parameter needs the kit entity surface:
/// process_audio(TimePoint), on_link_up/down, on_gptp_announce,
/// on_timeout, state_string, print_state, components(), net_handlers(),
/// ctl/media log channels, set_log_verbosity, tx_pcap_* accessors.

#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/buffer/stream_utils.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/gptp/gptp_format.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_published.hpp"
#include "statusbar/itc/itc_telemetry_counter.hpp"
#include "statusbar/logging/logging_collector.hpp"
#include "statusbar/logging/logging_sink.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/net/net_link_monitor.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/realtime/realtime.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/stats/stats_atomic_histogram.hpp"
#include "statusbar/stats/stats_atomic_wake_stats.hpp"
#include "statusbar/status/status.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace statusbar::avb_entity {

/// Load a whole file (the descriptor blob) into memory; empty on failure.
[[nodiscard]] inline auto load_blob_file(std::filesystem::path const& path) -> std::vector<uint8_t>
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

/// Dump an AtomicHistogram snapshot as bin_low_ns,bin_high_ns,count CSV.
[[nodiscard]] inline auto write_histogram_csv(std::string const& path, stats::AtomicHistogram<128>::Snapshot const& hist) -> bool
{
    std::ofstream out{path};
    if (!out) {
        std::println(stderr, "warning: cannot open {} for writing", path);
        return false;
    }
    out << "bin_low_ns,bin_high_ns,count\n";
    out << ',' << hist.config.low_ns << ',' << hist.underflow << '\n';
    for (size_t i = 0; i < hist.num_bins; ++i) {
        out << hist.bin_low(i) << ',' << hist.bin_high(i) << ',' << hist.bins[i] << '\n';
    }
    out << hist.config.high_ns << ',' << ',' << hist.overflow << '\n';
    return static_cast<bool>(out);
}

/// The tool-level knobs every entity tool shares (parsed alongside the
/// entity config by add_common_entity_arg_specs).
struct EntityToolOptions
{
    std::string descriptor_storage_path{};
    bool verbose{true};
    bool dump_stats_on_exit{true};
    bool require_ptp{false};
    std::string media_timer_wake_csv_path{};
    std::string media_timer_duration_csv_path{};
};

/// Register --stream-transport and --stream-xdp-mode, setting @p transport and
/// @p xdp_mode (the am824 and stereo tools call it on their own configs).
inline void add_stream_transport_arg_specs(args::ArgumentSpecs& specs, StreamTransport& transport, StreamXdpMode& xdp_mode)
{
    specs.add_choice(
        "stream-transport",
        "Stream link: 'packet' (AF_PACKET), 'xdp' (receive on an AF_XDP socket, drained by the media timer; refuses "
        "stream_rx.rt_timer) or 'auto' (xdp when it opens, else packet). Default packet.",
        {"packet", "xdp", "auto"},
        stream_transport_name(transport),
        [&transport](std::string_view const v) { transport = parse_stream_transport(v).value_or(StreamTransport::packet); });
    specs.add_choice(
        "stream-xdp-mode",
        "AF_XDP bind: 'auto' (native attach, zero-copy when the driver offers it), 'zerocopy' (or fail) or 'copy' "
        "(generic attach, no adapter reset). Default auto.",
        {"auto", "zerocopy", "copy"},
        stream_xdp_mode_name(xdp_mode),
        [&xdp_mode](std::string_view const v) { xdp_mode = parse_stream_xdp_mode(v).value_or(StreamXdpMode::auto_); });
}

/// Register the CLI arguments every entity tool shares: entity identity,
/// interface, ATDECC wire version, VLAN, stream addressing, SRP
/// workarounds, the talker gate, stream destination MACs, the stream
/// transport, presentation offset, media pacing, listener bindings, TX pcap, histogram CSVs and
/// the verbosity/stats/PTP-strictness flags. Tools add their own specs
/// (and the PTP specs) around this.
inline void add_common_entity_arg_specs(args::ArgumentSpecs& specs, AvbEntityAudioIOConfig& entity, EntityToolOptions& tool)
{  // NOLINT(statusbar-block-statements): one registration per shared argument
    specs.add<ieee::Eui64>("entity.id", "Entity ID (EUI-64)", entity.entity_id, [&entity](auto v) { entity.entity_id = v; });
    specs.add<ieee::Eui64>(
        "entity.model_id", "Entity Model ID (EUI-64)", entity.entity_model_id, [&entity](auto v) { entity.entity_model_id = v; });
    specs.add_device(
        "interface", "Network interface (required, e.g. eth0)", "", [&entity](auto v) { entity.interface_name = std::string{v}; });
    specs.add<std::string>("entity.name", "Entity name (shown in ATDECC controllers)", entity.entity_name, [&entity](auto v) {
        entity.entity_name = v;
    });
    specs.add<std::string>(
        "atdecc.version",
        "ATDECC wire version: '2016' truncates descriptors to their 1722.1-2013/2016 lengths; '2021' emits full "
        "2021 descriptors. Default 2016.",
        entity.atdecc_version,
        [&entity](auto v) { entity.atdecc_version = std::string{v}; });
    specs.add<uint16_t>("vlan_id", "VLAN ID for AVB streams", entity.vlan_id, [&entity](auto v) { entity.vlan_id = v; });
    specs.add<std::string>(
        "stream.address_mode",
        "Talker stream dest-address source: 'static' (the configured *_dest_mac) or 'maap' (acquire a block from "
        "the 1722 dynamic pool via MAAP at startup). Default static.",
        entity.stream_address_mode,
        [&entity](auto v) { entity.stream_address_mode = std::string{v}; });
    add_stream_transport_arg_specs(specs, entity.stream_transport, entity.stream_xdp_mode);
    specs.add<bool>(
        "srp.redeclare_registered_listeners",
        "Sticky-Listener MSRP workaround: re-declare REGISTERED Listener attributes every periodic/LeaveAll pass. "
        "Needed when feeding a downstream listener through some AVB switches. Default off.",
        entity.redeclare_registered_listeners,
        [&entity](auto v) { entity.redeclare_registered_listeners = v; });
    specs.add<bool>(
        "srp.suppress_leaveall",
        "Suppress-LeaveAll MSRP workaround: never originate a periodic LeaveAll. Default off.",
        entity.suppress_leaveall,
        [&entity](auto v) { entity.suppress_leaveall = v; });
    specs.add<bool>(
        "gate.talker_on_listener",
        "Transmit a talker stream only while an MSRP Listener Ready or Ready Failed is registered for it (with "
        "grace); stay silent otherwise (IEEE 802.1Q SRP, Milan 1.2 5.3.7.3). Each stream gates "
        "independently -- the CRF media clock on its own reservation, not on the audio talkers. "
        "Default true (false = stream unconditionally from link-up; non-spec, bench only).",
        entity.gate_talker_on_listener,
        [&entity](auto v) { entity.gate_talker_on_listener = v; });
    specs.add<ieee::Eui48>(
        "am824.dest_mac", "AM824 talker destination multicast MAC (stream 0)", entity.am824_talker_dest_mac, [&entity](auto v) {
            entity.am824_talker_dest_mac = v;
        });
    specs.add<ieee::Eui48>(
        "aaf.dest_mac", "AAF talker destination multicast MAC (stream 1)", entity.aaf_talker_dest_mac, [&entity](auto v) {
            entity.aaf_talker_dest_mac = v;
        });
    specs.add<ieee::Eui48>(
        "crf.dest_mac",
        "CRF media-clock talker destination multicast MAC (stream 2)",
        entity.crf_talker_dest_mac,
        [&entity](auto v) { entity.crf_talker_dest_mac = v; });
    specs.add<uint64_t>(
        "presentation_offset_ns",
        "AVTP presentation-time offset added to gPTP time to form the stream timestamp (ns)",
        entity.presentation_offset_ns,
        [&entity](auto v) { entity.presentation_offset_ns = v; });
    specs.add<uint64_t>(
        "media.packets_per_wake",
        "Stream packets per stream per media-timer wake (default 1 = strict Class A shaping).",
        static_cast<uint64_t>(entity.packets_per_wake),
        [&entity](auto v) { entity.packets_per_wake = static_cast<size_t>((v < 1) ? 1 : ((v > 16) ? 16 : v)); });
    specs.add<std::string>(
        "listener.bindings",
        "Persist listener fast-connect bindings to this file (kit phase 5b): controller-made input connections are "
        "remembered and fast-connected (IEEE 1722.1 8.2.2.1.1) after an entity or talker restart. Empty = off.",
        entity.listener_bindings_path,
        [&entity](auto v) { entity.listener_bindings_path = std::string{v}; });
    specs.add<std::string>(
        "persistent_state",
        "Keep the entity's Milan non-volatile settings (names, formats, rate, clock source, presentation offsets, "
        "listener bindings, system unique id, media clock reference) in this TOML file across restarts; imports "
        "listener.bindings once. Empty = off.",
        entity.persistent_state_path,
        [&entity](auto v) { entity.persistent_state_path = std::string{v}; });
    specs.add<std::string>(
        "atdecc.socket",
        "Serve local ATDECC controllers on an AF_UNIX datagram socket at this path (one AVTPDU per datagram, as "
        "la_avdecc's local protocol interface): statusbar-atdecc-monitor --socket=PATH reaches the entity without raw "
        "sockets. Empty = off.",
        entity.atdecc_socket_path,
        [&entity](auto v) { entity.atdecc_socket_path = std::string{v}; });
    specs.add<std::string>(
        "atdecc.socket_mode", "File mode of the atdecc.socket socket, in octal (default 0660).", "0660", [&entity](auto v) {
            std::string_view const text{v};
            unsigned mode = 0;
            auto const [end, ec] = std::from_chars(text.data(), text.data() + text.size(), mode, 8);
            if (ec != std::errc{} || end != text.data() + text.size() || mode > 0777U) {
                std::println(
                    stderr,
                    "warning: atdecc.socket_mode '{}' is not an octal mode; keeping {:04o}",
                    text,
                    entity.atdecc_socket_mode);
                return;
            }
            entity.atdecc_socket_mode = static_cast<int>(mode);
        });
    specs.add<std::string>(
        "mcr.default_priority",
        "Default Media Clock Reference Priority of each CLOCK_DOMAIN by index, 0-255, comma-separated; the last also "
        "serves the domains after it (Milan v1.3 Section 7.6.1.1: 192 audio interfaces, 224 signal routers). Empty = "
        "192.",
        "",
        [&entity](auto v) {
            if (auto parsed = parse_default_mcr_priorities(v)) {
                entity.default_mcr_priorities = std::move(*parsed);
                return;
            }
            std::println(stderr, "warning: mcr.default_priority '{}' is not a list of 0-255; keeping 192", std::string_view{v});
        });
    specs.add<std::string>(
        "tx_pcap.path",
        "Diagnostic: record our OWN transmitted stream frames (AAF/AM824/CRF, normally invisible because the TX "
        "socket is PACKET_QDISC_BYPASS) to this libpcap file, gPTP-timestamped, for tx_pcap.seconds. Empty = off.",
        entity.tx_pcap_path,
        [&entity](auto v) { entity.tx_pcap_path = std::string{v}; });
    specs.add<uint32_t>(
        "tx_pcap.seconds",
        "Capture window in seconds for tx_pcap.path, from the first transmitted frame (default 10).",
        entity.tx_pcap_seconds,
        [&entity](auto v) { entity.tx_pcap_seconds = v; });
    specs.add<std::string>(
        "media-timer-wake-csv",
        "Dump the 8 kHz media-timer wake-error histogram to this CSV at exit. Empty = off",
        tool.media_timer_wake_csv_path,
        [&tool](auto v) { tool.media_timer_wake_csv_path = v; });
    specs.add<std::string>(
        "media-timer-duration-csv",
        "Dump the media-timer callback-DURATION histogram to this CSV at exit",
        tool.media_timer_duration_csv_path,
        [&tool](auto v) { tool.media_timer_duration_csv_path = v; });
    specs.add<std::string>(
        "descriptor-storage",
        "Path to the AEM descriptor storage .bin blob (build with statusbar-aemxml json2bin)",
        tool.descriptor_storage_path,
        [&tool](auto v) { tool.descriptor_storage_path = v; });
    specs.add<bool>("verbose", "Enable verbose output", tool.verbose, [&tool](auto v) { tool.verbose = v; });
    specs.add<bool>("dump_stats_on_exit", "Dump timer statistics on exit", tool.dump_stats_on_exit, [&tool](auto v) {
        tool.dump_stats_on_exit = v;
    });
    specs.add<bool>(
        "ptp.require",
        "Refuse to run if PTP falls back to the system clock (talker timestamps would not be gPTP)",
        tool.require_ptp,
        [&tool](auto v) { tool.require_ptp = v; });
}

/// Set up the PTP app with the standard fallback flow: on a silent fall
/// back to the system clock, print the loud LATE_TIMESTAMP warning and —
/// when @p require_ptp — refuse to run. A failure return with shutdown
/// already requested means Ctrl-C during setup, not an error.
[[nodiscard]] inline auto setup_entity_ptp(ptpclient::PtpAppConfig& ptp_app, bool const require_ptp)
    -> StatusValue<ptpclient::PtpAppContext>
{
    auto is_shutdown = [] { return realtime::is_shutdown_requested(); };
    std::string const requested_driver = ptp_app.driver_name;
    std::string const requested_device = ptp_app.device_path;
    auto ctx_result = ptpclient::setup_ptp_app_with_fallback(ptp_app, is_shutdown);
    if (!ctx_result) {
        return ctx_result;
    }
    if (requested_driver != "system" && ptp_app.driver_name == "system") {
        std::print(
            stderr,
            "\n*** WARNING: PTP fell back to the SYSTEM clock -- could not open the gPTP PHC ({}).\n"
            "***          AVTP timestamps will be in the local clock domain, NOT gPTP, so listeners\n"
            "***          will report LATE_TIMESTAMP and the stream will not lock. Run as root or\n"
            "***          grant read access to the PHC (udev rule).\n\n",
            requested_device);
        if (require_ptp) {
            std::print(stderr, "Error: --ptp.require is set; refusing to stream without a gPTP clock.\n");
            return failure(std::errc::state_not_recoverable);
        }
    }
    return ctx_result;
}

/// What the shared loop reports back for the exit banner / CSVs.
struct MainLoopResult
{
    int exit_code{EXIT_SUCCESS};
    stats::AtomicWakeStats::Snapshot wake_stats{};
    int64_t compensation_ns{0};
    int64_t recovery_count{0};
    int64_t missed_cycles{0};
};

/// Optional tool-specific extensions to run_entity_main_loop. All run on
/// the reactor/main thread.
struct EntityRunnerHooks
{
    /// Register extra SPSC log channels (beyond ctl + media).
    /// Allocation-free inplace_function; captures must fit in 128 bytes
    /// (the tools bind by-reference lambdas over several telemetry locals).
    statusbar::sg14::inplace_function<void(logging::LogCollector<4>&), 128> add_log_channels{};
    /// Called once per loop iteration, after the reactor poll.
    statusbar::sg14::inplace_function<void(), 128> poll{};
    /// Called once per verbose telemetry tick, after the bridge line.
    statusbar::sg14::inplace_function<void(), 128> extra_telemetry{};
};

/// What a media-timer callback publishes for the control loop: it runs on a
/// SCHED_FIFO thread, so it must not std::print (alloc/throw/block).
struct MediaTimerTelemetry
{
    itc::TelemetryCounter<int64_t> errors;
    itc::Published<int64_t> wake_count;
    itc::Published<int64_t> wake_error_ns;
};

/// The entity's SPSC log channels (ctl = reactor thread, media = RT media timer),
/// drained on the main thread: the entities and libraries only ever enqueue.
struct LogDrain
{
    logging::LogCollector<4> collector;
    logging::StderrSink sink;

    void poll() { (void)collector.poll(sink); }
};

/// Add @p entity's ctl and media log channels to @p logs at the tool's verbosity.
template <typename Entity>
void attach_log_channels(LogDrain& logs, Entity& entity, bool const verbose)
{
    (void)logs.collector.add(entity.ctl_log_channel());
    (void)logs.collector.add(entity.media_log_channel());
    entity.set_log_verbosity(verbose ? logging::LogLevel::Debug : logging::LogLevel::Status);
}

/// On a grandmaster change, log it, update the ADP advertiser (with the Announce's domain)
/// and drive the entity.
template <typename Entity>
void wire_grandmaster_announce(Entity& entity)
{
    auto* const net_handlers = entity.net_handlers();
    if (net_handlers == nullptr) {
        return;
    }
    net_handlers->gptp_handler().set_callbacks(nanoavb::GptpAnnounceCallbacks{
        .grandmaster_id_changed =
            [&entity](int64_t /*now_ns*/, gptp::ClockIdentity const& grandmaster_id, gptp::AnnounceMessage const& announce) {
                std::string gm_str;
                gptp::format_to(std::back_inserter(gm_str), grandmaster_id);
                std::print(
                    "gPTP: Grandmaster changed to {} (priority1={}, priority2={})\n",
                    gm_str,
                    announce.grandmaster_priority1.get(),
                    announce.grandmaster_priority2.get());
                auto const sm_now = std::chrono::steady_clock::now();
                entity.components().adp_advertiser.set_gptp_info(grandmaster_id, announce.header.domain_number.get(), sm_now);
                entity.on_gptp_announce(sm_now, true);
            }});
}

/// Grandmaster edge-detect on the reactor/main thread (NOT the SCHED_FIFO media timer), so
/// on_gptp_announce drives the non-atomic SM/MSRP stack from one thread. It covers a
/// grandmaster regained with an unchanged identity; the announce callback covers the rest.
template <typename Entity>
void detect_grandmaster_edge(Entity& entity, bool& had_grandmaster, sm::TimePoint const now)
{
    auto* const net_handlers = entity.net_handlers();
    if (net_handlers == nullptr) {
        return;
    }
    bool const has_gm = net_handlers->gptp_handler().has_grandmaster();
    if (has_gm && !had_grandmaster) {
        entity.on_gptp_announce(now, has_gm);
    }
    had_grandmaster = has_gm;
}

/// What the runner's media timer publishes, plus its callback-duration histogram:
/// how long process_audio() takes per wake (the timer's stats() track only wake error).
struct RunnerTelemetry
{
    MediaTimerTelemetry media{};
    stats::AtomicHistogram<128> duration{stats::AtomicHistogramConfig{.low_ns = 0, .high_ns = 100'000, .bin_width_ns = 1000}};
};

/// The control loop's watch on the entity (reactor/main thread only).
struct RunnerWatch
{
    std::string_view last_state;
    std::chrono::steady_clock::time_point last_state_change{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_telemetry{std::chrono::steady_clock::now()};
    bool lock_timeout_reported{false};  ///< the gPTP watchdog fired in this stay in the state
    bool had_grandmaster{false};
};

/// The PTP media timer: Entity::process_audio on the SCHED_FIFO thread each @p period_ns.
template <typename Entity>
[[nodiscard]] auto make_runner_media_timer(
    ptpclient::PtpAppContext& ctx, Entity& entity, int64_t const period_ns, RunnerTelemetry& telemetry)
{
    return ptpclient::make_ptp_timer(
        *ctx.bridge,
        period_ns,
        [&entity, &telemetry](StatusValue<ptpclient::TimerWakeInfo> const& wake_info) {
            if (!wake_info) {
                telemetry.media.errors.add(1);
                return;
            }
            auto const cb_t0 = std::chrono::steady_clock::now();
            entity.process_audio(sm::TimePoint{std::chrono::nanoseconds{wake_info->actual_time_ns}});
            telemetry.duration.update(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - cb_t0).count());
            telemetry.media.wake_count.publish(wake_info->wake_count);
            telemetry.media.wake_error_ns.publish(wake_info->error_ns);
        },
        ctx.compensation_ns,
        ctx.enable_realtime,
        ctx.cpu_affinity);
}

/// Drive the initial link-up synchronously (before the reactor runs) so the supervisor
/// leaves Down -> Init before any gPTP announce is processed, then track the link state.
template <typename Entity>
void start_link_tracking(net::MessageReactor& reactor, Entity& entity)
{
    entity.on_link_up(sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()});
    reactor.add(std::make_unique<net::LinkMonitor>(entity.config().interface_name, [&entity](bool up, int64_t now_ns) {
        auto const tp = sm::TimePoint{std::chrono::nanoseconds{now_ns}};
        if (up) {
            entity.on_link_up(tp);
        } else {
            entity.on_link_down(tp);
        }
    }));
}

/// Track the link state and follow grandmaster changes.
template <typename Entity>
void follow_link_and_grandmaster(net::MessageReactor& reactor, Entity& entity)
{
    start_link_tracking(reactor, entity);
    wire_grandmaster_announce(entity);
}

/// TX pcap capture finished (window elapsed / ring full): write the file here, off the
/// media RT thread.
template <typename Entity>
void flush_tx_pcap_if_ready(Entity& entity)
{
    if (!entity.tx_pcap_ready_to_write()) {
        return;
    }
    auto const wr = entity.flush_tx_pcap();
    if (wr) {
        std::print("TX pcap: wrote {} frames to {}\n", entity.tx_pcap_frame_count(), entity.config().tx_pcap_path);
    } else {
        std::print(stderr, "TX pcap: write failed: {}\n", wr.error().message());
    }
}

/// Note a state change: it resets the gPTP watchdog.
template <typename Entity>
void observe_state(RunnerWatch& watch, Entity const& entity, bool const verbose)
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
    watch.lock_timeout_reported = false;
}

/// The gPTP watchdog reports once per stay in Init (as EntityUpkeep): on_timeout leaves the
/// entity in Init, so re-arming it would log the lock timeout every GPTP_LOCK_TIMEOUT.
template <typename Entity>
void supervise(RunnerWatch& watch, Entity& entity, std::chrono::steady_clock::time_point const now)
{
    constexpr auto GPTP_LOCK_TIMEOUT = std::chrono::seconds{10};
    auto const sm_now = sm::TimePoint{now.time_since_epoch()};
    detect_grandmaster_edge(entity, watch.had_grandmaster, sm_now);
    if (watch.last_state == "Init" && !watch.lock_timeout_reported && now - watch.last_state_change > GPTP_LOCK_TIMEOUT) {
        entity.on_timeout(sm_now);
        watch.lock_timeout_reported = true;
    }
}

/// Once a second when verbose: the PTP bridge, the media timer and the hooks' extras.
template <typename Entity, typename Timer>
void report_telemetry(
    RunnerWatch& watch,
    std::chrono::steady_clock::time_point const now,
    ptpclient::PtpAppContext& ctx,
    Entity const& entity,
    Timer const& timer,
    MediaTimerTelemetry const& media,
    EntityRunnerHooks const& hooks)
{
    if (!ctx.bridge || now - watch.last_telemetry < std::chrono::seconds(1)) {
        return;
    }
    watch.last_telemetry = now;
    auto const bt = ctx.bridge->telemetry();
    std::print(
        stderr,
        "[bridge] healthy={} epoch={} rms={}ns reject={} step={} overruns={} | timer recovery={} missed={} "
        "wake={} err={:+}ns state={} timer_errors={}\n",
        bt.healthy,
        bt.epoch,
        bt.rms_residual_ns,
        bt.reject_count,
        bt.step_count,
        bt.regression_overruns,
        timer.recovery_count(),
        timer.missed_cycles(),
        media.wake_count.load(),
        media.wake_error_ns.load(),
        entity.state_string(),
        media.errors.load());
    if (hooks.extra_telemetry) {
        hooks.extra_telemetry();
    }
}

/// The watch at the loop's start: the entity's state and whether it has a grandmaster.
template <typename Entity>
[[nodiscard]] auto make_runner_watch(Entity& entity) -> RunnerWatch
{
    auto* const net_handlers = entity.net_handlers();
    return RunnerWatch{
        .last_state = entity.state_string(),
        .had_grandmaster = net_handlers != nullptr && net_handlers->gptp_handler().has_grandmaster()};
}

/// One pass of the main thread's I/O: drain the logs, poll the reactor, run the hook.
inline void poll_io(LogDrain& logs, net::MessageReactor& reactor, EntityRunnerHooks const& hooks)
{
    logs.poll();
    (void)reactor.poll_once(100);
    if (hooks.poll) {
        hooks.poll();
    }
}

/// Poll the logs, the reactor and the hooks until shutdown is requested; drain the logs once more.
template <typename Entity, typename Timer>
void run_runner_control_loop(
    net::MessageReactor& reactor,
    ptpclient::PtpAppContext& ctx,
    Entity& entity,
    EntityToolOptions const& tool,
    EntityRunnerHooks const& hooks,
    Timer const& timer,
    MediaTimerTelemetry const& media)
{
    RunnerWatch watch = make_runner_watch(entity);
    LogDrain logs;
    attach_log_channels(logs, entity, tool.verbose);
    if (hooks.add_log_channels) {
        hooks.add_log_channels(logs.collector);
    }
    while (!realtime::is_shutdown_requested()) {
        poll_io(logs, reactor, hooks);
        flush_tx_pcap_if_ready(entity);
        observe_state(watch, entity, tool.verbose);
        auto const now = std::chrono::steady_clock::now();
        supervise(watch, entity, now);
        if (tool.verbose) {
            report_telemetry(watch, now, ctx, entity, timer, media, hooks);
        }
    }
    logs.poll();  // flush log entries produced during shutdown
}

/// Write the wake-error and callback-duration histograms to the CSVs asked for.
inline void write_timer_csvs(EntityToolOptions const& tool, MainLoopResult const& result, RunnerTelemetry const& telemetry)
{
    if (!tool.media_timer_wake_csv_path.empty()) {
        (void)write_histogram_csv(tool.media_timer_wake_csv_path, result.wake_stats.error_histogram);
    }
    if (!tool.media_timer_duration_csv_path.empty()) {
        (void)write_histogram_csv(tool.media_timer_duration_csv_path, telemetry.duration.snapshot());
    }
}

/// Stop the media timer and PTP sampling; collect the counters and write the CSVs asked for.
template <typename Timer>
[[nodiscard]] auto stop_runner_timer(
    Timer& timer, RunnerTelemetry const& telemetry, ptpclient::PtpAppContext& ctx, EntityToolOptions const& tool) -> MainLoopResult
{
    timer.stop();
    if (auto const errs = telemetry.media.errors.load(); errs > 0) {
        std::print(stderr, "Warning: media timer had {} wake error(s) (likely gPTP sync loss)\n", errs);
    }
    MainLoopResult result{
        .wake_stats = timer.stats(),
        .compensation_ns = ctx.compensation_ns,
        .recovery_count = timer.recovery_count(),
        .missed_cycles = timer.missed_cycles()};
    ctx.guard.stop();
    write_timer_csvs(tool, result, telemetry);
    return result;
}

/// The shared main loop: PTP media timer -> Entity::process_audio on the
/// SCHED_FIFO thread; link monitor, grandmaster edge-detect, Init
/// timeout, log draining, TX-pcap flushing and telemetry on this thread.
/// Returns when shutdown is requested (Ctrl-C).
template <typename Entity>
auto run_entity_main_loop(
    net::MessageReactor& reactor,
    ptpclient::PtpAppContext& ctx,
    Entity& entity,
    EntityToolOptions const& tool,
    int64_t const packet_period_ns,
    EntityRunnerHooks const& hooks = {}) -> MainLoopResult
{
    RunnerTelemetry telemetry{};
    auto timer = make_runner_media_timer(ctx, entity, packet_period_ns, telemetry);
    if (auto const started = timer.start(); !started) {
        std::print(stderr, "Error: Failed to start timer: {}\n", started.error().message());
        return MainLoopResult{.exit_code = EXIT_FAILURE, .compensation_ns = ctx.compensation_ns};
    }
    follow_link_and_grandmaster(reactor, entity);
    run_runner_control_loop(reactor, ctx, entity, tool, hooks, timer, telemetry.media);
    return stop_runner_timer(timer, telemetry, ctx, tool);
}

/// The shared exit banner: entity state + timer counters + entity
/// print_state(), plus the wake-stats report when requested.
template <typename Entity>
void print_final_status(Entity const& entity, MainLoopResult const& result, bool const dump_stats)
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

}  // namespace statusbar::avb_entity
