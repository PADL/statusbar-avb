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
/// The slave's bridge samples CLOCK_MONOTONIC_RAW, the clock the udptun
/// wire timeline runs on (setup_local_identity refuses any other
/// `SlaveSessionConfig::bridge_clock`), so this source's reference clock
/// is RAW: to_raw_ns()/from_raw_ns() are the bridge's rated line, and a
/// realtime::Timer on it sleeps early on CLOCK_MONOTONIC and spins on
/// RAW to each deadline.
class GptpSlaveClockSource
{
  public:
    static constexpr bool reference_is_raw = true;

    explicit GptpSlaveClockSource(gptp::SlaveSession* session) noexcept
        : session_{session}
    {}

    [[nodiscard]] auto now_ns() const noexcept -> int64_t { return session_->bridge().gptp_now(); }

    /// gPTP nanoseconds -> CLOCK_MONOTONIC_RAW nanoseconds (the bridge's rated line).
    [[nodiscard]] auto to_raw_ns(int64_t gptp_ns) const noexcept -> int64_t { return session_->bridge().from_gptp_rated(gptp_ns); }

    /// CLOCK_MONOTONIC_RAW nanoseconds -> gPTP nanoseconds (the bridge's rated line).
    [[nodiscard]] auto from_raw_ns(int64_t raw_ns) const noexcept -> int64_t { return session_->bridge().to_gptp_rated(raw_ns); }

    /// CLOCK_MONOTONIC conversions, for the ClockSource concept: RAW through
    /// the offset between the two clocks at the moment of the call (see
    /// realtime::MonotonicRawClockAdapter). Good for a one-off conversion,
    /// not for a schedule; a Timer on a raw-reference source never uses them.
    [[nodiscard]] auto to_monotonic_ns(int64_t gptp_ns) const noexcept -> int64_t
    {
        return realtime::MonotonicRawClockAdapter{}.to_monotonic_ns(to_raw_ns(gptp_ns));
    }

    [[nodiscard]] auto from_monotonic_ns(int64_t mono_ns) const noexcept -> int64_t
    {
        return from_raw_ns(realtime::MonotonicRawClockAdapter{}.from_monotonic_ns(mono_ns));
    }

    [[nodiscard]] auto is_healthy() const noexcept -> bool { return session_->synced(); }

    /// gPTP slave doesn't currently expose a discontinuity counter; the
    /// realtime::Timer uses `epoch()` to detect time-stepping events
    /// and resync its deadline computation. Returning the slave's
    /// "is_synced" toggle as a bit is a coarse approximation: a flip
    /// from synced→unsynced→synced increments the epoch and the timer
    /// resyncs.
    [[nodiscard]] auto epoch() const noexcept -> uint64_t { return session_->synced() ? 1U : 0U; }

  private:
    gptp::SlaveSession* session_;
};

}  // namespace statusbar::udptun

#endif  // __linux__
