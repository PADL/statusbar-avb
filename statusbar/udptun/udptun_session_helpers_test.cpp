// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Regression tests for the RX-path hardening (session C1 fix):
//   - same_host(): the Layer 1 source-address gate predicate.
//   - presentation_time_plausible(): the Layer 2 wire-PT plausibility gate
//     that keeps an attacker-controlled presentation time from indexing the
//     redundancy slot map out of bounds / driving scan() ~forever.

#include "statusbar/udptun/udptun_session_helpers.hpp"

#include "statusbar/net/net.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>
#include <limits>

using namespace statusbar;

TEST(session_helpers, same_host_matches_ipv4_ignoring_port)
{
    auto const a = net::SocketAddress::ipv4(192, 0, 2, 10, 17220);
    auto const b_same_ip_diff_port = net::SocketAddress::ipv4(192, 0, 2, 10, 40000);
    auto const c_diff_ip = net::SocketAddress::ipv4(192, 0, 2, 11, 17220);

    // NAT may rewrite the source port, so host-only match must hold.
    EXPECT_TRUE(udptun::same_host(a, b_same_ip_diff_port));
    EXPECT_FALSE(udptun::same_host(a, c_diff_ip));
}

TEST(session_helpers, same_host_rejects_family_mismatch_and_invalid)
{
    auto const v4 = net::SocketAddress::ipv4(198, 51, 100, 7, 17220);
    auto const v6 = net::SocketAddress::ipv6_loopback(17220);
    net::SocketAddress const invalid{};

    EXPECT_FALSE(udptun::same_host(v4, v6));
    EXPECT_FALSE(udptun::same_host(v4, invalid));
    EXPECT_FALSE(udptun::same_host(invalid, invalid));
}

TEST(session_helpers, same_host_matches_ipv6)
{
    auto const a = net::SocketAddress::ipv6_loopback(17220);
    auto const b = net::SocketAddress::ipv6_loopback(40000);
    EXPECT_TRUE(udptun::same_host(a, b));
}

TEST(session_helpers, presentation_time_rejects_negative)
{
    // The exact attack: a high-bit-set wire timestamp is negative and would
    // index the slot map out of bounds. Must be rejected at any rx clock.
    constexpr int64_t rx = 1'800'000'000'000'000'000;  // ~2026 TAI ns
    EXPECT_FALSE(udptun::presentation_time_plausible(-1, rx));
    EXPECT_FALSE(udptun::presentation_time_plausible(std::numeric_limits<int64_t>::min(), rx));
}

TEST(session_helpers, presentation_time_rejects_absurd_future)
{
    // INT64_MAX would make self_redundancy scan() iterate ~forever.
    constexpr int64_t rx = 1'800'000'000'000'000'000;
    EXPECT_FALSE(udptun::presentation_time_plausible(std::numeric_limits<int64_t>::max(), rx));
    // Just past the 5 s ceiling is rejected; just under is accepted.
    EXPECT_FALSE(udptun::presentation_time_plausible(rx + 5'000'000'001, rx));
    EXPECT_TRUE(udptun::presentation_time_plausible(rx + 4'999'999'999, rx));
}

TEST(session_helpers, presentation_time_accepts_realistic)
{
    constexpr int64_t rx = 1'800'000'000'000'000'000;
    // A frame slightly ahead of the local wire clock (WCL - transit) and
    // one slightly behind (normal lateness) are both plausible.
    EXPECT_TRUE(udptun::presentation_time_plausible(rx + 20'000'000, rx));   // +20 ms
    EXPECT_TRUE(udptun::presentation_time_plausible(rx - 100'000'000, rx));  // -100 ms
    EXPECT_TRUE(udptun::presentation_time_plausible(0, rx));
    EXPECT_TRUE(udptun::presentation_time_plausible(rx, rx));
}

TEST_MAIN(statusbar_udptun, udptun_session_helpers_test)
