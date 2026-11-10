// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/srp/srp_mrp_timers.hpp"

namespace statusbar::srp::mrp {

TimerScheduler::TimerScheduler(uint64_t seed, TimerConfig const& config)
    : config_{config}
    , rng_{seed != 0 ? seed : std::random_device{}()}
{}

auto TimerScheduler::randomized_leaveall_duration() -> std::chrono::milliseconds
{
    std::uniform_int_distribution<int64_t> dist{config_.leave_all_time_min.count(), config_.leave_all_time_max.count() - 1};
    return std::chrono::milliseconds(dist(rng_));
}

//
// Idempotent starts — preserve existing deadline if already running.
//

void TimerScheduler::start_join(TimePoint now) noexcept
{
    if (!join_.has_value()) {
        join_ = now + config_.join_time;
    }
}

void TimerScheduler::start_leave(TimePoint now) noexcept
{
    if (!leave_.has_value()) {
        leave_ = now + config_.leave_time;
    }
}

void TimerScheduler::start_leave_at(TimePoint const deadline) noexcept
{
    if (!leave_.has_value() || deadline < *leave_) {
        leave_ = deadline;
    }
}

void TimerScheduler::start_leaveall(TimePoint now)
{
    if (!leaveall_.has_value()) {
        leaveall_ = now + randomized_leaveall_duration();
    }
}

void TimerScheduler::start_periodic(TimePoint now) noexcept
{
    if (!periodic_.has_value()) {
        periodic_ = now + config_.periodic_time;
    }
}

//
// Force rearms — always reset the deadline.
//

void TimerScheduler::arm_join(TimePoint now) noexcept
{
    join_ = now + config_.join_time;
}

void TimerScheduler::arm_leave(TimePoint now) noexcept
{
    leave_ = now + config_.leave_time;
}

void TimerScheduler::arm_leaveall(TimePoint now)
{
    leaveall_ = now + randomized_leaveall_duration();
}

void TimerScheduler::arm_periodic(TimePoint now) noexcept
{
    periodic_ = now + config_.periodic_time;
}

//
// Queries and tick.
//

auto TimerScheduler::next_deadline() const noexcept -> TimePoint
{
    auto earliest = TimePoint::max();
    auto consider = [&earliest](std::optional<TimePoint> const& t) {
        if (t.has_value() && *t < earliest) {
            earliest = *t;
        }
    };
    consider(join_);
    consider(leave_);
    consider(leaveall_);
    consider(periodic_);
    return earliest;
}

auto TimerScheduler::tick(TimePoint now) noexcept -> Expired
{
    Expired e{};
    if (join_.has_value() && *join_ <= now) {
        e.join = true;
        join_.reset();
    }
    if (leave_.has_value() && *leave_ <= now) {
        e.leave = true;
        leave_.reset();
    }
    if (leaveall_.has_value() && *leaveall_ <= now) {
        e.leaveall = true;
        leaveall_.reset();
    }
    if (periodic_.has_value() && *periodic_ <= now) {
        e.periodic = true;
        periodic_.reset();
    }
    return e;
}

}  // namespace statusbar::srp::mrp
