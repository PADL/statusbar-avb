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
| Was its talker heard before the bind? | the `heard` argument of `receive_controller_command` and `receive_mvu_binding_command` | `AtdeccNetHandler`, from the ADP discovery it feeds |
| Look again, SRP changed | `on_srp_changed()` | from `MsrpHandler`'s `on_remote_talker` |
| Start and stop the reservation | `AcmpListenerCallbacks::on_connect`, `on_disconnect` | `ListenerStreams`: MSRP Listener attach with the same `TalkerMatch`, multicast join |
| Declare the VLANs of the settled sinks (4.4.1) | `set_on_sink_vlans_changed` | not the kit's: `NanoAvbComponents::use_milan_acmp()` gives the list to `MvrpHandler::set_sink_vlans()` |
| Save the bindings (5.5.2.4) | `set_on_bindings_changed` | the `listener.bindings` file, written before the controller is answered; then each input's stopped state (5.3.8.7) |

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

### The VLANs of the settled sinks

4.4.1 has a Listener declare an MVRP VID attribute for each VLAN its
settled sinks use. The listener keeps the VLAN each sink settled on, the
one its PROBE_TX_RESPONSE named, and tells the list (`sink_vlans()`, each
VLAN once) through `set_on_sink_vlans_changed` when it changes: before
`on_connect` for a sink that settles, so the VLAN is asked for before the
Listener attribute, and after `on_disconnect` for one that leaves its
settled state. `NanoAvbComponents::use_milan_acmp()` hands the list to
`MvrpHandler::set_sink_vlans()`, so an entity of the Milan profile has it
with nothing to do in the kit or the host.

`MvrpHandler` keeps two holders of a VLAN apart: the entity
(`register_vlan()`, `withdraw_vlan()`: its SRP Domain's VLAN) and the
sinks. A VLAN is declared while either holds it. A sink that leaves the
entity's own VLAN takes nothing away, and a change of the SRP Domain's
VLAN leaves a sink's VLAN declared.

| What happens | MVRP |
|---|---|
| a sink settles on a VLAN nobody holds | the VLAN is declared |
| a sink settles on a VLAN the entity or another sink holds | nothing |
| the last settled sink on a VLAN leaves, the entity does not hold it | the VLAN is withdrawn |
| the entity withdraws a VLAN a sink is settled on | nothing: it stays declared |
| a sink settles on a talker that named VLAN 0 | nothing: the stream is on the SRP Domain's VLAN, the entity's own |
| the run ends (`withdraw_all()`) | every VLAN is withdrawn and nobody holds one |

### Bindings across a restart

5.3.8.3 wants the binding parameters (talker, source, controller,
STREAMING_WAIT) in non-volatile memory. The kit writes them at every
bind, unbind and change of the started/stopped state and, at start, puts
each saved sink in PRB_W_AVAIL (5.5.3.6.2), where it waits for its
talker's ADP.

Where they are written: the file `listener.bindings` names, or with none
named the file `listener-bindings` of the entity's state directory,
`<state.dir>/<entity ID as sixteen hex digits>/` (see "Saved state" in
`ENTITY_CONSTRUCTION_KIT.md`). The entity tools default `state.dir` to
`/var/lib/statusbar-avb`, which the package creates, so an installed
Milan device saves its bindings with nothing configured. Where there is
no state directory and no file named, the bindings last as long as the
process and one status line at start says so.

The file is the one the IEEE 1722.1 listener uses for its fast-connect
goals, in a second form (`statusbar-listener-bindings v2`) with the two
fields the first has no place for; each listener reads both forms.

### A stopped stream input (5.3.8.7)

A bound stream input is started or stopped. "A PAAD-AE having a stopped
Stream Input shall discard the Stream AVTPDUs it receives."

The state is a binding parameter, the STREAMING_WAIT of the sink's
binding. Three things set it:

| | Sets |
|---|---|
| A bind (BIND_RX_COMMAND, BIND_STREAM) | stopped when its STREAMING_WAIT flag is set, started when it is not |
| STOP_STREAMING on the STREAM_INPUT (5.4.2.20) | stopped |
| START_STREAMING on the STREAM_INPUT (5.4.2.19) | started |

START_STREAMING and STOP_STREAMING go to the sink
(`MilanSink::set_streaming_wait`), which saves the state as it saves a
bind. They are not events of the sink machine: the sink goes on probing,
or stays settled with its reservation, as it was. They have no effect on
an input that is not bound, and an unbind takes the state away with the
binding. The entity's lock refuses them from another controller.

Whatever changed it, the kit hears of it through the bindings hook
(`AvbEntityHost::set_on_milan_bindings_changed`) and sets the input
(`ListenerStreams::set_stream_stopped`). At start the state comes back
with the saved binding, before a frame can arrive.

While an input is stopped:

- each AVTPDU of its stream is discarded unread: nothing is decoded and
  no consumer is called;
- the sink, the MSRP Listener declaration and the group membership stay
  as they are, so the talker goes on streaming (5.5.4.1) and a start
  takes the next frame;
- GET_STREAM_INFO reports STREAMING_WAIT (Table 5.8) and GET_RX_STATE
  the same flag (Tables 5.42 to 5.44), and a change of the state sends a
  GET_STREAM_INFO notification (Table 5.20).

Under IEEE 1722.1 connection management the two commands stop and start
an input the same way. Nothing binds an input stopped there, and the
state is not saved.

A stream output cannot be stopped under the Milan profile (5.3.7.3): it
streams for as long as its Talker attribute is declared and a Listener
registered. START_STREAMING and STOP_STREAMING for a STREAM_OUTPUT are
answered NOT_SUPPORTED and change nothing (5.4.2.19, 5.4.2.20), and
GET_STREAM_INFO never reports an output as STREAMING_WAIT (Table 5.9).
Under IEEE 1722.1 they stop and start the talker's stream.

### Binding over AECP MVU (5.4.4.6 to 5.4.4.8)

A controller may bind and unbind with the vendor-unique commands
BIND_STREAM and UNBIND_STREAM, unicast to the listener, where ACMP's
BIND_RX and UNBIND_RX commands go to the ACMP multicast address. The
sink state machine has one event for each pair (RCV_BIND_RX_CMD,
RCV_UNBIND_RX_CMD), and 5.5.3.2 sends an MVU command through it as
5.5.3.1 sends the ACMP one. So there is no second treatment here:

1. `AemCommandHandler` reads the command. One shorter than its format is
   answered PAYLOAD_TOO_SHORT, one whose descriptor_type is not
   STREAM_INPUT BAD_ARGUMENTS, and neither reaches a sink.
2. The command becomes the ACMP command it equals by Table 5.22
   (`atdecc::milan_bind_command_from_mvu`, `milan_unbind_command_from_mvu`):
   the target is the listener, descriptor_index the listener_unique_id,
   talker_stream_index the talker_unique_id. STREAMING_WAIT is the one
   flag carried over; the bits Table 5.19 reserves are not ACMP flags.
3. `NanoAvbMilanAcmpListener::receive_mvu_binding_command` gives it to the
   sink, with the talker's last advertisement as for an ACMP bind. Where
   the sink would send its ACMP response, the response goes to the caller
   and nothing goes out on ACMP. The MVU response is sent from there:
   after the binding is saved and before the probe, as the steps of
   5.5.3.6.3 are numbered.
4. The status of the sink's ACMP response names the MVU one
   (`atdecc::mvu_status_for_binding_response`):

| The sink answers | The MVU response says | Table |
|---|---|---|
| SUCCESS | SUCCESS | 5.35, 5.41 |
| LISTENER_UNKNOWN_ID | NO_SUCH_DESCRIPTOR | 5.28 |
| CONTROLLER_NOT_AUTHORIZED | ENTITY_LOCKED | 5.33, 5.39 |

A sink bound one way may be bound again or unbound the other way: it is
one binding, saved in one file.

GET_STREAM_INPUT_INFO_EX reads a sink's variables
(`atdecc::milan_stream_input_info_ex`): the talker it is bound to, zero
when unbound, the Probing status and the ACMP status of 5.3.8.6. It is
not a sink event and is not shut out by the lock.

`AtdeccNetHandler::set_milan_acmp` attaches the listener's sinks to the
AEM handler (`AemCommandHandler::attach_mvu_stream_inputs`) and its
destructor takes them back. An entity that answers MVU and runs IEEE
1722.1 connection management has nothing attached, and answers the three
commands NOT_IMPLEMENTED.

`statusbar-atdecc-mvu` sends any of the three from a command line, and
GET_MILAN_INFO, and prints the answer with each field named:

```
statusbar-atdecc-mvu --interface=eth0 --command=bind \
    --target-entity-id=<listener> --stream-input=0 \
    --talker-entity-id=<talker> --talker-stream=0
statusbar-atdecc-mvu --interface=eth0 --command=stream-input-info \
    --target-entity-id=<listener> --stream-input=0
```

The MVU_BINDING flag of GET_MILAN_INFO says that BIND_STREAM and
UNBIND_STREAM are implemented. The kit sets it under the Milan profile
and clears it under any other, whatever `milan.features_flags` gives.

### Notifications (5.4.5.3, Table 5.21)

Two kinds of MVU notification come from connection management. Both go
to the controllers registered with REGISTER_UNSOLICITED_NOTIFICATION,
each with its own MVU sequence id (5.4.5.2), through
`AemCommandHandler::emit_mvu_unsolicited()`.

The command's own. A BIND_STREAM or UNBIND_STREAM answered SUCCESS that
changed the entity's state is sent on, with the data its response
carried, to every registered controller but the one that sent it
(5.4.5.3). The handler reads the stream input's binding parameters
(5.3.8.3) before and after the sink's treatment
(`MvuStreamInputCallbacks::binding`); a binding that is not what it was
is a command that was taken and changed something. The notification
follows the response.

The input's state. Table 5.21 asks for a GET_STREAM_INPUT_INFO_EX
notification when a stream input's talker, Probing status, ACMP status
or Talker attribute registration changes. Many things change those: a
command on either protocol, a probe's answer, an advertisement, SRP, a
timer. So the change is looked for in one place, once a tick, after the
sinks' timers have run: `NanoAvbMilanAcmpListener::tell_stream_input_changes()`
compares each sink with what the last look found
(`atdecc::MilanStreamInputNotified`), and `AtdeccNetHandler` has
`AemCommandHandler::notify_stream_input_info_ex()` send the input's
response as it reads then. No command is being answered at the tick, so
every registered controller is told.

What follows from that:

| What happened | BIND_STREAM or UNBIND_STREAM notification | GET_STREAM_INPUT_INFO_EX notification |
|---|---|---|
| MVU bind that changes the binding | at once, to the others | at the next tick, to all |
| MVU bind of what is bound already | none | none |
| MVU unbind of a bound input | at once, to the others | at the next tick, to all |
| MVU unbind of an unbound input | none | none |
| Bind or unbind over ACMP | none | at the next tick, to all |
| Probe answered, timed out or refused | none | when the Probing or ACMP status changed |
| Talker attribute registered, failed or gone, sink settled | none | at the next tick, to all |

### GET_STREAM_INFO of a stream input (5.4.2.10.1, Table 5.8)

A sink's state decides what GET_STREAM_INFO says of its stream input. The
kit reads it from the sink and from MSRP (`AvbEntityKit::milan_sink_stream_info`)
and `avb_entity_stream_info.hpp` turns it into the flags and fields:

| The input is | Flags set | Fields that are not zero |
|---|---|---|
| not bound | STREAM_FORMAT_VALID | the format |
| bound | FAST_CONNECT, SAVED_STATE, CONNECTED; STREAMING_WAIT while it is stopped | |
| bound, registering no matching Talker attribute | NOT_REGISTERING_SRP | |
| settled | STREAM_ID_VALID, STREAM_DEST_MAC_VALID, STREAM_VLAN_ID_VALID | the stream's id, destination address and VLAN as the PROBE_TX_RESPONSE gave them |
| settled, registering a matching Talker Advertise | MSRP_ACC_LAT_VALID | the attribute's accumulated latency |
| settled, registering a matching Talker Failed | MSRP_ACC_LAT_VALID, SRP_REGISTRATION_FAILED, MSRP_FAILURE_VALID | the latency, the failure's bridge id and code |

"Matching" is the match of 5.3.8.8 the sink itself goes by: the StreamID
on the destination address and VLAN it probed. `MsrpHandler::registered_talker()`
gives the attribute with its latency and failure information.

Table 5.20 has a GET_STREAM_INFO notification sent when the stream ID, the
destination address, the VLAN, the accumulated latency, the registration
of the Talker attribute, the failure code or bridge, the connected state
or the started/stopped state of an input changes. All of those are in the
answer, so the kit compares each input's answer, the format left out,
with the one it last looked at, once a reactor cycle
(`AvbEntityKit::end_cycle`), and notifies the registered controllers of an
input whose answer is another. What changed within one cycle is one
notification: a bind that settles before the cycle ends is told once,
settled. START_STREAMING and STOP_STREAMING notify as they are answered,
and that notification is not sent a second time.

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
holds a stream five seconds past the withdrawal of its Listener Ready, and a
disconnect closes it at once; a Milan talker hears of no disconnect, so
the same grace would keep frames going out for five seconds after the
last listener left and the Talker attribute was withdrawn. The MSRP
registrar keeps a Listener registered through a LeaveAll for LeaveTime,
which is what the grace was for.

## Advertising (5.6.2, 5.6.3)

`use_milan_acmp()` is where an entity becomes one of the Milan profile, so
it switches the ADP advertiser too (`NanoAvbAdpAdvertiser::use_milan_advertise()`):

- ENTITY_AVAILABLE says valid_time 10, which is 20 s (5.6.2).
- It goes out by the advertise state machine of 5.6.3. Every one waits out
  TMR_DELAY first, a random 0 to 4 s (0 to 2 s for the first after
  start-up). After one is sent TMR_ADVERTISE runs 5 s and asks for the next,
  so they are 5 to 9 s apart.
- An ENTITY_DISCOVER for the entity or for all, and a new grandmaster, ask
  for one through TMR_DELAY when none is waiting, and are ignored while one
  is (Table 5.56). Nothing is answered at once.

The delay is drawn from a generator seeded with the entity ID, so two
entities do not advertise in step; `set_random_delay()` replaces the draw.
An entity of the IEEE 1722.1 profile advertises as before: at start, every
5 s, and at once for an ENTITY_DISCOVER or a change.

What a controller has to allow for: a Milan entity may take 4 s to answer
an ENTITY_DISCOVER. `statusbar-atdecc-ctl` waits for an entity a command
names (see `TOOLS.md`).

Left open:

- The entity_capabilities bits 5.6.2 asks for (VENDOR_UNIQUE_SUPPORTED,
  AEM_INTERFACE_INDEX_VALID) are the ENTITY descriptor's, as the model has
  them. The advertiser does not add them.
- A start after the first counts as a LINK_UP (0 to 4 s), whether the link
  was down or the entity was stopped.

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
- A sink whose talker named VLAN 0 adds no VLAN to those declared for
  4.4.1. Should its Talker attribute be on another VLAN than the SRP
  Domain's, that VLAN is not declared. A Milan talker names its VLAN
  (Table 5.48), so this is an IEEE 1722.1 talker's case.
- The accumulated latency GET_STREAM_INFO gives for a stream input is the
  registered Talker attribute's. 5.4.2.10.1 adds "any internal
  implementation-dependent max delay" a frame meets before the MAC's
  ingress buffer; the entity knows of none and adds none.
- A stream input's GET_STREAM_INFO notification is looked for once a
  reactor cycle, so two changes inside one cycle are one notification of
  the later state.
- A PROBE_TX_COMMAND counts for the 15 seconds of 4.3.3.1 when it is
  answered TALKER_DEST_MAC_FAILED too.
- A stop and start of the entity kit is a power cycle to a bound sink: it
  gives up its reservation and waits in PRB_W_AVAIL.
- The transmit gate has no grace under Milan.
- Tables 5.28, 5.33, 5.35, 5.39 and 5.41 give the message_type of a
  BIND_STREAM or UNBIND_STREAM response as VENDOR_UNIQUE_COMMAND. It is
  sent as VENDOR_UNIQUE_RESPONSE, as every AECP response is.
- A BIND_STREAM or UNBIND_STREAM response gives back the fields 5.4.4.6
  and 5.4.4.7 name, the flags as they came, and a reserved field of zero.
  A refusal carries them too; PAYLOAD_TOO_SHORT carries no data.
- A GET_STREAM_INPUT_INFO_EX that is refused answers in the response's
  own format, eighteen octets: the descriptor it named, and zero for the
  rest. A controller that checks a response's length before its status
  still reads it.
- The handler answers ENTITY_MISBEHAVING if a sink gives a bind or unbind
  no answer at all. No transition of Table 5.31 does that.
- A discarded AVTPDU of a stopped input counts in FRAMES_RX and in no
  other counter: it was received. An input that was locked counts
  MEDIA_UNLOCKED when its first frame is discarded, on the receive
  thread, so with no frame arriving it is counted when one does. After a
  start the stream is taken as a new one: no SEQ_NUM_MISMATCH or
  MEDIA_RESET for what went by unread.
- An input that loses its stream (an unbind, or a sink that leaves its
  settled state) counts MEDIA_UNLOCKED at once if it was locked: with no
  stream it is not synchronized, and MEDIA_LOCKED equals MEDIA_UNLOCKED
  again (5.3.8.10). The same holds for an IEEE 1722.1 disconnect. The
  next stream's first AVTPDU is then compared with nothing before it: no
  SEQ_NUM_MISMATCH or MEDIA_RESET against the stream that went, and the
  input locks on a run of the new stream's packets.
- STREAM_INTERRUPTED (Table 5.6) counts a media lock lost for any reason
  other than a controller's unbind: a timestamp step out of the lock
  tolerance, which lost AVTPDUs or wrong timestamps make; a sink that
  leaves its settled state because its Talker attribute went or never
  came (5.5.3.6.36, .48; `MilanSinkContext::srp_stop`); and an input
  still locked that receives nothing for a whole observation interval
  (`StreamInputIntervals::fold()`). A talker that goes counts one
  interruption, whichever of the last two comes first. A lock the entity
  gives up is not counted: a controller's unbind or bind to another
  source, STOP_STREAMING and an IEEE 1722.1 disconnect, a departed
  talker's among them, count MEDIA_UNLOCKED alone, unless a silent
  interval ended before them.

- 5.4.5.3 has a command notified when it "modifies the state" and does
  not say of what. Here it is the stream input's binding parameters
  (5.3.8.3): the talker, the controller that bound it and the started or
  stopped state. A BIND_STREAM that names what is bound already, by the
  same controller, is answered SUCCESS and not notified. So is an
  UNBIND_STREAM of an input that is not bound.
- A bind over ACMP raises no BIND_STREAM notification: 5.4.5.3 lists MVU
  commands. The other controllers learn of it from the
  GET_STREAM_INPUT_INFO_EX notification of Table 5.21.
- Table 5.21 names the registration of the Talker attribute, which is no
  field of the response (Figure 5.12). A change of it alone is notified
  with the response as it was. Registered as Advertise, registered as
  Failed and not registered are three states; a sink that is not settled
  has no stream to match and counts as not registered.
- The GET_STREAM_INPUT_INFO_EX notification goes to the controller whose
  bind began the change too. It is sent "asynchronously of any command",
  and a BIND_STREAM response does not carry the Probing status.
- An input that changed and changed back between two ticks is not
  notified: there is nothing new to read.

## Not done

- GET_STREAM_INFO of a stream output has no Table 5.20 notification for
  the registration of its Listener attribute or the declaration of its
  Talker attribute, and reports the presentation time offset as its
  accumulated latency while the Talker attribute declares none (5.4.2.10.2
  has the two the same).
- A PROBE_TX_COMMAND is not checked against the interface it arrived on
  (5.5.4.1 step 2): the entity has one AVB interface.
