// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the gPTP ClockSlaveControl / ServoLoop PI controller.
//
// Focuses on the two branches that protect the media-clock integrator rail
// against a stuck or poisoned integrator:
//
//   1. Negative-time-jump suppression: when the grandmaster steps backwards
//      between two consecutive syncs the servo must suppress that sample
//      without poisoning the integrator (current_ppm_ unchanged) while still
//      re-baselining its previous-sample tracking so it recovers next cycle.
//
//   2. Phase-jump-pending integrator suppression: while the consecutive
//      out-of-band counter is maturing the servo must NOT pump the PI
//      integrator with the (huge) phase error — doing so winds current_ppm_
//      straight to the ±servo_ppm_limit rail and the servo never recovers.

#include "statusbar/gptp/gptp_servo.hpp"

#include "statusbar/gptp/gptp_base.hpp"
#include "statusbar/gptp/gptp_config.hpp"
#include "statusbar/gptp/gptp_md_sync_receive.hpp"
#include "statusbar/test/test.hpp"

#include <cmath>
#include <cstdint>

using namespace statusbar::gptp;

namespace {

// Build a sync indication whose corrected master time equals `master_ns`
// (mean_link_delay and correction are 0) and whose hardware receive time is
// `local_ns`, so the servo's phase error is exactly master_ns - local_ns.
auto make_ind(int64_t master_ns, int64_t local_ns, int8_t log_interval = -3) -> MDSyncReceiveIndication
{
    MDSyncReceiveIndication ind{};
    auto const secs = static_cast<uint64_t>(master_ns / 1'000'000'000LL);
    auto const nsecs = static_cast<uint32_t>(master_ns % 1'000'000'000LL);
    ind.precise_origin_timestamp = Timestamp(secs, nsecs);
    ind.correction_field_ns = 0;
    ind.sync_rx_local_ns = local_ns;
    ind.log_message_interval = log_interval;
    ind.has_follow_up_tlv = false;
    return ind;
}

auto approx(double a, double b, double eps = 1e-6) -> bool
{
    return std::abs(a - b) <= eps;
}

constexpr int64_t SEC = 1'000'000'000LL;

}  // namespace

// --- Basic behaviour -------------------------------------------------------

TEST(gptp_servo, first_sample_uses_unity_rate)
{
    GptpConfig cfg{};
    ServoLoop servo{cfg};

    // First sample: no previous pair, so the rate term defaults to 1.0 and
    // only the integral term (driven by the phase error) acts.
    auto const out = servo.process(make_ind((100 * SEC), (100 * SEC) - 30'000), 0, 1.0);

    EXPECT_FALSE(out.servo_suppressed);
    EXPECT_TRUE(approx(out.rate_ratio, 1.0));
    EXPECT_EQ(out.master_offset_ns, 30'000);
    EXPECT_TRUE(out.frequency_adjust_ppb.has_value());
    EXPECT_FALSE(out.phase_jump_ns.has_value());
    // integral_delta = integral_gain(0.0003) * sync_per_sec(8) * 30000 = 72 ppm
    EXPECT_TRUE(approx(servo.current_ppm(), 72.0, 1e-3));
}

TEST(gptp_servo, rate_ratio_reported_from_two_samples)
{
    GptpConfig cfg{};
    ServoLoop servo{cfg};

    (void)servo.process(make_ind((100 * SEC), (100 * SEC)), 0, 1.0);
    // Master advances 100'040'000 ns while local advances 100'000'000 ns:
    // rate = 1.0004, and the 40'000 ns gap stays under the 50µs jump threshold.
    auto const out = servo.process(make_ind((100 * SEC) + 100'040'000, (100 * SEC) + 100'000'000), 0, 1.0);

    EXPECT_FALSE(out.servo_suppressed);
    EXPECT_TRUE(approx(out.rate_ratio, 1.0004, 1e-9));
}

// --- Regression site #1: negative-time-jump suppression --------------------

TEST(gptp_servo, negative_time_jump_suppresses_without_poisoning_integrator)
{
    GptpConfig cfg{};
    ServoLoop servo{cfg};

    // Prime the integrator to a known non-zero value with one clean sample.
    (void)servo.process(make_ind((100 * SEC), (100 * SEC) - 30'000), 0, 1.0);
    double const ppm_before = servo.current_ppm();
    EXPECT_TRUE(approx(ppm_before, 72.0, 1e-3));

    // Grandmaster steps backwards (99.999s after 100.0s) while local time
    // advances normally: master_delta < 0 must suppress this sample.
    auto const out = servo.process(make_ind((100 * SEC) - 1'000'000, (100 * SEC) + 125'000'000), 0, 1.0);

    EXPECT_TRUE(out.servo_suppressed);
    EXPECT_TRUE(approx(out.rate_ratio, 1.0));
    EXPECT_FALSE(out.frequency_adjust_ppb.has_value());
    EXPECT_FALSE(out.phase_jump_ns.has_value());
    // The integrator must be untouched — this is what the suppression protects.
    EXPECT_TRUE(approx(servo.current_ppm(), ppm_before));
}

TEST(gptp_servo, recovers_after_negative_time_jump)
{
    GptpConfig cfg{};
    ServoLoop servo{cfg};

    (void)servo.process(make_ind((100 * SEC), (100 * SEC) - 30'000), 0, 1.0);
    auto const jumped = servo.process(make_ind((100 * SEC) - 1'000'000, (100 * SEC) + 125'000'000), 0, 1.0);
    EXPECT_TRUE(jumped.servo_suppressed);

    // Because the suppressed sample re-baselined previous_{master,local}, the
    // next clean sample is processed normally rather than suppressed again.
    auto const recovered = servo.process(make_ind((100 * SEC) + 250'000'000, (100 * SEC) + 250'000'000 - 30'000), 0, 1.0);
    EXPECT_FALSE(recovered.servo_suppressed);
    EXPECT_TRUE(recovered.frequency_adjust_ppb.has_value());
}

// --- Regression site #2: integrator suppressed while jump pending ----------

TEST(gptp_servo, phase_jump_pending_does_not_wind_integrator)
{
    GptpConfig cfg{};
    ServoLoop servo{cfg};

    // Prime to a known ppm.
    (void)servo.process(make_ind((100 * SEC), (100 * SEC) - 30'000), 0, 1.0);
    double const ppm_before = servo.current_ppm();

    // A single huge phase error (1 ms > 50 µs threshold) with master advancing
    // forward (so it's NOT a negative jump). The consecutive counter is only 1,
    // far below servo_phase_jump_consecutive_samples(6), so the servo must
    // suppress this sample and leave the integrator alone.
    auto const out = servo.process(make_ind((100 * SEC) + 125'000'000, (100 * SEC) + 125'000'000 - 1'000'000), 0, 1.0);

    EXPECT_TRUE(out.servo_suppressed);
    EXPECT_FALSE(out.frequency_adjust_ppb.has_value());
    EXPECT_FALSE(out.phase_jump_ns.has_value());
    EXPECT_EQ(static_cast<int>(servo.consecutive_phase_jumps()), 1);
    // The crux: a single out-of-band sample must NOT move current_ppm_.
    EXPECT_TRUE(approx(servo.current_ppm(), ppm_before));
}

TEST(gptp_servo, phase_jump_applied_after_consecutive_samples)
{
    GptpConfig cfg{};
    ServoLoop servo{cfg};
    auto const need = static_cast<int>(cfg.servo_phase_jump_consecutive_samples);

    ServoOutput out{};
    for (int i = 0; i < need; ++i) {
        // Constant +1 ms phase error, master advancing 0.125 s each sync so
        // master_delta stays positive (not a negative jump).
        int64_t const master = (100 * SEC) + (static_cast<int64_t>(i) * 125'000'000);
        out = servo.process(make_ind(master, master - 1'000'000), 0, 1.0);
        if (i + 1 < need) {
            // Still maturing: suppressed, no step, no frequency pump.
            EXPECT_TRUE(out.servo_suppressed);
            EXPECT_FALSE(out.phase_jump_ns.has_value());
        }
    }

    // On the Nth consecutive sample the servo steps the clock and resets.
    EXPECT_TRUE(out.phase_jump_ns.has_value());
    EXPECT_EQ(out.phase_jump_ns.value(), 1'000'000);
    EXPECT_TRUE(out.frequency_adjust_ppb.has_value());
    EXPECT_TRUE(approx(out.frequency_adjust_ppb.value(), 0.0));
    EXPECT_TRUE(approx(servo.current_ppm(), 0.0));
    EXPECT_EQ(static_cast<int>(servo.consecutive_phase_jumps()), 0);
}

// --- PI integrator clamp ---------------------------------------------------

TEST(gptp_servo, integrator_clamps_at_ppm_limit)
{
    GptpConfig cfg{};
    ServoLoop servo{cfg};

    // Sustained +40 µs error (below the 50 µs jump threshold) with master and
    // local advancing in lock-step (rate 1.0, so only the integral term acts).
    // integral_delta = 0.0003 * 8 * 40000 = 96 ppm/sample -> clamps to +250
    // within a few samples.
    ServoOutput out{};
    for (int i = 0; i < 6; ++i) {
        int64_t const master = (100 * SEC) + (static_cast<int64_t>(i) * 125'000'000);
        out = servo.process(make_ind(master, master - 40'000), 0, 1.0);
        EXPECT_FALSE(out.servo_suppressed);
    }

    EXPECT_TRUE(approx(servo.current_ppm(), cfg.servo_ppm_limit));
    EXPECT_TRUE(out.frequency_adjust_ppb.has_value());
    EXPECT_TRUE(approx(out.frequency_adjust_ppb.value(), cfg.servo_ppm_limit * 1000.0));
}

// --- reset() ---------------------------------------------------------------

TEST(gptp_servo, reset_clears_state)
{
    GptpConfig cfg{};
    ServoLoop servo{cfg};

    (void)servo.process(make_ind((100 * SEC), (100 * SEC) - 40'000), 0, 1.0);
    EXPECT_TRUE(servo.current_ppm() != 0.0);

    servo.reset();
    EXPECT_TRUE(approx(servo.current_ppm(), 0.0));
    EXPECT_EQ(static_cast<int>(servo.consecutive_phase_jumps()), 0);

    // After reset the next sample is treated as a first sample again: the rate
    // term has no previous pair, so it reports unity rate.
    auto const out = servo.process(make_ind((200 * SEC), (200 * SEC)), 0, 1.0);
    EXPECT_TRUE(approx(out.rate_ratio, 1.0));
}

// Test runner

TEST_MAIN(statusbar_gptp, gptp_servo_test)
