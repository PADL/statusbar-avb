<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->

# Milan talker: connection management, declarations and the transmit gate

A Milan talker answers ACMP without connection state
(`nanoavb::NanoAvbMilanAcmpTalker`, [`MILAN_ACMP.md`](MILAN_ACMP.md)), declares a
stream's MSRP Talker Advertise only while someone wants it
(`nanoavb::MsrpHandler::set_talker_waits_for_listener`) and transmits only while a
Listener attribute admits it (`avb_entity::TalkerGate`). Milan v1.3 Sections 4.3.3.1,
5.3.7 and 5.5.4.

## Connection management

A Milan talker keeps no ACMP connection state (Milan v1.3 Section 5.5.2.7): it
learns whether anyone wants a stream from MSRP alone, and PROBE_TX_COMMAND
(CONNECT_TX_COMMAND renamed, Section 5.5.2.2) only queries a stream's parameters.
The responder and its tables are in [`MILAN_ACMP.md`](MILAN_ACMP.md). The kit wires
three things into it (`AvbEntityKit::wire_milan_talker_callbacks`): a probe goes to
the MSRP handler (`note_talker_probe`), the destination address is valid while MAAP
says so (else TALKER_DEST_MAC_FAIL), and REGISTERING_FAILED is the MSRP handler's.

## When a stream declares its Talker Advertise

The kit hands each stream's parameters to the MSRP handler (`talker_advertise`) when
the control plane comes up and again when they change. Under the Milan profile the
handler declares only while the stream's SRP parameters are valid (Section 4.3.3.1):

1. MAAP has allocated the destination address and reports no conflict
   (`set_talker_addresses_valid`), and
2. a PROBE_TX_COMMAND for the stream arrived in the last 15 s, or a Listener
   attribute for its stream ID is registered,

and leaves the stream `TalkerReservationState::Waiting` otherwise, so a stream nobody
asked for costs the network nothing. A probe re-hands a waiting stream's parameters
first, so the AccumulatedLatency it declares is the port's as it is then (IEEE
802.1Q-2022 Clause 35.2.2.8.6). A changed Talker Advertise of a declared stream ID
(a new MAAP address, the adopted Class A Domain's VID or priority) is held two
LeaveAll periods by the participant (IEEE 802.1Q-2022 Clause 35.2.2.8).

Condition 2's Listener arm is the MSRP registrar's: In, or Lv after a LeaveAll until
LeaveTime (5 s, Milan v1.3 Table 4.3) runs out. A Listener's Lv moves the registrar
to MT at once (Section 4.2.7.2.2), so the last Listener's Lv withdraws a stream whose
probe window is over at once, and a Listener lost without one is withdrawn LeaveTime
after the next LeaveAll received.

A classic (non-Milan) entity does not wait: every stream handed to the handler is
declared at once.

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
   within the gate's grace (`GRACE` for a classic entity; the kit sets none,
   `set_listener_grace(0)`, for a Milan one);
2. our Talker Advertise is declared. This is implied by (1): a listener
   declaration for our stream only arises against our Talker Advertise, and the
   gate closes at once when we withdraw it (`notify_talker_withdrawn`);
3. the stream is Started (default true; stops a stream without tearing down its
   reservation).

4. an IEEE 1722.1 stream has an ACMP connection (`note_acmp_connections`). A Milan
   talker tracks no connections, so the kit turns this off under the Milan profile
   (`set_needs_acmp_connection(false)`), as does a talker without an ATDECC entity.

Streams are matched by
the ACMP stream_id, the same value MSRP advertises. The match is a StreamID to
stream index lookup given to the gate (`TalkerGate::StreamIndexFn`); the kit's
is its ACMP talker table, and a talker without an ATDECC entity (one driving an
`nanoavb::SrpPort`) gives its own.

### One clock, one stamp, one predicate

The gate's clock is `CLOCK_MONOTONIC_RAW` (`TalkerGate::RawTimePoint`,
`realtime::MonotonicRawClock`): the clock a media wake is told on
(`TimerWakeInfo::actual_raw`), and the one `net::rx_clock_now()` reads. The
reactor is told its own time (`sm::TimePoint`, the reactor's `ClockFn`) and has
no line to RAW, so the loop reads both clocks once per cycle and tells the kit the
pair (`AvbEntityKit::tell_cycle_time`, `entity_reactor_clock`): each gate input
of the cycle is stamped with that RAW instant, and `end_cycle()` carries it
forward to the admission deadline (`raw_at()`). Neither the gate nor the kit
reads a clock.

Per stream the gate holds one stamp, `ready_raw_ns_`: the last RAW instant the
Listener declaration was known registered. `notify_listener_ready` writes it on a
registration and on a withdrawal (the moment the registration was last known),
then the `listener_ready_` flag with release; `notify_talker_withdrawn` zeroes the
stamp (no grace) before clearing the flag. A reader loads the flag with acquire
and then the stamp, so a withdrawal never pairs an old stamp with a cleared flag.

`admitted(idx, now)` is the whole predicate: gating off, or Started, connected
(when connections count) and (`listener_ready_` or `now < stamp + grace`; with no
grace the flag alone). `should_transmit(idx, now)` is
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

The grace window rides out a Listener re-declaring after a LeaveAll, which must
not chop the stream. The registrar already does that: a LeaveAll leaves the
Listener registered (Lv) for LeaveTime, so the grace only adds to it. A Milan
entity's LeaveTime is 5 s and its MSRP Lv immediate (Milan v1.3 Section
4.2.7.2.2), so its gate has no grace: it closes at the last Listener's Lv, or
LeaveTime after the next LeaveAll received, and SET_STREAM_FORMAT
stops answering STREAM_IS_RUNNING then. A classic entity keeps `GRACE` (5 s)
over IEEE 802.1Q's 1 s LeaveTime. Only a registration or a withdrawal moves the
stamp, so a withdrawn declaration closes the gate the grace later and the
window cannot be extended without a fresh registration. A genuine departure
closes the gate; `admitted()` depends on time, so the kit arms one timer at the
earliest grace end (`AvbEntityKit::admission_deadline()`) and re-evaluates it
then (`end_cycle()`), noticing a grace expiry without an MSRP event. `listener_admitted()` reports the listener half alone (whatever
`gate_enabled_` says, Started/Stopped playing no part).

## See also

- [`ACMP_STATE_MACHINES.md`](ACMP_STATE_MACHINES.md) - the IEEE 1722.1 ACMP state machines
- [`MILAN_ACMP.md`](MILAN_ACMP.md) - the Milan ACMP talker and listener
- [`MRP_STATE_MACHINES.md`](MRP_STATE_MACHINES.md) - MSRP declarations
- [`SHAPER.md`](SHAPER.md) - the credit-based shaper behind the gate
