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

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <print>
#include <string>
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

/// Register the CLI arguments every entity tool shares: entity identity,
/// interface, ATDECC wire version, VLAN, stream addressing, SRP
/// workarounds, the talker gate, stream destination MACs, presentation
/// offset, media pacing, listener bindings, TX pcap, histogram CSVs and
/// the verbosity/stats/PTP-strictness flags. Tools add their own specs
/// (and the PTP specs) around this.
inline void add_common_entity_arg_specs(args::ArgumentSpecs& specs, AvbEntityAudioIOConfig& entity, EntityToolOptions& tool)
{
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
        "Transmit a talker stream only when it is fully SR-class admitted: an ACMP connection AND MSRP Listener "
        "Ready (with grace) for that stream; stay silent otherwise (IEEE 802.1Q SRP). Each stream gates "
        "independently -- the CRF media clock on its own connection + reservation, not on the audio talkers. "
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
    using TimePoint = sm::TimePoint;

    MainLoopResult result;
    result.compensation_ns = ctx.compensation_ns;

    auto* net_handlers = entity.net_handlers();
    bool had_grandmaster = net_handlers != nullptr ? net_handlers->gptp_handler().has_grandmaster() : false;

    // Callback-duration histogram for the media timer: how long
    // process_audio() takes per wake (the timer's own stats() track only
    // wake-error). Dumped to --media-timer-duration-csv at exit.
    stats::AtomicHistogram<128> media_dur_hist{stats::AtomicHistogramConfig{.low_ns = 0, .high_ns = 100'000, .bin_width_ns = 1000}};

    // The media-timer callback runs on a SCHED_FIFO RT thread, so it must NOT
    // std::print (alloc/throw/block) — it publishes to these lock-free itc
    // channels and the control loop below surfaces them off-thread.
    itc::TelemetryCounter<int64_t> timer_error_count;
    itc::Published<int64_t> last_wake_count;
    itc::Published<int64_t> last_wake_error_ns;

    auto timer = ptpclient::make_ptp_timer(
        *ctx.bridge,
        packet_period_ns,
        [&](StatusValue<ptpclient::TimerWakeInfo> const& wake_info) {
            if (!wake_info) {
                timer_error_count.add(1);
                return;
            }
            auto const now = TimePoint{std::chrono::nanoseconds{wake_info->actual_time_ns}};
            auto const cb_t0 = std::chrono::steady_clock::now();
            entity.process_audio(now);
            media_dur_hist.update(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - cb_t0).count());
            last_wake_count.publish(wake_info->wake_count);
            last_wake_error_ns.publish(wake_info->error_ns);
        },
        ctx.compensation_ns,
        ctx.enable_realtime,
        ctx.cpu_affinity);

    auto start_result = timer.start();
    if (!start_result) {
        std::print(stderr, "Error: Failed to start timer: {}\n", start_result.error().message());
        result.exit_code = EXIT_FAILURE;
        return result;
    }

    // Drive the initial link-up synchronously (before the reactor runs) so the
    // supervisor leaves Down -> Init before any gPTP announce is processed,
    // then track the interface link state continuously.
    auto startup_time = TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    entity.on_link_up(startup_time);
    reactor.add(std::make_unique<net::LinkMonitor>(entity.config().interface_name, [&entity](bool up, int64_t now_ns) {
        auto const tp = TimePoint{std::chrono::nanoseconds{now_ns}};
        if (up) {
            entity.on_link_up(tp);
        } else {
            entity.on_link_down(tp);
        }
    }));

    constexpr auto GPTP_LOCK_TIMEOUT = std::chrono::seconds{10};
    auto last_state_change_time = std::chrono::steady_clock::now();
    auto last_state = entity.state_string();
    auto last_telemetry_time = std::chrono::steady_clock::now();

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
                    entity.components().adp_advertiser.set_gptp_info(grandmaster_id, 0);
                    entity.components().adp_advertiser.notify_entity_changed();
                    (void)now_ns;
                    auto const sm_now = std::chrono::steady_clock::now();
                    entity.on_gptp_announce(sm_now, true);
                }});
    }

    // Drain the entity's SPSC log channels (ctl = reactor thread, media = RT
    // media timer) here on the main thread — the entities/libraries only ever
    // enqueue; formatting and stderr I/O happen in this loop.
    logging::LogCollector<4> log_collector;
    logging::StderrSink log_sink;
    (void)log_collector.add(entity.ctl_log_channel());
    (void)log_collector.add(entity.media_log_channel());
    if (hooks.add_log_channels) {
        hooks.add_log_channels(log_collector);
    }
    entity.set_log_verbosity(tool.verbose ? logging::LogLevel::Debug : logging::LogLevel::Status);

    while (!realtime::is_shutdown_requested()) {
        (void)log_collector.poll(log_sink);
        (void)reactor.poll_once(100);
        if (hooks.poll) {
            hooks.poll();
        }

        // TX pcap capture finished (window elapsed / ring full): write the file
        // here, off the media RT thread.
        if (entity.tx_pcap_ready_to_write()) {
            auto const wr = entity.flush_tx_pcap();
            if (wr) {
                std::print("TX pcap: wrote {} frames to {}\n", entity.tx_pcap_frame_count(), entity.config().tx_pcap_path);
            } else {
                std::print(stderr, "TX pcap: write failed: {}\n", wr.error().message());
            }
        }

        auto const current_state = entity.state_string();
        if (current_state != last_state) {
            if (tool.verbose) {
                std::print("State: {} -> {}\n", last_state, current_state);
            }
            last_state_change_time = std::chrono::steady_clock::now();
            last_state = current_state;
        }

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

        if (tool.verbose && ctx.bridge && now - last_telemetry_time >= std::chrono::seconds(1)) {
            last_telemetry_time = now;
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
                last_wake_count.load(),
                last_wake_error_ns.load(),
                entity.state_string(),
                timer_error_count.load());
            if (hooks.extra_telemetry) {
                hooks.extra_telemetry();
            }
        }
    }

    // Final drain: flush log entries produced during shutdown.
    (void)log_collector.poll(log_sink);

    timer.stop();
    if (auto const errs = timer_error_count.load(); errs > 0) {
        std::print(stderr, "Warning: media timer had {} wake error(s) (likely gPTP sync loss)\n", errs);
    }
    result.wake_stats = timer.stats();
    result.recovery_count = timer.recovery_count();
    result.missed_cycles = timer.missed_cycles();
    ctx.guard.stop();

    if (!tool.media_timer_wake_csv_path.empty()) {
        (void)write_histogram_csv(tool.media_timer_wake_csv_path, result.wake_stats.error_histogram);
    }
    if (!tool.media_timer_duration_csv_path.empty()) {
        (void)write_histogram_csv(tool.media_timer_duration_csv_path, media_dur_hist.snapshot());
    }
    return result;
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
