// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ptpclient/ptpclient_phc_follower.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

namespace statusbar::ptpclient {

namespace {

/// A PhcCrossTimestamp pair whose followed PHC (PHC_d) is open writable, adjusted through it.
class DevicePort final : public PhcFollowerPort
{
  public:
    explicit DevicePort(PhcCrossTimestamp cross) noexcept
        : cross_{std::move(cross)}
    {}
    [[nodiscard]] auto measure() noexcept -> StatusValue<PhcCrossSample> override { return cross_.measure(); }
    [[nodiscard]] auto set_frequency_ppb(double const ppb) noexcept -> Status override
    {
        return cross_.phcd().set_frequency_ppb(ppb);
    }
    [[nodiscard]] auto step_ns(int64_t const ns) noexcept -> Status override { return cross_.phcd().step_ns(ns); }
    [[nodiscard]] auto frequency_ppb() const noexcept -> StatusValue<double> override { return cross_.phcd().frequency_ppb(); }
    [[nodiscard]] auto max_frequency_ppb() const noexcept -> StatusValue<double> override
    {
        return cross_.phcd().max_frequency_ppb();
    }

  private:
    PhcCrossTimestamp cross_;
};

}  // namespace

auto open_phc_follower_port(std::string_view const phc0_path, std::string_view const followed_path, PhcDeviceConfig const config)
    -> StatusValue<std::unique_ptr<PhcFollowerPort>>
{
    return PhcCrossTimestamp::open(phc0_path, followed_path, config, PhcCrossAccess::phcd_writable)
        .transform([](PhcCrossTimestamp&& cross) {
            return std::unique_ptr<PhcFollowerPort>{std::make_unique<DevicePort>(std::move(cross))};
        });
}

PhcFollower::PhcFollower(PhcFollowerParams const& params, std::unique_ptr<PhcFollowerPort> port, PhcServo const& servo) noexcept
    : params_{params}
    , port_{std::move(port)}
    , servo_{servo}
{}

namespace {

/// The servo for `port`: `params` clamped to the port's range (written back, so the line's rate is checked against
/// the clamp in force), starting from its frequency; the port's error else.
[[nodiscard]] auto servo_for(PhcServoParams& params, PhcFollowerPort const& port) -> StatusValue<PhcServo>
{
    auto const range = port.max_frequency_ppb();
    auto const frequency = range ? port.frequency_ppb() : StatusValue<double>{forward_failure(range)};
    if (!frequency) {
        return forward_failure(frequency);
    }
    params.max_frequency_ppb = std::min(params.max_frequency_ppb, *range);
    return PhcServo::create(params, *frequency);
}

/// Whether the restep rule of `params` can settle: off, or past both the servo's first step and unlock thresholds, so a
/// restart steps what it restarts for and jitter that only unlocks never restarts.
[[nodiscard]] auto restep_settles(PhcFollowerParams const& params) noexcept -> bool
{
    auto const& servo = params.servo;
    bool const past = params.restep_threshold_ns >= std::max(servo.first_step_threshold_ns, servo.unlock_threshold_ns);
    return params.restep_threshold_ns == 0 || (servo.first_step_threshold_ns > 0 && past);
}

}  // namespace

auto PhcFollower::create(PhcFollowerParams const& params, std::unique_ptr<PhcFollowerPort> port) -> StatusValue<PhcFollower>
{
    if (port == nullptr || params.restep_threshold_ns < 0 || params.restep_count < 1 || !restep_settles(params)) {
        return failure(std::errc::invalid_argument);
    }
    auto clamped = params;
    return servo_for(clamped.servo, *port).transform([&](PhcServo const& servo) {
        return PhcFollower{clamped, std::move(port), servo};
    });
}

void PhcFollower::freeze() noexcept
{
    servo_.freeze();
    over_ = 0;
}

void PhcFollower::thaw() noexcept
{
    if (servo_.state() == PhcServoState::frozen) {
        servo_.thaw();
        over_ = 0;  // not a thaw of a follower that was not frozen: a restep count goes on
    }
}

void PhcFollower::reset() noexcept
{
    servo_.reset();
    over_ = 0;
    refused_steps_ = 0;  // the caller's start over: a step is tried again, and the restep rule is back on
}

auto PhcFollower::state() const noexcept -> PhcServoState
{
    return over_ > 0 ? PhcServoState::unlocked : servo_.state();
}

auto PhcFollower::held(int64_t const error_ns) noexcept -> Restep
{
    bool const watched = params_.restep_threshold_ns != 0 && servo_.estimated() && refused_steps_ < 2;
    over_ = watched && std::abs(error_ns) > params_.restep_threshold_ns ? over_ + 1 : 0;
    return Restep{.hold = over_ > 0 && over_ < params_.restep_count, .restart = over_ >= params_.restep_count};
}

auto PhcFollower::sample(PhcLine const& line) noexcept -> PhcFollowerSample
{
    if (!std::isfinite(line.rate_offset_ppb) || std::abs(line.rate_offset_ppb) >= params_.servo.max_frequency_ppb) {
        return PhcFollowerSample{
            .state = state(), .failed = PhcFollowerStage::line, .error = std::make_error_code(std::errc::invalid_argument)};
    }
    auto const pair = port_->measure();
    if (!pair) {
        return PhcFollowerSample{.state = state(), .failed = PhcFollowerStage::measure, .error = pair.error()};
    }
    PhcFollowerSample const measured{.pair = *pair, .error_ns = pair->phcd_ns - line.at(pair->phc0_ns)};
    if (servo_.state() == PhcServoState::frozen) {
        return PhcFollowerSample{.pair = measured.pair, .error_ns = measured.error_ns, .state = PhcServoState::frozen};
    }
    return steer(measured, line);
}

auto PhcFollower::steer(PhcFollowerSample sample, PhcLine const& line) noexcept -> PhcFollowerSample
{
    Undo const undo{.servo = servo_, .over = over_};
    auto const restep = held(sample.error_ns);
    if (restep.restart) {
        reset();  // the restep rule: a jump PHC0 made, stepped by the servo once it has estimated again
    }
    sample.restarted = restep.restart;
    sample.state = state();
    return restep.hold ? sample : apply(servo_.sample(sample.error_ns, sample.pair.raw_ns, line.rate_offset_ppb), sample, undo);
}

auto PhcFollower::apply(PhcServoAction const& action, PhcFollowerSample sample, Undo const& undo) noexcept -> PhcFollowerSample
{
    if (auto const set = port_->set_frequency_ppb(action.frequency_ppb); !set) {
        servo_ = undo.servo;  // nothing applied: as before the sample, a restep still owed
        over_ = undo.over;
        return PhcFollowerSample{
            .pair = sample.pair,
            .error_ns = sample.error_ns,
            .state = state(),
            .failed = PhcFollowerStage::frequency,
            .error = set.error()};
    }
    sample.steered = true;
    if (action.step_ns) {
        auto const stepped = port_->step_ns(*action.step_ns);
        sample.stepped = stepped.has_value();
        refused_steps_ = stepped ? 0 : refused_steps_ + 1;
        if (!stepped) {
            refuse_step(sample, stepped.error());
        }
    }
    sample.state = state();
    return sample;
}

void PhcFollower::refuse_step(PhcFollowerSample& sample, std::error_code const error) noexcept
{
    sample.failed = PhcFollowerStage::step;
    sample.error = error;
    if (refused_steps_ == 1) {
        servo_.reset();  // not reset(): the count of refusals stands
        over_ = 0;       // its frequency is the device's; it estimates again, so the step is owed again, once
    }
}

}  // namespace statusbar::ptpclient
