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

namespace statusbar::atdecc::listener_actions {

void send_connect_tx(ListenerContext<>& ctx, sm::TimePoint const event_time)
{
    auto& cmd = ctx.rcvd_cmd_resp;

    // A talker exchange is already in flight: drop this command rather than
    // overwrite pending_command (the in-flight response would then be matched to
    // the wrong sequence_id and dropped) or let it inherit the in-flight command's
    // retried flag. The controller retries; the slot frees when the pending
    // exchange completes or times out (bounded by the ACMP timeout).
    if (ctx.has_pending) {
        return;
    }

    // Validate stream exists
    auto* stream = ctx.get_stream(cmd.listener_unique_id.get());
    if (stream == nullptr) {
        // Send error response
        ctx.response = cmd;
        ctx.response.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
        ctx.response.set_status(ACMP_STATUS_LISTENER_UNKNOWN_ID);
        if (ctx.tx_response) {
            ctx.tx_response(ctx.response);
        }
        ctx.clear_pending();
        return;
    }

    // Authorization check
    if (ctx.is_authorized && !ctx.is_authorized(cmd.controller_entity_id)) {
        ctx.response = cmd;
        ctx.response.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
        ctx.response.set_status(ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
        if (ctx.tx_response) {
            ctx.tx_response(ctx.response);
        }
        ctx.clear_pending();
        return;
    }

    // listenerIsConnected check (LISTENER_EXCLUSIVE) per IEEE 1722.1 Clause 8.2.3.4
    if (stream->connected) {
        bool const same_talker =
            stream->talker_entity_id == cmd.talker_entity_id && stream->talker_unique_id == cmd.talker_unique_id.get();
        if (!same_talker) {
            ctx.response = cmd;
            ctx.response.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
            ctx.response.set_status(ACMP_STATUS_LISTENER_EXCLUSIVE);
            if (ctx.tx_response) {
                ctx.tx_response(ctx.response);
            }
            ctx.clear_pending();
            return;
        }
        // Already connected to the SAME talker: this is a redundant CONNECT_RX (e.g.
        // a controller re-asserting an existing connection). Respond SUCCESS straight
        // from the stored stream state and do NOT re-run the handshake — re-sending
        // CONNECT_TX would make the talker re-process the connection and needlessly
        // churn its MSRP reservation / Listener-Ready state for a stream that is
        // already up, briefly disrupting live audio. True idempotency: confirm the
        // existing connection.
        ctx.response = cmd;
        ctx.response.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
        ctx.response.set_status(ACMP_STATUS_SUCCESS);
        ctx.response.stream_id = stream->stream_id;
        ctx.response.stream_dest_mac = stream->stream_dest_mac;
        ctx.response.connection_count = 1;
        if (ctx.tx_response) {
            ctx.tx_response(ctx.response);
        }
        ctx.clear_pending();
        return;
    }

    // Build CONNECT_TX_COMMAND to send to talker (copy and modify)
    AcmpCommandResponse tx_cmd = cmd;
    tx_cmd.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    tx_cmd.listener_entity_id = ctx.my_id;
    tx_cmd.connection_count = 0;

    // Store pending state. retried is reset here (not only in clear_pending) so a
    // fresh transaction always starts its retry budget clean, independent of how
    // the previous one ended.
    ctx.pending_command = cmd;
    ctx.current_stream_index = cmd.listener_unique_id.get();
    ctx.has_pending = true;
    ctx.retried = false;

    // Set timeout
    auto timeout = acmp_timeout_for_message_type(tx_cmd.message_type());
    ctx.pending_timeout = event_time + timeout;

    // Send the command to talker
    if (ctx.tx_command) {
        ctx.tx_command(tx_cmd);
    }
}

void send_disconnect_tx(ListenerContext<>& ctx, sm::TimePoint const event_time)
{
    auto& cmd = ctx.rcvd_cmd_resp;

    // Already-in-flight guard: see send_connect_tx. Don't clobber a pending
    // transaction or inherit its retried flag; drop and let the controller retry.
    if (ctx.has_pending) {
        return;
    }

    // Validate stream exists
    auto* stream = ctx.get_stream(cmd.listener_unique_id.get());
    if (stream == nullptr) {
        // Send error response
        ctx.response = cmd;
        ctx.response.set_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE);
        ctx.response.set_status(ACMP_STATUS_LISTENER_UNKNOWN_ID);
        if (ctx.tx_response) {
            ctx.tx_response(ctx.response);
        }
        ctx.clear_pending();
        return;
    }

    // Authorization check
    if (ctx.is_authorized && !ctx.is_authorized(cmd.controller_entity_id)) {
        ctx.response = cmd;
        ctx.response.set_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE);
        ctx.response.set_status(ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
        if (ctx.tx_response) {
            ctx.tx_response(ctx.response);
        }
        ctx.clear_pending();
        return;
    }

    // Check if stream is actually connected
    if (!stream->connected) {
        // Not connected - send success anyway (idempotent)
        ctx.response = cmd;
        ctx.response.set_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE);
        ctx.response.set_status(ACMP_STATUS_SUCCESS);
        if (ctx.tx_response) {
            ctx.tx_response(ctx.response);
        }
        ctx.clear_pending();
        return;
    }

    // The DISCONNECT_RX must name the talker this sink is actually connected to
    // (IEEE 1722.1 Clause 8.2.2.6.2). Otherwise a command for a different talker
    // would tear down the live connection to the one we ARE connected to. Reject a
    // mismatch with NO_SUCH_CONNECTION rather than disconnecting.
    bool const same_talker = stream->talker_entity_id == cmd.talker_entity_id
        && stream->talker_unique_id == cmd.talker_unique_id.get();
    if (!same_talker) {
        ctx.response = cmd;
        ctx.response.set_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE);
        ctx.response.set_status(ACMP_STATUS_NO_SUCH_CONNECTION);
        if (ctx.tx_response) {
            ctx.tx_response(ctx.response);
        }
        ctx.clear_pending();
        return;
    }

    // Build DISCONNECT_TX_COMMAND to send to talker (copy and modify)
    AcmpCommandResponse tx_cmd = cmd;
    tx_cmd.set_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND);
    tx_cmd.talker_entity_id = stream->talker_entity_id;  // Use stored talker
    tx_cmd.listener_entity_id = ctx.my_id;
    tx_cmd.talker_unique_id = stream->talker_unique_id;  // Use stored talker unique ID
    tx_cmd.connection_count = 0;
    tx_cmd.stream_vlan_id = stream->stream_vlan_id;

    // Store pending state. retried is reset here (not only in clear_pending) so a
    // fresh transaction always starts its retry budget clean, independent of how
    // the previous one ended.
    ctx.pending_command = cmd;
    ctx.current_stream_index = cmd.listener_unique_id.get();
    ctx.has_pending = true;
    ctx.retried = false;

    // Set timeout
    auto timeout = acmp_timeout_for_message_type(tx_cmd.message_type());
    ctx.pending_timeout = event_time + timeout;

    // Send the command to talker
    if (ctx.tx_command) {
        ctx.tx_command(tx_cmd);
    }
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
    ctx.response.set_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE);
    ctx.response.set_status(resp.status());

    if (resp.status() == ACMP_STATUS_SUCCESS) {
        // Clear stream state
        stream->talker_entity_id = {};
        stream->talker_unique_id = 0;
        stream->stream_id = {};
        stream->stream_dest_mac = {};
        stream->stream_vlan_id = 0;
        stream->connected = false;
    }

    // Send response to controller
    if (ctx.tx_response) {
        ctx.tx_response(ctx.response);
    }

    ctx.clear_pending();
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

void handle_timeout(ListenerContext<>& ctx, sm::TimePoint event_time)
{
    auto& orig_cmd = ctx.pending_command;

    // Retry once on connect timeout (not disconnect)
    bool const is_connect = orig_cmd.message_type() == ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND;
    if (is_connect && !ctx.retried) {
        ctx.retried = true;

        // Re-send CONNECT_TX_COMMAND to talker
        AcmpCommandResponse tx_cmd = orig_cmd;
        tx_cmd.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
        tx_cmd.listener_entity_id = ctx.my_id;
        tx_cmd.connection_count = 0;

        auto timeout = acmp_timeout_for_message_type(tx_cmd.message_type());
        ctx.pending_timeout = event_time + timeout;

        if (ctx.tx_command) {
            ctx.tx_command(tx_cmd);
        }
        return;
    }

    // Determine which response type to send based on the original command
    uint8_t response_type = ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE;
    if (orig_cmd.message_type() == ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND) {
        response_type = ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE;
    }

    // Build timeout response to controller
    ctx.response = orig_cmd;
    ctx.response.set_message_type(response_type);
    ctx.response.set_status(ACMP_STATUS_LISTENER_TALKER_TIMEOUT);

    // Send response to controller
    if (ctx.tx_response) {
        ctx.tx_response(ctx.response);
    }

    ctx.clear_pending();
}

}  // namespace statusbar::atdecc::listener_actions
