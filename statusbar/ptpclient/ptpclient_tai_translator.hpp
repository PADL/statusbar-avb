// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// GPS-TAI translator over a local master clock (ptp4l-fed PHC or gPTP slave
// timeline). Since the site GMs became free-running switches (the
// boundary-clock fixes), the local master timeline has an arbitrary epoch —
// it is NOT TAI. Absolute GPS time reaches each node only as NTP-disciplined
// CLOCK_REALTIME (chrony <- the GPS grandmaster). This class recovers an absolute TAI
// timeline by combining the two:
//
//   TAI(master) = master - offset_predicted(master) + tai_minus_utc
//
// where offset = master - CLOCK_REALTIME is tracked by the 3-state
// KalmanRatioTracker (phase / frequency / drift — see GPS_MEDIA_CLOCK.md).
// The master clock provides the short-term stability (ns-class, hardware
// timestamped); the long-time-constant filter provides the absolute epoch
// and the frequency relationship (the free-running switch GM is tens of ppm
// off GPS), without re-injecting per-sample NTP jitter: the translation uses
// the *filtered* phase, propagated with the filtered frequency + drift
// between sampling updates.
//
// The leap-second offset is applied manually (default 37 s, in force since
// 2017-01-01) — neither ptp4l, the PHC, nor the kernel TAI offset is
// consulted or modified, per the deployment's "manual second offset" rule.
//
// Pure (no I/O, no platform deps), single-threaded by design: feed
// add_sample() and call tai_ns() from the same thread (the wan-timer / poll
// loop). The Linux sampling (bracketed CLOCK_REALTIME reads around a master
// read) lives with the caller.

#pragma once

#include "statusbar/ptpclient/ptpclient_freq_ratio.hpp"

#include <cmath>
#include <cstdint>

namespace statusbar::ptpclient {

/// Immutable snapshot of the translator state needed to evaluate tai_ns() for
/// an arbitrary master-timeline reading. Trivially copyable so it can be
/// published across threads via itc::AtomicTripleBuffer: the thread that owns
/// the Kalman (the media-timer thread) publishes a snapshot after each
/// add_sample(); other threads evaluate tai_ns() from the snapshot without ever
/// touching the live filter (which is single-threaded by contract).
struct GpsTaiSnapshot
{
    bool have_sample = false;
    bool valid = false;
    std::int64_t filtered_offset_ns = 0;
    double r = 1.0;
    double drift_ppm_per_hr = 0.0;
    std::int64_t last_master_ns = 0;
    std::int64_t tai_minus_utc_ns = 37'000'000'000LL;
};

/// Pure evaluation of the GPS-TAI mapping from a snapshot. Identical formula to
/// GpsTaiTranslator::tai_ns() (see that method for the derivation); the member
/// delegates here so the two can never diverge.
[[nodiscard]] inline auto tai_ns(GpsTaiSnapshot const& snap, std::int64_t master_ns) noexcept -> std::int64_t
{
    if (!snap.have_sample) {
        return master_ns;
    }
    // Keep the base offset in int64: filtered_offset_ns is ~1.77e18 (GPS-TAI vs
    // master epoch), which loses ~128-256 ns of resolution if routed through
    // double. Only the small propagation term (freq*dt + 0.5*drift*dt^2) needs
    // floating point, and double is exact for a value that small.
    std::int64_t offset_pred_ns = snap.filtered_offset_ns;
    if (snap.valid) {
        double const dt_s = static_cast<double>(master_ns - snap.last_master_ns) * 1e-9;
        double const freq_ns_per_s = (snap.r - 1.0) * 1e9;
        double const drift_ns_per_s2 = snap.drift_ppm_per_hr / 3.6;
        double const propagation_ns = (freq_ns_per_s * dt_s) + (0.5 * drift_ns_per_s2 * dt_s * dt_s);
        offset_pred_ns += static_cast<std::int64_t>(std::llround(propagation_ns));
    }
    return master_ns - offset_pred_ns + snap.tai_minus_utc_ns;
}

class GpsTaiTranslator
{
  public:
    struct Config
    {
        /// TAI - UTC in ns, applied manually (37 s since 2017-01-01).
        std::int64_t tai_minus_utc_ns = 37'000'000'000LL;

        /// Filter tuning. Defaults match the media-clock deployment of
        /// KalmanRatioTracker (same physical sample pair: chrony-disciplined
        /// CLOCK_REALTIME vs the local PHC/gPTP timeline).
        KalmanRatioTracker::Config kalman{};
    };

    GpsTaiTranslator()
        : GpsTaiTranslator(Config{})
    {}

    explicit GpsTaiTranslator(Config c)
        : cfg_{c}
        , kalman_{c.kalman}
    {}

    /// Feed one paired reading: `master_ns` (PHC/gPTP timeline) and
    /// `utc_ns` (CLOCK_REALTIME) captured at the same instant — bracket the
    /// master read with two realtime reads and pass the midpoint.
    void add_sample(std::int64_t master_ns, std::int64_t utc_ns)
    {
        double const dt_s = have_sample_ ? static_cast<double>(utc_ns - last_utc_ns_) * 1e-9 : 0.0;
        kalman_.add(master_ns - utc_ns, dt_s);
        last_master_ns_ = master_ns;
        last_utc_ns_ = utc_ns;
        have_sample_ = true;
    }

    /// Translate a master-timeline reading to TAI.
    ///
    /// Quality tiers, in order of acquisition:
    ///   - no samples yet: identity (master_ns unchanged) — callers should
    ///     gate on has_sample() before stamping anything that leaves the box;
    ///   - 1 sample: raw offset (NTP-quality, jittery but absolutely sane);
    ///   - >= 2 samples: filtered phase propagated with filtered frequency
    ///     (+ drift) from the last update epoch. Converges in ~5 s of
    ///     samples; long-run accuracy is bounded by chrony's absolute
    ///     accuracy, not by per-sample jitter.
    //
    // Propagates the filtered phase from the last update epoch; dt is measured
    // on the master timeline, differing from GPS time by the ppm-level ratio (a
    // second-order error over the sub-second horizon — negligible).
    [[nodiscard]] std::int64_t tai_ns(std::int64_t master_ns) const noexcept
    {
        return ptpclient::tai_ns(snapshot(), master_ns);
    }

    /// Capture the current state as a trivially-copyable snapshot for
    /// cross-thread publication. Evaluating tai_ns(snapshot(), m) equals
    /// tai_ns(m) exactly.
    [[nodiscard]] auto snapshot() const noexcept -> GpsTaiSnapshot
    {
        auto const est = kalman_.estimate();
        return GpsTaiSnapshot{
            .have_sample = have_sample_,
            .valid = est.valid,
            .filtered_offset_ns = est.filtered_offset_ns,
            .r = est.r,
            .drift_ppm_per_hr = est.drift_ppm_per_hr,
            .last_master_ns = last_master_ns_,
            .tai_minus_utc_ns = cfg_.tai_minus_utc_ns,
        };
    }

    [[nodiscard]] bool has_sample() const noexcept { return have_sample_; }

    /// Filter diagnostics (ratio, uncertainty, drift) for reporting.
    [[nodiscard]] RatioEstimate estimate() const { return kalman_.estimate(); }

    void reset()
    {
        kalman_.reset();
        have_sample_ = false;
        last_master_ns_ = 0;
        last_utc_ns_ = 0;
    }

  private:
    Config cfg_{};
    KalmanRatioTracker kalman_;
    std::int64_t last_master_ns_{0};
    std::int64_t last_utc_ns_{0};
    bool have_sample_{false};
};

}  // namespace statusbar::ptpclient
