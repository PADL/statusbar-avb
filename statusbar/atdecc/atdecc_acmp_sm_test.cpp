// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for ACMP State Machines
// Tests Controller, Listener, and Talker state machines per IEEE 1722.1-2021

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <source_location>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::ieee;
using namespace statusbar::sm;

//
// Test Helpers
//

// Fixed time point for deterministic testing
inline constexpr auto test_time_base = std::chrono::steady_clock::time_point{};
inline auto test_time(int ms)
{
    return test_time_base + std::chrono::milliseconds(ms);
}

// Test entity IDs
inline Eui64 const CONTROLLER_ID{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
inline Eui64 const TALKER_ID{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
inline Eui64 const LISTENER_ID{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
inline Eui64 const STREAM_ID{0xAA, 0xBB, 0xCC, 0xDD, 0x00, 0x00, 0x00, 0x01};
inline Eui48 const STREAM_MAC{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01};

//
// Controller State Machine Tests
//

TEST(controller_ctx, initial_state)
{
    ControllerContext ctx(4);
    ctx.my_id = CONTROLLER_ID;

    EXPECT_EQ(ctx.inflight_count(), 0U);
    EXPECT_EQ(ctx.next_sequence_id, 0U);
    EXPECT_EQ(ctx.current_inflight_index, 0U);
}

TEST(controller_ctx, add_inflight)
{
    ControllerContext ctx(4);
    ctx.my_id = CONTROLLER_ID;

    AcmpCommandResponse cmd{};
    cmd.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    cmd.sequence_id = 1;

    bool added = ctx.add_inflight(cmd, test_time(2000));
    EXPECT_TRUE(added);
    EXPECT_EQ(ctx.inflight_count(), 1U);
    EXPECT_TRUE(ctx.get_inflight(0) != nullptr);
}

TEST(controller_ctx, add_inflight_full)
{
    ControllerContext ctx(2);
    ctx.my_id = CONTROLLER_ID;

    AcmpCommandResponse cmd{};
    cmd.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);

    cmd.sequence_id = 1;
    EXPECT_TRUE(ctx.add_inflight(cmd, test_time(2000)));

    cmd.sequence_id = 2;
    EXPECT_TRUE(ctx.add_inflight(cmd, test_time(2000)));

    cmd.sequence_id = 3;
    EXPECT_FALSE(ctx.add_inflight(cmd, test_time(2000)));  // Full

    EXPECT_EQ(ctx.inflight_count(), 2U);
}

TEST(controller_ctx, remove_inflight)
{
    ControllerContext ctx(4);
    ctx.my_id = CONTROLLER_ID;

    AcmpCommandResponse cmd{};
    cmd.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    cmd.sequence_id = 1;

    ctx.add_inflight(cmd, test_time(2000));
    EXPECT_EQ(ctx.inflight_count(), 1U);

    ctx.remove_inflight(0);
    EXPECT_EQ(ctx.inflight_count(), 0U);
    EXPECT_TRUE(ctx.get_inflight(0) == nullptr);
}

TEST(controller_ctx, find_inflight)
{
    ControllerContext ctx(4);
    ctx.my_id = CONTROLLER_ID;

    AcmpCommandResponse cmd{};
    cmd.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    cmd.controller_entity_id = CONTROLLER_ID;
    cmd.sequence_id = 42;

    ctx.add_inflight(cmd, test_time(2000));

    // Create matching response
    AcmpCommandResponse resp{};
    resp.init_response(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, ACMP_STATUS_SUCCESS);
    resp.controller_entity_id = CONTROLLER_ID;
    resp.sequence_id = 42;

    size_t idx = ctx.find_inflight(resp);
    EXPECT_EQ(idx, 0U);

    // Non-matching sequence ID
    resp.sequence_id = 99;
    idx = ctx.find_inflight(resp);
    EXPECT_EQ(idx, ctx.max_inflight());  // Not found
}

TEST(controller_ctx, find_timed_out)
{
    ControllerContext ctx(4);
    ctx.my_id = CONTROLLER_ID;

    AcmpCommandResponse cmd{};
    cmd.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    cmd.sequence_id = 1;

    ctx.add_inflight(cmd, test_time(2000));

    // Before timeout
    size_t idx = ctx.find_timed_out(test_time(1999));
    EXPECT_EQ(idx, ctx.max_inflight());  // Not found

    // At timeout
    idx = ctx.find_timed_out(test_time(2000));
    EXPECT_EQ(idx, 0U);  // Found

    // After timeout
    idx = ctx.find_timed_out(test_time(2001));
    EXPECT_EQ(idx, 0U);  // Found
}

TEST(controller_sm, initial_state)
{
    AcmpControllerStateMachine<> sm;
    EXPECT_EQ(sm.current_state(), ControllerState::Start);
}

TEST(controller_sm, send_command)
{
    ControllerContext ctx(4);
    ctx.my_id = CONTROLLER_ID;

    std::vector<AcmpCommandResponse> sent_commands;
    ctx.tx_command = [&](AcmpCommandResponse const& cmd) {
        sent_commands.push_back(cmd);
        return true;
    };

    // Set up command parameters
    ctx.command_params.message_type = ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND;
    ctx.command_params.talker_entity_id = TALKER_ID;
    ctx.command_params.listener_entity_id = LISTENER_ID;
    ctx.command_params.talker_unique_id = 0;
    ctx.command_params.listener_unique_id = 0;

    AcmpControllerStateMachine<> sm;
    sm.handle_event(ctx, ControllerEvent::UCT, test_time(0));

    // Send DoCommand event
    sm.handle_event(ctx, ControllerEvent::DoCommand, test_time(0));

    // Should be back in Waiting state after UCT
    EXPECT_EQ(sm.current_state(), ControllerState::Waiting);

    // Command should have been sent
    EXPECT_EQ(sent_commands.size(), 1U);
    EXPECT_EQ(sent_commands[0].message_type(), ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_TRUE(sent_commands[0].controller_entity_id == CONTROLLER_ID);
    EXPECT_TRUE(sent_commands[0].talker_entity_id == TALKER_ID);

    // Should be added to inflight
    EXPECT_EQ(ctx.inflight_count(), 1U);
}

TEST(controller_sm, receive_response)
{
    ControllerContext ctx(4);
    ctx.my_id = CONTROLLER_ID;

    std::vector<AcmpCommandResponse> received_responses;
    ctx.tx_command = [](AcmpCommandResponse const&) { return true; };
    ctx.process_response = [&](AcmpCommandResponse const& resp) { received_responses.push_back(resp); };

    // Send a command first
    ctx.command_params.message_type = ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND;
    ctx.command_params.talker_entity_id = TALKER_ID;
    ctx.command_params.listener_entity_id = LISTENER_ID;

    AcmpControllerStateMachine<> sm;
    sm.handle_event(ctx, ControllerEvent::UCT, test_time(0));
    sm.handle_event(ctx, ControllerEvent::DoCommand, test_time(0));

    EXPECT_EQ(ctx.inflight_count(), 1U);

    // Create matching response
    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_response(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, ACMP_STATUS_SUCCESS);
    ctx.rcvd_cmd_resp.controller_entity_id = CONTROLLER_ID;
    ctx.rcvd_cmd_resp.sequence_id = 0;  // First command gets sequence 0

    // Find and set current inflight index
    ctx.current_inflight_index = ctx.find_inflight(ctx.rcvd_cmd_resp);
    EXPECT_EQ(ctx.current_inflight_index, 0U);

    // Handle response
    sm.handle_event(ctx, ControllerEvent::RcvdResponse, test_time(100));

    EXPECT_EQ(sm.current_state(), ControllerState::Waiting);
    EXPECT_EQ(received_responses.size(), 1U);
    EXPECT_EQ(ctx.inflight_count(), 0U);  // Removed from inflight
}

TEST(controller_sm, timeout_retry)
{
    ControllerContext ctx(4);
    ctx.my_id = CONTROLLER_ID;

    std::vector<AcmpCommandResponse> sent_commands;
    ctx.tx_command = [&](AcmpCommandResponse const& cmd) {
        sent_commands.push_back(cmd);
        return true;
    };

    ctx.command_params.message_type = ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND;
    ctx.command_params.talker_entity_id = TALKER_ID;

    AcmpControllerStateMachine<> sm;
    sm.handle_event(ctx, ControllerEvent::UCT, test_time(0));
    sm.handle_event(ctx, ControllerEvent::DoCommand, test_time(0));

    EXPECT_EQ(sent_commands.size(), 1U);
    EXPECT_EQ(ctx.inflight_count(), 1U);
    EXPECT_FALSE(ctx.get_inflight(0)->retried);

    // Simulate timeout - first timeout should retry
    ctx.current_inflight_index = 0;
    sm.handle_event(ctx, ControllerEvent::Timeout, test_time(2001));

    EXPECT_EQ(sent_commands.size(), 2U);  // Retried
    EXPECT_EQ(ctx.inflight_count(), 1U);  // Still inflight
    EXPECT_TRUE(ctx.get_inflight(0)->retried);

    // Second timeout should give up
    sm.handle_event(ctx, ControllerEvent::Timeout, test_time(4001));

    EXPECT_EQ(sent_commands.size(), 2U);  // No more retries
    EXPECT_EQ(ctx.inflight_count(), 0U);  // Removed
}

TEST(controller_helper, should_handle_response)
{
    ControllerContext ctx(4);
    ctx.my_id = CONTROLLER_ID;

    // Add an inflight command
    AcmpCommandResponse cmd{};
    cmd.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    cmd.controller_entity_id = CONTROLLER_ID;
    cmd.sequence_id = 5;
    ctx.add_inflight(cmd, test_time(2000));

    // Matching response
    AcmpCommandResponse resp{};
    resp.init_response(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, ACMP_STATUS_SUCCESS);
    resp.controller_entity_id = CONTROLLER_ID;
    resp.sequence_id = 5;

    EXPECT_TRUE(controller_should_handle_response(ctx, resp));

    // Wrong controller ID
    resp.controller_entity_id = TALKER_ID;
    EXPECT_FALSE(controller_should_handle_response(ctx, resp));

    // Command instead of response
    resp.controller_entity_id = CONTROLLER_ID;
    resp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_FALSE(controller_should_handle_response(ctx, resp));
}

TEST(controller_helper, has_timeout)
{
    ControllerContext ctx(4);
    ctx.my_id = CONTROLLER_ID;

    EXPECT_FALSE(controller_has_timeout(ctx, test_time(0)));

    AcmpCommandResponse cmd{};
    cmd.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    cmd.sequence_id = 1;
    ctx.add_inflight(cmd, test_time(2000));

    EXPECT_FALSE(controller_has_timeout(ctx, test_time(1999)));
    EXPECT_TRUE(controller_has_timeout(ctx, test_time(2000)));
    EXPECT_TRUE(controller_has_timeout(ctx, test_time(3000)));
}

//
// Talker State Machine Tests
//

TEST(talker_ctx, initial_state)
{
    TalkerContext ctx(4, 4);
    ctx.my_id = TALKER_ID;

    auto* stream = ctx.get_stream(0);
    EXPECT_TRUE(stream != nullptr);
    EXPECT_EQ(stream->connection_count(), 0U);
}

TEST(talker_ctx, get_stream_valid)
{
    TalkerContext ctx(4, 4);
    ctx.my_id = TALKER_ID;

    auto* stream0 = ctx.get_stream(0);
    auto* stream3 = ctx.get_stream(3);
    auto* stream4 = ctx.get_stream(4);  // Out of range

    EXPECT_TRUE(stream0 != nullptr);
    EXPECT_TRUE(stream3 != nullptr);
    EXPECT_TRUE(stream4 == nullptr);
}

TEST(talker_ctx, is_listener_connected)
{
    TalkerContext ctx(4, 4);
    ctx.my_id = TALKER_ID;

    auto* stream = ctx.get_stream(0);
    stream->add_listener({LISTENER_ID, 0});

    EXPECT_TRUE(ctx.is_listener_connected(0, LISTENER_ID, 0));
    EXPECT_FALSE(ctx.is_listener_connected(0, LISTENER_ID, 1));  // Wrong unique_id
    EXPECT_FALSE(ctx.is_listener_connected(1, LISTENER_ID, 0));  // Wrong stream
}

TEST(talker_sm, initial_state)
{
    AcmpTalkerStateMachine<> sm;
    EXPECT_EQ(sm.current_state(), TalkerState::Start);
}

TEST(talker_sm, connect_success)
{
    TalkerContext ctx(4, 4);
    ctx.my_id = TALKER_ID;

    // Set up stream with predefined info
    auto* stream = ctx.get_stream(0);
    stream->stream_id = STREAM_ID;
    stream->stream_dest_mac = STREAM_MAC;
    stream->stream_vlan_id = 2;

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    // Receive connect command
    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;

    AcmpTalkerStateMachine<> sm;
    sm.handle_event(ctx, TalkerEvent::UCT, test_time(0));
    sm.handle_event(ctx, TalkerEvent::RcvdConnectTx, test_time(0));

    EXPECT_EQ(sm.current_state(), TalkerState::Waiting);
    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].message_type(), ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(stream->connection_count(), 1U);
}

TEST(talker_sm, connect_unknown_id)
{
    TalkerContext ctx(4, 4);
    ctx.my_id = TALKER_ID;

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    // Receive connect command for invalid stream
    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.talker_unique_id = 99;  // Invalid stream ID

    AcmpTalkerStateMachine<> sm;
    sm.handle_event(ctx, TalkerEvent::UCT, test_time(0));
    sm.handle_event(ctx, TalkerEvent::RcvdConnectTx, test_time(0));

    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_TALKER_UNKNOWN_ID);
}

TEST(talker_sm, connect_idempotent)
{
    TalkerContext ctx(4, 4);
    ctx.my_id = TALKER_ID;

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;

    AcmpTalkerStateMachine<> sm;
    sm.handle_event(ctx, TalkerEvent::UCT, test_time(0));

    // Connect twice - should succeed both times (idempotent)
    sm.handle_event(ctx, TalkerEvent::RcvdConnectTx, test_time(0));
    sm.handle_event(ctx, TalkerEvent::RcvdConnectTx, test_time(100));

    EXPECT_EQ(sent_responses.size(), 2U);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(sent_responses[1].status(), ACMP_STATUS_SUCCESS);

    // Should only have one connection
    auto* stream = ctx.get_stream(0);
    EXPECT_EQ(stream->connection_count(), 1U);
}

TEST(talker_sm, disconnect_success)
{
    TalkerContext ctx(4, 4);
    ctx.my_id = TALKER_ID;

    // Pre-connect a listener
    auto* stream = ctx.get_stream(0);
    stream->add_listener({LISTENER_ID, 0});
    EXPECT_EQ(stream->connection_count(), 1U);

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND);
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;

    AcmpTalkerStateMachine<> sm;
    sm.handle_event(ctx, TalkerEvent::UCT, test_time(0));
    sm.handle_event(ctx, TalkerEvent::RcvdDisconnectTx, test_time(0));

    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].message_type(), ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(stream->connection_count(), 0U);
}

TEST(talker_sm, get_tx_state)
{
    TalkerContext ctx(4, 4);
    ctx.my_id = TALKER_ID;

    auto* stream = ctx.get_stream(0);
    stream->stream_id = STREAM_ID;
    stream->stream_dest_mac = STREAM_MAC;
    stream->add_listener({LISTENER_ID, 0});

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND);
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;

    AcmpTalkerStateMachine<> sm;
    sm.handle_event(ctx, TalkerEvent::UCT, test_time(0));
    sm.handle_event(ctx, TalkerEvent::RcvdGetTxState, test_time(0));

    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].message_type(), ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_SUCCESS);
    EXPECT_TRUE(sent_responses[0].stream_id == STREAM_ID);
    EXPECT_EQ(sent_responses[0].connection_count.get(), 1U);
}

TEST(talker_sm, get_tx_connection)
{
    TalkerContext ctx(4, 4);
    ctx.my_id = TALKER_ID;

    auto* stream = ctx.get_stream(0);
    stream->add_listener({LISTENER_ID, 5});

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND);
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;
    ctx.rcvd_cmd_resp.connection_count = 0;  // Get connection at index 0

    AcmpTalkerStateMachine<> sm;
    sm.handle_event(ctx, TalkerEvent::UCT, test_time(0));
    sm.handle_event(ctx, TalkerEvent::RcvdGetTxConnection, test_time(0));

    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].message_type(), ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_SUCCESS);
    EXPECT_TRUE(sent_responses[0].listener_entity_id == LISTENER_ID);
    EXPECT_EQ(sent_responses[0].listener_unique_id.get(), 5U);
}

TEST(talker_sm, get_tx_connection_echoes_index)
{
    TalkerContext ctx(4, 4);
    ctx.my_id = TALKER_ID;

    auto* stream = ctx.get_stream(0);
    // Connect 3 listeners
    stream->add_listener({LISTENER_ID, 0});
    stream->add_listener({Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}, 1});
    stream->add_listener({Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08}, 2});

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    // Request connection at index 1 - response connection_count should be 1, not 3
    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND);
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;
    ctx.rcvd_cmd_resp.connection_count = 1;

    AcmpTalkerStateMachine<> sm;
    sm.handle_event(ctx, TalkerEvent::UCT, test_time(0));
    sm.handle_event(ctx, TalkerEvent::RcvdGetTxConnection, test_time(0));

    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_SUCCESS);
    // connection_count should echo the requested index, not the total count
    EXPECT_EQ(sent_responses[0].connection_count.get(), 1U);
}

TEST(talker_sm, get_tx_connection_invalid_index)
{
    TalkerContext ctx(4, 4);
    ctx.my_id = TALKER_ID;

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND);
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;
    ctx.rcvd_cmd_resp.connection_count = 99;  // Invalid index

    AcmpTalkerStateMachine<> sm;
    sm.handle_event(ctx, TalkerEvent::UCT, test_time(0));
    sm.handle_event(ctx, TalkerEvent::RcvdGetTxConnection, test_time(0));

    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_NO_SUCH_CONNECTION);
}

TEST(talker_sm, connect_unauthorized)
{
    TalkerContext ctx(4, 4);
    ctx.my_id = TALKER_ID;

    // Set authorization callback that rejects all except CONTROLLER_ID
    ctx.is_authorized = [](Eui64 const& controller_id) { return controller_id == CONTROLLER_ID; };

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    // Connect from unauthorized controller
    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;
    ctx.rcvd_cmd_resp.controller_entity_id = Eui64{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    AcmpTalkerStateMachine<> sm;
    sm.handle_event(ctx, TalkerEvent::UCT, test_time(0));
    sm.handle_event(ctx, TalkerEvent::RcvdConnectTx, test_time(0));

    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
}

TEST(talker_sm, connect_authorized)
{
    TalkerContext ctx(4, 4);
    ctx.my_id = TALKER_ID;

    ctx.is_authorized = [](Eui64 const& controller_id) { return controller_id == CONTROLLER_ID; };

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;
    ctx.rcvd_cmd_resp.controller_entity_id = CONTROLLER_ID;

    AcmpTalkerStateMachine<> sm;
    sm.handle_event(ctx, TalkerEvent::UCT, test_time(0));
    sm.handle_event(ctx, TalkerEvent::RcvdConnectTx, test_time(0));

    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_SUCCESS);
}

TEST(talker_sm, connect_no_auth_callback)
{
    TalkerContext ctx(4, 4);
    ctx.my_id = TALKER_ID;
    // No is_authorized callback set - all controllers should be authorized

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;

    AcmpTalkerStateMachine<> sm;
    sm.handle_event(ctx, TalkerEvent::UCT, test_time(0));
    sm.handle_event(ctx, TalkerEvent::RcvdConnectTx, test_time(0));

    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_SUCCESS);
}

TEST(talker_helper, event_for_command)
{
    TalkerContext ctx(4, 4);
    ctx.my_id = TALKER_ID;

    AcmpCommandResponse cmd{};
    cmd.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    cmd.talker_entity_id = TALKER_ID;

    EXPECT_EQ(talker_event_for_command(ctx, cmd), TalkerEvent::RcvdConnectTx);

    cmd.set_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND);
    EXPECT_EQ(talker_event_for_command(ctx, cmd), TalkerEvent::RcvdDisconnectTx);

    cmd.set_message_type(ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND);
    EXPECT_EQ(talker_event_for_command(ctx, cmd), TalkerEvent::RcvdGetTxState);

    cmd.set_message_type(ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND);
    EXPECT_EQ(talker_event_for_command(ctx, cmd), TalkerEvent::RcvdGetTxConnection);

    // Wrong talker ID - should return nullopt
    cmd.talker_entity_id = CONTROLLER_ID;
    EXPECT_FALSE(talker_event_for_command(ctx, cmd).has_value());

    // Response instead of command - should return nullopt
    cmd.talker_entity_id = TALKER_ID;
    cmd.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    EXPECT_FALSE(talker_event_for_command(ctx, cmd).has_value());
}

//
// Listener State Machine Tests
//

TEST(listener_ctx, initial_state)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    EXPECT_FALSE(ctx.has_pending);
    EXPECT_EQ(ctx.current_stream_index, 0U);
}

TEST(listener_ctx, get_stream)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    auto* stream0 = ctx.get_stream(0);
    auto* stream3 = ctx.get_stream(3);
    auto* stream4 = ctx.get_stream(4);

    EXPECT_TRUE(stream0 != nullptr);
    EXPECT_TRUE(stream3 != nullptr);
    EXPECT_TRUE(stream4 == nullptr);
}

TEST(listener_ctx, pending_management)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    ctx.has_pending = true;
    ctx.pending_timeout = test_time(2000);

    EXPECT_FALSE(ctx.check_timeout(test_time(1999)));
    EXPECT_TRUE(ctx.check_timeout(test_time(2000)));

    ctx.clear_pending();
    EXPECT_FALSE(ctx.has_pending);
    EXPECT_FALSE(ctx.check_timeout(test_time(3000)));  // No pending = no timeout
}

TEST(listener_sm, initial_state)
{
    AcmpListenerStateMachine<> sm;
    EXPECT_EQ(sm.current_state(), ListenerState::Start);
}

TEST(listener_sm, connect_sends_to_talker)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    std::vector<AcmpCommandResponse> sent_commands;
    ctx.tx_command = [&](AcmpCommandResponse const& cmd) {
        sent_commands.push_back(cmd);
        return true;
    };

    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;
    ctx.rcvd_cmd_resp.sequence_id = 42;

    AcmpListenerStateMachine<> sm;
    sm.handle_event(ctx, ListenerEvent::UCT, test_time(0));
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(0));

    // Async send: the SM waits in Waiting with a pending command (the talker's
    // response / a timeout is handled from Waiting; there is no separate
    // response-wait state to park in).
    EXPECT_EQ(sm.current_state(), ListenerState::Waiting);

    // Command should have been sent to talker
    EXPECT_EQ(sent_commands.size(), 1U);
    EXPECT_EQ(sent_commands[0].message_type(), ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_TRUE(sent_commands[0].talker_entity_id == TALKER_ID);

    // Should have pending state
    EXPECT_TRUE(ctx.has_pending);
}

TEST(listener_sm, connect_unknown_stream)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.listener_unique_id = 99;  // Invalid stream

    AcmpListenerStateMachine<> sm;
    sm.handle_event(ctx, ListenerEvent::UCT, test_time(0));
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(0));

    // Should get error response immediately
    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_LISTENER_UNKNOWN_ID);

    // A synchronous error must return the SM to Waiting (not park in a dead
    // response-wait state) so the next command is accepted rather than dropped.
    EXPECT_EQ(sm.current_state(), ListenerState::Waiting);
    EXPECT_FALSE(ctx.has_pending);
}

// Regression (acmp#1): after a synchronous error CONNECT_RX the listener must not
// wedge -- a subsequent VALID CONNECT_RX is dispatched to the talker, not dropped.
TEST(listener_sm, error_then_valid_connect_not_wedged)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    std::vector<AcmpCommandResponse> sent_commands;
    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_command = [&](AcmpCommandResponse const& cmd) {
        sent_commands.push_back(cmd);
        return true;
    };
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    AcmpListenerStateMachine<> sm;
    sm.handle_event(ctx, ListenerEvent::UCT, test_time(0));

    // First: an error-path CONNECT_RX (unknown stream) -> error response, back to Waiting.
    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.listener_unique_id = 99;  // invalid stream
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(0));
    EXPECT_EQ(sm.current_state(), ListenerState::Waiting);
    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_LISTENER_UNKNOWN_ID);

    // Second: a VALID CONNECT_RX must now be processed -- a CONNECT_TX goes to the
    // talker. The wedge would have left the SM parked in ConnectTxResp and dropped
    // this command (sent_commands stays empty).
    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;  // valid stream
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(1));

    EXPECT_EQ(sent_commands.size(), 1U);
    EXPECT_EQ(sent_commands[0].message_type(), ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_TRUE(ctx.has_pending);
    EXPECT_EQ(sm.current_state(), ListenerState::Waiting);
}

// Regression (acmp#2): a second command arriving while a talker exchange is in
// flight must NOT clobber the pending transaction; the original completes and its
// controller gets the answer.
TEST(listener_sm, pending_connect_not_clobbered_by_second_command)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    std::vector<AcmpCommandResponse> sent_commands;
    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_command = [&](AcmpCommandResponse const& c) {
        sent_commands.push_back(c);
        return true;
    };
    ctx.tx_response = [&](AcmpCommandResponse const& r) {
        sent_responses.push_back(r);
        return true;
    };

    AcmpListenerStateMachine<> sm;
    sm.handle_event(ctx, ListenerEvent::UCT, test_time(0));

    // First CONNECT_RX (seq 1) -> pending, CONNECT_TX sent to talker.
    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;
    ctx.rcvd_cmd_resp.sequence_id = 1;
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(0));
    EXPECT_EQ(sent_commands.size(), 1U);
    EXPECT_TRUE(ctx.has_pending);
    EXPECT_EQ(ctx.pending_command.sequence_id.get(), 1U);

    // Second CONNECT_RX (seq 2) while the first is in flight -> dropped, does NOT
    // overwrite the pending transaction.
    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;
    ctx.rcvd_cmd_resp.sequence_id = 2;
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(1));
    EXPECT_EQ(sent_commands.size(), 1U);  // no second CONNECT_TX
    EXPECT_EQ(ctx.pending_command.sequence_id.get(), 1U);  // still the original

    // The talker responds to the ORIGINAL (seq 1); the controller gets its answer
    // (built from pending_command, so it carries seq 1 -- a clobber would have made
    // this seq 2 and left the seq-1 controller unanswered).
    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_response(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, ACMP_STATUS_SUCCESS);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.sequence_id = 1;
    sm.handle_event(ctx, ListenerEvent::RcvdConnectTxResp, test_time(2));
    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].message_type(), ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    EXPECT_EQ(sent_responses[0].sequence_id.get(), 1U);
}

// Regression (acmp#2): the busy gate holds through the retry window, and a fresh
// transaction always starts with a clean retry budget (retried reset).
TEST(listener_sm, busy_gate_during_retry_and_fresh_retry_budget)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    std::vector<AcmpCommandResponse> sent_commands;
    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_command = [&](AcmpCommandResponse const& c) {
        sent_commands.push_back(c);
        return true;
    };
    ctx.tx_response = [&](AcmpCommandResponse const& r) {
        sent_responses.push_back(r);
        return true;
    };

    auto connect_rx = [&](uint16_t seq) {
        ctx.rcvd_cmd_resp = {};
        ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
        ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
        ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
        ctx.rcvd_cmd_resp.listener_unique_id = 0;
        ctx.rcvd_cmd_resp.sequence_id = seq;
    };

    AcmpListenerStateMachine<> sm;
    sm.handle_event(ctx, ListenerEvent::UCT, test_time(0));

    connect_rx(1);
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(0));
    EXPECT_EQ(sent_commands.size(), 1U);

    // First timeout -> retry (retried set), a second CONNECT_TX for the same txn.
    sm.handle_event(ctx, ListenerEvent::TxTimeout, test_time(5000));
    EXPECT_EQ(sent_commands.size(), 2U);
    EXPECT_TRUE(ctx.retried);
    EXPECT_TRUE(ctx.has_pending);

    // A new CONNECT_RX during the retry window is dropped (no clobber, no 3rd send).
    connect_rx(2);
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(5001));
    EXPECT_EQ(sent_commands.size(), 2U);

    // Second timeout -> the original fails and pending clears (retried reset).
    sm.handle_event(ctx, ListenerEvent::TxTimeout, test_time(10000));
    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_LISTENER_TALKER_TIMEOUT);
    EXPECT_FALSE(ctx.has_pending);
    EXPECT_FALSE(ctx.retried);

    // A FRESH CONNECT_RX gets its OWN full retry budget: its first timeout retries
    // (a stale retried flag would have made it fail immediately instead).
    connect_rx(3);
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(11000));
    EXPECT_EQ(sent_commands.size(), 3U);
    EXPECT_FALSE(ctx.retried);
    sm.handle_event(ctx, ListenerEvent::TxTimeout, test_time(16000));
    EXPECT_EQ(sent_commands.size(), 4U);  // retried, not failed
    EXPECT_TRUE(ctx.retried);
}

TEST(listener_sm, connect_listener_exclusive)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    // Pre-connect stream 0 to TALKER_ID
    auto* stream = ctx.get_stream(0);
    stream->connected = true;
    stream->talker_entity_id = TALKER_ID;
    stream->talker_unique_id = 0;

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    // Try to connect from a different talker
    Eui64 const OTHER_TALKER{0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11, 0x22, 0x33};
    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_entity_id = OTHER_TALKER;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;

    AcmpListenerStateMachine<> sm;
    sm.handle_event(ctx, ListenerEvent::UCT, test_time(0));
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(0));

    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_LISTENER_EXCLUSIVE);
    EXPECT_FALSE(ctx.has_pending);
}

TEST(listener_sm, connect_same_talker_idempotent)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    // Pre-connect stream 0 to TALKER_ID with concrete stream params.
    auto* stream = ctx.get_stream(0);
    stream->connected = true;
    stream->talker_entity_id = TALKER_ID;
    stream->talker_unique_id = 0;
    stream->stream_id = Eui64{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    stream->stream_dest_mac = Eui48{0x91, 0xe0, 0xf0, 0x00, 0x12, 0x34};

    std::vector<AcmpCommandResponse> sent_commands;
    ctx.tx_command = [&](AcmpCommandResponse const& cmd) {
        sent_commands.push_back(cmd);
        return true;
    };
    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    // Re-CONNECT_RX from the SAME talker (a controller re-asserting an existing
    // connection) must be idempotent: confirm SUCCESS straight away, do NOT re-run
    // the handshake (no CONNECT_TX to the talker, no pending state) — re-handshaking
    // would churn the talker's MSRP reservation for a live stream.
    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;

    AcmpListenerStateMachine<> sm;
    sm.handle_event(ctx, ListenerEvent::UCT, test_time(0));
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(0));

    // No re-handshake: nothing sent to the talker, no pending.
    EXPECT_TRUE(sent_commands.empty());
    EXPECT_FALSE(ctx.has_pending);
    EXPECT_TRUE(stream->connected);  // still connected

    // Immediate idempotent SUCCESS response carrying the existing stream params.
    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].message_type(), ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(sent_responses[0].stream_id, stream->stream_id);
    EXPECT_EQ(sent_responses[0].stream_dest_mac, stream->stream_dest_mac);
}

TEST(listener_sm, connect_receives_response)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    std::vector<AcmpCommandResponse> sent_commands;
    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_command = [&](AcmpCommandResponse const& cmd) {
        sent_commands.push_back(cmd);
        return true;
    };
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    // Send connect command
    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;
    ctx.rcvd_cmd_resp.sequence_id = 1;

    AcmpListenerStateMachine<> sm;
    sm.handle_event(ctx, ListenerEvent::UCT, test_time(0));
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(0));

    EXPECT_EQ(sm.current_state(), ListenerState::Waiting);

    // Receive success response from talker
    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_response(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, ACMP_STATUS_SUCCESS);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.stream_id = STREAM_ID;
    ctx.rcvd_cmd_resp.stream_dest_mac = STREAM_MAC;
    ctx.rcvd_cmd_resp.sequence_id = 1;

    sm.handle_event(ctx, ListenerEvent::RcvdConnectTxResp, test_time(100));

    EXPECT_EQ(sm.current_state(), ListenerState::Waiting);
    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].message_type(), ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_SUCCESS);

    // Stream should be connected
    auto* stream = ctx.get_stream(0);
    EXPECT_TRUE(stream->connected);
    EXPECT_TRUE(stream->stream_id == STREAM_ID);
}

TEST(listener_sm, connect_timeout)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_command = [](AcmpCommandResponse const&) { return true; };
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;

    AcmpListenerStateMachine<> sm;
    sm.handle_event(ctx, ListenerEvent::UCT, test_time(0));
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(0));

    EXPECT_EQ(sm.current_state(), ListenerState::Waiting);

    // First timeout triggers retry
    sm.handle_event(ctx, ListenerEvent::TxTimeout, test_time(5000));
    EXPECT_EQ(sm.current_state(), ListenerState::Waiting);
    EXPECT_TRUE(ctx.has_pending);

    // Second timeout sends failure
    sm.handle_event(ctx, ListenerEvent::TxTimeout, test_time(10000));
    EXPECT_EQ(sm.current_state(), ListenerState::Waiting);
    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].message_type(), ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_LISTENER_TALKER_TIMEOUT);
    EXPECT_FALSE(ctx.has_pending);
}

TEST(listener_sm, connect_first_timeout_retries)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    std::vector<AcmpCommandResponse> sent_commands;
    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_command = [&](AcmpCommandResponse const& cmd) {
        sent_commands.push_back(cmd);
        return true;
    };
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;

    AcmpListenerStateMachine<> sm;
    sm.handle_event(ctx, ListenerEvent::UCT, test_time(0));
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(0));

    // First command sent
    EXPECT_EQ(sent_commands.size(), 1U);

    // First timeout - should retry, not send response
    sm.handle_event(ctx, ListenerEvent::TxTimeout, test_time(5));
    EXPECT_EQ(sent_commands.size(), 2U);   // Second command sent (retry)
    EXPECT_EQ(sent_responses.size(), 0U);  // No response to controller yet
    EXPECT_TRUE(ctx.has_pending);
    EXPECT_TRUE(ctx.retried);
}

TEST(listener_sm, connect_second_timeout_fails)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    std::vector<AcmpCommandResponse> sent_commands;
    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_command = [&](AcmpCommandResponse const& cmd) {
        sent_commands.push_back(cmd);
        return true;
    };
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;

    AcmpListenerStateMachine<> sm;
    sm.handle_event(ctx, ListenerEvent::UCT, test_time(0));
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(0));

    // First timeout - retry
    sm.handle_event(ctx, ListenerEvent::TxTimeout, test_time(5));

    // Second timeout - should give up
    sm.handle_event(ctx, ListenerEvent::TxTimeout, test_time(10));
    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_LISTENER_TALKER_TIMEOUT);
    EXPECT_FALSE(ctx.has_pending);
}

TEST(listener_sm, connect_response_after_retry)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    std::vector<AcmpCommandResponse> sent_commands;
    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_command = [&](AcmpCommandResponse const& cmd) {
        sent_commands.push_back(cmd);
        return true;
    };
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;

    AcmpListenerStateMachine<> sm;
    sm.handle_event(ctx, ListenerEvent::UCT, test_time(0));
    sm.handle_event(ctx, ListenerEvent::RcvdConnectRx, test_time(0));

    // First timeout - retry
    sm.handle_event(ctx, ListenerEvent::TxTimeout, test_time(5));

    // Talker responds after retry
    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    ctx.rcvd_cmd_resp.set_status(ACMP_STATUS_SUCCESS);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.talker_entity_id = TALKER_ID;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;
    ctx.rcvd_cmd_resp.talker_unique_id = 0;
    ctx.rcvd_cmd_resp.stream_id = STREAM_ID;
    ctx.rcvd_cmd_resp.stream_dest_mac = STREAM_MAC;
    sm.handle_event(ctx, ListenerEvent::RcvdConnectTxResp, test_time(7));

    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_SUCCESS);
    EXPECT_FALSE(ctx.has_pending);
}

TEST(listener_sm, disconnect_not_connected)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;

    AcmpListenerStateMachine<> sm;
    sm.handle_event(ctx, ListenerEvent::UCT, test_time(0));
    sm.handle_event(ctx, ListenerEvent::RcvdDisconnectRx, test_time(0));

    // Not connected - should return success anyway (idempotent)
    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].message_type(), ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_SUCCESS);
}

TEST(listener_sm, get_rx_state)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    // Set up connected stream
    auto* stream = ctx.get_stream(0);
    stream->connected = true;
    stream->talker_entity_id = TALKER_ID;
    stream->stream_id = STREAM_ID;
    stream->stream_dest_mac = STREAM_MAC;

    std::vector<AcmpCommandResponse> sent_responses;
    ctx.tx_response = [&](AcmpCommandResponse const& resp) {
        sent_responses.push_back(resp);
        return true;
    };

    ctx.rcvd_cmd_resp = {};
    ctx.rcvd_cmd_resp.init_command(ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND);
    ctx.rcvd_cmd_resp.listener_entity_id = LISTENER_ID;
    ctx.rcvd_cmd_resp.listener_unique_id = 0;

    AcmpListenerStateMachine<> sm;
    sm.handle_event(ctx, ListenerEvent::UCT, test_time(0));
    sm.handle_event(ctx, ListenerEvent::RcvdGetRxState, test_time(0));

    EXPECT_EQ(sm.current_state(), ListenerState::Waiting);
    EXPECT_EQ(sent_responses.size(), 1U);
    EXPECT_EQ(sent_responses[0].message_type(), ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE);
    EXPECT_EQ(sent_responses[0].status(), ACMP_STATUS_SUCCESS);
    EXPECT_TRUE(sent_responses[0].talker_entity_id == TALKER_ID);
    EXPECT_TRUE(sent_responses[0].stream_id == STREAM_ID);
    EXPECT_EQ(sent_responses[0].connection_count.get(), 1U);
}

TEST(listener_helper, event_for_controller_command)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    AcmpCommandResponse cmd{};
    cmd.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    cmd.listener_entity_id = LISTENER_ID;

    EXPECT_EQ(listener_event_for_controller_command(ctx, cmd), ListenerEvent::RcvdConnectRx);

    cmd.set_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND);
    EXPECT_EQ(listener_event_for_controller_command(ctx, cmd), ListenerEvent::RcvdDisconnectRx);

    cmd.set_message_type(ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND);
    EXPECT_EQ(listener_event_for_controller_command(ctx, cmd), ListenerEvent::RcvdGetRxState);

    // Wrong listener ID - should return nullopt
    cmd.listener_entity_id = TALKER_ID;
    EXPECT_FALSE(listener_event_for_controller_command(ctx, cmd).has_value());

    // Response instead of command - should return nullopt
    cmd.listener_entity_id = LISTENER_ID;
    cmd.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    EXPECT_FALSE(listener_event_for_controller_command(ctx, cmd).has_value());
}

TEST(listener_helper, event_for_talker_response)
{
    ListenerContext ctx(4);
    ctx.my_id = LISTENER_ID;

    // No pending - should return nullopt
    AcmpCommandResponse resp{};
    resp.init_response(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, ACMP_STATUS_SUCCESS);
    resp.listener_entity_id = LISTENER_ID;
    resp.sequence_id = 1;

    EXPECT_FALSE(listener_event_for_talker_response(ctx, resp).has_value());

    // Set up pending
    ctx.has_pending = true;
    ctx.pending_command.sequence_id = 1;

    EXPECT_EQ(listener_event_for_talker_response(ctx, resp), ListenerEvent::RcvdConnectTxResp);

    resp.set_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE);
    EXPECT_EQ(listener_event_for_talker_response(ctx, resp), ListenerEvent::RcvdDisconnectTxResp);

    // Wrong sequence ID - should return nullopt
    resp.sequence_id = 99;
    EXPECT_FALSE(listener_event_for_talker_response(ctx, resp).has_value());

    // Wrong listener ID - should return nullopt
    resp.sequence_id = 1;
    resp.listener_entity_id = TALKER_ID;
    EXPECT_FALSE(listener_event_for_talker_response(ctx, resp).has_value());
}

//
// Test Runner
//

TEST_MAIN(statusbar_atdecc, atdecc_acmp_sm_test)