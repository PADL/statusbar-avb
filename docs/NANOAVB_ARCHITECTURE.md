<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->


# NanoAVB Architecture

This document describes the architecture of the NanoAVB module, a minimal AVB/ATDECC implementation for audio-over-Ethernet applications.

## Overview

NanoAVB provides a lightweight implementation of AVB (Audio Video Bridging) and ATDECC (AV Transport Protocol for Device Enumeration, Connection Management, and Control) protocols. The module uses a state-machine-driven architecture to coordinate protocol lifecycle and stream engine control.

## Design Goals

- **Minimal footprint**: Small memory and CPU overhead suitable for embedded systems
- **Single-threaded reactor pattern**: All protocol handling runs in one thread for simplicity
- **State machine coordination**: Clear, testable state transitions for protocol lifecycle
- **Modular components**: Protocol handlers can be used independently or composed together

## Module Structure

```
statusbar.nanoavb
├── :base                   # Error codes, common types
├── :entity_model           # AVDECC Entity Model (AEM) descriptors
├── :entity                 # AEM command handler
├── :adp                    # ADP (Entity Discovery Protocol) advertiser
├── :acmp                   # ACMP (Audio Connection Management Protocol) talker/listener
├── :srp                    # MVRP/MSRP (Stream Reservation Protocol) handlers
├── :components             # Network handlers and component container
└── State Machines:
    ├── :supervisor_sm      # Top-level protocol coordinator
    ├── :gptp_sm            # gPTP synchronization status
    ├── :mvrp_sm            # VLAN registration lifecycle
    ├── :adp_adv_sm         # ADP advertising state
    ├── :acmp_talker_sm     # ACMP talker connection state
    ├── :acmp_listener_sm   # ACMP listener connection state
    ├── :msrp_talker_sm     # MSRP talker reservation state
    ├── :msrp_listener_sm   # MSRP listener reservation state
    ├── :talker_engine_sm   # Audio TX pipeline control
    └── :listener_engine_sm # Audio RX pipeline control
```

## State Machine Hierarchy

The supervisor state machine coordinates all other state machines:

```
                    ┌─────────────────┐
                    │  supervisor_sm  │
                    │  (coordinator)  │
                    └────────┬────────┘
                             │
         ┌───────────────────┼───────────────────┐
         │                   │                   │
    ┌────▼────┐        ┌─────▼────┐        ┌─────▼─────┐
    │ gptp_sm │        │ mvrp_sm  │        │ adp_adv_sm│
    │ (sync)  │        │ (VLAN)   │        │ (announce)│
    └─────────┘        └──────────┘        └───────────┘
                             │
              ┌──────────────┼──────────────┐
              │              │              │
    ┌─────────▼─────────┐ ┌──▼───────────┐ │
    │ msrp_talker_sm    │ │msrp_listener │ │
    │ (reservation)     │ │_sm (reserv.) │ │
    └─────────┬─────────┘ └──────┬───────┘ │
              │                  │         │
    ┌─────────▼─────────┐ ┌──────▼───────┐ │
    │ talker_engine_sm  │ │listener_eng. │ │
    │ (audio TX)        │ │_sm (audio RX)│ │
    └───────────────────┘ └──────────────┘ │
                                           │
                    ┌──────────────────────┼─────┐
                    │                      │     │
              ┌─────▼────────┐  ┌──────────▼───┐ │
              │acmp_talker_sm│  │acmp_listener │ │
              │(connections) │  │_sm (connect.)│ │
              └──────────────┘  └──────────────┘ │
                                                 │
```

## Supervisor State Machine

The supervisor coordinates the overall protocol lifecycle:

```
    ┌───────┐
    │ Start │
    └───┬───┘
        │ UCT (init_iface)
        ▼
    ┌───────┐◄──────────────────────────────────────────┐
    │ Down  │                                           │
    └───┬───┘                                           │
        │ LinkUp (start_protocols)                      │
        ▼                                               │
    ┌───────┐──────Timeout─────► Down (timeout_gptp)    │
    │ Init  │                                           │
    └───┬───┘──────LinkDown────►────────────────────────┤
        │ GptpLocked                                    │
        │ (enter_wait_vlan)                             │
        ▼                                               │
    ┌─────────────┐──Timeout──► Degraded (timeout_vlan) │
    │ WaitVlanBase│                                     │
    └───┬─────────┘──LinkDown──►────────────────────────┤
        │ VlanBaseReady                                 │
        │ (enter_ready)                                 │
        ▼                                               │
    ┌───────┐                                           │
    │ Ready │──────GptpLost────► Degraded               │
    └───────┘──────LinkDown────►────────────────────────┘
        ▲
        │
    ┌───────────┐──LinkDown────► Down
    │ Degraded  │
    └───────────┘──GptpLocked──► WaitVlanBase
```

### States

| State | Description |
|-------|-------------|
| Start | Initial state before hardware initialization |
| Down | Link is down, protocols stopped |
| Init | Link up, waiting for gPTP synchronization |
| WaitVlanBase | gPTP locked, waiting for base VLAN registration |
| Ready | Fully operational, streams can run |
| Degraded | Partial failure (gPTP lost), streams stopped |

### Events

| Event | Description |
|-------|-------------|
| UCT | Unconditional transition (startup) |
| LinkUp | Network link established |
| LinkDown | Network link lost |
| GptpLocked | gPTP time synchronized |
| GptpLost | gPTP synchronization lost |
| VlanBaseReady | Base VLAN registered |
| Timeout | Watchdog timeout in waiting states |

## Listener Engine State Machine

Controls the audio receive pipeline:

```
    ┌───────┐
    │ Start │
    └───┬───┘
        │ UCT (init)
        ▼
    ┌───────┐◄─────────────────────────────────┐
    │  Off  │                                  │
    └───┬───┘                                  │
        │ GateListen (enable_rx_filter)        │
        ▼                                      │
    ┌───────────┐──GateStop──► Off (stop_all)  │
    │ Listening │                              │
    └───┬───────┘                              │
        │ FirstPacket (start_sync)             │
        ▼                                      │
    ┌─────────┐──GateStop──► Off (stop_all)    │
    │ Syncing │◄──PacketGap (resync)           │
    └───┬─────┘                                │
        │ Synced (start_audio_sink)            │
        ▼                                      │
    ┌─────────┐                                │
    │ Playing │──Underrun──► Muted (mute_out)  │
    └─────────┘                                │
        ▲ Recovered (unmute_out)               │
        │                                      │
    ┌───────┐──PacketGap──► Syncing (resync)   │
    │ Muted │──GateStop────►───────────────────┘
    └───────┘
```

## Threading Model

NanoAVB uses a **single-threaded event-driven reactor pattern**:

- All state machine transitions and callbacks execute in the reactor thread
- Network I/O is non-blocking via the reactor's poll loop
- PTP timer callbacks (for realtime audio) may run in a separate high-priority thread

### Thread Safety Guidelines

**Thread-safe operations:**
- Reading context state flags (marked with comments in code)
- Querying current state machine state via `current_state()`

**Operations requiring external synchronization:**
- State machine `handle_event()` calls must be serialized
- Setting or modifying callbacks
- Modifying EntityModel

### Callback Execution

Callbacks are invoked synchronously during state machine transitions:

```cpp
// Inside supervisor state machine:
inline void enter_ready(Context& ctx, TimePoint time)
{
    // Callback runs in reactor thread
    ctx.callbacks.enter_ready(ctx, time);
}
```

## Component Architecture

### NanoAvbComponents

Container for all protocol handlers (without network I/O):

```cpp
struct NanoAvbComponents
{
    EntityModel entity_model;           // AVDECC entity descriptors
    AemCommandHandler aem_handler;      // AEM command processing
    NanoAvbAdpAdvertiser adp_advertiser;// Entity discovery
    NanoAvbAcmpTalker acmp_talker;      // Talker connections
    NanoAvbAcmpListener acmp_listener;  // Listener connections
    MvrpHandler mvrp_handler;           // VLAN registration
    MsrpHandler msrp_handler;           // Stream reservation
};
```

### NanoAvbNetHandlers

Container for network handlers with reactor integration:

```cpp
class NanoAvbNetHandlers
{
    MvrpNetHandler mvrp;          // MVRP packet I/O
    MsrpNetHandler msrp;          // MSRP packet I/O
    AtdeccNetHandler atdecc;      // ADP/ACMP/AEM packet I/O
    GptpAnnounceHandler gptp_announce; // gPTP Announce monitoring
};
```

## Error Handling

### Callback Patterns

NanoAVB uses consistent callback patterns:

| Pattern | Signature | Use Case |
|---------|-----------|----------|
| State machine action | `void(Context&, TimePoint)` | State transition actions (init, start, stop); the `TimePoint` is the time the machine was told |
| Network send | `bool(data)` | Packet transmission (returns success/failure) |
| State notification | `void(id, state, TimePoint now)` | Observer callbacks (on_connect, on_talker_listener, on_entity_departing); `now` is the receive or tick that caused them, stashed by the handler at its told entry |

Examples:
```cpp
// State machine callback - action on transition
std::function<void(Context&, TimePoint)> enter_ready{};

// Network send - returns success/failure
std::function<bool(AdpDu const&)> send_adpdu;

// Notification - pure observer
std::function<void(uint16_t, VlanState)> on_vlan_state_change;
```

### Error Codes

NanoAVB defines error codes in `NanoAvbError`:

| Error | Description |
|-------|-------------|
| InvalidConfiguration | Configuration descriptor is invalid |
| MaxDescriptorsReached | Cannot add more descriptors |
| DescriptorNotFound | Requested descriptor does not exist |
| InvalidStreamIndex | Stream index out of range |
| InvalidVlanId | VLAN ID is 0 or 4095 (reserved) |
| StreamNotConnected | Operation requires active connection |
| DescriptorTooLarge | Descriptor exceeds AEM response buffer |

### ADP Send Failure Tracking

The ADP advertiser tracks consecutive send failures:

```cpp
// Only increment available_index on successful send (per IEEE 1722.1)
if (send_ok) {
    ++available_index_;
    consecutive_send_failures_ = 0;
} else {
    ++consecutive_send_failures_;
}

// Query failure count for diagnostics
uint32_t failures = adp_advertiser.consecutive_send_failures();
```

## Configuration Example

```cpp
// Create entity model
EntityModel model{EntityModelConfig{
    .max_configurations = 1,
    .max_stream_inputs = 2,
    .max_stream_outputs = 2
}};

// Configure entity descriptor
DescriptorEntity entity{};
entity.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
entity.entity_capabilities = entity_capabilities::AEM_SUPPORTED |
                             entity_capabilities::CLASS_A_SUPPORTED;
model.set_entity(entity);

// Create components
NanoAvbComponents components{
    .entity_model = std::move(model),
    .aem_handler = AemCommandHandler{model},
    .adp_advertiser = NanoAvbAdpAdvertiser{model, callbacks, config},
    // ...
};

// Create network handlers and wire callbacks
NanoAvbNetHandlers handlers{interface_name, components};
setup_nanoavb_callbacks(components, handlers);

// Add to reactor
handlers.add_to_reactor(reactor);
```

## Timeout Handling

State machines include timeout handling for stuck states:

```cpp
// Supervisor handles timeout in Init (gPTP lock timeout)
t.at(S::Init, E::Timeout) = T::action<timeout_gptp>(S::Down);

// Supervisor handles timeout in WaitVlanBase (VLAN timeout)
t.at(S::WaitVlanBase, E::Timeout) = T::action<timeout_vlan>(S::Degraded);
```

The application is responsible for implementing a watchdog timer that injects `Timeout` events:

```cpp
// The reactor tells its ports the cycle's time; the watchdog is told it in
// turn and tells the machine. Nothing here reads the clock twice.
auto last_state_change = sm::TimePoint{};
void tick(int64_t now_ns) {
    auto const now = sm::TimePoint{std::chrono::nanoseconds{now_ns}};
    if (now - last_state_change > WATCHDOG_TIMEOUT) {
        supervisor.handle_event(ctx, Event::Timeout, now);
        last_state_change = now;
    }
}
```

### Time

NanoAVB runs on the control-plane clock, `sm::Clock` (`std::chrono::steady_clock`),
and is told its time: the reactor reads the clock once per cycle and hands
it to every handler (`on_ready(now_ns)` / `tick(now_ns)`), the handler wraps
it into a `TimePoint` at its entry and tells the state machine, and hooks the
machine fires from its actions carry that same time. Setup and edge calls
(`start(reactor, now)`, `register_vlan(vid, now)`, controller commands) take
a default-argument `now` for callers that own no tick. Media-side stamps and
deadlines live on `CLOCK_MONOTONIC_RAW` and are told separately, typed; the
two never mix. The policy, with its forms and contracts, is core's
[`TIME_POLICY.md`](../../core/docs/TIME_POLICY.md).

## Protocol Interaction Sequence

Typical startup sequence:

```
1. LinkUp detected
   └─► supervisor: Down → Init (start_protocols)
       └─► gptp_sm: AsCapableUp
       └─► mvrp_sm: Acquire

2. gPTP Announce received with grandmaster
   └─► gptp_sm: LockedStable
       └─► supervisor: GptpLocked
           └─► supervisor: Init → WaitVlanBase

3. MVRP Join OK received
   └─► mvrp_sm: JoinOk → Joined
       └─► supervisor: VlanBaseReady
           └─► supervisor: WaitVlanBase → Ready

4. ACMP CONNECT_RX_COMMAND received
   └─► acmp_listener: connect stream
   └─► msrp_listener_sm: Advertise → Attached
       └─► listener_engine_sm: GateListen → Listening

5. First AVTP packet received
   └─► listener_engine_sm: FirstPacket → Syncing

6. Dejitter buffer filled
   └─► listener_engine_sm: Synced → Playing
```

## See Also

- `docs/LTC_DESIGN.md` - SMPTE LTC module design
- `docs/ERROR_HANDLING_EXAMPLES.md` - Error handling patterns
- `statusbar/nanoavb/nanoavb_example.cpp` - Complete usage example
