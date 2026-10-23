#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACMP Talker State Machine - IEEE 1722.1-2021 Clause 8.2.4
/// Implements the ATDECC Talker state machine for managing listener connections

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
// Talker State Machine Definition (Figure 8-3)
//

/// Talker state machine states
enum class TalkerState : uint8_t
{
    Start,          // Initial state after construction
    Waiting,        // Idle state, waiting for commands
    Connect,        // Processing a CONNECT_TX_COMMAND
    Disconnect,     // Processing a DISCONNECT_TX_COMMAND
    GetState,       // Processing a GET_TX_STATE_COMMAND
    GetConnection,  // Processing a GET_TX_CONNECTION_COMMAND
    Count
};

/// Talker state machine events
enum class TalkerEvent : uint8_t
{
    UCT,                  // Unconditional transition
    RcvdConnectTx,        // Received CONNECT_TX_COMMAND
    RcvdDisconnectTx,     // Received DISCONNECT_TX_COMMAND
    RcvdGetTxState,       // Received GET_TX_STATE_COMMAND
    RcvdGetTxConnection,  // Received GET_TX_CONNECTION_COMMAND
    Count
};

/// Talker state machine context - holds all state and callbacks
/// @tparam MaxStreams Compile-time capacity for talker streams
/// @tparam MaxConnectedListeners Compile-time capacity for connected listeners per stream
template <size_t MaxStreams = 32, size_t MaxConnectedListeners = 32>
struct TalkerContext
{
    /// The compile-time stream capacity (the runtime max_streams is at most this).
    static constexpr size_t MAX_STREAMS = MaxStreams;

    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    /// Construct with specified max streams and max connected listeners per stream
    /// Reserves capacity for zero-allocation runtime operation
    /// @param max_streams Maximum number of talker streams
    /// @param max_connected_listeners Maximum connected listeners per stream
    explicit TalkerContext(size_t max_streams = 16, size_t max_connected_listeners = 16)
        : max_streams_{max_streams}
        , max_connected_listeners_{max_connected_listeners}
    {
        streams_.reserve(max_streams);
        for (size_t i = 0; i < max_streams; ++i) {
            streams_.emplace_back(max_connected_listeners);
        }
    }

    // Entity ID of this talker
    ieee::Eui64 my_id{};

    // Received command being processed
    AcmpCommandResponse rcvd_cmd_resp{};

    // Response to send
    AcmpCommandResponse response{};

    // Callbacks - must be set before use
    statusbar::sg14::inplace_function<bool(AcmpCommandResponse const&), 64> tx_response{};

    /// Authorization callback: returns true if the given controller is allowed
    /// to issue ACMP commands. If not set, all controllers are authorized.
    statusbar::sg14::inplace_function<bool(ieee::Eui64 const& controller_entity_id), 64> is_authorized;

    // Stream management

    /// Get max streams capacity
    [[nodiscard]] auto max_streams() const noexcept { return max_streams_; }

    /// Get stream info for a unique_id, or nullptr if out of range
    /// @param unique_id Stream unique identifier
    [[nodiscard]] auto get_stream(uint16_t const unique_id) noexcept -> TalkerStreamInfoDynamic<MaxConnectedListeners>*
    {
        if (unique_id < max_streams_) {
            return &streams_[unique_id];
        }
        return nullptr;
    }

    /// @param unique_id Stream unique identifier
    [[nodiscard]] auto get_stream(uint16_t const unique_id) const noexcept -> TalkerStreamInfoDynamic<MaxConnectedListeners> const*
    {
        if (unique_id < max_streams_) {
            return &streams_[unique_id];
        }
        return nullptr;
    }

    /// Check if a listener is connected to a stream
    /// @param unique_id Stream unique identifier
    /// @param listener_entity_id Entity ID of the listener to check
    /// @param listener_unique_id Unique ID of the listener stream to check
    [[nodiscard]] auto is_listener_connected(
        uint16_t const unique_id, ieee::Eui64 const listener_entity_id, uint16_t const listener_unique_id) const noexcept -> bool
    {
        auto const* stream = get_stream(unique_id);
        if (stream == nullptr) {
            return false;
        }
        return stream->contains_listener({.listener_entity_id = listener_entity_id, .listener_unique_id = listener_unique_id});
    }

  private:
    statusbar::sg14::inplace_vector<TalkerStreamInfoDynamic<MaxConnectedListeners>, MaxStreams> streams_;
    size_t max_streams_;
    size_t max_connected_listeners_;
};

//
// Talker State Machine Actions
//

namespace talker_actions {

void handle_connect_tx(TalkerContext<>& ctx, sm::TimePoint event_time);
void handle_disconnect_tx(TalkerContext<>& ctx, sm::TimePoint event_time);
void handle_get_tx_state(TalkerContext<>& ctx, sm::TimePoint event_time);
void handle_get_tx_connection(TalkerContext<>& ctx, sm::TimePoint event_time);

}  // namespace talker_actions

//
// Talker State Machine Definition Type
//

struct TalkerDef
{
    using Context = TalkerContext<>;
    using State = TalkerState;
    using Event = TalkerEvent;
};

//
// Talker Transition Table
//

/// Build the talker state machine transition table
consteval auto make_talker_table()
{
    using T = sm::Transitions<TalkerDef>;
    using State = TalkerState;
    using Event = TalkerEvent;

    sm::TransitionTable<TalkerDef> t{};

    // From START state - UCT to Waiting
    t.at(State::Start, Event::UCT) = T::transition(State::Waiting);

    // From WAITING state - transition to appropriate action state
    t.at(State::Waiting, Event::RcvdConnectTx) = T::transition(State::Connect);
    t.at(State::Waiting, Event::RcvdDisconnectTx) = T::transition(State::Disconnect);
    t.at(State::Waiting, Event::RcvdGetTxState) = T::transition(State::GetState);
    t.at(State::Waiting, Event::RcvdGetTxConnection) = T::transition(State::GetConnection);

    // From CONNECT state - UCT back to WAITING after handling
    t.at(State::Connect, Event::UCT) = T::action<talker_actions::handle_connect_tx>(State::Waiting);

    // From DISCONNECT state - UCT back to WAITING after handling
    t.at(State::Disconnect, Event::UCT) = T::action<talker_actions::handle_disconnect_tx>(State::Waiting);

    // From GET_STATE state - UCT back to WAITING after handling
    t.at(State::GetState, Event::UCT) = T::action<talker_actions::handle_get_tx_state>(State::Waiting);

    // From GET_CONNECTION state - UCT back to WAITING after handling
    t.at(State::GetConnection, Event::UCT) = T::action<talker_actions::handle_get_tx_connection>(State::Waiting);

    return t;
}

/// Static talker transition table
inline constexpr auto talker_table = make_talker_table();

//
// Talker State Machine Type
//

/// ACMP Talker state machine
template <typename Observer = sm::NullObserver>
using AcmpTalkerStateMachine = sm::StateMachine<TalkerDef, talker_table, Observer>;

//
// Talker State Machine Helper Functions
//

/// Check if a received command should trigger a talker event
/// @param ctx Talker state machine context
/// @param cmd Received ACMP command to check
/// @return The event to trigger, or std::nullopt if the command is not recognized
template <size_t MaxStreams, size_t MaxConnectedListeners>
[[nodiscard]] inline auto talker_event_for_command(
    TalkerContext<MaxStreams, MaxConnectedListeners> const& ctx, AcmpCommandResponse const& cmd) noexcept
    -> std::optional<TalkerEvent>
{
    // Must be a command (even message type)
    if ((cmd.message_type() & 0x01) != 0) {
        return std::nullopt;  // Not a command
    }

    // Must be addressed to this talker
    if (cmd.talker_entity_id != ctx.my_id) {
        return std::nullopt;  // Not for us
    }

    switch (cmd.message_type()) {
        case ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND:
            return TalkerEvent::RcvdConnectTx;
        case ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND:
            return TalkerEvent::RcvdDisconnectTx;
        case ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND:
            return TalkerEvent::RcvdGetTxState;
        case ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND:
            return TalkerEvent::RcvdGetTxConnection;
        default:
            return std::nullopt;  // Unknown command type
    }
}

}  // namespace statusbar::atdecc
