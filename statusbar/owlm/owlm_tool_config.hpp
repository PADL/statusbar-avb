#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// owlm_tool_config — Config struct, CLI parsing, and helpers that
// map a Config into udptun::SessionConfig / TimeSourceConfig.
//
// Linux-only because the embedded types (realtime::TimerConfig,
// realtime::TraceConfig, gptp::Profile-coupled session config,
// ptpclient::PtpAppConfig, the udptun time-source flavours) come from
// Linux-only modules. When/if those gain macOS support or stand-ins,
// the guard here can be relaxed and a portable subset can compile on
// other platforms. See docs/OWLM.md for the protocol/CLI spec.

#if defined(__linux__)

#    include "statusbar/args/args_spec.hpp"
#    include "statusbar/gptp/gptp_config.hpp"
#    include "statusbar/ptpclient/ptpclient_setup.hpp"
#    include "statusbar/realtime/realtime_timer_config.hpp"
#    include "statusbar/realtime/realtime_tripwire.hpp"
#    include "statusbar/udptun/udptun_session.hpp"
#    include "statusbar/udptun/udptun_time_source.hpp"

#    include <cstdint>
#    include <ctime>
#    include <string>
#    include <string_view>

namespace statusbar::owlm::tool {

enum class Mode
{
    Normal,
    Reflect
};

[[nodiscard]] inline auto parse_mode(std::string_view s) noexcept -> Mode
{
    return (s == "reflect") ? Mode::Reflect : Mode::Normal;
}

[[nodiscard]] inline auto parse_profile(std::string_view s) noexcept -> ::statusbar::gptp::Profile
{
    return (s == "automotive") ? ::statusbar::gptp::Profile::AvnuAutomotive : ::statusbar::gptp::Profile::Standard;
}

[[nodiscard]] inline auto parse_bridge_clock(std::string_view s) noexcept -> clockid_t
{
    if (s == "monotonic") {
        return CLOCK_MONOTONIC;
    }
    if (s == "monotonic_raw") {
        return CLOCK_MONOTONIC_RAW;
    }
    if (s == "realtime") {
        return CLOCK_REALTIME;
    }
    return -1;  // "none"
}

[[nodiscard]] inline auto parse_time_source(std::string_view s) noexcept -> ::statusbar::udptun::TimeSourceKind
{
    if (s == "ptp4l") {
        return ::statusbar::udptun::TimeSourceKind::Ptp4l;
    }
    if (s == "realtime") {
        return ::statusbar::udptun::TimeSourceKind::Realtime;
    }
    return ::statusbar::udptun::TimeSourceKind::GptpSlave;
}

struct Config
{
    // Inherited gPTP flags
    std::string gptp_interface{"eth0"};
    bool software_timestamping{false};
    int64_t manual_peer_delay_ns{-1};
    int64_t phase_jump_threshold_ns{-1};
    double servo_kp{-1.0};
    double servo_ki{-1.0};
    bool verbose{false};

    Mode mode{Mode::Normal};

    // Rendezvous flags. When --rendezvous-server is non-empty, the tool
    // performs a STUN REGISTER handshake before constructing the udptun
    // Session, then uses the resulting socket + peer reflexive address
    // for owlm. --peer / --peer-port are ignored in that case.
    std::string rendezvous_server{};
    std::string rendezvous_key_hex{};
    std::string rendezvous_session_id_hex{};
    std::string rendezvous_role{"initiator"};
    int64_t rendezvous_timeout_ms{60'000};
    bool rendezvous_verbose{false};

    // OWLM flags
    std::string owlm_interface{};
    std::string peer_host{};
    uint16_t peer_port{9991};
    uint16_t local_port{9991};
    uint64_t tx_interval_us{100'000};
    uint64_t report_interval_us{1'000'000};
    uint16_t id_mid{0xFFFE};
    uint32_t payload_bytes{0};
    // Set by --packet-profile=audio-8ch-96k; applied post-parse (see
    // apply_packet_profile) because the arg framework runs every unset option's
    // default callback after explicit ones, which would clobber a callback-set
    // payload_bytes/tx_interval_us.
    bool packet_profile_audio{false};
    int mcast_ttl{1};
    int dscp{46};  // EF — interactive low-latency; Wi-Fi maps this to AC_VO
    bool ipv6{false};
    uint64_t max_sources{32};
    int64_t hist_low_ns{0};
    int64_t hist_high_ns{300'000'000};  // 300 ms
    int64_t hist_bucket_ns{500'000};    // 500 µs → 600 bins
    bool redundant{true};               ///< Send a delayed redundant copy of every primary packet
    int64_t temporal_shift_ms{10};      ///< How far in the past the redundant copy was originally sent
    int64_t worst_case_latency_ms{60};  ///< PT = acquisition_wall + this offset; receiver lateness is rx − PT
    int64_t peer_tai_offset_ns{
        0};  ///< Signed bias added to cross-clock latency to compensate for known peer-vs-local GM TAI offset
    /// Translate the wire timeline to absolute GPS-TAI (master clock for
    /// short-term stability + Kalman-tracked offset vs chrony-disciplined
    /// CLOCK_REALTIME for the absolute epoch + manual leap offset). Needed
    /// for cross-site one-way latency now that the site gPTP GMs are
    /// free-running switches whose epoch is not TAI.
    bool wire_clock_gps_tai{false};
    /// TAI − UTC in ns, applied manually (37 s since 2017-01-01). Only
    /// used with --wire-clock=gps-tai.
    int64_t tai_minus_utc_ns{37'000'000'000LL};
    /// Per-packet CSV output path. Empty = disabled (no file opened,
    /// the RX hot path does only a null-pointer check per packet). When
    /// non-empty, Session opens the file in its constructor and runs a
    /// low-priority writer thread that streams each received packet to
    /// disk via an SPSC ring — the realtime timer thread never blocks
    /// on disk I/O. Overflows when the writer can't keep up (multi-sec
    /// SD-card stall) are silently dropped and counted in
    /// Session::csv_records_dropped().
    std::string csv_output_path{};
    /// Per-packet binary output path (.colbin). Empty (default) disables.
    /// Same record content as the CSV; ~10× smaller per row and mmap-
    /// appended for negligible per-packet write cost. Can be enabled
    /// alongside --csv-output to write both formats simultaneously.
    std::string bin_output_path{};
    /// Pre-allocated colbin size in MiB. 0 (default) = auto: sized from
    /// --duration-s when set (worst-case row rate × duration + headroom),
    /// else the SessionConfig default (15 min @ 96 kHz). The writer maps this
    /// in full and never grows, so no mid-run mremap stalls the RT data plane;
    /// a run that outlasts it stops recording with capacity_exceeded.
    uint64_t colbin_max_mb{0};
    /// Wake-error histogram CSV output path for the realtime timer.
    /// Empty (default) disables. When set, dumped at end of run
    /// regardless of --enable-tripwire (collection is already
    /// unconditional). One row per bin: bin_low_ns,bin_high_ns,count.
    /// Underflow row has an empty bin_low_ns; overflow row has an
    /// empty bin_high_ns.
    std::string wan_timer_wake_stats_csv_path{};
    /// Callback-duration histogram CSV output path. Empty (default)
    /// disables. Same row format and gating semantics as
    /// --wan-timer-wake-stats-csv.
    std::string wan_timer_duration_stats_csv_path{};
    /// Hard run duration in seconds. 0 (default) = run until SIGINT /
    /// SIGTERM. >0 schedules a graceful stop after this many wall-clock
    /// seconds have elapsed since `run()` started (raises the same
    /// stop flag the signal handlers raise; drain, summary, and CSV
    /// flush all run as usual).
    double duration_s{0.0};
    /// Wire-time source selector. `gptp-slave` (default) uses
    /// statusbar's own pure-C++ gPTP slave; `ptp4l` consumes a kernel
    /// PHC kept in sync by an external ptp4l/phc2sys daemon (mirrors
    /// rttest_send_avtp's --use-ptp-clock path); `realtime` skips any
    /// master clock and stamps with CLOCK_REALTIME (replaces the old
    /// --no-gptp flag).
    ::statusbar::udptun::TimeSourceKind time_source{::statusbar::udptun::TimeSourceKind::GptpSlave};
    /// Ptp4l-mode: PTP client / bridge / sampling settings (consumed
    /// only when --time-source=ptp4l). Mirrors rttest's --ptp.* flags.
    ::statusbar::ptpclient::PtpAppConfig ptp4l{};
    bool gptp_verbose{false};  ///< Allow gPTP slave to print stderr diagnostics

    int bridge_stable_samples{16};             ///< Bridge-stability window: # consecutive anchor refreshes
    double bridge_stable_rate_ppm{3.0};        ///< Max rate spread across window (ppm)
    int64_t bridge_stable_offset_ns{100'000};  ///< Max offset spread across window (ns)

    /// Realtime timer for the WAN transport loop (Linux + gPTP only).
    /// Default period 1 ms (was --rt-tick-us=1000). Set period_ns=0 to
    /// fall back to the portable polling loop. cpu/priority/busy_wait
    /// are configurable via --wan_timer.* and TOML [wan_timer].
    /// start_offset_cycles defaults to 5000 (5 s at 1 ms period) so the
    /// gPTP servo + mraw↔gPTP bridge have time to settle before the
    /// first scheduled wake — otherwise the first deadline can land in
    /// the past, causing a multi-second wake-error and a spurious
    /// tripwire fire on tick 0.
    ::statusbar::realtime::TimerConfig wan_timer{.period_ns = 1'000'000, .start_offset_cycles = 5'000};

    /// Tripwire / wake-latency / callback-duration diagnostics for the
    /// realtime timer. Off by default — flip on with --enable-tripwire
    /// to engage the wake/duration thresholds and dump per-timer wake +
    /// callback-duration histograms at exit. Thresholds default to the
    /// permissive values below; tune with --trace.* flags.
    ///   wake-error threshold:    500 µs (50 % of a 1 ms tick)
    ///   callback-duration limit: 800 µs (80 % of a 1 ms tick)
    ///   ftrace capture:          off (zero kernel overhead; flip on
    ///                                 with --trace.enable_tracing=true
    ///                                 to dump artifacts on a fire)
    bool enable_tripwire{false};
    ::statusbar::realtime::TraceConfig trace{
        .tripwire_threshold_ns = 500'000,
        .tripwire_duration_threshold_ns = 800'000,
        .enable_tracing = false,
    };

    // Derived
    ::statusbar::gptp::Profile profile{::statusbar::gptp::Profile::Standard};
    clockid_t bridge_clock{CLOCK_MONOTONIC_RAW};
};

inline auto build_arg_specs(Config& c) -> ::statusbar::args::ArgumentSpecs
{
    ::statusbar::args::ArgumentSpecs s;

    s.add_device("gptp-interface", "gPTP network interface", "eth0", [&](auto v) { c.gptp_interface = std::string{v}; });
    s.add_choice("profile", "gPTP profile", {"standard", "automotive"}, "standard", [&](auto v) { c.profile = parse_profile(v); });
    s.add<int64_t>("manual-peer-delay", "Manual peer delay in ns", -1, [&](auto v) { c.manual_peer_delay_ns = v; });
    s.add<int64_t>("phase-jump-threshold", "Phase jump threshold in ns", -1, [&](auto v) { c.phase_jump_threshold_ns = v; });
    s.add<double>("servo-ki", "Servo PI integral gain", -1.0, [&](auto v) { c.servo_ki = v; });
    s.add<double>("servo-kp", "Servo PI proportional gain", -1.0, [&](auto v) { c.servo_kp = v; });
    s.add_flag("software", "Use software timestamping (no PHC required)", [&](auto v) { c.software_timestamping = v; });
    s.add_choice(
        "bridge-clock",
        "POSIX clock to bridge gPTP against (the wire timeline runs on CLOCK_MONOTONIC_RAW; only monotonic_raw is accepted)",
        {"none", "monotonic", "monotonic_raw", "realtime"},
        "monotonic_raw",
        [&](auto v) { c.bridge_clock = parse_bridge_clock(v); });
    s.add_flag("verbose", "Print every sync update", [&](auto v) { c.verbose = v; });

    s.add_choice("mode", "Operating mode", {"normal", "reflect"}, "normal", [&](auto v) { c.mode = parse_mode(v); });
    s.add_device(
        "owlm-interface", "Interface for UDP send/recv (empty = wildcard)", "", [&](auto v) { c.owlm_interface = std::string{v}; });
    s.add<std::string>("peer", "Destination IP (unicast or IPv4 multicast)", std::string{}, [&](auto v) { c.peer_host = v; });
    s.add<uint16_t>("peer-port", "Destination UDP port", 9991, [&](auto v) { c.peer_port = v; });
    s.add<uint16_t>("local-port", "Local UDP bind port", 9991, [&](auto v) { c.local_port = v; });
    s.add<uint64_t>(
        "tx-interval-us", "Microseconds between transmits (0 = RX only)", 100'000, [&](auto v) { c.tx_interval_us = v; });
    s.add<uint64_t>("report-interval-ms", "Live summary print cadence in milliseconds", 1'000, [&](auto v) {
        c.report_interval_us = v * 1'000;
    });
    s.add<uint16_t>(
        "id-mid", "Middle 2 bytes of EUI-64 (legacy single-stream mode; ignored when --redundant is on)", 0xFFFE, [&](auto v) {
            c.id_mid = v;
        });
    s.add_flag("no-redundant", "Disable the default dual-stream redundancy and send a single legacy stream", [&](auto v) {
        c.redundant = !v;
    });
    s.add<int64_t>("temporal-shift-ms", "Delay between primary packet and its redundant copy (ms)", 10, [&](auto v) {
        c.temporal_shift_ms = v;
    });
    s.add<int64_t>(
        "worst-case-latency-ms",
        "Sender-applied offset between acquisition time and the presentation time stamped on the wire "
        "(PT = now_wall + this offset). Set to the deployment's tolerated worst-case one-way latency: "
        "the receiver's lateness metric is rx_wall - PT, so packets arriving on time read as lateness "
        "≤ 0 and a positive lateness means the packet missed its WCL deadline. 0 disables the offset "
        "(PT == acquisition; lateness then equals real one-way transit).",
        60,
        [&](auto v) { c.worst_case_latency_ms = v; });
    s.add<int>(
        "bridge-stable-samples",
        "Number of consecutive gPTP-bridge anchor refreshes that must stay within the stability "
        "thresholds before owlm starts sending OWLM packets. The bridge stores the most recent "
        "sample's rate without smoothing, so the cached rate can swing tens of ppm during servo "
        "pull-in. The bootstrap blocks until this many consecutive anchors all fall inside "
        "--bridge-stable-rate-ppm and --bridge-stable-offset-ns. Larger window = slower start but "
        "more confidence the bridge has converged. Capped at 128 internally.",
        16,
        [&](auto v) { c.bridge_stable_samples = v; });
    s.add<double>(
        "bridge-stable-rate-ppm", "Max peak-to-peak spread of bridge rate across the stability window (ppm).", 3.0, [&](auto v) {
            c.bridge_stable_rate_ppm = v;
        });
    s.add<int64_t>(
        "bridge-stable-offset-ns",
        "Max peak-to-peak spread of bridge offset across the stability window (ns).",
        100'000,
        [&](auto v) { c.bridge_stable_offset_ns = v; });
    s.add<int64_t>(
        "peer-tai-offset-ns",
        "Signed nanosecond offset added to cross-clock one-way latency measurements to compensate "
        "for a known constant residual between the peer's grandmaster and ours (e.g. two GPS-disciplined "
        "GMs that each track TAI to ~1 µs but with a small offset between them). Positive when the peer's "
        "clock leads ours. Applied only to incoming RemotePrimary / RemoteLegacy packets; never to RTT or "
        "self-redundancy. 0 = no compensation.",
        0,
        [&](auto v) { c.peer_tai_offset_ns = v; });
    s.add_choice(
        "time-source",
        "Wire-time source. 'gptp-slave' (default) starts statusbar's own pure-C++ gPTP slave "
        "on --gptp-interface. 'ptp4l' consumes a kernel PHC kept in sync by an external "
        "ptp4l/phc2sys daemon (uses --ptp.driver/--ptp.device for device selection). "
        "'realtime' skips any master clock and stamps packets with CLOCK_REALTIME — assumes "
        "both endpoints are NTP-synced (replaces the old --no-gptp flag).",
        {"gptp-slave", "ptp4l", "realtime"},
        "gptp-slave",
        [&](auto v) { c.time_source = parse_time_source(v); });
    s.add_choice(
        "wire-clock",
        "Timeline the on-wire timestamps live in. 'master' (default) stamps the raw time-source "
        "timeline — fine within one gPTP domain, but since the site GMs became free-running "
        "switches their epoch is arbitrary, so cross-site latency is meaningless. 'gps-tai' "
        "keeps the master clock for short-term stability but maps every on-wire timestamp to "
        "absolute GPS-TAI via a 3-state Kalman filter tracking (master − CLOCK_REALTIME), with "
        "CLOCK_REALTIME chrony-disciplined to the site's GPS NTP source, plus the manual "
        "--tai-minus-utc-ns leap offset. Both peers running 'gps-tai' share one absolute "
        "timeline, making cross-site one-way latency directly meaningful. Does not touch ptp4l, "
        "the PHC, or kernel TAI.",
        {"master", "gps-tai"},
        "master",
        [&](auto v) { c.wire_clock_gps_tai = (v == std::string_view{"gps-tai"}); });
    s.add<int64_t>(
        "tai-minus-utc-ns",
        "TAI − UTC in nanoseconds, applied manually when --wire-clock=gps-tai (37 s, the value in "
        "force since 2017-01-01). The kernel's TAI offset is never consulted.",
        37'000'000'000LL,
        [&](auto v) { c.tai_minus_utc_ns = v; });
    ::statusbar::ptpclient::add_ptp_arg_specs(s, c.ptp4l);
    s.add_flag(
        "gptp-verbose",
        "Allow the gPTP slave to print stderr diagnostics. Off by default — when running under "
        "the realtime timer the callback must stay IO-free, so gPTP debug output is suppressed unless "
        "explicitly enabled. Only meaningful when --time-source=gptp-slave.",
        [&](auto v) { c.gptp_verbose = v; });
    s.merge(::statusbar::realtime::build_timer_arg_specs(c.wan_timer), "wan_timer.");
    s.add_flag(
        "enable-tripwire",
        "Enable the realtime-timer tripwire and per-tick wake/duration "
        "diagnostics. When off (default), the realtime timer runs without "
        "monitoring overhead and no histograms are printed at exit.",
        [&](auto v) { c.enable_tripwire = v; });
    s.merge(::statusbar::realtime::build_trace_arg_specs(c.trace), "trace.");
    s.add<uint32_t>("payload-bytes", "Pad bytes after the 32-byte header", 0, [&](auto v) { c.payload_bytes = v; });
    s.add_choice(
        "packet-profile",
        "Preset packet size + rate (overrides payload-bytes/tx-interval-us, applied post-parse). 'audio-8ch-96k' -> "
        "payload-bytes=1420 + tx-interval-us=458: a 1452-byte UDP datagram at ~2180 pps, matching the AAF-v1/AnnexJ "
        "8ch/96kHz inter-site audio tunnel (44 frames/packet, single Ethernet MTU). 'default' = no preset.",
        {"default", "audio-8ch-96k"},
        "default",
        [&](auto v) { c.packet_profile_audio = (v == std::string_view{"audio-8ch-96k"}); });
    s.add<int>("mcast-ttl", "IP_MULTICAST_TTL when peer is multicast", 1, [&](auto v) { c.mcast_ttl = v; });
    s.add<int>(
        "dscp",
        "DSCP value (0-63); -1 disables. Default 46 (EF, Expedited Forwarding) — DSCP-aware "
        "switches and Wi-Fi WMM (AC_VO) prioritize this class. Most public-internet hops strip it.",
        46,
        [&](auto v) { c.dscp = v; });
    s.add_flag("ipv6", "Bind IPv6 in RX-only mode (otherwise inferred from --peer)", [&](auto v) { c.ipv6 = v; });
    s.add<uint64_t>("max-sources", "Cap on tracked sources (LRU evict)", 32, [&](auto v) { c.max_sources = v; });
    s.add<int64_t>("hist-low-ns", "Histogram lower bound (ns)", 0, [&](auto v) { c.hist_low_ns = v; });
    s.add<int64_t>("hist-high-ns", "Histogram upper bound (ns)", 300'000'000, [&](auto v) { c.hist_high_ns = v; });
    s.add<int64_t>("hist-bucket-ns", "Histogram bucket width (ns)", 500'000, [&](auto v) { c.hist_bucket_ns = v; });
    s.add<std::string>(
        "csv-output",
        "Stream per-packet CSV to PATH for the duration of the session (empty = disabled). "
        "Each row is rx_gptp_ns,presentation_time_ns,latency_ns,sender_eui64,sequence,interval_us,role. "
        "Writes go through an SPSC ring drained by a low-priority writer thread so the RX hot path "
        "never touches disk; on multi-second writer stalls, records are dropped and counted.",
        std::string{},
        [&](auto v) { c.csv_output_path = v; });
    s.add<std::string>(
        "bin-output",
        "Stream per-packet binary records to PATH (.colbin format, native byte order, "
        "48 bytes/row). Same fields as --csv-output but mmap-appended for negligible per-packet "
        "write cost. Use scripts/owlm/owlm_analyze.py to read; both --csv-output and --bin-output "
        "can be enabled simultaneously.",
        std::string{},
        [&](auto v) { c.bin_output_path = v; });
    s.add<uint64_t>(
        "colbin-max-mb",
        "Pre-allocated size (MiB) of the --bin-output colbin; the writer maps it in full and never grows "
        "(no mid-run mremap to stall the RX path). 0 (default) auto-sizes from --duration-s, or uses the "
        "15-min @ 96 kHz default when no duration is set. A run that outlasts it stops recording.",
        uint64_t{0},
        [&](auto v) { c.colbin_max_mb = v; });
    s.add<std::string>(
        "wan-timer-wake-stats-csv",
        "Dump the realtime timer's wake-error histogram to PATH at end of run. Each row is "
        "bin_low_ns,bin_high_ns,count; underflow row has empty bin_low_ns, overflow row has empty "
        "bin_high_ns. Collected on every tick regardless of --enable-tripwire.",
        std::string{},
        [&](auto v) { c.wan_timer_wake_stats_csv_path = v; });
    s.add<std::string>(
        "wan-timer-duration-stats-csv",
        "Dump the realtime timer's callback-duration histogram to PATH at end of run. Same row "
        "format as --wan-timer-wake-stats-csv. Collected on every tick regardless of "
        "--enable-tripwire.",
        std::string{},
        [&](auto v) { c.wan_timer_duration_stats_csv_path = v; });
    s.add<double>(
        "duration-s",
        "Hard run duration in seconds (wall-clock since session start). 0 = run until SIGINT/SIGTERM. "
        ">0 schedules a graceful stop after the deadline — drain, summary, and CSV flush all run "
        "as usual. Fractional seconds supported (e.g. 0.5).",
        0.0,
        [&](auto v) { c.duration_s = v; });

    s.add<std::string>(
        "rendezvous-server",
        "STUN rendezvous server as host:port (e.g. reflex.statusbar.com:3478). "
        "When set, owlm performs a STUN REGISTER handshake to discover the peer's "
        "reflexive address before starting; --peer and --peer-port are ignored.",
        std::string{},
        [&](auto v) { c.rendezvous_server = v; });
    s.add<std::string>(
        "rendezvous-key",
        "64-hex-char (32-byte) AES-128-SIV pre-shared key used to authenticate "
        "STUN messages. Must match the server's --key.",
        std::string{},
        [&](auto v) { c.rendezvous_key_hex = v; });
    s.add<std::string>(
        "rendezvous-session-id",
        "32-hex-char (16-byte) session id. Both peers must use the same value.",
        std::string{},
        [&](auto v) { c.rendezvous_session_id_hex = v; });
    s.add_choice(
        "rendezvous-role", "Role hint (decorative; does not affect pairing)", {"initiator", "responder"}, "initiator", [&](auto v) {
            c.rendezvous_role = std::string{v};
        });
    s.add<int64_t>("rendezvous-timeout-ms", "How long to wait for the peer before giving up (ms)", 60'000, [&](auto v) {
        c.rendezvous_timeout_ms = v;
    });
    s.add_flag("rendezvous-verbose", "Trace each STUN send/recv", [&](auto v) { c.rendezvous_verbose = v; });
    return s;
}

[[nodiscard]] inline auto build_time_source_config(Config const& cli) -> ::statusbar::udptun::TimeSourceConfig
{
    ::statusbar::udptun::TimeSourceConfig cfg{};
    cfg.kind = cli.time_source;
    cfg.mac_interface = cli.owlm_interface;

    // gptp-slave inputs: populate even when not in gptp-slave mode so
    // setting just --time-source=ptp4l on a config that already
    // specified --gptp-interface doesn't surprise on later toggle.
    cfg.identity.fallback_interface = cli.owlm_interface;
    cfg.identity.gptp_session_config.interface = cli.gptp_interface;
    cfg.identity.gptp_session_config.profile = cli.profile;
    cfg.identity.gptp_session_config.software_timestamping = cli.software_timestamping;
    cfg.identity.gptp_session_config.manual_peer_delay_ns = cli.manual_peer_delay_ns;
    cfg.identity.gptp_session_config.phase_jump_threshold_ns = cli.phase_jump_threshold_ns;
    cfg.identity.gptp_session_config.servo_kp = cli.servo_kp;
    cfg.identity.gptp_session_config.servo_ki = cli.servo_ki;
    cfg.identity.gptp_session_config.bridge_clock = cli.bridge_clock;
    cfg.identity.gptp_session_config.verbose = cli.gptp_verbose;
    // owlm consumes master-time only via the bridge; nothing else on
    // the system needs the PHC disciplined, so keep PHC raw and let
    // SoftClock track the offset internally. Avoids the PHC-step
    // discontinuity that wrecked the realtime::Timer's scheduled_time
    // anchor at first lock.
    cfg.identity.gptp_session_config.phc_passthrough = true;

    // ptp4l inputs: pass through verbatim. setup_ptp_app_with_fallback
    // handles bridge bring-up and waiting for healthy.
    cfg.ptp4l = cli.ptp4l;
    return cfg;
}

[[nodiscard]] inline auto build_session_config(Config const& cli) -> ::statusbar::udptun::SessionConfig
{
    ::statusbar::udptun::SessionConfig cfg{};
    cfg.interface = cli.owlm_interface;
    cfg.peer_host = cli.peer_host;
    cfg.peer_port = cli.peer_port;
    cfg.local_port = cli.local_port;
    cfg.ipv6 = cli.ipv6;
    cfg.dscp = cli.dscp;
    cfg.mcast_ttl = cli.mcast_ttl;
    cfg.tx_interval_us = cli.tx_interval_us;
    cfg.report_interval_us = cli.report_interval_us;
    cfg.worst_case_latency_ns = cli.worst_case_latency_ms * 1'000'000;
    cfg.peer_tai_offset_ns = cli.peer_tai_offset_ns;
    cfg.wire_clock_gps_tai = cli.wire_clock_gps_tai;
    cfg.tai_minus_utc_ns = cli.tai_minus_utc_ns;
    cfg.bridge_stable_samples = cli.bridge_stable_samples;
    cfg.bridge_stable_rate_ppm = cli.bridge_stable_rate_ppm;
    cfg.bridge_stable_offset_ns = cli.bridge_stable_offset_ns;
    cfg.payload_bytes = cli.payload_bytes;
    cfg.max_sources = cli.max_sources;
    cfg.latency_hist = {.low_ns = cli.hist_low_ns, .high_ns = cli.hist_high_ns, .bin_width_ns = cli.hist_bucket_ns};
    cfg.csv_output_path = cli.csv_output_path;
    cfg.bin_output_path = cli.bin_output_path;
    // Pre-allocate the colbin for the whole run (the writer never grows mid-
    // stream — a growing mmap stalls the RT data plane). Explicit override wins;
    // else size precisely from the hard duration; else the SessionConfig default
    // (15 min @ 96 kHz) stands. Worst-case row rate = 96000/44 pkt/s × 2
    // (redundant), 48 B/row, ×1.25 headroom.
    if (!cfg.bin_output_path.empty()) {
        if (cli.colbin_max_mb > 0) {
            cfg.bin_capacity_bytes = cli.colbin_max_mb * 1024ULL * 1024ULL;
        } else if (cli.duration_s > 0.0) {
            double const rows_per_s = (96000.0 / 44.0) * 2.0;
            auto const bytes = static_cast<uint64_t>(cli.duration_s * rows_per_s * 48.0 * 1.25);
            cfg.bin_capacity_bytes = std::max(bytes, udptun::default_colbin_capacity_bytes);
        }
    }
    cfg.wan_timer = cli.wan_timer;
    if (cli.enable_tripwire) {
        cfg.trace = cli.trace;
    }
    return cfg;
}

}  // namespace statusbar::owlm::tool

#endif  // __linux__
