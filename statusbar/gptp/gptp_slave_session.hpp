#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// SlaveSession — reusable gPTP slave wiring for Linux tools.
//
// Owns the raw socket, PHC fd (HW path) or SoftClock (SW path),
// GptpClockOps, GptpSlavePort, and GptpTimeBridge. Exposes a single
// poll FD plus a dispatch entrypoint for callers that integrate the
// slave into their own poll() loop.
//
// SlaveSession owns all verbose gPTP stderr output (mode banner,
// per-sync line, peer-delay line, end-of-run summary) when
// cfg_.verbose is true. Composing tools (gptp_slave_linux_tool,
// owlm_tool) just construct + start + run loop + stop.
//

#if defined(__linux__)

#    include "statusbar/gptp/gptp_clock_ops.hpp"
#    include "statusbar/gptp/gptp_clock_ops_linux.hpp"
#    include "statusbar/gptp/gptp_config.hpp"
#    include "statusbar/gptp/gptp_linux_socket.hpp"
#    include "statusbar/gptp/gptp_slave_port.hpp"
#    include "statusbar/gptp/gptp_soft_clock.hpp"
#    include "statusbar/gptp/gptp_time_bridge.hpp"
#    include "statusbar/ieee/ieee.hpp"
#    include "statusbar/sm/sm.hpp"

#    include <array>
#    include <atomic>
#    include <cstdint>
#    include <ctime>
#    include <memory>
#    include <span>
#    include <string>

namespace statusbar::gptp {

struct SlaveSessionConfig
{
    std::string interface{"eth0"};
    Profile profile{Profile::Standard};
    bool software_timestamping{false};
    int64_t manual_peer_delay_ns{-1};
    int64_t phase_jump_threshold_ns{-1};
    double servo_kp{-1.0};  // <0 → use profile default
    double servo_ki{-1.0};  // <0 → use profile default
    /// Keep a gPTP <-> CLOCK_MONOTONIC_RAW bridge. RAW is the one clock the
    /// bridge is kept against: it is the application's only timing reference.
    bool bridge{false};
    bool verbose{false};

    /// How fast the bridge's line may go wrong while no Sync arrives, in
    /// parts per billion: the deployment's oscillator and temperature
    /// figure. See GptpTimeBridge::holdover_drift_ppb.
    uint32_t holdover_drift_ppb{realtime::DEFAULT_HOLDOVER_DRIFT_PPB};

    /// When true (and software_timestamping is false), use the
    /// hardware-timestamping passthrough path: PHC is read for time
    /// and used for SO_TIMESTAMPING, but the servo's phase / frequency
    /// corrections accumulate in a SoftClock instead of being applied
    /// to the PHC via clock_adjtime / clock_settime. The application
    /// reads master-time via the bridge (`bridge.gptp_now()`), which
    /// returns `raw_PHC + soft_clock.virtual_offset`.
    ///
    /// Use this mode when master-time consumption is contained inside
    /// the process and the system PHC must not be modified — e.g.
    /// owlm with self-contained gPTP, no ptp4l-style discipline.
    bool phc_passthrough{false};
};

class SlaveSession
{
  public:
    explicit SlaveSession(SlaveSessionConfig cfg);
    ~SlaveSession();

    SlaveSession(SlaveSession const&) = delete;
    auto operator=(SlaveSession const&) -> SlaveSession& = delete;
    SlaveSession(SlaveSession&&) = delete;
    auto operator=(SlaveSession&&) -> SlaveSession& = delete;

    /// Open sockets, enable timestamping, start the slave port.
    /// Returns false on any setup failure (logs to stderr).
    [[nodiscard]] auto start() -> bool;

    /// Poll FDs the caller should add to its poll() set.
    [[nodiscard]] auto poll_fds() const -> std::span<int const>;

    /// Returns true if `fd` is one of ours.
    [[nodiscard]] auto owns_fd(int fd) const -> bool;

    /// Called by the caller when poll() reports POLLIN on `fd`.
    /// No-op if the fd is not ours.
    void dispatch(int fd, sm::TimePoint now);

    /// Called once per loop iteration after dispatch (drives port.tick()).
    void tick(sm::TimePoint now);

    /// Stop the port, refresh the bridge offset, print end-of-run summary.
    void stop();

    /// Bridge accessor for tools that need timestamp domain conversion.
    [[nodiscard]] auto bridge() noexcept -> GptpTimeBridge& { return bridge_; }
    [[nodiscard]] auto bridge() const noexcept -> GptpTimeBridge const& { return bridge_; }

    /// gPTP iface MAC, valid after start() returns true.
    [[nodiscard]] auto local_mac() const noexcept -> ieee::Eui48 { return mac_; }

    /// True once the gPTP slave has reached the LOCKED sync state at
    /// least once and has not since reverted to LOST. Updated by the
    /// session's internal observer; readable lock-free from any thread.
    /// Composing tools (owlm_tool) gate their TX/RX on this state.
    [[nodiscard]] auto synced() const noexcept -> bool { return is_synced_.load(std::memory_order_relaxed); }

  private:
    /// A different grandmaster is a different timeline: tell the bridge,
    /// with the time source the new one announces.
    void follow_grandmaster();

    SlaveSessionConfig cfg_;
    int raw_fd_{-1};
    int phc_fd_{-1};
    SoftClock soft_clock_{};
    GptpClockOps ops_{};
    ieee::Eui48 mac_{};
    int if_index_{-1};
    std::array<int, 1> poll_fd_storage_{-1};
    std::unique_ptr<GptpSlavePort> port_;
    GptpTimeBridge bridge_{};
    std::array<uint8_t, 1500> payload_buf_{};
    std::atomic<bool> is_synced_{false};
    uint64_t grandmaster_id_{0};
};

}  // namespace statusbar::gptp

#endif  // __linux__
