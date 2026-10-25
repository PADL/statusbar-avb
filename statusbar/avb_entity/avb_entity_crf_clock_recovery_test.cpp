// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// CrfClockRecovery tests (kit phase 3b): feed synthetic CRF timelines and
// check the recovered rate r converges to the remote/local frequency ratio,
// the phase estimate carries the talker's presentation lead, and the lock
// gate holds until min_samples. Pure — no sockets, no threads.

#include "statusbar/avb_entity/avb_entity_crf_clock_recovery.hpp"

#include "statusbar/test/test.hpp"

#include <cmath>
#include <cstdint>

using namespace statusbar;
using namespace statusbar::avb_entity;

namespace {

// Milan-ish cadence: one CRF PDU (1 timestamp) every 2 ms of local time.
constexpr int64_t kRxStep = 2'000'000;

/// Drive `n` PDUs of a remote clock running at `ratio` x local, with a fixed
/// presentation lead, starting at local time t0.
void drive(CrfClockRecovery& rec, double const ratio, int64_t const lead_ns, int const n, int64_t const t0 = 1'000'000'000)
{
    for (int i = 0; i < n; ++i) {
        int64_t const rx = t0 + (static_cast<int64_t>(i) * kRxStep);
        auto const remote =
            static_cast<uint64_t>((static_cast<double>(rx - t0) * ratio) + static_cast<double>(t0) + static_cast<double>(lead_ns));
        rec.on_crf_timestamp(remote, rx);
    }
}

}  // namespace

TEST(crf_recovery, converges_to_remote_rate)
{
    // Remote media clock 50 ppm fast, 1 ms presentation lead.
    CrfClockRecovery rec{};
    drive(rec, 1.000050, 1'000'000, 500);
    auto const est = rec.estimate();
    EXPECT_TRUE(est.locked);
    EXPECT_TRUE(est.samples == 500U);
    // Recovered rate within 2 ppm of truth.
    EXPECT_TRUE(std::abs(est.r - 1.000050) < 2e-6);
    // Phase estimate carries the lead (within 100 us of the truth).
    EXPECT_TRUE(std::llabs(est.filtered_offset_ns - 1'000'000) < 100'000);
}

TEST(crf_recovery, nominal_rate_stays_at_unity)
{
    CrfClockRecovery rec{};
    drive(rec, 1.0, 0, 200);
    auto const est = rec.estimate();
    EXPECT_TRUE(est.locked);
    EXPECT_TRUE(std::abs(est.r - 1.0) < 1e-6);
}

TEST(crf_recovery, lock_waits_for_min_samples)
{
    CrfClockRecovery::Config cfg{};
    cfg.min_samples = 16;
    CrfClockRecovery rec{cfg};
    drive(rec, 1.0, 0, 8);
    EXPECT_FALSE(rec.estimate().locked);  // not enough samples yet
    drive(rec, 1.0, 0, 16, 1'000'000'000 + (8 * kRxStep));
    EXPECT_TRUE(rec.estimate().locked);
}

TEST(crf_recovery, repeated_rx_time_adds_no_rate_sample)
{
    // Two timestamps from the same PDU share one receive wake: the second
    // must not feed a zero-dt sample into the filter (but still counts).
    CrfClockRecovery rec{};
    rec.on_crf_timestamp(1'000'000'000, 1'000'000'000);
    rec.on_crf_timestamp(1'000'002'000, 1'000'000'000);  // same rx time
    auto const est = rec.estimate();
    EXPECT_TRUE(est.samples == 2U);
    EXPECT_TRUE(std::abs(est.r - 1.0) < 1e-9);  // no bogus rate from dt=0
}

TEST(crf_recovery, consumer_adapter_feeds_the_filter)
{
    CrfClockRecovery rec{};
    auto consumer = rec.make_consumer();
    for (int i = 0; i < 32; ++i) {
        int64_t const rx = 1'000'000'000 + (static_cast<int64_t>(i) * kRxStep);
        consumer(static_cast<uint64_t>(rx), 0, rx, false, 1);  // via the StreamCrfFn shape
    }
    EXPECT_TRUE(rec.estimate().locked);
    EXPECT_TRUE(rec.estimate().samples == 32U);
}

// IEEE 1722-2016 Clause 10.4: a toggled mr is a media clock restart, so the estimate starts again;
// a steady mr, set or clear, is none.
TEST(crf_recovery, a_toggled_mr_restarts_the_estimate)
{
    CrfClockRecovery rec{};
    auto consumer = rec.make_consumer();
    int64_t rx = 1'000'000'000;
    for (int i = 0; i < 32; ++i, rx += kRxStep) {
        consumer(static_cast<uint64_t>(rx), 0, rx, false, 1);
    }
    EXPECT_TRUE(rec.estimate().locked);
    consumer(static_cast<uint64_t>(rx), 0, rx, true, 1);
    rx += kRxStep;
    EXPECT_FALSE(rec.estimate().locked);
    EXPECT_TRUE(rec.estimate().samples == 1U);
    for (int i = 0; i < 31; ++i, rx += kRxStep) {
        consumer(static_cast<uint64_t>(rx), 0, rx, true, 1);
    }
    EXPECT_TRUE(rec.estimate().locked);
    EXPECT_TRUE(rec.estimate().samples == 32U);
}

// A new source generation (the input rebound to another talker) starts the estimate again.
TEST(crf_recovery, a_new_source_restarts_the_estimate)
{
    CrfClockRecovery rec{};
    auto consumer = rec.make_consumer();
    int64_t rx = 1'000'000'000;
    for (int i = 0; i < 32; ++i, rx += kRxStep) {
        consumer(static_cast<uint64_t>(rx), 0, rx, false, 1);
    }
    EXPECT_TRUE(rec.estimate().locked);
    consumer(static_cast<uint64_t>(rx), 0, rx, false, 2);
    EXPECT_FALSE(rec.estimate().locked);
    EXPECT_TRUE(rec.estimate().samples == 1U);

    // A second consumer (the recovery moved to another input) starts afresh at its source 1.
    for (int i = 0; i < 32; ++i, rx += kRxStep) {
        consumer(static_cast<uint64_t>(rx), 0, rx, false, 2);
    }
    EXPECT_TRUE(rec.estimate().locked);
    auto moved = rec.make_consumer();
    moved(static_cast<uint64_t>(rx), 0, rx, false, 2);
    EXPECT_TRUE(rec.estimate().samples == 1U);
}

TEST_MAIN(statusbar_avb_entity, avb_entity_crf_clock_recovery_test)
