// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_acmp.hpp"

#include <print>

namespace statusbar::nanoavb {

NanoAvbAcmpTalker::NanoAvbAcmpTalker(
    Eui64 entity_id, AcmpTalkerCallbacks callbacks, size_t max_streams, size_t max_listeners_per_stream)
    : callbacks_{std::move(callbacks)}
    , ctx_{max_streams, max_listeners_per_stream}
{
    ctx_.my_id = entity_id;

    // Wire up the internal state machine callback to our callback interface
    wire_talker_tx_response();
}

void NanoAvbAcmpTalker::set_callbacks(AcmpTalkerCallbacks callbacks)
{
    callbacks_ = std::move(callbacks);
    wire_talker_tx_response();
}

void NanoAvbAcmpTalker::wire_talker_tx_response()
{
    ctx_.tx_response = [this](AcmpCommandResponse const& resp) -> bool {
        if (resp.status() == ACMP_STATUS_SUCCESS) {
            if (resp.message_type() == ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE) {
                if (callbacks_.on_connect) {
                    callbacks_.on_connect(resp.talker_unique_id.get(), resp.listener_entity_id, resp.listener_unique_id.get());
                }
            } else if (resp.message_type() == ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE) {
                if (callbacks_.on_disconnect) {
                    callbacks_.on_disconnect(resp.talker_unique_id.get(), resp.listener_entity_id, resp.listener_unique_id.get());
                }
            }
        }
        if (callbacks_.tx_response) {
            return callbacks_.tx_response(resp);
        }
        return false;
    };
}

auto NanoAvbAcmpTalker::configure_stream(
    uint16_t const stream_index, Eui64 const stream_id, Eui48 const dest_mac, uint16_t const vlan_id) -> Status
{
    auto* stream = ctx_.get_stream(stream_index);
    if (stream == nullptr) {
        return failure(make_error_code(NanoAvbError::InvalidStreamIndex));
    }

    stream->stream_id = stream_id;
    stream->stream_dest_mac = dest_mac;
    stream->stream_vlan_id = vlan_id;
    return success();
}

auto NanoAvbAcmpTalker::receive_command(AcmpCommandResponse const& cmd, TimePoint event_time) -> bool
{
    // Check if this command is for us
    auto event = talker_event_for_command(ctx_, cmd);
    if (!event) {
        return false;
    }

    // Store the command in context
    ctx_.rcvd_cmd_resp = cmd;

    // Handle the event with the state machine
    sm_.handle_event(ctx_, *event, event_time);

    return true;
}

NanoAvbAcmpListener::NanoAvbAcmpListener(Eui64 entity_id, AcmpListenerCallbacks callbacks, size_t max_streams)
    : callbacks_{std::move(callbacks)}
    , ctx_{max_streams}
{
    ctx_.my_id = entity_id;

    wire_listener_callbacks();
}

void NanoAvbAcmpListener::set_callbacks(AcmpListenerCallbacks callbacks)
{
    callbacks_ = std::move(callbacks);
    wire_listener_callbacks();
}

void NanoAvbAcmpListener::wire_listener_callbacks()
{
    ctx_.tx_command = [this](AcmpCommandResponse const& cmd) -> bool {
        if (callbacks_.tx_command) {
            return callbacks_.tx_command(cmd);
        }
        return false;
    };

    ctx_.tx_response = [this](AcmpCommandResponse const& resp) -> bool {
        if (resp.status() == ACMP_STATUS_SUCCESS) {
            if (resp.message_type() == ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE) {
                if (callbacks_.on_connect) {
                    auto const* stream = ctx_.get_stream(resp.listener_unique_id.get());
                    if (stream != nullptr) {
                        callbacks_.on_connect(resp.listener_unique_id.get(), stream->stream_id, stream->stream_dest_mac);
                    }
                }
            } else if (resp.message_type() == ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE) {
                if (callbacks_.on_disconnect) {
                    callbacks_.on_disconnect(resp.listener_unique_id.get());
                }
            }
        }
        if (callbacks_.tx_response) {
            return callbacks_.tx_response(resp);
        }
        return false;
    };
}

auto NanoAvbAcmpListener::receive_controller_command(AcmpCommandResponse const& cmd, TimePoint event_time) -> bool
{
    // Check if this command is for us
    auto event = listener_event_for_controller_command(ctx_, cmd);
    if (!event) {
        return false;
    }

    // Store the command in context
    ctx_.rcvd_cmd_resp = cmd;

    // Handle the event with the state machine
    sm_.handle_event(ctx_, *event, event_time);

    return true;
}

auto NanoAvbAcmpListener::receive_talker_response(AcmpCommandResponse const& resp, TimePoint event_time) -> bool
{
    // Check if this response is for our pending command
    auto event = listener_event_for_talker_response(ctx_, resp);
    if (!event) {
        return false;
    }

    // Store the response in context
    ctx_.rcvd_cmd_resp = resp;

    // Handle the event with the state machine
    sm_.handle_event(ctx_, *event, event_time);

    return true;
}

auto NanoAvbAcmpListener::check_timeout(TimePoint current_time) -> bool
{
    if (listener_has_timeout(ctx_, current_time)) {
        sm_.handle_event(ctx_, ListenerEvent::TxTimeout, current_time);
        return true;
    }

    // Watchdog against a wedged listener. The SM only ever rests in Waiting:
    // every other state is either transient (entered and left inside a single
    // handle_event UCT chain) or a *_TxResp wait that persists strictly while a
    // talker request is pending. So a non-Waiting, non-Start state with NO
    // pending request is a wedge. It happens because send_connect_tx /
    // send_disconnect_tx take immediate error-response paths (LISTENER_UNKNOWN_ID
    // for an out-of-range stream, CONTROLLER_NOT_AUTHORIZED, LISTENER_EXCLUSIVE)
    // that clear_pending() and return -- yet the transition table still advances
    // the SM into ConnectTxResp / DisconnectTxResp. That state has no transition
    // for a fresh CONNECT_RX or GET_RX_STATE and (with nothing pending) no
    // timeout to fire, so the listener would silently ignore EVERY later
    // controller command until the process restarts. Recover to Waiting so the
    // next command is serviced. (Hardware-observed on node-a: listener stopped
    // answering ACMP after hours while the talker kept working.)
    auto const st = sm_.current_state();
    if (!ctx_.has_pending && st != ListenerState::Waiting && st != ListenerState::Start) {
        // TEMPORARY diagnostic: surface every wedge recovery so we can confirm in
        // production how often the error-path wedge fires (and remove this log
        // once we've stopped seeing it). The recovery itself is permanent.
        std::print(stderr, "[acmp-listener] watchdog: recovered wedged listener from state {} -> Waiting\n", static_cast<int>(st));
        ctx_.clear_pending();
        sm_.reset();          // -> Start
        start(current_time);  // UCT: Start -> Waiting
        return true;
    }
    return false;
}

//
// NanoAvbAcmpController
//

NanoAvbAcmpController::NanoAvbAcmpController(Eui64 entity_id, AcmpControllerCallbacks callbacks, size_t max_inflight)
    : callbacks_{std::move(callbacks)}
    , ctx_{max_inflight}
{
    ctx_.my_id = entity_id;
    wire_callbacks();
}

void NanoAvbAcmpController::set_callbacks(AcmpControllerCallbacks callbacks)
{
    callbacks_ = std::move(callbacks);
    wire_callbacks();
}

void NanoAvbAcmpController::wire_callbacks()
{
    ctx_.tx_command = [this](AcmpCommandResponse const& cmd) -> bool {
        if (callbacks_.tx_command) {
            return callbacks_.tx_command(cmd);
        }
        return false;
    };

    ctx_.process_response = [this](AcmpCommandResponse const& resp) {
        if (callbacks_.on_response) {
            callbacks_.on_response(resp);
        }
    };
}

auto NanoAvbAcmpController::send_command(
    uint8_t message_type, Eui64 talker_id, uint16_t talker_uid, Eui64 listener_id, uint16_t listener_uid, uint16_t connection_count)
    -> bool
{
    ctx_.command_params = AcmpCommandParams{
        .message_type = message_type,
        .talker_entity_id = talker_id,
        .listener_entity_id = listener_id,
        .talker_unique_id = talker_uid,
        .listener_unique_id = listener_uid,
        .connection_count = connection_count,
    };

    auto const before = ctx_.inflight_count();
    sm_.handle_event(ctx_, ControllerEvent::DoCommand, TimePoint::clock::now());
    return ctx_.inflight_count() > before;
}

auto NanoAvbAcmpController::connect(Eui64 talker_id, uint16_t talker_uid, Eui64 listener_id, uint16_t listener_uid) -> bool
{
    return send_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, talker_id, talker_uid, listener_id, listener_uid);
}

auto NanoAvbAcmpController::disconnect(Eui64 talker_id, uint16_t talker_uid, Eui64 listener_id, uint16_t listener_uid) -> bool
{
    return send_command(ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, talker_id, talker_uid, listener_id, listener_uid);
}

auto NanoAvbAcmpController::connect_tx(Eui64 talker_id, uint16_t talker_uid, Eui64 listener_id, uint16_t listener_uid) -> bool
{
    return send_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, talker_id, talker_uid, listener_id, listener_uid);
}

auto NanoAvbAcmpController::disconnect_tx(Eui64 talker_id, uint16_t talker_uid, Eui64 listener_id, uint16_t listener_uid) -> bool
{
    return send_command(ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND, talker_id, talker_uid, listener_id, listener_uid);
}

auto NanoAvbAcmpController::get_rx_state(Eui64 listener_id, uint16_t listener_uid) -> bool
{
    return send_command(ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, {}, 0, listener_id, listener_uid);
}

auto NanoAvbAcmpController::get_tx_state(Eui64 talker_id, uint16_t talker_uid) -> bool
{
    return send_command(ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, talker_id, talker_uid, {}, 0);
}

auto NanoAvbAcmpController::get_tx_connection(Eui64 talker_id, uint16_t talker_uid, uint16_t index) -> bool
{
    return send_command(ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND, talker_id, talker_uid, {}, 0, index);
}

auto NanoAvbAcmpController::receive_response(AcmpCommandResponse const& resp, TimePoint event_time) -> bool
{
    auto idx = ctx_.find_inflight(resp);
    if (idx >= ctx_.max_inflight()) {
        return false;
    }
    ctx_.rcvd_cmd_resp = resp;
    ctx_.current_inflight_index = idx;
    sm_.handle_event(ctx_, ControllerEvent::RcvdResponse, event_time);
    return true;
}

[[nodiscard]] static auto should_notify_timeout(
    bool was_retried, InflightCommand const* entry, AcmpControllerCallbacks const& callbacks) -> bool
{
    return was_retried && static_cast<bool>(callbacks.on_timeout) && entry != nullptr;
}

void NanoAvbAcmpController::tick(TimePoint now)
{
    auto idx = ctx_.find_timed_out(now);
    while (idx < ctx_.max_inflight()) {
        ctx_.current_inflight_index = idx;
        auto const* entry = ctx_.get_inflight(idx);
        bool const was_retried = entry != nullptr && entry->retried;

        sm_.handle_event(ctx_, ControllerEvent::Timeout, now);

        // If the entry was already retried, it was removed -> notify timeout
        if (should_notify_timeout(was_retried, entry, callbacks_)) {
            callbacks_.on_timeout(entry->command);
        }

        idx = ctx_.find_timed_out(now);
    }
}

}  // namespace statusbar::nanoavb
