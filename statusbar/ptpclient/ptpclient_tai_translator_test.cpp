// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for GpsTaiTranslator: recovering an absolute GPS-TAI timeline
// from a local master clock (free-running-GM epoch, ppm off GPS) plus
// NTP-disciplined CLOCK_REALTIME samples.

#include "statusbar/ptpclient/ptpclient_tai_translator.hpp"

#include "statusbar/test/test.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <random>

using namespace statusbar::ptpclient;

namespace {

constexpr std::int64_t k_tai_offset = 37'000'000'000LL;

// Simulated deployment shaped like the real one: UTC (chrony) is truth;
// the master clock free-runs from an arbitrary boot-ish epoch at a
// constant ppm offset from GPS:
//   master(utc) = master_epoch + (utc - utc0) * r
// node-a-like numbers: epoch ~37.9 days, +45 ppm.
struct SimClocks
{
    std::int64_t utc0 = 1'780'000'000'000'000'000LL;      // ~2026 in UTC ns
    std::int64_t master_epoch = 3'274'000'000'000'000LL;  // ~37.9 days
    double r = 1.000045;                                  // master_rate / GPS_rate

    [[nodiscard]] std::int64_t utc_at(double t_s) const { return utc0 + static_cast<std::int64_t>(std::llround(t_s * 1e9)); }
    [[nodiscard]] std::int64_t master_at(double t_s) const
    {
        return master_epoch + static_cast<std::int64_t>(std::llround(t_s * 1e9 * r));
    }
};

}  // namespace

TEST(gps_tai_translator, identity_before_first_sample)
{
    GpsTaiTranslator tr;
    EXPECT_FALSE(tr.has_sample());
    EXPECT_EQ(tr.tai_ns(123'456'789), std::int64_t{123'456'789});
}

TEST(gps_tai_translator, single_sample_raw_offset)
{
    // One sample: translation = raw offset removal + leap offset. NTP-quality
    // but absolutely sane from the very first pair.
    SimClocks sim;
    GpsTaiTranslator tr;
    tr.add_sample(sim.master_at(0.0), sim.utc_at(0.0));
    EXPECT_TRUE(tr.has_sample());
    std::int64_t const tai = tr.tai_ns(sim.master_at(0.0));
    EXPECT_EQ(tai, sim.utc_at(0.0) + k_tai_offset);
}

TEST(gps_tai_translator, realtime_master_collapses_to_plus_offset)
{
    // When the master IS CLOCK_REALTIME (time-source=realtime deployments),
    // offset == 0 and TAI = master + 37 s exactly.
    GpsTaiTranslator tr;
    std::int64_t const now = 1'780'000'000'000'000'000LL;
    for (int k = 0; k < 8; ++k) {
        std::int64_t const t = now + (k * 250'000'000LL);
        tr.add_sample(t, t);
    }
    std::int64_t const probe = now + 2'000'000'000LL;
    EXPECT_EQ(tr.tai_ns(probe), probe + k_tai_offset);
}

TEST(gps_tai_translator, converges_on_drifting_master)
{
    // Clean samples at 4 Hz: after convergence the translated time must match
    // UTC+37 s to sub-microsecond even though the master runs +45 ppm with a
    // 37.9-day epoch error.
    SimClocks sim;
    GpsTaiTranslator tr{GpsTaiTranslator::Config{
        .kalman = {.meas_noise_ns = 50.0, .jerk_psd = 1e-3},
    }};
    double t = 0.0;
    for (int k = 0; k < 400; ++k, t += 0.25) {
        tr.add_sample(sim.master_at(t), sim.utc_at(t));
    }
    // Probe between samples (propagation path), 100 ms past the last update.
    double const tp = t - 0.25 + 0.1;
    std::int64_t const tai = tr.tai_ns(sim.master_at(tp));
    std::int64_t const expect = sim.utc_at(tp) + k_tai_offset;
    EXPECT_TRUE(std::llabs(tai - expect) < 1'000);  // < 1 us
}

TEST(gps_tai_translator, filtered_phase_rejects_sample_jitter)
{
    // Noisy NTP-like samples (400 ns sigma): the translation error must end up
    // well below the per-sample jitter — proof we use the FILTERED phase, not
    // the raw last offset.
    SimClocks sim;
    std::mt19937 rng{4242};
    std::normal_distribution<double> noise{0.0, 400.0};
    GpsTaiTranslator tr{GpsTaiTranslator::Config{
        .kalman = {.meas_noise_ns = 400.0, .jerk_psd = 1e-3},
    }};
    double t = 0.0;
    for (int k = 0; k < 600; ++k, t += 0.25) {
        std::int64_t const utc_meas = sim.utc_at(t) + std::llround(noise(rng));
        tr.add_sample(sim.master_at(t), utc_meas);
    }
    // Average translation error across many probe points; the mean must be
    // small and each individual probe well under 3 sigma of the raw jitter.
    double err_sum = 0.0;
    for (int k = 0; k < 16; ++k) {
        double const tp = t - 0.25 + (0.015 * k);
        std::int64_t const tai = tr.tai_ns(sim.master_at(tp));
        std::int64_t const expect = sim.utc_at(tp) + k_tai_offset;
        double const err = static_cast<double>(tai - expect);
        EXPECT_TRUE(std::fabs(err) < 1'200.0);  // 3 sigma of raw jitter
        err_sum += err;
    }
    EXPECT_TRUE(std::fabs(err_sum / 16.0) < 400.0);  // mean well inside 1 sigma
}

TEST(gps_tai_translator, snapshot_reproduces_stateful_tai_ns)
{
    // The cross-thread publication (itc snapshot) relies on the pure
    // tai_ns(snapshot, master) matching the stateful member tai_ns(master)
    // EXACTLY, at arbitrary probe points and every sample-count tier.
    SimClocks sim;
    GpsTaiTranslator tr{GpsTaiTranslator::Config{.kalman = {.meas_noise_ns = 50.0, .jerk_psd = 1e-3}}};

    // Before the first sample: identity, and snapshot agrees.
    {
        auto const snap = tr.snapshot();
        std::int64_t const m = sim.master_at(0.0);
        EXPECT_EQ(tai_ns(snap, m), m);            // identity
        EXPECT_EQ(tai_ns(snap, m), tr.tai_ns(m));  // snapshot == member
    }

    // 1-sample (raw offset) then filtered (>=2): snapshot must reproduce the
    // member at several propagation offsets past the last update.
    double t = 0.0;
    for (int k = 0; k < 300; ++k, t += 0.25) {
        tr.add_sample(sim.master_at(t), sim.utc_at(t));
        auto const snap = tr.snapshot();
        for (double const dp : {0.0, 0.05, 0.1, 0.2}) {
            std::int64_t const m = sim.master_at(t + dp);
            EXPECT_EQ(tai_ns(snap, m), tr.tai_ns(m));
        }
    }
}

TEST(gps_tai_translator, reset_returns_to_identity)
{
    SimClocks sim;
    GpsTaiTranslator tr;
    tr.add_sample(sim.master_at(0.0), sim.utc_at(0.0));
    EXPECT_TRUE(tr.has_sample());
    tr.reset();
    EXPECT_FALSE(tr.has_sample());
    EXPECT_EQ(tr.tai_ns(42), std::int64_t{42});
}

// Guards the udptun_session TAI-translation invariant (two-node hardware case)
// run (2026-06-10): the wire timestamp must be stamped through ONE consistent
// master-time source for both TX and RX, or the per-direction error fails to
// cancel and the measured fwd+rev "RTT" goes impossibly negative.
//
// In the session, RX and the 4 Hz training sampler both read the master via
// `master_clock_.wire_ns(mraw)` (the ptp4l mraw->PHC mapping), but the RT-mode
// TX path stamped the presentation time from `bridge->now_ns()`. Those two
// "master now" readings differ by a large, PHC-epoch-dependent constant D
// (~±200 ms, observed to flip sign across a reboot). Because the sender's
// stamp carries -D and the receiver's does not, D does NOT cancel in fwd+rev.
//
// This models two clock-aligned nodes (different PHC epochs) and shows:
//   - stamping TX via the bridge-now source (the broken path) yields fwd+rev < 0;
//   - stamping TX via the SAME master_clock(mraw) source RX uses (the correct path)
//     recovers the true round-trip transit.
TEST(gps_tai_translator, tx_must_share_rx_master_source)
{
    constexpr std::int64_t k_tai = 37'000'000'000LL;
    // True time == utc == mraw (1:1). Each node's PHC master = mraw + epoch.
    // bridge_now() diverges from master(mraw) by a constant D — the source of the error.
    constexpr std::int64_t epoch_a = 3'000'000'000'000'000LL;  // ~34.7 days
    constexpr std::int64_t epoch_e = 3'274'000'000'000'000LL;  // ~37.9 days (node-a-like)
    constexpr std::int64_t d_a = 180'000'000LL;                // 180 ms bridge/translator gap
    constexpr std::int64_t d_e = 205'000'000LL;
    constexpr std::int64_t t_ea = 12'000'000LL;  // e->a transit (12 ms)
    constexpr std::int64_t t_ae = 11'000'000LL;  // a->e transit (11 ms) — real path is ~symmetric

    auto build = [](std::int64_t epoch) {
        GpsTaiTranslator tr;  // trained on (master(mraw), utc), master = mraw + epoch
        std::int64_t t = 1'700'000'000'000'000'000LL;
        for (int k = 0; k < 80; ++k, t += 250'000'000LL) {
            tr.add_sample(t + epoch, t);
        }
        return tr;
    };
    auto a = build(epoch_a);
    auto e = build(epoch_e);
    auto master_a = [&](std::int64_t mraw) { return mraw + epoch_a; };
    auto master_e = [&](std::int64_t mraw) { return mraw + epoch_e; };

    std::int64_t const send = 1'700'000'010'000'000'000LL;  // a true instant during the run

    // RX always stamps via the node's master(mraw) — the consistent path.
    // e->a: E sends at `send`, A receives at send + t_ea.
    std::int64_t const rx_fwd = a.tai_ns(master_a(send + t_ea));
    // a->e: A sends at `send`, E receives at send + t_ae.
    std::int64_t const rx_rev = e.tai_ns(master_e(send + t_ae));

    // --- BROKEN PATH: TX stamps via bridge_now == master(mraw) + D ---
    std::int64_t const tx_fwd_bug = e.tai_ns(master_e(send) + d_e);
    std::int64_t const tx_rev_bug = a.tai_ns(master_a(send) + d_a);
    std::int64_t const fwd_bug = rx_fwd - tx_fwd_bug;
    std::int64_t const rev_bug = rx_rev - tx_rev_bug;
    // The hallmark of the broken path: an impossible negative round-trip.
    EXPECT_TRUE((fwd_bug + rev_bug) < 0);

    // --- CORRECT PATH: TX stamps via the SAME master(mraw) source RX uses ---
    std::int64_t const tx_fwd_fix = e.tai_ns(master_e(send));
    std::int64_t const tx_rev_fix = a.tai_ns(master_a(send));
    std::int64_t const fwd_fix = rx_fwd - tx_fwd_fix;
    std::int64_t const rev_fix = rx_rev - tx_rev_fix;
    // Each direction recovers its true transit; the sum is the real RTT.
    EXPECT_TRUE(std::llabs(fwd_fix - t_ea) < 2'000);  // < 2 us
    EXPECT_TRUE(std::llabs(rev_fix - t_ae) < 2'000);
    EXPECT_TRUE(std::llabs((fwd_fix + rev_fix) - (t_ea + t_ae)) < 4'000);
}

TEST_MAIN(statusbar_ptpclient, ptpclient_tai_translator_test)
