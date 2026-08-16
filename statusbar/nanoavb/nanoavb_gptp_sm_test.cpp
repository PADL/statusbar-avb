// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_gptp_sm.hpp"

#include "statusbar/sm/sm.hpp"
#include "statusbar/sm/sm_test_support.hpp"
#include "statusbar/test/test.hpp"

#include <chrono>
#include <string>

using TimePoint = statusbar::sm::TimePoint;
namespace sm_test = statusbar::sm::test;

namespace gptp = statusbar::nanoavb::gptp_sm;

// Helper: create a Context with no-op callbacks (avoids bad_function_call)
static auto make_ctx() -> gptp::Context
{
    gptp::Context ctx;
    ctx.callbacks.init = [](gptp::Context&, TimePoint) {};
    ctx.callbacks.start_servo = [](gptp::Context&, TimePoint) {};
    ctx.callbacks.report_locked = [](gptp::Context&, TimePoint) {};
    ctx.callbacks.report_unlocked = [](gptp::Context&, TimePoint) {};
    return ctx;
}

//
// Construction and initial state
//

TEST(nanoavb_gptp_sm, initial_state)
{
    sm_test::Observed<gptp::Def, gptp::table> machine;
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Start);
}

//
// UCT transition: Start -> Unlocked
//

TEST(nanoavb_gptp_sm, uct_to_unlocked)
{
    sm_test::Observed<gptp::Def, gptp::table> machine;
    auto ctx = make_ctx();
    bool init_called = false;
    ctx.callbacks.init = [&](gptp::Context&, TimePoint) { init_called = true; };

    machine.handle_event(ctx, gptp::Def::Event::UCT, TimePoint{});

    EXPECT_EQ(machine.current_state(), gptp::Def::State::Unlocked);
    EXPECT_TRUE(init_called);
    EXPECT_EQ(machine.last_action, "init");
}

//
// Unlocked -> Acquiring on AsCapableUp
//

TEST(nanoavb_gptp_sm, as_capable_up_to_acquiring)
{
    sm_test::Observed<gptp::Def, gptp::table> machine;
    auto ctx = make_ctx();
    bool servo_called = false;
    ctx.callbacks.start_servo = [&](gptp::Context&, TimePoint) { servo_called = true; };

    machine.handle_event(ctx, gptp::Def::Event::UCT, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Unlocked);

    machine.last_action = "";
    machine.handle_event(ctx, gptp::Def::Event::AsCapableUp, TimePoint{});

    EXPECT_EQ(machine.current_state(), gptp::Def::State::Acquiring);
    EXPECT_TRUE(servo_called);
    EXPECT_EQ(machine.last_action, "");  // start_servo moved to the Acquiring entry hook
    EXPECT_FALSE(ctx.time_locked);
}

//
// Acquiring -> Locked on LockedStable
//

TEST(nanoavb_gptp_sm, locked_stable_to_locked)
{
    sm_test::Observed<gptp::Def, gptp::table> machine;
    auto ctx = make_ctx();
    bool locked_called = false;
    ctx.callbacks.report_locked = [&](gptp::Context&, TimePoint) { locked_called = true; };

    machine.handle_event(ctx, gptp::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, gptp::Def::Event::AsCapableUp, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Acquiring);

    machine.handle_event(ctx, gptp::Def::Event::LockedStable, TimePoint{});

    EXPECT_EQ(machine.current_state(), gptp::Def::State::Locked);
    EXPECT_TRUE(locked_called);
    EXPECT_EQ(machine.last_action, "report_locked");
    EXPECT_TRUE(ctx.time_locked);
}

//
// Acquiring -> Unlocked on AsCapableDown
//

TEST(nanoavb_gptp_sm, as_capable_down_from_acquiring)
{
    sm_test::Observed<gptp::Def, gptp::table> machine;
    auto ctx = make_ctx();
    bool unlocked_called = false;
    ctx.callbacks.report_unlocked = [&](gptp::Context&, TimePoint) { unlocked_called = true; };

    machine.handle_event(ctx, gptp::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, gptp::Def::Event::AsCapableUp, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Acquiring);

    machine.handle_event(ctx, gptp::Def::Event::AsCapableDown, TimePoint{});

    EXPECT_EQ(machine.current_state(), gptp::Def::State::Unlocked);
    EXPECT_TRUE(unlocked_called);
    EXPECT_EQ(machine.last_action, "report_unlocked");
    EXPECT_FALSE(ctx.time_locked);
}

//
// Locked -> Unlocked on AsCapableDown
//

TEST(nanoavb_gptp_sm, as_capable_down_from_locked)
{
    sm_test::Observed<gptp::Def, gptp::table> machine;
    auto ctx = make_ctx();
    bool unlocked_called = false;
    ctx.callbacks.report_unlocked = [&](gptp::Context&, TimePoint) { unlocked_called = true; };

    // Start -> Unlocked -> Acquiring -> Locked
    machine.handle_event(ctx, gptp::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, gptp::Def::Event::AsCapableUp, TimePoint{});
    machine.handle_event(ctx, gptp::Def::Event::LockedStable, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Locked);
    EXPECT_TRUE(ctx.time_locked);

    machine.handle_event(ctx, gptp::Def::Event::AsCapableDown, TimePoint{});

    EXPECT_EQ(machine.current_state(), gptp::Def::State::Unlocked);
    EXPECT_TRUE(unlocked_called);
    EXPECT_EQ(machine.last_action, "report_unlocked");
    EXPECT_FALSE(ctx.time_locked);
}

//
// Locked -> Acquiring on LockLost
//

TEST(nanoavb_gptp_sm, lock_lost_to_acquiring)
{
    sm_test::Observed<gptp::Def, gptp::table> machine;
    auto ctx = make_ctx();
    int servo_count = 0;
    ctx.callbacks.start_servo = [&](gptp::Context&, TimePoint) { ++servo_count; };

    // Start -> Unlocked -> Acquiring -> Locked
    machine.handle_event(ctx, gptp::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, gptp::Def::Event::AsCapableUp, TimePoint{});
    EXPECT_EQ(servo_count, 1);
    machine.handle_event(ctx, gptp::Def::Event::LockedStable, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Locked);

    machine.last_action = "";
    machine.handle_event(ctx, gptp::Def::Event::LockLost, TimePoint{});

    EXPECT_EQ(machine.current_state(), gptp::Def::State::Acquiring);
    EXPECT_EQ(servo_count, 2);
    EXPECT_EQ(machine.last_action, "");  // start_servo moved to the Acquiring entry hook
    EXPECT_FALSE(ctx.time_locked);
}

//
// Full lifecycle: lock, lose, reacquire, link down
//

TEST(nanoavb_gptp_sm, full_lock_cycle)
{
    sm_test::Observed<gptp::Def, gptp::table> machine;
    auto ctx = make_ctx();

    // Start -> Unlocked (UCT)
    machine.handle_event(ctx, gptp::Def::Event::UCT, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Unlocked);

    // Unlocked -> Acquiring
    machine.handle_event(ctx, gptp::Def::Event::AsCapableUp, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Acquiring);
    EXPECT_FALSE(ctx.time_locked);

    // Acquiring -> Locked
    machine.handle_event(ctx, gptp::Def::Event::LockedStable, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Locked);
    EXPECT_TRUE(ctx.time_locked);

    // Locked -> Acquiring (LockLost)
    machine.handle_event(ctx, gptp::Def::Event::LockLost, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Acquiring);
    EXPECT_FALSE(ctx.time_locked);

    // Acquiring -> Locked again
    machine.handle_event(ctx, gptp::Def::Event::LockedStable, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Locked);
    EXPECT_TRUE(ctx.time_locked);

    // Locked -> Unlocked (AsCapableDown)
    machine.handle_event(ctx, gptp::Def::Event::AsCapableDown, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Unlocked);
    EXPECT_FALSE(ctx.time_locked);
}

//
// Unexpected events: no transition, state unchanged
//

TEST(nanoavb_gptp_sm, unexpected_event_in_unlocked)
{
    sm_test::Observed<gptp::Def, gptp::table> machine;
    auto ctx = make_ctx();

    machine.handle_event(ctx, gptp::Def::Event::UCT, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Unlocked);
    machine.last_action = "";

    // LockedStable is not valid in Unlocked
    machine.handle_event(ctx, gptp::Def::Event::LockedStable, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Unlocked);
    EXPECT_EQ(machine.last_action, "");

    // LockLost is not valid in Unlocked
    machine.handle_event(ctx, gptp::Def::Event::LockLost, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Unlocked);
    EXPECT_EQ(machine.last_action, "");

    // AsCapableDown is not valid in Unlocked
    machine.handle_event(ctx, gptp::Def::Event::AsCapableDown, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Unlocked);
    EXPECT_EQ(machine.last_action, "");
}

TEST(nanoavb_gptp_sm, unexpected_event_in_acquiring)
{
    sm_test::Observed<gptp::Def, gptp::table> machine;
    auto ctx = make_ctx();

    machine.handle_event(ctx, gptp::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, gptp::Def::Event::AsCapableUp, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Acquiring);
    machine.last_action = "";

    // AsCapableUp is not valid in Acquiring (already acquiring)
    machine.handle_event(ctx, gptp::Def::Event::AsCapableUp, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Acquiring);
    EXPECT_EQ(machine.last_action, "");

    // LockLost is not valid in Acquiring
    machine.handle_event(ctx, gptp::Def::Event::LockLost, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Acquiring);
    EXPECT_EQ(machine.last_action, "");
}

TEST(nanoavb_gptp_sm, unexpected_event_in_locked)
{
    sm_test::Observed<gptp::Def, gptp::table> machine;
    auto ctx = make_ctx();

    machine.handle_event(ctx, gptp::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, gptp::Def::Event::AsCapableUp, TimePoint{});
    machine.handle_event(ctx, gptp::Def::Event::LockedStable, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Locked);
    machine.last_action = "";

    // AsCapableUp is not valid in Locked
    machine.handle_event(ctx, gptp::Def::Event::AsCapableUp, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Locked);
    EXPECT_EQ(machine.last_action, "");

    // LockedStable is not valid in Locked (already locked)
    machine.handle_event(ctx, gptp::Def::Event::LockedStable, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Locked);
    EXPECT_EQ(machine.last_action, "");
}

//
// Repeated events: re-acquiring after multiple lock losses
//

TEST(nanoavb_gptp_sm, repeated_lock_lost)
{
    sm_test::Observed<gptp::Def, gptp::table> machine;
    auto ctx = make_ctx();
    int servo_count = 0;
    ctx.callbacks.start_servo = [&](gptp::Context&, TimePoint) { ++servo_count; };

    machine.handle_event(ctx, gptp::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, gptp::Def::Event::AsCapableUp, TimePoint{});
    EXPECT_EQ(servo_count, 1);

    // Lock and lose multiple times
    for (int i = 0; i < 3; ++i) {
        machine.handle_event(ctx, gptp::Def::Event::LockedStable, TimePoint{});
        EXPECT_EQ(machine.current_state(), gptp::Def::State::Locked);
        EXPECT_TRUE(ctx.time_locked);

        machine.handle_event(ctx, gptp::Def::Event::LockLost, TimePoint{});
        EXPECT_EQ(machine.current_state(), gptp::Def::State::Acquiring);
        EXPECT_FALSE(ctx.time_locked);
    }

    // start_servo called once for initial + 3 more for lock losses
    EXPECT_EQ(servo_count, 4);
}

//
// Transition table well-formedness
//

TEST(nanoavb_gptp_sm, table_state_count)
{
    // Verify expected number of states and events
    EXPECT_EQ(gptp::Machine::num_states, 4U);
    EXPECT_EQ(gptp::Machine::num_events, 5U);
}

TEST(nanoavb_gptp_sm, table_has_uct)
{
    EXPECT_TRUE(gptp::Machine::has_uct_event);
}

TEST(nanoavb_gptp_sm, table_start_has_uct_transition)
{
    auto const& t = gptp::table.get(gptp::Def::State::Start, gptp::Def::Event::UCT);
    EXPECT_TRUE(t.is_valid());
    EXPECT_EQ(t.next_state, gptp::Def::State::Unlocked);
}

TEST(nanoavb_gptp_sm, table_all_transitions_valid)
{
    // Verify that each defined transition has a valid action and target state
    using S = gptp::Def::State;
    using E = gptp::Def::Event;

    // Start: only UCT defined
    EXPECT_TRUE(gptp::table.get(S::Start, E::UCT).is_valid());
    EXPECT_FALSE(gptp::table.get(S::Start, E::AsCapableUp).is_valid());
    EXPECT_FALSE(gptp::table.get(S::Start, E::AsCapableDown).is_valid());
    EXPECT_FALSE(gptp::table.get(S::Start, E::LockedStable).is_valid());
    EXPECT_FALSE(gptp::table.get(S::Start, E::LockLost).is_valid());

    // Unlocked: only AsCapableUp
    EXPECT_FALSE(gptp::table.get(S::Unlocked, E::UCT).is_valid());
    EXPECT_TRUE(gptp::table.get(S::Unlocked, E::AsCapableUp).is_valid());
    EXPECT_FALSE(gptp::table.get(S::Unlocked, E::AsCapableDown).is_valid());
    EXPECT_FALSE(gptp::table.get(S::Unlocked, E::LockedStable).is_valid());
    EXPECT_FALSE(gptp::table.get(S::Unlocked, E::LockLost).is_valid());

    // Acquiring: LockedStable and AsCapableDown
    EXPECT_FALSE(gptp::table.get(S::Acquiring, E::UCT).is_valid());
    EXPECT_FALSE(gptp::table.get(S::Acquiring, E::AsCapableUp).is_valid());
    EXPECT_TRUE(gptp::table.get(S::Acquiring, E::AsCapableDown).is_valid());
    EXPECT_TRUE(gptp::table.get(S::Acquiring, E::LockedStable).is_valid());
    EXPECT_FALSE(gptp::table.get(S::Acquiring, E::LockLost).is_valid());

    // Locked: AsCapableDown and LockLost
    EXPECT_FALSE(gptp::table.get(S::Locked, E::UCT).is_valid());
    EXPECT_FALSE(gptp::table.get(S::Locked, E::AsCapableUp).is_valid());
    EXPECT_TRUE(gptp::table.get(S::Locked, E::AsCapableDown).is_valid());
    EXPECT_FALSE(gptp::table.get(S::Locked, E::LockedStable).is_valid());
    EXPECT_TRUE(gptp::table.get(S::Locked, E::LockLost).is_valid());
}

//
// Reset functionality
//

TEST(nanoavb_gptp_sm, reset_returns_to_start)
{
    sm_test::Observed<gptp::Def, gptp::table> machine;
    auto ctx = make_ctx();

    machine.handle_event(ctx, gptp::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, gptp::Def::Event::AsCapableUp, TimePoint{});
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Acquiring);

    machine.reset();
    EXPECT_EQ(machine.current_state(), gptp::Def::State::Start);
}

//
// Context state tracking
//

TEST(nanoavb_gptp_sm, context_time_locked_tracks_state)
{
    sm_test::Observed<gptp::Def, gptp::table> machine;
    auto ctx = make_ctx();

    EXPECT_FALSE(ctx.time_locked);

    machine.handle_event(ctx, gptp::Def::Event::UCT, TimePoint{});
    EXPECT_FALSE(ctx.time_locked);

    machine.handle_event(ctx, gptp::Def::Event::AsCapableUp, TimePoint{});
    EXPECT_FALSE(ctx.time_locked);

    machine.handle_event(ctx, gptp::Def::Event::LockedStable, TimePoint{});
    EXPECT_TRUE(ctx.time_locked);

    machine.handle_event(ctx, gptp::Def::Event::LockLost, TimePoint{});
    EXPECT_FALSE(ctx.time_locked);

    machine.handle_event(ctx, gptp::Def::Event::LockedStable, TimePoint{});
    EXPECT_TRUE(ctx.time_locked);

    machine.handle_event(ctx, gptp::Def::Event::AsCapableDown, TimePoint{});
    EXPECT_FALSE(ctx.time_locked);
}

//
// Test Runner
//

TEST_MAIN(statusbar_nanoavb, nanoavb_gptp_sm_test)
