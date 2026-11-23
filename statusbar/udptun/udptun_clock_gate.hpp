#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Whether the session's clocks are good enough to measure and transmit on.
///
/// The session used to ask its master clock one question, "are you
/// synchronized?", and the answer went false the moment the protocol's
/// timeout fired: three missed Syncs stopped a measurement that could have
/// run on for seconds. ClockGate asks how large the worst-case error is and
/// compares it with the limit the session was configured with, so a clock
/// that keeps its rate and offset through a short loss keeps the session
/// running, and one that has coasted too long stops it.
///
/// Two clocks are judged. The master link (the gPTP slave's bridge or the
/// ptp4l PHC bridge) is what every figure the session prints depends on, and
/// its limit is enforced. The system clock matters only to a figure compared
/// across sites (the gps-tai wire clock, the realtime time source); its bound
/// is always reported, and enforced only when a limit for it is configured.
///
/// Told the time, integers only, no allocation. Owned by the packet thread,
/// which publishes report() for the reporter.

#include "statusbar/realtime/realtime_accuracy_limit.hpp"
#include "statusbar/realtime/realtime_clock.hpp"

#include <cstdint>
#include <iosfwd>

namespace statusbar::udptun {

/// The default limit on the master link's worst-case error: a tenth of a
/// millisecond, well under what a latency figure on a routed path is read to.
/// A link coasting at the default holdover figure of 15 ppm stays inside it
/// for about six and a half seconds.
inline constexpr int64_t DEFAULT_MAX_CLOCK_ERROR_NS = 100'000;

/// The session's clocks as last judged, for the report. Trivially copyable,
/// so the packet thread can publish it whole.
struct ClockReport
{
    /// A worst-case error that is not bounded.
    static constexpr int64_t UNBOUNDED = -1;

    int64_t link_accuracy_ns{UNBOUNDED};
    int64_t link_age_ns{0};  ///< since the link was last synchronized
    int64_t link_limit_ns{UNBOUNDED};
    int64_t link_outside_ns{0};  ///< time spent over the limit
    uint64_t link_excursions{0};
    uint64_t link_timeline_changes{0};
    int64_t utc_accuracy_ns{UNBOUNDED};
    int64_t utc_limit_ns{0};  ///< 0: reported, not enforced
    uint64_t utc_excursions{0};
    bool has_link{false};
    bool tracks_utc{false};
    bool usable{false};
};

class ClockGate
{
  public:
    struct Config
    {
        /// The largest worst-case error of the master link the session
        /// runs on. Negative takes any bounded link.
        int64_t max_link_error_ns{DEFAULT_MAX_CLOCK_ERROR_NS};

        /// The largest worst-case error of the system clock the session
        /// runs on. 0 reports the bound without enforcing one.
        int64_t max_utc_error_ns{0};

        /// There is a master link to judge. Without one (the realtime time
        /// source) there is nothing to wait for.
        bool has_link{false};

        /// The wire timeline is made from the system clock, so its bound
        /// belongs beside every figure compared across sites.
        bool tracks_utc{false};
    };

    ClockGate() noexcept = default;

    explicit ClockGate(Config const config) noexcept
        : config_{config}
        , link_{limit_of(config.max_link_error_ns)}
        , utc_{config.max_utc_error_ns > 0 ? realtime::Nanoseconds{config.max_utc_error_ns} : realtime::AccuracyLimit::ANY_BOUNDED}
    {}

    /// Judge both clocks at @p now.
    /// @param link The master link's quality; ignored without a link
    /// @param utc The system clock's quality; ignored unless tracked
    /// @return Whether the session may measure and transmit
    auto judge(
        realtime::ClockQuality const& link,
        realtime::ClockQuality const& utc,
        realtime::MonotonicRawClock::time_point const now) noexcept -> bool
    {
        link_verdict_ = config_.has_link ? link_.judge(link, now) : NOTHING_TO_JUDGE;
        utc_verdict_ = config_.tracks_utc ? utc_.judge(utc, now) : NOTHING_TO_JUDGE;
        link_age_ = link.age(now);
        usable_ = link_verdict_.usable && (!enforces_utc() || utc_verdict_.usable);
        return usable_;
    }

    /// The last verdict: false before the first.
    [[nodiscard]] auto usable() const noexcept -> bool { return usable_; }

    /// Whether the system clock is one of the clocks judged.
    [[nodiscard]] auto tracks_utc() const noexcept -> bool { return config_.tracks_utc; }

    /// The master link moved to a different timeline at the last verdict.
    [[nodiscard]] auto link_timeline_changed() const noexcept -> bool { return link_verdict_.timeline_changed; }

    [[nodiscard]] auto report() const noexcept -> ClockReport
    {
        return {
            .link_accuracy_ns = ns_of(link_verdict_.accuracy),
            .link_age_ns = link_age_.count(),
            .link_limit_ns = ns_of(link_.limit()),
            .link_outside_ns = link_.record().outside.count(),
            .link_excursions = link_.record().excursions,
            .link_timeline_changes = link_.record().timeline_changes,
            .utc_accuracy_ns = ns_of(utc_verdict_.accuracy),
            .utc_limit_ns = config_.max_utc_error_ns > 0 ? config_.max_utc_error_ns : 0,
            .utc_excursions = enforces_utc() ? utc_.record().excursions : 0,
            .has_link = config_.has_link,
            .tracks_utc = config_.tracks_utc,
            .usable = usable_};
    }

  private:
    /// The verdict on a clock the session does not depend on.
    static constexpr realtime::AccuracyVerdict NOTHING_TO_JUDGE{
        .usable = true, .timeline_changed = false, .accuracy = realtime::Nanoseconds::zero()};

    [[nodiscard]] static constexpr auto limit_of(int64_t const max_error_ns) noexcept -> realtime::Nanoseconds
    {
        return max_error_ns < 0 ? realtime::AccuracyLimit::ANY_BOUNDED : realtime::Nanoseconds{max_error_ns};
    }

    [[nodiscard]] static constexpr auto ns_of(realtime::Nanoseconds const accuracy) noexcept -> int64_t
    {
        return accuracy == realtime::UNBOUNDED_ACCURACY ? ClockReport::UNBOUNDED : accuracy.count();
    }

    [[nodiscard]] auto enforces_utc() const noexcept -> bool { return config_.tracks_utc && config_.max_utc_error_ns > 0; }

    Config config_{};
    realtime::AccuracyLimit link_{};
    realtime::AccuracyLimit utc_{};
    realtime::AccuracyVerdict link_verdict_{};
    realtime::AccuracyVerdict utc_verdict_{};
    realtime::Nanoseconds link_age_{0};
    bool usable_{false};
};

/// One line for the live report and the final summary: each judged clock's
/// worst-case error, the link's limit and how it has kept to it. Prints
/// nothing when the session judges no clock.
void print_clock_report(std::ostream& out, ClockReport const& report);

}  // namespace statusbar::udptun
