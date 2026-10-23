// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/srp/srp_mrp_registrar_sm.hpp"

#include "statusbar/test/test.hpp"

using namespace statusbar;
using namespace statusbar::srp::mrp::registrar_sm;

namespace {

struct Walker
{
    Machine machine{};
    Context ctx{};

    void fire(Def::Event event)
    {
        ctx.clear_outputs();
        machine.handle_event(ctx, event);
    }
};

}  // namespace

//
// Initialization
//

TEST(mrp_registrar_sm, initial_state_is_start_then_uct_to_mt)
{
    Walker w{};
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::Start));
    // Firing any event triggers UCT first, landing in Mt.
    w.fire(Def::Event::LvTimer);  // LvTimer from Mt is a no-op
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::Mt));
}

//
// rNew always emits notify=New and lands in In
//

TEST(mrp_registrar_sm, rnew_from_mt_to_in_notifies_new)
{
    Walker w{};
    w.fire(Def::Event::UCT);  // Start -> Mt
    w.fire(Def::Event::RNew);
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::In));
    EXPECT_EQ(static_cast<int>(w.ctx.notify), static_cast<int>(Notify::New));
    EXPECT_FALSE(w.ctx.lvtimer_request);
}

TEST(mrp_registrar_sm, rnew_from_in_self_loop_still_notifies_new)
{
    Walker w{};
    w.fire(Def::Event::UCT);   // -> Mt
    w.fire(Def::Event::RNew);  // Mt -> In
    // Second rNew from In: self-loop, but notify still set.
    w.fire(Def::Event::RNew);
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::In));
    EXPECT_EQ(static_cast<int>(w.ctx.notify), static_cast<int>(Notify::New));
}

//
// rJoinIn / rJoinMt: silent refresh from In (no notify)
//

TEST(mrp_registrar_sm, rjoinin_from_in_is_silent_refresh)
{
    Walker w{};
    w.fire(Def::Event::UCT);   // -> Mt
    w.fire(Def::Event::RNew);  // Mt -> In  (notify=New)
    w.fire(Def::Event::RJoinIn);
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::In));
    // Silent — no notify.
    EXPECT_EQ(static_cast<int>(w.ctx.notify), static_cast<int>(Notify::None));
}

TEST(mrp_registrar_sm, rjoinin_from_mt_to_in_notifies_join)
{
    Walker w{};
    w.fire(Def::Event::UCT);  // -> Mt
    w.fire(Def::Event::RJoinIn);
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::In));
    EXPECT_EQ(static_cast<int>(w.ctx.notify), static_cast<int>(Notify::Join));
}

//
// rLv from In: transition to Lv, notify Leave, request lvtimer start
//

TEST(mrp_registrar_sm, rleave_from_in_to_lv_notifies_leave_and_arms_timer)
{
    Walker w{};
    w.fire(Def::Event::UCT);   // -> Mt
    w.fire(Def::Event::RNew);  // Mt -> In
    w.fire(Def::Event::RLeave);
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::Lv));
    EXPECT_EQ(static_cast<int>(w.ctx.notify), static_cast<int>(Notify::Leave));
    EXPECT_TRUE(w.ctx.lvtimer_request);
}

//
// LvTimer from Lv: transition to Mt with notify Leave
//

TEST(mrp_registrar_sm, lvtimer_from_lv_to_mt_notifies_leave)
{
    Walker w{};
    w.fire(Def::Event::UCT);
    w.fire(Def::Event::RNew);    // Mt -> In
    w.fire(Def::Event::RLeave);  // In -> Lv
    w.fire(Def::Event::LvTimer);
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::Mt));
    EXPECT_EQ(static_cast<int>(w.ctx.notify), static_cast<int>(Notify::Leave));
}

//
// RLeaveImmediate (MSRP, Milan v1.2 Section 4.2.7.2.2): IN / rLv! -> (Lv) -> MT
//

TEST(mrp_registrar_sm, rleave_immediate_from_in_to_mt_without_lvtimer)
{
    Walker w{};
    w.fire(Def::Event::UCT);
    w.fire(Def::Event::RNew);  // Mt -> In
    w.fire(Def::Event::RLeaveImmediate);
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::Mt));
    EXPECT_EQ(static_cast<int>(w.ctx.notify), static_cast<int>(Notify::Leave));
    EXPECT_FALSE(w.ctx.lvtimer_request);
    // A later join registers again with a fresh notification.
    w.fire(Def::Event::RJoinIn);
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::In));
    EXPECT_EQ(static_cast<int>(w.ctx.notify), static_cast<int>(Notify::Join));
}

TEST(mrp_registrar_sm, rleave_immediate_from_lv_and_mt_keeps_state)
{
    Walker w{};
    w.fire(Def::Event::UCT);
    w.fire(Def::Event::RLeaveImmediate);
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::Mt));
    EXPECT_FALSE(w.ctx.lvtimer_request);
    w.fire(Def::Event::RNew);
    w.fire(Def::Event::RLeaveAll);  // In -> Lv (leavetimer as standard)
    EXPECT_TRUE(w.ctx.lvtimer_request);
    w.fire(Def::Event::RLeaveImmediate);
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::Lv));
    EXPECT_FALSE(w.ctx.lvtimer_request);
}

//
// TxLeaveAll / RLeaveAll / Redeclare from In: silent (no notify), arm lvtimer, -> Lv
//

TEST(mrp_registrar_sm, txleaveall_from_in_to_lv_no_notify)
{
    Walker w{};
    w.fire(Def::Event::UCT);
    w.fire(Def::Event::RNew);  // -> In
    w.fire(Def::Event::TxLeaveAll);
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::Lv));
    EXPECT_EQ(static_cast<int>(w.ctx.notify), static_cast<int>(Notify::None));
    EXPECT_TRUE(w.ctx.lvtimer_request);
}

TEST(mrp_registrar_sm, rleaveall_from_mt_is_noop)
{
    Walker w{};
    w.fire(Def::Event::UCT);
    w.fire(Def::Event::RLeaveAll);
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::Mt));
    EXPECT_EQ(static_cast<int>(w.ctx.notify), static_cast<int>(Notify::None));
}

//
// Flush: all states -> Mt with notify Leave
//

TEST(mrp_registrar_sm, flush_from_in_to_mt_notifies_leave)
{
    Walker w{};
    w.fire(Def::Event::UCT);
    w.fire(Def::Event::RNew);  // -> In
    w.fire(Def::Event::Flush);
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::Mt));
    EXPECT_EQ(static_cast<int>(w.ctx.notify), static_cast<int>(Notify::Leave));
}

TEST(mrp_registrar_sm, flush_from_mt_self_loop_still_notifies_leave)
{
    Walker w{};
    w.fire(Def::Event::UCT);  // -> Mt
    w.fire(Def::Event::Flush);
    EXPECT_EQ(static_cast<int>(w.machine.current_state()), static_cast<int>(Def::State::Mt));
    EXPECT_EQ(static_cast<int>(w.ctx.notify), static_cast<int>(Notify::Leave));
}

TEST_MAIN(statusbar_srp, srp_mrp_registrar_sm_test)
