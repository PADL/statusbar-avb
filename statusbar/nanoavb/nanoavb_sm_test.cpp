// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/sm/sm_test_support.hpp"
#include "statusbar/test/test.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

using TimePoint = statusbar::sm::TimePoint;
namespace sm_test = statusbar::sm::test;

//
// Supervisor State Machine Tests
//
namespace supervisor = statusbar::nanoavb::supervisor_sm;

TEST(nanoavb_supervisor_sm, initial_state)
{
    sm_test::Observed<supervisor::Def, supervisor::table> machine;
    supervisor::Context ctx;

    // Initial state is Start
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Start);
}

TEST(nanoavb_supervisor_sm, uct_to_down)
{
    sm_test::Observed<supervisor::Def, supervisor::table> machine;
    supervisor::Context ctx;
    bool init_called = false;
    ctx.callbacks.init_iface = [&](supervisor::Context&, TimePoint) { init_called = true; };

    // UCT transition to Down
    machine.handle_event(ctx, supervisor::Def::Event::UCT, TimePoint{});

    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Down);
    EXPECT_TRUE(init_called);
    EXPECT_EQ(machine.last_action, "init_iface");
}

TEST(nanoavb_supervisor_sm, link_up_to_init)
{
    sm_test::Observed<supervisor::Def, supervisor::table> machine;
    supervisor::Context ctx;
    bool start_called = false;
    ctx.callbacks.init_iface = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.start_protocols = [&](supervisor::Context&, TimePoint) { start_called = true; };

    // Move to Down first
    machine.handle_event(ctx, supervisor::Def::Event::UCT, TimePoint{});
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Down);

    // LinkUp -> Init
    machine.handle_event(ctx, supervisor::Def::Event::LinkUp, TimePoint{});

    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Init);
    EXPECT_TRUE(start_called);
    EXPECT_EQ(machine.last_action, "start_protocols");
}

TEST(nanoavb_supervisor_sm, gptp_locked_to_ready)
{
    sm_test::Observed<supervisor::Def, supervisor::table> machine;
    supervisor::Context ctx;
    bool ready_called = false;
    ctx.callbacks.init_iface = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.start_protocols = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.enter_ready = [&](supervisor::Context&, TimePoint) { ready_called = true; };

    // Down -> Init -> Ready (gPTP lock alone enables SRP+streaming; no VLAN gate)
    machine.handle_event(ctx, supervisor::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, supervisor::Def::Event::LinkUp, TimePoint{});
    machine.last_action = "";
    machine.handle_event(ctx, supervisor::Def::Event::GptpLocked, TimePoint{});

    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Ready);
    EXPECT_TRUE(ready_called);
    EXPECT_EQ(machine.last_action, "");  // enter_ready moved to the Ready entry hook
}

TEST(nanoavb_supervisor_sm, gptp_lost_then_relock_is_dynamic)
{
    sm_test::Observed<supervisor::Def, supervisor::table> machine;
    supervisor::Context ctx;
    int ready_count = 0;
    int degrade_count = 0;
    ctx.callbacks.init_iface = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.start_protocols = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.enter_ready = [&](supervisor::Context&, TimePoint) { ++ready_count; };
    ctx.callbacks.degrade_stop_streams = [&](supervisor::Context&, TimePoint) { ++degrade_count; };

    machine.handle_event(ctx, supervisor::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, supervisor::Def::Event::LinkUp, TimePoint{});
    machine.handle_event(ctx, supervisor::Def::Event::GptpLocked, TimePoint{});
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Ready);

    // gPTP lost -> Degraded (tear down SRP + streams)
    machine.handle_event(ctx, supervisor::Def::Event::GptpLost, TimePoint{});
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Degraded);
    EXPECT_EQ(machine.last_action, "degrade_stop_streams");

    // gPTP re-locked -> Ready again (dynamic)
    machine.handle_event(ctx, supervisor::Def::Event::GptpLocked, TimePoint{});
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Ready);

    EXPECT_EQ(ready_count, 2);
    EXPECT_EQ(degrade_count, 1);
}

TEST(nanoavb_supervisor_sm, timeout_in_init_goes_to_down)
{
    sm_test::Observed<supervisor::Def, supervisor::table> machine;
    supervisor::Context ctx;
    bool timeout_gptp_called = false;
    bool stop_all_called = false;
    ctx.callbacks.init_iface = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.start_protocols = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.timeout_gptp = [&](supervisor::Context&, TimePoint) { timeout_gptp_called = true; };
    ctx.callbacks.stop_all = [&](supervisor::Context&, TimePoint) { stop_all_called = true; };

    // Down -> Init
    machine.handle_event(ctx, supervisor::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, supervisor::Def::Event::LinkUp, TimePoint{});
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Init);

    // Timeout in Init -> Down (gPTP lock timeout)
    machine.handle_event(ctx, supervisor::Def::Event::Timeout, TimePoint{});

    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Down);
    EXPECT_TRUE(timeout_gptp_called);
    EXPECT_TRUE(stop_all_called);
    EXPECT_EQ(machine.last_action, "timeout_gptp");
}

TEST(nanoavb_supervisor_sm, link_down_from_any_state_to_down)
{
    sm_test::Observed<supervisor::Def, supervisor::table> machine;
    supervisor::Context ctx;
    int stop_count = 0;
    ctx.callbacks.init_iface = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.start_protocols = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.enter_ready = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.stop_all = [&](supervisor::Context&, TimePoint) { ++stop_count; };

    // Get to Ready (gPTP lock alone)
    machine.handle_event(ctx, supervisor::Def::Event::UCT, TimePoint{});
    stop_count = 0;  // the Down entry hook fires stop_all on the initial UCT
    machine.handle_event(ctx, supervisor::Def::Event::LinkUp, TimePoint{});
    machine.handle_event(ctx, supervisor::Def::Event::GptpLocked, TimePoint{});
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Ready);

    // LinkDown from Ready -> Down
    machine.handle_event(ctx, supervisor::Def::Event::LinkDown, TimePoint{});
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Down);
    EXPECT_EQ(stop_count, 1);
}

TEST(nanoavb_supervisor_sm, null_callbacks_no_crash)
{
    // All callbacks left as empty std::function — must not throw bad_function_call
    sm_test::Observed<supervisor::Def, supervisor::table> machine;
    supervisor::Context ctx;

    // UCT -> Down (calls init_iface with null callback)
    machine.handle_event(ctx, supervisor::Def::Event::UCT, TimePoint{});
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Down);

    // LinkUp -> Init (calls start_protocols with null callback)
    machine.handle_event(ctx, supervisor::Def::Event::LinkUp, TimePoint{});
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Init);

    // Timeout in Init -> Down (calls timeout_gptp + stop_all, both null)
    machine.handle_event(ctx, supervisor::Def::Event::Timeout, TimePoint{});
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Down);
}

TEST(nanoavb_supervisor_sm, gptp_lost_null_callbacks)
{
    sm_test::Observed<supervisor::Def, supervisor::table> machine;
    supervisor::Context ctx;

    // Only set the callbacks needed to reach Ready; leave degrade_stop_streams null.
    ctx.callbacks.init_iface = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.start_protocols = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.enter_ready = [](supervisor::Context&, TimePoint) {};

    machine.handle_event(ctx, supervisor::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, supervisor::Def::Event::LinkUp, TimePoint{});
    machine.handle_event(ctx, supervisor::Def::Event::GptpLocked, TimePoint{});
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Ready);

    // GptpLost in Ready -> Degraded (calls degrade_stop_streams, which is null — must not crash)
    machine.handle_event(ctx, supervisor::Def::Event::GptpLost, TimePoint{});
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Degraded);
}

//
// Listener Engine State Machine Tests
//
namespace listener = statusbar::nanoavb::listener_engine_sm;

TEST(nanoavb_listener_sm, initial_state)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    listener::Context ctx;

    EXPECT_EQ(machine.current_state(), listener::Def::State::Start);
}

TEST(nanoavb_listener_sm, uct_to_off)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    listener::Context ctx;
    bool init_called = false;
    ctx.callbacks.init = [&](listener::Context&, TimePoint) { init_called = true; };

    machine.handle_event(ctx, listener::Def::Event::UCT, TimePoint{});

    EXPECT_EQ(machine.current_state(), listener::Def::State::Off);
    EXPECT_TRUE(init_called);
    EXPECT_EQ(machine.last_action, "init");
}

TEST(nanoavb_listener_sm, gate_listen_to_listening)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    listener::Context ctx;
    bool filter_called = false;
    ctx.callbacks.init = [](listener::Context&, TimePoint) {};
    ctx.callbacks.enable_rx_filter = [&](listener::Context&, TimePoint) { filter_called = true; };

    machine.handle_event(ctx, listener::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::GateListen, TimePoint{});

    EXPECT_EQ(machine.current_state(), listener::Def::State::Listening);
    EXPECT_TRUE(filter_called);
    EXPECT_EQ(machine.last_action, "enable_rx_filter");
}

TEST(nanoavb_listener_sm, gate_stop_from_listening_to_off)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    listener::Context ctx;
    bool stop_called = false;
    ctx.callbacks.init = [](listener::Context&, TimePoint) {};
    ctx.callbacks.enable_rx_filter = [](listener::Context&, TimePoint) {};
    ctx.callbacks.stop_all = [&](listener::Context&, TimePoint) { stop_called = true; };

    // Off -> Listening
    machine.handle_event(ctx, listener::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::GateListen, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Listening);

    // GateStop from Listening -> Off
    machine.last_action = "";
    machine.handle_event(ctx, listener::Def::Event::GateStop, TimePoint{});

    EXPECT_EQ(machine.current_state(), listener::Def::State::Off);
    EXPECT_TRUE(stop_called);
    EXPECT_EQ(machine.last_action, "");  // stop_all moved to the Off entry hook
}

TEST(nanoavb_listener_sm, gate_stop_from_syncing_to_off)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    listener::Context ctx;
    bool stop_called = false;
    ctx.callbacks.init = [](listener::Context&, TimePoint) {};
    ctx.callbacks.enable_rx_filter = [](listener::Context&, TimePoint) {};
    ctx.callbacks.start_sync = [](listener::Context&, TimePoint) {};
    ctx.callbacks.stop_all = [&](listener::Context&, TimePoint) { stop_called = true; };

    // Off -> Listening -> Syncing
    machine.handle_event(ctx, listener::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::GateListen, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::FirstPacket, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Syncing);

    // GateStop from Syncing -> Off
    machine.last_action = "";
    machine.handle_event(ctx, listener::Def::Event::GateStop, TimePoint{});

    EXPECT_EQ(machine.current_state(), listener::Def::State::Off);
    EXPECT_TRUE(stop_called);
    EXPECT_EQ(machine.last_action, "");  // stop_all moved to the Off entry hook
}

TEST(nanoavb_listener_sm, packet_gap_in_syncing_resyncs)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    listener::Context ctx;
    int resync_count = 0;
    ctx.callbacks.init = [](listener::Context&, TimePoint) {};
    ctx.callbacks.enable_rx_filter = [](listener::Context&, TimePoint) {};
    ctx.callbacks.start_sync = [](listener::Context&, TimePoint) {};
    ctx.callbacks.resync = [&](listener::Context&, TimePoint) { ++resync_count; };

    // Off -> Listening -> Syncing
    machine.handle_event(ctx, listener::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::GateListen, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::FirstPacket, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Syncing);

    // PacketGap in Syncing stays in Syncing with resync
    machine.handle_event(ctx, listener::Def::Event::PacketGap, TimePoint{});

    EXPECT_EQ(machine.current_state(), listener::Def::State::Syncing);
    EXPECT_EQ(resync_count, 1);
    EXPECT_EQ(machine.last_action, "resync");
}

TEST(nanoavb_listener_sm, packet_gap_in_muted_resyncs)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    listener::Context ctx;
    int resync_count = 0;
    ctx.callbacks.init = [](listener::Context&, TimePoint) {};
    ctx.callbacks.enable_rx_filter = [](listener::Context&, TimePoint) {};
    ctx.callbacks.start_sync = [](listener::Context&, TimePoint) {};
    ctx.callbacks.start_audio_sink = [](listener::Context&, TimePoint) {};
    ctx.callbacks.mute_out = [](listener::Context&, TimePoint) {};
    ctx.callbacks.resync = [&](listener::Context&, TimePoint) { ++resync_count; };

    // Off -> Listening -> Syncing -> Playing -> Muted
    machine.handle_event(ctx, listener::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::GateListen, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::FirstPacket, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::Synced, TimePoint{});
    machine.handle_event(ctx, listener::Def::Event::Underrun, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Muted);

    // PacketGap in Muted -> Syncing with resync
    machine.handle_event(ctx, listener::Def::Event::PacketGap, TimePoint{});

    EXPECT_EQ(machine.current_state(), listener::Def::State::Syncing);
    EXPECT_EQ(resync_count, 1);
    EXPECT_EQ(machine.last_action, "resync");
}

TEST(nanoavb_listener_sm, full_playback_cycle)
{
    sm_test::Observed<listener::Def, listener::table> machine;
    listener::Context ctx;
    ctx.callbacks.init = [](listener::Context&, TimePoint) {};
    ctx.callbacks.enable_rx_filter = [](listener::Context&, TimePoint) {};
    ctx.callbacks.start_sync = [](listener::Context&, TimePoint) {};
    ctx.callbacks.start_audio_sink = [](listener::Context&, TimePoint) {};
    ctx.callbacks.mute_out = [](listener::Context&, TimePoint) {};
    ctx.callbacks.unmute_out = [](listener::Context&, TimePoint) {};
    ctx.callbacks.stop_all = [](listener::Context&, TimePoint) {};
    ctx.callbacks.resync = [](listener::Context&, TimePoint) {};

    // Full cycle: Off -> Listening -> Syncing -> Playing -> Muted -> Playing -> Off
    machine.handle_event(ctx, listener::Def::Event::UCT, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Off);

    machine.handle_event(ctx, listener::Def::Event::GateListen, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Listening);

    machine.handle_event(ctx, listener::Def::Event::FirstPacket, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Syncing);

    machine.handle_event(ctx, listener::Def::Event::Synced, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Playing);

    machine.handle_event(ctx, listener::Def::Event::Underrun, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Muted);

    machine.handle_event(ctx, listener::Def::Event::Recovered, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Playing);

    machine.handle_event(ctx, listener::Def::Event::GateStop, TimePoint{});
    EXPECT_EQ(machine.current_state(), listener::Def::State::Off);
}

//
// ACMP Listener State Machine Tests
//
namespace acmp_listener = statusbar::nanoavb::acmp_listener_sm;

TEST(nanoavb_acmp_listener_sm, link_down_while_disconnecting)
{
    // Regression test for issue 57d859f: Disconnecting state has no LinkDown
    // handler, causing the SM to get stuck if link goes down while waiting
    // for DisconnectOk.
    sm_test::Observed<acmp_listener::Def, acmp_listener::table> machine;
    acmp_listener::Context ctx;
    ctx.callbacks.init = [](acmp_listener::Context&, TimePoint) {};
    ctx.callbacks.send_connect_tx = [](acmp_listener::Context&, TimePoint) {};
    ctx.callbacks.mark_connected = [](acmp_listener::Context&, TimePoint) {};
    ctx.callbacks.send_disconnect_tx = [](acmp_listener::Context&, TimePoint) {};
    ctx.callbacks.mark_disconnected = [](acmp_listener::Context&, TimePoint) {};
    ctx.callbacks.mark_failed = [](acmp_listener::Context&, TimePoint) {};

    // Get to Connected state
    machine.handle_event(ctx, acmp_listener::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, acmp_listener::Def::Event::ConnectReq, TimePoint{});
    machine.handle_event(ctx, acmp_listener::Def::Event::ConnectOk, TimePoint{});
    EXPECT_EQ(machine.current_state(), acmp_listener::Def::State::Connected);

    // Disconnect request -> Disconnecting
    machine.handle_event(ctx, acmp_listener::Def::Event::DisconnectReq, TimePoint{});
    EXPECT_EQ(machine.current_state(), acmp_listener::Def::State::Disconnecting);

    // LinkDown while Disconnecting -> should go to Idle, not stay stuck
    machine.handle_event(ctx, acmp_listener::Def::Event::LinkDown, TimePoint{});
    EXPECT_EQ(machine.current_state(), acmp_listener::Def::State::Idle);
}

//
// MVRP State Machine Tests
//
namespace mvrp = statusbar::nanoavb::mvrp_sm;

TEST(nanoavb_mvrp_sm, initial_state)
{
    sm_test::Observed<mvrp::Def, mvrp::table> machine;
    mvrp::Context ctx;

    EXPECT_EQ(machine.current_state(), mvrp::Def::State::Start);
}

TEST(nanoavb_mvrp_sm, uct_to_not_joined)
{
    sm_test::Observed<mvrp::Def, mvrp::table> machine;
    mvrp::Context ctx;
    bool init_called = false;
    ctx.callbacks.init = [&](mvrp::Context&, TimePoint) { init_called = true; };

    machine.handle_event(ctx, mvrp::Def::Event::UCT, TimePoint{});

    EXPECT_EQ(machine.current_state(), mvrp::Def::State::NotJoined);
    EXPECT_TRUE(init_called);
    EXPECT_FALSE(ctx.joined);
    EXPECT_EQ(machine.last_action, "init");
}

TEST(nanoavb_mvrp_sm, acquire_to_joining)
{
    sm_test::Observed<mvrp::Def, mvrp::table> machine;
    mvrp::Context ctx;
    bool join_called = false;
    ctx.callbacks.init = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.send_join = [&](mvrp::Context&, TimePoint) { join_called = true; };

    machine.handle_event(ctx, mvrp::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, mvrp::Def::Event::Acquire, TimePoint{});

    EXPECT_EQ(machine.current_state(), mvrp::Def::State::Joining);
    EXPECT_TRUE(join_called);
    EXPECT_EQ(machine.last_action, "send_join");
}

TEST(nanoavb_mvrp_sm, join_ok_to_joined)
{
    sm_test::Observed<mvrp::Def, mvrp::table> machine;
    mvrp::Context ctx;
    bool mark_joined_called = false;
    ctx.callbacks.init = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.send_join = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.mark_joined = [&](mvrp::Context&, TimePoint) { mark_joined_called = true; };

    machine.handle_event(ctx, mvrp::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, mvrp::Def::Event::Acquire, TimePoint{});
    machine.handle_event(ctx, mvrp::Def::Event::JoinOk, TimePoint{});

    EXPECT_EQ(machine.current_state(), mvrp::Def::State::Joined);
    EXPECT_TRUE(mark_joined_called);
    EXPECT_TRUE(ctx.joined);
    EXPECT_EQ(machine.last_action, "mark_joined");
}

TEST(nanoavb_mvrp_sm, join_fail_to_error)
{
    sm_test::Observed<mvrp::Def, mvrp::table> machine;
    mvrp::Context ctx;
    bool mark_error_called = false;
    ctx.callbacks.init = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.send_join = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.mark_error = [&](mvrp::Context&, TimePoint) { mark_error_called = true; };

    machine.handle_event(ctx, mvrp::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, mvrp::Def::Event::Acquire, TimePoint{});
    machine.last_action = "";
    machine.handle_event(ctx, mvrp::Def::Event::JoinFail, TimePoint{});

    EXPECT_EQ(machine.current_state(), mvrp::Def::State::Error);
    EXPECT_TRUE(mark_error_called);
    EXPECT_FALSE(ctx.joined);
    EXPECT_EQ(machine.last_action, "");  // mark_error moved to the Error entry hook
}

TEST(nanoavb_mvrp_sm, leave_fail_to_error)
{
    sm_test::Observed<mvrp::Def, mvrp::table> machine;
    mvrp::Context ctx;
    bool mark_error_called = false;
    ctx.callbacks.init = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.send_join = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.mark_joined = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.send_leave = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.mark_error = [&](mvrp::Context&, TimePoint) { mark_error_called = true; };

    // NotJoined -> Joining -> Joined -> Leaving -> Error
    machine.handle_event(ctx, mvrp::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, mvrp::Def::Event::Acquire, TimePoint{});
    machine.handle_event(ctx, mvrp::Def::Event::JoinOk, TimePoint{});
    machine.handle_event(ctx, mvrp::Def::Event::ReleaseLast, TimePoint{});
    EXPECT_EQ(machine.current_state(), mvrp::Def::State::Leaving);

    machine.last_action = "";
    machine.handle_event(ctx, mvrp::Def::Event::LeaveFail, TimePoint{});

    EXPECT_EQ(machine.current_state(), mvrp::Def::State::Error);
    EXPECT_TRUE(mark_error_called);
    EXPECT_FALSE(ctx.joined);
    EXPECT_EQ(machine.last_action, "");  // mark_error moved to the Error entry hook
}

TEST(nanoavb_mvrp_sm, reset_from_error_to_not_joined)
{
    sm_test::Observed<mvrp::Def, mvrp::table> machine;
    mvrp::Context ctx;
    bool reset_called = false;
    ctx.callbacks.init = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.send_join = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.mark_error = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.reset = [&](mvrp::Context&, TimePoint) { reset_called = true; };

    // NotJoined -> Joining -> Error
    machine.handle_event(ctx, mvrp::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, mvrp::Def::Event::Acquire, TimePoint{});
    machine.handle_event(ctx, mvrp::Def::Event::JoinFail, TimePoint{});
    EXPECT_EQ(machine.current_state(), mvrp::Def::State::Error);

    // Error -> NotJoined via Reset
    machine.handle_event(ctx, mvrp::Def::Event::Reset, TimePoint{});

    EXPECT_EQ(machine.current_state(), mvrp::Def::State::NotJoined);
    EXPECT_TRUE(reset_called);
    EXPECT_FALSE(ctx.joined);
    EXPECT_EQ(ctx.refcount, 0U);
    EXPECT_EQ(machine.last_action, "reset");
}

TEST(nanoavb_mvrp_sm, full_join_leave_cycle)
{
    sm_test::Observed<mvrp::Def, mvrp::table> machine;
    mvrp::Context ctx;
    ctx.callbacks.init = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.send_join = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.mark_joined = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.send_leave = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.mark_left = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.mark_error = [](mvrp::Context&, TimePoint) {};
    ctx.callbacks.reset = [](mvrp::Context&, TimePoint) {};

    // Full cycle: Start -> NotJoined -> Joining -> Joined -> Leaving -> NotJoined
    machine.handle_event(ctx, mvrp::Def::Event::UCT, TimePoint{});
    EXPECT_EQ(machine.current_state(), mvrp::Def::State::NotJoined);
    EXPECT_FALSE(ctx.joined);

    machine.handle_event(ctx, mvrp::Def::Event::Acquire, TimePoint{});
    EXPECT_EQ(machine.current_state(), mvrp::Def::State::Joining);

    machine.handle_event(ctx, mvrp::Def::Event::JoinOk, TimePoint{});
    EXPECT_EQ(machine.current_state(), mvrp::Def::State::Joined);
    EXPECT_TRUE(ctx.joined);

    machine.handle_event(ctx, mvrp::Def::Event::ReleaseLast, TimePoint{});
    EXPECT_EQ(machine.current_state(), mvrp::Def::State::Leaving);

    machine.handle_event(ctx, mvrp::Def::Event::LeaveOk, TimePoint{});
    EXPECT_EQ(machine.current_state(), mvrp::Def::State::NotJoined);
    EXPECT_FALSE(ctx.joined);
}

// ===========================================================================
// Supervisor SM — degrade_stop_streams direct tests
// ===========================================================================

TEST(nanoavb_supervisor_degrade, degrade_stop_streams_invokes_callback)
{
    supervisor::Context ctx;
    bool called = false;
    ctx.callbacks.degrade_stop_streams = [&](supervisor::Context&, TimePoint) { called = true; };
    statusbar::nanoavb::supervisor_sm::degrade_stop_streams(ctx, TimePoint{});
    EXPECT_TRUE(called);
}

TEST(nanoavb_supervisor_degrade, null_callback_no_crash)
{
    supervisor::Context ctx;
    // No callback installed: the action must guard against the empty callback
    // and return without crashing.
    statusbar::nanoavb::supervisor_sm::degrade_stop_streams(ctx, TimePoint{});
}

TEST(nanoavb_supervisor_degrade, gptp_lost_ready_triggers_degrade)
{
    sm_test::Observed<supervisor::Def, supervisor::table> machine;
    supervisor::Context ctx;
    bool called = false;
    ctx.callbacks.init_iface = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.start_protocols = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.enter_ready = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.degrade_stop_streams = [&](supervisor::Context&, TimePoint) { called = true; };

    machine.handle_event(ctx, supervisor::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, supervisor::Def::Event::LinkUp, TimePoint{});
    machine.handle_event(ctx, supervisor::Def::Event::GptpLocked, TimePoint{});
    machine.handle_event(ctx, supervisor::Def::Event::GptpLost, TimePoint{});
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Degraded);
    EXPECT_TRUE(called);
}

TEST(nanoavb_supervisor_degrade, degraded_recovers_on_gptp_lock)
{
    sm_test::Observed<supervisor::Def, supervisor::table> machine;
    supervisor::Context ctx;
    ctx.callbacks.init_iface = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.start_protocols = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.enter_ready = [](supervisor::Context&, TimePoint) {};
    ctx.callbacks.degrade_stop_streams = [](supervisor::Context&, TimePoint) {};

    machine.handle_event(ctx, supervisor::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, supervisor::Def::Event::LinkUp, TimePoint{});
    machine.handle_event(ctx, supervisor::Def::Event::GptpLocked, TimePoint{});
    machine.handle_event(ctx, supervisor::Def::Event::GptpLost, TimePoint{});
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Degraded);

    // gPTP re-locks: straight back to Ready (no VLAN gate).
    machine.handle_event(ctx, supervisor::Def::Event::GptpLocked, TimePoint{});
    EXPECT_EQ(machine.current_state(), supervisor::Def::State::Ready);
}

// ===========================================================================
// Talker Engine State Machine
// ===========================================================================

namespace talker = statusbar::nanoavb::talker_engine_sm;

TEST(nanoavb_talker_engine_sm, initial_state)
{
    sm_test::Observed<talker::Def, talker::table> machine;
    talker::Context ctx;
    EXPECT_EQ(machine.current_state(), talker::Def::State::Start);
}

TEST(nanoavb_talker_engine_sm, uct_to_off)
{
    sm_test::Observed<talker::Def, talker::table> machine;
    talker::Context ctx;
    bool called = false;
    ctx.callbacks.init = [&](talker::Context&, TimePoint) { called = true; };
    machine.handle_event(ctx, talker::Def::Event::UCT, TimePoint{});
    EXPECT_EQ(machine.current_state(), talker::Def::State::Off);
    EXPECT_TRUE(called);
    EXPECT_EQ(machine.last_action, "init");
}

TEST(nanoavb_talker_engine_sm, audio_ready_to_priming)
{
    sm_test::Observed<talker::Def, talker::table> machine;
    talker::Context ctx;
    ctx.callbacks.init = [](talker::Context&, TimePoint) {};
    ctx.callbacks.start_audio_source = [](talker::Context&, TimePoint) {};
    machine.handle_event(ctx, talker::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::AudioReady, TimePoint{});
    EXPECT_EQ(machine.current_state(), talker::Def::State::Priming);
    EXPECT_EQ(machine.last_action, "start_audio_source");
}

TEST(nanoavb_talker_engine_sm, primed_to_armed)
{
    sm_test::Observed<talker::Def, talker::table> machine;
    talker::Context ctx;
    ctx.callbacks.init = [](talker::Context&, TimePoint) {};
    ctx.callbacks.start_audio_source = [](talker::Context&, TimePoint) {};
    ctx.callbacks.arm_stream = [](talker::Context&, TimePoint) {};
    machine.handle_event(ctx, talker::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::AudioReady, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::Primed, TimePoint{});
    EXPECT_EQ(machine.current_state(), talker::Def::State::Armed);
    EXPECT_EQ(machine.last_action, "arm_stream");
}

TEST(nanoavb_talker_engine_sm, gate_go_to_running)
{
    sm_test::Observed<talker::Def, talker::table> machine;
    talker::Context ctx;
    ctx.callbacks.init = [](talker::Context&, TimePoint) {};
    ctx.callbacks.start_audio_source = [](talker::Context&, TimePoint) {};
    ctx.callbacks.arm_stream = [](talker::Context&, TimePoint) {};
    ctx.callbacks.start_tx = [](talker::Context&, TimePoint) {};
    machine.handle_event(ctx, talker::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::AudioReady, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::Primed, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::GateGo, TimePoint{});
    EXPECT_EQ(machine.current_state(), talker::Def::State::Running);
    EXPECT_EQ(machine.last_action, "start_tx");
}

TEST(nanoavb_talker_engine_sm, gate_stop_from_running)
{
    sm_test::Observed<talker::Def, talker::table> machine;
    talker::Context ctx;
    ctx.callbacks.init = [](talker::Context&, TimePoint) {};
    ctx.callbacks.start_audio_source = [](talker::Context&, TimePoint) {};
    ctx.callbacks.arm_stream = [](talker::Context&, TimePoint) {};
    ctx.callbacks.start_tx = [](talker::Context&, TimePoint) {};
    ctx.callbacks.stop_tx = [](talker::Context&, TimePoint) {};
    machine.handle_event(ctx, talker::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::AudioReady, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::Primed, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::GateGo, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::GateStop, TimePoint{});
    EXPECT_EQ(machine.current_state(), talker::Def::State::Armed);
    EXPECT_EQ(machine.last_action, "stop_tx");
}

TEST(nanoavb_talker_engine_sm, underrun_to_muted)
{
    sm_test::Observed<talker::Def, talker::table> machine;
    talker::Context ctx;
    ctx.callbacks.init = [](talker::Context&, TimePoint) {};
    ctx.callbacks.start_audio_source = [](talker::Context&, TimePoint) {};
    ctx.callbacks.arm_stream = [](talker::Context&, TimePoint) {};
    ctx.callbacks.start_tx = [](talker::Context&, TimePoint) {};
    ctx.callbacks.mute_tx = [](talker::Context&, TimePoint) {};
    machine.handle_event(ctx, talker::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::AudioReady, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::Primed, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::GateGo, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::Underrun, TimePoint{});
    EXPECT_EQ(machine.current_state(), talker::Def::State::Muted);
    EXPECT_EQ(machine.last_action, "mute_tx");
}

TEST(nanoavb_talker_engine_sm, recovered_from_muted)
{
    sm_test::Observed<talker::Def, talker::table> machine;
    talker::Context ctx;
    ctx.callbacks.init = [](talker::Context&, TimePoint) {};
    ctx.callbacks.start_audio_source = [](talker::Context&, TimePoint) {};
    ctx.callbacks.arm_stream = [](talker::Context&, TimePoint) {};
    ctx.callbacks.start_tx = [](talker::Context&, TimePoint) {};
    ctx.callbacks.mute_tx = [](talker::Context&, TimePoint) {};
    ctx.callbacks.unmute_tx = [](talker::Context&, TimePoint) {};
    machine.handle_event(ctx, talker::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::AudioReady, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::Primed, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::GateGo, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::Underrun, TimePoint{});
    machine.handle_event(ctx, talker::Def::Event::Recovered, TimePoint{});
    EXPECT_EQ(machine.current_state(), talker::Def::State::Running);
    EXPECT_EQ(machine.last_action, "unmute_tx");
}

TEST(nanoavb_talker_engine_sm, fatal_from_each_state)
{
    // Fatal from Running
    {
        talker::Machine m;
        talker::Context c;
        c.callbacks.init = [](talker::Context&, TimePoint) {};
        c.callbacks.start_audio_source = [](talker::Context&, TimePoint) {};
        c.callbacks.arm_stream = [](talker::Context&, TimePoint) {};
        c.callbacks.start_tx = [](talker::Context&, TimePoint) {};
        c.callbacks.stop_all = [](talker::Context&, TimePoint) {};
        m.handle_event(c, talker::Def::Event::UCT, TimePoint{});
        m.handle_event(c, talker::Def::Event::AudioReady, TimePoint{});
        m.handle_event(c, talker::Def::Event::Primed, TimePoint{});
        m.handle_event(c, talker::Def::Event::GateGo, TimePoint{});
        m.handle_event(c, talker::Def::Event::Fatal, TimePoint{});
        EXPECT_EQ(m.current_state(), talker::Def::State::Off);
    }
    // Fatal from Off
    {
        talker::Machine m;
        talker::Context c;
        c.callbacks.init = [](talker::Context&, TimePoint) {};
        c.callbacks.stop_all = [](talker::Context&, TimePoint) {};
        m.handle_event(c, talker::Def::Event::UCT, TimePoint{});
        m.handle_event(c, talker::Def::Event::Fatal, TimePoint{});
        EXPECT_EQ(m.current_state(), talker::Def::State::Off);
    }
    // Fatal from Muted
    {
        talker::Machine m;
        talker::Context c;
        c.callbacks.init = [](talker::Context&, TimePoint) {};
        c.callbacks.start_audio_source = [](talker::Context&, TimePoint) {};
        c.callbacks.arm_stream = [](talker::Context&, TimePoint) {};
        c.callbacks.start_tx = [](talker::Context&, TimePoint) {};
        c.callbacks.mute_tx = [](talker::Context&, TimePoint) {};
        c.callbacks.stop_all = [](talker::Context&, TimePoint) {};
        m.handle_event(c, talker::Def::Event::UCT, TimePoint{});
        m.handle_event(c, talker::Def::Event::AudioReady, TimePoint{});
        m.handle_event(c, talker::Def::Event::Primed, TimePoint{});
        m.handle_event(c, talker::Def::Event::GateGo, TimePoint{});
        m.handle_event(c, talker::Def::Event::Underrun, TimePoint{});
        m.handle_event(c, talker::Def::Event::Fatal, TimePoint{});
        EXPECT_EQ(m.current_state(), talker::Def::State::Off);
    }
}

//
// Test Runner
//
int statusbar_nanoavb_nanoavb_sm_test(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    int result = 0;
    result |= ::statusbar::test::TestRegister::run_section("nanoavb_sm_test");
    return result;
}