// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The PHC tests that need real devices: /dev/ptpN opened, read against the
// reference clock, and, for the one that is asked to, steered. They are
// functional tests because they run on a real kernel and one of them waits on
// real time. Each skips when its devices do not open, so a machine with no
// PHC, or a run without the rights, passes. docs/LINUXPTP.md says how to name
// the devices (STATUSBAR_PHC0, STATUSBAR_PHCD, STATUSBAR_PHC_SERVO_DEVICE).

#include "statusbar/ptpclient/ptpclient_phc.hpp"
#include "statusbar/ptpclient/ptpclient_phc_follower.hpp"
#include "statusbar/ptpclient/ptpclient_phc_servo.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <print>
#include <string>
#include <utility>

using namespace statusbar;
using namespace statusbar::ptpclient;

namespace {

/// The servo's sample interval on a real device: 16 updates a second.
constexpr int64_t INTERVAL_NS = 62'500'000;

}  // namespace

TEST(ptpclient_phc, a_writable_phcd_can_be_adjusted)
{
    // Needs /dev/ptpN read and write access (root); STATUSBAR_PHCD chooses the device.
    char const* const envd = std::getenv("STATUSBAR_PHCD");
    std::string const phcd = envd != nullptr ? envd : "/dev/ptp0";
    auto pair = PhcCrossTimestamp::open(phcd, phcd, {}, PhcCrossAccess::phcd_writable);
    if (!pair) {
        SKIP("cannot open the device writable (no PHC, or not root)");
    }
    auto const frequency = pair->phcd().frequency_ppb();
    EXPECT_TRUE(frequency.has_value());
    if (frequency) {
        EXPECT_TRUE(pair->phcd().set_frequency_ppb(*frequency).has_value());  // its own frequency: nothing moves
    }
    auto read_only = PhcCrossTimestamp::open(phcd, phcd);
    EXPECT_TRUE(read_only.has_value());
    if (frequency && read_only) {
        EXPECT_FALSE(read_only->phcd().set_frequency_ppb(*frequency).has_value());  // the default stays read-only
    }
}

TEST(ptpclient_phc, real_phcs_cross_timestamp_within_their_brackets)
{
    // Needs read access to /dev/ptpN (root); STATUSBAR_PHC0 / STATUSBAR_PHCD choose the devices.
    char const* const env0 = std::getenv("STATUSBAR_PHC0");
    char const* const envd = std::getenv("STATUSBAR_PHCD");
    std::string const phc0 = env0 != nullptr ? env0 : "/dev/ptp0";
    std::string const phcd = envd != nullptr ? envd : phc0;
    auto pair = PhcCrossTimestamp::open(phc0, phcd);
    if (!pair) {
        std::println("  skipped: cannot open {} and {} ({})", phc0, phcd, pair.error().message());
        return;
    }
    int64_t worst = 0;
    int64_t previous_raw = 0;
    for (int i = 0; i < 16; ++i) {
        auto const sample = pair->measure();
        EXPECT_TRUE(sample.has_value());
        if (!sample) {
            return;
        }
        EXPECT_TRUE(sample->raw_ns > previous_raw);
        previous_raw = sample->raw_ns;
        EXPECT_TRUE(sample->uncertainty_ns >= 0 && sample->uncertainty_ns < 100'000);
        if (phc0 == phcd) {
            // one clock against itself: the pair agrees to within the brackets
            EXPECT_TRUE(std::abs(sample->phc0_ns - sample->phcd_ns) <= (2 * sample->uncertainty_ns) + 100);
        }
        worst = std::max(worst, sample->uncertainty_ns);
    }
    std::println(
        "  {} ({}) / {} ({}): worst uncertainty {} ns",
        phc0,
        static_cast<int>(pair->phc0().method()),
        phcd,
        static_cast<int>(pair->phcd().method()),
        worst);
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

#if defined(__linux__)

TEST(PhcServo, a_real_phc_is_steered_onto_a_100_ppm_target)
{
    // Steers a spare PHC: opt-in, as root on the bench (docs/LINUXPTP.md).
    char const* const steered = std::getenv("STATUSBAR_PHC_SERVO_DEVICE");
    char const* const reference = std::getenv("STATUSBAR_PHC0");
    if (steered == nullptr || reference == nullptr || std::string{steered} == reference) {
        std::println("  skipped: set STATUSBAR_PHC0 and a different STATUSBAR_PHC_SERVO_DEVICE");
        return;
    }
    auto pair = PhcCrossTimestamp::open(reference, steered);
    auto device = PhcDevice::open(steered, PhcDeviceConfig{.writable = true});
    if (!pair || !device) {
        std::println("  skipped: cannot open {} and {}", reference, steered);
        return;
    }
    auto const original = device->frequency_ppb();
    auto const max_ppb = device->max_frequency_ppb();
    EXPECT_TRUE(original.has_value() && max_ppb.has_value());
    auto servo = PhcServo::create(PhcServoParams{.max_frequency_ppb = std::min(*max_ppb, 900'000'000.0)}, original.value_or(0.0));
    EXPECT_TRUE(servo.has_value());

    auto const start = pair->measure();
    EXPECT_TRUE(start.has_value());
    int64_t worst_last_second = 0;
    int steps = 0;
    for (int i = 0; i < 160 && start; ++i) {
        auto const now = pair->measure();
        EXPECT_TRUE(now.has_value());
        if (!now) {
            break;
        }
        // target: 1 ms after PHC_d's start, at 100 ppm over PHC0
        auto const target = start->phcd_ns + 1'000'000 + std::llround(static_cast<double>(now->phc0_ns - start->phc0_ns) * 1.0001);
        int64_t const error = now->phcd_ns - target;
        auto const action = servo->sample(error, now->raw_ns, 100'000.0);
        steps += action.step_ns ? 1 : 0;
        EXPECT_TRUE(apply(*device, action).has_value());
        if (i >= 144) {
            worst_last_second = std::max(worst_last_second, std::abs(error));
        }
        timespec const interval{.tv_sec = 0, .tv_nsec = INTERVAL_NS};
        ::nanosleep(&interval, nullptr);
    }
    std::println(
        "  {} ({}): {} steps, worst error in the last second {} ns, frequency {:.1f} ppb",
        steered,
        static_cast<int>(pair->phcd().method()),
        steps,
        worst_last_second,
        servo->frequency_ppb());
    EXPECT_EQ(steps, 1);
    EXPECT_TRUE(servo->state() == PhcServoState::locked);
    EXPECT_TRUE(worst_last_second < 1'000);
    (void)device->set_frequency_ppb(original.value_or(0.0));
}

#endif  // __linux__

TEST_MAIN(statusbar_ptpclient, ptpclient_phc_functional_test)
