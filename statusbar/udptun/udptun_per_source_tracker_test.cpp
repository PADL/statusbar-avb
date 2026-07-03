// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/udptun/udptun_per_source_tracker.hpp"

#include "statusbar/test/test.hpp"

#include <cstddef>
#include <cstdint>

namespace {

using namespace statusbar;
using namespace statusbar::udptun;
using namespace statusbar::stats;

auto cfg() -> AtomicHistogramConfig
{
    return AtomicHistogramConfig{.low_ns = 0, .high_ns = 256'000, .bin_width_ns = 1'000};
}

auto eui(uint8_t a) -> ieee::Eui64
{
    return ieee::Eui64{a, 0, 0, 0xFF, 0xFE, 0, 0, 0};
}

auto count_in_use(PerSourceTracker const& t) -> size_t
{
    size_t n = 0;
    for (auto const& s : t.sources()) {
        if (s.in_use.load()) {
            ++n;
        }
    }
    return n;
}

TEST(udptun_pst, observe_creates_new_entry)
{
    PerSourceTracker t{cfg(), 4};
    auto* st = t.observe(eui(1), /*seq=*/0, /*latency_ns=*/10, /*rx_gptp_ns=*/110, /*interval_us=*/1000);
    EXPECT_TRUE(st != nullptr);
    EXPECT_EQ(st->received_count.load(), uint64_t{1});
    EXPECT_EQ(st->first_seq.load(), uint32_t{0});
    EXPECT_EQ(st->last_seq.load(), uint32_t{0});
    EXPECT_EQ(st->stats->histogram.snapshot().total_count(), int64_t{1});
}

TEST(udptun_pst, in_order_increments_received_only)
{
    PerSourceTracker t{cfg(), 4};
    auto* st = t.observe(eui(1), 0, 10, 110, 1000);
    t.observe(eui(1), 1, 15, 215, 1000);
    t.observe(eui(1), 2, 20, 320, 1000);
    EXPECT_EQ(st->received_count.load(), uint64_t{3});
    EXPECT_EQ(st->out_of_order_count.load(), uint64_t{0});
    EXPECT_EQ(st->duplicate_count.load(), uint64_t{0});
    EXPECT_EQ(st->last_seq.load(), uint32_t{2});
}

TEST(udptun_pst, out_of_order_counts_correctly)
{
    PerSourceTracker t{cfg(), 4};
    t.observe(eui(1), 0, 10, 110, 1000);
    t.observe(eui(1), 5, 15, 215, 1000);
    auto* st = t.observe(eui(1), 3, 20, 320, 1000);  // out of order
    EXPECT_EQ(st->out_of_order_count.load(), uint64_t{1});
    EXPECT_EQ(st->last_seq.load(), uint32_t{5});  // unchanged by ooo
}

TEST(udptun_pst, duplicate_counts_correctly)
{
    PerSourceTracker t{cfg(), 4};
    t.observe(eui(1), 0, 10, 110, 1000);
    t.observe(eui(1), 5, 15, 215, 1000);
    auto* st = t.observe(eui(1), 5, 15, 265, 1000);  // duplicate of 5
    EXPECT_EQ(st->duplicate_count.load(), uint64_t{1});
    EXPECT_EQ(st->last_seq.load(), uint32_t{5});
}

TEST(udptun_pst, sequence_wrap_handled_as_in_order)
{
    PerSourceTracker t{cfg(), 4};
    t.observe(eui(1), 0xFFFFFFFEU, 10, 110, 1000);
    auto* st = t.observe(eui(1), 0U, 15, 215, 1000);  // wrap forward by 2
    EXPECT_EQ(st->out_of_order_count.load(), uint64_t{0});
    EXPECT_EQ(st->last_seq.load(), uint32_t{0});
    EXPECT_EQ(st->received_count.load(), uint64_t{2});
}

TEST(udptun_pst, multiple_sources_independent)
{
    PerSourceTracker t{cfg(), 4};
    t.observe(eui(1), 0, 10, 110, 1000);
    t.observe(eui(2), 0, 20, 120, 1000);
    EXPECT_EQ(count_in_use(t), size_t{2});
    auto* s1 = t.observe(eui(1), 1, 15, 215, 1000);
    auto* s2 = t.observe(eui(2), 1, 30, 230, 1000);
    EXPECT_TRUE(s1 != s2);
    EXPECT_EQ(s1->received_count.load(), uint64_t{2});
    EXPECT_EQ(s2->received_count.load(), uint64_t{2});
}

TEST(udptun_pst, lru_eviction_at_capacity)
{
    PerSourceTracker t{cfg(), 2};
    t.observe(eui(1), 0, 10, 110, 1000);
    t.observe(eui(2), 0, 20, 220, 1000);
    EXPECT_EQ(count_in_use(t), size_t{2});
    t.observe(eui(3), 0, 30, 330, 1000);  // evicts oldest (eui(1))
    EXPECT_EQ(count_in_use(t), size_t{2});
    auto* re = t.observe(eui(1), 0, 40, 440, 1000);
    EXPECT_EQ(re->received_count.load(), uint64_t{1});  // fresh entry
}

TEST(udptun_pst, dropped_invalid_counter)
{
    PerSourceTracker t{cfg(), 4};
    EXPECT_EQ(t.dropped_invalid(), uint64_t{0});
    t.increment_dropped_invalid();
    t.increment_dropped_invalid();
    EXPECT_EQ(t.dropped_invalid(), uint64_t{2});
}

TEST(udptun_pst, truncated_counter)
{
    PerSourceTracker t{cfg(), 4};
    EXPECT_EQ(t.truncated(), uint64_t{0});
    t.increment_truncated();
    t.increment_truncated();
    t.increment_truncated();
    EXPECT_EQ(t.truncated(), uint64_t{3});
    EXPECT_EQ(t.dropped_invalid(), uint64_t{0});  // independent counter
}

TEST(udptun_pst, zero_capacity_returns_null)
{
    PerSourceTracker t{cfg(), 0};
    auto* st = t.observe(eui(1), 0, 100, 110, 1000);
    EXPECT_TRUE(st == nullptr);
}

TEST(udptun_pst, first_observation_is_not_duplicate_or_out_of_order)
{
    // Regression for the implicit "first observation" path: even though
    // init_state sets last_seq = seq, observe must not classify the first
    // packet as a duplicate (which would happen if we treated delta==0
    // uniformly).
    PerSourceTracker t{cfg(), 4};
    auto* st = t.observe(eui(1), /*seq=*/42, 100, 110, 1000);
    EXPECT_TRUE(st != nullptr);
    EXPECT_EQ(st->received_count.load(), uint64_t{1});
    EXPECT_EQ(st->duplicate_count.load(), uint64_t{0});
    EXPECT_EQ(st->out_of_order_count.load(), uint64_t{0});
    EXPECT_EQ(st->first_seq.load(), uint32_t{42});
    EXPECT_EQ(st->last_seq.load(), uint32_t{42});

    // A second arrival of seq=42 IS a duplicate.
    t.observe(eui(1), 42, 200, 215, 1000);
    EXPECT_EQ(st->duplicate_count.load(), uint64_t{1});
}

TEST(udptun_pst, observation_after_lru_eviction_is_fresh_first_observation)
{
    // After LRU eviction reuses the slot, the next observe of that sender
    // must NOT see stale duplicate/out-of-order classification carried
    // over from the previous occupant of the slot.
    PerSourceTracker t{cfg(), 2};
    t.observe(eui(1), /*seq=*/100, 100, 110, 1000);
    t.observe(eui(2), /*seq=*/0, 200, 220, 1000);
    t.observe(eui(3), /*seq=*/0, 300, 330, 1000);  // evicts eui(1)
    auto* st = t.observe(eui(1), /*seq=*/0, 400, 440, 1000);
    EXPECT_TRUE(st != nullptr);
    EXPECT_EQ(st->received_count.load(), uint64_t{1});
    EXPECT_EQ(st->duplicate_count.load(), uint64_t{0});
    EXPECT_EQ(st->out_of_order_count.load(), uint64_t{0});
    EXPECT_EQ(st->first_seq.load(), uint32_t{0});
}

}  // namespace

TEST_MAIN(statusbar_udptun, udptun_per_source_tracker_test)
