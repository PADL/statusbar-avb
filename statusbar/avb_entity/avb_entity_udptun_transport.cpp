// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// UdptunTransport methods — tunnel socket establishment, the STUN punch-retry
// worker, and the media-thread install/keepalive/watchdog service (refactor
// phase C, split out of EntityUdptunBridge; bodies preserved).

#include "statusbar/avb_entity/avb_entity_udptun_transport.hpp"

#include "statusbar/avb_entity/avb_entity_udptun_egress_path.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_ingest.hpp"
#include "statusbar/net/net_util.hpp"
#include "statusbar/status/catch_or_status.hpp"
#include "statusbar/stun/stun_rendezvous.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <utility>

#include <netinet/in.h>
#include <sys/socket.h>

namespace statusbar::avb_entity {

auto UdptunTransport::open_tx_socket() -> bool
{
    auto addr = net::SocketAddress::from_string(config_.udptun_peer_host, std::to_string(config_.udptun_peer_port));
    if (!addr) {
        if (ctl_log_) {
            ctl_log_->error(
                "udptun: cannot resolve peer {}:{}", logging::embed<40>(config_.udptun_peer_host), config_.udptun_peer_port);
        }
        return false;
    }
    peer_ = *addr;
    int const fd = ::socket(peer_.family(), SOCK_DGRAM, 0);
    if (fd < 0) {
        if (ctl_log_) {
            ctl_log_->error("udptun: socket() failed");
        }
        return false;
    }
    fd_ = net::FileDescriptor{fd};
    if (auto const s = net::set_nonblocking(fd); !s) {
        if (ctl_log_) {
            ctl_log_->error("udptun: set_nonblocking failed: errno {}", s.error().value());
        }
        fd_ = net::FileDescriptor{};
        return false;
    }
    return true;
}

auto UdptunTransport::open_shared_socket() -> bool
{
    auto addr = net::SocketAddress::from_string(config_.udptun_peer_host, std::to_string(config_.udptun_peer_port));
    if (!addr) {
        if (ctl_log_) {
            ctl_log_->error(
                "udptun: cannot resolve peer {}:{}", logging::embed<40>(config_.udptun_peer_host), config_.udptun_peer_port);
        }
        return false;
    }
    peer_ = *addr;
    int const fd = ::socket(peer_.family(), SOCK_DGRAM, 0);
    if (fd < 0) {
        if (ctl_log_) {
            ctl_log_->error("udptun: shared socket() failed");
        }
        return false;
    }
    fd_ = net::FileDescriptor{fd};
    if (auto const s = net::set_reuse_addr(fd); !s) {
        if (ctl_log_) {
            ctl_log_->warning("udptun: set_reuse_addr failed (continuing): errno {}", s.error().value());
        }
    }
    auto const bind_addr = (peer_.family() == AF_INET6) ? net::SocketAddress::ipv6_any(config_.udptun_listen_port)
                                                        : net::SocketAddress::ipv4_any(config_.udptun_listen_port);
    if (::bind(fd, bind_addr.sockaddr(), bind_addr.length()) != 0) {
        if (ctl_log_) {
            ctl_log_->error("udptun: shared bind :{} failed", config_.udptun_listen_port);
        }
        fd_ = net::FileDescriptor{};
        return false;
    }
    if (auto const s = net::set_nonblocking(fd); !s) {
        if (ctl_log_) {
            ctl_log_->error("udptun: set_nonblocking failed: errno {}", s.error().value());
        }
        fd_ = net::FileDescriptor{};
        return false;
    }
    shared_socket_ = true;
    direct_shared_mode_ = true;
    // Arm service()'s keepalive: a DIRECT-SHARED node that is momentarily idle
    // (no ingest audio) must still send the ~1 ms keepalive to hold its NAT
    // pinhole open, or the peer's packets are dropped and the tunnel is
    // one-directional until this node happens to transmit. The STUN punch only
    // armed this via the worker's socket hand-off, which never happens here --
    // so set the install timestamp ourselves. (The STUN-only teardown/re-punch
    // paths in the service are gated off by direct_shared_mode_.)
    int64_t const tai = realtime_tai_ns(config_.udptun_tai_offset_ns, 1);
    install_tai_ns_ = tai;
    last_rx_ns_ = tai;
    rx_baseline_ = telemetry_->any_rx.load();
    saw_data_ = true;  // shared socket is "up" at bind; no STUN first-data grace
    if (ctl_log_) {
        ctl_log_->status(
            "udptun: direct-peer shared socket :{} <-> {} (bidirectional hole-punch, no STUN)",
            config_.udptun_listen_port,
            logging::embed<48>(peer_.to_string()));
    }
    return true;
}

auto UdptunTransport::open_rx_socket() -> bool
{
    int const fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        if (ctl_log_) {
            ctl_log_->error("udptun: egress socket() failed");
        }
        return false;
    }
    rx_fd_ = net::FileDescriptor{fd};
    if (auto const s = net::set_reuse_addr(fd); !s) {
        if (ctl_log_) {
            ctl_log_->warning("udptun: egress set_reuse_addr failed (continuing): errno {}", s.error().value());
        }
    }
    auto const bind_addr = net::SocketAddress::ipv4_any(config_.udptun_listen_port);
    if (::bind(fd, bind_addr.sockaddr(), bind_addr.length()) != 0) {
        if (ctl_log_) {
            ctl_log_->error("udptun: egress bind :{} failed", config_.udptun_listen_port);
        }
        rx_fd_ = net::FileDescriptor{};
        return false;
    }
    if (auto const s = net::set_nonblocking(fd); !s) {
        if (ctl_log_) {
            ctl_log_->error("udptun: egress set_nonblocking failed: errno {}", s.error().value());
        }
        rx_fd_ = net::FileDescriptor{};
        return false;
    }
    return true;
}

void UdptunTransport::send(std::span<uint8_t const> const datagram) noexcept
{
    (void)::sendto(fd_.get(), datagram.data(), datagram.size(), MSG_DONTWAIT, peer_.sockaddr(), peer_.length());
}

auto UdptunTransport::start_punch_worker() -> bool
{
    if (config_.udptun_rendezvous_server.empty()) {
        return false;
    }
    if (!config_.udptun_enable && !config_.udptun_egress) {
        if (ctl_log_) {
            ctl_log_->error("udptun: punch: rendezvous set but neither enable nor egress -- skipping");
        }
        return false;
    }
    punch_run_.publish(true);
    punch_thread_ = std::thread([this] { statusbar::run_guarded("udptun-punch", [this] { udptun_punch_loop(); }); });
    if (ctl_log_) {
        ctl_log_->status(
            "udptun: STUN punch-retry worker started ({} as {}); local AVB runs now, tunnel comes up async",
            logging::embed<40>(config_.udptun_rendezvous_server),
            logging::embed<12>(config_.udptun_rendezvous_role));
    }
    return true;
}

void UdptunTransport::stop_punch_worker()
{
    punch_run_.publish(false);
    if (punch_thread_.joinable()) {
        punch_thread_.join();
    }
}

void UdptunTransport::udptun_punch_loop()
{
    using namespace std::chrono_literals;

    // The punch worker is its own producer thread context: log through the
    // transport's dedicated channel, never the ctl channel (SPSC contract).
    auto wlog = worker_log_channel_.logger();

    // Parse the static rendezvous inputs once.
    stun::SessionId base{};
    if (!net::parse_hex_into(config_.udptun_rendezvous_session_id, std::span<uint8_t>{base.bytes})) {
        wlog.error("punch: rendezvous-session-id must be 32 hex chars");
        return;
    }
    statusbar::crypto::Aes128SivKey key{};
    if (!net::parse_hex_into(config_.udptun_rendezvous_key, std::span<uint8_t>{key.data})) {
        wlog.error("punch: rendezvous-key must be 64 hex chars");
        return;
    }
    std::string host{};
    std::string port{};
    if (!net::split_host_port(config_.udptun_rendezvous_server, host, port)) {
        wlog.error("punch: rendezvous-server must be HOST:PORT");
        return;
    }
    auto server_addr = net::SocketAddress::from_string(host, port, net::SocketDatagram);
    if (!server_addr) {
        wlog.error("punch: cannot resolve rendezvous server '{}:{}'", logging::embed<40>(host), logging::embed<8>(port));
        return;
    }
    bool const responder = (config_.udptun_rendezvous_role == "responder");

    // Both peers attempt the SAME session id within each WINDOW, derived from the
    // shared GPS-NTP TAI clock (window counter overwrites the id's high 8 bytes).
    // Aligning attempts to window boundaries lands both nodes in the handshake
    // together with no external orchestration, and rotating the id every window
    // avoids stale STUN-server state -- exactly what owlm's harness achieves with a
    // fresh id per retry. The tunnel, once up, persists; the worker only re-punches
    // when the media-thread watchdog (service()) reports lost data.
    constexpr int64_t WINDOW_NS = 15'000'000'000LL;  // 15 s rendezvous window

    auto tai_now = [this]() -> int64_t { return realtime_tai_ns(config_.udptun_tai_offset_ns); };

    while (punch_run_.load()) {
        int64_t const now = tai_now();
        int64_t const window = (now > 0) ? now / WINDOW_NS : 0;
        int64_t const into_window = now - (window * WINDOW_NS);
        int64_t const remain_ms = (WINDOW_NS - into_window) / 1'000'000LL;

        stun::SessionId sid = base;
        for (int i = 0; i < 8; ++i) {
            sid.bytes[i] = static_cast<uint8_t>((window >> (8 * (7 - i))) & 0xFF);
        }
        // Placeholder EUI-64 from session id + role byte; the server only needs
        // the two peers' EUI-64s to differ (mirrors owlm's perform_rendezvous_into).
        ieee::Eui64 client_eui{};
        auto eui_span = client_eui.span();
        for (size_t i = 0; i < eui_span.size(); ++i) {
            eui_span[i] = sid.bytes[i];
        }
        eui_span[0] = responder ? 0x02U : 0x01U;

        stun::RendezvousConfig rcfg{
            .server_address = *server_addr,
            .session_id = sid,
            .client_eui64 = client_eui,
            .role = responder ? stun::Role::Responder : stun::Role::Initiator,
            .shared_key = key,
            // Ephemeral local port (0), NOT udptun_listen_port. In rendezvous mode the
            // hole-punched socket is SHARED for TX+RX and the peer endpoint is learned
            // from the STUN XOR-MAPPED-ADDRESS, so the local port is immaterial to the
            // data plane. Pinning a fixed port (e.g. 17220 -- also used by the direct
            // and egress paths) invites an endpoint-dependent / reused NAT mapping whose
            // STUN-observed external port differs from the peer-facing pinhole, breaking
            // the punch. An ephemeral port gets a fresh mapping and reports the true
            // reflexive (matches the proven stun-client tool, which binds ephemeral).
            .local_port = 0,
            .local_interface = config_.interface_name,
            .timeout_ms = static_cast<uint32_t>(std::clamp<int64_t>(remain_ms - 1000, 3000, 12000)),
        };

        auto result = stun::perform_rendezvous(rcfg);
        if (!punch_run_.load()) {
            break;
        }
        if (result) {
            // Make the hole-punched socket non-blocking here on the (non-RT) worker
            // thread, before handing it to the media thread — so the RT path never
            // touches socket options.
            if (auto const s = net::set_nonblocking(result->socket.get()); !s) {
                wlog.error("punch: set_nonblocking(staged socket) failed: errno {}", s.error().value());
            }
            {
                std::scoped_lock const lk(stage_mutex_);
                staged_fd_ = std::move(result->socket);
                staged_peer_ = result->peer_reflexive_address;
                staged_ready_ = true;
            }
            punch_retry_.publish(false);
            wlog.status(
                "punch: paired (window {}) peer={} -- handing socket to media thread",
                window,
                logging::embed<48>(result->peer_reflexive_address.to_string()));
            // Hold while the tunnel is up; the media thread sets punch_retry_
            // if RX never starts or a live stream stalls, prompting a fresh punch.
            while (punch_run_.load() && !punch_retry_.load()) {
                std::this_thread::sleep_for(250ms);
            }
            punch_retry_.publish(false);
        } else {
            wlog.status("punch: window {} no pair (errno {}) -- retry next window", window, result.error().value());
        }

        // Align the next attempt to the next window boundary so both peers fire
        // together; sleep in small chunks for responsive shutdown.
        int64_t const t2 = tai_now();
        int64_t const next_boundary = (((t2 > 0 ? t2 : 0) / WINDOW_NS) + 1) * WINDOW_NS;
        int64_t sleep_ns = next_boundary - t2;
        while (punch_run_.load() && sleep_ns > 0) {
            int64_t const chunk = std::min<int64_t>(sleep_ns, 200'000'000LL);
            std::this_thread::sleep_for(std::chrono::nanoseconds(chunk));
            sleep_ns -= chunk;
        }
    }
}

void UdptunTransport::teardown_for_repunch()
{
    telemetry_->egress_repunch_count.add(1);
    {
        std::scoped_lock const ig(tx_lock_);  // no close while the reactor may sendto
        fd_ = net::FileDescriptor{};          // close -> forces a fresh punch
    }
    shared_socket_ = false;
    rendezvous_active_ = false;
    install_tai_ns_ = 0;
    punch_retry_.publish(true);  // wake the worker for a fresh window punch
}

void UdptunTransport::service(int64_t now_tai_ns)
{
    // 1) Install a staged hole-punched socket (worker -> media handoff). Only this
    // (media) thread ever assigns fd_ during operation.
    {
        std::scoped_lock const lk(stage_mutex_);
        if (staged_ready_) {
            // Exclude the reactor-thread send while we swap the socket + peer.
            // Lock order stage_mutex_ -> tx_lock_ is the only site taking
            // both; the send takes only tx_lock_, staging only stage_mutex_.
            std::scoped_lock const ig(tx_lock_);
            fd_ = std::move(staged_fd_);
            peer_ = staged_peer_;
            staged_ready_ = false;
            // (socket was already set non-blocking by the worker before staging)
            shared_socket_ = true;
            rendezvous_active_ = true;
            install_tai_ns_ = now_tai_ns;
            rx_baseline_ = telemetry_->any_rx.load();
            last_rx_ns_ = now_tai_ns;
            saw_data_ = false;
            // Fresh tunnel: the bridge's hook re-anchors the ingest TAI and resets
            // the egress playout timeline (so a re-punch never leaves it stuck
            // emitting silence) + the egress-silence watchdog baselines.
            // No std::print on the RT media thread: the non-RT punch worker already
            // logs "paired ... peer=" just before handing the socket over.
            if (on_socket_installed_) {
                on_socket_installed_(now_tai_ns);
            }
        }
    }

    if (install_tai_ns_ == 0 || fd_.get() < 0) {
        return;  // no active tunnel socket
    }

    // 1b) Keepalive: a silence_source node already streams continuously, but a node
    // whose tunnel source is a (possibly-disconnected) AVB listener sends nothing
    // when idle -- so its NAT pinhole never opens and the punch can't complete until
    // audio happens to start. Emit a tiny raw datagram (NOT through the codec, so no
    // cross-thread ingest race) straight to the peer while idle, so both pinholes
    // open and stay warm; real audio takes over seamlessly when it arrives. The far
    // end drops the bad decode but still counts it as tunnel liveness (any_rx).
    if (!config_.udptun_silence_source) {
        int64_t const last_audio = telemetry_->last_real_ingest_tai.load();
        bool const streaming = udptun_source_streaming(last_audio, now_tai_ns);
        // ~1 ms cadence (matches owlm's proven punch rate + the ~2000 pkt/s real
        // audio that opened the pinhole on hardware). A slow keepalive (e.g. 50 ms)
        // refreshes a mapping but does NOT reliably open one on these home NATs.
        if (!streaming && (now_tai_ns - last_keepalive_ns_ > 1'000'000LL)) {
            last_keepalive_ns_ = now_tai_ns;
            std::array<uint8_t, 16> ka{};  // bare keepalive; opens/holds the NAT pinhole
            send(ka);
        }
    }

    // 2) Watchdog liveness via ANY received datagram (audio OR keepalive) -- the far
    // end's packets prove the punch actually opened (STUN pairing alone does not).
    // Counting keepalives keeps an idle-but-open pinhole warm, so when real audio
    // starts it flows immediately instead of waiting on a fresh punch. No first
    // packet within the grace window, or a live tunnel going silent, tears the
    // socket down and asks the worker to re-punch.
    uint64_t const rx = telemetry_->any_rx.load();
    if (rx != rx_baseline_) {
        rx_baseline_ = rx;
        last_rx_ns_ = now_tai_ns;
        saw_data_ = true;
    }

    // 2b) Egress-anchor self-heal (see UdptunEgressPath::self_heal): a true return
    // means several anchor resets did not recover playout while decoded audio kept
    // arriving -- escalate to a re-punch (STUN tunnel only; a DIRECT-SHARED socket
    // has no punch worker to re-create it, so self_heal never escalates there).
    if (egress_ != nullptr && egress_->self_heal(now_tai_ns, /*allow_escalate=*/!direct_shared_mode_)) {
        teardown_for_repunch();
        return;  // socket gone; nothing else to service this tick
    }

    // A residential-NAT hole-punch can take tens of seconds to actually open even
    // when it will succeed, so give first data a generous window before giving up
    // (tearing down too early kills a punch that was about to come through). Once a
    // stream is live, a much shorter stall triggers a re-punch.
    constexpr int64_t GRACE_NS = 45'000'000'000LL;  // first data must arrive within 45 s of install
    constexpr int64_t STALL_NS = 8'000'000'000LL;   // a live stream silent this long -> re-punch
    int64_t const since_install = now_tai_ns - install_tai_ns_;
    int64_t const since_rx = now_tai_ns - last_rx_ns_;
    bool const dead = (!saw_data_ && since_install > GRACE_NS) || (saw_data_ && since_rx > STALL_NS);
    // Only the STUN tunnel tears down + re-punches on a stall; DIRECT-SHARED has no
    // worker to re-punch, and its keepalive keeps the pinhole warm so it re-converges
    // on its own when the peer comes back.
    if (dead && !direct_shared_mode_) {
        teardown_for_repunch();
    }
}

}  // namespace statusbar::avb_entity
