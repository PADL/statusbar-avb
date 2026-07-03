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
//    `aem-entity-blob --dual --out entity_audio.bin`)

#include "statusbar/avb_entity/avb_entity_audio_io.hpp"
#include "statusbar/avb_entity/avb_entity_identity.hpp"
#include "statusbar/buffer/stream_utils.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/gptp/gptp_format.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_published.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/itc/itc_telemetry_counter.hpp"
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
#include <expected>
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

/// Dump a histogram snapshot to CSV (bin_low_ns,bin_high_ns,count), matching
/// owlm's --wan-timer-*-stats-csv format so the same tooling reads both.
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
        .firmware_version = "1.0.0",
    };

    // Defaults to the descriptor blob shipped by the package (see
    // STATUSBAR_AVB_DEFAULT_BLOB, set in CMake); override with --descriptor-storage.
#ifdef STATUSBAR_AVB_DEFAULT_BLOB
    std::string descriptor_storage_path{STATUSBAR_AVB_DEFAULT_BLOB};
#else
    std::string descriptor_storage_path;
#endif
    bool verbose{true};
    bool dump_stats_on_exit{true};
    // Refuse to run if the PTP setup falls back to the (non-gPTP) system clock.
    // A talker on the local monotonic clock stamps AVTP timestamps in the wrong
    // domain, so every listener flags LATE_TIMESTAMP and the stream never locks.
    bool require_ptp{false};

    // Media-timer (8 kHz process_audio) RT diagnostics: wake-error and callback-
    // duration histograms dumped to CSV at exit (bin_low_ns,bin_high_ns,count).
    // Empty = disabled. Mirrors owlm's --wan-timer-*-stats-csv.
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
        "ATDECC wire version: '2016' truncates descriptors to their 1722.1-2013/2016 lengths for "
        "controllers that reject 2021 forms; '2021' emits full 2021 descriptors. Default 2016.",
        config.entity.atdecc_version,
        [&](auto v) { config.entity.atdecc_version = std::string{v}; });
    specs.add<uint16_t>("vlan_id", "VLAN ID for AVB streams", config.entity.vlan_id, [&](auto v) { config.entity.vlan_id = v; });
    specs.add<std::string>(
        "stream.address_mode",
        "Talker stream dest-address source: 'static' (the configured *_dest_mac) or 'maap' (acquire a "
        "block from the 1722 dynamic pool via MAAP at startup). Default static.",
        config.entity.stream_address_mode,
        [&](auto v) { config.entity.stream_address_mode = std::string{v}; });
    specs.add<bool>(
        "srp.redeclare_registered_listeners",
        "Sticky-Listener MSRP workaround: re-declare REGISTERED Listener attributes every periodic/LeaveAll "
        "pass (echo a downstream listener's Listener-Ready back to the bridge) so the bridge keeps forwarding "
        "our stream to that listener. Needed when feeding a downstream listener through an AVB switch. Default off.",
        config.entity.redeclare_registered_listeners,
        [&](auto v) { config.entity.redeclare_registered_listeners = v; });
    specs.add<bool>(
        "srp.suppress_leaveall",
        "Suppress-LeaveAll MSRP workaround: never originate a periodic LeaveAll; only re-assert declarations "
        "via the periodic timer (never release). Reproduces the earlier sticky behaviour some AVB switches "
        "need for stable downstream forwarding. Default off.",
        config.entity.suppress_leaveall,
        [&](auto v) { config.entity.suppress_leaveall = v; });
    specs.add<std::string>(
        "tx_pcap.path",
        "Diagnostic: record our OWN transmitted stream frames (AAF/AM824/CRF -- invisible to local capture due to "
        "PACKET_QDISC_BYPASS) to this libpcap file, gPTP-timestamped, for tx_pcap.seconds. Lets you inspect the AVTP "
        "presentation timestamps (media clock) we put on the wire. Empty = off.",
        config.entity.tx_pcap_path,
        [&](auto v) { config.entity.tx_pcap_path = std::string{v}; });
    specs.add<uint32_t>(
        "tx_pcap.seconds",
        "Capture window in seconds for tx_pcap.path, from the first transmitted frame (default 10).",
        config.entity.tx_pcap_seconds,
        [&](auto v) { config.entity.tx_pcap_seconds = v; });
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
        "presentation_offset_ns",
        "AVTP presentation-time offset added to gPTP time to form the stream timestamp (ns)",
        config.entity.presentation_offset_ns,
        [&](auto v) { config.entity.presentation_offset_ns = v; });
    specs.add<uint64_t>(
        "media.packets_per_wake",
        "Stream packets per stream per media-timer wake (default 1 = strict Class A shaping). N>1 wakes "
        "every N*125us and bursts N x 12-sample packets/stream: same 8000 pkt/s wire rate, N x fewer "
        "deadlines to miss, but non-compliant Class A shaping (absorbed by presentation_offset_ns)",
        static_cast<uint64_t>(config.entity.packets_per_wake),
        [&](auto v) { config.entity.packets_per_wake = static_cast<size_t>((v < 1) ? 1 : ((v > 16) ? 16 : v)); });
    specs.add<uint64_t>(
        "lock.tolerance_ns",
        "MEDIA_LOCKED detector tolerance in ns (default 5000 = 5us). Widen to tolerate talker media-timer "
        "jitter (e.g. with media.packets_per_wake>1 or a hot talker) without flapping the lock",
        static_cast<uint64_t>(config.entity.lock_tolerance_ns),
        [&](auto v) { config.entity.lock_tolerance_ns = static_cast<uint32_t>((v > 0xFFFFFFFFULL) ? 0xFFFFFFFFULL : v); });
    specs.add<bool>(
        "gate.talker_on_listener",
        "Transmit a talker stream only when it is fully SR-class admitted: an ACMP connection AND MSRP Listener "
        "Ready (with grace) for that stream; stay silent otherwise (IEEE 802.1Q SRP). Each stream gates "
        "independently -- the CRF media clock on its own connection + reservation, not on the audio talkers. "
        "Default true. Set false to stream unconditionally from link-up (non-spec: no reservation, bench only)",
        config.entity.gate_talker_on_listener,
        [&](auto v) { config.entity.gate_talker_on_listener = v; });
    specs.add<bool>(
        "media.lock_to_gptp",
        "Pin media-clock rate ratio r=1.0 (media clock = gPTP) instead of r=gPTP/CLOCK_REALTIME. Use when the "
        "gPTP grandmaster is the reference the listener recovers against and CLOCK_REALTIME is NTP (not GPS), so "
        "r would otherwise wander and wobble the listener's recovered clock. Default false (GPS-rate-pinned)",
        config.entity.media_lock_to_gptp,
        [&](auto v) { config.entity.media_lock_to_gptp = v; });
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
    specs.add<std::string>(
        "media-timer-wake-csv",
        "Dump the 8 kHz media-timer wake-error histogram (bin_low_ns,bin_high_ns,count) to this CSV at exit. Empty = off",
        config.media_timer_wake_csv_path,
        [&](auto v) { config.media_timer_wake_csv_path = v; });
    specs.add<std::string>(
        "media-timer-duration-csv",
        "Dump the media-timer callback-DURATION histogram (how long process_audio takes per wake) to this CSV at exit",
        config.media_timer_duration_csv_path,
        [&](auto v) { config.media_timer_duration_csv_path = v; });

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

    specs.add<std::string>(
        "descriptor-storage",
        "Path to descriptor storage .bin file (2 in + 2 out; defaults to the packaged blob)",
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
        "--interface=eth0 --descriptor-storage=entity_audio.bin",
        "--ptp.driver=system --descriptor-storage=entity_audio.bin",
    };
    config::default_print_usage(
        program_name,
        specs,
        "AVB Entity Audio I/O Tool (dual-format AM824 + AAF)\n"
        "Runs an AVB entity with one AM824 stream pair and one AAF stream pair.",
        examples);

    std::print(stderr, "\nGenerate the blob with: aem-entity-blob --dual --out entity_audio.bin\n");
    std::print(stderr, "\nPress Ctrl-C to stop.\n");
}

void print_entity_config(Config const& config, avb_entity::AvbEntityAudioIO const& entity)
{
    // Read entity fields from the entity's OWN config: the tool's config.entity
    // has been std::move()d into create() by the time this runs, so its
    // std::string members (entity_name, interface_name) are emptied. The
    // entity holds the live copy (incl. the MAC-derived id / hostname name).
    auto const& ecfg = entity.config();
    std::print("AVB Entity Audio I/O Tool (dual-format)\n");
    std::print("=======================================\n");
    std::print("Descriptor Storage: {}\n", config.descriptor_storage_path);
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
    std::print(
        "Sample Rate:      {} Hz, Samples/Packet: {}\n",
        avb_entity::AvbEntityAudioIO::SAMPLE_RATE,
        avb_entity::AvbEntityAudioIO::SAMPLES_PER_PACKET);
    {
        auto const base_ns = static_cast<uint64_t>(avb_entity::AvbEntityAudioIO::SAMPLES_PER_PACKET) * 1'000'000'000ULL /
            avb_entity::AvbEntityAudioIO::SAMPLE_RATE;
        std::print(
            "Packets/Wake:     {} (wake every {} us, {} pkt/s/stream{})\n",
            ecfg.packets_per_wake,
            (base_ns * ecfg.packets_per_wake) / 1000,
            avb_entity::AvbEntityAudioIO::SAMPLE_RATE / avb_entity::AvbEntityAudioIO::SAMPLES_PER_PACKET,
            ecfg.packets_per_wake > 1 ? " -- NON-strict Class A shaping" : "");
        std::print("Lock Tolerance:   {} ns\n", ecfg.lock_tolerance_ns);
        std::print(
            "Media clock rate: {}\n",
            ecfg.media_lock_to_gptp ? "r=1.0 PINNED to gPTP (CLOCK_REALTIME ignored)" : "r=gPTP/CLOCK_REALTIME (GPS-rate-pinned)");
    }
}

struct MainLoopResult
{
    int exit_code{EXIT_SUCCESS};
    stats::AtomicWakeStats::Snapshot wake_stats{};
    int64_t compensation_ns{0};
    int64_t recovery_count{0};
    int64_t missed_cycles{0};
};

MainLoopResult run_main_loop(
    net::MessageReactor& reactor, ptpclient::PtpAppContext& ctx, avb_entity::AvbEntityAudioIO& entity, Config const& config)
{
    MainLoopResult result;
    result.compensation_ns = ctx.compensation_ns;

    // Base packet interval (12 samples @ 96 kHz = 125 µs) times packets_per_wake:
    // the media timer wakes once per N packets and process_audio() emits N packets
    // per stream per wake, so the wire rate is fixed at 8000 pkt/s/stream while the
    // wake rate drops to 8000/N Hz (fewer deadlines to miss). N=1 = strict Class A.
    int64_t const PACKET_PERIOD_NS = (static_cast<int64_t>(avb_entity::AvbEntityAudioIO::SAMPLES_PER_PACKET) * 1'000'000'000LL /
                                      avb_entity::AvbEntityAudioIO::SAMPLE_RATE) *
        static_cast<int64_t>(config.entity.packets_per_wake);

    auto* net_handlers = entity.net_handlers();
    bool had_grandmaster = net_handlers != nullptr ? net_handlers->gptp_handler().has_grandmaster() : false;

    // Callback-duration histogram for the media timer: how long process_audio()
    // takes per wake (the timer's own stats() track only wake-error). Same bin
    // shape owlm uses; dumped to --media-timer-duration-csv at exit.
    stats::AtomicHistogram<128> media_dur_hist{stats::AtomicHistogramConfig{.low_ns = 0, .high_ns = 100'000, .bin_width_ns = 1000}};

    // The media-timer callback runs on a SCHED_FIFO RT thread, so it must NOT
    // std::print (alloc/throw/block) — it publishes to these lock-free itc
    // channels and the control loop below surfaces them off-thread.
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

    // Drive the initial link-up synchronously (before the reactor runs) so the
    // supervisor leaves Down -> Init before any gPTP announce is processed.
    auto startup_time = TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    entity.on_link_up(startup_time);

    // Then track the interface link state continuously: a real cable/switch/port
    // flap now emits genuine down/up events (the redundant first up is a no-op
    // since the supervisor already left Down), instead of staying on the stale
    // startup assumption. Polls SIOCGIFFLAGS/IFF_RUNNING every 500 ms.
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

    while (!realtime::is_shutdown_requested()) {
        (void)reactor.poll_once(100);

        // TX pcap capture finished (window elapsed / ring full): write the file
        // here, off the media RT thread.
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

    timer.stop();
    if (auto const errs = timer_error_count.load(); errs > 0) {
        std::print(stderr, "Warning: media timer had {} wake error(s) (likely gPTP sync loss)\n", errs);
    }
    result.wake_stats = timer.stats();
    result.recovery_count = timer.recovery_count();
    result.missed_cycles = timer.missed_cycles();
    ctx.guard.stop();

    // Dump the media-timer RT histograms to CSV (bin_low_ns,bin_high_ns,count).
    if (!config.media_timer_wake_csv_path.empty()) {
        (void)write_histogram_csv(config.media_timer_wake_csv_path, result.wake_stats.error_histogram);
    }
    if (!config.media_timer_duration_csv_path.empty()) {
        (void)write_histogram_csv(config.media_timer_duration_csv_path, media_dur_hist.snapshot());
    }

    return result;
}

void print_final_status(avb_entity::AvbEntityAudioIO const& entity, MainLoopResult const& result, bool dump_stats)
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

    auto cli_result = config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-avb-audio-io");
    if (!cli_result) {
        return config::handled_builtin_command(cli_result) ? 0 : 1;
    }

    // Per-node-unique identity, unless overridden by --entity.id / --entity.name.
    // entity_id: a modified EUI-64 derived from the interface MAC (OUI:FF:FE:NIC),
    // so two nodes running the same blob never collide on the wire (ACMP/AECP
    // address entities by entity_id). entity_name: the hostname, so a controller
    // can connect by node name (e.g. `--talker node-a:0`).
    if (config.entity.interface_name.empty()) {
        std::print(stderr, "Error: --interface=<iface> is required\n");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }

    if (config.descriptor_storage_path.empty()) {
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

    auto blob = load_file(config.descriptor_storage_path);
    if (blob.empty()) {
        std::print(stderr, "Error: Failed to load descriptor storage file: {}\n", config.descriptor_storage_path);
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

    auto is_shutdown = [] { return realtime::is_shutdown_requested(); };
    // Remember what the user asked for: setup_ptp_app_with_fallback rewrites
    // driver_name/device_path to "system" if it silently falls back.
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

    // Detect a silent fallback to the local system clock. For an AVB TALKER this
    // is a correctness bug, not a convenience: avtp_timestamp is then formed from
    // the local monotonic clock instead of gPTP, so every listener flags
    // LATE_TIMESTAMP and never locks. The usual cause is no read access to the PTP
    // hardware clock (/dev/ptp0 is root-only) -- run as root or install the udev
    // rule shipped with this package. Warn loudly; refuse if --ptp.require=true.
    if (requested_driver != "system" && config.ptp_app.driver_name == "system") {
        std::print(
            stderr,
            "\n"
            "*** WARNING: PTP fell back to the SYSTEM clock -- could not open the gPTP PHC ({}).\n"
            "***          AVTP timestamps will be in the local clock domain, NOT gPTP, so\n"
            "***          listeners will report LATE_TIMESTAMP and the stream will not lock.\n"
            "***          Run as root, or grant read access to the PHC (udev rule), so the\n"
            "***          linuxptp driver can read it.\n"
            "\n",
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
