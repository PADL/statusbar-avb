// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for ptpclient_freq_ratio: the OLS and Kalman ratio trackers.

#include "statusbar/ptpclient/ptpclient_freq_ratio.hpp"

#include "statusbar/test/test.hpp"

#include <cmath>
#include <cstdint>
#include <random>

using namespace statusbar::ptpclient;

namespace {

// Synthetic PHC-GPS offset for a constant ratio r at sample k (dt = 0.25 s):
//   offset(k) = base + round((r-1)*1e9 * t_k)
// base is a large GPS-like epoch difference, exercising the int64 /
// relativization path the way the real ~1.78e18 ns offset does.
constexpr double k_dt = 0.25;
constexpr std::int64_t k_base = 1'780'000'000'000'000'000LL;

std::int64_t synth_offset(double r, int k)
{
    double const t = k * k_dt;
    return k_base + static_cast<std::int64_t>(std::llround((r - 1.0) * 1e9 * t));
}

}  // namespace

TEST(ols_ratio_tracker, converges_to_known_ratio)
{
    OlsRatioTracker ols{60.0};
    for (int k = 0; k < 400; ++k) {
        ols.add(synth_offset(1.000045, k), k_dt);
    }
    auto e = ols.estimate();
    EXPECT_TRUE(e.valid);
    EXPECT_TRUE(std::fabs(e.ppm() - 45.0) < 0.1);
}

TEST(kalman_ratio_tracker, converges_to_known_ratio)
{
    KalmanRatioTracker kf{KalmanRatioTracker::Config{.meas_noise_ns = 50.0, .jerk_psd = 1e-3}};
    for (int k = 0; k < 400; ++k) {
        kf.add(synth_offset(1.000045, k), k_dt);
    }
    auto e = kf.estimate();
    EXPECT_TRUE(e.valid);
    EXPECT_TRUE(std::fabs(e.ppm() - 45.0) < 0.1);
}

TEST(kalman_ratio_tracker, acquires_quickly)
{
    // After ~10 s (40 samples) the Kalman should already be within ~1 ppm,
    // where the OLS window has barely any leverage yet.
    KalmanRatioTracker kf{KalmanRatioTracker::Config{.meas_noise_ns = 50.0, .jerk_psd = 1e-3}};
    for (int k = 0; k < 40; ++k) {
        kf.add(synth_offset(1.000045, k), k_dt);
    }
    auto e = kf.estimate();
    EXPECT_TRUE(e.valid);
    EXPECT_TRUE(std::fabs(e.ppm() - 45.0) < 1.0);
}

TEST(kalman_ratio_tracker, gates_outlier_spike)
{
    KalmanRatioTracker kf{KalmanRatioTracker::Config{.meas_noise_ns = 50.0, .jerk_psd = 1e-3, .gate_sigmas = 5.0}};
    for (int k = 0; k < 200; ++k) {
        kf.add(synth_offset(1.000045, k), k_dt);
    }
    double const before = kf.estimate().ppm();
    // Inject a 1 ms phase spike: it must be gated, not absorbed.
    double const innov = kf.add(synth_offset(1.000045, 200) + 1'000'000, k_dt);
    EXPECT_TRUE(std::isnan(innov));
    double const after = kf.estimate().ppm();
    EXPECT_TRUE(std::fabs(after - before) < 0.02);
}

TEST(kalman_ratio_tracker, robust_noisy_startup)
{
    // Regression: feed a NOISY startup (read jitter) with the real ~1.78e18 ns
    // offset from sample 0, using an R that previously diverged. The seeded
    // frequency + conservative P + Joseph-form update must converge and never
    // run away (the old f=0 + huge-P start hit +105/-99 ppm, drift -20937 ppm/hr).
    std::mt19937 rng{12345};
    std::normal_distribution<double> noise{0.0, 400.0};  // 400 ns read jitter
    KalmanRatioTracker kf{KalmanRatioTracker::Config{.meas_noise_ns = 400.0, .jerk_psd = 1e-2}};
    for (int k = 0; k < 300; ++k) {
        std::int64_t const off = synth_offset(1.000045, k) + std::llround(noise(rng));
        kf.add(off, k_dt);
        auto const e = kf.estimate();
        if (e.valid && k >= 20) {
            EXPECT_TRUE(std::fabs(e.ppm() - 45.0) < 30.0);        // never diverges
            EXPECT_TRUE(std::fabs(e.drift_ppm_per_hr) < 5000.0);  // drift stays bounded
        }
    }
    auto const e = kf.estimate();
    EXPECT_TRUE(e.valid);
    EXPECT_TRUE(std::fabs(e.ppm() - 45.0) < 1.5);  // converges despite the noise
}

TEST(kalman_ratio_tracker, seed_step_does_not_bake_bogus_rate)
{
    // Guards against a large transient offset railing the rate to +83345 ppm: the
    // entity took its first offset sample, then ptp4l stepped the PHC ~21 ms
    // before the second, so the 2-sample seed computed f0 = 21ms/0.25s ≈ +84000
    // ppm and then "slowly slewed" back over months. The seed must REJECT a
    // step-sized f0 and re-base, not bake the bogus rate.
    constexpr std::int64_t kStep = 21'000'000;  // 21 ms PHC step between sample 0 and 1
    KalmanRatioTracker kf{KalmanRatioTracker::Config{.meas_noise_ns = 400.0, .jerk_psd = 1e-2}};
    kf.add(synth_offset(1.000045, 0), k_dt);          // sample 0 -> base
    kf.add(synth_offset(1.000045, 1) + kStep, k_dt);  // sample 1 -> +21 ms STEP
    for (int k = 2; k < 300; ++k) {
        kf.add(synth_offset(1.000045, k) + kStep, k_dt);
        auto const e = kf.estimate();
        if (e.valid) {
            EXPECT_TRUE(std::fabs(e.ppm()) < 1000.0);  // never the +83345 ppm runaway
        }
    }
    auto const e = kf.estimate();
    EXPECT_TRUE(e.valid);
    EXPECT_TRUE(std::fabs(e.ppm() - 45.0) < 1.5);  // recovers the true rate
}

TEST(kalman_ratio_tracker, runtime_step_reacquires_true_rate)
{
    // Guards a lock-then-step scenario: the filter locks the true rate, then
    // chrony stepped CLOCK_REALTIME to GPS (~+1.78e9 s) so PHC-REALTIME jumped a
    // huge negative amount. A "snap phase, keep frequency" jump FROZE a corrupted
    // rate (-28913 ppm) forever (it stopped calling update()). The filter must
    // RE-ACQUIRE the true rate from the post-step regime, not freeze.
    constexpr double kR = 1.000045;  // +45 ppm
    KalmanRatioTracker kf{KalmanRatioTracker::Config{.meas_noise_ns = 50.0, .jerk_psd = 1e-3}};
    for (int k = 0; k < 200; ++k) {
        kf.add(synth_offset(kR, k), k_dt);
    }
    EXPECT_TRUE(std::fabs(kf.estimate().ppm() - 45.0) < 1.0);

    constexpr std::int64_t kStep = -1'500'000'000;  // chrony step: -1.5 s, permanent
    double const innov = kf.add(synth_offset(kR, 200) + kStep, k_dt);
    EXPECT_TRUE(std::isnan(innov));  // recognized as a step (re-based, re-acquiring)
    for (int k = 201; k < 400; ++k) {
        kf.add(synth_offset(kR, k) + kStep, k_dt);
    }
    auto const e = kf.estimate();
    EXPECT_TRUE(e.valid);
    EXPECT_TRUE(std::fabs(e.ppm() - 45.0) < 1.0);            // re-acquired the true rate (not frozen)
    EXPECT_TRUE(std::fabs(e.freq_uncertainty_ppb) < 1.0e6);  // converged (uncertainty did not explode)
    // Filtered phase tracks the NEW (stepped) offset level, not the old one.
    std::int64_t const expected = synth_offset(kR, 399) + kStep;
    EXPECT_TRUE(std::llabs(e.filtered_offset_ns - expected) < 200'000);
}

TEST(ratio_estimate, helpers)
{
    RatioEstimate e;
    e.r = 1.000045;
    EXPECT_TRUE(std::fabs(e.ppm() - 45.0) < 1e-6);
    // 96 kHz nominal 10416.667 ns/sample; +45 ppm -> ~10417.14
    EXPECT_TRUE(std::fabs(e.phc_ns_per_sample(96000.0) - 10417.135) < 0.1);
}

//
// Main test runner
//

TEST_MAIN(statusbar_ptpclient, ptpclient_freq_ratio_test)
