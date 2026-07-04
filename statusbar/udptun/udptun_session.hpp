#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Templated TX/RX session loop driven by a Codec.
///
/// The data structures (TxContext, RxContext, LoopState) are non-template
/// — they bundle per-run state (sockets, timerfds, trackers, identities)
/// independent of which codec is in use. The algorithms (send_packet,
/// drain_rx, run_main_loop, finalize_run) are templated on Codec; the
/// codec interface is documented in udptun_codec_concept.hpp.
///
/// Header-only: each consumer instantiates the templates against their
/// own Codec type. This keeps codec-method calls fully inlinable on the
/// hot RX path.

#include "statusbar/colbin/colbin_writer.hpp"
#include "statusbar/csv/csv.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_published.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/itc/itc_telemetry_counter.hpp"
#include "statusbar/net/net.hpp"
#include "statusbar/ptpclient/ptpclient_tai_translator.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/catch_or_status.hpp"
#include "statusbar/status/throw_or_abort.hpp"
#include "statusbar/udptun/udptun_codec_concept.hpp"
#include "statusbar/udptun/udptun_csv_record.hpp"
#include "statusbar/udptun/udptun_deadline_timer.hpp"
#include "statusbar/udptun/udptun_identity.hpp"
#include "statusbar/udptun/udptun_per_source_tracker.hpp"
#include "statusbar/udptun/udptun_record_sink.hpp"
#include "statusbar/udptun/udptun_redundant_rx.hpp"
#include "statusbar/udptun/udptun_redundant_tx.hpp"
#include "statusbar/udptun/udptun_report.hpp"
#include "statusbar/udptun/udptun_session_helpers.hpp"
#include "statusbar/udptun/udptun_stats.hpp"
#include "statusbar/udptun/udptun_time_source.hpp"
#include "statusbar/udptun/udptun_tx_history.hpp"
#include "statusbar/udptun/udptun_wire_clock.hpp"

#if defined(__linux__)
#    include "statusbar/gptp/gptp_slave_session.hpp"
#    include "statusbar/realtime/realtime.hpp"
#    include "statusbar/udptun/udptun_gptp_clock_source.hpp"
#endif

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/types.h>

namespace statusbar::udptun {

/// Per-run TX state bundled together so send_packet doesn't take a
/// dozen positional parameters. References its dependents — built once
/// at the top of run_normal-style code and passed by const&.
///
/// `tx_buf` is a pre-allocated, pre-zeroed working buffer sized for
/// `header_size + payload_bytes`. send_packet writes only the codec
/// header into it on each tick; the trailing payload bytes stay at
/// zero across the run. Owning the buffer as a fixed-size span (not a
/// vector) keeps the hot TX path allocation-free.
///
/// `tx_failures` is incremented when sendto() returns -1 with
/// MSG_DONTWAIT (e.g. ENOBUFS from a saturated TX queue). Counts both
/// primary and redundant emissions so the reporter can show how many
/// scheduled sends actually made it onto the wire.
struct TxContext
{
    int udp_fd;
    net::SocketAddress const& peer;
    TxIdentityPair const& tx;
    uint32_t tx_interval_us;
    /// Sender-applied offset between acquisition (now_wall) and the
    /// presentation time stamped on the wire: PT = now_wall + WCL.
    /// 0 disables the offset (PT == acquisition).
    int64_t worst_case_latency_ns;
    WireClock const& clock;
    std::span<uint8_t> tx_buf;
    TxHistory& tx_history;
    RedundantTxBuffer& redundant_buffer;
    /// Counter updated only on the TX path; the reporter thread takes
    /// wait-free snapshots via TelemetryCounter::load() — no locking
    /// on the hot send path.
    statusbar::itc::TelemetryCounter<uint64_t>& tx_failures;
    /// Sender's most recently stamped PT, updated by send_packet. Used
    /// by Session<C> as the upper bound for self_redundancy_.flush_all
    /// at end-of-run.
    statusbar::itc::Published<int64_t>& last_tx_pt_ns;
};

/// Per-run RX state for drain_rx, drain_inflight_after_stop and
/// handle_report.
struct RxContext
{
    int udp_fd;
    ieee::Eui64 my_pair_id;
    /// Expected peer address for the source-address gate (Layer 1). When
    /// `source_gate_enabled` is true, drain_rx drops any datagram whose
    /// source host does not match `peer` (host only — NAT may rewrite the
    /// port). Enabled only for a valid *unicast* peer; disabled for
    /// multicast (many legitimate senders, kernel enforces group
    /// membership) and for RX-only nodes with no configured peer.
    net::SocketAddress peer;
    bool source_gate_enabled;
    /// Sender-side WCL — assumed identical to the receiver's local
    /// config because both endpoints in a deployment pick the same
    /// value. Used to recover real one-way transit and RTT from the
    /// wire-relative "lateness vs PT":
    ///   transit = (rx_wall - PT) + WCL
    ///   rtt     = (rx_wall_loopback - PT) + WCL
    /// Without this offset the recorded latencies would all be skewed
    /// by -WCL because the on-wire stamp is PT, not acquisition.
    int64_t worst_case_latency_ns;
    /// Constant offset (signed, ns) added to the cross-clock latency
    /// calculation only — compensates for a known per-deployment bias
    /// between the peer's GM and ours (e.g. two GPS receivers each
    /// disciplined to ~1 µs of TAI but with a known constant residual
    /// between them). 0 = no compensation. Not applied to
    /// self-redundancy / RTT, which use only the local clock.
    int64_t peer_tai_offset_ns;
    WireClock const& clock;
    PerSourceTracker& tracker;
    LatencyStats& rtt_stats;
    RedundantRxTracker& self_redundancy;
    /// Optional per-packet CSV sink. When `csv_sink.ring == nullptr`
    /// (the default), the append is a no-op and the RT path takes no
    /// extra work beyond the null check. The session owns the backing
    /// ring and atomic counter; RxContext just borrows pointers.
    CsvSink csv_sink;
    /// Latest PT observed from any direction, updated by
    /// process_one_rx_datagram. Drives the cutoff for
    /// self_redundancy_.scan. Atomic so the reporter thread can read
    /// it lockless from outside the RX-driving thread.
    statusbar::itc::Published<int64_t>& last_rx_pt_ns;
};

/// Build and send the redundant copy: replays the primary's `sequence`
/// and `presentation_time_ns` from `replay` but stamps with the
/// redundant identity. Reuses tx_buf in place; the primary already
/// filled bytes [0..header) and zero-padded any payload, and the
/// codec's encode only touches the header.
template <Codec C>
void send_redundant_replay(C const& codec, TxContext const& ctx, RedundantTxBuffer::Entry const& replay) noexcept
{
    codec.encode(ctx.tx_buf, ctx.tx.redundant_id, replay.sequence, replay.tx_gptp_ns, ctx.tx_interval_us);
    ssize_t const sent =
        ::sendto(ctx.udp_fd, ctx.tx_buf.data(), ctx.tx_buf.size(), MSG_DONTWAIT, ctx.peer.sockaddr(), ctx.peer.length());
    if (sent < 0) {
        ctx.tx_failures.add();
    }
}

/// Drain the TX timerfd, send a primary packet, record into both
/// tx_history (for the report path) and redundant_buffer (for the
/// redundant TX path), and conditionally send the redundant replay.
/// Skipped if `synced` is false (gPTP not yet locked).
///
/// On-wire timestamp is the **presentation time**:
/// `PT = wire_ns(now_mraw) + worst_case_latency_ns`. The receiver uses
/// PT as the slot key in its redundancy / reordering tracker;
/// reordering within RTT is implicit. With worst_case_latency_ns == 0,
/// PT collapses to the acquisition wall time and the receiver's
/// "lateness" metric (rx_wall - PT) equals real one-way transit.
/// Shared TX tail for both send paths: publish the presentation time, encode +
/// sendto the primary, record it for redundancy, replay the temporally-shifted
/// redundant copy, and advance the sequence. @p now_mraw is the CLOCK_MONOTONIC_RAW
/// timestamp used for the redundancy ring + tx history (the two entry points differ
/// only in how they derive @p presentation_time_ns).
template <Codec C>
void emit_packet(
    C const& codec, TxContext const& ctx, std::atomic<uint32_t>& sequence, int64_t presentation_time_ns, int64_t now_mraw) noexcept
{
    ctx.last_tx_pt_ns.publish(presentation_time_ns);

    uint32_t const seq = sequence.load(std::memory_order_relaxed);
    // The buffer is pre-zeroed at session construction; the codec touches only the
    // header, so payload bytes stay at zero across the run. No per-tick re-fill.
    codec.encode(ctx.tx_buf, ctx.tx.primary_id, seq, presentation_time_ns, ctx.tx_interval_us);
    ssize_t const sent =
        ::sendto(ctx.udp_fd, ctx.tx_buf.data(), ctx.tx_buf.size(), MSG_DONTWAIT, ctx.peer.sockaddr(), ctx.peer.length());
    if (sent < 0) {
        ctx.tx_failures.add();
    }
    ctx.redundant_buffer.record(now_mraw, seq, presentation_time_ns);

    // Send the redundant copy of the primary that was emitted `temporal_shift` ago.
    // Skipped silently during the first shift_ms after startup when the buffer hasn't
    // yet aged a packet that far.
    if (ctx.tx.redundant_enabled) {
        auto const target_ns = now_mraw - ctx.tx.temporal_shift_ns;
        if (auto replay = ctx.redundant_buffer.entry_at_or_before(target_ns); replay.has_value()) {
            send_redundant_replay(codec, ctx, *replay);
        }
    }

    uint32_t const next_seq = seq + 1;
    sequence.store(next_seq, std::memory_order_relaxed);
    ctx.tx_history.record(now_mraw, next_seq);
}

template <Codec C>
void send_packet(C const& codec, TxContext const& ctx, std::atomic<uint32_t>& sequence, bool synced) noexcept
{
    if (!synced) {
        return;  // skip transmit until the wire-time source is locked
    }
    int64_t const now_mraw = monotonic_raw_ns();
    int64_t const presentation_time_ns = ctx.clock.wire_ns(now_mraw) + ctx.worst_case_latency_ns;
    emit_packet(codec, ctx, sequence, presentation_time_ns, now_mraw);
}

/// RT-mode TX: the gPTP-domain target time comes from the realtime timer's scheduled
/// deadline (event.scheduled_time_ns) rather than being computed from monotonic_raw +
/// bridge slope -- slightly cheaper (no slope multiply) and aligned to the scheduled
/// tick. now_mraw is sampled before the sendto (vs after, previously) so it can be
/// shared with emit_packet; the ~µs difference only shifts the redundancy/history
/// timestamp, which is millisecond-scale.
template <Codec C>
void send_packet_at_gptp(C const& codec, TxContext const& ctx, std::atomic<uint32_t>& sequence, int64_t scheduled_gptp_ns) noexcept
{
    int64_t const presentation_time_ns = scheduled_gptp_ns + ctx.worst_case_latency_ns;
    int64_t const now_mraw = monotonic_raw_ns();
    emit_packet(codec, ctx, sequence, presentation_time_ns, now_mraw);
}

/// Self-loopback: this packet's pair-id matches our local pair-id, so
/// it's our own primary or redundant (or legacy single-stream) coming
/// back via the reflector. Primary always feeds rtt_stats and the
/// coverage tracker; a redundant feeds rtt_stats only when it actually
/// rescued a primary loss — that sample's latency is naturally inflated
/// by the temporal shift, which correctly reflects user-visible
/// delivery time on the fallback path.
///
/// The on-wire stamp is the **presentation time**, so the literal
/// `rx_gptp - tx_gptp` is *lateness vs. PT*. To recover real RTT
/// (transit-out + transit-back), the sender adds its own WCL back —
/// the receiver-half of the loop is also our own process, so WCL is
/// known here.
template <Codec C>
void route_self_pair_packet(
    C const& codec,
    RxContext const& ctx,
    typename C::DecodedPacket const& p,
    int64_t /*rx_mraw*/,
    int64_t rx_gptp,
    PacketRole role) noexcept
{
    int64_t const presentation_time_ns = codec.tx_gptp_ns(p);
    int64_t const rtt_ns = (rx_gptp - presentation_time_ns) + ctx.worst_case_latency_ns;
    if (role == PacketRole::SelfRedundant) {
        if (ctx.self_redundancy.on_redundant(presentation_time_ns)) {
            ctx.rtt_stats.record(rtt_ns);
        }
        return;
    }
    if (role == PacketRole::SelfPrimary) {
        ctx.self_redundancy.on_primary(presentation_time_ns);
    }
    // SelfPrimary and SelfLegacy both feed RTT.
    ctx.rtt_stats.record(rtt_ns);
}

/// Decode one received datagram and dispatch it. Pre-sync packets are
/// discarded (the wire-time source is unreliable before the first sync).
/// Remote redundant duplicates are silently dropped to avoid
/// double-counting in the per-source list. `rx_mraw` is the
/// CLOCK_MONOTONIC_RAW timestamp sampled at the start of the current
/// drain loop; all packets in one drain share the same value. Drain
/// quantisation is bounded by one wan_timer tick (default 1 ms).
template <Codec C>
void process_one_rx_datagram(
    C const& codec, RxContext const& ctx, std::span<uint8_t const> bytes, bool synced, int64_t rx_mraw) noexcept
{
    if (!synced) {
        return;
    }
    auto pkt_or = codec.decode(bytes);
    if (!pkt_or.has_value()) {
        ctx.tracker.increment_dropped_invalid();
        return;
    }
    auto const& pkt = *pkt_or;
    int64_t const rx_gptp = ctx.clock.wire_ns(rx_mraw);
    int64_t const presentation_time_ns = codec.tx_gptp_ns(pkt);
    // Layer 2: the presentation time is attacker-controlled wire data. It
    // keys the redundancy slot map (negative -> out-of-bounds index) and
    // drives self_redundancy_.scan via last_rx_pt (absurdly-future ->
    // near-infinite loop). Reject anything not plausibly a real GPS-TAI PT
    // before it is used anywhere. (See presentation_time_plausible.)
    if (!presentation_time_plausible(presentation_time_ns, rx_gptp)) {
        ctx.tracker.increment_dropped_invalid();
        return;
    }
    if (int64_t const prev = ctx.last_rx_pt_ns.load(); presentation_time_ns > prev) {
        ctx.last_rx_pt_ns.publish(presentation_time_ns);
    }
    PacketRole const role = codec.classify(pkt, ctx.my_pair_id);

    int64_t const latency_for_csv = [&]() -> int64_t {
        switch (role) {
            case PacketRole::SelfPrimary:
            case PacketRole::SelfRedundant:
            case PacketRole::SelfLegacy:
                return (rx_gptp - presentation_time_ns) + ctx.worst_case_latency_ns;
            // Remote roles — including RemoteRedundant, which is dropped from
            // per-source observe but still recorded in the CSV stream.
            case PacketRole::RemotePrimary:
            case PacketRole::RemoteLegacy:
            case PacketRole::RemoteRedundant:
                return (rx_gptp - presentation_time_ns) + ctx.worst_case_latency_ns + ctx.peer_tai_offset_ns;
        }
        return 0;
    }();

    ctx.csv_sink.append(UdpTunCsvRecord{
        .rx_gptp_ns = rx_gptp,
        .presentation_time_ns = presentation_time_ns,
        .latency_ns = latency_for_csv,
        .sender_id = codec.sender_id(pkt),
        .sequence = codec.sequence(pkt),
        .interval_us = codec.announced_interval_us(pkt),
        .role = static_cast<uint8_t>(role),
        ._pad = {},
    });

    switch (role) {
        case PacketRole::SelfPrimary:
        case PacketRole::SelfRedundant:
        case PacketRole::SelfLegacy:
            route_self_pair_packet(codec, ctx, pkt, rx_mraw, rx_gptp, role);
            return;
        case PacketRole::RemoteRedundant:
            return;  // drop to avoid double-counting the same logical stream
        case PacketRole::RemotePrimary:
        case PacketRole::RemoteLegacy:
            // latency_for_csv already holds this exact value for the remote roles.
            ctx.tracker.observe(
                codec.sender_id(pkt), codec.sequence(pkt), latency_for_csv, rx_gptp, codec.announced_interval_us(pkt));
            return;
    }
}

/// Drain the UDP socket nonblocking-recvmsg loop. Each successfully
/// received datagram goes through process_one_rx_datagram; recvmsg
/// returning <= 0 (EAGAIN/EINTR or zero-length) ends the loop and
/// returns control to the caller's poll().
///
/// The buffer is sized for jumbo Ethernet (MTU ~9000) so OWLM and
/// AAF-over-Annex-J consumers don't truncate on networks that allow
/// large frames. MSG_TRUNC asks the kernel to report the full datagram
/// length so we can detect truncation explicitly when a peer sends
/// something even larger than that (and skip the partial decode).
///
/// rx_mraw is sampled once per drain from CLOCK_MONOTONIC_RAW and used
/// for every packet in the drain. We DO NOT use SO_TIMESTAMPNS_NEW /
/// SO_TIMESTAMPNS: those carry the CLOCK_REALTIME stamp, and a
/// clock_settime step (phc2sys / chronyd / systemd-timesyncd) between
/// arrival and drain corrupts the translation by exactly the step
/// size, producing single-packet latency anomalies. CLOCK_MONOTONIC_RAW
/// is immune to wall-clock discontinuities. The cost is RX-time
/// quantisation to the drain cadence — bounded by one wan_timer tick
/// (default 1 ms), well below any inter-host transit time of interest.
template <Codec C>
void drain_rx(C const& codec, RxContext const& ctx, bool synced) noexcept
{
    std::array<uint8_t, 9216> buf{};
    int64_t const rx_mraw = monotonic_raw_ns();

    while (true) {
        net::SocketAddress src{};
        src.reset_length();
        struct iovec iov{.iov_base = buf.data(), .iov_len = buf.size()};
        struct msghdr msg{
            .msg_name = src.sockaddr(),
            .msg_namelen = static_cast<socklen_t>(sizeof(struct sockaddr_storage)),
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_control = nullptr,
            .msg_controllen = 0,
            .msg_flags = 0,
        };
        ssize_t const n = ::recvmsg(ctx.udp_fd, &msg, MSG_DONTWAIT | MSG_TRUNC);
        if (n <= 0) {
            return;
        }
        // Layer 1 source-address gate: on a unicast tunnel the only
        // legitimate sender is the configured peer. Drop anything else
        // before it reaches the parser — this is the primary defense
        // against off-path injection on the public-internet socket.
        if (ctx.source_gate_enabled && !same_host(src, ctx.peer)) {
            ctx.tracker.increment_dropped_invalid();
            continue;
        }
        auto const datagram_bytes = static_cast<size_t>(n);
        if (datagram_bytes > buf.size()) {
            // Kernel reports a datagram larger than our buffer; the bytes
            // we can see are a prefix only, so we can't decode safely.
            ctx.tracker.increment_truncated();
            continue;
        }
        process_one_rx_datagram(codec, ctx, std::span<uint8_t const>(buf.data(), datagram_bytes), synced, rx_mraw);
    }
}

/// Post-Ctrl-C drain: keep accepting RX packets for one grace window
/// so packets still in flight at stop can arrive before the final
/// summary classifies them as missing. A second SIGINT (EINTR on poll)
/// breaks the drain early.
template <Codec C>
void drain_inflight_after_stop(C const& codec, RxContext const& ctx, bool synced, int64_t drain_ns) noexcept
{
    int64_t const drain_until = monotonic_raw_ns() + drain_ns;
    while (monotonic_raw_ns() < drain_until) {
        pollfd pfd{.fd = ctx.udp_fd, .events = POLLIN, .revents = 0};
        int const rv = ::poll(&pfd, 1, /*timeout_ms*/ 50);
        if (rv > 0 && (pfd.revents & POLLIN) != 0) {
            drain_rx(codec, ctx, synced);
        } else if (rv < 0 && errno == EINTR) {
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Session<C> — owning struct for a udptun run.
// ---------------------------------------------------------------------------

/// Static configuration for a udptun Session — transport, timing, payload,
/// identity-discovery inputs, and tracking caps. Codec-agnostic.
struct SessionConfig
{
    // Transport
    std::string interface;  ///< empty = wildcard bind, no SO_BINDTODEVICE
    std::string peer_host;  ///< empty when tx_interval_us == 0 (RX only)
    uint16_t peer_port{0};
    uint16_t local_port{0};
    bool ipv6{false};
    int dscp{-1};  ///< -1 disables DSCP
    int mcast_ttl{1};

    // Timing
    uint64_t tx_interval_us{100'000};  ///< 0 = RX only
    uint64_t report_interval_us{1'000'000};

    /// Sender-applied offset between acquisition (now_wall) and the
    /// presentation time stamped on the wire: PT = now_wall + WCL.
    /// 0 disables the offset (PT == acquisition; receiver's lateness
    /// metric equals real one-way transit).
    int64_t worst_case_latency_ns{0};

    /// Constant signed offset (ns) added to cross-clock one-way latency
    /// measurements. Compensates for a known per-deployment bias between
    /// the peer's grandmaster TAI and ours (typical: two GPS-disciplined
    /// GMs that each track TAI to ~1 µs but with a small residual offset
    /// between them). Sign convention: positive when the peer's clock
    /// leads ours, i.e. add this value to recover real transit time.
    /// 0 = no compensation; only applied to RemotePrimary / RemoteLegacy
    /// packets, never to RTT or self-redundancy.
    int64_t peer_tai_offset_ns{0};

    /// Translate the wire timeline to absolute GPS-TAI. The master clock
    /// (ptp4l-fed PHC / gPTP slave) keeps its ns-class short-term
    /// stability, but its epoch is whatever the local (free-running
    /// switch) GM advertises — NOT TAI. When enabled, a
    /// ptpclient::GpsTaiTranslator tracks (master − CLOCK_REALTIME) with
    /// a 3-state Kalman filter (CLOCK_REALTIME is chrony-disciplined to
    /// the site's GPS NTP source) and every on-wire timestamp — PT on TX,
    /// rx_gptp on RX — is mapped master→TAI. Two sites doing the same
    /// thing share one absolute timeline, so cross-site one-way latency
    /// is directly meaningful. In realtime mode the master IS
    /// CLOCK_REALTIME, so this degenerates to "+ tai_minus_utc_ns" (UTC
    /// wire → TAI wire). Neither ptp4l, the PHC, nor kernel TAI is
    /// touched.
    bool wire_clock_gps_tai{false};

    /// TAI − UTC in ns, applied manually (37 s, in force since
    /// 2017-01-01). Only used when wire_clock_gps_tai is set.
    int64_t tai_minus_utc_ns{37'000'000'000LL};

    // Payload size past the codec header.
    uint32_t payload_bytes{0};

    // Wire-time source: gPTP slave (default), ptp4l-fed PHC, or
    // CLOCK_REALTIME. The Session sets up the source in its
    // constructor — doing it inside Session (rather than handing in a
    // pre-emplaced optional) avoids moving non-movable resources like
    // gptp::SlaveSession.
    TimeSourceConfig time_source{};

    // Tracking caps + histogram config.
    size_t max_sources{32};
    statusbar::stats::AtomicHistogramConfig latency_hist{};

    /// Optional pre-opened, already-bound UDP fd. When valid, setup_udp
    /// adopts it (non-blocking is set, BINDTODEVICE / multicast are still
    /// applied, but no fresh socket is created or bound). Used by tools
    /// that performed a STUN rendezvous and want to keep the same source
    /// port + NAT mapping. Ownership transfers via std::move into
    /// SessionConfig and from there into Session; on Session ctor failure
    /// the FileDescriptor's destructor closes the fd cleanly.
    net::FileDescriptor preopened_udp{};

    /// Optional tripwire / wake-latency diagnostics for the realtime
    /// timer thread. When engaged, run_realtime() constructs an owned
    /// TripwireMonitor from this config, wraps each tick in a
    /// ScopedTripwireObserver, and a tripwire fire propagates as a
    /// TripwireFiredException that unwinds the timer thread cleanly.
    /// std::nullopt = no tripwire monitor, no diagnostics.
    std::optional<realtime::TraceConfig> trace{};

    /// Bridge-stability gate. Bootstrap waits indefinitely (until
    /// stop) for the gPTP↔mraw bridge to produce N consecutive anchor
    /// refreshes whose rate_offset_ppt and offset both stay within
    /// the configured spread thresholds. Until this gate releases,
    /// the realtime timer doesn't start, so no OWLM TX happens with
    /// a stale or wandering bridge mapping.
    /// Defaults: 16 samples, 3.0 ppm rate spread, 100 µs offset spread.
    /// Field testing on RPi5 shows the bridge has unsmoothed
    /// per-sample rate noise with periodic bursts to ±15 ppm; clean
    /// stretches sit around 1–2 ppm spread. 3 ppm separates the two.
    int bridge_stable_samples{16};
    double bridge_stable_rate_ppm{3.0};
    int64_t bridge_stable_offset_ns{100'000};

    /// Realtime timer for the WAN transport loop (Linux + gPTP only).
    /// When `period_ns > 0` and gPTP is locked, Session::run() drives
    /// the poll loop from a `realtime::Timer` thread aligned to the
    /// gPTP clock domain. Each tick: drain RX, dispatch gPTP fds, tick
    /// gPTP, optionally TX. Decoupled from `tx_interval_us` — TX fires
    /// only every `tx_interval_us / (wan_timer.period_ns / 1000)`
    /// ticks. `period_ns == 0` falls back to the portable poll loop
    /// driven by DeadlineTimer + monotonic clock. The `name` field is
    /// overridden internally to "udptun_rt"; everything else (cpu,
    /// priority, busy_wait, histograms…) is honored.
    realtime::TimerConfig wan_timer{.period_ns = 1'000'000};

    /// Path for the per-packet CSV stream. Empty (the default) disables
    /// the feature; no file is opened and the RX hot path does only a
    /// null-pointer check per packet. When non-empty, Session opens the
    /// file in its constructor and runs a low-priority writer thread
    /// that drains an SPSC ring fed by the RX path — so the realtime
    /// timer thread never blocks on disk I/O. Stops + joins the writer
    /// inside flush_csv().
    std::string csv_output_path{};
    /// Path for a per-packet binary stream (.colbin). Empty (default)
    /// disables. Shares the SPSC ring with the CSV path; the same
    /// writer thread drains and writes to whichever sinks are enabled.
    /// The on-disk row is a memcpy of UdpTunCsvRecord (48 bytes/row),
    /// mmap-appended for negligible per-packet write cost.
    std::string bin_output_path{};
    /// Pre-allocated colbin capacity in bytes. The writer maps this in full at
    /// start and NEVER grows (no mid-run mremap that would stall the data
    /// plane); writing past it stops recording with capacity_exceeded. Default
    /// = `default_colbin_capacity_bytes` (15 min @ 96 kHz). Size for the run.
    uint64_t bin_capacity_bytes{default_colbin_capacity_bytes};
};

/// Single-threaded udptun session. Owns every per-run resource that used
/// to live on the stack of a tool's `run_normal` (UDP socket, periodic
/// timerfds, RX/TX trackers, redundancy buffers, gPTP slave). Provides
/// `run(stop)` to drive the poll loop, `drain_after_stop` for
/// post-shutdown RX drain, and report/finalize methods that surface the
/// accumulated stats.
///
/// Non-copyable, non-movable: members hold references / atomics / fds
/// that don't tolerate relocation. Heap-allocate if the owner needs
/// runtime polymorphism over codec types.
template <Codec C>
class Session
{
  public:
    /// Caller-supplied codec-specific identity builder. Session calls
    /// this once during construction with the locally discovered MAC;
    /// the callback returns the codec's TxIdentityPair (primary_id,
    /// redundant_id, redundant_enabled, temporal_shift_ns) and the
    /// canonical pair_id used to detect self-loopback. Codec-specific
    /// helpers like `owlm::make_owlm_eui64` and `owlm::eui64_pair_id`
    /// live in the calling tool, not here.
    using IdentityBuilder = statusbar::sg14::inplace_function<std::pair<TxIdentityPair, ieee::Eui64>(ieee::Eui48 const&), 64>;

    /// Construct a fully-initialised session. Performs (in order):
    ///   1. local-identity discovery — starts a gPTP slave session if
    ///      configured, or reads the MAC directly via SIOCGIFHWADDR.
    ///   2. codec-specific identity construction via `build_identity`.
    ///   3. UDP socket bind + multicast setup if `peer` is multicast.
    ///   4. periodic timerfd creation for TX (if tx_interval_us > 0)
    ///      and the report ticker.
    /// Throws std::system_error on any step's failure. The gPTP slave
    /// session is owned in place as a member; SlaveSession is
    /// non-movable, which is why it's set up inside the constructor
    /// rather than passed in.
    Session(SessionConfig cfg, C codec, IdentityBuilder build_identity)
        : cfg_{std::move(cfg)}
        , codec_{std::move(codec)}
        , tracker_{cfg_.latency_hist, cfg_.max_sources}
        , rtt_stats_{cfg_.latency_hist}
        , tx_buf_(codec_.header_size() + cfg_.payload_bytes, uint8_t{0})
        , self_redundancy_{slot_width_ns_for_tracker(cfg_.tx_interval_us)}
        , record_sink_{cfg_.csv_output_path, cfg_.bin_output_path, cfg_.bin_capacity_bytes}
    {
        auto mac = bring_up_local_identity();
        if (!mac) {
            statusbar::throw_or_abort(std::errc::io_error, "local identity setup failed");
        }
        local_mac_ = *mac;
        auto built = build_identity(local_mac_);
        tx_state_ = built.first;
        my_pair_id_ = built.second;
        master_clock_.translate = build_wire_translator();
        if (cfg_.wire_clock_gps_tai) {
            tai_translator_ = ptpclient::GpsTaiTranslator{ptpclient::GpsTaiTranslator::Config{
                .tai_minus_utc_ns = cfg_.tai_minus_utc_ns,
            }};
            // Compose master→TAI on top of the master translator. Captures
            // only `this` (fits inplace_function); GpsTaiTranslator::tai_ns
            // is identity until the first sample, and tai_sample_tick()
            // feeds a sample on the very first loop tick — before any TX.
            clock_.translate = [this](int64_t mraw_ns) -> int64_t {
                return tai_translator_.tai_ns(master_clock_.wire_ns(mraw_ns));
            };
        } else {
            clock_.translate = master_clock_.translate;
        }

        setup_udp();
        start_mraw_ = monotonic_raw_ns();
        if (cfg_.tx_interval_us > 0) {
            int64_t const interval_ns = static_cast<int64_t>(cfg_.tx_interval_us) * 1'000;
            tx_timer_.arm(start_mraw_ + interval_ns, interval_ns);
        }
        int64_t const report_interval_ns = static_cast<int64_t>(cfg_.report_interval_us) * 1'000;
        report_timer_.arm(start_mraw_ + report_interval_ns, report_interval_ns);
        // The per-packet recorder (CSV/colbin ring + writer thread) is the record_sink_
        // member: it opened its files and started its thread during member init, and
        // its dtor stops+joins on destruction or a throw here (RAII replaces the old
        // "start the thread last, join it by hand" dance).
    }

    Session(Session const&) = delete;
    auto operator=(Session const&) -> Session& = delete;
    Session(Session&&) = delete;
    auto operator=(Session&&) -> Session& = delete;

    /// Drive the session loop until `stop` is set. On Linux with a
    /// configured wan_timer (period_ns > 0) and a master-clock source
    /// (gptp-slave or ptp4l), dispatches to the realtime::Timer-driven
    /// path (run_realtime). Otherwise falls through to the portable
    /// DeadlineTimer + poll loop (run_polling).
    void run(statusbar::itc::StopToken& stop)
    {
#if defined(__linux__)
        if (cfg_.wan_timer.period_ns > 0 && (gptp_session_.has_value() || ptp4l_ctx_.has_value())) {
            run_realtime(stop);
            return;
        }
#endif
        run_polling(stop);
    }

    /// Portable polling loop: build pollfd array, poll() with the
    /// nearest DeadlineTimer's deadline as timeout, dispatch any ready
    /// fds, fire any due timers. Used on macOS and on Linux when
    /// wan_timer.period_ns == 0 or gPTP isn't available.
    void run_polling(statusbar::itc::StopToken& stop)
    {
        TxContext const tx_ctx = build_tx_context();
        RxContext const rx_ctx = build_rx_context();

        while (!stop.stop_requested()) {
            std::array<pollfd, 2> fds{};
            size_t const n_fds = build_session_pollfds(fds);

            int64_t const now_mraw_pre = monotonic_raw_ns();
            int64_t const earliest = std::min(tx_timer_.next_deadline_ns(), report_timer_.next_deadline_ns());
            int const timeout_ms = deadline_to_poll_timeout_ms(now_mraw_pre, earliest, /*max_ms=*/100);

            int const rv = ::poll(fds.data(), n_fds, timeout_ms);
            if (rv < 0 && errno == EINTR) {
                continue;
            }

            auto const now = std::chrono::steady_clock::now();
            bool const synced = gptp_synced();
            for (size_t i = 0; i < n_fds; ++i) {
                if ((fds[i].revents & POLLIN) == 0) {
                    continue;
                }
                if (gptp_dispatch_if_owned(fds[i].fd, now)) {
                    continue;
                }
                if (fds[i].fd == udp_.get()) {
                    drain_rx(codec_, rx_ctx, synced);
                }
            }
            gptp_periodic_tick(now);

            int64_t const now_mraw_post = monotonic_raw_ns();
            tai_sample_tick(now_mraw_post);
            if (tx_timer_.consume_due(now_mraw_post) > 0) {
                send_packet(codec_, tx_ctx, sequence_, synced);
            }
            if (report_timer_.consume_due(now_mraw_post) > 0) {
                handle_report_tick();
            }
        }
    }

#if defined(__linux__)
    /// Realtime-timer-driven loop. The `realtime::Timer` runs in its
    /// own SCHED_FIFO thread, fires the callback at master-clock-aligned
    /// deadlines, and never does IO. Each tick:
    ///   1. Non-blocking poll(timeout=0) over gPTP + udp fds; dispatch.
    ///   2. gptp_session_->tick(now) (no-op in ptp4l mode).
    ///   3. If the scheduled master time has reached `next_tx_gptp_ns_`,
    ///      send a primary + redundant pair and advance the cursor by
    ///      cfg.tx_interval_us. This decouples the TX cadence from the
    ///      tick cadence: --wan_timer.period_ns=1000000 plus
    ///      --tx-interval-us=100000 means "drain RX every 1ms,
    ///      transmit every 100ms".
    ///
    /// The clock source under the realtime::Timer differs by mode:
    ///   - gptp-slave: ClockAdapter<GptpClock<0>, GptpSlaveClockSource>
    ///                 backed by the in-process gPTP slave's bridge.
    ///   - ptp4l:      ClockAdapter<GptpClock<0>, PtpTimeBridge>
    ///                 backed by the regression bridge over /dev/ptpN.
    ///
    /// A separate reporter thread polls stats every report_interval_us
    /// and prints — so the RT callback itself stays IO-free.
    void run_realtime(statusbar::itc::StopToken& stop)
    {
        using namespace statusbar::realtime;

        // gptp-slave needs a bridge-stable warmup before the timer
        // starts; ptp4l mode skips it because setup_ptp_app_with_fallback
        // already blocked until the regression bridge is healthy.
        if (gptp_session_.has_value()) {
            if (!warmup_gptp_slave_bridge(stop)) {
                return;  // stop requested mid-warmup
            }
        }

        int64_t const tx_interval_ns = static_cast<int64_t>(cfg_.tx_interval_us) * 1'000;

        // Anchor the first TX deadline at the next tx-interval boundary
        // in master time. Subsequent TXs advance by tx_interval_ns each.
        // tx_interval_ns == 0 disables TX entirely (RX-only run).
        int64_t const start_master_ns = consume_master_ns();
        next_tx_gptp_ns_ = (tx_interval_ns > 0) ? (((start_master_ns / tx_interval_ns) + 1) * tx_interval_ns) : INT64_MAX;

        if (gptp_session_.has_value()) {
            GptpSlaveClockSource clock_source{&*gptp_session_};
            run_realtime_with_adapter(stop, tx_interval_ns, ClockAdapter<GptpClock<0>, GptpSlaveClockSource>{clock_source});
        } else if (ptp4l_ctx_.has_value()) {
            run_realtime_with_adapter(
                stop, tx_interval_ns, ClockAdapter<GptpClock<0>, ptpclient::PtpTimeBridge>{*ptp4l_ctx_->bridge});
        }
    }

    /// gptp-slave-specific bootstrap: drive gPTP to sync from the main
    /// thread before spawning the realtime timer. realtime::Timer's
    /// wait_until_ready blocks the timer thread until the adapter is
    /// healthy — but the adapter only becomes healthy when gPTP
    /// processes incoming messages, and those are processed via
    /// gptp_session_->dispatch() which the RT callback owns. Without
    /// this bootstrap, the timer would wait for sync forever while
    /// gPTP messages piled up undrained.
    ///
    /// Exit condition: the bridge has produced N consecutive anchor
    /// refreshes whose cached rate_offset_ppt and offset stay inside
    /// the configured spreads. The gptp::GptpTimeBridge stores the
    /// most recent sample's rate without any smoothing, so during the
    /// servo's initial pull-in (or after a clock_settime fallback)
    /// the cached rate can swing tens of ppm sample-to-sample. Calls
    /// to to_gptp_rated()/from_gptp_rated() during that window
    /// translate timestamps wrong by milliseconds-to-seconds, which
    /// corrupts both PT (sender side) and rx_gptp (receiver side)
    /// for OWLM measurement. We block here until the rolling window
    /// of `bridge_stable_samples` recent samples has spread inside
    /// `bridge_stable_rate_ppm` and `bridge_stable_offset_ns`. Block
    /// forever until satisfied — Ctrl-C still exits via `stop`.
    ///
    /// @return true on warm-up success, false if `stop` fired first.
    [[nodiscard]] auto warmup_gptp_slave_bridge(statusbar::itc::StopToken& stop) -> bool
    {
        constexpr size_t max_bridge_stable_samples = 128;
        size_t const window_size = std::min(static_cast<size_t>(cfg_.bridge_stable_samples), max_bridge_stable_samples);
        statusbar::sg14::inplace_vector<int64_t, max_bridge_stable_samples> rate_ring;
        statusbar::sg14::inplace_vector<int64_t, max_bridge_stable_samples> offset_ring;
        rate_ring.resize(window_size, 0);
        offset_ring.resize(window_size, 0);
        size_t ring_idx = 0;
        size_t ring_filled = 0;
        int64_t prev_anchor_gptp_ns = 0;
        int64_t const rate_threshold_ppt = static_cast<int64_t>(cfg_.bridge_stable_rate_ppm * 1'000'000.0);
        int64_t const offset_threshold_ns = cfg_.bridge_stable_offset_ns;
        RxContext const rx_ctx = build_rx_context();
        auto bridge_warmed_up = [&]() -> bool {
            if (!gptp_session_->synced()) {
                return false;
            }
            int64_t const cur_anchor = gptp_session_->bridge().last_anchor_gptp_ns();
            if (cur_anchor == 0 || cur_anchor == prev_anchor_gptp_ns) {
                return false;  // no new anchor since last sample
            }
            prev_anchor_gptp_ns = cur_anchor;
            rate_ring[ring_idx] = gptp_session_->bridge().rate_offset_ppt();
            offset_ring[ring_idx] = gptp_session_->bridge().offset();
            ring_idx = (ring_idx + 1) % window_size;
            if (ring_filled < window_size) {
                ++ring_filled;
                return false;  // window not yet full
            }
            auto const [rmin, rmax] = std::minmax_element(rate_ring.begin(), rate_ring.end());
            auto const [omin, omax] = std::minmax_element(offset_ring.begin(), offset_ring.end());
            int64_t const rate_spread = *rmax - *rmin;
            int64_t const offset_spread = *omax - *omin;
            return (rate_spread <= rate_threshold_ppt) && (offset_spread <= offset_threshold_ns);
        };
        while (!stop.stop_requested() && !bridge_warmed_up()) {
            std::array<pollfd, 2> fds{};
            size_t const n_fds = build_session_pollfds(fds);
            int const rv = ::poll(fds.data(), n_fds, /*timeout_ms*/ 50);
            if (rv < 0 && errno == EINTR) {
                continue;
            }
            auto const now = std::chrono::steady_clock::now();
            for (size_t i = 0; i < n_fds; ++i) {
                if ((fds[i].revents & POLLIN) == 0) {
                    continue;
                }
                if (gptp_dispatch_if_owned(fds[i].fd, now)) {
                    continue;
                }
                if (fds[i].fd == udp_.get()) {
                    // Drain RX even pre-sync to avoid kernel buffer
                    // backup; process_one_rx_datagram discards
                    // unsynced packets internally.
                    drain_rx(codec_, rx_ctx, /*synced=*/false);
                }
            }
            gptp_periodic_tick(now);
        }
        return !stop.stop_requested();
    }

    /// One realtime-timer tick: tripwire wake-gate, drain gPTP + udp, periodic gPTP,
    /// GPS-TAI training sample, and (when synced and past the TX cursor) send the next
    /// primary packet, then the tripwire duration-gate. Extracted verbatim from the
    /// timer callback so run_realtime_with_adapter reads as setup + run + teardown; the
    /// former lambda captures are now explicit parameters. @p monitor_ptr is null when
    /// no trace config is active.
    template <typename ClockT>
    void realtime_tick(
        statusbar::realtime::TimerEvent<ClockT> const& event,
        TxContext const& tx_ctx,
        RxContext const& rx_ctx,
        int64_t tx_interval_ns,
        statusbar::realtime::TripwireMonitor* monitor_ptr,
        statusbar::itc::StopToken& stop)
    {
        // Suppress the tripwire if either:
        //   - stop flag is set (shutdown drain may be slow / blocked),
        //   - master clock isn't synced (mid-run sync loss invalidates
        //     the bridge mapping, so wake error becomes meaningless
        //     until sync re-acquires).
        // Wake check at entry, duration check at exit; either gate
        // closing mid-callback short-circuits the duration trip.
        int64_t tripwire_start_ns = 0;
        if (monitor_ptr != nullptr && !stop.stop_requested() && gptp_synced()) {
            tripwire_start_ns = observe_wake_and_handle_tripwire(*monitor_ptr, event);
            if (tripwire_start_ns == 0) {
                return;  // wake threshold exceeded — abort this tick
            }
        }

        // Drain gPTP + udp non-blocking. timeout_ms=0 means "tell
        // me what's ready right now"; we already slept inside the
        // timer's wait_until_deadline. In ptp4l mode the gPTP
        // helpers are no-ops (gptp_session_ is empty).
        std::array<pollfd, 2> fds{};
        size_t const n_fds = build_session_pollfds(fds);
        ::poll(fds.data(), n_fds, /*timeout_ms*/ 0);

        auto const now_steady = std::chrono::steady_clock::now();
        bool const synced = gptp_synced();
        for (size_t i = 0; i < n_fds; ++i) {
            if ((fds[i].revents & POLLIN) == 0) {
                continue;
            }
            if (gptp_dispatch_if_owned(fds[i].fd, now_steady)) {
                continue;
            }
            if (fds[i].fd == udp_.get()) {
                drain_rx(codec_, rx_ctx, synced);
            }
        }
        gptp_periodic_tick(now_steady);
        tai_sample_tick(monotonic_raw_ns());

        if (tx_interval_ns > 0 && synced) {
            // Use the bridge's live master-clock reading, not
            // event.scheduled_time. realtime::Timer's scheduled_time
            // is anchored to adapter.now_ns() at run_loop start and
            // increments by period_ns each tick, but that anchor
            // doesn't track mid-run clock steps (the gPTP slave's
            // clock_settime fallback for large initial offsets
            // bypasses the bridge offset; ptp4l-fed PHC step is
            // similar). The live read is always in the current
            // time domain — at the cost of a clock read per send,
            // we get correct PT values even after a discontinuity.
            // The TX cadence cursor stays in the master (bridge-now)
            // domain. The stamped PT, however, MUST go through the same
            // WireClock+mraw path that RX (ctx.clock.wire_ns(rx_mraw)) and
            // the gps-tai training sampler (master_clock_.wire_ns(mraw))
            // use. Stamping it from consume_master_ns()/bridge->now_ns()
            // instead diverges from master_clock_.wire_ns(mraw) by a large,
            // PHC-epoch-dependent constant that does NOT cancel in fwd+rev
            // (it produced an impossible negative cross-site RTT on a-e —
            // see ptpclient_tai_translator_test tx_must_share_rx_master_source).
            // In master mode clock_.wire_ns == master_clock_.wire_ns, so
            // this also makes RT-mode TX match the portable polling path
            // (send_packet, which already stamps via ctx.clock.wire_ns).
            int64_t const sched_gptp_ns = consume_master_ns();
            if (sched_gptp_ns >= next_tx_gptp_ns_) {
                send_packet_at_gptp(codec_, tx_ctx, sequence_, clock_.wire_ns(monotonic_raw_ns()));
                next_tx_gptp_ns_ += tx_interval_ns;
            }
        }

        // Duration check gated on the same conditions as the wake
        // check; either stop or sync loss mid-callback suppresses
        // the duration trip.
        if (monitor_ptr != nullptr && tripwire_start_ns != 0 && !stop.stop_requested() && gptp_synced()) {
            (void)observe_duration_and_handle_tripwire(*monitor_ptr, event, tripwire_start_ns);
        }
    }

    /// Templated body of run_realtime: builds the realtime::Timer,
    /// spawns the reporter thread, idle-waits for stop. The adapter
    /// type encodes which clock source drives the timer (gptp-slave
    /// or ptp4l); everything else (callback body, tripwire wiring,
    /// reporter cadence) is identical across modes.
    template <typename AdapterT>
    void run_realtime_with_adapter(statusbar::itc::StopToken& stop, int64_t tx_interval_ns, AdapterT adapter)
    {
        using namespace statusbar::realtime;
        TxContext const tx_ctx = build_tx_context();
        RxContext const rx_ctx = build_rx_context();

        TimerConfig tcfg = cfg_.wan_timer;
        tcfg.name = "udptun_rt";
        tcfg.period_ns = cfg_.wan_timer.period_ns;

        // Optional tripwire monitor — owned here, lives until this
        // method returns. Callback captures by pointer (nullptr when
        // no trace config provided). RAII Start guards lifecycle.
        std::optional<TripwireMonitor> tripwire_monitor;
        std::optional<TripwireMonitorStart> tripwire_start;
        if (cfg_.trace.has_value()) {
            tripwire_monitor.emplace(*cfg_.trace);
            tripwire_start.emplace(*tripwire_monitor, &stop);
        }
        TripwireMonitor* const monitor_ptr = tripwire_monitor ? &*tripwire_monitor : nullptr;

        using ClockT = typename AdapterT::clock_type;
        auto callback = [this, &tx_ctx, &rx_ctx, tx_interval_ns, monitor_ptr, &stop](
                            TimerEvent<ClockT> const& event, statusbar::stats::AtomicWakeStats::Snapshot const& /*stats*/) {
            realtime_tick(event, tx_ctx, rx_ctx, tx_interval_ns, monitor_ptr, stop);
        };

        Timer<AdapterT> timer{tcfg, std::move(callback), std::move(adapter)};
        timer.start();

        // Reporter thread: prints stats every report_interval_us. Uses
        // stop.wait_for_stop() so the reporter wakes immediately when
        // stop is requested from any path.
        int64_t const report_interval_us = static_cast<int64_t>(cfg_.report_interval_us);
        std::thread reporter{[this, &stop, report_interval_us]() {
            // Exception barrier (run_guarded): print_live_report can throw; an
            // exception escaping a thread entry calls std::terminate. On a throw,
            // stop reporting rather than abort the process.
            run_guarded("udptun live-report thread", [this, &stop, report_interval_us]() {
                auto const period = std::chrono::microseconds(report_interval_us);
                while (!stop.stop_requested()) {
                    if (stop.wait_for_stop(period)) {
                        break;
                    }
                    print_live_report(std::cout);
                }
            });
        }};

        // Short-poll idle wait. SIGINT/SIGTERM's signal_set() is
        // signal-safe but doesn't cv-notify, so a sentinel-long
        // wait_for_stop wouldn't wake until its timeout. 100ms poll
        // matches the watchdog cadence and is plenty fast for a
        // graceful shutdown.
        while (!stop.stop_requested()) {
            (void)stop.wait_for_stop(std::chrono::milliseconds(100));
        }

        timer.stop();
        // Reporter wakes on stop. On request_stop() (called by the
        // watchdog when its deadline fires) it wakes immediately via
        // cv-notify; on a pure signal_set() path (SIGINT/SIGTERM with
        // no --duration-s) it wakes on its next wait_for_stop
        // timeout, up to one report_interval_us later.
        if (reporter.joinable()) {
            reporter.join();
        }

        // Snapshot the timer's wake/duration histograms before the timer
        // object goes out of scope. Histograms are populated every tick
        // regardless of tripwire, so the snapshot is always meaningful.
        latest_wake_stats_ = timer.stats();

        if (cfg_.trace.has_value()) {
            print_timer_stats(timer);
        }
    }
#endif

  public:
    /// Diagnostic counters from the active time-source bridge's
    /// AtomicTripleBuffer-protected state — `(rate, offset_ns)` for
    /// ptp4l mode, `(offset, ppt, prev_gptp, prev_app)` for gptp-slave
    /// mode. `overruns` counts publishes that overwrote a still-
    /// unconsumed prior publish. Non-zero is expected and benign: the
    /// sampler publishes at ~500 Hz while the RT-timer consumer reads
    /// at ~8 kHz, so most "overruns" just mean the wan_timer didn't
    /// happen to consume between two sample publishes. `available` is
    /// false in realtime mode (no bridge at all).
    struct BridgeBufferStats
    {
        uint64_t overruns{0};
        bool available{false};
    };
    [[nodiscard]] auto bridge_buffer_stats() const noexcept -> BridgeBufferStats
    {
#if defined(__linux__)
        if (ptp4l_ctx_.has_value()) {
            return {
                .overruns = ptp4l_ctx_->bridge->regression_buffer_overruns(),
                .available = true,
            };
        }
        if (gptp_session_.has_value()) {
            return {
                .overruns = gptp_session_->bridge().rated_buffer_overruns(),
                .available = true,
            };
        }
#endif
        return {};
    }

    /// Stop the gPTP slave and drain any RX packets still in flight at
    /// shutdown. `drain_ns` is typically the value returned by
    /// `natural_drain_ns()`. Idempotent for the gptp stop; the drain
    /// terminates early on a second SIGINT (EINTR on poll).
    void drain_after_stop(int64_t drain_ns)
    {
        gptp_stop();
        bool const drain_synced = gptp_synced();
        RxContext const rx_ctx = build_rx_context();
        drain_inflight_after_stop(codec_, rx_ctx, drain_synced, drain_ns);
    }

    /// Two-RTT-plus-floor drain window derived from observed max RTT.
    /// Suitable input for `drain_after_stop`.
    [[nodiscard]] auto natural_drain_ns() const noexcept -> int64_t
    {
        return rtt_grace_ns(rtt_stats_.time_stats.snapshot().max_ns);
    }

    /// Print the live report (per-source lines + RTT line) to `out`. The
    /// run loop calls this on each report_tfd expiration via
    /// handle_report_tick, but it can also be invoked externally on the
    /// owning thread.
    void print_live_report(std::ostream& out)
    {
        int64_t const now_mraw = monotonic_raw_ns();
        double const wall = static_cast<double>(now_mraw - start_mraw_) / 1e9;
        int64_t const grace_ns = natural_drain_ns();
        TxLossStats tx{
            .sent_total = sequence_.load(std::memory_order_relaxed),
            .sent_at_cutoff = tx_history_.sent_count_at_or_before(now_mraw - grace_ns),
            .tx_failures = tx_failures_.load(),
        };
        RedundancyDisplayStats red{};
        bool const red_enabled = tx_state_.redundant_enabled;
        if (red_enabled) {
            // Use the latest observed PT (from any RX direction) as the
            // scan reference. If we've sent but not yet received any
            // self-loopback echo, fall back to the latest stamped PT —
            // that bounds the cursor so we still retire empty slots.
            int64_t const last_rx = last_rx_pt_.load();
            int64_t const ref_pt = (last_rx != 0) ? last_rx : last_tx_pt_.load();
            self_redundancy_.scan(ref_pt, grace_ns);
            red.recovered = self_redundancy_.recovered();
            red.true_loss = self_redundancy_.true_loss();
            red.primary_received = self_redundancy_.primary_received();
        }
        // Show wire-timeline time: master + the published TAI shift
        // (0 unless --wire-clock=gps-tai), so the printed gptp_ns matches
        // what actually goes on the wire.
        ::statusbar::udptun::print_live_report(
            out, wall, tracker_, &rtt_stats_, &tx, red_enabled ? &red : nullptr, cached_master_ns() + last_tai_shift_ns_.load());
    }

    /// Drain and close the record-sink writer thread. No-op when recording is
    /// disabled. Returns the first write error the writer thread observed, if any.
    [[nodiscard]] auto flush_csv() -> Status { return record_sink_.flush(); }

    /// Returns true if at least one per-packet sink (CSV or colbin) is configured.
    [[nodiscard]] auto record_sink_enabled() const noexcept -> bool { return record_sink_.enabled(); }

    /// Number of CSV records dropped because the SPSC ring was full (writer thread fell
    /// behind disk for longer than the ring depth).
    [[nodiscard]] auto csv_records_dropped() const noexcept -> uint64_t { return record_sink_.records_dropped(); }

    /// Print the end-of-run summary (per-source lines + histograms +
    /// dropped/truncated counters) to `out`. Should be called after
    /// `drain_after_stop` so tail packets are accounted before any
    /// remaining redundancy entries are flushed as true_loss.
    void print_final_summary(std::ostream& out, std::string_view label)
    {
        uint32_t const sent = sequence_.load(std::memory_order_relaxed);
        TxLossStats tx{
            .sent_total = sent,
            .sent_at_cutoff = sent,
            .tx_failures = tx_failures_.load(),
        };
        RedundancyDisplayStats red{};
        bool const red_enabled = tx_state_.redundant_enabled;
        if (red_enabled) {
            // Sweep the cursor through every PT we sent; empty slots
            // along the way count as true_loss.
            self_redundancy_.flush_all(last_tx_pt_.load());
            red.recovered = self_redundancy_.recovered();
            red.true_loss = self_redundancy_.true_loss();
            red.primary_received = self_redundancy_.primary_received();
        }
        ::statusbar::udptun::print_final_summary(
            out, label, tracker_, &rtt_stats_, &tx, red_enabled ? &red : nullptr, cached_master_ns() + last_tai_shift_ns_.load());
    }

    /// Final snapshot of the realtime timer's wake-error and
    /// callback-duration histograms (plus min/max/avg/stddev). Populated
    /// at the end of `run()` when the realtime-timer path was taken;
    /// `std::nullopt` for run_polling or before `run()` finishes.
    /// Histograms are accumulated every tick regardless of `trace`, so
    /// the snapshot is always meaningful when present.
#if defined(__linux__)
    [[nodiscard]] auto latest_wake_stats() const noexcept -> std::optional<statusbar::stats::AtomicWakeStats::Snapshot> const&
    {
        return latest_wake_stats_;
    }
#endif

    [[nodiscard]] auto sent_count() const noexcept -> uint32_t { return sequence_.load(std::memory_order_relaxed); }
    [[nodiscard]] auto tx_failures() const noexcept -> uint64_t { return tx_failures_.load(); }
    [[nodiscard]] auto tracker() const noexcept -> PerSourceTracker const& { return tracker_; }
    [[nodiscard]] auto rtt_stats() const noexcept -> LatencyStats const& { return rtt_stats_; }
    [[nodiscard]] auto tx_state() const noexcept -> TxIdentityPair const& { return tx_state_; }
    [[nodiscard]] auto local_mac() const noexcept -> ieee::Eui48 const& { return local_mac_; }
    [[nodiscard]] auto redundancy_enabled() const noexcept -> bool { return tx_state_.redundant_enabled; }

  private:
    void setup_udp()
    {
        bool use_ipv6 = cfg_.ipv6;
        if (cfg_.tx_interval_us > 0) {
            auto pres = net::SocketAddress::from_string(cfg_.peer_host, std::to_string(cfg_.peer_port));
            if (!pres) {
                statusbar::throw_or_abort(std::errc::invalid_argument, "invalid peer address");
            }
            peer_ = *pres;
            use_ipv6 = (peer_.family() == AF_INET6);
        }
        if (cfg_.preopened_udp.valid()) {
            udp_ = std::move(cfg_.preopened_udp);
        } else {
            auto local_addr =
                use_ipv6 ? net::SocketAddress::ipv6_any(cfg_.local_port) : net::SocketAddress::ipv4_any(cfg_.local_port);
            auto udp_res = net::create_udp_socket(local_addr, /*do_bind=*/true, cfg_.dscp);
            if (!udp_res) {
                statusbar::throw_or_abort(udp_res.error(), "create_udp_socket failed");
            }
            udp_ = net::FileDescriptor{std::move(*udp_res)};
        }
        if (auto st = net::set_nonblocking(udp_.get()); !st) {
            statusbar::throw_or_abort(st.error(), "set_nonblocking failed");
        }
        // RX kernel-side timestamping (SO_TIMESTAMPNS_NEW) was removed
        // intentionally: its CLOCK_REALTIME basis turns clock_settime
        // events (phc2sys / chronyd / systemd-timesyncd steps) into
        // single-packet latency anomalies of size = step. drain_rx
        // self-samples CLOCK_MONOTONIC_RAW at drain-loop start instead,
        // which quantises RX timestamps to the drain cadence (≤ one
        // wan_timer tick, default 1 ms) but is immune to wall-clock
        // discontinuities.
        int const fam = use_ipv6 ? AF_INET6 : AF_INET;
        bind_to_interface(udp_.get(), fam, cfg_.interface);
        if (peer_.valid() && is_ipv4_multicast(peer_)) {
            if (!configure_multicast(udp_.get(), cfg_.interface, peer_, cfg_.mcast_ttl)) {
                statusbar::throw_or_abort(std::errc::io_error, "configure_multicast failed");
            }
        }
    }

    [[nodiscard]] auto build_tx_context() noexcept -> TxContext
    {
        return TxContext{
            .udp_fd = udp_.get(),
            .peer = peer_,
            .tx = tx_state_,
            .tx_interval_us = static_cast<uint32_t>(cfg_.tx_interval_us),
            .worst_case_latency_ns = cfg_.worst_case_latency_ns,
            .clock = clock_,
            .tx_buf = std::span<uint8_t>(tx_buf_),
            .tx_history = tx_history_,
            .redundant_buffer = redundant_buffer_,
            .tx_failures = tx_failures_,
            .last_tx_pt_ns = last_tx_pt_,
        };
    }

    [[nodiscard]] auto build_rx_context() noexcept -> RxContext
    {
        return RxContext{
            .udp_fd = udp_.get(),
            .my_pair_id = my_pair_id_,
            .peer = peer_,
            .source_gate_enabled = peer_.valid() && !is_ipv4_multicast(peer_),
            .worst_case_latency_ns = cfg_.worst_case_latency_ns,
            .peer_tai_offset_ns = cfg_.peer_tai_offset_ns,
            .clock = clock_,
            .tracker = tracker_,
            .rtt_stats = rtt_stats_,
            .self_redundancy = self_redundancy_,
            .csv_sink = record_sink_.sink(),
            .last_rx_pt_ns = last_rx_pt_,
        };
    }

    /// Slot width for the self-loopback redundancy tracker. In RX-only
    /// runs (tx_interval_us == 0) the tracker is dormant; we still need
    /// a positive slot_width for construction, so use a 1 ms placeholder.
    [[nodiscard]] static auto slot_width_ns_for_tracker(uint64_t tx_interval_us) noexcept -> int64_t
    {
        return tx_interval_us > 0 ? static_cast<int64_t>(tx_interval_us) * 1'000 : int64_t{1'000'000};
    }

    void handle_report_tick()
    {
        // DeadlineTimer-driven now: no kernel fd to drain, just emit.
        print_live_report(std::cout);
    }

    /// Wire the configured time source (gptp-slave, ptp4l, realtime)
    /// into the session in a single place so the rest of the class
    /// doesn't sprinkle `#if defined(__linux__)` everywhere. On
    /// non-Linux platforms only the realtime path is supported;
    /// gptp-slave and ptp4l fall back to it with a warning.
    [[nodiscard]] auto bring_up_local_identity() -> std::optional<ieee::Eui48>
    {
#if defined(__linux__)
        switch (cfg_.time_source.kind) {
            case TimeSourceKind::GptpSlave:
                return setup_local_identity(cfg_.time_source.identity, gptp_session_);
            case TimeSourceKind::Ptp4l:
                return bring_up_ptp4l();
            case TimeSourceKind::Realtime:
                return read_mac_for_non_gptp_modes();
        }
        return std::nullopt;
#else
        return read_mac_for_non_gptp_modes();
#endif
    }

#if defined(__linux__)
    [[nodiscard]] auto bring_up_ptp4l() -> std::optional<ieee::Eui48>
    {
        // Bound the wait for the ptp4l PHC bridge to become healthy. wait_for_healthy()
        // has its timeout disabled by default (0) and was handed an always-false abort
        // predicate, so an unhealthy /dev/ptp0 (e.g. ptp4l not disciplining the PHC) made
        // it loop forever. Because bring_up_ptp4l() runs in the Session constructor —
        // before run_normal() installs the stop signal and arms the --duration-s watchdog —
        // that hang ignored both Ctrl-C and the run deadline while the already-bound UDP
        // socket backed up (the 41-minute zombie holding the port). Fail fast: once the
        // deadline passes is_shutdown() returns true, so setup_ptp_app_with_fallback()
        // returns the error without falling back to the system clock, the ctor fails, and
        // owlm exits non-zero.
        constexpr auto health_timeout = std::chrono::seconds{30};
        auto const health_deadline = std::chrono::steady_clock::now() + health_timeout;
        auto ctx_or = ptpclient::setup_ptp_app_with_fallback(
            cfg_.time_source.ptp4l, [health_deadline]() noexcept { return std::chrono::steady_clock::now() >= health_deadline; });
        if (!ctx_or) {
            std::println(stderr, "ptp4l: setup_ptp_app_with_fallback failed: {}", ctx_or.error().message());
            return std::nullopt;
        }
        ptp4l_ctx_.emplace(std::move(*ctx_or));
        return read_mac_for_non_gptp_modes();
    }
#endif

    [[nodiscard]] auto read_mac_for_non_gptp_modes() -> std::optional<ieee::Eui48>
    {
        std::string iface = cfg_.time_source.mac_interface;
#if defined(__linux__)
        if (iface.empty()) {
            iface = cfg_.time_source.identity.fallback_interface.empty()
                ? cfg_.time_source.identity.gptp_session_config.interface
                : cfg_.time_source.identity.fallback_interface;
        }
#else
        if (iface.empty()) {
            iface = cfg_.time_source.identity.fallback_interface;
        }
#endif
        return read_mac_from_interface(iface);
    }

    [[nodiscard]] auto build_wire_translator() -> WireClock::Translator
    {
#if defined(__linux__)
        switch (cfg_.time_source.kind) {
            case TimeSourceKind::GptpSlave:
                return gptp_session_.has_value() ? make_gptp_slave_translator(gptp_session_->bridge()) : WireClock::Translator{};
            case TimeSourceKind::Ptp4l:
                return ptp4l_ctx_.has_value() ? make_ptp4l_translator(*ptp4l_ctx_->bridge) : WireClock::Translator{};
            case TimeSourceKind::Realtime:
                return WireClock::Translator{};
        }
#endif
        return WireClock::Translator{};
    }

    [[nodiscard]] auto gptp_synced() const noexcept -> bool
    {
#if defined(__linux__)
        if (gptp_session_.has_value()) {
            return gptp_session_->synced();
        }
        if (ptp4l_ctx_.has_value()) {
            return ptp4l_ctx_->bridge->is_healthy();
        }
#endif
        // Realtime mode: always "synced" — no master clock to wait for.
        return true;
    }

    /// SPSC consumer. Reads the active time source's "now" once
    /// (consuming the bridge's triple buffer in ptp4l mode) and
    /// caches the result in cached_master_ns_ as a side effect so
    /// multi-reader callers can read it race-free. May only be
    /// called from one thread at a time — the consumer position
    /// transfers between the main thread (during setup / shutdown)
    /// and the wan_timer thread (during steady-state operation).
    /// Returns 0 in realtime mode (no master).
    [[nodiscard]] auto consume_master_ns() noexcept -> int64_t
    {
        int64_t value = 0;
#if defined(__linux__)
        if (gptp_session_.has_value()) {
            value = gptp_session_->bridge().gptp_now();
        } else if (ptp4l_ctx_.has_value()) {
            value = ptp4l_ctx_->bridge->now_ns();
        }
#endif
        cached_master_ns_.publish(value);
        return value;
    }

    /// Multi-reader. Returns the value most recently stored by
    /// consume_master_ns(). Single Published<int64_t>::load() —
    /// race-free regardless of how many threads call it concurrently.
    /// Staleness: at most one wan_timer tick (~125 µs) behind the
    /// real-time master clock during steady-state operation; reads 0
    /// before the first consume_master_ns() call.
    [[nodiscard]] auto cached_master_ns() const noexcept -> int64_t { return cached_master_ns_.load(); }

    /// GPS-TAI sampler: rate-limited (4 Hz) paired read of the master
    /// timeline vs CLOCK_REALTIME, fed to the Kalman translator. The
    /// realtime reads bracket the master read; their midpoint is the
    /// paired UTC instant. Runs on the loop thread (RT callback or
    /// polling loop) so the translator stays single-threaded. The
    /// master read goes through master_clock_ (mraw→master mapping; no
    /// PHC syscall in ptp4l mode), the same timeline the RX path and
    /// the RT scheduler use. No-op unless wire_clock_gps_tai.
    void tai_sample_tick(int64_t now_mraw) noexcept
    {
        if (!cfg_.wire_clock_gps_tai || now_mraw < next_tai_sample_mraw_) {
            return;
        }
        next_tai_sample_mraw_ = now_mraw + tai_sample_interval_ns_;
        int64_t const rt1 = realtime_ns();
        int64_t const master = master_clock_.wire_ns(monotonic_raw_ns());
        int64_t const rt2 = realtime_ns();
        tai_translator_.add_sample(master, rt1 + ((rt2 - rt1) / 2));
        // Race-free view of the master→TAI shift for the reporter thread
        // (which must not touch the single-threaded Kalman state).
        last_tai_shift_ns_.publish(tai_translator_.tai_ns(master) - master);
    }

    [[nodiscard]] auto build_session_pollfds(std::array<pollfd, 2>& fds) noexcept -> size_t
    {
#if defined(__linux__)
        return build_pollfds(gptp_session_, udp_.get(), fds);
#else
        return build_pollfds_udp_only(udp_.get(), fds);
#endif
    }

    [[nodiscard]] auto gptp_dispatch_if_owned(int fd, std::chrono::steady_clock::time_point now) -> bool
    {
#if defined(__linux__)
        if (gptp_session_.has_value() && gptp_session_->owns_fd(fd)) {
            gptp_session_->dispatch(fd, now);
            return true;
        }
        return false;
#else
        (void)fd;
        (void)now;
        return false;
#endif
    }

    void gptp_periodic_tick(std::chrono::steady_clock::time_point now)
    {
#if defined(__linux__)
        if (gptp_session_.has_value()) {
            gptp_session_->tick(now);
        }
#else
        (void)now;
#endif
    }

    void gptp_stop()
    {
#if defined(__linux__)
        if (gptp_session_.has_value()) {
            gptp_session_->stop();
        }
#endif
    }

    SessionConfig cfg_;
    C codec_;
#if defined(__linux__)
    // gPTP slave session is owned in place — SlaveSession is non-movable
    // (deletes both copy and move), so its address must stay stable for
    // Session's lifetime. setup_local_identity emplaces into this
    // optional during construction.
    std::optional<gptp::SlaveSession> gptp_session_{};
    // Ptp4l mode owns a PtpAppContext (client + bridge + sampling
    // guard). Mutually exclusive with gptp_session_; only one of the
    // two is populated based on cfg_.time_source.kind.
    std::optional<ptpclient::PtpAppContext> ptp4l_ctx_{};
#endif
    // Updated by consume_master_ns(); read by cached_master_ns().
    // Decouples the SPSC bridge consumer (wan_timer or main during
    // setup/shutdown) from multi-reader callers (reporter thread,
    // shutdown summary).
    statusbar::itc::Published<int64_t> cached_master_ns_{};
    ieee::Eui48 local_mac_{};
    TxIdentityPair tx_state_{};
    ieee::Eui64 my_pair_id_{};
    WireClock clock_{};
    /// Inner mraw→master mapping (gPTP slave / ptp4l bridge / realtime).
    /// When wire_clock_gps_tai is off, clock_ holds the same translator;
    /// when on, clock_ composes tai_translator_ on top of this.
    WireClock master_clock_{};
    /// GPS-TAI translator state. Only touched on the loop thread (RT
    /// callback or polling loop): tai_sample_tick() feeds it and the clock_
    /// closure reads it. No cross-thread use.
    ptpclient::GpsTaiTranslator tai_translator_{};
    int64_t next_tai_sample_mraw_{0};
    static constexpr int64_t tai_sample_interval_ns_ = 250'000'000;  // 4 Hz
    /// (TAI − master) as of the last sampler update. Published for the
    /// reporter thread so the live report can show wire-timeline time
    /// without touching the Kalman state. 0 until the first sample (and
    /// always 0 with wire_clock_gps_tai off).
    statusbar::itc::Published<int64_t> last_tai_shift_ns_{};
    net::SocketAddress peer_{};
    net::FileDescriptor udp_{-1};
    DeadlineTimer tx_timer_{};
    DeadlineTimer report_timer_{};
    PerSourceTracker tracker_;
    LatencyStats rtt_stats_;
    std::vector<uint8_t> tx_buf_;
    TxHistory tx_history_{};
    RedundantTxBuffer redundant_buffer_{};
    RedundantRxTracker self_redundancy_;
    statusbar::itc::TelemetryCounter<uint64_t> tx_failures_{};
    std::atomic<uint32_t> sequence_{0};
    /// Gptp-domain deadline for the next TX. Updated by the RT
    /// callback only; not accessed cross-thread. Initialized in
    /// run_realtime() to the first tx_interval boundary after
    /// session start.
    int64_t next_tx_gptp_ns_{INT64_MAX};
    int64_t start_mraw_{0};
    /// Most recently stamped PT (set by send_packet via TxContext).
    /// Used as the upper bound for self_redundancy_.flush_all.
    statusbar::itc::Published<int64_t> last_tx_pt_{};
    /// Most recently observed PT from any RX (set by
    /// process_one_rx_datagram via RxContext). Used as the scan
    /// reference for self_redundancy_.scan; falls back to last_tx_pt_
    /// before the first echo arrives.
    statusbar::itc::Published<int64_t> last_rx_pt_{};
    /// CSV streaming state. The ring is the SPSC FIFO between the RX
    /// hot path (producer) and `csv_writer_thread_` (consumer); writer
    /// thread owns the open file via `csv_writer_`. `csv_writer_stop_`
    /// signals the writer to drain and exit; `csv_writer_error_` holds
    /// the first errno seen (0 = healthy). All addresses are stable for
    /// the run since Session is non-movable; RxContext borrows pointers.
#if defined(__linux__)
    /// Final snapshot of the realtime timer's wake/duration stats. Set
    /// at the end of run_realtime_with_adapter (after timer.stop), so
    /// callers can read it once `run()` has returned.
    std::optional<statusbar::stats::AtomicWakeStats::Snapshot> latest_wake_stats_{};
#endif
    // Per-packet CSV/colbin recorder (SPSC ring + writer thread). Declared last so it
    // is the last member constructed (thread starts after everything it might race) and
    // the first destroyed (thread stops before the state it reads goes away).
    RecordSink record_sink_;
};

}  // namespace statusbar::udptun
