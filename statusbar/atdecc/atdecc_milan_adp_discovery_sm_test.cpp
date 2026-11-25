// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The Milan v1.3 listener discovery state machine, section 5.6.4: every
// transition of Table 5.59 and the checks that pick it.

#include "statusbar/atdecc/atdecc_milan_adp_discovery_sm.hpp"

#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/test/test.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

using namespace statusbar;
using namespace statusbar::atdecc;
using statusbar::ieee::Eui64;
using TimePoint = statusbar::sm::TimePoint;
using State = MilanDiscoveryState;
using Event = MilanDiscoveryEvent;

namespace {

constexpr Eui64 TALKER{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
constexpr ClockIdentity GRANDMASTER{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x00, 0x01};
constexpr ClockIdentity OTHER_GRANDMASTER{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x00, 0x02};
constexpr MilanGptpState OWN{.grandmaster_id = GRANDMASTER, .domain_number = 0};

auto at(int const ms) -> TimePoint
{
    return TimePoint{std::chrono::milliseconds{ms}};
}

/// An ENTITY_AVAILABLE from the talker, good for 10 seconds (valid_time 5).
auto available(uint32_t const available_index) -> AdpDu
{
    AdpDu adp{};
    adp.init_entity_available(TALKER, 5);
    adp.available_index = available_index;
    adp.gptp_grandmaster_id = GRANDMASTER;
    adp.gptp_domain_number = 0;
    adp.interface_index = 1;
    adp.entity_capabilities = entity_capabilities::AEM_SUPPORTED | entity_capabilities::AEM_CONFIGURATION_INDEX_VALID;
    adp.current_configuration_index = 0;
    return adp;
}

auto departing() -> AdpDu
{
    AdpDu adp{};
    adp.init_entity_departing(TALKER);
    adp.interface_index = 1;
    return adp;
}

struct Discovery
{
    MilanDiscoveryContext ctx{};
    MilanDiscoveryStateMachine<> sm{};

    [[nodiscard]] auto state() const -> State { return sm.current_state(); }

    /// Run @p adp through the checks and the machine; true when it was an event.
    auto receive(AdpDu const& adp, TimePoint const now, MilanGptpState const& own = OWN) -> bool
    {
        auto const event = milan_discovery_event_for(ctx, state(), adp, own);
        ctx.departed = false;
        ctx.discovered = false;
        if (event) {
            ctx.rcvd = adp;
            sm.handle_event(ctx, *event, now);
        }
        return event.has_value();
    }

    void start() { sm.handle_event(ctx, Event::Start, at(0)); }

    /// Started and discovered by an ENTITY_AVAILABLE with available_index 7 at t = 1 s.
    void discover()
    {
        start();
        EXPECT_TRUE(receive(available(7), at(1000)));
        EXPECT_EQ(state(), State::TkDiscovered);
    }
};

}  // namespace

TEST(milan_discovery, a_machine_not_started_ignores_adp)
{
    Discovery d;
    EXPECT_EQ(d.state(), State::Stopped);
    EXPECT_FALSE(d.receive(available(1), at(0)));
    EXPECT_FALSE(d.receive(departing(), at(0)));
    EXPECT_EQ(d.state(), State::Stopped);
}

TEST(milan_discovery, started_the_talker_is_not_discovered_and_nothing_is_reported)
{
    Discovery d;
    d.start();
    EXPECT_EQ(d.state(), State::TkNotDiscovered);
    EXPECT_FALSE(d.ctx.departed);
    EXPECT_FALSE(d.ctx.discovered);
    EXPECT_FALSE(d.ctx.timer.has_value());
}

TEST(milan_discovery, an_entity_available_on_our_gptp_domain_discovers_the_talker)
{
    // 5.6.4.5.1
    Discovery d;
    d.start();
    EXPECT_TRUE(d.receive(available(7), at(1000)));
    EXPECT_EQ(d.state(), State::TkDiscovered);
    EXPECT_TRUE(d.ctx.discovered);
    EXPECT_FALSE(d.ctx.departed);
    EXPECT_EQ(d.ctx.interface_index, 1);
    EXPECT_EQ(d.ctx.available_index, 7U);
    EXPECT_EQ(*d.ctx.timer, at(1000) + std::chrono::seconds{10});  // valid_time 5, in 2 second units
}

TEST(milan_discovery, an_entity_available_on_another_grandmaster_or_domain_is_ignored)
{
    // 5.6.4.5.1 step 1
    Discovery d;
    d.start();
    auto other_gm = available(7);
    other_gm.gptp_grandmaster_id = OTHER_GRANDMASTER;
    EXPECT_FALSE(d.receive(other_gm, at(1000)));
    auto other_domain = available(7);
    other_domain.gptp_domain_number = 1;
    EXPECT_FALSE(d.receive(other_domain, at(1000)));
    // A listener that has no grandmaster yet matches nobody who has one.
    EXPECT_FALSE(d.receive(available(7), at(1000), MilanGptpState{}));
    EXPECT_EQ(d.state(), State::TkNotDiscovered);
    EXPECT_FALSE(d.receive(departing(), at(1000)));  // "-" in Table 5.59
}

TEST(milan_discovery, a_later_entity_available_restarts_the_timeout)
{
    // 5.6.4.5.2 step 3
    Discovery d;
    d.discover();
    EXPECT_TRUE(d.receive(available(8), at(6000)));
    EXPECT_EQ(d.state(), State::TkDiscovered);
    EXPECT_FALSE(d.ctx.departed);
    EXPECT_FALSE(d.ctx.discovered);
    EXPECT_EQ(d.ctx.available_index, 8U);
    EXPECT_EQ(*d.ctx.timer, at(16'000));
    // The grandmaster is not looked at again while the talker stays up.
    auto moved = available(9);
    moved.gptp_grandmaster_id = OTHER_GRANDMASTER;
    EXPECT_TRUE(d.receive(moved, at(7000)));
    EXPECT_EQ(d.state(), State::TkDiscovered);
    EXPECT_FALSE(d.ctx.departed);
}

TEST(milan_discovery, a_message_on_another_interface_index_is_ignored_once_discovered)
{
    // 5.6.4.5.2 step 1 and 5.6.4.5.3 step 1
    Discovery d;
    d.discover();
    auto elsewhere = available(8);
    elsewhere.interface_index = 2;
    EXPECT_FALSE(d.receive(elsewhere, at(2000)));
    auto leaving = departing();
    leaving.interface_index = 2;
    EXPECT_FALSE(d.receive(leaving, at(2000)));
    EXPECT_EQ(d.state(), State::TkDiscovered);
    EXPECT_EQ(*d.ctx.timer, at(11'000));
}

TEST(milan_discovery, an_available_index_that_did_not_rise_is_a_departure_and_a_discovery)
{
    // 5.6.4.5.2 step 2: the talker restarted between two advertisements.
    for (uint32_t const index : {7U, 0U}) {
        Discovery d;
        d.discover();
        EXPECT_TRUE(d.receive(available(index), at(3000)));
        EXPECT_EQ(d.state(), State::TkDiscovered);
        EXPECT_TRUE(d.ctx.departed);
        EXPECT_TRUE(d.ctx.discovered);
        EXPECT_EQ(d.ctx.available_index, index);
        EXPECT_EQ(*d.ctx.timer, at(13'000));
    }
}

TEST(milan_discovery, another_configuration_is_a_departure_and_a_discovery)
{
    // 5.6.4.5.2 step 2
    Discovery d;
    d.discover();
    auto reconfigured = available(8);
    reconfigured.current_configuration_index = 1;
    EXPECT_TRUE(d.receive(reconfigured, at(3000)));
    EXPECT_EQ(d.state(), State::TkDiscovered);
    EXPECT_TRUE(d.ctx.departed);
    EXPECT_TRUE(d.ctx.discovered);
    EXPECT_TRUE(d.ctx.current_configuration_index == uint16_t{1});
    // The same configuration again is an ordinary refresh.
    auto next = available(9);
    next.current_configuration_index = 1;
    EXPECT_TRUE(d.receive(next, at(4000)));
    EXPECT_FALSE(d.ctx.departed);
}

TEST(milan_discovery, a_configuration_index_not_marked_valid_is_not_compared)
{
    // 5.6.4.5.2 step 2 compares the field "if present": IEEE 1722.1-2021
    // 6.2.1.10 gives it a meaning only under AEM_CONFIGURATION_INDEX_VALID.
    Discovery d;
    d.discover();
    auto unmarked = available(8);
    unmarked.entity_capabilities = entity_capabilities::AEM_SUPPORTED;
    unmarked.current_configuration_index = 5;
    EXPECT_TRUE(d.receive(unmarked, at(3000)));
    EXPECT_FALSE(d.ctx.departed);
    EXPECT_FALSE(d.ctx.current_configuration_index.has_value());
    // Marked valid again: there is no earlier value to differ from.
    auto marked = available(9);
    marked.current_configuration_index = 5;
    EXPECT_TRUE(d.receive(marked, at(4000)));
    EXPECT_FALSE(d.ctx.departed);
    // From here on a change is a restart.
    auto changed = available(10);
    changed.current_configuration_index = 6;
    EXPECT_TRUE(d.receive(changed, at(5000)));
    EXPECT_TRUE(d.ctx.departed);
    EXPECT_TRUE(d.ctx.discovered);
}

TEST(milan_discovery, a_valid_time_of_zero_is_good_for_one_unit)
{
    Discovery d;
    d.start();
    auto brief = available(7);
    brief.set_valid_time(0);
    EXPECT_TRUE(d.receive(brief, at(1000)));
    EXPECT_EQ(d.state(), State::TkDiscovered);
    EXPECT_EQ(*d.ctx.timer, at(3000));
}

TEST(milan_discovery, a_talker_that_restarted_on_another_grandmaster_is_only_departed)
{
    // 5.6.4.5.2 step 2b
    Discovery d;
    d.discover();
    auto moved = available(1);
    moved.gptp_grandmaster_id = OTHER_GRANDMASTER;
    EXPECT_TRUE(d.receive(moved, at(3000)));
    EXPECT_EQ(d.state(), State::TkNotDiscovered);
    EXPECT_TRUE(d.ctx.departed);
    EXPECT_FALSE(d.ctx.discovered);
    EXPECT_FALSE(d.ctx.timer.has_value());
}

TEST(milan_discovery, entity_departing_and_the_timeout_both_report_the_talker_departed)
{
    // 5.6.4.5.3 and 5.6.4.5.4
    Discovery left;
    left.discover();
    EXPECT_TRUE(left.receive(departing(), at(2000)));
    EXPECT_EQ(left.state(), State::TkNotDiscovered);
    EXPECT_TRUE(left.ctx.departed);
    EXPECT_FALSE(left.ctx.timer.has_value());

    Discovery lapsed;
    lapsed.discover();
    lapsed.ctx.departed = false;
    lapsed.sm.handle_event(lapsed.ctx, Event::TmrNoAdp, at(11'000));
    EXPECT_EQ(lapsed.state(), State::TkNotDiscovered);
    EXPECT_TRUE(lapsed.ctx.departed);
    EXPECT_FALSE(lapsed.ctx.timer.has_value());
    // Not discovered, the timer cannot run (x in Table 5.59).
    lapsed.ctx.departed = false;
    lapsed.sm.handle_event(lapsed.ctx, Event::TmrNoAdp, at(12'000));
    EXPECT_FALSE(lapsed.ctx.departed);
}

TEST(milan_discovery, stop_and_start_forget_the_talker_without_reporting_it_departed)
{
    for (auto const event : {Event::Stop, Event::Start}) {
        Discovery d;
        d.discover();
        d.ctx.discovered = false;
        d.sm.handle_event(d.ctx, event, at(2000));
        EXPECT_EQ(d.state(), event == Event::Stop ? State::Stopped : State::TkNotDiscovered);
        EXPECT_FALSE(d.ctx.departed);
        EXPECT_FALSE(d.ctx.discovered);
        EXPECT_FALSE(d.ctx.timer.has_value());
        EXPECT_EQ(d.ctx.available_index, 0U);
    }
}

TEST(milan_discovery, an_entity_discover_is_never_an_event)
{
    Discovery d;
    d.discover();
    AdpDu discover = available(9);
    discover.set_message_type(ADP_MESSAGE_TYPE_ENTITY_DISCOVER);
    EXPECT_FALSE(d.receive(discover, at(2000)));
    EXPECT_EQ(d.state(), State::TkDiscovered);
}

TEST_MAIN(statusbar_atdecc, atdecc_milan_adp_discovery_sm_test)
