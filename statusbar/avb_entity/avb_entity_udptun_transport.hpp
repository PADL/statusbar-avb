#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_udptun_transport.hpp
/// @brief UdptunTransport — the inter-site tunnel's TRANSPORT concern
/// (refactor phase C, split out of EntityUdptunBridge).
///
/// Owns the tunnel socket(s) and everything that establishes or defends them:
/// direct / direct-shared socket setup, the async STUN punch-retry worker, and
/// the media-thread service() that installs a freshly hole-punched socket,
/// sends the NAT keepalive, watches RX liveness, and tears down for a re-punch.
/// The ingest/egress paths borrow the sockets through send()/rx_fd()/tx_lock();
/// they never own them. Diagnostics stay in the shared UdptunTelemetry.

#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_telemetry.hpp"
#include "statusbar/itc/itc_published.hpp"
#include "statusbar/itc/itc_spin_lock.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/net/net_address.hpp"
#include "statusbar/net/net_socket.hpp"
#include "statusbar/sg14/inplace_function.h"

#include <atomic>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>

namespace statusbar::avb_entity {

class UdptunEgressPath;  // service() consults its self-heal; full type in the .cpp

/// CLOCK_REALTIME as nanoseconds since epoch plus @p offset_ns -- i.e. a point on the
/// TAI tunnel timeline (offset is TAI-UTC, default 37e9). Returns @p fallback if the
/// clock read fails (does not happen on Linux in practice). Centralizes the ns
/// arithmetic + the easy-to-forget offset that was repeated ~7 times across the entity
/// data plane.
[[nodiscard]] inline auto realtime_tai_ns(int64_t offset_ns, int64_t fallback = 0) noexcept -> int64_t
{
    timespec ts{};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return fallback;
    }
    return (static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL) + ts.tv_nsec + offset_ns;
}

class UdptunTransport
{
  public:
    UdptunTransport(AvbEntityAudioIOConfig const& config, std::shared_ptr<UdptunTelemetry> telemetry) noexcept
        : config_{config}
        , telemetry_{std::move(telemetry)}
    {}

    UdptunTransport(UdptunTransport const&) = delete;
    auto operator=(UdptunTransport const&) -> UdptunTransport& = delete;
    UdptunTransport(UdptunTransport&&) = delete;
    auto operator=(UdptunTransport&&) -> UdptunTransport& = delete;
    ~UdptunTransport() = default;

    /// Inject the main/reactor-thread logger (setup + rendezvous lines).
    void set_ctl_logger(logging::Logger const log) noexcept { ctl_log_ = log; }
    /// The punch worker's log channel — register with the tool's LogCollector.
    [[nodiscard]] auto worker_log_channel() noexcept -> logging::LogChannelBase& { return worker_log_channel_; }

    /// Fresh-tunnel hook, fired by service() the moment a staged hole-punched
    /// socket is installed — INSIDE the stage+tx locks, so the ingest re-anchor
    /// and egress reset it performs are never interleaved with a send. Must be
    /// plain field writes (RT media thread).
    void set_on_socket_installed(sg14::inplace_function<void(int64_t), 32> fn) noexcept { on_socket_installed_ = std::move(fn); }
    /// The egress path whose self-heal service() consults (nullptr = none).
    void set_egress(UdptunEgressPath* egress) noexcept { egress_ = egress; }

    // --- Establishment (one call from the bridge's start(); all non-fatal) -----
    /// Ingest-only direct socket -> config peer (no bind; egress has its own).
    [[nodiscard]] auto open_tx_socket() -> bool;
    /// Direct-peer bidirectional: ONE socket bound to udptun_listen_port does both
    /// ingest TX (sendto peer:peer_port) and egress RX. Both ends binding the same
    /// port and both transmitting opens both NAT pinholes -- the same hole-punch
    /// owlm uses in --time-source direct-peer mode -- so no STUN is needed. Used
    /// when direct mode has BOTH ingest and egress enabled.
    [[nodiscard]] auto open_shared_socket() -> bool;
    /// Egress-only direct socket bound to udptun_listen_port.
    [[nodiscard]] auto open_rx_socket() -> bool;
    /// Start the async STUN punch-retry worker (the production rendezvous path):
    /// the two sites' entities can't coordinate a single startup handshake, so
    /// retry with a clock-derived rotating session id until data flows. The
    /// entity's local AVB runs immediately; service() installs the socket the
    /// moment the worker stages one.
    [[nodiscard]] auto start_punch_worker() -> bool;
    void stop_punch_worker();

    // --- Media-thread service (install / keepalive / watchdog) ----------------
    /// Whether the media-thread must run service() this wake.
    /// True for the STUN worker (punch_run) AND for a DIRECT-SHARED socket:
    /// direct-shared has no worker to publish punch_run, but still needs the
    /// service's NAT keepalive + egress anchor-reset self-heal (the STUN-only
    /// teardown/re-punch paths inside the service are separately gated off by
    /// direct_shared_mode_). Pure so it is unit-testable without a socket.
    [[nodiscard]] static constexpr auto punch_service_should_run(bool punch_run, bool direct_shared_mode) noexcept -> bool
    {
        return punch_run || direct_shared_mode;
    }
    [[nodiscard]] auto punch_service_active() const noexcept -> bool
    {
        return punch_service_should_run(punch_run_.load(), direct_shared_mode_);
    }
    void service(int64_t now_tai_ns);

    // --- Data-plane access (borrowed by the ingest/egress paths) --------------
    /// True when the TX socket exists (direct, shared, or installed by a punch).
    [[nodiscard]] auto tx_ready() const noexcept -> bool { return fd_.get() >= 0; }
    /// Fire-and-forget datagram to the peer (MSG_DONTWAIT). Caller holds tx_lock().
    void send(std::span<uint8_t const> datagram) noexcept;
    /// The egress RX fd: the shared TX+RX socket when one is active, else the
    /// egress-only socket (-1 when neither exists).
    [[nodiscard]] auto rx_fd() const noexcept -> int { return shared_socket_ ? fd_.get() : rx_fd_.get(); }
    /// Serializes the two ingest producers (reactor RX = real audio, media RT =
    /// silence/sweep filler) into the non-thread-safe reframer, AND guards the
    /// send-visible socket state (fd_/peer_/ingest anchor) so the punch service's
    /// install/teardown never overlaps a reactor-thread sendto. RT caller
    /// try-acquires (wait-free skip on contention); reactor caller spin-acquires.
    [[nodiscard]] auto tx_lock() noexcept -> itc::SpinLock& { return tx_lock_; }
    [[nodiscard]] auto direct_shared() const noexcept -> bool { return direct_shared_mode_; }

  private:
    void udptun_punch_loop();
    /// Close the tunnel socket and wake the worker for a fresh window punch
    /// (counted in telemetry). RT path: count, don't print (the worker logs the
    /// ensuing re-punch).
    void teardown_for_repunch();

    AvbEntityAudioIOConfig const& config_;
    std::shared_ptr<UdptunTelemetry> telemetry_;

    net::FileDescriptor fd_{};  ///< TX socket (also RX in shared mode)
    net::SocketAddress peer_{};
    net::FileDescriptor rx_fd_{};  ///< egress-only RX socket (non-shared mode)
    itc::SpinLock tx_lock_{};

    // --- Socket sharing mode flags --------------------------------------------
    bool rendezvous_active_{false};   ///< fd_ is a STUN-traversed socket shared by TX+RX
    bool shared_socket_{false};       ///< fd_ does both ingest TX and egress RX (STUN or direct)
    bool direct_shared_mode_{false};  ///< DIRECT-SHARED (no STUN); stable for the socket's life

    // --- Logging ---------------------------------------------------------------
    // ctl_log_: main/reactor-thread lines (setup, rendezvous) — injected by the
    // owning bridge (set_ctl_logger). worker_log_channel_: the punch worker is
    // its OWN producer thread context, so it gets its own SPSC channel; the
    // tool registers it with the LogCollector like the host's channels.
    std::optional<logging::Logger> ctl_log_{};
    logging::LogChannel<64> worker_log_channel_{logging::lit("udptun")};

    // --- Async punch-retry worker + media-thread install/watchdog -------------
    std::thread punch_thread_{};
    itc::Published<bool> punch_run_{};
    std::mutex stage_mutex_{};
    net::FileDescriptor staged_fd_{};   ///< guarded by stage_mutex_
    net::SocketAddress staged_peer_{};  ///< guarded by stage_mutex_
    bool staged_ready_{false};          ///< guarded by stage_mutex_
    itc::Published<bool> punch_retry_{};
    int64_t install_tai_ns_{0};     ///< media: when current socket installed (0 = none)
    uint64_t rx_baseline_{0};       ///< media: any-rx snapshot for liveness
    int64_t last_rx_ns_{0};         ///< media: last time any-rx advanced
    bool saw_data_{false};          ///< media: any datagram since this install
    int64_t last_keepalive_ns_{0};  ///< media: last keepalive send TAI (rate limit)

    sg14::inplace_function<void(int64_t), 32> on_socket_installed_{};
    UdptunEgressPath* egress_{nullptr};
};

}  // namespace statusbar::avb_entity
