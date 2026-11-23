// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for ptpclient_freq_ratio: the OLS and Kalman ratio trackers.

#include "statusbar/ptpclient/ptpclient_freq_ratio.hpp"

#include "statusbar/test/test.hpp"

#include <algorithm>
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

// A run of samples outside the gate is the clocks telling the filter it is
// wrong. These cases are the two ways that happened on the bench, and the one
// that must keep being rejected.

TEST(kalman_ratio_tracker, a_seed_from_a_pair_with_a_read_error_corrects_itself_within_seconds)
{
    // The fault as captured between two sites: the second sample ever read was
    // 2.2 us off, so the frequency seeded from the first pair was 8.8 ppm wrong
    // and five sigma away. Every exact sample after it was gated; the filter
    // coasted on the wrong frequency for two minutes, until the error passed a
    // millisecond and the step detector re-acquired.
    constexpr double kR = 1.000043;
    KalmanRatioTracker kf{};  // the defaults the tunnel's translator runs on
    double worst_ns = 0.0;
    for (int k = 0; k < 4 * 60; ++k) {
        kf.add(synth_offset(kR, k) + (k == 1 ? 2200 : 0), k_dt);
        double const error_ns = static_cast<double>(kf.estimate().filtered_offset_ns - synth_offset(kR, k));
        worst_ns = std::max(worst_ns, std::fabs(error_ns));
        if (k >= 4 * 10) {
            EXPECT_TRUE(std::fabs(error_ns) < 500.0);  // on the clocks again ten seconds in
        }
    }
    EXPECT_TRUE(worst_ns < 25'000.0);  // two seconds of coasting at 8.8 ppm, not two minutes
    EXPECT_EQ(kf.self_corrections(), std::uint64_t{1});
    EXPECT_TRUE(kf.estimate().valid);  // it never re-based: the frequency was corrected, not thrown away
    EXPECT_TRUE(std::fabs(kf.estimate().ppm() - 43.0) < 0.05);
}

TEST(kalman_ratio_tracker, a_slew_too_small_for_a_step_is_followed)
{
    // A time daemon removing a 33 us offset: it runs its clock 3.3 ppm off for
    // ten seconds. That is far outside the gate and far below a step. The
    // filter used to reject the next 519 samples and sit 33 us behind for two
    // minutes, until its own uncertainty had grown over the miss.
    constexpr double kR = 1.000045;
    constexpr int kSlewStart = 4 * 60;
    constexpr int kSlewEnd = kSlewStart + (4 * 10);
    auto const offset_at = [](int k) {
        int const slewed = std::clamp(k - kSlewStart, 0, kSlewEnd - kSlewStart);
        return synth_offset(kR, k) + (std::int64_t{slewed} * 825);  // 3.3 ppm for a quarter second, 33 us in all
    };
    KalmanRatioTracker kf{};
    for (int k = 0; k < kSlewEnd + (4 * 20); ++k) {
        kf.add(offset_at(k), k_dt);
    }
    int const last = kSlewEnd + (4 * 20) - 1;
    EXPECT_TRUE(std::llabs(kf.estimate().filtered_offset_ns - offset_at(last)) < 500);  // on the new level, not 33 us behind
    EXPECT_TRUE(std::fabs(kf.estimate().ppm() - 45.0) < 0.1);                           // and back on the true rate
    EXPECT_TRUE(kf.self_corrections() >= 1U);
}

TEST(kalman_ratio_tracker, a_short_run_of_bad_reads_is_still_rejected)
{
    constexpr double kR = 1.000045;
    KalmanRatioTracker kf{};
    for (int k = 0; k < 200; ++k) {
        kf.add(synth_offset(kR, k), k_dt);
    }
    // Eight bad reads in a row, each 50 us off: as many as the filter tolerates.
    for (int k = 200; k < 208; ++k) {
        EXPECT_TRUE(std::isnan(kf.add(synth_offset(kR, k) + 50'000, k_dt)));
    }
    EXPECT_FALSE(std::isnan(kf.add(synth_offset(kR, 208), k_dt)));  // a good read ends the run
    EXPECT_EQ(kf.self_corrections(), std::uint64_t{0});
    EXPECT_TRUE(std::llabs(kf.estimate().filtered_offset_ns - synth_offset(kR, 208)) < 200);
    EXPECT_TRUE(std::fabs(kf.estimate().ppm() - 45.0) < 0.02);
}

TEST(kalman_ratio_tracker, the_ninth_sample_outside_the_gate_is_taken)
{
    constexpr double kR = 1.000045;
    KalmanRatioTracker kf{};
    for (int k = 0; k < 200; ++k) {
        kf.add(synth_offset(kR, k), k_dt);
    }
    // The offset moves 50 us and stays there: not a bad read, a new level.
    for (int k = 200; k < 208; ++k) {
        EXPECT_TRUE(std::isnan(kf.add(synth_offset(kR, k) + 50'000, k_dt)));
    }
    EXPECT_FALSE(std::isnan(kf.add(synth_offset(kR, 208) + 50'000, k_dt)));
    EXPECT_EQ(kf.self_corrections(), std::uint64_t{1});
    EXPECT_TRUE(std::llabs(kf.estimate().filtered_offset_ns - (synth_offset(kR, 208) + 50'000)) < 500);
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
