// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/udptun/udptun_loss_snapshot.hpp"

#include "statusbar/itc/itc_atomic_triple_buffer.hpp"
#include "statusbar/test/test.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

using namespace statusbar;
using statusbar::udptun::LossSnapshot;

// The figures the owning thread takes: the sends old enough to have come back,
// and the redundancy slots retired behind the grace window with what landed in
// them. Ten packets a millisecond apart; the primaries of 5 ms and 7 ms never
// came back, the redundant copy of 5 ms did.
TEST(udptun_loss_snapshot, takes_the_figures_as_of_the_grace_cutoff)
{
    constexpr int64_t ms = 1'000'000;
    udptun::TxHistory history{};
    udptun::RedundantRxTracker redundancy{/*slot_width_ns=*/ms};
    for (int64_t i = 0; i < 10; ++i) {
        history.record(i * ms, static_cast<uint64_t>(i + 1));
        if (i != 5 && i != 7) {
            redundancy.on_primary(i * ms);
        }
    }
    EXPECT_TRUE(redundancy.on_redundant(5 * ms));

    // now = 10 ms, grace 3 ms: sends by 7 ms count (eight of them); with the
    // latest PT at 12 ms the slots up to 9 ms are behind the grace and retired.
    auto const snap = udptun::take_loss_snapshot(
        history, redundancy, /*sent_total=*/10, /*now_mraw=*/10 * ms, /*grace_ns=*/3 * ms, /*ref_pt=*/12 * ms);
    EXPECT_EQ(snap.sent_total, 10U);
    EXPECT_EQ(snap.sent_at_cutoff, 8U);
    EXPECT_EQ(snap.primary_received, 8U);
    EXPECT_EQ(snap.recovered, 1U);
    EXPECT_EQ(snap.true_loss, 1U);
}

// The reporter reads the snapshot whole: never a sent_total from one publish
// beside a true_loss from another. The publisher writes snapshots whose five
// fields all carry the same number; a reader that ever sees two fields differ
// has read a torn one.
TEST(udptun_loss_snapshot, the_reporter_sees_one_publish_at_a_time)
{
    statusbar::itc::AtomicTripleBuffer<LossSnapshot> published{};
    std::atomic<bool> done{false};
    std::atomic<uint64_t> torn{0};
    std::atomic<uint64_t> reads{0};

    std::thread reporter{[&] {
        while (!done.load(std::memory_order_acquire)) {
            LossSnapshot const s = published.consume();
            bool const whole = s.sent_at_cutoff == s.sent_total && s.recovered == s.sent_total && s.true_loss == s.sent_total &&
                s.primary_received == s.sent_total;
            if (!whole) {
                torn.fetch_add(1, std::memory_order_relaxed);
            }
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    }};
    for (uint64_t k = 1; k <= 200'000; ++k) {
        published.publish(
            LossSnapshot{.sent_total = k, .sent_at_cutoff = k, .recovered = k, .true_loss = k, .primary_received = k});
    }
    done.store(true, std::memory_order_release);
    reporter.join();

    EXPECT_EQ(torn.load(), 0U);
    EXPECT_TRUE(reads.load() > 0U);
}

TEST_MAIN(statusbar_udptun, udptun_loss_snapshot_test)
