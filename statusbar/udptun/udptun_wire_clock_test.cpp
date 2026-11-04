// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/udptun/udptun_wire_clock.hpp"

#include "statusbar/gptp/gptp_time_bridge.hpp"
#include "statusbar/ptpclient/ptpclient_bridge.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>

using namespace statusbar;

TEST(wire_clock, empty_translator_falls_back_to_realtime)
{
    udptun::WireClock clock{};
    EXPECT_FALSE(static_cast<bool>(clock.translate));

    int64_t const before = udptun::realtime_ns();
    int64_t const w = clock.wire_ns(/*mraw_ns=*/12345);
    int64_t const after = udptun::realtime_ns();

    EXPECT_TRUE(w >= before);
    EXPECT_TRUE(w <= after);
}

TEST(wire_clock, gptp_slave_translator_forwards_to_to_gptp)
{
    gptp::GptpTimeBridge bridge{};
    constexpr int64_t app_anchor = 100'000;
    constexpr int64_t master_anchor = 1'000'000;
    bridge.get_app_time_ns = []() noexcept { return app_anchor; };
    bridge.update_from_anchor(/*rx_local_ns=*/0, /*tx_master_ns=*/master_anchor, /*rate_ratio=*/1.0);

    constexpr int64_t expected_offset = master_anchor - app_anchor;  // 900'000

    udptun::WireClock clock{.translate = udptun::make_gptp_slave_translator(bridge)};
    EXPECT_TRUE(static_cast<bool>(clock.translate));
    EXPECT_EQ(clock.wire_ns(0), expected_offset);
    EXPECT_EQ(clock.wire_ns(500'000), 500'000 + expected_offset);
    EXPECT_EQ(clock.wire_ns(-1'000'000), -1'000'000 + expected_offset);
}

// The ptp4l translator is the bridge's primary PHC<->RAW line applied to
// the RAW instant it is given, nothing else: with a default-constructed
// PtpTimeBridge (rate=1.0, offset=0) it is the identity, exactly, and it
// reads no clock of its own, so two calls with the same input agree.
TEST(wire_clock, ptp4l_translator_is_the_bridge_raw_line)
{
    ptpclient::PtpTimeBridge bridge{};

    auto translator = udptun::make_ptp4l_translator(bridge);

    EXPECT_EQ(translator(0), 0);
    EXPECT_EQ(translator(1'000'000), 1'000'000);
    EXPECT_EQ(translator(1'000'000 + 50'000), 1'050'000);
    EXPECT_EQ(translator(-1'000'000), -1'000'000);

    int64_t const mraw_now = udptun::monotonic_raw_ns();
    EXPECT_EQ(translator(mraw_now), bridge.from_raw_ns(mraw_now));
    EXPECT_EQ(translator(mraw_now), translator(mraw_now));
}

TEST_MAIN(statusbar_udptun, udptun_wire_clock_test)
