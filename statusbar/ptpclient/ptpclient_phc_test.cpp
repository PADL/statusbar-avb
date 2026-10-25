// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ptpclient/ptpclient_phc.hpp"

#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <print>
#include <string>

using namespace statusbar;
using namespace statusbar::ptpclient;

TEST(ptpclient_phc, the_narrowest_bracket_wins)
{
    std::array<PhcBracket, 4> const brackets{{
        {.before_ns = 1000, .phc_ns = 50'000, .after_ns = 1900},
        {.before_ns = 2000, .phc_ns = 51'000, .after_ns = 2300},  // 300 ns: the narrowest
        {.before_ns = 3000, .phc_ns = 52'000, .after_ns = 2000},  // disordered: skipped
        {.before_ns = 4000, .phc_ns = 53'000, .after_ns = 4400},
    }};
    auto const best = narrowest_bracket(brackets);
    EXPECT_TRUE(best.has_value());
    EXPECT_EQ(best->raw_ns, int64_t{2150});
    EXPECT_EQ(best->phc_ns, int64_t{51'000});
    EXPECT_EQ(best->width_ns, int64_t{300});
    // the first of equally narrow brackets
    std::array<PhcBracket, 2> const tie{
        {{.before_ns = 0, .phc_ns = 7, .after_ns = 10}, {.before_ns = 20, .phc_ns = 8, .after_ns = 30}}};
    EXPECT_EQ(narrowest_bracket(tie).value_or(PhcReading{}).phc_ns, int64_t{7});
    EXPECT_FALSE(narrowest_bracket({}).has_value());
    EXPECT_FALSE(narrowest_bracket(std::span{brackets}.subspan(2, 1)).has_value());
}

TEST(ptpclient_phc, phc0_is_interpolated_at_the_phcd_instant)
{
    PhcReading const first0{.raw_ns = 1000, .phc_ns = 5000, .width_ns = 20};
    PhcReading const d{.raw_ns = 1500, .phc_ns = 9000, .width_ns = 30};
    PhcReading const second0{.raw_ns = 2000, .phc_ns = 6000, .width_ns = 40};
    auto const pair = cross_timestamp(first0, d, second0);
    EXPECT_TRUE(pair.has_value());
    EXPECT_EQ(pair->phc0_ns, int64_t{5500});
    EXPECT_EQ(pair->phcd_ns, int64_t{9000});
    EXPECT_EQ(pair->raw_ns, int64_t{1500});
    EXPECT_EQ(pair->uncertainty_ns, int64_t{15 + 15});  // half of 30, plus half the mean of 20 and 40
}

TEST(ptpclient_phc, interpolation_holds_at_tai_times)
{
    int64_t const tai = int64_t{1'800'000'000} * 1'000'000'000;  // 2027, in ns
    PhcReading const first0{.raw_ns = 100, .phc_ns = tai, .width_ns = 0};
    PhcReading const d{.raw_ns = 20'100, .phc_ns = 7, .width_ns = 0};
    PhcReading const second0{.raw_ns = 40'100, .phc_ns = tai + 40'004, .width_ns = 0};
    auto const pair = cross_timestamp(first0, d, second0);
    EXPECT_TRUE(pair.has_value());
    EXPECT_EQ(pair->phc0_ns, tai + 20'002);
}

TEST(ptpclient_phc, a_disordered_triple_is_refused)
{
    PhcReading const a{.raw_ns = 1000, .phc_ns = 1};
    PhcReading const b{.raw_ns = 2000, .phc_ns = 2};
    EXPECT_FALSE(cross_timestamp(b, PhcReading{.raw_ns = 1500}, a).has_value());  // PHC0 reads reversed
    EXPECT_FALSE(cross_timestamp(a, PhcReading{.raw_ns = 2500}, b).has_value());  // PHC_d after both
    EXPECT_FALSE(cross_timestamp(a, PhcReading{.raw_ns = 500}, b).has_value());   // PHC_d before both
    EXPECT_FALSE(cross_timestamp(a, PhcReading{.raw_ns = 1000}, a).has_value());  // no span
    EXPECT_TRUE(cross_timestamp(a, PhcReading{.raw_ns = 2000}, b).has_value());   // the ends are inside
}

TEST(ptpclient_phc, open_refuses_bad_configuration)
{
    EXPECT_FALSE(PhcDevice::open("").has_value());
    EXPECT_FALSE(PhcDevice::open("/dev/ptp0", PhcDeviceConfig{.samples = 0}).has_value());
    EXPECT_FALSE(PhcDevice::open("/dev/ptp0", PhcDeviceConfig{.samples = 26}).has_value());
    EXPECT_FALSE(PhcDevice::open("/nonexistent/ptp").has_value());
}

TEST(ptpclient_phc, real_phcs_cross_timestamp_within_their_brackets)
{
    // Needs read access to /dev/ptpN (root); STATUSBAR_PHC0 / STATUSBAR_PHCD choose the devices.
    char const* const env0 = std::getenv("STATUSBAR_PHC0");
    char const* const envd = std::getenv("STATUSBAR_PHCD");
    std::string const phc0 = env0 != nullptr ? env0 : "/dev/ptp0";
    std::string const phcd = envd != nullptr ? envd : phc0;
    auto pair = PhcCrossTimestamp::open(phc0, phcd);
    if (!pair) {
        std::println("  skipped: cannot open {} and {} ({})", phc0, phcd, pair.error().message());
        return;
    }
    int64_t worst = 0;
    int64_t previous_raw = 0;
    for (int i = 0; i < 16; ++i) {
        auto const sample = pair->measure();
        EXPECT_TRUE(sample.has_value());
        if (!sample) {
            return;
        }
        EXPECT_TRUE(sample->raw_ns > previous_raw);
        previous_raw = sample->raw_ns;
        EXPECT_TRUE(sample->uncertainty_ns >= 0 && sample->uncertainty_ns < 100'000);
        if (phc0 == phcd) {
            // one clock against itself: the pair agrees to within the brackets
            EXPECT_TRUE(std::abs(sample->phc0_ns - sample->phcd_ns) <= (2 * sample->uncertainty_ns) + 100);
        }
        worst = std::max(worst, sample->uncertainty_ns);
    }
    std::println(
        "  {} ({}) / {} ({}): worst uncertainty {} ns",
        phc0,
        static_cast<int>(pair->phc0().method()),
        phcd,
        static_cast<int>(pair->phcd().method()),
        worst);
}

TEST_MAIN(statusbar_ptpclient, ptpclient_phc_test)
