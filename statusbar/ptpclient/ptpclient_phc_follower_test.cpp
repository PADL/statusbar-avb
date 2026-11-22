// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ptpclient/ptpclient_phc_follower.hpp"

#include "statusbar/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

using namespace statusbar;
using namespace statusbar::ptpclient;

namespace {

constexpr int64_t INTERVAL_NS = 62'500'000;

/// What the fake pair's PHCs do: PHC0 runs at local time plus an epoch; the followed PHC runs at its own oscillator
/// error, adjusted by the frequency set and stepped by the steps applied. They start 1000 s into their timescale, where
/// a double still holds a fraction of a nanosecond.
struct Clocks
{
    double phc0_ns{1'000'000'000'000.0};
    double followed_ns{1'000'000'000'000.0};
    double natural_ppb{0.0};
    double frequency_ppb{0.0};
    double max_ppb{10'000'000.0};
    int64_t local_ns{0};  ///< CLOCK_MONOTONIC_RAW, as the pair reports it
    int steps{0};
    int steers{0};
    bool fail_next_measure{false};
    bool refuse_frequency{false};  ///< every frequency refused while set
    bool refuse_next_step{false};  ///< the next step refused, its frequency applied
    bool refuse_steps{false};      ///< every step refused (a device without ADJ_SETOFFSET)

    /// One interval of local time.
    void advance()
    {
        local_ns += INTERVAL_NS;
        auto const dt = static_cast<double>(INTERVAL_NS);
        phc0_ns += dt;
        followed_ns += dt * (1.0 + (natural_ppb * 1e-9)) * (1.0 + (frequency_ppb * 1e-9));
    }
};

/// A PhcFollowerPort over Clocks, as a device pair behaves.
class FakePort final : public PhcFollowerPort
{
  public:
    explicit FakePort(Clocks& clocks)
        : clocks_{clocks}
    {}
    [[nodiscard]] auto measure() noexcept -> StatusValue<PhcCrossSample> override
    {
        if (std::exchange(clocks_.fail_next_measure, false)) {
            return failure(std::errc::timed_out);
        }
        return PhcCrossSample{
            .phc0_ns = std::llround(clocks_.phc0_ns), .phcd_ns = std::llround(clocks_.followed_ns), .raw_ns = clocks_.local_ns};
    }
    [[nodiscard]] auto set_frequency_ppb(double const ppb) noexcept -> Status override
    {
        if (clocks_.refuse_frequency) {
            return failure(std::errc::operation_not_permitted);
        }
        ++clocks_.steers;
        clocks_.frequency_ppb = std::clamp(ppb, -clocks_.max_ppb, clocks_.max_ppb);
        return success();
    }
    [[nodiscard]] auto step_ns(int64_t const ns) noexcept -> Status override
    {
        if (std::exchange(clocks_.refuse_next_step, false) || clocks_.refuse_steps) {
            return failure(std::errc::device_or_resource_busy);
        }
        clocks_.followed_ns += static_cast<double>(ns);
        ++clocks_.steps;
        return success();
    }
    [[nodiscard]] auto frequency_ppb() const noexcept -> StatusValue<double> override { return clocks_.frequency_ppb; }
    [[nodiscard]] auto max_frequency_ppb() const noexcept -> StatusValue<double> override { return clocks_.max_ppb; }

  private:
    Clocks& clocks_;
};

auto make_follower(Clocks& clocks, PhcFollowerParams const& params = {}) -> PhcFollower
{
    auto follower = PhcFollower::create(params, std::make_unique<FakePort>(clocks));
    EXPECT_TRUE(follower.has_value());
    return std::move(follower).value();  // a failed create aborts here (no exceptions in this tree)
}

/// `n` samples onto `line`, one an interval; the last sample.
auto run(PhcFollower& follower, Clocks& clocks, PhcLine const& line, int const n) -> PhcFollowerSample
{
    PhcFollowerSample last{};
    for (int i = 0; i < n; ++i) {
        last = follower.sample(line);
        EXPECT_TRUE(last.ok());
        clocks.advance();
    }
    return last;
}

/// The followed PHC less `line` now.
auto error_now(Clocks const& clocks, PhcLine const& line) -> int64_t
{
    return std::llround(clocks.followed_ns) - line.at(std::llround(clocks.phc0_ns));
}

}  // namespace

TEST(PhcLine, a_line_maps_phc0_time_at_its_rate)
{
    PhcLine const line{.anchor_ns = 1'000'000'000, .target_ns = 5'000'000'000, .rate_offset_ppb = 100'000.0};
    EXPECT_EQ(line.at(1'000'000'000), int64_t{5'000'000'000});
    EXPECT_EQ(line.at(2'000'000'000), int64_t{6'000'100'000});  // 100 ppm fast: 100 us a second
    EXPECT_EQ(line.at(0), int64_t{3'999'900'000});
    auto const offset = PhcLine::offset_from_phc0(-250);
    EXPECT_EQ(offset.at(1'700'000'000'000'000'000), int64_t{1'700'000'000'000'000'000} - 250);
}

TEST(PhcFollower, create_refuses_bad_parameters_and_a_null_port)
{
    Clocks clocks;
    EXPECT_FALSE(PhcFollower::create({}, nullptr).has_value());
    EXPECT_FALSE(PhcFollower::create({.restep_threshold_ns = -1}, std::make_unique<FakePort>(clocks)).has_value());
    EXPECT_FALSE(PhcFollower::create({.restep_count = 0}, std::make_unique<FakePort>(clocks)).has_value());
    EXPECT_FALSE(PhcFollower::create({.servo = {.interval_ns = 0}}, std::make_unique<FakePort>(clocks)).has_value());
    // A restep rule that cannot settle: under the first step threshold, under the unlock threshold, or no first step.
    EXPECT_FALSE(PhcFollower::create({.restep_threshold_ns = 10'000}, std::make_unique<FakePort>(clocks)).has_value());
    PhcServoParams const no_first_step{.first_step_threshold_ns = 0};
    EXPECT_FALSE(PhcFollower::create({.servo = no_first_step, .restep_threshold_ns = 50'000}, std::make_unique<FakePort>(clocks))
                     .has_value());
    PhcServoParams const high_unlock{.unlock_threshold_ns = 40'000};
    EXPECT_FALSE(
        PhcFollower::create({.servo = high_unlock, .restep_threshold_ns = 30'000}, std::make_unique<FakePort>(clocks)).has_value());
    EXPECT_TRUE(PhcFollower::create({.restep_threshold_ns = 20'000}, std::make_unique<FakePort>(clocks)).has_value());
}

TEST(PhcFollower, a_follower_steps_once_then_slews)
{
    Clocks clocks{.natural_ppb = 30'000.0};
    clocks.followed_ns += 2'000'000.0;  // 2 ms ahead: past the first step threshold
    auto follower = make_follower(clocks);
    auto const line = PhcLine::offset_from_phc0(0);
    auto const last = run(follower, clocks, line, 160);
    EXPECT_EQ(clocks.steps, 1);
    EXPECT_TRUE(last.state == PhcServoState::locked);
    EXPECT_TRUE(std::abs(error_now(clocks, line)) < 100);
    // A later 30 us error is slewed, not stepped (no restep rule by default).
    clocks.followed_ns += 30'000.0;
    (void)run(follower, clocks, line, 160);
    EXPECT_EQ(clocks.steps, 1);
    EXPECT_TRUE(std::abs(error_now(clocks, line)) < 100);
}

TEST(PhcFollower, a_follower_tracks_a_line_with_its_rate_fed_forward)
{
    Clocks clocks{.natural_ppb = -12'000.0};
    auto follower = make_follower(clocks);
    PhcLine const line{
        .anchor_ns = std::llround(clocks.phc0_ns),
        .target_ns = std::llround(clocks.followed_ns) + 3'000,
        .rate_offset_ppb = 50'000.0};
    auto const last = run(follower, clocks, line, 320);
    EXPECT_TRUE(last.state == PhcServoState::locked);
    EXPECT_TRUE(std::abs(error_now(clocks, line)) < 100);
    EXPECT_EQ(clocks.steps, 0);  // 3 us: under the first step threshold
    // (1 - 12 ppm)(1 + f) = 1 + 50 ppm
    EXPECT_TRUE(std::abs(clocks.frequency_ppb - ((1.00005 / 0.999988 - 1.0) * 1e9)) < 200.0);
}

TEST(PhcFollower, a_restep_after_lock_waits_for_its_count)
{
    Clocks clocks{.natural_ppb = 5'000.0};
    auto follower = make_follower(clocks, {.restep_threshold_ns = 20'000, .restep_count = 3});
    auto const line = PhcLine::offset_from_phc0(0);
    (void)run(follower, clocks, line, 160);
    EXPECT_TRUE(follower.state() == PhcServoState::locked);
    EXPECT_EQ(clocks.steps, 0);
    // PHC0's timescale jumps by 5 ms: two samples over the threshold are held (no steering, no wind-up), the
    // third restarts the servo.
    clocks.phc0_ns += 5'000'000.0;
    double const frequency = clocks.frequency_ppb;
    int restarts = 0;
    for (int i = 0; i < 3; ++i) {
        auto const sample = follower.sample(line);
        EXPECT_TRUE(sample.ok());
        restarts += sample.restarted ? 1 : 0;
        EXPECT_EQ(sample.restarted, i == 2);
        EXPECT_EQ(sample.steered, i == 2);
        if (i < 2) {
            EXPECT_EQ(clocks.frequency_ppb, frequency);
            EXPECT_TRUE(sample.state == PhcServoState::unlocked);  // a held sample is not reported locked
            EXPECT_TRUE(follower.state() == PhcServoState::unlocked);
        }
        clocks.advance();
    }
    EXPECT_EQ(restarts, 1);
    // Started over, the servo steps once more and locks again.
    auto const last = run(follower, clocks, line, 160);
    EXPECT_EQ(clocks.steps, 1);
    EXPECT_TRUE(last.state == PhcServoState::locked);
    EXPECT_TRUE(std::abs(error_now(clocks, line)) < 100);
}

TEST(PhcFollower, a_failed_measure_changes_nothing)
{
    Clocks clocks{.natural_ppb = 1'000.0};
    auto follower = make_follower(clocks);
    auto const line = PhcLine::offset_from_phc0(0);
    (void)run(follower, clocks, line, 40);
    int const steers = clocks.steers;
    double const frequency = clocks.frequency_ppb;
    clocks.fail_next_measure = true;
    auto const failed = follower.sample(line);
    EXPECT_TRUE(failed.failed == PhcFollowerStage::measure);
    EXPECT_TRUE(failed.error == std::make_error_code(std::errc::timed_out));
    EXPECT_EQ(clocks.steers, steers);
    EXPECT_EQ(clocks.frequency_ppb, frequency);
    EXPECT_TRUE(follower.sample(line).ok());  // the next one works
    EXPECT_EQ(clocks.steers, steers + 1);
}

TEST(PhcFollower, a_frozen_follower_measures_but_steers_nothing_until_thawed)
{
    Clocks clocks{.natural_ppb = 4'000.0};
    auto follower = make_follower(clocks);
    auto const line = PhcLine::offset_from_phc0(0);
    (void)run(follower, clocks, line, 160);
    EXPECT_TRUE(follower.state() == PhcServoState::locked);
    follower.freeze();
    follower.freeze();  // twice is once
    int const steers = clocks.steers;
    double const frequency = clocks.frequency_ppb;
    clocks.followed_ns += 10'000.0;
    auto const frozen = follower.sample(line);
    EXPECT_TRUE(frozen.ok());
    EXPECT_TRUE(frozen.state == PhcServoState::frozen);
    EXPECT_FALSE(frozen.steered);
    EXPECT_TRUE(frozen.error_ns > 9'000);
    EXPECT_EQ(frozen.pair.phcd_ns, std::llround(clocks.followed_ns));  // the pair it measured
    EXPECT_EQ(clocks.steers, steers);
    EXPECT_EQ(clocks.frequency_ppb, frequency);
    // Thawed, it slews the 10 us back without a step (it has locked before) and locks again.
    follower.thaw();
    auto const last = run(follower, clocks, line, 240);
    EXPECT_TRUE(last.steered);
    EXPECT_TRUE(last.state == PhcServoState::locked);
    EXPECT_EQ(clocks.steps, 0);
    EXPECT_TRUE(std::abs(error_now(clocks, line)) < 100);
}

TEST(PhcFollower, a_refused_frequency_is_reported_and_its_step_still_owed)
{
    Clocks clocks;
    clocks.followed_ns += 2'000'000.0;
    auto follower = make_follower(clocks);
    auto const line = PhcLine::offset_from_phc0(0);
    // Refuse every frequency for a while: nothing is applied, and the step the servo wants is never lost.
    clocks.refuse_frequency = true;
    for (int i = 0; i < 40; ++i) {
        auto const refused = follower.sample(line);
        if (!refused.ok()) {
            EXPECT_TRUE(refused.failed == PhcFollowerStage::frequency);
            EXPECT_TRUE(refused.error == std::make_error_code(std::errc::operation_not_permitted));
        }
        clocks.advance();
    }
    EXPECT_EQ(clocks.steps, 0);
    clocks.refuse_frequency = false;
    (void)run(follower, clocks, line, 160);
    EXPECT_EQ(clocks.steps, 1);
    EXPECT_TRUE(std::abs(error_now(clocks, line)) < 100);
}

TEST(PhcFollower, a_refused_step_keeps_device_and_servo_agreed_and_steps_later)
{
    Clocks clocks{.natural_ppb = 20'000.0};
    clocks.followed_ns += 2'000'000.0;
    auto follower = make_follower(clocks);
    auto const line = PhcLine::offset_from_phc0(0);
    clocks.refuse_next_step = true;
    bool refused = false;
    for (int i = 0; i < 40 && !refused; ++i) {
        auto const sample = follower.sample(line);
        refused = sample.failed == PhcFollowerStage::step;
        if (refused) {
            EXPECT_TRUE(sample.steered);  // the frequency went through
            EXPECT_FALSE(sample.stepped);
            EXPECT_TRUE(sample.error == std::make_error_code(std::errc::device_or_resource_busy));
            EXPECT_EQ(clocks.frequency_ppb, follower.servo().frequency_ppb());
        }
        clocks.advance();
    }
    EXPECT_TRUE(refused);
    EXPECT_EQ(clocks.steps, 0);
    (void)run(follower, clocks, line, 200);
    EXPECT_EQ(clocks.steps, 1);  // owed, and made once the servo estimates again
    EXPECT_TRUE(std::abs(error_now(clocks, line)) < 100);
}

TEST(PhcFollower, a_jump_before_the_first_lock_is_restepped)
{
    Clocks clocks{.natural_ppb = 3'000.0};
    auto follower = make_follower(clocks, {.restep_threshold_ns = 20'000, .restep_count = 3});
    auto const line = PhcLine::offset_from_phc0(0);
    while (!follower.servo().estimated()) {
        (void)follower.sample(line);
        clocks.advance();
    }
    EXPECT_FALSE(follower.servo().state() == PhcServoState::locked);
    EXPECT_EQ(clocks.steps, 0);
    clocks.phc0_ns -= 3'000'000.0;  // the followed PHC is now 3 ms ahead, before any lock
    auto const last = run(follower, clocks, line, 200);
    EXPECT_EQ(clocks.steps, 1);
    EXPECT_TRUE(last.state == PhcServoState::locked);
    EXPECT_TRUE(std::abs(error_now(clocks, line)) < 100);
}

TEST(PhcFollower, a_line_that_resumed_elsewhere_after_a_freeze_is_restepped)
{
    Clocks clocks{.natural_ppb = 2'000.0};
    auto follower = make_follower(clocks, {.restep_threshold_ns = 20'000, .restep_count = 3});
    auto const line = PhcLine::offset_from_phc0(0);
    (void)run(follower, clocks, line, 160);
    follower.freeze();
    clocks.phc0_ns += 7'000'000.0;  // the line resumes 7 ms on
    follower.thaw();
    auto const last = run(follower, clocks, line, 240);
    EXPECT_EQ(clocks.steps, 1);  // re-estimated first, then the restep rule
    EXPECT_TRUE(last.state == PhcServoState::locked);
    EXPECT_TRUE(std::abs(error_now(clocks, line)) < 100);
}

TEST(PhcFollower, a_device_that_refuses_every_step_is_slewed)
{
    Clocks clocks{.natural_ppb = 5'000.0};
    clocks.followed_ns += 2'000'000.0;  // 2 ms: the servo wants a step it never gets
    clocks.refuse_steps = true;
    auto follower = make_follower(clocks, {.restep_threshold_ns = 20'000, .restep_count = 3});
    auto const line = PhcLine::offset_from_phc0(0);
    int refused = 0;
    for (int i = 0; i < 1600; ++i) {  // 100 s
        refused += follower.sample(line).failed == PhcFollowerStage::step ? 1 : 0;
        clocks.advance();
    }
    EXPECT_EQ(refused, 2);  // asked twice, then slewed with the restep rule off
    EXPECT_EQ(clocks.steps, 0);
    EXPECT_TRUE(std::abs(error_now(clocks, line)) < 100);
    EXPECT_TRUE(follower.state() == PhcServoState::locked);
}

TEST(PhcFollower, a_reset_after_refused_steps_turns_the_restep_rule_back_on)
{
    Clocks clocks{.natural_ppb = 5'000.0};
    clocks.followed_ns += 2'000'000.0;
    clocks.refuse_steps = true;
    auto follower = make_follower(clocks, {.restep_threshold_ns = 20'000, .restep_count = 3});
    auto const line = PhcLine::offset_from_phc0(0);
    int refused = 0;
    for (int i = 0; i < 1600; ++i) {  // refused twice, then slewed in and locked
        refused += follower.sample(line).failed == PhcFollowerStage::step ? 1 : 0;
        clocks.advance();
    }
    EXPECT_EQ(refused, 2);
    EXPECT_EQ(clocks.steps, 0);
    EXPECT_TRUE(follower.state() == PhcServoState::locked);
    clocks.phc0_ns += 5'000'000.0;  // a jump: the rule stays off, the servo slews and asks for no step
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(follower.sample(line).failed != PhcFollowerStage::step);
        clocks.advance();
    }
    clocks.refuse_steps = false;
    follower.reset();  // the caller's start over: a step is tried again
    (void)run(follower, clocks, line, 200);
    EXPECT_EQ(clocks.steps, 1);
    EXPECT_TRUE(std::abs(error_now(clocks, line)) < 100);
}

TEST(PhcFollower, a_lines_rate_is_checked_against_the_devices_range)
{
    // The parameters allow 900 000 ppm; the device 1000 ppm: a line at 2000 ppm is past the clamp in force.
    Clocks clocks{.max_ppb = 1'000'000.0};
    auto follower = make_follower(clocks);
    PhcLine const fast{.rate_offset_ppb = 2'000'000.0};
    EXPECT_TRUE(follower.sample(fast).failed == PhcFollowerStage::line);
    PhcLine const at{.rate_offset_ppb = 1'000'000.0};
    EXPECT_TRUE(follower.sample(at).failed == PhcFollowerStage::line);  // at the clamp: no headroom
    EXPECT_EQ(clocks.steers, 0);                                        // nothing applied
    PhcLine const within{.rate_offset_ppb = 500'000.0};
    EXPECT_TRUE(follower.sample(within).ok());
}

TEST(PhcFollower, a_thaw_of_a_follower_not_frozen_keeps_its_restep_count)
{
    Clocks clocks{.natural_ppb = 1'000.0};
    auto follower = make_follower(clocks, {.restep_threshold_ns = 20'000, .restep_count = 3});
    auto const line = PhcLine::offset_from_phc0(0);
    (void)run(follower, clocks, line, 160);
    clocks.phc0_ns += 5'000'000.0;
    int restarts = 0;
    for (int i = 0; i < 3; ++i) {
        follower.thaw();  // a caller that thaws on every sample
        restarts += follower.sample(line).restarted ? 1 : 0;
        clocks.advance();
    }
    EXPECT_EQ(restarts, 1);
}

TEST(PhcFollower, a_line_whose_rate_is_not_finite_is_refused)
{
    Clocks clocks;
    auto follower = make_follower(clocks);
    PhcLine const line{.rate_offset_ppb = std::nan("")};
    auto const sample = follower.sample(line);
    EXPECT_TRUE(sample.failed == PhcFollowerStage::line);
    EXPECT_TRUE(sample.error == std::make_error_code(std::errc::invalid_argument));
    PhcLine const too_fast{.rate_offset_ppb = 1e10};  // past the servo's clamp: at() would overflow
    EXPECT_TRUE(follower.sample(too_fast).failed == PhcFollowerStage::line);
    EXPECT_EQ(clocks.steers, 0);
}

TEST(PhcFollower, create_forwards_the_ports_error)
{
    class Unreadable final : public PhcFollowerPort
    {
      public:
        [[nodiscard]] auto measure() noexcept -> StatusValue<PhcCrossSample> override { return PhcCrossSample{}; }
        [[nodiscard]] auto set_frequency_ppb(double const /*ppb*/) noexcept -> Status override { return success(); }
        [[nodiscard]] auto step_ns(int64_t const /*ns*/) noexcept -> Status override { return success(); }
        [[nodiscard]] auto frequency_ppb() const noexcept -> StatusValue<double> override
        {
            return failure(std::errc::permission_denied);
        }
        [[nodiscard]] auto max_frequency_ppb() const noexcept -> StatusValue<double> override { return 1e7; }
    };
    auto const follower = PhcFollower::create({}, std::make_unique<Unreadable>());
    EXPECT_FALSE(follower.has_value());
    EXPECT_TRUE(follower.error() == std::make_error_code(std::errc::permission_denied));
}

TEST(PhcFollower, a_missing_device_does_not_open)
{
    auto const port = open_phc_follower_port("/dev/statusbar-no-such-ptp", "/dev/statusbar-no-such-ptp");
    EXPECT_FALSE(port.has_value());
}

TEST(PhcFollower, the_clamp_is_the_devices_max_adj)
{
    // A followed PHC 2000 ppm slow on a device of 1000 ppm range: the servo's output never leaves the range.
    Clocks clocks{.natural_ppb = -2'000'000.0, .max_ppb = 1'000'000.0};
    auto follower = make_follower(clocks);
    EXPECT_TRUE(follower.servo().frequency_ppb() == 0.0);
    for (int i = 0; i < 64; ++i) {
        auto const sample = follower.sample(PhcLine::offset_from_phc0(0));
        EXPECT_TRUE(sample.ok());
        EXPECT_TRUE(std::abs(follower.servo().frequency_ppb()) <= clocks.max_ppb);
        clocks.advance();
    }
}

TEST(PhcFollower, a_device_pair_measures_while_frozen)
{
    // Needs /dev/ptpN read and write access (root); STATUSBAR_PHC0 / STATUSBAR_PHCD choose the devices.
    char const* const env0 = std::getenv("STATUSBAR_PHC0");
    char const* const envd = std::getenv("STATUSBAR_PHCD");
    std::string const phc0 = env0 != nullptr ? env0 : "/dev/ptp0";
    std::string const phcd = envd != nullptr ? envd : phc0;
    auto port = open_phc_follower_port(phc0, phcd);
    if (!port) {
        SKIP("cannot open the device pair (no PHCs, or not root)");
    }
    auto follower = PhcFollower::create({}, std::move(*port));
    EXPECT_TRUE(follower.has_value());
    if (!follower) {
        return;
    }
    follower->freeze();  // measure only: a test does not steer a real clock
    auto const sample = follower->sample(PhcLine::offset_from_phc0(0));
    EXPECT_TRUE(sample.ok());
    EXPECT_FALSE(sample.steered);
    EXPECT_TRUE(sample.pair.raw_ns > 0);
}

TEST_MAIN(statusbar_ptpclient, ptpclient_phc_follower_test)
