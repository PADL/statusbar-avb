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
///
/// The system clock is only worth following while it knows the time. The
/// caller judges it (the operating system's own worst-case error, coasted
/// through a loss of the time server) against two limits of the entity's and
/// tells observe() both verdicts with each pair:
///   - the limit for following its rate. This one is loose: a rate is as good
///     from a clock that is 30 ms off as from one that is exact, so the limit
///     only has to tell a disciplined clock from one that never was or has
///     been left alone for hours. Outside it no pair is fed and r holds the
///     value it had, so the media clock coasts on the last ratio it trusted.
///   - the tighter limit for vouching for a TAI timestamp made from it, which
///     the tunnel's talker puts on the wire as the tu bit.
/// A step of the system clock is a different timeline; the filters start over
/// on it and r again holds until they have an estimate.
///
/// The other clock of each pair is the media timer's: gPTP as the timer's
/// clock link (the PHC bridge) maps it. LinkWatch holds what the entity is
/// told of that link. Its worst-case error is added to the system clock's
/// before either limit is applied, since a TAI timestamp is made from both,
/// and a change of its timeline restarts the filters as a step of the system
/// clock does.

#include "statusbar/ptpclient/ptpclient_freq_ratio.hpp"      // KalmanRatioTracker, RatioEstimate
#include "statusbar/ptpclient/ptpclient_tai_translator.hpp"  // GpsTaiTranslator
#include "statusbar/realtime/realtime_accuracy_limit.hpp"    // AccuracyVerdict
#include "statusbar/realtime/realtime_clock.hpp"             // ClockQuality

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace statusbar::avb_entity {

/// The entity's two questions about the system clock at one reading.
struct SystemClockVerdict
{
    /// Against the limit for following the clock's rate and training the TAI
    /// mapping from it.
    realtime::AccuracyVerdict follow{};

    /// Against the tighter limit for vouching for a TAI timestamp made from it.
    bool tai_usable{false};
};

/// The media timer's clock link as the entity was last told it. Told the
/// quality, told the time, single-threaded (the media thread).
class LinkWatch
{
  public:
    /// The link's quality now, from whoever owns the link.
    void tell(realtime::ClockQuality const& link) noexcept
    {
        changed_ = changed_ || (told_ && link.epoch != quality_.epoch);
        quality_ = link;
        told_ = true;
    }

    /// The link's worst-case error at @p now. Zero until a link has been
    /// told: an entity that is told of none has none to account for.
    [[nodiscard]] auto accuracy(realtime::MonotonicRawClock::time_point const now) const noexcept -> realtime::Nanoseconds
    {
        return told_ ? quality_.uncertainty(now) : realtime::Nanoseconds::zero();
    }

    /// Whether the link moved to another timeline since this was last
    /// asked. True once for each run of changes.
    [[nodiscard]] auto take_timeline_change() noexcept -> bool { return std::exchange(changed_, false); }

  private:
    realtime::ClockQuality quality_{};
    bool told_{false};
    bool changed_{false};
};

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
        last_attempt_ns_ = 0;
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
    [[nodiscard]] auto tai_snapshot() const noexcept -> ptpclient::GpsTaiSnapshot
    {
        auto snapshot = tai_.snapshot();
        snapshot.tai_usable = tai_usable_;
        return snapshot;
    }

    /// The last verdict told to observe(): whether a TAI timestamp made from
    /// the system clock can be vouched for. False until the first.
    [[nodiscard]] bool tai_usable() const noexcept { return tai_usable_; }

    /// Rate-limit the GPS sampling to at most once per SAMPLE_INTERVAL so the
    /// CLOCK_REALTIME syscall stays off the per-packet hot path.
    [[nodiscard]] bool should_sample(std::uint64_t gptp_now_ns) const noexcept
    {
        std::uint64_t const last_read_ns = std::max(last_sample_ns_, last_attempt_ns_);
        return last_read_ns == 0 || (gptp_now_ns - last_read_ns) >= SAMPLE_INTERVAL_NS;
    }

    /// One paired reading together with the caller's verdicts on the system
    /// clock it was read from. The pair is fed only when the clock is inside
    /// the limit for following it and @p follow is set; otherwise r and the
    /// TAI mapping stay as they are. A change of the system clock's timeline
    /// restarts the filters.
    /// @param follow False when the media clock is pinned to gPTP: the system
    ///        clock is still judged, for the tunnel's timestamps, but not followed
    /// @return The estimate when the pair was fed
    auto observe(std::uint64_t gptp_now_ns, std::uint64_t utc_now_ns, SystemClockVerdict const& clock, bool follow = true)
        -> std::optional<ptpclient::RatioEstimate>
    {
        if (clock.follow.timeline_changed) {
            restart();
        }
        tai_usable_ = clock.tai_usable;
        if (!clock.follow.usable || !follow) {
            last_attempt_ns_ = gptp_now_ns;
            return std::nullopt;
        }
        return add_sample(gptp_now_ns, utc_now_ns);
    }

    /// Start the filters over, as on a new timeline of the system clock. r
    /// keeps its value until the filters have a new estimate, so the media
    /// clock does not jump back to the uncorrected rate.
    void restart()
    {
        ratio_.reset();
        tai_.reset();
        last_sample_ns_ = 0;
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

    /// Times either filter (the ratio's, the TAI mapping's) found its samples
    /// persistently outside its gate and widened itself to follow them,
    /// since the filters were last started. Not zero means one of the two
    /// clocks moved in a way the filters had not allowed for.
    [[nodiscard]] auto self_corrections() const noexcept -> std::uint64_t
    {
        return ratio_.self_corrections() + tai_.self_corrections();
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
    std::uint64_t last_sample_ns_{0};   ///< the last pair fed
    std::uint64_t last_attempt_ns_{0};  ///< the last pair read and not fed
    std::uint64_t last_log_ns_{0};
    bool tai_usable_{false};
};

}  // namespace statusbar::avb_entity
