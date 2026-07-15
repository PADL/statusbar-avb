// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AVB Entity Audio I/O Tool — dual-format entity (AM824 + AAF)
//
// Runs an AVB entity with TWO talker stream sources and TWO listener stream
// sinks, each N-channel 96 kHz: stream 0 AM824, stream 1 AAF (32-bit PCM). Both
// talkers carry the same per-channel sine source; the single stream RX port
// joins both multicast groups and dispatches by AVTP subtype.
//
// Usage: avb_entity_audio_io_tool --interface=eth0 --descriptor-storage=entity_audio.bin
//   (the blob must declare 2 stream inputs + 2 stream outputs; build with
//    `statusbar-aemxml json2bin examples/dual.json entity_audio.bin`)
//
// The main loop, common CLI arguments, PTP setup and telemetry all come
// from the shared kit runner (avb_entity_runner.hpp, kit phase 5d) — this
// file keeps the audio-io-specific pieces: tunnel/sweep/filter/RX-timer
// arguments, the optional dedicated RT RX drain timer and its telemetry.

#include "statusbar/avb_entity/avb_entity_audio_io.hpp"
#include "statusbar/avb_entity/avb_entity_identity.hpp"
#include "statusbar/avb_entity/avb_entity_runner.hpp"
#include "statusbar/avb_entity/avb_entity_version.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_published.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/itc/itc_telemetry_counter.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/realtime/realtime.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/stats/stats_atomic_histogram.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <print>
#include <string>

namespace {

using namespace statusbar;
using TimePoint = sm::TimePoint;
using Entity = avb_entity::AvbEntityAudioIO;

struct Config
{
    ptpclient::PtpAppConfig ptp_app;
    avb_entity::EntityToolOptions tool;

    avb_entity::AvbEntityAudioIOConfig entity{
        // entity_id / entity_name left as sentinels (zero / empty): main()
        // derives a per-node-unique entity_id from the interface MAC
        // (modified EUI-64) and entity_name from the hostname unless --entity.id
        // / --entity.name override them. entity_model_id stays fixed (it
        // identifies the shared descriptor MODEL, not this instance).
        .entity_id = ieee::Eui64{},
        .entity_model_id = ieee::Eui64{0x70, 0xB3, 0xD5, 0xED, 0xCF, 0x00, 0x00, 0x00},
        // interface_name has no default — --interface is required.
        // JDKS OUI-36 multicast (NOT the 91:E0:F0 MAAP pool). CRF = top of range.
        .am824_talker_dest_mac = {0x71, 0xB3, 0xD5, 0xED, 0xCF, 0xFD},
        .aaf_talker_dest_mac = {0x71, 0xB3, 0xD5, 0xED, 0xCF, 0xFE},
        .crf_talker_dest_mac = {0x71, 0xB3, 0xD5, 0xED, 0xCF, 0xFF},
        .vlan_id = 2,
        .filter_freq_hz = 1000.0,
        .filter_gain_db = 0.0,
        .filter_q = 0.707,
        .entity_name = "",
        .firmware_version = statusbar::avb_entity::build_version,
    };
};

auto build_arg_specs(Config& config) -> args::ArgumentSpecs
{
    args::ArgumentSpecs specs;
    ptpclient::add_ptp_arg_specs(specs, config.ptp_app);
    avb_entity::add_common_entity_arg_specs(specs, config.entity, config.tool);

    specs.add<bool>(
        "sweep.enable",
        "Test signal: emit a repeating logarithmic sine sweep as the UDP TUNNEL source (replaces the listener "
        "source / silence). The peer site receives the sweep -- e.g. node-b generates it -> tunnel -> node-a's audio interface "
        "audio-interface ch out; patch out->in to loop it back through the tunnel. Default off.",
        config.entity.sweep_enable,
        [&](auto v) { config.entity.sweep_enable = v; });
    specs.add<double>("sweep.f_start_hz", "Sweep start frequency Hz (default 20)", config.entity.sweep_f_start_hz, [&](auto v) {
        config.entity.sweep_f_start_hz = v;
    });
    specs.add<double>("sweep.f_end_hz", "Sweep end frequency Hz (default 1000)", config.entity.sweep_f_end_hz, [&](auto v) {
        config.entity.sweep_f_end_hz = v;
    });
    specs.add<double>(
        "sweep.duration_s", "Sweep duration seconds, then repeats (default 5)", config.entity.sweep_duration_s, [&](auto v) {
            config.entity.sweep_duration_s = v;
        });
    specs.add<uint32_t>(
        "sweep.channel",
        "Channel index carrying the sweep; others silent (default 0 = first)",
        static_cast<uint32_t>(config.entity.sweep_channel),
        [&](auto v) { config.entity.sweep_channel = static_cast<uint16_t>(v); });
    specs.add<double>(
        "sweep.amplitude",
        "Sweep amplitude 0..1 full scale (default 0.5)",
        static_cast<double>(config.entity.sweep_amplitude),
        [&](auto v) { config.entity.sweep_amplitude = static_cast<float>(v); });
    specs.add<uint64_t>(
        "lock.tolerance_ns",
        "MEDIA_LOCKED detector tolerance in ns (default 5000 = 5us). Widen to tolerate talker media-timer "
        "jitter (e.g. with media.packets_per_wake>1 or a hot talker) without flapping the lock",
        static_cast<uint64_t>(config.entity.lock_tolerance_ns),
        [&](auto v) { config.entity.lock_tolerance_ns = static_cast<uint32_t>((v > 0xFFFFFFFFULL) ? 0xFFFFFFFFULL : v); });
    specs.add<bool>(
        "media.lock_to_gptp",
        "Pin media-clock rate ratio r=1.0 (media clock = gPTP) instead of r=gPTP/CLOCK_REALTIME. Use when the "
        "gPTP grandmaster is the reference the listener recovers against and CLOCK_REALTIME is NTP (not GPS), so "
        "r would otherwise wander and wobble the listener's recovered clock. Default false (GPS-rate-pinned)",
        config.entity.media_lock_to_gptp,
        [&](auto v) { config.entity.media_lock_to_gptp = v; });
    specs.add<bool>(
        "stream_rx.rt_timer",
        "Drain the AM824/AAF stream RX on a dedicated SCHED_FIFO timer (its own isolated core) instead of the "
        "shared reactor. Deterministic low-latency draining stamps ingress frames against a fresh gPTP wake time, "
        "eliminating spurious LATE_TIMESTAMP from reactor scheduling jitter. Default false (reactor). Needs a spare "
        "core -- set stream_rx.cpu to an isolated core distinct from the media timer",
        config.entity.stream_rx_rt_timer,
        [&](auto v) { config.entity.stream_rx_rt_timer = v; });
    specs.add<int64_t>(
        "stream_rx.cpu",
        "CPU core for the RX timer thread (stream_rx.rt_timer). Should be isolated + distinct from the media timer",
        static_cast<int64_t>(config.entity.stream_rx_cpu_affinity),
        [&](auto v) { config.entity.stream_rx_cpu_affinity = static_cast<int>(v); });
    specs.add<uint64_t>(
        "stream_rx.period_us",
        "RX drain tick period in microseconds (stream_rx.rt_timer). Default 50 (2.5x the 125us packet interval)",
        static_cast<uint64_t>(config.entity.stream_rx_period_us),
        [&](auto v) { config.entity.stream_rx_period_us = static_cast<uint32_t>((v < 5) ? 5 : ((v > 1000) ? 1000 : v)); });

    specs.add<bool>(
        "udptun.enable",
        "Enable the inter-site UDPTUN ingest: forward the AAF (int32) listener audio to a far-site peer as "
        "AAF-v1 over IEEE-1722 Annex J UDP, with TAI presentation timestamps. Requires --udptun.peer",
        config.entity.udptun_enable,
        [&](auto v) { config.entity.udptun_enable = v; });
    specs.add<std::string>(
        "udptun.peer", "Far-site UDPTUN peer host/IP (enables the tunnel TX path)", config.entity.udptun_peer_host, [&](auto v) {
            config.entity.udptun_peer_host = v;
        });
    specs.add<uint64_t>(
        "udptun.port",
        "Far-site UDPTUN UDP port (IEEE 1722 Annex J continuous default 17220)",
        static_cast<uint64_t>(config.entity.udptun_peer_port),
        [&](auto v) { config.entity.udptun_peer_port = static_cast<uint16_t>(v & 0xFFFFULL); });
    specs.add<uint64_t>(
        "udptun.tai_offset_ns",
        "TAI - UTC offset in ns added to CLOCK_REALTIME to form the TAI tunnel timeline (default 37e9)",
        static_cast<uint64_t>(config.entity.udptun_tai_offset_ns),
        [&](auto v) { config.entity.udptun_tai_offset_ns = static_cast<int64_t>(v); });
    specs.add<uint64_t>(
        "udptun.source_stream",
        "Listener stream feeding the tunnel: 0 = AM824 (stream 0), 1 = AAF int32 (stream 1, default)",
        static_cast<uint64_t>(config.entity.udptun_source_stream),
        [&](auto v) { config.entity.udptun_source_stream = static_cast<uint16_t>(v & 0x1ULL); });
    specs.add<bool>(
        "udptun.egress",
        "Enable the inter-site UDPTUN egress: receive AAF-v1/AnnexJ packets on --udptun.listen, reclock them at "
        "TAI+WCL (drop-to-0 concealment), and emit the de-tunneled audio on the local AVB talkers in place of the tone",
        config.entity.udptun_egress,
        [&](auto v) { config.entity.udptun_egress = v; });
    specs.add<uint64_t>(
        "udptun.listen",
        "UDP port the egress binds to receive far-site packets (Annex J continuous default 17220)",
        static_cast<uint64_t>(config.entity.udptun_listen_port),
        [&](auto v) { config.entity.udptun_listen_port = static_cast<uint16_t>(v & 0xFFFFULL); });
    specs.add<uint64_t>(
        "udptun.wcl_ns",
        "Egress worst-case-latency / playout delay in ns (de-jitter buffer depth; default 25e6 = 25 ms)",
        static_cast<uint64_t>(config.entity.udptun_wcl_ns),
        [&](auto v) { config.entity.udptun_wcl_ns = static_cast<int64_t>(v); });
    specs.add<std::string>(
        "udptun.rendezvous",
        "STUN rendezvous server HOST:PORT for NAT traversal (e.g. reflex.statusbar.com:3478). When set, the entity "
        "performs a STUN handshake to discover the peer + obtain a hole-punched socket shared by ingest TX and egress "
        "RX; --udptun.peer/.listen are ignored. Requires --udptun.rendezvous_key/_session_id and a matching peer role",
        config.entity.udptun_rendezvous_server,
        [&](auto v) { config.entity.udptun_rendezvous_server = v; });
    specs.add<std::string>(
        "udptun.rendezvous_key",
        "64-hex (32-byte) AES-128-SIV shared key for the STUN rendezvous (must match the peer + server)",
        config.entity.udptun_rendezvous_key,
        [&](auto v) { config.entity.udptun_rendezvous_key = v; });
    specs.add<std::string>(
        "udptun.rendezvous_session_id",
        "32-hex (16-byte) rendezvous session id (both peers must use the same value)",
        config.entity.udptun_rendezvous_session_id,
        [&](auto v) { config.entity.udptun_rendezvous_session_id = v; });
    specs.add_choice(
        "udptun.rendezvous_role",
        "Rendezvous role; the two peers must use different roles",
        {"initiator", "responder"},
        "initiator",
        [&](auto v) { config.entity.udptun_rendezvous_role = std::string{v}; });
    specs.add<uint64_t>(
        "udptun.frames_per_packet",
        "Frames (samples/channel) per inter-site packet. Default 44 = 458us @ 96kHz, the largest that fits one "
        "Ethernet MTU at 8ch/int32 (1452-byte datagram, no IP fragmentation). 48 = round 500us but fragments into 2",
        static_cast<uint64_t>(config.entity.udptun_frames_per_packet),
        [&](auto v) { config.entity.udptun_frames_per_packet = static_cast<uint16_t>((v < 1) ? 1 : ((v > 1023) ? 1023 : v)); });
    specs.add<bool>(
        "udptun.silence_source",
        "Ingest emits zero-PCM (silence) frames at the media cadence even without a connected listener source -- the "
        "entity transmits silence as if its far talker (e.g. the DSP processor) were sending zeros / not received. Keeps the "
        "reverse "
        "tunnel + its NAT pinhole warm; with both ends transmitting, direct-peer hole-punches both ways without STUN",
        config.entity.udptun_silence_source,
        [&](auto v) { config.entity.udptun_silence_source = v; });
    specs.add<std::string>(
        "udptun.egress_colbin",
        "Path to a .colbin file the egress fills with one per-packet timing row (rx_TAI, presentation_time, latency = "
        "rx_TAI - PT, sequence) in the owlm UdpTunCsvRecord schema; read/plot with scripts/owlm/owlm_analyze. Empty = off",
        config.entity.udptun_egress_colbin_path,
        [&](auto v) { config.entity.udptun_egress_colbin_path = v; });
    specs.add<uint64_t>(
        "udptun.egress_colbin_max_mb",
        "Pre-allocated size (MiB) of the egress colbin. The recorder maps this in full at start and NEVER grows, so no "
        "mid-run mremap stalls the real-time data plane; recording stops if a run outlasts it. Default 256 = 15 min @ "
        "96 kHz with redundancy (~180 MiB of rows + headroom). Size it for your run; on a tmpfs/RAM path it is reserved "
        "in RAM up front",
        config.entity.udptun_egress_colbin_max_bytes / (1024ULL * 1024ULL),
        [&](auto v) { config.entity.udptun_egress_colbin_max_bytes = static_cast<uint64_t>((v < 1) ? 1 : v) * 1024ULL * 1024ULL; });
    specs.add<bool>(
        "udptun.redundant",
        "Send a temporally-shifted redundant copy of every ingest packet (owlm-style): same sequence + TAI, a distinct "
        "stream_id, sent --udptun.temporal_shift_ms later. The egress keys playout by TAI so a redundant fills a lost "
        "primary's slot; the colbin labels primary/redundant for owlm_analyze recovery accounting. Doubles wire bandwidth",
        config.entity.udptun_redundant,
        [&](auto v) { config.entity.udptun_redundant = v; });
    specs.add<int64_t>(
        "udptun.temporal_shift_ms",
        "Delay (ms) between a primary and its redundant copy (default 5). Only used with --udptun.redundant",
        config.entity.udptun_temporal_shift_ms,
        [&](auto v) { config.entity.udptun_temporal_shift_ms = v; });

    specs.add<double>("filter.freq_hz", "Filter center frequency (Hz)", config.entity.filter_freq_hz, [&](auto v) {
        config.entity.filter_freq_hz = v;
    });
    specs.add<double>(
        "filter.gain_db", "Filter gain (dB, negative=cut, positive=boost)", config.entity.filter_gain_db, [&](auto v) {
            config.entity.filter_gain_db = v;
        });
    specs.add<double>("filter.q", "Filter Q factor", config.entity.filter_q, [&](auto v) { config.entity.filter_q = v; });

    specs.add<double>(
        "tone.freq_hz",
        "Test-tone frequency per channel in Hz (default 96000/7 ~= 13714)",
        config.entity.tone_freq_hz,
        [&](auto v) { config.entity.tone_freq_hz = v; });
    specs.add<double>(
        "tone.amplitude",
        "Test-tone amplitude, 0..1 linear (1.0 = 0 dBFS; default 0.5 = -6 dBFS)",
        static_cast<double>(config.entity.tone_amplitude),
        [&](auto v) { config.entity.tone_amplitude = static_cast<float>(v); });

    return specs;
}

void print_usage(char const* program_name, args::ArgumentSpecs const& specs)
{
    static constexpr std::array<std::string_view, 2> examples{
        "--interface=eth0 --descriptor-storage=entity_audio.bin",
        "--ptp.driver=system --descriptor-storage=entity_audio.bin",
    };
    config::default_print_usage(
        program_name,
        specs,
        "AVB Entity Audio I/O Tool (dual-format AM824 + AAF)\n"
        "Runs an AVB entity with one AM824 stream pair and one AAF stream pair.",
        examples);

    std::print(stderr, "\nGenerate the blob with: statusbar-aemxml json2bin examples/dual.json entity_audio.bin\n");
    std::print(stderr, "\nPress Ctrl-C to stop.\n");
}

void print_entity_config(Config const& config, Entity const& entity)
{
    // Read entity fields from the entity's OWN config: the tool's config.entity
    // has been std::move()d into create() by the time this runs, so its
    // std::string members (entity_name, interface_name) are emptied. The
    // entity holds the live copy (incl. the MAC-derived id / hostname name).
    auto const& ecfg = entity.config();
    std::print("AVB Entity Audio I/O Tool (dual-format)\n");
    std::print("=======================================\n");
    std::print("Descriptor Storage: {}\n", config.tool.descriptor_storage_path);
    std::print("Entity ID:        {}\n", ieee::to_string(ecfg.entity_id).view());
    std::print("Entity Name:      {}\n", ecfg.entity_name);
    std::print("Interface:        {}\n", ecfg.interface_name);
    std::print("VLAN ID:          {}\n", ecfg.vlan_id);
    std::print("Stream 0 (AM824): dest {}\n", ieee::to_string(ecfg.am824_talker_dest_mac).view());
    std::print("Stream 1 (AAF):   dest {}  (int32 PCM)\n", ieee::to_string(ecfg.aaf_talker_dest_mac).view());
    std::print(
        "Stream 2 (CRF):   dest {}  (96 kHz media clock, {}x{} ts/pkt)\n",
        ieee::to_string(ecfg.crf_talker_dest_mac).view(),
        ecfg.crf_timestamps_per_packet,
        ecfg.crf_timestamp_interval);
    std::print("Channels:         {}\n", entity.channels());
    std::print(
        "Test Tone:        {:.1f} Hz, amplitude {:.3f} ({:+.1f} dBFS)\n",
        ecfg.tone_freq_hz,
        ecfg.tone_amplitude,
        ecfg.tone_amplitude > 0.0F ? 20.0 * std::log10(static_cast<double>(ecfg.tone_amplitude)) : -120.0);
    std::print("Sample Rate:      {} Hz, Samples/Packet: {}\n", Entity::SAMPLE_RATE, Entity::SAMPLES_PER_PACKET);
    {
        auto const base_ns = static_cast<uint64_t>(Entity::SAMPLES_PER_PACKET) * 1'000'000'000ULL / Entity::SAMPLE_RATE;
        std::print(
            "Packets/Wake:     {} (wake every {} us, {} pkt/s/stream{})\n",
            ecfg.packets_per_wake,
            (base_ns * ecfg.packets_per_wake) / 1000,
            Entity::SAMPLE_RATE / Entity::SAMPLES_PER_PACKET,
            ecfg.packets_per_wake > 1 ? " -- NON-strict Class A shaping" : "");
        std::print("Lock Tolerance:   {} ns\n", ecfg.lock_tolerance_ns);
        std::print(
            "Media clock rate: {}\n",
            ecfg.media_lock_to_gptp ? "r=1.0 PINNED to gPTP (CLOCK_REALTIME ignored)" : "r=gPTP/CLOCK_REALTIME (GPS-rate-pinned)");
    }
}

/// The shared runner loop plus the audio-io extras: the optional dedicated
/// SCHED_FIFO RX drain timer (stream_rx.rt_timer) wrapped around it, the
/// udptun log channel, and the per-second RX-timer telemetry line.
auto run_main_loop(net::MessageReactor& reactor, ptpclient::PtpAppContext& ctx, Entity& entity, Config const& config)
    -> avb_entity::MainLoopResult
{
    // ---- Optional dedicated RT RX timer (stream_rx.rt_timer) -------------------
    // Drain AM824/AAF on its own SCHED_FIFO core so the reactor/control-plane (and
    // media-timer preemption) can't delay RX -- which otherwise batch-tallies queued
    // frames against a now-later clock and reads as spurious LATE_TIMESTAMP. Its stats
    // mirror the media timer: wake accuracy, callback duration, per-tick batch size.
    itc::TelemetryCounter<int64_t> rx_timer_error_count;
    itc::TelemetryCounter<uint64_t> rx_frames_drained;
    itc::Published<int64_t> rx_last_wake_error_ns;
    itc::Published<int64_t> rx_last_batch;
    stats::AtomicHistogram<128> rx_dur_hist{stats::AtomicHistogramConfig{.low_ns = 0, .high_ns = 50'000, .bin_width_ns = 500}};

    int64_t const rx_period_ns = static_cast<int64_t>(entity.config().stream_rx_period_us) * 1'000;
    auto rx_timer = ptpclient::make_ptp_timer(
        *ctx.bridge,
        rx_period_ns,
        [&](StatusValue<ptpclient::TimerWakeInfo> const& wake_info) {
            if (!wake_info) {
                rx_timer_error_count.add(1);
                return;
            }
            auto const cb_t0 = std::chrono::steady_clock::now();
            size_t const n = entity.drain_stream_rx(wake_info->actual_time_ns);
            rx_dur_hist.update(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - cb_t0).count());
            rx_frames_drained.add(n);
            rx_last_batch.publish(static_cast<int64_t>(n));
            rx_last_wake_error_ns.publish(wake_info->error_ns);
        },
        ctx.compensation_ns,
        ctx.enable_realtime,
        entity.config().stream_rx_cpu_affinity);

    bool const rx_rt = entity.config().stream_rx_rt_timer;
    if (rx_rt) {
        if (auto const rs = rx_timer.start(); !rs) {
            std::print(stderr, "Warning: stream RX timer failed to start ({}); RX stays on the reactor\n", rs.error().message());
        } else {
            std::print(
                "Stream RX: dedicated SCHED_FIFO timer on core {} @ {} us tick\n",
                entity.config().stream_rx_cpu_affinity,
                entity.config().stream_rx_period_us);
        }
    }

    avb_entity::EntityRunnerHooks const hooks{
        .add_log_channels = [&entity](logging::LogCollector<4>& collector) { (void)collector.add(entity.udptun_log_channel()); },
        .extra_telemetry =
            [&]() {
                if (rx_rt) {
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
            },
    };

    // Base packet interval (12 samples @ 96 kHz = 125 µs) times packets_per_wake.
    int64_t const packet_period_ns = (static_cast<int64_t>(Entity::SAMPLES_PER_PACKET) * 1'000'000'000LL / Entity::SAMPLE_RATE) *
        static_cast<int64_t>(entity.config().packets_per_wake);

    auto result = avb_entity::run_entity_main_loop(reactor, ctx, entity, config.tool, packet_period_ns, hooks);

    if (rx_rt) {
        rx_timer.stop();
        std::print(
            stderr,
            "[rx-timer] final: frames_drained={} wake_errors={} recovery={} missed={}\n",
            rx_frames_drained.load(),
            rx_timer_error_count.load(),
            rx_timer.recovery_count(),
            rx_timer.missed_cycles());
    }
    return result;
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    using namespace statusbar;

    Config config;
#ifdef STATUSBAR_AVB_DEFAULT_BLOB
    config.tool.descriptor_storage_path = STATUSBAR_AVB_DEFAULT_BLOB;
#endif
    auto specs = build_arg_specs(config);

    auto cli_result = config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-avb-audio-io");
    if (!cli_result) {
        return config::handled_builtin_command(cli_result) ? 0 : 1;
    }

    if (config.entity.interface_name.empty()) {
        std::print(stderr, "Error: --interface=<iface> is required\n");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }
    if (config.tool.descriptor_storage_path.empty()) {
        std::print(stderr, "Error: --descriptor-storage=<path> is required\n");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }

    // entity_id: a per-node-unique modified EUI-64 from the NIC MAC (so two
    // nodes don't collide on the wire); entity_name: the hostname (so a
    // controller can connect by node name). Both overridable by --entity.id /
    // --entity.name.
    avb_entity::apply_node_identity_defaults(
        config.entity.interface_name,
        config.entity.entity_id,
        config.entity.entity_name,
        ieee::Eui64{0x70, 0xB3, 0xD5, 0xED, 0xCF, 0x00, 0x00, 0x02},
        "AVB Audio IO");

    auto blob = avb_entity::load_blob_file(config.tool.descriptor_storage_path);
    if (blob.empty()) {
        std::print(stderr, "Error: Failed to load descriptor storage file: {}\n", config.tool.descriptor_storage_path);
        return EXIT_FAILURE;
    }
    config.entity.descriptor_storage_blob = std::move(blob);

    realtime::setup_shutdown_signal_handlers();

    auto entity_result = avb_entity::AvbEntityAudioIO::create(std::move(config.entity));
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

    auto ctx_result = avb_entity::setup_entity_ptp(config.ptp_app, config.tool.require_ptp);
    if (!ctx_result) {
        (void)entity.stop();
        if (realtime::is_shutdown_requested()) {
            std::print("Shutdown requested during setup\n");
            return EXIT_SUCCESS;
        }
        std::print(stderr, "Error: Failed to setup PTP: {}\n", ctx_result.error().message());
        return EXIT_FAILURE;
    }
    auto& ctx = *ctx_result;
    std::print("\nStarting main loop (Ctrl-C to stop)...\n\n");

    auto loop_result = run_main_loop(reactor, ctx, entity, config);

    std::print("\n\nShutting down...\n");
    auto stop_time = TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    entity.on_link_down(stop_time);
    (void)entity.stop();
    std::print("Entity stopped.\n");

    avb_entity::print_final_status(entity, loop_result, config.tool.dump_stats_on_exit);
    return loop_result.exit_code;
}
