<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->


# ACMP State Machines - IEEE 1722.1-2021

This document describes how to use the ACMP (ATDECC Connection Management Protocol) state machines
for implementing ATDECC Controllers, Talkers, and Listeners per IEEE 1722.1-2021 Clause 8.

## Overview

The ACMP state machines manage audio/video stream connections in AVB networks:

- **Controller**: Initiates connection requests between Talkers and Listeners
- **Talker**: Manages outgoing streams and tracks connected Listeners
- **Listener**: Manages incoming streams and communicates with Talkers

All state machines use the `statusbar::sm` framework with:
- Compile-time transition tables for zero-overhead dispatch
- Constructor-specified capacities for zero-allocation runtime operation
- Time injection for deterministic testing

## Required Includes

```cpp
#include "statusbar/atdecc/atdecc.hpp"  // Includes all ACMP types and state machines
#include "statusbar/sm/sm.hpp"           // State machine framework
#include "statusbar/ieee/ieee.hpp"       // IEEE types (Eui64, Eui48)
```

All ACMP types live in `namespace statusbar::atdecc`; `Eui64` and `Eui48`
live in `namespace statusbar::ieee`.

## Generated State Machine Diagrams

The diagrams and transition tables below are generated from the compiled state machine definitions.

### ACMP Controller

```{graphviz} ../sm/atdecc/acmp_controller_sm.dot
```

```{include} ../sm/atdecc/acmp_controller_sm.md
:start-line: 2
```

### ACMP Talker

```{graphviz} ../sm/atdecc/acmp_talker_sm.dot
```

```{include} ../sm/atdecc/acmp_talker_sm.md
:start-line: 2
```

### ACMP Listener

```{graphviz} ../sm/atdecc/acmp_listener_sm.dot
```

```{include} ../sm/atdecc/acmp_listener_sm.md
:start-line: 2
```

## Controller State Machine

The Controller sends commands to Listeners and Talkers to establish/tear down connections.

### States and Events

| State | Description |
|-------|-------------|
| `Start` | Initial state after construction (UCT to `Waiting`) |
| `Waiting` | Idle, waiting for commands or responses |
| `Command` | Sending a command |
| `Timeout` | Handling a command timeout |
| `Response` | Processing a received response |

| Event | Description |
|-------|-------------|
| `UCT` | Unconditional transition (drives `Start` -> `Waiting` and action-state exits) |
| `DoCommand` | User requested to send a command |
| `DoTerminate` | Shutdown requested (currently ignored in `Waiting`) |
| `RcvdResponse` | Received matching response |
| `RcvdOther` | Received non-matching response (ignored) |
| `Timeout` | Inflight command timed out |

### Minimal Controller Example

```cpp
#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

using namespace statusbar::atdecc;
using namespace statusbar::ieee;

class SimpleController
{
public:
    SimpleController()
        : ctx_(16)  // Max 16 inflight commands (runtime soft cap)
    {
        // Set our entity ID (Eui64 takes 8 individual bytes)
        ctx_.my_id = Eui64{0x00, 0x1C, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x01};

        // Set up transmit callback - called when state machine needs to send
        ctx_.tx_command = [this](AcmpCommandResponse const& cmd) -> bool {
            return send_acmp_packet(cmd);
        };

        // Set up response callback - called when a matching response is processed
        ctx_.process_response = [this](AcmpCommandResponse const& resp) {
            handle_connection_response(resp);
        };

        // Drive the SM from Start to Waiting via the UCT chain
        sm_.handle_event(ctx_, ControllerEvent::UCT);
    }

    // Request a connection between a Talker and Listener
    void connect(Eui64 talker_id, std::uint16_t talker_uid,
                 Eui64 listener_id, std::uint16_t listener_uid)
    {
        // Fill command parameters
        ctx_.command_params.message_type = ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND;
        ctx_.command_params.talker_entity_id = talker_id;
        ctx_.command_params.talker_unique_id = talker_uid;
        ctx_.command_params.listener_entity_id = listener_id;
        ctx_.command_params.listener_unique_id = listener_uid;
        ctx_.command_params.flags = 0;

        // Trigger the state machine
        sm_.handle_event(ctx_, ControllerEvent::DoCommand);
    }

    // Call this when an ACMP packet is received
    void on_packet_received(AcmpCommandResponse const& pdu)
    {
        // Check if this response matches an inflight command
        if (controller_should_handle_response(ctx_, pdu)) {
            ctx_.rcvd_cmd_resp = pdu;
            ctx_.current_inflight_index = ctx_.find_inflight(pdu);
            sm_.handle_event(ctx_, ControllerEvent::RcvdResponse);
        }
    }

    // Call this periodically to check for timeouts
    void poll(std::chrono::steady_clock::time_point now)
    {
        if (controller_has_timeout(ctx_, now)) {
            ctx_.current_inflight_index = ctx_.find_timed_out(now);
            sm_.handle_event(ctx_, ControllerEvent::Timeout, now);
        }
    }

private:
    bool send_acmp_packet(AcmpCommandResponse const& cmd)
    {
        // TODO: Serialize cmd and send via network
        // Return true if sent successfully
        (void)cmd;
        return true;
    }

    void handle_connection_response(AcmpCommandResponse const& resp)
    {
        if (resp.status() == ACMP_STATUS_SUCCESS) {
            // Connection established!
            // resp.stream_id, resp.stream_dest_mac contain stream info
        } else {
            // Handle error - see acmp_status_name() for status descriptions
        }
    }

    ControllerContext ctx_;
    AcmpControllerStateMachine<> sm_;
};
```

## Talker State Machine

The Talker receives commands from Listeners (via Controllers) and manages stream connections.

### States and Events

| State | Description |
|-------|-------------|
| `Start` | Initial state after construction (UCT to `Waiting`) |
| `Waiting` | Idle, waiting for commands |
| `Connect` | Processing CONNECT_TX_COMMAND |
| `Disconnect` | Processing DISCONNECT_TX_COMMAND |
| `GetState` | Processing GET_TX_STATE_COMMAND |
| `GetConnection` | Processing GET_TX_CONNECTION_COMMAND |

| Event | Description |
|-------|-------------|
| `UCT` | Unconditional transition (drives action-state exits back to `Waiting`) |
| `RcvdConnectTx` | Received CONNECT_TX_COMMAND |
| `RcvdDisconnectTx` | Received DISCONNECT_TX_COMMAND |
| `RcvdGetTxState` | Received GET_TX_STATE_COMMAND |
| `RcvdGetTxConnection` | Received GET_TX_CONNECTION_COMMAND |

### Minimal Talker Example

```cpp
#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/ieee/ieee.hpp"

using namespace statusbar::atdecc;
using namespace statusbar::ieee;

class SimpleTalker
{
public:
    SimpleTalker()
        : ctx_(4, 8)  // Runtime caps: 4 streams, 8 listeners per stream
    {
        // Set our entity ID (Eui64 takes 8 individual bytes)
        ctx_.my_id = Eui64{0x00, 0x1C, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x02};

        // Set up transmit callback
        ctx_.tx_response = [this](AcmpCommandResponse const& resp) -> bool {
            return send_acmp_packet(resp);
        };

        // Configure streams - set stream_id and dest_mac for each stream
        configure_streams();

        // Drive the SM from Start to Waiting via the UCT chain
        sm_.handle_event(ctx_, TalkerEvent::UCT);
    }

    // Call this when an ACMP packet is received
    void on_packet_received(AcmpCommandResponse const& pdu)
    {
        // talker_event_for_command returns std::optional<TalkerEvent>
        if (auto event = talker_event_for_command(ctx_, pdu)) {
            ctx_.rcvd_cmd_resp = pdu;
            sm_.handle_event(ctx_, *event);
        }
    }

private:
    void configure_streams()
    {
        // Configure stream 0 (Eui64 takes 8 bytes, Eui48 takes 6 bytes)
        if (auto* stream = ctx_.get_stream(0)) {
            stream->stream_id = Eui64{0x00, 0x1C, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x00};
            stream->stream_dest_mac = Eui48{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01};
            stream->stream_vlan_id = 2;
        }
        // Configure additional streams as needed...
    }

    bool send_acmp_packet(AcmpCommandResponse const& resp)
    {
        // TODO: Serialize resp and send via network
        (void)resp;
        return true;
    }

    TalkerContext<> ctx_;
    AcmpTalkerStateMachine<> sm_;
};
```

## Listener State Machine

The Listener receives commands from Controllers, forwards requests to Talkers,
and manages local stream reception state.

### States and Events

| State | Description |
|-------|-------------|
| `Start` | Initial state after construction (UCT to `Waiting`) |
| `Waiting` | Idle, waiting for commands or responses |
| `ConnectTxCmd` | Sending CONNECT_TX_COMMAND to Talker |
| `ConnectTxResp` | Waiting for CONNECT_TX_RESPONSE |
| `DisconnectTxCmd` | Sending DISCONNECT_TX_COMMAND to Talker |
| `DisconnectTxResp` | Waiting for DISCONNECT_TX_RESPONSE |
| `GetState` | Processing GET_RX_STATE_COMMAND |

| Event | Description |
|-------|-------------|
| `UCT` | Unconditional transition |
| `RcvdConnectRx` | Received CONNECT_RX_COMMAND from Controller |
| `RcvdDisconnectRx` | Received DISCONNECT_RX_COMMAND from Controller |
| `RcvdGetRxState` | Received GET_RX_STATE_COMMAND from Controller |
| `RcvdConnectTxResp` | Received CONNECT_TX_RESPONSE from Talker |
| `RcvdDisconnectTxResp` | Received DISCONNECT_TX_RESPONSE from Talker |
| `TxTimeout` | Timeout waiting for Talker response |

### Minimal Listener Example

```cpp
#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <chrono>
#include <cstdint>

using namespace statusbar::atdecc;
using namespace statusbar::ieee;

class SimpleListener
{
public:
    SimpleListener()
        : ctx_(4)  // Runtime cap: 4 listener streams
    {
        // Set our entity ID (Eui64 takes 8 individual bytes)
        ctx_.my_id = Eui64{0x00, 0x1C, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x03};

        // Callback to send commands to Talkers
        ctx_.tx_command = [this](AcmpCommandResponse const& cmd) -> bool {
            return send_acmp_packet(cmd);
        };

        // Callback to send responses to Controllers
        ctx_.tx_response = [this](AcmpCommandResponse const& resp) -> bool {
            return send_acmp_packet(resp);
        };

        // Drive the SM from Start to Waiting via the UCT chain
        sm_.handle_event(ctx_, ListenerEvent::UCT);
    }

    // Call this when an ACMP packet is received
    void on_packet_received(AcmpCommandResponse const& pdu)
    {
        // Check for commands from Controller (returns std::optional<ListenerEvent>)
        if (auto event = listener_event_for_controller_command(ctx_, pdu)) {
            ctx_.rcvd_cmd_resp = pdu;
            sm_.handle_event(ctx_, *event);
            return;
        }

        // Check for responses from Talker
        if (auto event = listener_event_for_talker_response(ctx_, pdu)) {
            ctx_.rcvd_cmd_resp = pdu;
            sm_.handle_event(ctx_, *event);
        }
    }

    // Call this periodically to check for timeouts
    void poll(std::chrono::steady_clock::time_point now)
    {
        if (listener_has_timeout(ctx_, now)) {
            sm_.handle_event(ctx_, ListenerEvent::TxTimeout, now);
        }
    }

    // Query connection status for a stream
    bool is_connected(std::uint16_t stream_index) const
    {
        auto const* stream = ctx_.get_stream(stream_index);
        return stream != nullptr && stream->connected;
    }

private:
    bool send_acmp_packet(AcmpCommandResponse const& pdu)
    {
        // TODO: Serialize pdu and send via network
        (void)pdu;
        return true;
    }

    ListenerContext<> ctx_;
    AcmpListenerStateMachine<> sm_;
};
```

## Context Configuration

### ControllerContext

```cpp
ControllerContext ctx(max_inflight);  // Runtime soft cap; default 16 (compile-time MAX_INFLIGHT = 16)
```

| Member | Type | Description |
|--------|------|-------------|
| `my_id` | `ieee::Eui64` | Controller's entity ID |
| `command_params` | `AcmpCommandParams` | Parameters for next command |
| `rcvd_cmd_resp` | `AcmpCommandResponse` | Received PDU being processed |
| `current_inflight_index` | `size_t` | Index into the inflight table for the current response/timeout |
| `tx_command` | `inplace_function<bool(AcmpCommandResponse const&), 64>` | Transmit callback |
| `process_response` | `inplace_function<void(AcmpCommandResponse const&), 64>` | Response handler |

Inflight commands are managed by helpers on `ControllerContext`:
`add_inflight`, `remove_inflight`, `get_inflight`, `find_inflight`,
`find_timed_out`, `inflight_count`, `max_inflight`. A "not found" return
from `find_inflight` / `find_timed_out` is any index `>= max_inflight()`.

### TalkerContext

`TalkerContext<MaxStreams, MaxConnectedListeners>` is a class template with
compile-time capacities (defaults `MaxStreams = 32`, `MaxConnectedListeners = 32`).
The constructor sets the runtime soft caps:

```cpp
TalkerContext<> ctx(max_streams, max_connected_listeners);  // Runtime caps; defaults 16, 16
```

| Member | Type | Description |
|--------|------|-------------|
| `my_id` | `ieee::Eui64` | Talker's entity ID |
| `rcvd_cmd_resp` | `AcmpCommandResponse` | Received PDU being processed |
| `response` | `AcmpCommandResponse` | Outgoing response scratch buffer |
| `tx_response` | `inplace_function<bool(AcmpCommandResponse const&), 64>` | Transmit callback |
| `is_authorized` | `inplace_function<bool(ieee::Eui64 const&), 64>` | Optional per-controller authorization check |

Access streams via `ctx.get_stream(unique_id)` which returns
`TalkerStreamInfoDynamic<MaxConnectedListeners>*` (or `nullptr` if out of range).

### ListenerContext

`ListenerContext<MaxStreams>` is a class template with a compile-time
capacity (default `MaxStreams = 32`). The constructor sets the runtime soft cap:

```cpp
ListenerContext<> ctx(max_streams);  // Runtime cap; default 16
```

| Member | Type | Description |
|--------|------|-------------|
| `my_id` | `ieee::Eui64` | Listener's entity ID |
| `rcvd_cmd_resp` | `AcmpCommandResponse` | Received PDU being processed |
| `pending_command` | `AcmpCommandResponse` | Pending command awaiting talker response |
| `has_pending` | `bool` | True when a command is awaiting a talker response |
| `retried` | `bool` | True after the first retry of the pending command |
| `tx_command` | `inplace_function<bool(AcmpCommandResponse const&), 64>` | Send to Talker |
| `tx_response` | `inplace_function<bool(AcmpCommandResponse const&), 64>` | Send to Controller |
| `is_authorized` | `inplace_function<bool(ieee::Eui64 const&), 64>` | Optional per-controller authorization check |

Access streams via `ctx.get_stream(unique_id)` which returns
`ListenerStreamInfo*` (or `nullptr` if out of range).

## Helper Functions

### Controller Helpers

```cpp
// Check if a received PDU should trigger the RcvdResponse event
[[nodiscard]] auto controller_should_handle_response(
    ControllerContext const& ctx,
    AcmpCommandResponse const& resp) noexcept -> bool;

// Check if any inflight command has timed out
[[nodiscard]] auto controller_has_timeout(
    ControllerContext const& ctx,
    sm::TimePoint current_time) noexcept -> bool;
```

### Talker Helpers

```cpp
// Returns the event to dispatch, or std::nullopt if the PDU is not a
// recognised command for this talker.
template <size_t MaxStreams, size_t MaxConnectedListeners>
[[nodiscard]] auto talker_event_for_command(
    TalkerContext<MaxStreams, MaxConnectedListeners> const& ctx,
    AcmpCommandResponse const& cmd) noexcept -> std::optional<TalkerEvent>;
```

### Listener Helpers

```cpp
// Returns the event for a controller command, or std::nullopt if the PDU
// is not a recognised controller command for this listener.
template <size_t MaxStreams>
[[nodiscard]] auto listener_event_for_controller_command(
    ListenerContext<MaxStreams> const& ctx,
    AcmpCommandResponse const& cmd) noexcept -> std::optional<ListenerEvent>;

// Returns the event for a talker response, or std::nullopt if the PDU
// does not match the pending command.
template <size_t MaxStreams>
[[nodiscard]] auto listener_event_for_talker_response(
    ListenerContext<MaxStreams> const& ctx,
    AcmpCommandResponse const& resp) noexcept -> std::optional<ListenerEvent>;

// Check if the pending command has timed out
template <size_t MaxStreams>
[[nodiscard]] auto listener_has_timeout(
    ListenerContext<MaxStreams> const& ctx,
    sm::TimePoint current_time) noexcept -> bool;
```

## Using Observers for Debugging

You can add an observer to trace state machine transitions. The observer is
a callable with the signature
`void(State old_state, Event event, std::string_view action_name, State new_state)`.
Human-readable enum names come from the state machine type's
`state_name()` / `event_name()` static lookups:

```cpp
#include <print>
#include <string_view>

struct LoggingObserver
{
    using Machine = AcmpControllerStateMachine<>;  // for name lookup

    void operator()(ControllerState from,
                    ControllerEvent event,
                    std::string_view action_name,
                    ControllerState to) const
    {
        std::print("Controller: {} --[{}]--> {} (action: {})\n",
                   Machine::state_name(from),
                   Machine::event_name(event),
                   Machine::state_name(to),
                   action_name);
    }
};

AcmpControllerStateMachine<LoggingObserver> sm{LoggingObserver{}};
```

## Time Injection for Testing

`handle_event` takes an optional `event_time` (defaults to `Clock::now()`):

```cpp
using Clock = std::chrono::steady_clock;

// For production - uses current time
sm.handle_event(ctx, ControllerEvent::DoCommand);

// For testing - inject specific time
auto const test_time = Clock::time_point{} + std::chrono::milliseconds{1000};
sm.handle_event(ctx, ControllerEvent::DoCommand, test_time);

// Simulate timeout after 2500 ms
auto const later = Clock::time_point{} + std::chrono::milliseconds{3500};
if (controller_has_timeout(ctx, later)) {
    ctx.current_inflight_index = ctx.find_timed_out(later);
    sm.handle_event(ctx, ControllerEvent::Timeout, later);
}
```

## ACMP Status Codes

| Status | Value | Description |
|--------|-------|-------------|
| `ACMP_STATUS_SUCCESS` | 0 | Command completed successfully |
| `ACMP_STATUS_LISTENER_UNKNOWN_ID` | 1 | Listener unique_id invalid |
| `ACMP_STATUS_TALKER_UNKNOWN_ID` | 2 | Talker unique_id invalid |
| `ACMP_STATUS_TALKER_DEST_MAC_FAIL` | 3 | Talker couldn't allocate dest MAC |
| `ACMP_STATUS_TALKER_NO_STREAM_INDEX` | 4 | Talker at max connections |
| `ACMP_STATUS_TALKER_NO_BANDWIDTH` | 5 | Talker couldn't reserve bandwidth |
| `ACMP_STATUS_TALKER_EXCLUSIVE` | 6 | Talker already exclusively bound |
| `ACMP_STATUS_LISTENER_TALKER_TIMEOUT` | 7 | Listener timed out waiting for Talker |
| `ACMP_STATUS_LISTENER_EXCLUSIVE` | 8 | Listener already exclusively bound |
| `ACMP_STATUS_STATE_UNAVAILABLE` | 9 | Listener/Talker state unavailable |
| `ACMP_STATUS_NOT_CONNECTED` | 10 | Stream not currently connected |
| `ACMP_STATUS_NO_SUCH_CONNECTION` | 11 | GET_TX_CONNECTION index invalid |
| `ACMP_STATUS_COULD_NOT_SEND_MESSAGE` | 12 | Could not send PDU |
| `ACMP_STATUS_TALKER_MISBEHAVING` | 13 | Talker misbehaving |
| `ACMP_STATUS_LISTENER_MISBEHAVING` | 14 | Listener misbehaving |
| `ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED` | 16 | Controller not authorized |
| `ACMP_STATUS_INCOMPATIBLE_REQUEST` | 17 | Incompatible request |
| `ACMP_STATUS_NOT_SUPPORTED` | 31 | Command not supported |

See `atdecc_acmp_types.hpp` for the full list. Use `acmp_status_name(status)`
to get a human-readable string.

## ACMP Message Types

| Command | Response |
|---------|----------|
| `ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND` (0) | `ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE` (1) |
| `ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND` (2) | `ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE` (3) |
| `ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND` (4) | `ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE` (5) |
| `ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND` (6) | `ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE` (7) |
| `ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND` (8) | `ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE` (9) |
| `ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND` (10) | `ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE` (11) |
| `ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND` (12) | `ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE` (13) |

## Timeouts (per IEEE 1722.1-2021)

Compile-time constants in `atdecc_acmp_types.hpp`
(`ACMP_TIMEOUT_*_MS`). `acmp_timeout_for_message_type(message_type)`
returns a `std::chrono::milliseconds` for runtime lookup.

| Command | Timeout |
|---------|---------|
| `CONNECT_TX_COMMAND` | 2000 ms |
| `DISCONNECT_TX_COMMAND` | 200 ms |
| `GET_TX_STATE_COMMAND` | 200 ms |
| `CONNECT_RX_COMMAND` | 4500 ms |
| `DISCONNECT_RX_COMMAND` | 500 ms |
| `GET_RX_STATE_COMMAND` | 200 ms |
| `GET_TX_CONNECTION_COMMAND` | 200 ms |

## Complete Integration Example

Here's how all three state machines work together:

```
Controller                    Listener                      Talker
    |                            |                            |
    |-- CONNECT_RX_COMMAND ----->|                            |
    |                            |-- CONNECT_TX_COMMAND ----->|
    |                            |                            |
    |                            |<-- CONNECT_TX_RESPONSE ----|
    |<-- CONNECT_RX_RESPONSE ----|                            |
    |                            |                            |
```

1. Controller sends `CONNECT_RX_COMMAND` to Listener
2. Listener forwards as `CONNECT_TX_COMMAND` to Talker
3. Talker adds Listener to stream's connected list, responds
4. Listener updates local state, forwards response to Controller
5. Controller receives confirmation, connection established

## Command-Line Tool

A simple ACMP controller tool is provided for testing connections. The
CMake target is `acmp_controller_tool`; the installed binary is named
`statusbar-acmp-controller`.

```bash
Usage: statusbar-acmp-controller [options]

Options:
  --interface=DEVICE          Network interface (e.g., en0, eth0)
  --action=CHOICE             Action to perform: CONNECT or DISCONNECT (default: CONNECT)
  --talker-entity-id=VALUE    Talker Entity ID (EUI-64)
  --talker-uid=INTEGER        Talker Unique ID (0-65535, default: 0)
  --listener-entity-id=VALUE  Listener Entity ID (EUI-64)
  --listener-uid=INTEGER      Listener Unique ID (0-65535, default: 0)
  --help                      Show help message
  --config-load=FILE          Load configuration from TOML file
  --config-save=FILE          Save configuration to TOML file
```

Example:
```bash
# Connect a talker to a listener
sudo ./build/statusbar/atdecc/acmp_controller_tool --interface=eth0 --action=CONNECT \
    --talker-entity-id=00:11:22:00:00:00:00:02 --talker-uid=0 \
    --listener-entity-id=00:11:22:00:00:00:00:03 --listener-uid=0

# Disconnect
sudo ./build/statusbar/atdecc/acmp_controller_tool --interface=eth0 --action=DISCONNECT \
    --talker-entity-id=00:11:22:00:00:00:00:02 --talker-uid=0 \
    --listener-entity-id=00:11:22:00:00:00:00:03 --listener-uid=0

# Using a configuration file
sudo ./build/statusbar/atdecc/acmp_controller_tool --config-load=acmp_config.toml
```

Configuration file example:
```toml
interface = "eth0"
action = "CONNECT"
talker-entity-id = "00:11:22:00:00:00:00:02"
talker-uid = 0
listener-entity-id = "00:11:22:00:00:00:00:03"
listener-uid = 0
```

The tool:
- Uses EtherType 0x22F0 (AVTP/ATDECC)
- Sends to the ATDECC multicast MAC (91:E0:F0:01:00:00)
- Logs all state machine transitions and ACMP packets
- Runs until Ctrl-C

The hardcoded controller entity ID is `70:B3:D5:ED:C0:00:00:00` (J. D. Koftinoff Software, Ltd. OUI-36).

**Note**: Raw Ethernet access typically requires root/sudo privileges or appropriate network capabilities.
