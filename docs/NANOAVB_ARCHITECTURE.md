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
    ┌───────┐──────Timeout─────► Init (timeout_gptp)    │
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

### Local ATDECC socket

`AtdeccLocalPort` (`nanoavb_local_port.hpp`) lets controllers on the same
host reach the entity without raw sockets: an AF_UNIX `SOCK_DGRAM` socket
at a configured path (`set_local_port_config()` before `add_to_reactor()`;
`AvbEntityHost::set_local_atdecc_socket()`; the kit's
`AvbEntityAudioIOConfig::atdecc_socket_path`, `--atdecc.socket` on the
entity tools). The wire protocol is la_avdecc's local protocol interface,
as ATDECCSwift's `UnixSocketPort` speaks it: one bare AVTPDU per datagram
(AVTP control header and ADP/AECP/ACMP data; no Ethernet header, padding
or COBS), at most 1500 octets. Empty, short (< 12) and oversize datagrams
are dropped. Controllers bind an address of their own (an autobound
abstract one on Linux) and connect to the path; `AtdeccLocalClient` is that
end, shaped like `RawnetContext`.

- **Clients.** A controller exists from its first datagram, keyed by the
  address it came from (an entity that restarts learns it again from its
  next one). It is forgotten when a send to it fails with
  ECONNREFUSED/ENOENT (its socket closed), when the table (16) is full and
  it is the least recently heard, or after an optional idle timeout (off by
  default: la_avdecc and ATDECCSwift controllers need not send anything
  while idle). Forgetting is deferred to the next reactor point outside
  the entity's own processing, and drops the unsolicited-notification
  registration made from that controller and any acquisition it holds
  (its MAC never comes back to release it).
- **A controller that stops reading** is not expected on a dedicated
  device. What it has not read stays charged to the entity's socket (send
  buffer raised to 1 MiB); after 64 consecutive sends to it fail for want
  of buffer it is forgotten. (A full buffer fails every controller's
  sends, so others may be forgotten too; each rejoins with its next
  datagram.)
- **The path.** `<path>.lock` is flocked for the port's lifetime (left in
  place on exit), so one entity serves a path. A socket file that a
  probe connect finds refused is stale and replaced; a live one is
  EADDRINUSE, and anything else (another error, a non-socket) is left
  alone. The socket is bound and given its mode inside a private (0700)
  directory beside the path and renamed into place, so the mode never
  reaches a symlink's target. The directory must not be writable by
  others.
- **Identity.** Each controller gets a synthetic source MAC:
  `0A:E9:1B:01:01:01` (the protocol's well-known controller MAC) for the
  first, then `0A:E9:1B:01:xx:xx` in turn. The entity handles its frames
  exactly as frames from the network, so AECP responses and unsolicited
  notifications (registered from that MAC) reach only that controller.
  Two controllers must still use distinct controller entity IDs, as on
  the network: a registration is keyed by entity ID (a repeated
  REGISTER_UNSOLICITED_NOTIFICATION moves it to the new MAC). Controllers
  see the entity as `0A:E9:1B:FF:FF:FF`.
- **Fan-out.** `AtdeccNetHandler::send()` routes: a unicast to a local
  controller's MAC (the 0A:E9:1B:01 prefix) goes to that controller only,
  or nowhere if it has been forgotten, and never to the network; every multicast
  (ADP, ACMP, the IDENTIFY notification) goes to the network and to every
  controller. The one exception: a local ENTITY_DISCOVER is answered to
  that controller alone, so local discovery adds no network traffic.
- **Not a proxy.** This is a port into the entity. Frames from local
  controllers are never forwarded onto the AVB network, and network
  traffic not sent by the entity itself never reaches them: a local
  controller sees and controls this entity only. (A proxy bridging the
  two would be a later extension.)
- **Threading.** The port is a reactor `Pollable`: control-plane thread
  only, non-blocking socket, at most 32 datagrams per readiness, and a
  controller whose queue is full loses frames instead of blocking the
  reactor. The ATDECC handler now joins the reactor even when its raw
  socket would not open (tick-only), so a local port still reaches the
  entity and its ADP/ACMP/AEM timers run.

## Entity profile: Milan or classic

`EntityProfile` (`nanoavb_entity_profile.hpp`) is one value every module
reads, so Milan-only behaviour that a classic IEEE 1722.1 controller could
trip over is switched in one place. Interoperable Milan behaviour (MRP
timers, TSpec, the AVTP `tu` bit, the MSRP Domain, ADP valid_time) is
unconditional and does not consult it.

Being Milan is a property of the device model, so the default comes from the
model: the blob's symbol table may carry one of two well-known symbols on the
ENTITY descriptor (aemxml's entity `milan` attribute,
`ENTITY_PROFILE_*_SYMBOL` in `python/aemxml/flatten.py`). The symbol table is
blob metadata the kit reads internally and is never served to a controller
(READ_DESCRIPTOR serves the TOC's descriptor bytes only), so the AVDECC
information model is unchanged. A model that says nothing is Milan; the kit
config's `milan` setting overrides the model.

Keyed off `EntityProfile::milan`:

- ACQUIRE_ENTITY answers NOT_SUPPORTED (Milan v1.2 Section 5.4.2.1; Milan
  controllers use LOCK_ENTITY). Nothing is acquired, so the controller is not
  implicitly registered for notifications either.
- MVU commands are answered only when Milan (see `docs/MVU.md`).
- ACMP: the talker and listener handlers switch between the classic and the
  Milan state machines (below).
- Unsolicited notifications: Milan v1.2 Section 5.4.5.1 notifies every
  registered controller but the requester; classic (IEEE 1722.1-2021 Cor 1)
  notifies the requester too.

## ACMP: classic and Milan

`NanoAvbAcmpTalker` and `NanoAvbAcmpListener` have two behaviours each,
chosen by `set_milan()` (`NanoAvbComponents` follows the entity profile).

- **Talker, classic** (the default): the IEEE 1722.1-2021 Clause 8.2.4 talker
  state machine, which counts the listeners connected with CONNECT_TX.
- **Talker, Milan** (Milan v1.2 Section 5.5.4): a stateless responder
  (`atdecc::milan_talker_respond`). PROBE_TX (the same message type as
  CONNECT_TX) only queries the stream; nothing is counted,
  `connection_count` is always 0 and `on_connect` / `on_disconnect` never
  fire. A probe that counts for the Talker Advertise window (Section 4.3.3.1)
  is reported through `on_probe`. Unset, the stream-state hook reports the
  destination address valid when not zero, interface 0, no Asking Failed.
- **Listener, classic** (the default): the Clause 8.2.4 listener state
  machine, with fast-connect goals.
- **Listener, Milan** (Milan v1.2 Section 5.5.3): one `atdecc::MilanListener`
  sink per stream (`milan_sinks()`). CONNECT_RX / DISCONNECT_RX are BIND_RX /
  UNBIND_RX (the same message types). A settled sink is mirrored into the
  stream table (`get_stream()`, `is_connected()`) and reported through
  `on_connect`, leaving the settled states through `on_disconnect`, so the
  data plane follows a Milan sink exactly as a classic connection.
  Fast-connect goals and `on_talker_departed` are unused: the sinks discover
  their talkers themselves (`receive_adpdu`).

In both profiles a talker without a destination address yet (MAAP still
acquiring) refuses CONNECT_TX with TALKER_DEST_MAC_FAIL (IEEE 1722.1-2021
Table 8-3) rather than hand the listener an address it will not stream to.
Nothing is connected and the listener's controller retries; the next connect
or probe after MAAP gets the MAAP address. After a restart a CONNECT_TX can
arrive before MAAP has defended its block.

## AEM command handling

- **Response size.** `MAX_AEM_RESPONSE_SIZE` is what one Ethernet frame
  carries after the AEM header (1500 - 24 octets): Milan v1.3 Section 5.4.1
  lets a response exceed control_data_length 524, and GET_AUDIO_MAP with 176
  mappings is 1420 octets.
- **Other AECP types.** Any AECP command type other than AEM (ADDRESS_ACCESS,
  AVC, VENDOR_UNIQUE, HDCP_APM, EXTENDED) addressed to the entity is answered
  NOT_IMPLEMENTED, the command echoed as the response type, rather than
  dropped, so the controller does not time out (IEEE 1722.1-2021 Clause
  9.2.1.1.6). `dispatch_aecp` routes MVU commands to the MVU handler first,
  so this covers other vendor unique protocols.
- **Counters notifications.** Milan v1.2 Table 5.22 allows no more than one
  GET_COUNTERS notification per descriptor per second. `tick()` sends them,
  each stamped with the time it is made: the next tick for a descriptor quiet
  for a second, else the tick a second after its last one.
- **Stream state table.** STREAM_INPUT and STREAM_OUTPUT descriptors with
  runtime state (a SET_STREAM_FORMAT or a latched START/STOP_STREAMING) share
  one table of 2 x 32 entries, the 32 streams the ACMP talker and listener
  contexts each hold (`atdecc::TalkerContext` / `ListenerContext`
  `MAX_STREAMS`).
- **Managed object names** (IEEE 1722.1-2021 Clauses 7.4.17-18). Opt-in like
  the entity name: after `manage_object_names()` the storage handler serves
  GET_NAME and applies SET_NAME (name_index 0) for the renameable
  descriptors: CONTROL (the IDENTIFY control among them), SIGNAL_SELECTOR,
  MATRIX, CONTROL_BLOCK and every object_name Milan v1.2 Section 5.3.13 has
  the entity keep (CONFIGURATION, AUDIO_UNIT, STREAM_INPUT, STREAM_OUTPUT,
  AVB_INTERFACE, CLOCK_SOURCE, AUDIO_CLUSTER, CLOCK_DOMAIN, and v1.3's
  TIMING, PTP_INSTANCE and PTP_PORT), plus the ENTITY's group_name
  (name_index 1), with READ_DESCRIPTOR reflecting the override. Overrides
  live in RAM and reset to the authored name on restart; an entity that keeps
  them reads them with `for_each_object_name_override` /
  `group_name_override` and restores them through `on_set_name`. SET_NAME
  with an empty name clears back to the authored name.
- **Runtime settings.** The storage handler visits each setting controllers
  changed from the blob's authored values, so an entity that keeps them
  across restarts (Milan v1.2 Section 5.3) can save them and restore them
  through `on_set_descriptor_value` and `on_set_name`, which validate them as
  a controller's command would be. `set_runtime_stream_format` changes a
  stream's format without the stream-format callback (the application
  re-timing streams on SET_SAMPLING_RATE notifies controllers itself, Milan
  v1.2 Section 5.4.5.2).
- **Persistent settings.** `is_persistent_setting_command` lists the commands
  whose effect Milan v1.2 has the entity keep in non-volatile memory and that
  nanoavb implements: SET_NAME, SET_STREAM_FORMAT, SET_SAMPLING_RATE,
  SET_CLOCK_SOURCE, the presentation time offset (SET_STREAM_INFO,
  SET_MAX_TRANSIT_TIME) and ADD/REMOVE_AUDIO_MAPPINGS. After such a command
  succeeds, its response and notification have gone, `on_settings_changed`
  is told and the entity re-reads and saves its settings.
- **Presentation time offset.** SET_STREAM_INFO follows Milan v1.2 Section
  5.4.2.9: NOT_SUPPORTED on a STREAM_INPUT, and on a STREAM_OUTPUT only
  MSRP_ACC_LAT_VALID (BAD_ARGUMENTS past 0x7FFFFFFF ns; any other
  sub-command refuses the whole command NOT_SUPPORTED). SET/GET_MAX_TRANSIT_TIME
  (IEEE 1722.1-2021 Clauses 7.4.77-78, Milan v1.3 Sections 5.4.2.30-31) set
  the same offset. A change through either notifies SET_STREAM_INFO or
  SET_MAX_TRANSIT_TIME, GET_STREAM_INFO and GET_MAX_TRANSIT_TIME (a Milan 1.2
  controller reads the offset in GET_STREAM_INFO). In the IEEE 1722.1-2021
  GET_STREAM_INFO layout (`set_stream_info_layout`) SET_STREAM_INFO's
  MSRP_ACC_LAT_VALID is BAD_ARGUMENTS (Milan v1.3 Section 5.4.2.9) and only
  SET_MAX_TRANSIT_TIME sets the offset.
- **Notifications.** Each registration has its own MVU sequence id, a space
  apart from the AEM one (Milan v1.2 Section 5.3.4.2, as v1.3 Section 5.4.5.1
  and controllers have it), incremented whether or not the send succeeds.
  `note_controller_command` restarts a controller's monitor timer for MVU
  commands, which the AEM handler does not see (Milan v1.2 Section 5.4.5.3).
  `forget_controller_at` drops a controller whose MAC will not come back (a
  local controller whose socket closed): otherwise a dead acquirer would
  hold the entity until it restarts.

### Dynamic audio mappings

A STREAM_PORT_INPUT with no static AUDIO_MAP keeps dynamic mappings (Milan
v1.2 Sections 5.3.10, 5.4.2.26-28; the store is `nanoavb_dynamic_audio_maps.hpp`).
Each channel of each of its clusters maps from at most one channel of a
STREAM_INPUT; a stream channel may feed several.

- ADD_AUDIO_MAPPINGS takes a command whose every mapping names an existing
  cluster channel of the port and an existing channel of the stream's current
  format at the audio unit's rate (unless the port converts rates), no two
  naming one cluster channel from different stream channels, and none mapping
  an already mapped cluster channel from another stream channel (Milan allows
  refusing it: the controller removes the old one first). Anything else is
  BAD_ARGUMENTS with nothing added.
- REMOVE_AUDIO_MAPPINGS removes what it lists; a mapping not there, or listed
  twice, is ignored, and one naming no cluster channel or stream of the model
  is BAD_ARGUMENTS.
- Each change goes through `on_audio_mappings_changed` before it is kept (all
  or nothing; the data plane may refuse with BAD_ARGUMENTS), is answered with
  the command echoed, notified to the other registered controllers, and
  reported to `on_settings_changed`. `set_audio_mappings` seeds default or
  saved mappings without notification or settings report;
  `apply_local_audio_mappings` is the entity's own REMOVE-then-ADD (refused
  while locked or acquired), notified in AECPDUs of at most 63 mappings.
- GET_AUDIO_MAP partitions the port's cluster channels, in cluster order,
  into maps of `MAX_AUDIO_MAP_MAPPINGS` (fixed for the model) and answers
  map_index P with the mappings of partition P. A port with static maps
  answers NOT_SUPPORTED (its AUDIO_MAP descriptors say them). A dynamic
  STREAM_PORT_OUTPUT answers GET_AUDIO_MAP with the entity's own mappings
  (`set_output_mappings`, a store apart from the inputs', validated as an ADD
  but for the unit's rate, which the entity keeps; a change notified as
  REMOVE then ADD) and ADD/REMOVE NOT_SUPPORTED: controller mappings on it are
  not implemented, which Milan v1.2 Sections 5.4.2.27-28 require of a port
  without AUDIO_MAP; a Milan model gives its output ports static AUDIO_MAPs.

### SrpPort

`SrpPort` (`nanoavb_srp_port.hpp`) is MSRP and MVRP on one interface without
an ATDECC entity: the two handlers, their net handlers (`nanoavb_srp_net.hpp`,
the same ones `NanoAvbNetHandlers` adds) and the Class A Domain. It serves an
end station that only declares Talker Advertise and Listener attributes (IEEE
802.1Q-2022 Clause 35.2.4) and adopts the bridge's Domain as below:

1. `add_to_reactor()` opens the sockets (or `set_senders()` and the
   `receive_*()` / `tick()` calls stand in for them);
2. `declare_domain()` declares the configured Class A Domain and joins its VID,
   at start and on each link up;
3. `declare_talker()` advertises a stream on the Domain's VID and priority;
   `attach_listener()` declares a Listener for a StreamID, found from a
   stream's destination with `find_registered_talker_advertise(destination,
   vid)` (Clause 35.2.2.8.3: one stream per destination);
4. `SrpPortObserver` reports peers' registrations as they happen: the Domain
   (SRP is in force while the peer's is registered), Talker Advertise, Talker
   Failed and their leaving, the Listener declaration for each of our talker
   streams, and an attached listener following its talker. A Domain VID
   adopted from the bridge moves the MVRP join and re-advertises the talkers.
   The Domain and a talker's leaving are reported once the received PDU is
   done, with their final state: a peer's Domain counts only at our
   SRclassPriority (Clause 35.2.1.4), and a talker whose Advertise turns into
   a Talker Failed has not left.
5. `withdraw_all()` withdraws every talker, listener, the Domain and its VID
   and sends the Leaves at once (`MsrpParticipant::transmit()`,
   `MvrpParticipant::transmit()`), before the port closes: its peer drops the
   registrations at its leave timer instead of its next LeaveAll.

The participant is `DiscoveryMsrpLimits`: matching by destination needs every
Talker attribute the bridge propagates (it does not prune, Clause 35.2.1.4 b),
so it holds 256 of each, 32 kept for the port's own streams. A withdrawn
stream's entry in the handler's tables is reused by the next new StreamID.

The listener behaves as the Milan listener below (nothing declared until the
talker's attribute is registered).

## MSRP: domain, listener and table sizing

- **Domain declaration.** Without an explicit MSRP Domain declaration a
  bridge treats the port as an SRP domain boundary for every SR class it
  supports (IEEE 802.1Q Clauses 35.2.1.4, 35.2.2.9) and converts inbound
  Talker Advertise declarations into Talker Failed with failure_code 8
  ("Egress port is not AVB-capable"), so a remote talker's stream can never
  reserve a path to the entity. Only Class A (SRclassID 6, priority 3) is
  declared, on the VID in use: Milan v1.2 Section 4.2.7.2.1 declares "an MSRP
  Domain for Class A", and the entity has no Class B streams (every STREAM_*
  descriptor sets CLASS_A, Section 5.3.3.4). After startup or link up
  `reset_domain()` restores the configured domain (priority 3, VID 2 by
  default) and declares it, whether or not gPTP is up.
- **Domain adoption.** A peer's Class A Domain with different parameters is
  adopted (Milan v1.2 Section 4.2.7.2.1), but only its SRclassVID and only at
  our SRclassPriority (3): stream frames always carry that PCP (the shaper's
  queue map is keyed by it), so adopting another priority would declare a
  TSpec the frames do not use. A Class A Domain at another priority leaves the
  port an SRP domain boundary (IEEE 802.1Q-2018 Clause 35.2.1.4 h), as in
  GenAVB and lib_avb. The VID is the same at every position of a Domain
  vector (a bridge's default `{5,2,VID},{6,3,VID}`); the participant hands
  over the per-position value. Adoption happens at most once per link up: two
  adopting end stations on a direct link with different configured VIDs would
  otherwise swap each other's previous value on every periodic PDU, churning
  MVRP, ACMP, the TX tag and the advertisements. A VID change is reported so
  the talker moves its MVRP join, Talker Advertise and stream frames.
- **Milan listener.** With `set_listener_waits_for_talker` (Milan v1.2
  Section 5.3.8.5) an attached listener declares Listener Ready while the
  talker's Advertise is registered, Asking Failed while its Talker Failed is,
  and nothing otherwise (rather than Asking Failed); never Ready Failed. Only
  a peer's live registration counts (`peer_talker_advertise`: never one the
  peer has left, whose record lingers until reclaimed, nor our own
  declaration). The remote-talker hook is the Milan listener's
  EVT_TK_REGISTERED / UNREGISTERED source.
- **Talker permission.** `on_talker_listener` reports "may transmit" per
  Milan v1.2 Section 5.3.7.3: a talker streams while it registers a Listener
  Ready or Listener Ready Failed, not Asking Failed.
- **Full tables.** A declaration refused with `attribute_table_full` is the
  caller's to see, and the reservation entry is put back as it was (a new one
  removed, an existing one restored), so the table never claims a
  declaration that was never sent.
- **Table sizing.** `entity_msrp_config` gives the participant the full
  compile-time capacity (`srp::msrp::DefaultMsrpLimits`), since each table
  holds both what the entity declares and what its peers declare to it.
  Without pruning, a bridge propagates every Talker Advertise on the network,
  so part of each table is reserved for the entity's own attributes and those
  of the streams it is party to: the talker streams' own Advertises plus one
  bound talker's Advertise (and Talker Failed) per listener stream, and the
  listener streams' own Listener declarations plus one peer Listener per
  talker stream. Other stations' advertises get only what is left, so they
  can never refuse the entity's declarations (each reservation is clamped to
  its table; at 32 + 32 streams nothing is left).

## gPTP Announce tracking

The Announce handler's grandmaster expires when Announce messages stop for
the announce receipt timeout (IEEE 802.1AS Clause 10.7.3.2:
announceReceiptTimeout, 3, times the sender's announce interval), which
reports `grandmaster_lost`; the next Announce reports a grandmaster again.
The timeout uses the slow end of Milan v1.2's tolerance: Table 4.1 allows an
Announce interval +50% (1 s nominal, 1.5 s max) and Table 4.2 an
announceReceiptTimeout up to 4.5 s, so a compliant but slow sender is not
declared lost. The log interval is clamped to [-3, 4] (125 ms to 16 s). The
grandmaster state feeds the talker's AVTP `tu` bit (Milan v1.2 Section
4.3.5.2, Annex B.1.1). The socket hears the host's own transmissions, so a
local ptp4l that is grandmaster keeps `has_grandmaster()` true with its own
Announces. `path_trace()` is the last Announce's path trace TLV (IEEE
802.1AS-2020 Clause 10.6.3.3), served by GET_AS_PATH (IEEE 1722.1-2021 Clause
7.4.41, Milan v1.2 Section 5.4.2.24) and notified when it changes (Table
5.22).

## Error Handling

### Callback Patterns

NanoAVB uses consistent callback patterns:

| Pattern | Signature | Use Case |
|---------|-----------|----------|
| State machine action | `void(Context&, TimePoint)` | State transition actions (init, start, stop) |
| Network send | `bool(data)` | Packet transmission (returns success/failure) |
| State notification | `void(id, state)` | Observer callbacks (on_connect, on_state_change) |

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
// Supervisor reports a timeout in Init (gPTP lock timeout) and keeps
// waiting: ADP runs with the link, gPTP or not (Milan v1.3 5.6.3)
t.at(S::Init, E::Timeout) = T::action<timeout_gptp>(S::Init);

// Supervisor handles timeout in WaitVlanBase (VLAN timeout)
t.at(S::WaitVlanBase, E::Timeout) = T::action<timeout_vlan>(S::Degraded);
```

The application is responsible for implementing a watchdog timer that injects `Timeout` events:

```cpp
auto last_state_change = std::chrono::steady_clock::now();
while (!shutdown_requested) {
    auto now = std::chrono::steady_clock::now();
    if (now - last_state_change > WATCHDOG_TIMEOUT) {
        supervisor.handle_event(ctx, Event::Timeout, sm_now);
        last_state_change = now;
    }
    reactor.poll(100);
}
```

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
- `docs/MVU.md` - Milan Vendor Unique commands
- `statusbar/nanoavb/nanoavb_example.cpp` - Complete usage example
