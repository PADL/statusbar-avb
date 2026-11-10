<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->

# Milan talker: connection management, declarations and the transmit gate

A Milan talker answers ACMP without connection state
(`statusbar/atdecc/atdecc_acmp_milan_talker.hpp`), declares a stream's MSRP
Talker Advertise only while someone wants it (`avb_entity::TalkerDeclarations`)
and transmits only while a Listener attribute admits it
(`avb_entity::TalkerGate`). Milan v1.2 Sections 4.3.3.1, 5.3.7 and 5.5.4.

## Connection management

A Milan talker keeps no ACMP connection state (Milan v1.2 Section 5.5.2.7): it
learns whether anyone wants a stream from MSRP alone (Listener Ready / Ready
Failed registrations), and PROBE_TX_COMMAND only queries a stream's parameters.
`atdecc_acmp_milan_talker.hpp` is therefore a stateless responder: each command
is answered from what the stream output is now (its stream identity, and
whether MAAP has given it a destination address). Nothing it receives changes
the talker, except that a probe opens the 15 s window in which the talker
declares its Talker Advertise (Section 4.3.3.1). That window belongs to the
caller; `milan_talker_respond` reports whether a probe counts.

PROBE_TX_COMMAND is CONNECT_TX_COMMAND renamed (Section 5.5.2.2), so a classic
listener's CONNECT_TX gets a PROBE_TX_RESPONSE with connection_count 0 and goes
on to declare its Listener Ready, which is all a Milan talker needs from it.

Without a MAAP address (Section 4.3.3.1 condition 1) a probe is answered
TALKER_DEST_MAC_FAIL rather than ignored: a silent talker leaves the listener
probing. A probe counts for the declaration window when it is a
PROBE_TX_COMMAND for a valid stream output on its own AVB interface, answered
SUCCESS or TALKER_DEST_MAC_FAIL.

| Command           | Responses                                                         |
|-------------------|-------------------------------------------------------------------|
| PROBE_TX          | TALKER_UNKNOWN_ID (Table 5.40), INCOMPATIBLE_REQUEST (5.41), TALKER_DEST_MAC_FAIL (5.42), SUCCESS (5.43) |
| DISCONNECT_TX     | TALKER_UNKNOWN_ID (5.44) or SUCCESS, changing nothing (5.45)      |
| GET_TX_STATE      | TALKER_UNKNOWN_ID (5.46) or SUCCESS (5.47)                         |
| GET_TX_CONNECTION | NOT_SUPPORTED (5.48), whatever the unique id                       |

Fields the tables call undefined are left as the command had them; the fields
of Table 5.25 (CLASS_B, SAVED_STATE, the encryption flags) are cleared in every
successful response.

## When a stream declares its Talker Advertise

`TalkerDeclarations` decides when each stream output declares its MSRP Talker
Advertise. A Milan talker declares only while the stream's SRP parameters are
valid (Section 4.3.3.1):

1. MAAP has allocated the destination address and reports no conflict, and
2. a PROBE_TX_COMMAND for the stream arrived in the last 15 s, or a Listener
   attribute for its stream ID is registered,

so a stream nobody asked for costs the network nothing. While declaring, a MAAP
conflict withdraws the attribute; the stream "waits for 2 LeaveAll periods and
allocates a new Destination MAC Address, then declares a new Talker attribute"
(Table 5.3). The hold follows every withdrawal but binds only a re-declaration
with different parameters: the same stream ID, address and VID come back at
once (a probe after the 15 s window ran out re-declares without waiting). A parameter change the control plane chose (the
adopted Class A VID) is re-declared without a hold.

A departed Listener keeps condition 2 true for `LISTENER_GRACE_NS`, the same
LeaveAll re-registration blip the transmit gate rides out: only a Listener gone
that long withdraws a stream whose probe window is over.

A classic (non-Milan) entity skips condition 2 (`interest_required` false):
every stream declares as soon as its address is valid and is not held (a new
MAAP address is declared at once). A new VID (the adopted Class A Domain) is not
held in either profile.

The control plane must also be up (`set_active`): the host's MSRP talker is
advertising (Ready), not withdrawn (Degraded / Down). The class is pure state
and time; the kit applies what `evaluate()` decides to MSRP and runs a timer to
`next_deadline()`.

## The transmit gate

`TalkerGate` evaluates SRP admission per stream, independently of every other
stream; a CRF media-clock stream gates on its own reservation and never rides
the audio streams' gate. Milan v1.2 Section 5.3.7.3: "As long as a PAAD is
declaring a Talker Advertise attribute and receiving a Listener Ready or
Listener Ready Failed attribute for a Stream Output, it shall be streaming AVTP
packets"; Section 5.5.2.5: when no active Listeners remain "the Talker will stop
transmitting the stream". A talker puts stream `idx` on the wire only when:

1. an MSRP Listener declaration that permits transmission (Ready or Ready
   Failed, not Asking Failed) is registered for the stream's stream_id, or was
   within `GRACE`;
2. our Talker Advertise is declared. This is implied by (1): a listener
   declaration for our stream only arises against our Talker Advertise, and the
   gate closes at once when we withdraw it (`note_talker_withdrawn`);
3. the stream is Started (default true; stops a stream without tearing down its
   reservation).

ACMP connections play no part: a Milan listener binds with PROBE_TX_COMMAND and
a Milan talker tracks no connections, while a classic IEEE 1722.1 listener
declares Listener Ready after CONNECT_RX all the same. Streams are matched by
the ACMP stream_id, the same value MSRP advertises. The match is a StreamID to
stream index lookup given to the gate (`TalkerGate::StreamIndexFn`); the kit's
is its ACMP talker table, and a talker without an ATDECC entity (one driving an
`nanoavb::SrpPort`) gives its own.

### One clock, one stamp, one predicate

The gate's clock is `CLOCK_MONOTONIC_RAW` (`TalkerGate::RawTimePoint`,
`realtime::MonotonicRawClock`): the clock a media wake is told on
(`TimerWakeInfo::actual_raw`), and the one `net::rx_clock_now()` reads. The
reactor is told its own time (`sm::TimePoint`, the reactor's `ClockFn`) and has
no line to RAW, so for each event that changes or evaluates the gate it reads
RAW once (`AvbEntityKit::note_listener_ready` receives that reading beside the
reactor's time; `on_admission_timer` is told one). The reactor is not real-time;
the media thread never reads a clock for the gate.

Per stream the gate holds one stamp, `ready_raw_ns_`: the last RAW instant the
Listener declaration was known registered. `note_listener_ready` writes it on a
registration and on a withdrawal (the moment the registration was last known),
then the `listener_ready_` flag with release; `note_talker_withdrawn` zeroes the
stamp (no grace) before clearing the flag. A reader loads the flag with acquire
and then the stamp, so a withdrawal never pairs an old stamp with a cleared flag.

`admitted(idx, now)` is the whole predicate: gating off, or Started and
(`listener_ready_` or `now < stamp + GRACE`). `should_transmit(idx, now)` is
`admitted()` by another name for the media wake, so a Ready set and cleared
between two media wakes, a STOP through a LeaveAll blip and a START within the
grace, a Talker withdrawal followed by a Ready, all read the same from the
reactor and from the media thread (`avb_entity_talker_gate_test.cpp`).
`grace_ends(idx)` gives a host the instant the stamp's grace runs out, for a
deadline of its own.

The kit adds two things to the gate for what goes on the wire:
`AvbEntityKit::talker_should_transmit()` is `admitted()` and the MAAP address
ready (in "maap" mode) and the stream not STOP_STREAMING'd; the STREAM_OUTPUT
`streaming` counter follows it. The shaper's admitted set
(`admitted_talker_streams()`) is the gate's `admitted()` alone: a stream
stopped by a controller, or waiting for its MAAP block, keeps its reservation
and its share of the shaper, because its Talker Advertise stays declared (IEEE
1722.1-2021 Clause 7.4.37, STOP_STREAMING, withdraws nothing) and the listener
keeps its Ready; the credit is there the moment it streams again.

The grace window covers the MRP LeaveAll cycle: the peer's Listener declaration
ages out and re-declares on a roughly 10 s cycle, momentarily withdrawn for 1-3
s, which must not chop the stream. Only a registration or a withdrawal moves
the stamp, so a withdrawn declaration closes the gate `GRACE` later and the
window cannot be extended without a fresh registration. A genuine departure
closes the gate; `admitted()` depends on time, so the kit arms one timer at the
earliest grace end (`AvbEntityKit::admission_deadline()`) and re-evaluates it
then (`on_admission_timer`), noticing a grace expiry without an MSRP event. `listener_admitted()` reports the listener half alone (whatever
`gate_enabled_` says, Started/Stopped playing no part).

The declarations' deadlines (`TalkerDeclarations`, the probe window and the
re-declare hold) are a different matter: they run on the reactor's clock, built
from the time each hook is told, and the kit's `DeclarationTimer` compares them
with the time the reactor tells its tick, so a reactor on any `ClockFn` closes
a window on time. An AEM hook told no time (SET_STREAM_FORMAT, whose TSpec
re-declares the stream) asks for the re-declaration (`RefreshSet`) and the next
reactor pass runs it, told its time; a refresh is per stream, never the table.

## See also

- [`ACMP_STATE_MACHINES.md`](ACMP_STATE_MACHINES.md) - the IEEE 1722.1 ACMP state machines
- [`MILAN_LISTENER.md`](MILAN_LISTENER.md) - the listener side
- [`MRP_STATE_MACHINES.md`](MRP_STATE_MACHINES.md) - MSRP declarations
- [`SHAPER.md`](SHAPER.md) - the credit-based shaper behind the gate
