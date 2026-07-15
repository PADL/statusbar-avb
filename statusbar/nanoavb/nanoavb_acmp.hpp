#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB ACMP - Talker/Listener ACMP wrappers
/// Provides simplified wrappers around statusbar::atdecc ACMP state machines

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_base.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <source_location>
#include <span>
#include <utility>

namespace statusbar::nanoavb {

using ieee::Eui48;
using ieee::Eui64;
using statusbar::failure;
using statusbar::Status;
using statusbar::success;
using tsn::StreamId;
using namespace atdecc;

//
// ACMP Callback Interfaces
//
/// Callbacks for ACMP talker operations
struct AcmpTalkerCallbacks
{
    /// Called when the handler needs to send an ACMP response
    statusbar::sg14::inplace_function<bool(AcmpCommandResponse const&), 64> tx_response;

    /// Called when a stream connection is established
    statusbar::sg14::inplace_function<void(uint16_t stream_index, Eui64 listener_entity_id, uint16_t listener_unique_id), 64>
        on_connect;

    /// Called when a stream connection is removed
    statusbar::sg14::inplace_function<void(uint16_t stream_index, Eui64 listener_entity_id, uint16_t listener_unique_id), 64>
        on_disconnect;

    /// Called to register a talker stream with SRP (on first listener connect)
    statusbar::sg14::inplace_function<void(uint16_t stream_index, Eui64 const& stream_id, Eui48 dest_mac, uint16_t vlan_id), 64>
        srp_register_stream;

    /// Called to deregister a talker stream from SRP (on last listener disconnect)
    statusbar::sg14::inplace_function<void(uint16_t stream_index), 64> srp_deregister_stream;
};

/// Callbacks for ACMP listener operations
struct AcmpListenerCallbacks
{
    /// Called when the handler needs to send an ACMP command to a talker
    statusbar::sg14::inplace_function<bool(AcmpCommandResponse const&), 64> tx_command;

    /// Called when the handler needs to send an ACMP response to the controller
    statusbar::sg14::inplace_function<bool(AcmpCommandResponse const&), 64> tx_response;

    /// Called when a stream is connected
    statusbar::sg14::inplace_function<void(uint16_t stream_index, Eui64 const& stream_id, Eui48 dest_mac), 64> on_connect;

    /// Called when a stream is disconnected
    statusbar::sg14::inplace_function<void(uint16_t stream_index), 64> on_disconnect;

    /// Called to attach a listener to an SRP stream reservation
    statusbar::sg14::inplace_function<void(uint16_t stream_index, Eui64 const& stream_id), 64> srp_attach_listener;

    /// Called to detach a listener from an SRP stream reservation
    statusbar::sg14::inplace_function<void(uint16_t stream_index), 64> srp_detach_listener;
};

//
// NanoAVB ACMP Talker Handler
//
/// NanoAvbAcmpTalker - simplified wrapper around ACMP talker state machine
/// Manages talker streams and responds to controller commands
class NanoAvbAcmpTalker
{
  public:
    using TimePoint = sm::TimePoint;

    /// Construct with entity ID and optional max streams
    /// @param entity_id The entity ID for this talker
    /// @param callbacks Callback interface for sending responses and notifications
    /// @param max_streams Maximum number of talker streams
    /// @param max_listeners_per_stream Maximum number of listeners per stream
    explicit NanoAvbAcmpTalker(
        Eui64 entity_id, AcmpTalkerCallbacks callbacks = {}, size_t max_streams = 16, size_t max_listeners_per_stream = 16);

    /// Activate the state machine (UCT from Start to Waiting)
    /// @param event_time Current time
    void start(TimePoint event_time = {}) { sm_.handle_event(ctx_, TalkerEvent::UCT, event_time); }

    /// Get the entity ID
    [[nodiscard]] auto entity_id() const noexcept -> Eui64 { return ctx_.my_id; }

    /// Set the entity ID
    /// @param entity_id The new entity ID
    void set_entity_id(Eui64 entity_id) noexcept { ctx_.my_id = entity_id; }

    /// Set or update callbacks after construction
    /// This allows wiring up network handlers after both components are created
    /// @param callbacks The new callback interface
    void set_callbacks(AcmpTalkerCallbacks callbacks);

    /// Set just the connection callbacks without disturbing the others
    /// (set_callbacks replaces the whole struct, which would clear tx_response
    /// already wired by the net layer). Lets the application observe ACMP
    /// connect/disconnect after tx_response has been set up.
    void set_connection_callbacks(
        statusbar::sg14::inplace_function<void(uint16_t, Eui64, uint16_t), 64> on_connect,
        statusbar::sg14::inplace_function<void(uint16_t, Eui64, uint16_t), 64> on_disconnect)
    {
        callbacks_.on_connect = std::move(on_connect);
        callbacks_.on_disconnect = std::move(on_disconnect);
    }

    /// Get the maximum number of streams
    [[nodiscard]] auto max_streams() const noexcept -> size_t { return ctx_.max_streams(); }

    // Stream Configuration

    /// Configure a talker stream
    /// @param stream_index The stream index (0 to max_streams-1)
    /// @param stream_id The stream ID to advertise (as raw Eui64)
    /// @param dest_mac The destination MAC address
    /// @param vlan_id The VLAN ID
    [[nodiscard]] auto configure_stream(
        uint16_t stream_index, Eui64 stream_id, Eui48 dest_mac, uint16_t vlan_id = srp::DEFAULT_SR_CLASS_A_VID) -> Status;

    /// Get stream info
    /// @param stream_index The stream index to query
    [[nodiscard]] auto get_stream(uint16_t const stream_index) const noexcept -> TalkerStreamInfoDynamic<> const*
    {
        return ctx_.get_stream(stream_index);
    }

    /// Get the number of listeners connected to a stream
    /// @param stream_index The stream index to query
    [[nodiscard]] auto connection_count(uint16_t const stream_index) const noexcept -> size_t
    {
        auto const* stream = ctx_.get_stream(stream_index);
        return stream != nullptr ? stream->connection_count() : 0;
    }

    // ACMP Command Processing

    /// Process a received ACMP command
    /// @param cmd The received ACMP command
    /// @param event_time Current time for timeout tracking
    /// @return true if the command was handled
    auto receive_command(AcmpCommandResponse const& cmd, TimePoint event_time) -> bool;

    /// Get the current state machine state
    [[nodiscard]] auto current_state() const noexcept -> TalkerState { return sm_.current_state(); }

  private:
    void wire_talker_tx_response();

    AcmpTalkerCallbacks callbacks_;
    TalkerContext<> ctx_;
    AcmpTalkerStateMachine<> sm_;
};

//
// NanoAVB ACMP Listener Handler
//
/// NanoAvbAcmpListener - simplified wrapper around ACMP listener state machine
/// Manages listener streams, connects to talkers, responds to controller commands
class NanoAvbAcmpListener
{
  public:
    using TimePoint = sm::TimePoint;

    /// Construct with entity ID and optional max streams
    /// @param entity_id The entity ID for this listener
    /// @param callbacks Callback interface for sending commands/responses and notifications
    /// @param max_streams Maximum number of listener streams
    explicit NanoAvbAcmpListener(Eui64 entity_id, AcmpListenerCallbacks callbacks = {}, size_t max_streams = 16);

    /// Activate the state machine (UCT from Start to Waiting)
    /// @param event_time Current time
    void start(TimePoint event_time = {}) { sm_.handle_event(ctx_, ListenerEvent::UCT, event_time); }

    /// Get the entity ID
    [[nodiscard]] auto entity_id() const noexcept -> Eui64 { return ctx_.my_id; }

    /// Set the entity ID
    /// @param entity_id The new entity ID
    void set_entity_id(Eui64 entity_id) noexcept { ctx_.my_id = entity_id; }

    /// Set or update callbacks after construction
    /// This allows wiring up network handlers after both components are created
    /// @param callbacks The new callback interface
    void set_callbacks(AcmpListenerCallbacks callbacks);

    /// Set just the connection callbacks without disturbing the others
    /// (set_callbacks replaces the whole struct, which would clear the tx_command/
    /// tx_response already wired by the net layer). Lets the application react to a
    /// successful listener connect/disconnect -- e.g. issue the MSRP Listener Ready
    /// reservation that makes the talker actually start streaming. The wired
    /// tx_response lambda reads callbacks_ live, so updating it post-wiring works.
    void set_connection_callbacks(
        statusbar::sg14::inplace_function<void(uint16_t, Eui64 const&, Eui48), 64> on_connect,
        statusbar::sg14::inplace_function<void(uint16_t), 64> on_disconnect)
    {
        callbacks_.on_connect = std::move(on_connect);
        callbacks_.on_disconnect = std::move(on_disconnect);
    }

    /// Get the maximum number of streams
    [[nodiscard]] auto max_streams() const noexcept -> size_t { return ctx_.max_streams(); }

    // Stream State Access

    /// Get stream info
    /// @param stream_index The stream index to query
    [[nodiscard]] auto get_stream(uint16_t const stream_index) const noexcept -> ListenerStreamInfo const*
    {
        return ctx_.get_stream(stream_index);
    }

    /// Check if a stream is connected
    /// @param stream_index The stream index to query
    [[nodiscard]] auto is_connected(uint16_t const stream_index) const noexcept -> bool
    {
        auto const* stream = ctx_.get_stream(stream_index);
        return stream != nullptr && stream->connected;
    }

    /// Check if there is a pending command (waiting for talker response)
    [[nodiscard]] auto has_pending() const noexcept -> bool { return ctx_.has_pending; }

    // Fast connect (kit phase 5b)

    /// A remembered (sink -> talker) binding the listener keeps trying to
    /// fast-connect while disconnected.
    struct FastConnectGoal
    {
        uint16_t listener_unique_id{0};
        Eui64 talker_entity_id{};
        uint16_t talker_unique_id{0};
    };

    /// How often tick() re-attempts a disconnected fast-connect goal.
    static constexpr auto FAST_CONNECT_RETRY = std::chrono::seconds{2};
    static constexpr size_t MAX_FAST_CONNECT_GOALS = 16;

    /// IEEE 1722.1 Clause 8.2.2.1.1 fast connect: the listener originates a
    /// connection to a remembered talker with itself as the controller (no
    /// external controller involved). Feeds the normal CONNECT_RX path, so
    /// the CONNECT_TX handshake, SRP attach and on_connect all behave as if
    /// a controller had asked. Returns false when a command is already
    /// pending, the sink is connected, or the index is out of range.
    auto fast_connect(uint16_t listener_unique_id, Eui64 const& talker_id, uint16_t talker_unique_id, TimePoint now) -> bool;

    /// Remember a binding and keep fast-connecting until it succeeds —
    /// tick() retries every FAST_CONNECT_RETRY while the sink is
    /// disconnected (talker rebooted, we rebooted, response lost...). A
    /// controller DISCONNECT_RX for the sink clears the goal (an operator
    /// tore the connection down on purpose); a talker departure does NOT
    /// (that is exactly the case fast connect exists to heal).
    void set_fast_connect_goal(uint16_t listener_unique_id, Eui64 const& talker_id, uint16_t talker_unique_id);

    /// Forget the goal for @p listener_unique_id (no-op if absent).
    void clear_fast_connect_goal(uint16_t listener_unique_id);

    /// The remembered goals (for persistence).
    [[nodiscard]] auto fast_connect_goals() const noexcept -> std::span<FastConnectGoal const>
    {
        return {fast_connect_goals_.data(), fast_connect_goals_.size()};
    }

    /// Opt-in: automatically record every successful connect (controller-
    /// or fast-connect-made) as a fast-connect goal, so the sink keeps
    /// re-connecting its talker after either side restarts. Combine with
    /// set_on_goals_changed to persist the goals across our own restarts.
    void enable_sticky_bindings() noexcept { sticky_bindings_ = true; }

    /// Called after the goal table actually changes (recorded, retargeted
    /// or cleared) — the persistence hook. Not called for no-op updates.
    void set_on_goals_changed(statusbar::sg14::inplace_function<void(), 64> fn) { on_goals_changed_ = std::move(fn); }

    /// Tear down any sink connected to a departed talker (ADP ENTITY_DEPARTING or a
    /// discovery ageout). Clears each matching sink's connection state and fires the
    /// on_disconnect callback so SRP/MSRP is released. Returns the number torn down.
    /// Without this a sink stays connected forever after its talker vanishes, since
    /// ACMP only clears a sink on an explicit controller DISCONNECT_RX.
    auto on_talker_departed(Eui64 const& talker_id) -> size_t;

    // ACMP Command/Response Processing

    /// Process a received ACMP command from a controller
    /// @param cmd The received ACMP command
    /// @param event_time Current time for timeout tracking
    /// @return true if the command was handled
    auto receive_controller_command(AcmpCommandResponse const& cmd, TimePoint event_time) -> bool;

    /// Process a received ACMP response from a talker
    /// @param resp The received ACMP response
    /// @param event_time Current time for timeout tracking
    /// @return true if the response was handled
    auto receive_talker_response(AcmpCommandResponse const& resp, TimePoint event_time) -> bool;

    /// Check for and process timeouts
    /// @param current_time Current time
    /// @return true if a timeout was processed
    auto check_timeout(TimePoint current_time) -> bool;

    /// Periodic tick - call regularly to check for timeouts and to retry
    /// disconnected fast-connect goals.
    /// @param current_time Current time for timeout processing
    void tick(TimePoint current_time)
    {
        check_timeout(current_time);
        fast_connect_tick(current_time);
    }

    /// Get the current state machine state
    [[nodiscard]] auto current_state() const noexcept -> ListenerState { return sm_.current_state(); }

  private:
    void wire_listener_callbacks();
    void fast_connect_tick(TimePoint now);

    AcmpListenerCallbacks callbacks_;
    ListenerContext<> ctx_;
    AcmpListenerStateMachine<> sm_;
    statusbar::sg14::inplace_vector<FastConnectGoal, MAX_FAST_CONNECT_GOALS> fast_connect_goals_{};
    statusbar::sg14::inplace_function<void(), 64> on_goals_changed_{};
    TimePoint next_fast_connect_attempt_{};
    uint16_t fast_connect_sequence_{0};
    bool sticky_bindings_{false};
};

//
// NanoAVB ACMP Controller Handler
//

/// Callbacks for ACMP controller operations
struct AcmpControllerCallbacks
{
    /// Called to send an ACMP command on the wire (multicast)
    statusbar::sg14::inplace_function<bool(AcmpCommandResponse const&), 64> tx_command;

    /// Called when a response is received for an inflight command
    statusbar::sg14::inplace_function<void(AcmpCommandResponse const& response), 64> on_response;

    /// Called when an inflight command times out after retry
    statusbar::sg14::inplace_function<void(AcmpCommandResponse const& original_command), 64> on_timeout;
};

/// NanoAvbAcmpController - simplified wrapper around ACMP controller state machine
/// Sends ACMP commands to listeners and tracks responses
class NanoAvbAcmpController
{
  public:
    using TimePoint = sm::TimePoint;

    /// Construct with entity ID and optional max inflight commands
    explicit NanoAvbAcmpController(Eui64 entity_id, AcmpControllerCallbacks callbacks = {}, size_t max_inflight = 16);

    /// Activate the state machine
    void start(TimePoint event_time = {}) { sm_.handle_event(ctx_, ControllerEvent::UCT, event_time); }

    /// Get the entity ID
    [[nodiscard]] auto entity_id() const noexcept -> Eui64 { return ctx_.my_id; }

    /// Set the entity ID
    void set_entity_id(Eui64 entity_id) noexcept { ctx_.my_id = entity_id; }

    /// Set or update callbacks
    void set_callbacks(AcmpControllerCallbacks callbacks);

    // High-level command API

    /// Connect a talker to a listener
    auto connect(Eui64 talker_id, uint16_t talker_uid, Eui64 listener_id, uint16_t listener_uid) -> bool;

    /// Disconnect a talker from a listener
    auto disconnect(Eui64 talker_id, uint16_t talker_uid, Eui64 listener_id, uint16_t listener_uid) -> bool;

    /// Send a CONNECT_TX_COMMAND directly to the talker's ACMP state machine.
    /// Unlike connect() (which sends CONNECT_RX to the listener and lets the
    /// listener relay a TX command), this drives the talker side directly --
    /// needed by the supervise/self-heal path to re-establish a talker.
    auto connect_tx(Eui64 talker_id, uint16_t talker_uid, Eui64 listener_id, uint16_t listener_uid) -> bool;

    /// Send a DISCONNECT_TX_COMMAND directly to the talker's ACMP state machine.
    /// Clears the talker's per-listener connection (and its SRP registration)
    /// without going through the listener -- the clean half of a stale-stream
    /// reset.
    auto disconnect_tx(Eui64 talker_id, uint16_t talker_uid, Eui64 listener_id, uint16_t listener_uid) -> bool;

    /// Query listener connection state
    auto get_rx_state(Eui64 listener_id, uint16_t listener_uid) -> bool;

    /// Query talker connection state
    auto get_tx_state(Eui64 talker_id, uint16_t talker_uid) -> bool;

    /// Query a specific talker connection by index
    auto get_tx_connection(Eui64 talker_id, uint16_t talker_uid, uint16_t index) -> bool;

    // Response/timeout processing

    /// Process a received ACMP response from the network
    auto receive_response(AcmpCommandResponse const& resp, TimePoint event_time) -> bool;

    /// Check for timed-out inflight commands
    void tick(TimePoint now);

    // Status

    /// Number of currently inflight commands
    [[nodiscard]] auto inflight_count() const noexcept -> size_t { return ctx_.inflight_count(); }

    /// Maximum inflight commands
    [[nodiscard]] auto max_inflight() const noexcept -> size_t { return ctx_.max_inflight(); }

    /// Current state machine state
    [[nodiscard]] auto current_state() const noexcept -> ControllerState { return sm_.current_state(); }

  private:
    void wire_callbacks();
    auto send_command(
        uint8_t message_type,
        Eui64 talker_id,
        uint16_t talker_uid,
        Eui64 listener_id,
        uint16_t listener_uid,
        uint16_t connection_count = 0) -> bool;

    AcmpControllerCallbacks callbacks_;
    ControllerContext ctx_;
    AcmpControllerStateMachine<> sm_;
};

}  // namespace statusbar::nanoavb
