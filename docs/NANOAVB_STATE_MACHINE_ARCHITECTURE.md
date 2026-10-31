<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->


# NanoAVB State Machine Architecture

This document describes the hierarchical state machine architecture used to coordinate AVB protocol lifecycle and stream engine control in the NanoAVB module.

## Overview

The NanoAVB module uses a layered state machine architecture where a top-level **Supervisor** coordinates multiple protocol and stream engine state machines. This design provides:

- Clear separation of concerns between protocols
- Predictable lifecycle management
- Graceful degradation on failures
- Easy debugging via state observation

## State Machines

The diagrams and transition tables below are generated from the compiled state machine definitions using `nanoavb_supervisor_sm_tool`.

### supervisor_sm (Top-Level Coordinator)

Manages overall AVB endpoint lifecycle from link detection through stream readiness.

```{graphviz} ../sm/nanoavb/supervisor_sm.dot
```

```{include} ../sm/nanoavb/supervisor_sm.md
:start-line: 2
```

### gptp_sm (gPTP Synchronization)

Tracks IEEE 802.1AS time synchronization status.

```{graphviz} ../sm/nanoavb/gptp_sm.dot
```

```{include} ../sm/nanoavb/gptp_sm.md
:start-line: 2
```

### mvrp_sm (VLAN Registration)

Manages 802.1Q VLAN registration via MVRP.

```{graphviz} ../sm/nanoavb/mvrp_sm.dot
```

```{include} ../sm/nanoavb/mvrp_sm.md
:start-line: 2
```

### adp_adv_sm (ADP Advertise)

The ADP advertise state machine of Milan v1.2 Section 5.6.3 (IEEE 1722.1-2021 Clause 6.2.4 with link state): a random TMR_DELAY before every ENTITY_AVAILABLE, ENTITY_DEPARTING only on shutdown.

```{graphviz} ../sm/nanoavb/adp_adv_sm.dot
```

```{include} ../sm/nanoavb/adp_adv_sm.md
:start-line: 2
```

### acmp_talker_sm (ACMP Talker)

Manages ACMP talker connection state.

```{graphviz} ../sm/nanoavb/acmp_talker_sm.dot
```

```{include} ../sm/nanoavb/acmp_talker_sm.md
:start-line: 2
```

### acmp_listener_sm (ACMP Listener)

Manages ACMP listener connection state.

```{graphviz} ../sm/nanoavb/acmp_listener_sm.dot
```

```{include} ../sm/nanoavb/acmp_listener_sm.md
:start-line: 2
```

### msrp_talker_sm (MSRP Talker Reservation)

Manages 802.1Qat stream reservation protocol for talkers.

```{graphviz} ../sm/nanoavb/msrp_talker_sm.dot
```

```{include} ../sm/nanoavb/msrp_talker_sm.md
:start-line: 2
```

### msrp_listener_sm (MSRP Listener Reservation)

Manages 802.1Qat stream reservation protocol for listeners.

```{graphviz} ../sm/nanoavb/msrp_listener_sm.dot
```

```{include} ../sm/nanoavb/msrp_listener_sm.md
:start-line: 2
```

### talker_engine_sm (Audio TX Pipeline)

Controls the audio capture and AVTP transmission pipeline.

```{graphviz} ../sm/nanoavb/talker_engine_sm.dot
```

```{include} ../sm/nanoavb/talker_engine_sm.md
:start-line: 2
```

### listener_engine_sm (Audio RX Pipeline)

Controls the AVTP reception and audio playback pipeline.

```{graphviz} ../sm/nanoavb/listener_engine_sm.dot
```

```{include} ../sm/nanoavb/listener_engine_sm.md
:start-line: 2
```

## Integration Pattern

### Callback Wiring

State machines communicate via callbacks. Each state machine has a `Callbacks` struct with function pointers for each action:

```cpp
struct SupervisedNanoAvb
{
    supervisor_sm::Context supervisor_ctx{};
    supervisor_sm::Machine supervisor{};

    gptp_sm::Context gptp_ctx{};
    gptp_sm::Machine gptp{};

    // ... other state machines

    void wire_callbacks(NanoAvbComponents& components, NanoAvbNetHandlers& handlers)
    {
        // Supervisor starts protocols on link up
        supervisor_ctx.callbacks.start_protocols = [this, &handlers](auto& ctx, TimePoint time) {
            gptp.handle_event(gptp_ctx, gptp_sm::Def::Event::AsCapableUp, time);
            mvrp.handle_event(mvrp_ctx, mvrp_sm::Def::Event::Acquire, time);
        };

        // gPTP notifies supervisor when locked
        gptp_ctx.callbacks.report_locked = [this](auto& ctx, TimePoint time) {
            supervisor.handle_event(supervisor_ctx, supervisor_sm::Def::Event::GptpLocked, time);
        };

        // MVRP notifies supervisor when joined
        mvrp_ctx.callbacks.mark_joined = [this](auto& ctx, TimePoint time) {
            supervisor_ctx.vlan_base_ready = true;
            supervisor.handle_event(supervisor_ctx, supervisor_sm::Def::Event::VlanBaseReady, time);
        };
    }
};
```

### Event Flow Example

When a network link comes up:

1. **Network layer** detects link up
2. **Supervisor** receives `LinkUp` → transitions `Down → Init`, calls `start_protocols()`
3. **start_protocols()** dispatches `AsCapableUp` to `gptp_sm`, `Acquire` to `mvrp_sm`
4. **gptp_sm** starts servo via `GptpAnnounceHandler`
5. When gPTP syncs, **gptp_sm** calls `report_locked()` callback
6. **report_locked()** dispatches `GptpLocked` to supervisor
7. **Supervisor** transitions `Init → WaitVlanBase`, calls `enter_wait_vlan()`
8. When MVRP joins, **mvrp_sm** calls `mark_joined()` callback
9. **mark_joined()** dispatches `VlanBaseReady` to supervisor
10. **Supervisor** transitions `WaitVlanBase → Ready`, calls `enter_ready()`
11. **enter_ready()** enables stream engines

### Component Integration

The state machines integrate with `NanoAvbComponents` (protocol handlers) and `NanoAvbNetHandlers` (network I/O):

```cpp
// Components hold protocol state
NanoAvbComponents components = create_nanoavb_components();

// Network handlers provide I/O
NanoAvbNetHandlers net_handlers{interface_name, components};

// Wire components to network handlers
setup_nanoavb_callbacks(components, net_handlers);

// Create supervised state machine controller
SupervisedNanoAvb supervised;
supervised.wire_callbacks(components, net_handlers);

// State machines respond to network events
supervised.on_link_up(time);  // Triggers supervisor state machine
```

## Tools

### State Machine Documentation Tool

The `nanoavb_supervisor_sm_tool` generates documentation for all state machines:

```bash
# Generate DOT graph for supervisor
./build/build-Debug/statusbar/nanoavb/nanoavb_supervisor_sm_tool --format=dot --machine=supervisor_sm > supervisor.dot
dot -Tpng supervisor.dot -o supervisor.png

# Generate Markdown tables for all machines
./build/build-Debug/statusbar/nanoavb/nanoavb_supervisor_sm_tool --format=markdown

# List available state machines
./build/build-Debug/statusbar/nanoavb/nanoavb_supervisor_sm_tool --list
```

## Design Rationale

### Why Hierarchical State Machines?

1. **Modularity**: Each protocol has its own state machine with clear responsibilities
2. **Testability**: State machines can be tested in isolation
3. **Observability**: Current state is always known and loggable
4. **Determinism**: Given same events, same transitions occur
5. **Error Recovery**: Graceful degradation paths are explicit in the transition table

### Why Callbacks Instead of Direct Coupling?

1. **Decoupling**: State machines don't need to know about each other's types
2. **Flexibility**: Different integration patterns can be used
3. **Testing**: Callbacks can be mocked for unit tests
4. **Tracing**: Callbacks provide natural instrumentation points

## Files

| File | Description |
|------|-------------|
| `nanoavb_supervisor_sm.hpp` | Top-level supervisor state machine |
| `nanoavb_gptp_sm.hpp` | gPTP synchronization state machine |
| `nanoavb_mvrp_sm.hpp` | VLAN registration state machine |
| `nanoavb_msrp_talker_sm.hpp` | MSRP talker reservation state machine |
| `nanoavb_msrp_listener_sm.hpp` | MSRP listener reservation state machine |
| `nanoavb_talker_engine_sm.hpp` | Audio TX pipeline state machine |
| `nanoavb_listener_engine_sm.hpp` | Audio RX pipeline state machine |
| `nanoavb_acmp_talker_sm.hpp` | ACMP talker connection state machine |
| `nanoavb_acmp_listener_sm.hpp` | ACMP listener connection state machine |
| `nanoavb_adp_adv_sm.hpp` | ADP advertiser state machine |
| `nanoavb_example.cpp` | Example integration with `SupervisedNanoAvb` |
| `nanoavb_supervisor_sm_tool.cpp` | DOT/Markdown documentation generator |
