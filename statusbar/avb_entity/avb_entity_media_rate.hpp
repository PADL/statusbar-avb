#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_media_rate.hpp
/// @brief MediaClockRateTracker — pins the media-clock rate to GPS and exposes
/// the gPTP -> GPS-TAI mapping the inter-site tunnel needs.
///
/// Samples the gPTP-master vs CLOCK_REALTIME(GPS) pair a few times a second and
/// turns the offset slope into r = switch_rate / GPS_rate (which pins the media
/// clock), while feeding the same pair to a GPS-TAI translator for the tunnel
/// timeline. The CLOCK_REALTIME read and the telemetry print stay in the caller
/// (I/O); this class is the pure numeric core + rate-limit bookkeeping, pulled
/// out of AvbEntityAudioIO so it is unit-testable. The Kalman ratio filter and
/// the TAI translator are themselves separately tested ptpclient units.

#include "statusbar/ptpclient/ptpclient_freq_ratio.hpp"      // KalmanRatioTracker, RatioEstimate
#include "statusbar/ptpclient/ptpclient_tai_translator.hpp"  // GpsTaiTranslator

#include <cstdint>

namespace statusbar::avb_entity {

class MediaClockRateTracker
{
  public:
    MediaClockRateTracker() = default;

    /// (Re)configure the ratio Kalman and reset the running state.
    void configure(ptpclient::KalmanRatioTracker::Config const& cfg)
    {
        ratio_ = ptpclient::KalmanRatioTracker{cfg};
        r_ = 1.0;
        last_sample_ns_ = 0;
        last_log_ns_ = 0;
    }

    /// r = switch_rate / GPS_rate. 1.0 until the first valid estimate — also the
    /// value when GPS sampling is disabled (media_lock_to_gptp), since add_sample
    /// is then never called.
    [[nodiscard]] double r() const noexcept { return r_; }

    /// True once there are enough GPS samples to map gPTP -> GPS-TAI for the tunnel.
    [[nodiscard]] bool has_tai_sample() const noexcept { return tai_.has_sample(); }
    [[nodiscard]] auto tai_ns(std::int64_t gptp_ns) const noexcept -> std::int64_t { return tai_.tai_ns(gptp_ns); }

    /// Trivially-copyable snapshot of the GPS-TAI translator state for
    /// cross-thread publication (itc). A consumer thread evaluates
    /// ptpclient::tai_ns(snapshot, gptp_ns) instead of touching the live
    /// translator, which is single-threaded by contract.
    [[nodiscard]] auto tai_snapshot() const noexcept -> ptpclient::GpsTaiSnapshot { return tai_.snapshot(); }

    /// Rate-limit the GPS sampling to at most once per SAMPLE_INTERVAL so the
    /// CLOCK_REALTIME syscall stays off the per-packet hot path.
    [[nodiscard]] bool should_sample(std::uint64_t gptp_now_ns) const noexcept
    {
        return last_sample_ns_ == 0 || (gptp_now_ns - last_sample_ns_) >= SAMPLE_INTERVAL_NS;
    }

    /// Feed one (gPTP-master, GPS CLOCK_REALTIME) pair: updates the ratio Kalman
    /// (offset = gptp - gps), the TAI translator, r_, and the sample timestamp.
    /// Returns the resulting estimate.
    auto add_sample(std::uint64_t gptp_now_ns, std::uint64_t gps_now_ns) -> ptpclient::RatioEstimate
    {
        auto const offset = static_cast<std::int64_t>(gptp_now_ns) - static_cast<std::int64_t>(gps_now_ns);
        double const dt = (last_sample_ns_ != 0) ? static_cast<double>(gptp_now_ns - last_sample_ns_) * 1e-9 : 0.0;
        ratio_.add(offset, dt);
        tai_.add_sample(static_cast<std::int64_t>(gptp_now_ns), static_cast<std::int64_t>(gps_now_ns));
        auto const est = ratio_.estimate();
        if (est.valid) {
            r_ = est.r;
        }
        last_sample_ns_ = gptp_now_ns;
        return est;
    }

    /// Telemetry rate-limit (5 s) for the [media-clock] r/offset-slope line.
    [[nodiscard]] bool should_log(std::uint64_t gptp_now_ns) const noexcept
    {
        return last_log_ns_ == 0 || (gptp_now_ns - last_log_ns_) >= LOG_INTERVAL_NS;
    }
    void mark_logged(std::uint64_t gptp_now_ns) noexcept { last_log_ns_ = gptp_now_ns; }

  private:
    static constexpr std::uint64_t SAMPLE_INTERVAL_NS = 250'000'000;  // 0.25 s
    static constexpr std::uint64_t LOG_INTERVAL_NS = 5'000'000'000;   // 5 s

    ptpclient::KalmanRatioTracker ratio_{};
    ptpclient::GpsTaiTranslator tai_{};
    double r_{1.0};
    std::uint64_t last_sample_ns_{0};
    std::uint64_t last_log_ns_{0};
};

}  // namespace statusbar::avb_entity
