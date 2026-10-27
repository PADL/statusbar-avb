// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ptpclient/ptpclient_phc_servo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <system_error>

namespace statusbar::ptpclient {

namespace {

constexpr double NS_PER_SECOND = 1e9;
constexpr double FREQ_EST_MARGIN = 0.001;  ///< linuxptp pi.c
constexpr double FREQ_EST_KI = 0.016;      ///< estimate over at least 0.016 / ki seconds
constexpr double FREQ_EST_MAX_S = 1000.0;

}  // namespace

PhcServo::PhcServo(PhcServoParams const& params, double const initial_frequency_ppb) noexcept
    : params_{params}
    , frequency_ppb_{initial_frequency_ppb}
{
    double const interval = static_cast<double>(params.interval_ns) / NS_PER_SECOND;
    kp_ = std::min(params.kp_scale * std::pow(interval, params.kp_exponent), params.kp_norm_max / interval);
    ki_ = std::min(params.ki_scale * std::pow(interval, params.ki_exponent), params.ki_norm_max / interval);
    frequency_ppb_ = clamp(initial_frequency_ppb);
}

auto PhcServo::create(PhcServoParams const& params, double const initial_frequency_ppb) -> StatusValue<PhcServo>
{
    if (params.interval_ns <= 0 || !(params.max_frequency_ppb > 0.0) || params.first_step_threshold_ns < 0 ||
        params.lock_threshold_ns <= 0 || params.unlock_threshold_ns < params.lock_threshold_ns || params.lock_count < 1 ||
        params.unlock_count < 1 || !(params.kp_scale > 0.0) || !(params.ki_scale > 0.0) || !(params.kp_norm_max > 0.0) ||
        !(params.ki_norm_max > 0.0) || !std::isfinite(initial_frequency_ppb)) {
        return failure(std::errc::invalid_argument);
    }
    return PhcServo{params, initial_frequency_ppb};
}

auto PhcServo::clamp(double const ppb) const noexcept -> double
{
    return std::clamp(ppb, -params_.max_frequency_ppb, params_.max_frequency_ppb);
}

auto PhcServo::state() const noexcept -> PhcServoState
{
    if (frozen_) {
        return PhcServoState::frozen;
    }
    return locked_ ? PhcServoState::locked : PhcServoState::unlocked;
}

void PhcServo::track_lock(int64_t const error_ns) noexcept
{
    int64_t const magnitude = std::abs(error_ns);
    if (!locked_) {
        good_ = magnitude < params_.lock_threshold_ns ? good_ + 1 : 0;
        if (good_ >= params_.lock_count) {
            locked_ = true;
            ever_locked_ = true;
            bad_ = 0;
        }
        return;
    }
    bad_ = magnitude > params_.unlock_threshold_ns ? bad_ + 1 : 0;
    if (bad_ >= params_.unlock_count) {
        locked_ = false;
        good_ = 0;
    }
}

auto PhcServo::sample(int64_t const error_ns, int64_t const local_ns, double const feed_forward_ppb) noexcept -> PhcServoAction
{
    PhcServoAction const hold{.frequency_ppb = frequency_ppb_};
    if (frozen_) {
        return hold;
    }
    if (count_ == 0 || (count_ == 1 && local_ns <= first_local_ns_)) {
        first_error_ns_ = error_ns;
        first_local_ns_ = local_ns;
        count_ = 1;
        return hold;
    }
    if (count_ == 1) {
        return estimate_drift(error_ns, local_ns, feed_forward_ppb);
    }
    regulate(error_ns, feed_forward_ppb);
    return PhcServoAction{.frequency_ppb = frequency_ppb_};
}

auto PhcServo::estimate_drift(int64_t const error_ns, int64_t const local_ns, double const feed_forward_ppb) noexcept
    -> PhcServoAction
{
    auto const elapsed = static_cast<double>(local_ns - first_local_ns_);
    double const wait_s = std::min(FREQ_EST_KI / ki_, FREQ_EST_MAX_S);
    if (elapsed * (1.0 + FREQ_EST_MARGIN) / NS_PER_SECOND < wait_s) {
        return PhcServoAction{.frequency_ppb = frequency_ppb_};
    }
    cancel_drift(error_ns, elapsed, feed_forward_ppb);
    bool const step = !ever_locked_ && params_.first_step_threshold_ns > 0 && std::abs(error_ns) > params_.first_step_threshold_ns;
    return PhcServoAction{.frequency_ppb = frequency_ppb_, .step_ns = step ? std::optional<int64_t>{-error_ns} : std::nullopt};
}

void PhcServo::cancel_drift(int64_t const error_ns, double const elapsed, double const feed_forward_ppb) noexcept
{
    // The PHC gains `rate` ns per ns at frequency_ppb_; cancel it (pi.c's drift estimate).
    double const rate = static_cast<double>(error_ns - first_error_ns_) / elapsed;
    double const total = clamp(frequency_ppb_ - ((NS_PER_SECOND + frequency_ppb_) * rate));
    drift_ppb_ = total - feed_forward_ppb;
    frequency_ppb_ = total;
    count_ = 2;
}

void PhcServo::regulate(int64_t const error_ns, double const feed_forward_ppb) noexcept
{
    auto const error = static_cast<double>(error_ns);
    double const ki_term = ki_ * error;
    double const ppb = feed_forward_ppb + drift_ppb_ - (kp_ * error) - ki_term;
    frequency_ppb_ = clamp(ppb);
    if (std::abs(ppb) <= params_.max_frequency_ppb) {
        drift_ppb_ -= ki_term;  // no wind-up while clamped
    }
    track_lock(error_ns);
}

void PhcServo::freeze() noexcept
{
    frozen_ = true;
    locked_ = false;
    good_ = 0;
    bad_ = 0;
}

void PhcServo::thaw() noexcept
{
    if (frozen_) {
        frozen_ = false;
        count_ = 0;
    }
}

void PhcServo::reset() noexcept
{
    frozen_ = false;
    locked_ = false;
    ever_locked_ = false;
    good_ = 0;
    bad_ = 0;
    count_ = 0;
}

auto apply(PhcDevice const& device, PhcServoAction const& action) noexcept -> Status
{
    if (auto status = device.set_frequency_ppb(action.frequency_ppb); !status) {
        return status;
    }
    if (action.step_ns) {
        return device.step_ns(*action.step_ns);
    }
    return success();
}

}  // namespace statusbar::ptpclient
