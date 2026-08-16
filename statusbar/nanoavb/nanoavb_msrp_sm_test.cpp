// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the nanoavb MSRP listener / talker registration state
// machines (nanoavb_msrp_{listener,talker}_sm). These drive whether the
// entity declares ListenerReady / TalkerAdvertise on the SRP bus, so the
// Idle -> Joining/Advertising -> Ready -> Leaving/Withdrawing cycle and the
// Failed branch must be exercised directly. Previously: zero coverage.

#include "statusbar/nanoavb/nanoavb_msrp_listener_sm.hpp"
#include "statusbar/nanoavb/nanoavb_msrp_talker_sm.hpp"
#include "statusbar/sm/sm_test_support.hpp"
#include "statusbar/test/test.hpp"

using TimePoint = statusbar::sm::TimePoint;
namespace sm_test = statusbar::sm::test;

namespace listener = statusbar::nanoavb::msrp_listener_sm;
namespace talker = statusbar::nanoavb::msrp_talker_sm;

namespace {

// Action-callback hit counts. Every state-machine action invokes its matching
// Context callback, so an unset (empty) inplace_function would crash — wire
// all of them.
struct Calls
{
    int init{0};
    int declare{0};  // msrp_listener_ready / msrp_talker_advertise (register the attribute)
    int mark_ready{0};
    int mark_failed{0};
    int leave{0};  // msrp_listener_leave / msrp_talker_withdraw
    int idle{0};
};

void wire(listener::Context& ctx, Calls& c)
{
    ctx.callbacks.init = [&c](listener::Context&, TimePoint) { c.init++; };
    ctx.callbacks.msrp_listener_ready = [&c](listener::Context&, TimePoint) { c.declare++; };
    ctx.callbacks.mark_ready = [&c](listener::Context&, TimePoint) { c.mark_ready++; };
    ctx.callbacks.mark_failed = [&c](listener::Context&, TimePoint) { c.mark_failed++; };
    ctx.callbacks.msrp_listener_leave = [&c](listener::Context&, TimePoint) { c.leave++; };
    ctx.callbacks.mark_idle = [&c](listener::Context&, TimePoint) { c.idle++; };
}

void wire(talker::Context& ctx, Calls& c)
{
    ctx.callbacks.init = [&c](talker::Context&, TimePoint) { c.init++; };
    ctx.callbacks.msrp_talker_advertise = [&c](talker::Context&, TimePoint) { c.declare++; };
    ctx.callbacks.mark_ready = [&c](talker::Context&, TimePoint) { c.mark_ready++; };
    ctx.callbacks.mark_failed = [&c](talker::Context&, TimePoint) { c.mark_failed++; };
    ctx.callbacks.msrp_talker_withdraw = [&c](talker::Context&, TimePoint) { c.leave++; };
    ctx.callbacks.mark_idle = [&c](talker::Context&, TimePoint) { c.idle++; };
}

}  // namespace

// ===========================================================================
// Listener registration state machine
// ===========================================================================

TEST(nanoavb_msrp_listener_sm, initial_state_is_start)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    EXPECT_EQ(machine.current_state(), listener::Def::State::Start);
}

TEST(nanoavb_msrp_listener_sm, uct_advances_start_to_idle)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    listener::Context ctx;
    Calls c{};
    wire(ctx, c);

    // The UCT (unconditional) chain auto-fires Start -> Idle on the first event.
    machine.handle_event(ctx, listener::Def::Event::UCT, TimePoint{});

    EXPECT_EQ(machine.current_state(), listener::Def::State::Idle);
    EXPECT_EQ(c.init, 1);
    EXPECT_EQ(machine.last_action, "init");
    EXPECT_FALSE(ctx.reserved);
    EXPECT_FALSE(ctx.failed);
}

TEST(nanoavb_msrp_listener_sm, join_then_ready_declares_and_reserves)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    listener::Context ctx;
    Calls c{};
    wire(ctx, c);

    machine.handle_event(ctx, listener::Def::Event::UCT, TimePoint{});
    machine.last_action = "";
    machine.handle_event(ctx, listener::Def::Event::StartJoin, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Joining);
    EXPECT_EQ(c.declare, 1);             // ListenerReady declared
    EXPECT_EQ(machine.last_action, "");  // msrp_listener_ready moved to the Joining entry hook

    machine.handle_event(ctx, listener::Def::Event::Ready, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Ready);
    EXPECT_EQ(c.mark_ready, 1);
    EXPECT_TRUE(ctx.reserved);
    EXPECT_FALSE(ctx.failed);
}

TEST(nanoavb_msrp_listener_sm, joining_failed_sets_failed_flag)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    listener::Context ctx;
    Calls c{};
    wire(ctx, c);

    machine.handle_event(ctx, listener::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::StartJoin, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::Failed, TimePoint{});

    EXPECT_EQ(machine.current_state(), listener::Def::State::Failed);
    EXPECT_EQ(c.mark_failed, 1);
    EXPECT_TRUE(ctx.failed);
    EXPECT_FALSE(ctx.reserved);
}

TEST(nanoavb_msrp_listener_sm, ready_lost_re_declares)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    listener::Context ctx;
    Calls c{};
    wire(ctx, c);

    machine.handle_event(ctx, listener::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::StartJoin, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::Ready, TimePoint{});

    // Losing the reservation re-registers ListenerReady and returns to Joining.
    machine.handle_event(ctx, listener::Def::Event::Lost, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Joining);
    EXPECT_EQ(c.declare, 2);  // once on StartJoin, once on Lost
}

TEST(nanoavb_msrp_listener_sm, ready_stopjoin_leaves_to_idle)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    listener::Context ctx;
    Calls c{};
    wire(ctx, c);

    machine.handle_event(ctx, listener::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::StartJoin, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::Ready, TimePoint{});

    machine.handle_event(ctx, listener::Def::Event::StopJoin, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Leaving);
    EXPECT_EQ(c.leave, 1);
    EXPECT_FALSE(ctx.reserved);

    machine.handle_event(ctx, listener::Def::Event::Left, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Idle);
    EXPECT_EQ(c.idle, 1);
}

TEST(nanoavb_msrp_listener_sm, failed_can_stopjoin_to_leaving)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    listener::Context ctx;
    Calls c{};
    wire(ctx, c);

    machine.handle_event(ctx, listener::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::StartJoin, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::Failed, TimePoint{});

    machine.handle_event(ctx, listener::Def::Event::StopJoin, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Leaving);
    EXPECT_EQ(c.leave, 1);
}

TEST(nanoavb_msrp_listener_sm, undefined_transition_is_ignored)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    listener::Context ctx;
    Calls c{};
    wire(ctx, c);

    machine.handle_event(ctx, listener::Def::Event::UCT, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Idle);

    // Ready has no transition from Idle: state and callbacks unchanged.
    machine.handle_event(ctx, listener::Def::Event::Ready, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Idle);
    EXPECT_EQ(c.mark_ready, 0);
}

// ===========================================================================
// Talker registration state machine (symmetric with the listener SM)
// ===========================================================================

TEST(nanoavb_msrp_talker_sm, initial_state_is_start)
{
    sm_test::Observed<talker::Def, talker::table> machine;
    EXPECT_EQ(machine.current_state(), talker::Def::State::Start);
}

TEST(nanoavb_msrp_talker_sm, advertise_then_ready_declares_and_reserves)
{
    sm_test::Observed<talker::Def, talker::table> machine;
    talker::Context ctx;
    Calls c{};
    wire(ctx, c);

    machine.handle_event(ctx, talker::Def::Event::UCT, TimePoint{});
    EXPECT_EQ(machine.current_state(), talker::Def::State::Idle);
    EXPECT_EQ(c.init, 1);

    machine.last_action = "";
    machine.handle_event(ctx, talker::Def::Event::StartAdvertise, TimePoint{});
    EXPECT_EQ(machine.current_state(), talker::Def::State::Advertising);
    EXPECT_EQ(c.declare, 1);             // TalkerAdvertise declared
    EXPECT_EQ(machine.last_action, "");  // msrp_talker_advertise moved to the Advertising entry hook

    machine.handle_event(ctx, talker::Def::Event::Ready, TimePoint{});
    EXPECT_EQ(machine.current_state(), talker::Def::State::Ready);
    EXPECT_EQ(c.mark_ready, 1);
    EXPECT_TRUE(ctx.reserved);
}

TEST(nanoavb_msrp_talker_sm, advertising_failed_sets_failed_flag)
{
    sm_test::Observed<talker::Def, talker::table> machine;
    talker::Context ctx;
    Calls c{};
    wire(ctx, c);

    machine.handle_event(ctx, talker::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::StartAdvertise, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::Failed, TimePoint{});

    EXPECT_EQ(machine.current_state(), talker::Def::State::Failed);
    EXPECT_EQ(c.mark_failed, 1);
    EXPECT_TRUE(ctx.failed);
    EXPECT_FALSE(ctx.reserved);
}

TEST(nanoavb_msrp_talker_sm, ready_lost_re_declares)
{
    sm_test::Observed<talker::Def, talker::table> machine;
    talker::Context ctx;
    Calls c{};
    wire(ctx, c);

    machine.handle_event(ctx, talker::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::StartAdvertise, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::Ready, TimePoint{});

    machine.handle_event(ctx, talker::Def::Event::Lost, TimePoint{});
    EXPECT_EQ(machine.current_state(), talker::Def::State::Advertising);
    EXPECT_EQ(c.declare, 2);
}

TEST(nanoavb_msrp_talker_sm, ready_stopadvertise_withdraws_to_idle)
{
    sm_test::Observed<talker::Def, talker::table> machine;
    talker::Context ctx;
    Calls c{};
    wire(ctx, c);

    machine.handle_event(ctx, talker::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::StartAdvertise, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::Ready, TimePoint{});

    machine.handle_event(ctx, talker::Def::Event::StopAdvertise, TimePoint{});
    EXPECT_EQ(machine.current_state(), talker::Def::State::Withdrawing);
    EXPECT_EQ(c.leave, 1);
    EXPECT_FALSE(ctx.reserved);

    machine.handle_event(ctx, talker::Def::Event::Withdrawn, TimePoint{});
    EXPECT_EQ(machine.current_state(), talker::Def::State::Idle);
    EXPECT_EQ(c.idle, 1);
}

TEST(nanoavb_msrp_talker_sm, undefined_transition_is_ignored)
{
    sm_test::Observed<talker::Def, talker::table> machine;
    talker::Context ctx;
    Calls c{};
    wire(ctx, c);

    machine.handle_event(ctx, talker::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::Withdrawn, TimePoint{});  // undefined from Idle
    EXPECT_EQ(machine.current_state(), talker::Def::State::Idle);
    EXPECT_EQ(c.idle, 0);
}

// Test runner

TEST_MAIN(statusbar_nanoavb, nanoavb_msrp_sm_test)
