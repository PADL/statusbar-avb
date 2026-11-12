// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACMP Listener State Machine action implementations

#include "statusbar/atdecc/atdecc_acmp_listener_sm.hpp"

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"
#include "statusbar/sm/sm_core.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace statusbar::atdecc::listener_actions {

namespace {

/// Finish ctx.response (already seeded from the command) with the message type
/// and status, send it to the controller, and leave nothing pending.
void finish_response(ListenerContext<>& ctx, uint8_t const response_type, uint8_t const status)
{
    ctx.response.set_message_type(response_type);
    ctx.response.set_status(status);
    if (ctx.tx_response) {
        ctx.tx_response(ctx.response);
    }
    ctx.clear_pending();
}

/// Answer the controller straight from its command: the figure's txResponse on
/// a path that never reaches the talker.
void answer_now(ListenerContext<>& ctx, AcmpCommandResponse const& cmd, uint8_t const response_type, uint8_t const status)
{
    ctx.response = cmd;
    finish_response(ctx, response_type, status);
}

/// Record tx_cmd as the talker exchange in flight for cmd, with the retry budget
/// fresh. retried is reset here (not only in clear_pending) so a new transaction
/// never inherits how the previous one ended.
void arm_pending(ListenerContext<>& ctx, AcmpCommandResponse const& cmd, AcmpCommandResponse const& tx_cmd)
{
    ctx.pending_command = cmd;
    ctx.pending_tx_command = tx_cmd;
    ctx.current_stream_index = cmd.listener_unique_id.get();
    ctx.has_pending = true;
    ctx.retried = false;
}

/// txCommand(..., FALSE) of IEEE 1722.1-2021 8.2.4.2.4: track tx_cmd for cmd,
/// start its timeout and send it to the talker.
void start_talker_exchange(
    ListenerContext<>& ctx, AcmpCommandResponse const& cmd, AcmpCommandResponse const& tx_cmd, sm::TimePoint const event_time)
{
    arm_pending(ctx, cmd, tx_cmd);
    ctx.pending_timeout = event_time + acmp_timeout_for_message_type(tx_cmd.message_type());
    if (ctx.tx_command) {
        ctx.tx_command(tx_cmd);
    }
}

/// The status a CONNECT_RX_COMMAND is refused with before any talker exchange,
/// or nullopt when it may proceed (to the talker, or to the idempotent answer
/// for a sink already connected to the same talker).
[[nodiscard]] auto connect_rx_refusal(
    ListenerContext<> const& ctx, AcmpCommandResponse const& cmd, ListenerStreamInfo const* stream) -> std::optional<uint8_t>
{
    if (stream == nullptr) {
        return ACMP_STATUS_LISTENER_UNKNOWN_ID;
    }
    if (ctx.is_authorized && !ctx.is_authorized(cmd.controller_entity_id)) {
        return ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED;
    }
    // listenerIsConnected check (LISTENER_EXCLUSIVE) per IEEE 1722.1 Clause 8.2.3.4
    bool const same_talker =
        stream->talker_entity_id == cmd.talker_entity_id && stream->talker_unique_id == cmd.talker_unique_id.get();
    if (stream->connected && !same_talker) {
        return ACMP_STATUS_LISTENER_EXCLUSIVE;
    }
    return std::nullopt;
}

/// Already connected to the SAME talker: this is a redundant CONNECT_RX (e.g.
/// a controller re-asserting an existing connection). Respond SUCCESS straight
/// from the stored stream state and do NOT re-run the handshake — re-sending
/// CONNECT_TX would make the talker re-process the connection and needlessly
/// churn its MSRP reservation / Listener-Ready state for a stream that is
/// already up, briefly disrupting live audio. True idempotency: confirm the
/// existing connection.
void answer_already_connected(ListenerContext<>& ctx, AcmpCommandResponse const& cmd, ListenerStreamInfo const& stream)
{
    ctx.response = cmd;
    ctx.response.stream_id = stream.stream_id;
    ctx.response.stream_dest_mac = stream.stream_dest_mac;
    ctx.response.connection_count = 1;
    finish_response(ctx, ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, ACMP_STATUS_SUCCESS);
}

/// The CONNECT_TX_COMMAND relayed to the talker for cmd.
[[nodiscard]] auto make_connect_tx(ListenerContext<> const& ctx, AcmpCommandResponse const& cmd) -> AcmpCommandResponse
{
    AcmpCommandResponse tx_cmd = cmd;
    tx_cmd.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    tx_cmd.listener_entity_id = ctx.my_id;
    tx_cmd.connection_count = 0;
    return tx_cmd;
}

/// The status a DISCONNECT_RX_COMMAND is answered with without a talker
/// exchange, or nullopt when the sink is connected to the named talker and the
/// disconnect proceeds.
[[nodiscard]] auto disconnect_rx_immediate_status(
    ListenerContext<> const& ctx, AcmpCommandResponse const& cmd, ListenerStreamInfo const* stream) -> std::optional<uint8_t>
{
    if (stream == nullptr) {
        return ACMP_STATUS_LISTENER_UNKNOWN_ID;
    }
    if (ctx.is_authorized && !ctx.is_authorized(cmd.controller_entity_id)) {
        return ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED;
    }
    if (!stream->connected) {
        // Not connected - send success anyway (idempotent)
        return ACMP_STATUS_SUCCESS;
    }
    // The DISCONNECT_RX must name the talker this sink is actually connected to
    // (IEEE 1722.1 Clause 8.2.2.6.2). Otherwise a command for a different talker
    // would tear down the live connection to the one we ARE connected to. Reject a
    // mismatch with NO_SUCH_CONNECTION rather than disconnecting.
    bool const same_talker =
        stream->talker_entity_id == cmd.talker_entity_id && stream->talker_unique_id == cmd.talker_unique_id.get();
    if (!same_talker) {
        return ACMP_STATUS_NO_SUCH_CONNECTION;
    }
    return std::nullopt;
}

/// The DISCONNECT_TX_COMMAND relayed to the talker for cmd. The command's talker
/// fields were checked against the stored connection, so they are the stored
/// talker's; the VLAN is the connection's.
[[nodiscard]] auto make_disconnect_tx(
    ListenerContext<> const& ctx, AcmpCommandResponse const& cmd, ListenerStreamInfo const& stream) -> AcmpCommandResponse
{
    AcmpCommandResponse tx_cmd = cmd;
    tx_cmd.set_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND);
    tx_cmd.listener_entity_id = ctx.my_id;
    tx_cmd.connection_count = 0;
    tx_cmd.stream_vlan_id = stream.stream_vlan_id;
    return tx_cmd;
}

}  // namespace

void send_connect_tx(ListenerContext<>& ctx, sm::TimePoint const event_time)
{
    // A talker exchange is already in flight: drop this command rather than
    // overwrite pending_command (the in-flight response would then be matched to
    // the wrong sequence_id and dropped) or let it inherit the in-flight command's
    // retried flag. The controller retries; the slot frees when the pending
    // exchange completes or times out (bounded by the ACMP timeout).
    if (ctx.has_pending) {
        return;
    }
    auto* stream = ctx.get_stream(ctx.rcvd_cmd_resp.listener_unique_id.get());
    if (auto const refusal = connect_rx_refusal(ctx, ctx.rcvd_cmd_resp, stream)) {
        answer_now(ctx, ctx.rcvd_cmd_resp, ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, *refusal);
        return;
    }
    if (stream->connected) {
        answer_already_connected(ctx, ctx.rcvd_cmd_resp, *stream);
        return;
    }
    start_talker_exchange(ctx, ctx.rcvd_cmd_resp, make_connect_tx(ctx, ctx.rcvd_cmd_resp), event_time);
}

void send_disconnect_tx(ListenerContext<>& ctx, sm::TimePoint const event_time)
{
    // Already-in-flight guard: see send_connect_tx. Don't clobber a pending
    // transaction or inherit its retried flag; drop and let the controller retry.
    if (ctx.has_pending) {
        return;
    }
    auto* stream = ctx.get_stream(ctx.rcvd_cmd_resp.listener_unique_id.get());
    if (auto const status = disconnect_rx_immediate_status(ctx, ctx.rcvd_cmd_resp, stream)) {
        answer_now(ctx, ctx.rcvd_cmd_resp, ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, *status);
        return;
    }
    auto const tx_cmd = make_disconnect_tx(ctx, ctx.rcvd_cmd_resp, *stream);
    // disconnectListener runs BEFORE the DISCONNECT_TX_COMMAND goes out (IEEE
    // 1722.1-2021 Figure 8-3, DISCONNECT RX COMMAND; 8.2.4.2.7): the sink is
    // free from here whatever the talker answers, or if it never does. Waiting
    // for the talker's SUCCESS left a sink connected to a dead talker for good.
    stream->reset();
    start_talker_exchange(ctx, ctx.rcvd_cmd_resp, tx_cmd, event_time);
}

void handle_connect_tx_response(ListenerContext<>& ctx, sm::TimePoint /*event_time*/)
{
    auto& resp = ctx.rcvd_cmd_resp;
    auto& orig_cmd = ctx.pending_command;

    // Get the stream
    auto* stream = ctx.get_stream(ctx.current_stream_index);
    if (stream == nullptr) {
        ctx.clear_pending();
        return;
    }

    // Build response to controller
    ctx.response = orig_cmd;
    ctx.response.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    ctx.response.set_status(resp.status());

    if (resp.status() == ACMP_STATUS_SUCCESS) {
        // Update stream state from talker response
        stream->talker_entity_id = resp.talker_entity_id;
        stream->talker_unique_id = resp.talker_unique_id.get();
        stream->stream_id = resp.stream_id;
        stream->stream_dest_mac = resp.stream_dest_mac;
        stream->stream_vlan_id = resp.stream_vlan_id.get();
        stream->connected = true;

        // Copy stream info to response
        ctx.response.stream_id = resp.stream_id;
        ctx.response.stream_dest_mac = resp.stream_dest_mac;
        ctx.response.connection_count = resp.connection_count;
    }

    // Send response to controller
    if (ctx.tx_response) {
        ctx.tx_response(ctx.response);
    }

    ctx.clear_pending();
}

void handle_disconnect_tx_response(ListenerContext<>& ctx, sm::TimePoint /*event_time*/)
{
    // The sink was cleared when the DISCONNECT_TX_COMMAND went out; the talker's
    // status is relayed to the controller as it stands (Figure 8-3, DISCONNECT
    // TX RESPONSE), whatever it is.
    answer_now(ctx, ctx.pending_command, ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, ctx.rcvd_cmd_resp.status());
}

void handle_get_rx_state(ListenerContext<>& ctx, sm::TimePoint /*event_time*/)
{
    auto& cmd = ctx.rcvd_cmd_resp;

    // Initialize response from command
    ctx.response = cmd;
    ctx.response.set_message_type(ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE);

    // Validate stream exists
    auto const* stream = ctx.get_stream(cmd.listener_unique_id.get());
    if (stream == nullptr) {
        ctx.response.set_status(ACMP_STATUS_LISTENER_UNKNOWN_ID);
        if (ctx.tx_response) {
            ctx.tx_response(ctx.response);
        }
        return;
    }

    ctx.response.set_status(ACMP_STATUS_SUCCESS);

    // Fill in stream info
    ctx.response.talker_entity_id = stream->talker_entity_id;
    ctx.response.talker_unique_id = stream->talker_unique_id;
    ctx.response.stream_id = stream->stream_id;
    ctx.response.stream_dest_mac = stream->stream_dest_mac;
    ctx.response.stream_vlan_id = stream->stream_vlan_id;
    ctx.response.connection_count = stream->connected ? 1 : 0;

    if (ctx.tx_response) {
        ctx.tx_response(ctx.response);
    }
}

void handle_timeout(ListenerContext<>& ctx, sm::TimePoint const event_time)
{
    // One retry for CONNECT_TX and DISCONNECT_TX alike (Figure 8-3 CONNECT TX
    // TIMEOUT and DISCONNECT TX TIMEOUT; 8.1.1 and 8.1.2): txCommand(..., TRUE)
    // re-sends the same command and restarts its timeout.
    if (!ctx.retried) {
        ctx.retried = true;
        ctx.pending_timeout = event_time + acmp_timeout_for_message_type(ctx.pending_tx_command.message_type());
        if (ctx.tx_command) {
            ctx.tx_command(ctx.pending_tx_command);
        }
        return;
    }
    // The retry timed out too: the controller hears LISTENER_TALKER_TIMEOUT.
    bool const is_disconnect = ctx.pending_command.message_type() == ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND;
    uint8_t const response_type = is_disconnect ? ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE : ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE;
    answer_now(ctx, ctx.pending_command, response_type, ACMP_STATUS_LISTENER_TALKER_TIMEOUT);
}

}  // namespace statusbar::atdecc::listener_actions
