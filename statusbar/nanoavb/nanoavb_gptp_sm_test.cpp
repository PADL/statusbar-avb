// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_gptp_sm.hpp"

#include "statusbar/gptp/gptp_messages.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
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

// The grandmaster expires after announceReceiptTimeout announce intervals without an
// Announce (IEEE 802.1AS Clause 10.7.3.2); the next Announce reports it again.
TEST(nanoavb_gptp_announce, grandmaster_expires_after_the_receipt_timeout)
{
    int changes = 0;
    int losses = 0;
    statusbar::nanoavb::GptpAnnounceHandler handler{
        "nanoavb-announce-test0",  // no such interface: no socket, on_announce driven directly
        statusbar::nanoavb::GptpAnnounceCallbacks{
            .grandmaster_id_changed =
                [&](int64_t, ::statusbar::gptp::ClockIdentity const&, ::statusbar::gptp::AnnounceMessage const&) { ++changes; },
            .grandmaster_lost = [&](int64_t) { ++losses; }}};
    ::statusbar::gptp::AnnounceMessage announce{};
    announce.grandmaster_identity = ::statusbar::gptp::ClockIdentity{0x0011'22FF'FE33'4455ULL};
    announce.header.log_message_interval = 0;  // 1 s: lost after 4.5 s (Milan Table 4.2's max)

    constexpr int64_t t0 = 100'000'000'000;
    handler.on_announce(announce, t0);
    EXPECT_TRUE(handler.has_grandmaster());
    EXPECT_EQ(changes, 1);
    handler.tick(t0 + 1'400'000'000);
    handler.on_announce(announce, t0 + 1'500'000'000);  // a slow (+50%) sender, same grandmaster
    EXPECT_EQ(changes, 1);
    handler.tick(t0 + 4'500'000'000);
    EXPECT_TRUE(handler.has_grandmaster());
    handler.tick(t0 + 5'999'999'999);  // 4.5 s after it, less a ns: one Announce lost is tolerated
    EXPECT_TRUE(handler.has_grandmaster());
    handler.tick(t0 + 6'000'000'001);
    EXPECT_FALSE(handler.has_grandmaster());
    EXPECT_EQ(losses, 1);
    handler.tick(t0 + 9'000'000'000);  // reported once
    EXPECT_EQ(losses, 1);
    handler.on_announce(announce, t0 + 10'000'000'000);  // back: a change again
    EXPECT_TRUE(handler.has_grandmaster());
    EXPECT_EQ(changes, 2);

    // logMessageInterval -3 (125 ms): lost after 562.5 ms.
    announce.header.log_message_interval = static_cast<uint8_t>(-3);
    handler.on_announce(announce, t0 + 11'000'000'000);
    handler.tick(t0 + 11'562'500'000);
    EXPECT_TRUE(handler.has_grandmaster());
    handler.tick(t0 + 11'562'500'001);
    EXPECT_FALSE(handler.has_grandmaster());
    EXPECT_EQ(losses, 2);
}

// The Announce's PATH_TRACE TLV (IEEE 802.1AS-2020 10.6.3.3) is the AS path
// GET_AS_PATH reports (Milan v1.2 Section 5.4.2.24); its changes, and its
// clearing when the grandmaster is lost, are reported once each.
TEST(nanoavb_gptp_announce, path_trace_from_the_tlv)
{
    using statusbar::ieee::Eui64;
    ::statusbar::gptp::AnnounceMessage announce{};
    announce.init();
    announce.grandmaster_identity = ::statusbar::gptp::ClockIdentity{0x0011'22FF'FE33'4455ULL};
    announce.header.log_message_interval = 0;
    // The Announce, a PATH_TRACE TLV of two identities, and messageLength over both.
    std::array<uint8_t, ::statusbar::gptp::AnnounceMessage::LENGTH + 4 + 16> frame{};
    announce.header.message_length = static_cast<uint16_t>(frame.size());
    statusbar::span_store(std::span{frame}.first(::statusbar::gptp::AnnounceMessage::LENGTH), announce);
    auto tlv = std::span{frame}.subspan(::statusbar::gptp::AnnounceMessage::LENGTH);
    tlv[0] = 0x00;
    tlv[1] = 0x08;  // PATH_TRACE
    tlv[2] = 0x00;
    tlv[3] = 16;
    std::array<uint8_t, 8> const gm{0x00, 0x11, 0x22, 0xFF, 0xFE, 0x33, 0x44, 0x55};
    std::array<uint8_t, 8> const bridge{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0x00, 0x00, 0x02};
    std::copy(gm.begin(), gm.end(), tlv.begin() + 4);
    std::copy(bridge.begin(), bridge.end(), tlv.begin() + 12);

    auto const path = statusbar::nanoavb::parse_announce_path_trace(frame);
    EXPECT_EQ(path.size(), size_t{2});
    EXPECT_EQ(path[0].to_uint64(), 0x0011'22FF'FE33'4455ULL);
    EXPECT_EQ(path[1].to_uint64(), 0x001B'21FF'FE00'0002ULL);
    // A TLV that runs past messageLength is not read.
    std::array<uint8_t, frame.size()> cut = frame;
    cut[2] = 0;
    cut[3] = static_cast<uint8_t>(::statusbar::gptp::AnnounceMessage::LENGTH + 4 + 8);
    EXPECT_TRUE(statusbar::nanoavb::parse_announce_path_trace(cut).empty());

    int changes = 0;
    statusbar::nanoavb::GptpAnnounceHandler handler{
        "nanoavb-announce-test0", statusbar::nanoavb::GptpAnnounceCallbacks{.path_trace_changed = [&](int64_t) { ++changes; }}};
    constexpr int64_t t0 = 100'000'000'000;
    handler.on_announce(announce, t0, path);
    EXPECT_EQ(changes, 1);
    EXPECT_EQ(handler.path_trace().size(), size_t{2});
    handler.on_announce(announce, t0 + 1'000'000'000, path);  // the same path
    EXPECT_EQ(changes, 1);
    // Without the TLV: the grandmaster alone.
    handler.on_announce(announce, t0 + 2'000'000'000);
    EXPECT_EQ(changes, 2);
    EXPECT_EQ(handler.path_trace().size(), size_t{1});
    EXPECT_EQ(handler.path_trace()[0].to_uint64(), 0x0011'22FF'FE33'4455ULL);
    // Lost: empty.
    handler.tick(t0 + 10'000'000'000);
    EXPECT_FALSE(handler.has_grandmaster());
    EXPECT_TRUE(handler.path_trace().empty());
    EXPECT_EQ(changes, 3);
}
