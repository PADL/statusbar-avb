// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ptpclient/ptpclient_phc_servo.hpp"

#include "statusbar/test/test.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <print>
#include <string>

using namespace statusbar;
using namespace statusbar::ptpclient;

namespace {

constexpr int64_t INTERVAL_NS = 62'500'000;

/// A PHC with its own oscillator error, steered by the servo, against a target line
/// `target = offset + rate * local` on a steady local clock.
struct Bench
{
    double natural_ppb{0.0};
    double phc_ns{1'000'000'000.0};
    double target_offset_ns{1'000'000'000.0};
    double target_rate{1.0};
    double frequency_ppb{0.0};
    int64_t local_ns{0};
    uint32_t noise_state{12345};
    int64_t noise_ns{0};  ///< measurement noise: uniform in [-noise_ns, noise_ns]
    int steps{0};

    [[nodiscard]] auto target_ns() const -> double { return target_offset_ns + (target_rate * static_cast<double>(local_ns)); }
    [[nodiscard]] auto error_ns() const -> int64_t { return std::llround(phc_ns - target_ns()); }

    auto noise() -> int64_t
    {
        if (noise_ns == 0) {
            return 0;
        }
        noise_state = (noise_state * 1103515245U) + 12345U;
        return static_cast<int64_t>((noise_state >> 8U) % static_cast<uint32_t>((2 * noise_ns) + 1)) - noise_ns;
    }

    /// One interval: measure, servo, apply, advance.
    auto step(PhcServo& servo, double feed_forward_ppb = 0.0) -> PhcServoAction
    {
        auto const action = servo.sample(error_ns() + noise(), local_ns, feed_forward_ppb);
        frequency_ppb = action.frequency_ppb;
        if (action.step_ns) {
            phc_ns += static_cast<double>(*action.step_ns);
            ++steps;
        }
        auto const dt = static_cast<double>(INTERVAL_NS);
        phc_ns += dt * (1.0 + (natural_ppb * 1e-9)) * (1.0 + (frequency_ppb * 1e-9));
        local_ns += INTERVAL_NS;
        return action;
    }

    /// Change the target's rate at the current instant, keeping its phase.
    void set_target_rate(double const rate)
    {
        target_offset_ns = target_ns() - (rate * static_cast<double>(local_ns));
        target_rate = rate;
    }
};

auto make_servo(PhcServoParams const& params = {}) -> PhcServo
{
    auto servo = PhcServo::create(params);
    EXPECT_TRUE(servo.has_value());
    return std::move(*servo);
}

}  // namespace

TEST(PhcServo, kp_and_ki_follow_linuxptp_at_16_hz)
{
    auto const servo = make_servo();
    EXPECT_TRUE(std::abs(servo.kp() - (0.7 * std::pow(0.0625, -0.3))) < 1e-12);  // 1.608
    EXPECT_TRUE(std::abs(servo.ki() - (0.3 * std::pow(0.0625, 0.4))) < 1e-12);   // 0.0990
    auto const one_hz = make_servo(PhcServoParams{.interval_ns = 1'000'000'000});
    EXPECT_TRUE(std::abs(one_hz.kp() - 0.7) < 1e-12);
    EXPECT_TRUE(std::abs(one_hz.ki() - 0.3) < 1e-12);
}

TEST(PhcServo, create_refuses_bad_parameters)
{
    EXPECT_FALSE(PhcServo::create(PhcServoParams{.interval_ns = 0}).has_value());
    EXPECT_FALSE(PhcServo::create(PhcServoParams{.max_frequency_ppb = 0.0}).has_value());
    EXPECT_FALSE(PhcServo::create(PhcServoParams{.lock_threshold_ns = 0}).has_value());
    EXPECT_FALSE(PhcServo::create(PhcServoParams{.lock_threshold_ns = 2000, .unlock_threshold_ns = 1000}).has_value());
    EXPECT_FALSE(PhcServo::create(PhcServoParams{.lock_count = 0}).has_value());
    EXPECT_FALSE(PhcServo::create(PhcServoParams{.kp_norm_max = 0.0}).has_value());
    EXPECT_FALSE(PhcServo::create(PhcServoParams{.ki_norm_max = -0.3}).has_value());
    EXPECT_FALSE(PhcServo::create(PhcServoParams{}, NAN).has_value());
    EXPECT_TRUE(PhcServo::create(PhcServoParams{}, 1234.0).has_value());
}

TEST(PhcServo, the_servo_converges_on_a_100_ppm_target_within_ten_seconds)
{
    auto servo = make_servo();
    Bench bench{.natural_ppb = 25'000.0, .target_rate = 1.0001, .noise_ns = 40};
    bench.target_offset_ns += 5'000;  // under the first step threshold: slewed
    int64_t worst_last_second = 0;
    for (int i = 0; i < 160; ++i) {
        (void)bench.step(servo);
        if (i >= 144) {
            worst_last_second = std::max(worst_last_second, std::abs(bench.error_ns()));
        }
    }
    EXPECT_TRUE(servo.state() == PhcServoState::locked);
    EXPECT_TRUE(worst_last_second < 100);
    EXPECT_EQ(bench.steps, 0);
    // (1 + 25 ppm)(1 + f) = 1 + 100 ppm
    EXPECT_TRUE(std::abs(servo.frequency_ppb() - ((1.0001 / 1.000025 - 1.0) * 1e9)) < 200.0);
}

TEST(PhcServo, the_servo_steps_only_before_its_first_lock)
{
    auto servo = make_servo();
    Bench bench{.natural_ppb = -10'000.0};
    bench.target_offset_ns -= 1'000'000;  // the PHC is 1 ms ahead
    EXPECT_FALSE(servo.estimated());
    for (int i = 0; i < 160; ++i) {
        (void)bench.step(servo);
    }
    EXPECT_TRUE(servo.estimated());
    EXPECT_EQ(bench.steps, 1);
    EXPECT_TRUE(servo.state() == PhcServoState::locked);
    EXPECT_TRUE(std::abs(bench.error_ns()) < 100);

    // Locked once: a 1 ms jump of the target is slewed, never stepped, and unlocks.
    bench.target_offset_ns += 1'000'000;
    for (int i = 0; i < 32; ++i) {
        auto const action = bench.step(servo);
        EXPECT_FALSE(action.step_ns.has_value());
    }
    EXPECT_EQ(bench.steps, 1);
    EXPECT_TRUE(servo.state() == PhcServoState::unlocked);

    // After reset() the servo estimates again, and may step again.
    servo.reset();
    EXPECT_FALSE(servo.estimated());
    bench.target_offset_ns += 1'000'000;
    for (int i = 0; i < 160; ++i) {
        (void)bench.step(servo);
    }
    EXPECT_EQ(bench.steps, 2);
    EXPECT_TRUE(servo.state() == PhcServoState::locked);
}

TEST(PhcServo, a_frozen_servo_holds_its_frequency)
{
    auto servo = make_servo();
    Bench bench{.natural_ppb = 40'000.0};
    for (int i = 0; i < 160; ++i) {
        (void)bench.step(servo);
    }
    EXPECT_TRUE(servo.state() == PhcServoState::locked);
    double const held = servo.frequency_ppb();

    servo.freeze();
    EXPECT_TRUE(servo.state() == PhcServoState::frozen);
    bench.target_offset_ns += 50'000;  // the target moves away: ignored
    for (int i = 0; i < 64; ++i) {
        auto const action = bench.step(servo);
        EXPECT_TRUE(action.frequency_ppb == held);
        EXPECT_FALSE(action.step_ns.has_value());
    }
    EXPECT_TRUE(servo.frequency_ppb() == held);

    // Thawed, it estimates its rate again and slews back without a step: it has locked before.
    servo.thaw();
    EXPECT_FALSE(servo.estimated());
    for (int i = 0; i < 320; ++i) {
        (void)bench.step(servo);
    }
    EXPECT_EQ(bench.steps, 0);
    EXPECT_TRUE(servo.state() == PhcServoState::locked);
}

TEST(PhcServo, a_fed_forward_rate_change_needs_no_relearning)
{
    auto servo = make_servo();
    Bench bench{.natural_ppb = 15'000.0, .target_rate = 1.0001};
    for (int i = 0; i < 160; ++i) {
        (void)bench.step(servo, 100'000.0);
    }
    EXPECT_TRUE(servo.state() == PhcServoState::locked);

    // The target's rate moves by 10 ppm and the feed-forward with it: the error stays small.
    bench.set_target_rate(1.00011);
    int64_t worst = 0;
    for (int i = 0; i < 160; ++i) {
        (void)bench.step(servo, 110'000.0);
        worst = std::max(worst, std::abs(bench.error_ns()));
    }
    EXPECT_TRUE(worst < 100);
    EXPECT_TRUE(servo.state() == PhcServoState::locked);
}

TEST(PhcServo, the_frequency_is_clamped)
{
    auto servo = make_servo(PhcServoParams{.max_frequency_ppb = 50'000.0});
    Bench bench{.target_rate = 1.0001};
    double highest = 0.0;
    for (int i = 0; i < 320; ++i) {
        (void)bench.step(servo);
        highest = std::max(highest, std::abs(bench.frequency_ppb));
    }
    EXPECT_TRUE(highest <= 50'000.0);
    EXPECT_TRUE(servo.state() == PhcServoState::unlocked);
}

TEST_MAIN(statusbar_ptpclient, ptpclient_phc_servo_test)
