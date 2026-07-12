// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// ReactorMarshal tests: cross-thread task posting wakes the fd, tasks run
// in order on the draining thread, and large captures (a full marshaled
// AEM outcome) fit the task budget.

#include "statusbar/atdecc_tools/atdecc_reactor_marshal.hpp"

#include "statusbar/test/test.hpp"

#include <poll.h>

#include <array>
#include <cstdint>
#include <thread>
#include <vector>

using statusbar::atdecc_tools::ReactorMarshal;

namespace {

/// Wait (bounded) until the marshal's fd is readable.
auto wait_readable(int const fd, int const timeout_ms) -> bool
{
    pollfd p{.fd = fd, .events = POLLIN, .revents = 0};
    return ::poll(&p, 1, timeout_ms) == 1 && (p.revents & POLLIN) != 0;
}

}  // namespace

TEST(reactor_marshal, posts_wake_and_run_in_order)
{
    ReactorMarshal marshal;
    EXPECT_TRUE(marshal.valid());
    EXPECT_TRUE(marshal.fd() >= 0);

    std::vector<int> ran;
    marshal.post([&ran] { ran.push_back(1); });
    marshal.post([&ran] { ran.push_back(2); });
    marshal.post([&ran] { ran.push_back(3); });
    EXPECT_EQ(marshal.pending(), 3U);

    EXPECT_TRUE(wait_readable(marshal.fd(), 1000));
    marshal.on_ready(0);
    EXPECT_TRUE(ran == (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(marshal.pending(), 0U);

    // Idle: nothing pending, fd not readable.
    EXPECT_FALSE(wait_readable(marshal.fd(), 0));
}

TEST(reactor_marshal, cross_thread_posting)
{
    ReactorMarshal marshal;
    EXPECT_TRUE(marshal.valid());

    constexpr int TOTAL = 200;
    int ran = 0;
    std::thread poster{[&marshal] {
        for (int i = 0; i < TOTAL; ++i) {
            marshal.post([] {});
        }
    }};
    poster.join();
    // All posts are queued; one drain runs them all (tasks counted via a
    // second batch that observes the first).
    marshal.post([&ran] { ran = static_cast<int>(TOTAL); });
    EXPECT_TRUE(wait_readable(marshal.fd(), 1000));
    marshal.on_ready(0);
    EXPECT_EQ(ran, TOTAL);
    EXPECT_EQ(marshal.pending(), 0U);
}

TEST(reactor_marshal, large_task_capture_fits)
{
    ReactorMarshal marshal;
    EXPECT_TRUE(marshal.valid());

    // A capture the size of a marshaled AEM outcome (512B response copy
    // plus bookkeeping) must fit the task budget.
    std::array<uint8_t, 600> payload{};
    payload.fill(0xAB);
    uint8_t seen = 0;
    marshal.post([payload, &seen] { seen = payload[599]; });
    EXPECT_TRUE(wait_readable(marshal.fd(), 1000));
    marshal.on_ready(0);
    EXPECT_EQ(seen, 0xAB);
}

//
// Test Runner
//

TEST_MAIN(statusbar_atdecc_tools, atdecc_reactor_marshal_test)
