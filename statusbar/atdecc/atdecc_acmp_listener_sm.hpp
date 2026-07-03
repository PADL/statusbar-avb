#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACMP Listener State Machine - IEEE 1722.1-2021 Clause 8.2.5
/// Implements the ATDECC Listener state machine for managing stream connections

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/sm/sm.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <source_location>

namespace statusbar::atdecc {

//
// Listener State Machine Definition (Figure 8-4)
//

/// Listener state machine states
enum class ListenerState : uint8_t
{
    Start,            // Initial state after construction
    Waiting,          // Idle state; also where async talker responses/timeouts are handled
    ConnectTxCmd,     // Sending CONNECT_TX_COMMAND to talker
    DisconnectTxCmd,  // Sending DISCONNECT_TX_COMMAND to talker
    ConnectTxResp,    // Deprecated: response-wait is now folded into Waiting (kept for API compat)
    DisconnectTxResp, // Deprecated: response-wait is now folded into Waiting (kept for API compat)
    GetState,         // Processing GET_RX_STATE_COMMAND
    Count
};

/// Listener state machine events
enum class ListenerEvent : uint8_t
{
    UCT,                   // Unconditional transition
    RcvdConnectRx,         // Received CONNECT_RX_COMMAND from controller
    RcvdDisconnectRx,      // Received DISCONNECT_RX_COMMAND from controller
    RcvdGetRxState,        // Received GET_RX_STATE_COMMAND from controller
    RcvdConnectTxResp,     // Received CONNECT_TX_RESPONSE from talker
    RcvdDisconnectTxResp,  // Received DISCONNECT_TX_RESPONSE from talker
    TxTimeout,             // Timeout waiting for talker response
    Count
};

/// Listener state machine context - holds all state and callbacks
/// @tparam MaxStreams Compile-time capacity for listener streams
template <size_t MaxStreams = 32>
struct ListenerContext
{
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    /// Construct with specified max streams (reserves capacity, no runtime allocations)
    /// @param max_streams Maximum number of listener streams
    explicit ListenerContext(size_t max_streams = 16)
        : max_streams_{max_streams}
    {
        streams_.resize(max_streams);
    }

    // Entity ID of this listener
    ieee::Eui64 my_id{};

    // Received command/response being processed
    AcmpCommandResponse rcvd_cmd_resp{};

    // Pending command waiting for talker response
    AcmpCommandResponse pending_command{};

    // Response to send
    AcmpCommandResponse response{};

    // Timeout time for pending command
    TimePoint pending_timeout{};

    // Whether we have a pending command
    bool has_pending{false};

    // Whether we have already retried the pending connect command
    bool retried{false};

    // The stream index being processed
    uint16_t current_stream_index{0};

    // Callbacks - must be set before use
    statusbar::sg14::inplace_function<bool(AcmpCommandResponse const&), 64> tx_command{};
    statusbar::sg14::inplace_function<bool(AcmpCommandResponse const&), 64> tx_response{};

    /// Authorization callback: returns true if the given controller is allowed
    /// to issue ACMP commands. If not set, all controllers are authorized.
    statusbar::sg14::inplace_function<bool(ieee::Eui64 const& controller_entity_id), 64> is_authorized;

    // Stream management

    /// Get max streams capacity
    [[nodiscard]] auto max_streams() const noexcept { return max_streams_; }

    /// Get stream info for a unique_id, or nullptr if out of range
    /// @param unique_id Stream unique identifier
    [[nodiscard]] auto get_stream(uint16_t const unique_id) noexcept -> ListenerStreamInfo*
    {
        if (unique_id < max_streams_) {
            return &streams_[unique_id];
        }
        return nullptr;
    }

    /// @param unique_id Stream unique identifier
    [[nodiscard]] auto get_stream(uint16_t const unique_id) const noexcept -> ListenerStreamInfo const*
    {
        if (unique_id < max_streams_) {
            return &streams_[unique_id];
        }
        return nullptr;
    }

    /// Clear the pending command state
    void clear_pending() noexcept
    {
        has_pending = false;
        retried = false;
        pending_command = AcmpCommandResponse{};
    }

    /// Check if the pending command has timed out
    /// @param current_time Current time point to compare against pending timeout
    [[nodiscard]] auto check_timeout(TimePoint const current_time) const noexcept -> bool
    {
        if (!has_pending) {
            return false;
        }
        return current_time >= pending_timeout;
    }

  private:
    statusbar::sg14::inplace_vector<ListenerStreamInfo, MaxStreams> streams_;
    size_t max_streams_;
};

//
// Listener State Machine Actions
//

namespace listener_actions {

void send_connect_tx(ListenerContext<>& ctx, sm::TimePoint event_time);
void send_disconnect_tx(ListenerContext<>& ctx, sm::TimePoint event_time);
void handle_connect_tx_response(ListenerContext<>& ctx, sm::TimePoint event_time);
void handle_disconnect_tx_response(ListenerContext<>& ctx, sm::TimePoint event_time);
void handle_get_rx_state(ListenerContext<>& ctx, sm::TimePoint event_time);
void handle_timeout(ListenerContext<>& ctx, sm::TimePoint event_time);

}  // namespace listener_actions

//
// Listener State Machine Definition Type
//

struct ListenerDef
{
    using Context = ListenerContext<>;
    using State = ListenerState;
    using Event = ListenerEvent;
};

//
// Listener Transition Table
//

/// Build the listener state machine transition table
consteval auto make_listener_table()
{
    using T = sm::Transitions<ListenerDef>;
    using State = ListenerState;
    using Event = ListenerEvent;

    sm::TransitionTable<ListenerDef> t{};

    // From START state - UCT to Waiting
    t.at(State::Start, Event::UCT) = T::transition(State::Waiting);

    // From WAITING state - either dispatch a new command to its action state, or
    // handle an async talker response / timeout for a command already in flight.
    // The listener waits for CONNECT_TX / DISCONNECT_TX responses HERE rather than
    // in a distinct *Resp state: a command that finishes synchronously (an error,
    // or an idempotent already-connected hit) clears pending and returns to Waiting
    // ready for the next command -- so it can never park in a response-wait state
    // with nothing pending, which had no timeout to escape and dropped every
    // subsequent command (the listener wedge).
    t.at(State::Waiting, Event::RcvdConnectRx) = T::transition(State::ConnectTxCmd);
    t.at(State::Waiting, Event::RcvdDisconnectRx) = T::transition(State::DisconnectTxCmd);
    t.at(State::Waiting, Event::RcvdGetRxState) = T::transition(State::GetState);
    t.at(State::Waiting, Event::RcvdConnectTxResp) = T::action<listener_actions::handle_connect_tx_response>(State::Waiting);
    t.at(State::Waiting, Event::RcvdDisconnectTxResp) = T::action<listener_actions::handle_disconnect_tx_response>(State::Waiting);
    t.at(State::Waiting, Event::TxTimeout) = T::action<listener_actions::handle_timeout>(State::Waiting);

    // CONNECT_TX_CMD / DISCONNECT_TX_CMD: run the send action, then return to
    // Waiting. On the async path the action sets has_pending and the talker's
    // response (or a timeout) is handled from Waiting above; on a synchronous path
    // the action has already answered the controller and cleared pending.
    t.at(State::ConnectTxCmd, Event::UCT) = T::action<listener_actions::send_connect_tx>(State::Waiting);
    t.at(State::DisconnectTxCmd, Event::UCT) = T::action<listener_actions::send_disconnect_tx>(State::Waiting);

    // From GET_STATE state - UCT back to WAITING after handling
    t.at(State::GetState, Event::UCT) = T::action<listener_actions::handle_get_rx_state>(State::Waiting);

    return t;
}

/// Static listener transition table
inline constexpr auto listener_table = make_listener_table();

//
// Listener State Machine Type
//

/// ACMP Listener state machine
template <typename Observer = sm::NullObserver>
using AcmpListenerStateMachine = sm::StateMachine<ListenerDef, listener_table, Observer>;

//
// Listener State Machine Helper Functions
//

/// Check if a received command should trigger a listener event (from controller)
/// @param ctx Listener state machine context
/// @param cmd Received ACMP command to check
/// @return The event to trigger, or std::nullopt if the command is not recognized
template <size_t MaxStreams>
[[nodiscard]] inline auto listener_event_for_controller_command(
    ListenerContext<MaxStreams> const& ctx, AcmpCommandResponse const& cmd) noexcept -> std::optional<ListenerEvent>
{
    // Must be a command (even message type)
    if ((cmd.message_type() & 0x01) != 0) {
        return std::nullopt;  // Not a command
    }

    // Must be addressed to this listener
    if (cmd.listener_entity_id != ctx.my_id) {
        return std::nullopt;  // Not for us
    }

    switch (cmd.message_type()) {
        case ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND:
            return ListenerEvent::RcvdConnectRx;
        case ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND:
            return ListenerEvent::RcvdDisconnectRx;
        case ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND:
            return ListenerEvent::RcvdGetRxState;
        default:
            return std::nullopt;  // Unknown command type
    }
}

/// Check if a received response is from talker for pending command
/// @param ctx Listener state machine context
/// @param resp Received ACMP response from talker to check
/// @return The event to trigger, or std::nullopt if the response is not recognized
template <size_t MaxStreams>
[[nodiscard]] inline auto listener_event_for_talker_response(
    ListenerContext<MaxStreams> const& ctx, AcmpCommandResponse const& resp) noexcept -> std::optional<ListenerEvent>
{
    // Must be a response (odd message type)
    if ((resp.message_type() & 0x01) == 0) {
        return std::nullopt;  // Not a response
    }

    // Must have a pending command
    if (!ctx.has_pending) {
        return std::nullopt;  // No pending command
    }

    // Must be addressed to this listener
    if (resp.listener_entity_id != ctx.my_id) {
        return std::nullopt;  // Not for us
    }

    // Must match sequence ID
    if (resp.sequence_id != ctx.pending_command.sequence_id) {
        return std::nullopt;  // Wrong sequence
    }

    switch (resp.message_type()) {
        case ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE:
            return ListenerEvent::RcvdConnectTxResp;
        case ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE:
            return ListenerEvent::RcvdDisconnectTxResp;
        default:
            return std::nullopt;  // Unknown response type
    }
}

/// Check if the listener has a pending command that has timed out
/// @param ctx Listener state machine context
/// @param current_time Current time point to compare against pending timeout
template <size_t MaxStreams>
[[nodiscard]] inline auto listener_has_timeout(ListenerContext<MaxStreams> const& ctx, sm::TimePoint const current_time) noexcept
    -> bool
{
    return ctx.check_timeout(current_time);
}

}  // namespace statusbar::atdecc
