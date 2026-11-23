#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Adapter that exposes a `gptp::SlaveSession`'s `GptpTimeBridge` through
/// the `statusbar::realtime::ClockSource` concept, so the udptun Session
/// can drive a `realtime::Timer<ClockAdapter<GptpClock<0>, ...>>` off the
/// same gPTP slave it already runs internally.
///
/// Linux only — depends on gptp::SlaveSession which is itself
/// Linux-only.

#if defined(__linux__)

#    include "statusbar/gptp/gptp_slave_session.hpp"
#    include "statusbar/realtime/realtime_clock.hpp"

#    include <cstdint>

namespace statusbar::udptun {

/// Lightweight wrapper. Holds a non-owning pointer to the slave session;
/// caller keeps the session alive across the adapter's lifetime (the
/// session is a Session-owned member, so this is automatic for the
/// usual usage from inside udptun::Session).
///
/// The slave's bridge is kept against CLOCK_MONOTONIC_RAW, the clock the
/// udptun wire timeline runs on, so to_raw_ns()/from_raw_ns() are
/// the bridge's rated line, and a realtime::Timer on it sleeps early on
/// CLOCK_MONOTONIC and spins on RAW to each deadline.
class GptpSlaveClockSource
{
  public:
    explicit GptpSlaveClockSource(gptp::SlaveSession* session) noexcept
        : session_{session}
    {}

    [[nodiscard]] auto now_ns() const noexcept -> int64_t { return realtime::GptpClock0::to_ns(session_->bridge().gptp_now()); }

    /// gPTP nanoseconds -> CLOCK_MONOTONIC_RAW nanoseconds (the bridge's rated line).
    [[nodiscard]] auto to_raw_ns(int64_t gptp_ns) const noexcept -> int64_t
    {
        return realtime::MonotonicRawClock::to_ns(session_->bridge().from_gptp_rated(realtime::GptpClock0::from_ns(gptp_ns)));
    }

    /// CLOCK_MONOTONIC_RAW nanoseconds -> gPTP nanoseconds (the bridge's rated line).
    [[nodiscard]] auto from_raw_ns(int64_t raw_ns) const noexcept -> int64_t
    {
        return realtime::GptpClock0::to_ns(session_->bridge().to_gptp_rated(realtime::MonotonicRawClock::from_ns(raw_ns)));
    }

    [[nodiscard]] auto is_healthy() const noexcept -> bool { return session_->synced(); }

    /// How well the bridge's line is known. While Syncs arrive it is
    /// refreshed at each one; when they stop it grows from the last, and
    /// the line goes on converting with the rate and offset it had. A
    /// realtime::Timer on this source tells every wake that accuracy.
    [[nodiscard]] auto quality() const noexcept -> realtime::ClockQuality { return session_->bridge().quality(); }

  private:
    gptp::SlaveSession* session_;
};

}  // namespace statusbar::udptun

#endif  // __linux__
