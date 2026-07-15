// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <system_error>

using namespace statusbar::nanoavb;
using namespace statusbar::atdecc;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;

//
// Test Helpers
//
static Eui64 make_entity_id(uint8_t id)
{
    return Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, id};
}

static Eui64 make_stream_id(Eui64 entity_id, uint16_t unique_id)
{
    // Stream ID is entity MAC + unique ID in network byte order
    statusbar::ieee::doublet_t const uid{unique_id};
    return Eui64{
        entity_id.span()[2],
        entity_id.span()[3],
        entity_id.span()[4],
        entity_id.span()[5],
        entity_id.span()[6],
        entity_id.span()[7],
        uid.span()[0],
        uid.span()[1]};
}

static AcmpCommandResponse make_connect_tx_command(
    Eui64 talker_id, uint16_t talker_unique, Eui64 listener_id, uint16_t listener_unique, uint16_t sequence_id = 1)
{
    AcmpCommandResponse cmd{};
    cmd.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    cmd.talker_entity_id = talker_id;
    cmd.talker_unique_id = talker_unique;
    cmd.listener_entity_id = listener_id;
    cmd.listener_unique_id = listener_unique;
    cmd.sequence_id = sequence_id;
    return cmd;
}

static AcmpCommandResponse make_disconnect_tx_command(
    Eui64 talker_id, uint16_t talker_unique, Eui64 listener_id, uint16_t listener_unique, uint16_t sequence_id = 1)
{
    AcmpCommandResponse cmd{};
    cmd.set_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND);
    cmd.talker_entity_id = talker_id;
    cmd.talker_unique_id = talker_unique;
    cmd.listener_entity_id = listener_id;
    cmd.listener_unique_id = listener_unique;
    cmd.sequence_id = sequence_id;
    return cmd;
}

static AcmpCommandResponse make_get_tx_state_command(Eui64 talker_id, uint16_t talker_unique, uint16_t sequence_id = 1)
{
    AcmpCommandResponse cmd{};
    cmd.set_message_type(ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND);
    cmd.talker_entity_id = talker_id;
    cmd.talker_unique_id = talker_unique;
    cmd.sequence_id = sequence_id;
    return cmd;
}

static AcmpCommandResponse make_connect_rx_command(
    Eui64 talker_id, uint16_t talker_unique, Eui64 listener_id, uint16_t listener_unique, uint16_t sequence_id = 1)
{
    AcmpCommandResponse cmd{};
    cmd.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    cmd.talker_entity_id = talker_id;
    cmd.talker_unique_id = talker_unique;
    cmd.listener_entity_id = listener_id;
    cmd.listener_unique_id = listener_unique;
    cmd.sequence_id = sequence_id;
    return cmd;
}

static AcmpCommandResponse make_get_rx_state_command(Eui64 listener_id, uint16_t listener_unique, uint16_t sequence_id = 1)
{
    AcmpCommandResponse cmd{};
    cmd.set_message_type(ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND);
    cmd.listener_entity_id = listener_id;
    cmd.listener_unique_id = listener_unique;
    cmd.sequence_id = sequence_id;
    return cmd;
}

//
// NanoAvbAcmpTalker Construction Tests
//
TEST(nanoavb_acmp_talker, construction)
{
    auto entity_id = make_entity_id(0x01);
    NanoAvbAcmpTalker talker{entity_id};

    EXPECT_EQ(talker.entity_id(), entity_id);
    EXPECT_EQ(talker.max_streams(), 16);  // Default
    EXPECT_EQ(talker.current_state(), TalkerState::Start);
}

TEST(nanoavb_acmp_talker, construction_with_max_streams)
{
    auto entity_id = make_entity_id(0x01);
    NanoAvbAcmpTalker talker{entity_id, {}, 8, 4};

    EXPECT_EQ(talker.max_streams(), 8);
}

TEST(nanoavb_acmp_talker, set_entity_id)
{
    auto entity_id1 = make_entity_id(0x01);
    auto entity_id2 = make_entity_id(0x02);

    NanoAvbAcmpTalker talker{entity_id1};
    talker.set_entity_id(entity_id2);

    EXPECT_EQ(talker.entity_id(), entity_id2);
}

//
// NanoAvbAcmpTalker Stream Configuration Tests
//
TEST(nanoavb_acmp_talker, configure_stream)
{
    auto entity_id = make_entity_id(0x01);
    NanoAvbAcmpTalker talker{entity_id};

    auto stream_id = make_stream_id(entity_id, 0);
    Eui48 dest_mac{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01};

    auto result = talker.configure_stream(0, stream_id, dest_mac, 2);
    EXPECT_TRUE(result.has_value());

    auto const* stream = talker.get_stream(0);
    EXPECT_TRUE(stream != nullptr);
    EXPECT_EQ(stream->stream_id, stream_id);
    EXPECT_EQ(stream->stream_dest_mac, dest_mac);
    EXPECT_EQ(stream->stream_vlan_id, 2);
}

TEST(nanoavb_acmp_talker, configure_stream_invalid_index)
{
    auto entity_id = make_entity_id(0x01);
    NanoAvbAcmpTalker talker{entity_id, {}, 4};

    auto stream_id = make_stream_id(entity_id, 0);
    Eui48 dest_mac{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01};

    auto result = talker.configure_stream(99, stream_id, dest_mac);
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(result.error() == make_error_code(NanoAvbError::InvalidStreamIndex));
}

//
// NanoAvbAcmpTalker CONNECT_TX_COMMAND Tests
//
TEST(nanoavb_acmp_talker, connect_tx_command)
{
    auto talker_id = make_entity_id(0x01);
    auto listener_id = make_entity_id(0x02);

    AcmpCommandResponse captured_response{};
    bool response_sent = false;

    AcmpTalkerCallbacks callbacks;
    callbacks.tx_response = [&](AcmpCommandResponse const& resp) {
        captured_response = resp;
        response_sent = true;
        return true;
    };

    NanoAvbAcmpTalker talker{talker_id, callbacks};
    talker.start();

    // Configure stream
    auto stream_id = make_stream_id(talker_id, 0);
    Eui48 dest_mac{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01};
    (void)talker.configure_stream(0, stream_id, dest_mac, 2);

    // Send CONNECT_TX_COMMAND
    auto cmd = make_connect_tx_command(talker_id, 0, listener_id, 0);
    auto now = statusbar::sm::TimePoint{};

    bool handled = talker.receive_command(cmd, now);

    EXPECT_TRUE(handled);
    EXPECT_TRUE(response_sent);
    EXPECT_EQ(captured_response.message_type(), ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    EXPECT_EQ(captured_response.status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(captured_response.connection_count, 1);
}

TEST(nanoavb_acmp_talker, connect_tx_invalid_stream)
{
    auto talker_id = make_entity_id(0x01);
    auto listener_id = make_entity_id(0x02);

    AcmpCommandResponse captured_response{};

    AcmpTalkerCallbacks callbacks;
    callbacks.tx_response = [&](AcmpCommandResponse const& resp) {
        captured_response = resp;
        return true;
    };

    NanoAvbAcmpTalker talker{talker_id, callbacks, 4};
    talker.start();

    // Send CONNECT_TX_COMMAND to non-configured stream
    auto cmd = make_connect_tx_command(talker_id, 99, listener_id, 0);  // Invalid stream
    auto now = statusbar::sm::TimePoint{};

    bool handled = talker.receive_command(cmd, now);

    EXPECT_TRUE(handled);
    EXPECT_EQ(captured_response.status(), ACMP_STATUS_TALKER_UNKNOWN_ID);
}

TEST(nanoavb_acmp_talker, disconnect_tx_command)
{
    auto talker_id = make_entity_id(0x01);
    auto listener_id = make_entity_id(0x02);

    AcmpCommandResponse captured_response{};

    AcmpTalkerCallbacks callbacks;
    callbacks.tx_response = [&](AcmpCommandResponse const& resp) {
        captured_response = resp;
        return true;
    };

    NanoAvbAcmpTalker talker{talker_id, callbacks};
    talker.start();

    // Configure stream
    auto stream_id = make_stream_id(talker_id, 0);
    Eui48 dest_mac{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01};
    (void)talker.configure_stream(0, stream_id, dest_mac);

    auto now = statusbar::sm::TimePoint{};

    // Connect first
    auto connect_cmd = make_connect_tx_command(talker_id, 0, listener_id, 0);
    (void)talker.receive_command(connect_cmd, now);
    EXPECT_EQ(talker.connection_count(0), 1);

    // Then disconnect
    auto disconnect_cmd = make_disconnect_tx_command(talker_id, 0, listener_id, 0);
    bool handled = talker.receive_command(disconnect_cmd, now);

    EXPECT_TRUE(handled);
    EXPECT_EQ(captured_response.message_type(), ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE);
    EXPECT_EQ(captured_response.status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(talker.connection_count(0), 0);
}

TEST(nanoavb_acmp_talker, get_tx_state_command)
{
    auto talker_id = make_entity_id(0x01);

    AcmpCommandResponse captured_response{};

    AcmpTalkerCallbacks callbacks;
    callbacks.tx_response = [&](AcmpCommandResponse const& resp) {
        captured_response = resp;
        return true;
    };

    NanoAvbAcmpTalker talker{talker_id, callbacks};
    talker.start();

    // Configure stream
    auto stream_id = make_stream_id(talker_id, 0);
    Eui48 dest_mac{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01};
    (void)talker.configure_stream(0, stream_id, dest_mac);

    // Send GET_TX_STATE_COMMAND
    auto cmd = make_get_tx_state_command(talker_id, 0);
    auto now = statusbar::sm::TimePoint{};

    bool handled = talker.receive_command(cmd, now);

    EXPECT_TRUE(handled);
    EXPECT_EQ(captured_response.message_type(), ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE);
    EXPECT_EQ(captured_response.status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(captured_response.stream_id, stream_id);
}

TEST(nanoavb_acmp_talker, ignores_wrong_entity_id)
{
    auto talker_id = make_entity_id(0x01);
    auto other_talker_id = make_entity_id(0x99);
    auto listener_id = make_entity_id(0x02);

    bool response_sent = false;

    AcmpTalkerCallbacks callbacks;
    callbacks.tx_response = [&](AcmpCommandResponse const&) {
        response_sent = true;
        return true;
    };

    NanoAvbAcmpTalker talker{talker_id, callbacks};
    talker.start();

    // Send command to different talker
    auto cmd = make_connect_tx_command(other_talker_id, 0, listener_id, 0);
    auto now = statusbar::sm::TimePoint{};

    bool handled = talker.receive_command(cmd, now);

    EXPECT_FALSE(handled);
    EXPECT_FALSE(response_sent);
}

//
// NanoAvbAcmpListener Construction Tests
//
TEST(nanoavb_acmp_listener, construction)
{
    auto entity_id = make_entity_id(0x01);
    NanoAvbAcmpListener listener{entity_id};

    EXPECT_EQ(listener.entity_id(), entity_id);
    EXPECT_EQ(listener.max_streams(), 16);  // Default
    EXPECT_EQ(listener.current_state(), ListenerState::Start);
    EXPECT_FALSE(listener.has_pending());
}

TEST(nanoavb_acmp_listener, construction_with_max_streams)
{
    auto entity_id = make_entity_id(0x01);
    NanoAvbAcmpListener listener{entity_id, {}, 8};

    EXPECT_EQ(listener.max_streams(), 8);
}

//
// NanoAvbAcmpListener Command Processing Tests
//
TEST(nanoavb_acmp_listener, get_rx_state_disconnected)
{
    auto listener_id = make_entity_id(0x01);

    AcmpCommandResponse captured_response{};

    AcmpListenerCallbacks callbacks;
    callbacks.tx_response = [&](AcmpCommandResponse const& resp) {
        captured_response = resp;
        return true;
    };

    NanoAvbAcmpListener listener{listener_id, callbacks};
    listener.start();

    // Send GET_RX_STATE_COMMAND
    auto cmd = make_get_rx_state_command(listener_id, 0);
    auto now = statusbar::sm::TimePoint{};

    bool handled = listener.receive_controller_command(cmd, now);

    EXPECT_TRUE(handled);
    EXPECT_EQ(captured_response.message_type(), ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE);
    EXPECT_EQ(captured_response.status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(captured_response.connection_count, 0);  // Not connected
}

TEST(nanoavb_acmp_listener, connect_rx_sends_to_talker)
{
    auto talker_id = make_entity_id(0x01);
    auto listener_id = make_entity_id(0x02);

    AcmpCommandResponse captured_command{};
    bool command_sent = false;

    AcmpListenerCallbacks callbacks;
    callbacks.tx_command = [&](AcmpCommandResponse const& cmd) {
        captured_command = cmd;
        command_sent = true;
        return true;
    };

    NanoAvbAcmpListener listener{listener_id, callbacks};
    listener.start();

    // Send CONNECT_RX_COMMAND from controller
    auto cmd = make_connect_rx_command(talker_id, 0, listener_id, 0);
    auto now = statusbar::sm::TimePoint{};

    bool handled = listener.receive_controller_command(cmd, now);

    EXPECT_TRUE(handled);
    EXPECT_TRUE(command_sent);
    EXPECT_EQ(captured_command.message_type(), ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_EQ(captured_command.talker_entity_id, talker_id);
    EXPECT_TRUE(listener.has_pending());
}

TEST(nanoavb_acmp_listener, connect_rx_response_success)
{
    auto talker_id = make_entity_id(0x01);
    auto listener_id = make_entity_id(0x02);

    AcmpCommandResponse captured_response{};
    bool response_sent = false;

    AcmpListenerCallbacks callbacks;
    callbacks.tx_command = [](AcmpCommandResponse const&) { return true; };
    callbacks.tx_response = [&](AcmpCommandResponse const& resp) {
        captured_response = resp;
        response_sent = true;
        return true;
    };

    NanoAvbAcmpListener listener{listener_id, callbacks};
    listener.start();

    auto now = statusbar::sm::TimePoint{};

    // Send CONNECT_RX_COMMAND from controller
    auto cmd = make_connect_rx_command(talker_id, 0, listener_id, 0);
    (void)listener.receive_controller_command(cmd, now);
    EXPECT_TRUE(listener.has_pending());

    // Send CONNECT_TX_RESPONSE from talker
    AcmpCommandResponse talker_resp{};
    talker_resp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    talker_resp.set_status(ACMP_STATUS_SUCCESS);
    talker_resp.talker_entity_id = talker_id;
    talker_resp.talker_unique_id = 0;
    talker_resp.listener_entity_id = listener_id;
    talker_resp.listener_unique_id = 0;
    talker_resp.sequence_id = cmd.sequence_id;
    talker_resp.stream_id = make_stream_id(talker_id, 0);
    talker_resp.stream_dest_mac = Eui48{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01};

    bool handled = listener.receive_talker_response(talker_resp, now);

    EXPECT_TRUE(handled);
    EXPECT_TRUE(response_sent);
    EXPECT_EQ(captured_response.message_type(), ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    EXPECT_EQ(captured_response.status(), ACMP_STATUS_SUCCESS);
    EXPECT_FALSE(listener.has_pending());
    EXPECT_TRUE(listener.is_connected(0));
}

// Fast connect (kit phase 5b, IEEE 1722.1 Clause 8.2.2.1.1): the listener
// originates the connection to a remembered talker with itself as the
// controller — no external controller in the loop.
TEST(nanoavb_acmp_listener, fast_connect_connects_without_controller)
{
    auto talker_id = make_entity_id(0x01);
    auto listener_id = make_entity_id(0x02);

    AcmpCommandResponse captured_command{};
    bool command_sent = false;
    bool connected_cb = false;

    AcmpListenerCallbacks callbacks;
    callbacks.tx_command = [&](AcmpCommandResponse const& cmd) {
        captured_command = cmd;
        command_sent = true;
        return true;
    };
    callbacks.tx_response = [](AcmpCommandResponse const&) { return true; };
    callbacks.on_connect = [&](uint16_t, Eui64 const&, Eui48) { connected_cb = true; };

    NanoAvbAcmpListener listener{listener_id, callbacks};
    listener.start();
    auto now = statusbar::sm::TimePoint{};

    // The fast connect sends a CONNECT_TX_COMMAND to the talker with the
    // FAST_CONNECT flag and ourselves as the controller.
    EXPECT_TRUE(listener.fast_connect(0, talker_id, 0, now));
    EXPECT_TRUE(command_sent);
    EXPECT_EQ(captured_command.message_type(), ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_EQ(captured_command.talker_entity_id, talker_id);
    EXPECT_EQ(captured_command.controller_entity_id, listener_id);
    EXPECT_TRUE(captured_command.is_fast_connect());
    EXPECT_TRUE(listener.has_pending());

    // Busy: a second fast connect while one is pending is refused.
    EXPECT_FALSE(listener.fast_connect(1, talker_id, 1, now));

    // The talker's SUCCESS response completes the connection like any other.
    AcmpCommandResponse talker_resp{};
    talker_resp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    talker_resp.set_status(ACMP_STATUS_SUCCESS);
    talker_resp.talker_entity_id = talker_id;
    talker_resp.talker_unique_id = 0;
    talker_resp.listener_entity_id = listener_id;
    talker_resp.listener_unique_id = 0;
    talker_resp.sequence_id = captured_command.sequence_id;
    talker_resp.stream_id = make_stream_id(talker_id, 0);
    talker_resp.stream_dest_mac = Eui48{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01};
    EXPECT_TRUE(listener.receive_talker_response(talker_resp, now));
    EXPECT_TRUE(listener.is_connected(0));
    EXPECT_TRUE(connected_cb);

    // Connected: another fast connect for the same sink is a no-op.
    EXPECT_FALSE(listener.fast_connect(0, talker_id, 0, now));
}

// A fast-connect goal retries from tick() while disconnected, stops once
// connected, and is forgotten on a deliberate controller DISCONNECT_RX.
TEST(nanoavb_acmp_listener, fast_connect_goal_retries_and_clears_on_disconnect)
{
    auto talker_id = make_entity_id(0x01);
    auto listener_id = make_entity_id(0x02);

    AcmpCommandResponse captured_command{};
    size_t commands_sent = 0;

    AcmpListenerCallbacks callbacks;
    callbacks.tx_command = [&](AcmpCommandResponse const& cmd) {
        captured_command = cmd;
        ++commands_sent;
        return true;
    };
    callbacks.tx_response = [](AcmpCommandResponse const&) { return true; };

    NanoAvbAcmpListener listener{listener_id, callbacks};
    listener.start();
    listener.set_fast_connect_goal(0, talker_id, 0);
    EXPECT_EQ(listener.fast_connect_goals().size(), size_t{1});

    // First tick attempts immediately.
    auto t0 = statusbar::sm::TimePoint{std::chrono::seconds{100}};
    listener.tick(t0);
    EXPECT_EQ(commands_sent, size_t{1});

    // The talker refuses (e.g. still booting): not connected, goal kept.
    AcmpCommandResponse refuse{};
    refuse.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    refuse.set_status(ACMP_STATUS_TALKER_MISBEHAVING);
    refuse.talker_entity_id = talker_id;
    refuse.listener_entity_id = listener_id;
    refuse.listener_unique_id = 0;
    refuse.sequence_id = captured_command.sequence_id;
    (void)listener.receive_talker_response(refuse, t0);
    EXPECT_FALSE(listener.is_connected(0));

    // Within the retry window nothing happens; past it the goal retries.
    listener.tick(t0 + std::chrono::milliseconds{100});
    EXPECT_EQ(commands_sent, size_t{1});
    auto t1 = t0 + std::chrono::seconds{3};
    listener.tick(t1);
    EXPECT_EQ(commands_sent, size_t{2});

    // This time the talker answers: connected; further ticks stay quiet.
    AcmpCommandResponse ok{};
    ok.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    ok.set_status(ACMP_STATUS_SUCCESS);
    ok.talker_entity_id = talker_id;
    ok.listener_entity_id = listener_id;
    ok.listener_unique_id = 0;
    ok.sequence_id = captured_command.sequence_id;
    ok.stream_id = make_stream_id(talker_id, 0);
    ok.stream_dest_mac = Eui48{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x02};
    (void)listener.receive_talker_response(ok, t1);
    EXPECT_TRUE(listener.is_connected(0));
    listener.tick(t1 + std::chrono::seconds{3});
    EXPECT_EQ(commands_sent, size_t{2});

    // A controller DISCONNECT_RX is deliberate: the goal is forgotten and
    // no tick ever re-connects it.
    auto disconnect = make_connect_rx_command(talker_id, 0, listener_id, 0);
    disconnect.set_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND);
    (void)listener.receive_controller_command(disconnect, t1 + std::chrono::seconds{4});
    EXPECT_TRUE(listener.fast_connect_goals().empty());
}

// Sticky bindings: with enable_sticky_bindings(), a controller-made connect is
// recorded as a fast-connect goal automatically and the goals-changed hook
// fires (the persistence seam) — including for the clearing DISCONNECT_RX.
TEST(nanoavb_acmp_listener, sticky_bindings_record_controller_connects)
{
    auto talker_id = make_entity_id(0x01);
    auto listener_id = make_entity_id(0x02);

    AcmpCommandResponse captured_command{};
    AcmpListenerCallbacks callbacks;
    callbacks.tx_command = [&](AcmpCommandResponse const& cmd) {
        captured_command = cmd;
        return true;
    };
    callbacks.tx_response = [](AcmpCommandResponse const&) { return true; };

    NanoAvbAcmpListener listener{listener_id, callbacks};
    listener.enable_sticky_bindings();
    size_t goals_changed = 0;
    listener.set_on_goals_changed([&]() { ++goals_changed; });
    listener.start();
    auto now = statusbar::sm::TimePoint{};

    // A normal controller connect...
    auto cmd = make_connect_rx_command(talker_id, 3, listener_id, 0);
    (void)listener.receive_controller_command(cmd, now);
    AcmpCommandResponse ok{};
    ok.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    ok.set_status(ACMP_STATUS_SUCCESS);
    ok.talker_entity_id = talker_id;
    ok.talker_unique_id = 3;
    ok.listener_entity_id = listener_id;
    ok.listener_unique_id = 0;
    ok.sequence_id = captured_command.sequence_id;
    ok.stream_id = make_stream_id(talker_id, 3);
    ok.stream_dest_mac = Eui48{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x03};
    (void)listener.receive_talker_response(ok, now);
    EXPECT_TRUE(listener.is_connected(0));

    // ...is remembered as a goal, and the persistence hook fired once.
    EXPECT_EQ(listener.fast_connect_goals().size(), size_t{1});
    EXPECT_EQ(listener.fast_connect_goals()[0].talker_entity_id, talker_id);
    EXPECT_EQ(listener.fast_connect_goals()[0].talker_unique_id, uint16_t{3});
    EXPECT_EQ(goals_changed, size_t{1});

    // A controller DISCONNECT clears the goal and fires the hook again.
    auto disconnect = make_connect_rx_command(talker_id, 3, listener_id, 0);
    disconnect.set_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND);
    (void)listener.receive_controller_command(disconnect, now);
    EXPECT_TRUE(listener.fast_connect_goals().empty());
    EXPECT_EQ(goals_changed, size_t{2});
}

// Regression (acmp#3): ACMP only clears a sink on an explicit DISCONNECT_RX, so a
// sink stays connected forever after its talker leaves. on_talker_departed() (wired
// to ADP ENTITY_DEPARTING) tears down only the sinks bound to the departed talker
// and fires on_disconnect so SRP is released.
TEST(nanoavb_acmp_listener, talker_departure_tears_down_connected_sink)
{
    auto talker_id = make_entity_id(0x01);
    auto other_talker = make_entity_id(0x03);
    auto listener_id = make_entity_id(0x02);

    std::vector<uint16_t> disconnected;
    AcmpListenerCallbacks callbacks;
    callbacks.tx_command = [](AcmpCommandResponse const&) { return true; };
    callbacks.tx_response = [](AcmpCommandResponse const&) { return true; };
    callbacks.on_disconnect = [&](uint16_t idx) { disconnected.push_back(idx); };

    NanoAvbAcmpListener listener{listener_id, callbacks};
    listener.start();
    auto now = statusbar::sm::TimePoint{};

    // Establish a connection on sink 0 to talker_id.
    auto cmd = make_connect_rx_command(talker_id, 0, listener_id, 0);
    (void)listener.receive_controller_command(cmd, now);
    AcmpCommandResponse talker_resp{};
    talker_resp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    talker_resp.set_status(ACMP_STATUS_SUCCESS);
    talker_resp.talker_entity_id = talker_id;
    talker_resp.talker_unique_id = 0;
    talker_resp.listener_entity_id = listener_id;
    talker_resp.listener_unique_id = 0;
    talker_resp.sequence_id = cmd.sequence_id;
    talker_resp.stream_id = make_stream_id(talker_id, 0);
    talker_resp.stream_dest_mac = Eui48{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01};
    (void)listener.receive_talker_response(talker_resp, now);
    EXPECT_TRUE(listener.is_connected(0));

    // A DIFFERENT talker departing does not touch our sink.
    EXPECT_EQ(listener.on_talker_departed(other_talker), 0U);
    EXPECT_TRUE(listener.is_connected(0));
    EXPECT_TRUE(disconnected.empty());

    // OUR talker departing tears the sink down and fires on_disconnect once.
    EXPECT_EQ(listener.on_talker_departed(talker_id), 1U);
    EXPECT_FALSE(listener.is_connected(0));
    EXPECT_EQ(disconnected.size(), 1U);
    if (!disconnected.empty()) {
        EXPECT_EQ(disconnected[0], 0U);
    }
}

// Regression (acmp#3, ageout): a talker that is powered off never sends
// ENTITY_DEPARTING, so only a discovery-database ageout catches it. Wiring the
// discovery's on_entity_departing to on_talker_departed (as NanoAvbNetHandlers does)
// tears the sink down when the talker's advertisement expires.
TEST(nanoavb_acmp_listener, talker_ageout_tears_down_connected_sink)
{
    auto talker_id = make_entity_id(0x01);
    auto listener_id = make_entity_id(0x02);

    AcmpListenerCallbacks callbacks;
    callbacks.tx_command = [](AcmpCommandResponse const&) { return true; };
    callbacks.tx_response = [](AcmpCommandResponse const&) { return true; };

    NanoAvbAcmpListener listener{listener_id, callbacks};
    listener.start();
    auto now = statusbar::sm::TimePoint{};

    // Establish a connection on sink 0 to talker_id.
    auto cmd = make_connect_rx_command(talker_id, 0, listener_id, 0);
    (void)listener.receive_controller_command(cmd, now);
    AcmpCommandResponse talker_resp{};
    talker_resp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    talker_resp.set_status(ACMP_STATUS_SUCCESS);
    talker_resp.talker_entity_id = talker_id;
    talker_resp.talker_unique_id = 0;
    talker_resp.listener_entity_id = listener_id;
    talker_resp.listener_unique_id = 0;
    talker_resp.sequence_id = cmd.sequence_id;
    talker_resp.stream_id = make_stream_id(talker_id, 0);
    talker_resp.stream_dest_mac = Eui48{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01};
    (void)listener.receive_talker_response(talker_resp, now);
    EXPECT_TRUE(listener.is_connected(0));

    // Discovery wired to the listener exactly as NanoAvbNetHandlers wires it.
    AdpDiscoveryCallbacks disc_cb;
    disc_cb.on_entity_departing = [&](Eui64 id) { (void)listener.on_talker_departed(id); };
    NanoAvbAdpDiscovery discovery{disc_cb};

    // The talker is advertising (valid_time field 1 -> valid ~2s).
    AdpDu adp{};
    adp.init_entity_available(talker_id, 1);
    adp.available_index = 1;
    discovery.receive_adpdu(adp, Eui48{}, now);
    EXPECT_TRUE(listener.is_connected(0));  // still up while advertising

    // Powered off: no more advertisements. Ticking past its validity ages it out ->
    // on_entity_departing -> the sink is torn down.
    discovery.tick(now + std::chrono::seconds(3));
    EXPECT_FALSE(listener.is_connected(0));
}

TEST(nanoavb_acmp_listener, ignores_wrong_entity_id)
{
    auto listener_id = make_entity_id(0x01);
    auto other_listener_id = make_entity_id(0x99);

    bool response_sent = false;

    AcmpListenerCallbacks callbacks;
    callbacks.tx_response = [&](AcmpCommandResponse const&) {
        response_sent = true;
        return true;
    };

    NanoAvbAcmpListener listener{listener_id, callbacks};
    listener.start();

    // Send command to different listener
    auto cmd = make_get_rx_state_command(other_listener_id, 0);
    auto now = statusbar::sm::TimePoint{};

    bool handled = listener.receive_controller_command(cmd, now);

    EXPECT_FALSE(handled);
    EXPECT_FALSE(response_sent);
}

TEST(nanoavb_acmp_listener, is_connected_initially_false)
{
    auto listener_id = make_entity_id(0x01);
    NanoAvbAcmpListener listener{listener_id};

    EXPECT_FALSE(listener.is_connected(0));
    EXPECT_FALSE(listener.is_connected(1));
}

// Regression (acmp#1): a CONNECT_RX whose send_connect_tx takes an immediate error
// path (here LISTENER_UNKNOWN_ID for an out-of-range sink) must return the SM to
// Waiting -- it must NOT park in a response-wait state with nothing pending (which
// had no timeout to escape and dropped every later controller command; the
// listener went silent after hours on node-a). Fixed in the listener transition
// table: the send actions return to Waiting. The next command is serviced
// immediately, with no watchdog tick required.
TEST(nanoavb_acmp_listener, error_path_does_not_wedge)
{
    auto listener_id = make_entity_id(0x01);
    auto talker_id = make_entity_id(0x02);

    int responses = 0;
    AcmpCommandResponse last_response{};
    AcmpListenerCallbacks callbacks;
    callbacks.tx_command = [](AcmpCommandResponse const&) { return true; };
    callbacks.tx_response = [&](AcmpCommandResponse const& r) {
        ++responses;
        last_response = r;
        return true;
    };

    NanoAvbAcmpListener listener{listener_id, callbacks, 2};  // valid sinks: 0, 1
    listener.start();
    auto now = statusbar::sm::TimePoint{};

    // CONNECT_RX for sink 5 (>= max_streams) -> LISTENER_UNKNOWN_ID error path.
    auto bad = make_connect_rx_command(talker_id, 0, listener_id, 5);
    (void)listener.receive_controller_command(bad, now);
    EXPECT_EQ(last_response.status(), ACMP_STATUS_LISTENER_UNKNOWN_ID);
    EXPECT_EQ(listener.current_state(), ListenerState::Waiting);  // no wedge
    EXPECT_FALSE(listener.has_pending());

    // The very next controller command is serviced immediately -- no watchdog tick.
    responses = 0;
    auto probe = make_get_rx_state_command(listener_id, 0);
    (void)listener.receive_controller_command(probe, now);
    EXPECT_EQ(responses, 1);
    EXPECT_EQ(last_response.message_type(), ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE);
}

TEST(nanoavb_acmp_callbacks, talker_on_connect_called)
{
    auto talker_id = make_entity_id(0x01);
    auto listener_id = make_entity_id(0x02);

    bool on_connect_called = false;
    uint16_t connected_stream_index = 0xFFFF;

    AcmpTalkerCallbacks callbacks;
    callbacks.tx_response = [](AcmpCommandResponse const&) { return true; };
    callbacks.on_connect = [&](uint16_t stream_index, Eui64, uint16_t) {
        on_connect_called = true;
        connected_stream_index = stream_index;
    };

    NanoAvbAcmpTalker talker{talker_id, callbacks};
    (void)talker.configure_stream(0, Eui64{0xAA, 0xBB, 0xCC, 0xDD, 0, 0, 0, 1}, Eui48{0x91, 0xE0, 0xF0, 0, 0, 1}, 2);
    talker.start();

    auto cmd = make_connect_tx_command(talker_id, 0, listener_id, 0);
    talker.receive_command(cmd, statusbar::sm::TimePoint{});

    EXPECT_TRUE(on_connect_called);
    EXPECT_EQ(connected_stream_index, 0U);
}

TEST(nanoavb_acmp_callbacks, talker_on_disconnect_called)
{
    auto talker_id = make_entity_id(0x01);
    auto listener_id = make_entity_id(0x02);

    bool on_disconnect_called = false;

    AcmpTalkerCallbacks callbacks;
    callbacks.tx_response = [](AcmpCommandResponse const&) { return true; };
    callbacks.on_connect = [](uint16_t, Eui64, uint16_t) {};
    callbacks.on_disconnect = [&](uint16_t, Eui64, uint16_t) { on_disconnect_called = true; };

    NanoAvbAcmpTalker talker{talker_id, callbacks};
    (void)talker.configure_stream(0, Eui64{0xAA, 0xBB, 0xCC, 0xDD, 0, 0, 0, 1}, Eui48{0x91, 0xE0, 0xF0, 0, 0, 1}, 2);
    talker.start();

    // Connect then disconnect
    auto cmd = make_connect_tx_command(talker_id, 0, listener_id, 0);
    talker.receive_command(cmd, statusbar::sm::TimePoint{});

    auto dcmd = make_disconnect_tx_command(talker_id, 0, listener_id, 0);
    talker.receive_command(dcmd, statusbar::sm::TimePoint{});

    EXPECT_TRUE(on_disconnect_called);
}

TEST(nanoavb_acmp_callbacks, talker_on_connect_not_called_on_failure)
{
    auto talker_id = make_entity_id(0x01);
    auto listener_id = make_entity_id(0x02);

    bool on_connect_called = false;

    AcmpTalkerCallbacks callbacks;
    callbacks.tx_response = [](AcmpCommandResponse const&) { return true; };
    callbacks.on_connect = [&](uint16_t, Eui64, uint16_t) { on_connect_called = true; };

    NanoAvbAcmpTalker talker{talker_id, callbacks};
    talker.start();

    // Connect to invalid stream index (99)
    auto cmd = make_connect_tx_command(talker_id, 99, listener_id, 0);
    talker.receive_command(cmd, statusbar::sm::TimePoint{});

    EXPECT_FALSE(on_connect_called);
}

TEST(nanoavb_acmp_srp, talker_callbacks_settable)
{
    auto entity_id = make_entity_id(0x01);

    bool srp_register_called = false;
    bool srp_deregister_called = false;

    AcmpTalkerCallbacks callbacks;
    callbacks.srp_register_stream = [&](uint16_t, Eui64 const&, Eui48, uint16_t) { srp_register_called = true; };
    callbacks.srp_deregister_stream = [&](uint16_t) { srp_deregister_called = true; };

    NanoAvbAcmpTalker talker{entity_id, callbacks};

    // Verify callbacks are set (call them directly to confirm)
    callbacks.srp_register_stream(0, Eui64{}, Eui48{}, 0);
    callbacks.srp_deregister_stream(0);
    EXPECT_TRUE(srp_register_called);
    EXPECT_TRUE(srp_deregister_called);
}

TEST(nanoavb_acmp_srp, listener_callbacks_settable)
{
    auto entity_id = make_entity_id(0x01);

    bool srp_attach_called = false;
    bool srp_detach_called = false;

    AcmpListenerCallbacks callbacks;
    callbacks.srp_attach_listener = [&](uint16_t, Eui64 const&) { srp_attach_called = true; };
    callbacks.srp_detach_listener = [&](uint16_t) { srp_detach_called = true; };

    NanoAvbAcmpListener listener{entity_id, callbacks};

    // Verify callbacks are set
    callbacks.srp_attach_listener(0, Eui64{});
    callbacks.srp_detach_listener(0);
    EXPECT_TRUE(srp_attach_called);
    EXPECT_TRUE(srp_detach_called);
}

TEST(nanoavb_acmp_srp, null_callbacks_safe)
{
    auto entity_id = make_entity_id(0x01);

    // Default callbacks - SRP callbacks should be null
    AcmpTalkerCallbacks tcbs;
    EXPECT_FALSE(static_cast<bool>(tcbs.srp_register_stream));
    EXPECT_FALSE(static_cast<bool>(tcbs.srp_deregister_stream));

    AcmpListenerCallbacks lcbs;
    EXPECT_FALSE(static_cast<bool>(lcbs.srp_attach_listener));
    EXPECT_FALSE(static_cast<bool>(lcbs.srp_detach_listener));
}

//
// NanoAvbAcmpController Tests
//

TEST(nanoavb_acmp_controller, construction)
{
    auto controller_id = make_entity_id(0x01);
    NanoAvbAcmpController controller{controller_id};

    EXPECT_EQ(controller.entity_id(), controller_id);
    EXPECT_EQ(controller.inflight_count(), 0U);
    EXPECT_EQ(controller.current_state(), ControllerState::Start);
}

TEST(nanoavb_acmp_controller, connect_sends_command)
{
    auto controller_id = make_entity_id(0x01);
    auto talker_id = make_entity_id(0x02);
    auto listener_id = make_entity_id(0x03);

    std::vector<AcmpCommandResponse> sent;
    AcmpControllerCallbacks callbacks;
    callbacks.tx_command = [&](AcmpCommandResponse const& cmd) {
        sent.push_back(cmd);
        return true;
    };

    NanoAvbAcmpController controller{controller_id, callbacks};
    controller.start();

    bool ok = controller.connect(talker_id, 0, listener_id, 0);
    EXPECT_TRUE(ok);
    EXPECT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].message_type(), ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    EXPECT_TRUE(sent[0].talker_entity_id == talker_id);
    EXPECT_TRUE(sent[0].listener_entity_id == listener_id);
    EXPECT_TRUE(sent[0].controller_entity_id == controller_id);
    EXPECT_EQ(controller.inflight_count(), 1U);
}

TEST(nanoavb_acmp_controller, disconnect_sends_command)
{
    auto controller_id = make_entity_id(0x01);

    std::vector<AcmpCommandResponse> sent;
    AcmpControllerCallbacks callbacks;
    callbacks.tx_command = [&](AcmpCommandResponse const& cmd) {
        sent.push_back(cmd);
        return true;
    };

    NanoAvbAcmpController controller{controller_id, callbacks};
    controller.start();

    controller.disconnect(make_entity_id(0x02), 0, make_entity_id(0x03), 0);
    EXPECT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].message_type(), ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND);
}

TEST(nanoavb_acmp_controller, receive_response)
{
    auto controller_id = make_entity_id(0x01);
    auto talker_id = make_entity_id(0x02);
    auto listener_id = make_entity_id(0x03);

    std::vector<AcmpCommandResponse> responses;
    AcmpControllerCallbacks callbacks;
    callbacks.tx_command = [](AcmpCommandResponse const&) { return true; };
    callbacks.on_response = [&](AcmpCommandResponse const& resp) { responses.push_back(resp); };

    NanoAvbAcmpController controller{controller_id, callbacks};
    controller.start();
    controller.connect(talker_id, 0, listener_id, 0);
    EXPECT_EQ(controller.inflight_count(), 1U);

    // Build matching response
    AcmpCommandResponse resp{};
    resp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    resp.set_status(ACMP_STATUS_SUCCESS);
    resp.controller_entity_id = controller_id;
    resp.sequence_id = 0;  // First command gets seq 0

    auto now = statusbar::sm::TimePoint{};
    bool handled = controller.receive_response(resp, now);
    EXPECT_TRUE(handled);
    EXPECT_EQ(responses.size(), 1U);
    EXPECT_EQ(controller.inflight_count(), 0U);
}

TEST(nanoavb_acmp_controller, unknown_response_ignored)
{
    auto controller_id = make_entity_id(0x01);

    AcmpControllerCallbacks callbacks;
    callbacks.tx_command = [](AcmpCommandResponse const&) { return true; };

    NanoAvbAcmpController controller{controller_id, callbacks};
    controller.start();

    AcmpCommandResponse resp{};
    resp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    resp.controller_entity_id = controller_id;
    resp.sequence_id = 99;  // No matching inflight

    EXPECT_FALSE(controller.receive_response(resp, statusbar::sm::TimePoint{}));
}

TEST(nanoavb_acmp_controller, get_rx_state_sends_command)
{
    auto controller_id = make_entity_id(0x01);

    std::vector<AcmpCommandResponse> sent;
    AcmpControllerCallbacks callbacks;
    callbacks.tx_command = [&](AcmpCommandResponse const& cmd) {
        sent.push_back(cmd);
        return true;
    };

    NanoAvbAcmpController controller{controller_id, callbacks};
    controller.start();
    controller.get_rx_state(make_entity_id(0x03), 0);

    EXPECT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].message_type(), ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND);
}

//
// Test Runner
//
int statusbar_nanoavb_nanoavb_acmp_test(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    int result = 0;
    result |= ::statusbar::test::TestRegister::run_section("nanoavb_acmp_test");
    return result;
}