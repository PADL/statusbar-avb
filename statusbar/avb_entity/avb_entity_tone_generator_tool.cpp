// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AVB Entity Tone Generator Tool — talker-only AM824 + AAF + CRF
//
// Runs an AVB entity with THREE talker stream sources and no listeners:
//   stream 0 AM824, stream 1 AAF (32-bit PCM), stream 2 CRF (Milan 48 kHz media
//   clock). Each audio channel carries a continuous sine; by default the 8
//   channels are the white piano keys C4..C5. The media clock is locked to gPTP
//   (r = 1.0). No inter-site tunnel, no listener.
//
// Usage: statusbar-avb-tone-generator --interface=eth0 --descriptor-storage=entity_tone.bin
//   (the blob must declare 0 stream inputs + 3 stream outputs; build with
//    `statusbar-aemxml json2bin examples/tone.json entity_tone.bin`)

#include "statusbar/avb_entity/avb_entity_identity.hpp"
#include "statusbar/avb_entity/avb_entity_tone_generator.hpp"
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
#include "statusbar/net/net_link_monitor.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_posix_util.hpp"
#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/realtime/realtime.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/stats/stats_atomic_histogram.hpp"
#include "statusbar/stats/stats_atomic_wake_stats.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace statusbar;
using TimePoint = sm::TimePoint;
using Entity = avb_entity::AvbEntityToneGenerator;

/// Note names for the 12 chromatic pitch classes (display only).
constexpr std::array<std::string_view, 12> kNoteNames{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

[[nodiscard]] auto note_name(uint8_t midi) -> std::string
{
    int const octave = (static_cast<int>(midi) / 12) - 1;  // MIDI 60 = C4
    return std::string{kNoteNames[static_cast<size_t>(midi % 12)]} + std::to_string(octave);
}

[[nodiscard]] auto write_histogram_csv(std::string const& path, stats::AtomicHistogram<128>::Snapshot const& hist) -> bool
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

struct Config
{
    ptpclient::PtpAppConfig ptp_app;

    avb_entity::AvbEntityAudioIOConfig entity{
        .entity_id = ieee::Eui64{},
        // Well-formed EUI-64 model id: OUI-24 70:b3:d5 + FF:FE insertion + device
        // bits (NOT OUI-36 bits jammed in right after the OUI-24, which is malformed).
        .entity_model_id = ieee::Eui64{0x70, 0xB3, 0xD5, 0xFF, 0xFE, 0xED, 0xC0, 0x00},
        // JDKS OUI-36 multicast (NOT the 91:E0:F0 MAAP pool). CRF = top of range.
        .am824_talker_dest_mac = {0x71, 0xB3, 0xD5, 0xED, 0xCF, 0xFD},
        .aaf_talker_dest_mac = {0x71, 0xB3, 0xD5, 0xED, 0xCF, 0xFE},
        .crf_talker_dest_mac = {0x71, 0xB3, 0xD5, 0xED, 0xCF, 0xFF},
        .vlan_id = 2,
        // The media clock is always gPTP-locked (r = 1.0) for the tone generator.
        .media_lock_to_gptp = true,
        .tone_amplitude = 0.1F,  // -20 dBFS
        .entity_name = "",
        .firmware_version = "1.0.0",
    };

    /// Lowest white-key tone (MIDI note number). Default 60 = C4; each channel
    /// takes the next white key up, so 8 channels = C4..C5.
    uint8_t base_midi_note{avb_entity::TONE_DEFAULT_BASE_MIDI_NOTE};

#ifdef STATUSBAR_AVB_DEFAULT_TONE_BLOB
    std::string descriptor_storage_path{STATUSBAR_AVB_DEFAULT_TONE_BLOB};
#else
    std::string descriptor_storage_path;
#endif
    /// Stream set: "all" = AM824 + AAF + CRF (3 outputs); "aaf" = a single AAF
    /// 8-ch stream (diagnostic — a clean device for listeners that stall on the
    /// mixed AM824+AAF aggregate). "aaf" auto-selects the entity_tone_aaf.bin blob.
    std::string streams_mode{"all"};
    bool verbose{true};
    bool dump_stats_on_exit{true};
    bool require_ptp{false};
    std::string media_timer_wake_csv_path{};
    std::string media_timer_duration_csv_path{};
};

auto build_arg_specs(Config& config) -> args::ArgumentSpecs
{
    args::ArgumentSpecs specs;
    ptpclient::add_ptp_arg_specs(specs, config.ptp_app);

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
    specs.add<std::string>(
        "atdecc.version",
        "ATDECC wire version: '2016' truncates descriptors to their 1722.1-2013/2016 lengths; '2021' emits full "
        "2021 descriptors. Default 2016.",
        config.entity.atdecc_version,
        [&](auto v) { config.entity.atdecc_version = std::string{v}; });
    specs.add<uint16_t>("vlan_id", "VLAN ID for AVB streams", config.entity.vlan_id, [&](auto v) { config.entity.vlan_id = v; });
    specs.add<std::string>(
        "stream.address_mode",
        "Talker stream dest-address source: 'static' (the configured *_dest_mac) or 'maap' (acquire a block from "
        "the 1722 dynamic pool via MAAP at startup). Default static.",
        config.entity.stream_address_mode,
        [&](auto v) { config.entity.stream_address_mode = std::string{v}; });
    specs.add<bool>(
        "srp.redeclare_registered_listeners",
        "Sticky-Listener MSRP workaround: re-declare REGISTERED Listener attributes every periodic/LeaveAll pass. "
        "Needed when feeding a downstream listener through some AVB switches. Default off.",
        config.entity.redeclare_registered_listeners,
        [&](auto v) { config.entity.redeclare_registered_listeners = v; });
    specs.add<bool>(
        "srp.suppress_leaveall",
        "Suppress-LeaveAll MSRP workaround: never originate a periodic LeaveAll. Default off.",
        config.entity.suppress_leaveall,
        [&](auto v) { config.entity.suppress_leaveall = v; });
    specs.add<bool>(
        "gate.talker_on_listener",
        "Transmit a talker stream only when it is fully SR-class admitted: an ACMP connection AND MSRP Listener "
        "Ready (with grace) for that stream; stay silent otherwise (IEEE 802.1Q SRP). Each stream gates "
        "independently -- the CRF media clock on its own connection + reservation, not on the audio talkers. "
        "Default true (false = stream unconditionally from link-up; non-spec, bench only).",
        config.entity.gate_talker_on_listener,
        [&](auto v) { config.entity.gate_talker_on_listener = v; });
    specs.add<ieee::Eui48>(
        "am824.dest_mac", "AM824 talker destination multicast MAC (stream 0)", config.entity.am824_talker_dest_mac, [&](auto v) {
            config.entity.am824_talker_dest_mac = v;
        });
    specs.add<ieee::Eui48>(
        "aaf.dest_mac", "AAF talker destination multicast MAC (stream 1)", config.entity.aaf_talker_dest_mac, [&](auto v) {
            config.entity.aaf_talker_dest_mac = v;
        });
    specs.add<ieee::Eui48>(
        "crf.dest_mac",
        "CRF media-clock talker destination multicast MAC (stream 2)",
        config.entity.crf_talker_dest_mac,
        [&](auto v) { config.entity.crf_talker_dest_mac = v; });
    specs.add<uint64_t>(
        "presentation_offset_ns",
        "AVTP presentation-time offset added to gPTP time to form the stream timestamp (ns)",
        config.entity.presentation_offset_ns,
        [&](auto v) { config.entity.presentation_offset_ns = v; });
    specs.add<uint64_t>(
        "media.packets_per_wake",
        "Stream packets per stream per media-timer wake (default 1 = strict Class A shaping).",
        static_cast<uint64_t>(config.entity.packets_per_wake),
        [&](auto v) { config.entity.packets_per_wake = static_cast<size_t>((v < 1) ? 1 : ((v > 16) ? 16 : v)); });
    specs.add<double>(
        "tone.amplitude",
        "Per-channel tone amplitude, 0..1 linear (1.0 = 0 dBFS; default 0.1 = -20 dBFS)",
        static_cast<double>(config.entity.tone_amplitude),
        [&](auto v) { config.entity.tone_amplitude = static_cast<float>(v); });
    specs.add<uint64_t>(
        "tone.base_note",
        "MIDI note number of the lowest channel's white key (default 60 = C4). Each channel takes the next white "
        "key up, so 8 channels span C4..C5.",
        static_cast<uint64_t>(config.base_midi_note),
        [&](auto v) { config.base_midi_note = static_cast<uint8_t>(v & 0x7FULL); });
    specs.add<std::string>(
        "tx_pcap.path",
        "Diagnostic: record our OWN transmitted stream frames (AAF/AM824/CRF) to this libpcap file, gPTP-timestamped, "
        "for tx_pcap.seconds. Empty = off.",
        config.entity.tx_pcap_path,
        [&](auto v) { config.entity.tx_pcap_path = std::string{v}; });
    specs.add<uint32_t>(
        "tx_pcap.seconds",
        "Capture window in seconds for tx_pcap.path, from the first transmitted frame (default 10).",
        config.entity.tx_pcap_seconds,
        [&](auto v) { config.entity.tx_pcap_seconds = v; });
    specs.add<std::string>(
        "media-timer-wake-csv",
        "Dump the 8 kHz media-timer wake-error histogram to this CSV at exit. Empty = off",
        config.media_timer_wake_csv_path,
        [&](auto v) { config.media_timer_wake_csv_path = v; });
    specs.add<std::string>(
        "media-timer-duration-csv",
        "Dump the media-timer callback-DURATION histogram to this CSV at exit",
        config.media_timer_duration_csv_path,
        [&](auto v) { config.media_timer_duration_csv_path = v; });
    specs.add_choice(
        "streams",
        "Stream set: 'all' = AM824 + AAF + CRF (3 outputs); 'aaf' = a single 8-ch AAF stream; 'aaf+crf' = AAF audio "
        "+ CRF media clock (clean 8-ch device WITH a clock reference for the listener). 'aaf'/'aaf+crf' auto-select "
        "their blob unless --descriptor-storage is given",
        {"all", "aaf", "aaf+crf"},
        "all",
        [&](auto v) { config.streams_mode = std::string{v}; });
    specs.add<std::string>(
        "descriptor-storage",
        "Path to descriptor storage .bin file (defaults to entity_tone.bin, or entity_tone_aaf.bin for --streams=aaf)",
        config.descriptor_storage_path,
        [&](auto v) { config.descriptor_storage_path = v; });
    specs.add<bool>("verbose", "Enable verbose output", config.verbose, [&](auto v) { config.verbose = v; });
    specs.add<bool>("dump_stats_on_exit", "Dump timer statistics on exit", config.dump_stats_on_exit, [&](auto v) {
        config.dump_stats_on_exit = v;
    });
    specs.add<bool>(
        "ptp.require",
        "Refuse to run if PTP falls back to the system clock (talker timestamps would not be gPTP)",
        config.require_ptp,
        [&](auto v) { config.require_ptp = v; });

    return specs;
}

void print_usage(char const* program_name, args::ArgumentSpecs const& specs)
{
    static constexpr std::array<std::string_view, 2> examples{
        "--interface=eth0 --descriptor-storage=entity_tone.bin",
        "--interface=eth0 --tone.base_note=60 --tone.amplitude=0.1",
    };
    config::default_print_usage(
        program_name,
        specs,
        "AVB Entity Tone Generator (talker-only AM824 + AAF + CRF)\n"
        "Transmits 8 channels of continuous sine tones (white piano keys C4..C5 by default) as both an AM824 and an\n"
        "AAF 96 kHz stream, plus a CRF media clock. The media clock is locked to gPTP (r = 1.0).",
        examples);
    std::print(stderr, "\nGenerate the blob with: statusbar-aemxml json2bin examples/tone.json entity_tone.bin\n");
    std::print(stderr, "\nPress Ctrl-C to stop.\n");
}

void print_entity_config(Config const& config, Entity const& entity)
{
    auto const& ecfg = entity.config();
    std::print("AVB Entity Tone Generator (talker-only)\n");
    std::print("=======================================\n");
    std::print("Descriptor Storage: {}\n", config.descriptor_storage_path);
    std::print("Entity ID:        {}\n", ieee::to_string(ecfg.entity_id).view());
    std::print("Entity Name:      {}\n", ecfg.entity_name);
    std::print("Interface:        {}\n", ecfg.interface_name);
    std::print("VLAN ID:          {}\n", ecfg.vlan_id);
    std::print("Stream 0 (AM824): dest {}\n", ieee::to_string(ecfg.am824_talker_dest_mac).view());
    std::print("Stream 1 (AAF):   dest {}  (int32 PCM)\n", ieee::to_string(ecfg.aaf_talker_dest_mac).view());
    std::print(
        "Stream 2 (CRF):   dest {}  (48 kHz media clock, {}x{} ts/pkt)\n",
        ieee::to_string(ecfg.crf_talker_dest_mac).view(),
        ecfg.crf_timestamps_per_packet,
        ecfg.crf_timestamp_interval);
    std::print(
        "Channels:         {}  (tone level {:.3f} = {:+.1f} dBFS)\n",
        entity.channels(),
        ecfg.tone_amplitude,
        ecfg.tone_amplitude > 0.0F ? 20.0 * std::log10(static_cast<double>(ecfg.tone_amplitude)) : -120.0);
    for (size_t ch = 0; ch < entity.channels(); ++ch) {
        auto const midi =
            static_cast<uint8_t>(config.base_midi_note + ((ch / 7) * 12) + std::array<int, 7>{0, 2, 4, 5, 7, 9, 11}[ch % 7]);
        std::print(
            "  ch {}: {:<4} {:8.3f} Hz\n", ch, note_name(midi), avb_entity::white_key_frequency_hz(config.base_midi_note, ch));
    }
    std::print("Sample Rate:      {} Hz, Samples/Packet: {}\n", Entity::SAMPLE_RATE, Entity::SAMPLES_PER_PACKET);
    std::print("Media clock rate: r=1.0 PINNED to gPTP\n");
}

struct MainLoopResult
{
    int exit_code{EXIT_SUCCESS};
    stats::AtomicWakeStats::Snapshot wake_stats{};
    int64_t compensation_ns{0};
    int64_t recovery_count{0};
    int64_t missed_cycles{0};
};

MainLoopResult run_main_loop(net::MessageReactor& reactor, ptpclient::PtpAppContext& ctx, Entity& entity, Config const& config)
{
    MainLoopResult result;
    result.compensation_ns = ctx.compensation_ns;

    int64_t const PACKET_PERIOD_NS = (static_cast<int64_t>(Entity::SAMPLES_PER_PACKET) * 1'000'000'000LL / Entity::SAMPLE_RATE) *
        static_cast<int64_t>(config.entity.packets_per_wake);

    auto* net_handlers = entity.net_handlers();
    bool had_grandmaster = net_handlers != nullptr ? net_handlers->gptp_handler().has_grandmaster() : false;

    stats::AtomicHistogram<128> media_dur_hist{stats::AtomicHistogramConfig{.low_ns = 0, .high_ns = 100'000, .bin_width_ns = 1000}};
    itc::TelemetryCounter<int64_t> timer_error_count;
    itc::Published<int64_t> last_wake_count;
    itc::Published<int64_t> last_wake_error_ns;

    auto timer = ptpclient::make_ptp_timer(
        *ctx.bridge,
        PACKET_PERIOD_NS,
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

    auto startup_time = TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    entity.on_link_up(startup_time);

    reactor.add(std::make_unique<net::LinkMonitor>(config.entity.interface_name, [&entity](bool up, int64_t now_ns) {
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
    entity.set_log_verbosity(config.verbose ? logging::LogLevel::Debug : logging::LogLevel::Status);

    while (!realtime::is_shutdown_requested()) {
        (void)log_collector.poll(log_sink);
        (void)reactor.poll_once(100);

        if (entity.tx_pcap_ready_to_write()) {
            auto const wr = entity.flush_tx_pcap();
            if (wr) {
                std::print("TX pcap: wrote {} frames to {}\n", entity.tx_pcap_frame_count(), config.entity.tx_pcap_path);
            } else {
                std::print(stderr, "TX pcap: write failed: {}\n", wr.error().message());
            }
        }

        auto const current_state = entity.state_string();
        if (current_state != last_state) {
            if (config.verbose) {
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

        if (config.verbose && ctx.bridge && now - last_telemetry_time >= std::chrono::seconds(1)) {
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

    if (!config.media_timer_wake_csv_path.empty()) {
        (void)write_histogram_csv(config.media_timer_wake_csv_path, result.wake_stats.error_histogram);
    }
    if (!config.media_timer_duration_csv_path.empty()) {
        (void)write_histogram_csv(config.media_timer_duration_csv_path, media_dur_hist.snapshot());
    }
    return result;
}

void print_final_status(Entity const& entity, MainLoopResult const& result, bool dump_stats)
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

auto main(int argc, char** argv) -> int
{
    using namespace statusbar;

    Config config;
    auto specs = build_arg_specs(config);

    auto cli_result = config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-avb-tone-generator");
    if (!cli_result) {
        return config::handled_builtin_command(cli_result) ? 0 : 1;
    }

    if (config.entity.interface_name.empty()) {
        std::print(stderr, "Error: --interface=<iface> is required\n");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }
    // Map --streams to the entity's StreamSet, and pick the matching default blob
    // unless the user gave an explicit --descriptor-storage path.
    auto const stream_set = (config.streams_mode == "aaf") ? Entity::StreamSet::AafOnly
        : (config.streams_mode == "aaf+crf")               ? Entity::StreamSet::AafCrf
                                                           : Entity::StreamSet::All;
#ifdef STATUSBAR_AVB_DEFAULT_TONE_BLOB
    if (config.descriptor_storage_path == STATUSBAR_AVB_DEFAULT_TONE_BLOB) {
#    ifdef STATUSBAR_AVB_DEFAULT_TONE_AAF_BLOB
        if (stream_set == Entity::StreamSet::AafOnly) {
            config.descriptor_storage_path = STATUSBAR_AVB_DEFAULT_TONE_AAF_BLOB;
        }
#    endif
#    ifdef STATUSBAR_AVB_DEFAULT_TONE_AAF_CRF_BLOB
        if (stream_set == Entity::StreamSet::AafCrf) {
            config.descriptor_storage_path = STATUSBAR_AVB_DEFAULT_TONE_AAF_CRF_BLOB;
        }
#    endif
    }
#endif
    if (config.descriptor_storage_path.empty()) {
        std::print(stderr, "Error: --descriptor-storage=<path> is required\n");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }

    avb_entity::apply_node_identity_defaults(
        config.entity.interface_name,
        config.entity.entity_id,
        config.entity.entity_name,
        ieee::Eui64{0x70, 0xB3, 0xD5, 0xED, 0xCF, 0x00, 0x00, 0x03},
        "Statusbar Tone Generator");

    auto blob = load_file(config.descriptor_storage_path);
    if (blob.empty()) {
        std::print(stderr, "Error: Failed to load descriptor storage file: {}\n", config.descriptor_storage_path);
        return EXIT_FAILURE;
    }
    config.entity.descriptor_storage_blob = std::move(blob);

    realtime::setup_shutdown_signal_handlers();

    auto const base_midi_note = config.base_midi_note;
    auto entity_result = avb_entity::AvbEntityToneGenerator::create(std::move(config.entity), base_midi_note, stream_set);
    if (!entity_result) {
        std::print(stderr, "Error: Failed to create entity: {}\n", entity_result.error().message());
        return EXIT_FAILURE;
    }
    auto& entity = **entity_result;

    print_entity_config(config, entity);

    std::string ptp_summary;
    ptpclient::format_ptp_config_to(std::back_inserter(ptp_summary), config.ptp_app, "\nPTP Configuration");
    std::print("{}\n", ptp_summary);

    itc::StopToken shutdown_flag{};
    net::MessageReactor reactor{shutdown_flag, net::monotonic_ns, 10};

    auto start_result = entity.start(reactor);
    if (!start_result) {
        std::print(stderr, "Error: Failed to start entity: {}\n", start_result.error().message());
        return EXIT_FAILURE;
    }

    std::print("\nEntity started. Network handlers registered.\n");

    auto is_shutdown = [] { return realtime::is_shutdown_requested(); };
    std::string const requested_driver = config.ptp_app.driver_name;
    std::string const requested_device = config.ptp_app.device_path;
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

    if (requested_driver != "system" && config.ptp_app.driver_name == "system") {
        std::print(
            stderr,
            "\n*** WARNING: PTP fell back to the SYSTEM clock -- could not open the gPTP PHC ({}).\n"
            "***          AVTP timestamps will be in the local clock domain, NOT gPTP, so listeners\n"
            "***          will report LATE_TIMESTAMP and the stream will not lock. Run as root or\n"
            "***          grant read access to the PHC (udev rule).\n\n",
            requested_device);
        if (config.require_ptp) {
            std::print(stderr, "Error: --ptp.require is set; refusing to stream without a gPTP clock.\n");
            (void)entity.stop();
            return EXIT_FAILURE;
        }
    }

    auto& ctx = *ctx_result;
    std::print("\nStarting main loop (Ctrl-C to stop)...\n\n");

    auto loop_result = run_main_loop(reactor, ctx, entity, config);

    std::print("\n\nShutting down...\n");
    auto stop_time = TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    entity.on_link_down(stop_time);
    (void)entity.stop();
    std::print("Entity stopped.\n");

    print_final_status(entity, loop_result, config.dump_stats_on_exit);
    return loop_result.exit_code;
}
