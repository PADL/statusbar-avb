<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->


# Milan Connection Management - Milan v1.3 Section 5.5

Milan keeps the ACMP PDUs of IEEE 1722.1 and changes what they mean. A
controller *binds* a stream input to a talker's stream output; the listener
answers at once and from then on does everything itself: it probes the
talker for the stream's parameters, reserves the stream with SRP, watches
the talker's SRP attribute, and starts again when anything is lost, across
power cycles included. The talker keeps no record of who listens.

That is a second implementation of connection management, and the tree
has it as one: its own files beside the IEEE 1722.1-2021 state machines
(`ACMP_STATE_MACHINES.md`), which are unchanged. An entity runs one or the
other, chosen once at start-up.

| | IEEE 1722.1-2021 clause 8 | Milan v1.3 section 5.5 |
|---|---|---|
| Protocol machines (`statusbar/atdecc`) | `atdecc_acmp_listener_sm`, `atdecc_acmp_talker_sm` | `atdecc_milan_acmp_listener_sm`, `atdecc_milan_adp_discovery_sm`, `atdecc_milan_acmp_sink`, `atdecc_milan_acmp_talker` |
| Entity side (`statusbar/nanoavb`) | `NanoAvbAcmpListener`, `NanoAvbAcmpTalker` | `NanoAvbMilanAcmpListener`, `NanoAvbMilanAcmpTalker` |
| CONNECT_RX_COMMAND | relayed to the talker; the controller is answered when the talker has answered | BIND_RX_COMMAND: saved, answered SUCCESS at once, then probed |
| CONNECT_TX_COMMAND | adds the listener to the talker's list | PROBE_TX_COMMAND: a question about the stream's parameters, nothing kept |
| A sink connected to another talker | LISTENER_EXCLUSIVE | the binding is replaced |
| Talker gone | the sink is torn down | a probing sink waits for its ADP; a settled one keeps its stream until SRP loses it |
| DISCONNECT_TX, GET_TX_CONNECTION | implemented | SUCCESS with nothing changed; NOT_SUPPORTED |
| When the talker transmits | an ACMP connection and MSRP Listener Ready | MSRP Listener Ready |

## Choosing

`NanoAvbComponents::use_milan_acmp()` makes the Milan pair. It is a
start-up choice: before it is called the components hold two null pointers
and nothing else of Milan's, and the IEEE 1722.1 pair runs. After it, the
ATDECC net handler gives every ACMP PDU to the Milan pair
(`AtdeccNetHandler::set_milan_acmp`), lets the Milan listener hear ADP, and
ticks it; the IEEE 1722.1 pair stays idle.

The entity kit calls it under the Milan profile (`atdecc.profile =
"milan13"`, or a Milan model with no profile named; see
`ENTITY_CONSTRUCTION_KIT.md`). `AvbEntityAm824IO` and `AvbEntityStereoIO`
never do.

Code that only needs ACMP's picture of a stream asks the components and
gets it from whichever pair runs: `talker_stream()`, `listener_sink()`,
`configure_talker_stream()`.

## The listener

Each STREAM_INPUT is one `atdecc::MilanSink`: the connection state machine
of 5.5.3 and the discovery state machine of 5.6.4, run together.

### Sink state machine (5.5.3)

```{graphviz} ../sm/atdecc/milan_acmp_sink_sm.dot
```

```{include} ../sm/atdecc/milan_acmp_sink_sm.md
:start-line: 2
```

The states are Table 5.29's and each action is one numbered treatment of
5.5.3.6. The framework gives each (state, event) pair one next state, so
an event whose treatment depends on a condition is split by it before it
reaches the table:

| Table 5.30 event | Events here | Split by |
|---|---|---|
| RCV_BIND_RX_CMD | `BindRefused`, `BindSameTalker`, `BindNewTalker` | the lock (step 1); the source already bound (step 2) |
| RCV_UNBIND_RX_CMD | `UnbindRefused`, `Unbind` | the lock |
| RCV_PROBE_TX_RESP | `ProbeAccepted`, `ProbeRejected` | the response's status; one that does not answer the probe sent is no event (5.5.3.6.18 step 1) |
| TMR_RETRY, TMR_NO_TK, EVT_TK_UNREGISTERED | each with a `...NoTalker` twin | whether the talker has been discovered |
| startup with a saved binding | `StartBound` | |

Timers (Table 5.30): TMR_NO_RESP 200 ms, TMR_RETRY 4 s, TMR_DELAY a random
0 to 1 s, TMR_NO_TK 10 s. A state runs at most one of them, so the sink
has one deadline. TMR_DELAY values come from a generator seeded with the
entity id (`NanoAvbMilanAcmpListener::set_random_delay` replaces it).

Probing status and ACMP status (5.3.8.6) are in the sink's context
(`MilanSinkContext::probing_status`, `acmp_status`), kept as the
treatments say. No command reports them yet.

### Discovery state machine (5.6.4)

```{graphviz} ../sm/atdecc/milan_adp_discovery_sm.dot
```

```{include} ../sm/atdecc/milan_adp_discovery_sm.md
:start-line: 2
```

One per bound sink, for the one talker it is bound to. A talker counts as
discovered only when its ENTITY_AVAILABLE names this listener's gPTP
grandmaster and domain. An available_index that did not rise, or another
current_configuration_index, means the talker restarted: the sink is told
it departed and was discovered again, and probes afresh. valid_time (in 2
second units) runs TMR_NO_ADP. `Stopped` is added to Table 5.57's two
states because the sink's machine starts and stops this one.

The configuration index is compared "if present" (5.6.4.5.2 step 2): only
when this advertisement and the last both mark the field valid
(AEM_CONFIGURATION_INDEX_VALID, IEEE 1722.1-2021 6.2.1.10).

Every bind treatment starts discovery with the talker not discovered
(5.5.3.6.3 step 4), and a controller usually binds a talker it has just
seen advertise. To the letter, a first probe that is refused or lost then
waits in PRB_W_AVAIL for the talker's next advertisement, seconds past
TMR_RETRY and TMR_DELAY. So after a bind the sink is given the talker's
last ENTITY_AVAILABLE when the entity's ADP discovery still holds one
(`MilanSink::on_advertisement_heard_earlier`): it runs through the
discovery machine as if it had arrived after the bind, with the checks of
5.6.4.5.1 and the time to live it had when it was heard. With none held
the sink waits, as the text says.

### What the listener asks of the rest of the entity

The machines know nothing of SRP, gPTP or AEM. `NanoAvbMilanAcmpListener`
takes each answer through a setter, and the entity kit supplies them:

| Question | Setter | The kit's answer |
|---|---|---|
| Is this controller shut out by the lock? (5.5.2.4) | `set_locked_out` | `AemCommandHandler::is_locked()` and `locking_controller()` |
| Our gPTP grandmaster and domain (5.6.4.5.1) | `set_gptp_state` | what the entity's own ADP reports |
| Which Talker attribute matches these parameters? (5.3.8.8) | `set_talker_registration` | `MsrpHandler::registered_talker()` with the sink's `TalkerMatch`: StreamID, destination address and VLAN |
| Was its talker heard before the bind? | the `heard` argument of `receive_controller_command` | `AtdeccNetHandler`, from the ADP discovery it feeds |
| Look again, SRP changed | `on_srp_changed()` | from `MsrpHandler`'s `on_remote_talker` |
| Start and stop the reservation | `AcmpListenerCallbacks::on_connect`, `on_disconnect` | `ListenerStreams`: MSRP Listener attach with the same `TalkerMatch`, multicast join |
| Save the bindings (5.5.2.4) | `set_on_bindings_changed` | the `listener.bindings` file, written before the controller is answered |

### The Listener declaration follows what was probed

5.3.8.5 has a settled sink declare Listener Ready for a Talker Advertise
"with matching SRP parameters". The sink's attach to MSRP names the
destination address and VLAN its PROBE_TX_RESPONSE gave
(`nanoavb::TalkerMatch`, `MsrpHandler::listener_attach`), and from then on
only a Talker attribute of the StreamID on that address and VLAN counts:
another leaves the Listener Waiting with nothing declared, and the sink
unregistered, so the two cannot disagree. A Milan talker names its VLAN
(Table 5.48). An IEEE 1722.1 talker answers VLAN 0 when its stream uses
the VLAN of the SRP Domain attribute (IEEE 1722.1-2021 8.2.1.17), and no
Talker attribute is ever declared on VLAN 0, so a sink that insisted on
it would never settle on such a talker. A probe answer of VLAN 0
therefore follows the attribute on whichever VLAN it is declared; the
sink still reports 0, the value it was given (5.3.8.9). An IEEE 1722.1
connection names no match and follows any Talker attribute of its
StreamID, as before.

### Bindings across a restart

5.3.8.3 wants the binding parameters (talker, source, controller,
STREAMING_WAIT) in non-volatile memory. With `listener.bindings = <file>`
the kit writes them at every bind and unbind and, at start, puts each
saved sink in PRB_W_AVAIL (5.5.3.6.2), where it waits for its talker's
ADP. The file is the one the IEEE 1722.1 listener uses for its
fast-connect goals, in a second form (`statusbar-listener-bindings v2`)
with the two fields the first has no place for; each listener reads both
forms. With the key unset the bindings last as long as the process: no
file is written that the configuration did not name, and one status line
at start says so. A Milan device's configuration therefore names the
file, and `stream.maap_range_file` beside it so that a restart keeps its
stream addresses.

## The talker

5.5.4 is four responses and no state, so there is no state machine:
`atdecc::milan_talker_response()` builds the response from what the talker
knows of the source at that moment.

| Command | Response |
|---|---|
| PROBE_TX_COMMAND | the stream's id, destination address and VLAN, connection_count 0 (Table 5.48); TALKER_UNKNOWN_ID for no such STREAM_OUTPUT (5.45); TALKER_DEST_MAC_FAILED while no valid address is available (5.47) |
| DISCONNECT_TX_COMMAND | SUCCESS with every stream field zero, nothing changed (5.50) |
| GET_TX_STATE_COMMAND | the stream's parameters, no listener named, REGISTERING_FAILED when the source declares its attribute and registers a Listener Asking Failed (5.52, 5.23) |
| GET_TX_CONNECTION_COMMAND | NOT_SUPPORTED (5.53) |

Every PROBE_TX_COMMAND for a stream the talker has is reported to the SRP
handler before the response goes out, which declares the stream's Talker
attribute for the next 15 seconds (4.3.3.1). The transmit gate under Milan
is the stream started and MSRP Listener Ready registered: there is no ACMP
connection to wait for (`TalkerGate::set_needs_acmp_connection`). It has
no grace either (`TalkerGate::set_listener_grace`). The IEEE 1722.1 gate
holds a stream five seconds past the last Listener Ready it saw, and a
disconnect closes it at once; a Milan talker hears of no disconnect, so
the same grace would keep frames going out for five seconds after the
last listener left and the Talker attribute was withdrawn. The MSRP
registrar keeps a Listener registered through a LeaveAll for LeaveTime,
which is what the grace was for.

## Where the text leaves a choice

- EVT_TK_REGISTERED fires at the settle itself when a matching Talker
  attribute is already registered.
- TMR_RETRY with the talker discovered (5.5.3.6.30 step 2) leaves the
  ACMP status at what the last probe ended with: the step does not reset it.
- A bind that names the source already bound is saved too: the controller
  and STREAMING_WAIT are binding parameters.
- The binding is saved before the response goes out, as the steps are
  numbered.
- TMR_NO_ADP is valid_time in units of two seconds. A valid_time of 0 is
  taken as one unit.
- A talker heard before the bind counts as discovered, and the
  configuration index is compared only when marked valid (both under
  "Discovery state machine" above).
- A probe answer naming VLAN 0 (IEEE 1722.1-2021 8.2.1.17: the SRP
  Domain's VLAN) follows the Talker attribute on any VLAN.
- A PROBE_TX_COMMAND counts for the 15 seconds of 4.3.3.1 when it is
  answered TALKER_DEST_MAC_FAILED too.
- A stop and start of the entity kit is a power cycle to a bound sink: it
  gives up its reservation and waits in PRB_W_AVAIL.
- The transmit gate has no grace under Milan.

## Not done

- BIND_STREAM and UNBIND_STREAM (the MVU form of bind and unbind, 5.4.4.6
  and 5.4.4.7, optional) and GET_STREAM_INPUT_INFO_EX (5.4.4.8, which
  reports the Probing status and ACMP status kept here).
- GET_STREAM_INFO still reports a sink as the IEEE 1722.1 listener would
  (settled reads as connected).
- START_STREAMING and STOP_STREAMING on a STREAM_INPUT do not change the
  started/stopped state saved with its binding (5.3.8.7); only a bind's
  STREAMING_WAIT sets it.
- MVRP is declared for the Class A Domain's VLAN. A sink settled on
  another VLAN does not add that one (4.4.1).
- A PROBE_TX_COMMAND is not checked against the interface it arrived on
  (5.5.4.1 step 2): the entity has one AVB interface.
