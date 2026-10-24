<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->

# AVB entity kit - runtime behaviour

`AvbEntityKit` (`statusbar/avb_entity/avb_entity_kit.hpp`) runs an AVB entity
from its AEM blob: the stream tables, the talker and listener paths, admission
and the Talker Advertise declarations, the AEM commands a controller uses to
change formats, rates, offsets, names and mappings, and the persistent state.
`AvbEntityHost` is the control plane underneath it. This document records the
behaviour the headers state as contracts, and why.

## Reactor-side upkeep

`EntityUpkeep` does what the reactor side of `run_entity_main_loop` does, for a
host whose main thread is a `net::MessageReactor` rather than the runner's own
loop. It works on the `AvbEntityKit`, so any kit entity can use it.

- `attach()` drives the first link-up synchronously (so the supervisor leaves
  Down before any announce), installs a `net::LinkMonitor` for later link
  changes, and the gPTP grandmaster-changed callback (ADP gPTP info and
  `on_gptp_announce`).
- `tick()`, every reactor pass, drains the entity's ctl/media log channels (and
  any added) to a sink; detects a grandmaster regained under an unchanged
  identity and calls `on_gptp_announce`; fires the Init timeout
  (`GPTP_LOCK_TIMEOUT`) as `on_timeout`; re-evaluates talker admission
  (`poll_admission`: a Listener Ready grace window can expire with no MSRP
  event); and logs state changes on the ctl channel.

The grandmaster edge detection runs on the reactor thread so that the state
machine and MSRP stack are only ever driven from one thread.

## Stream limits and the TSpec

`MAX_ENTITY_STREAMS` bounds one direction's descriptors. A CRF stream is a
STREAM_OUTPUT (STREAM_INPUT) like any other and takes a slot in the same table,
so the bound is the sum: 16 audio + 1 CRF = 17. The split is the host's to
enforce; the kit composes any 17 it can serve. The blob may declare more (the
spec allows 65535); deriving specs from such a blob is an error rather than a
silent truncation. It sizes the stream tables (`StreamSpecs`, `TalkerStreams`,
`ListenerStreams` and its pending registrations), the transmit gate, MAAP's
stream list (one address per talker stream, CRF included: one block of up to
17), the shaper's admitted set and the tone generator's renders. Collaborators
sized elsewhere are checked against it in `avb_entity_kit.hpp`.

`srp_max_frame_size` follows Milan v1.2 Section 4.3.3.2, Table 4.4: the AVTPDU
header plus the payload plus `MILAN_TSPEC_SPARE_BYTES` (one byte, since the
PAAD's sampling clock may run a little faster than nominal), MaxIntervalFrames
1. AAF reserves exactly the class A samples per PDU the talker sends (AAF
PCM32 48 kHz N channels: 24*N + 24 + 1); CRF its timestamps (28 + 1 for one).
AM824, not a Milan format, reserves a spare sample instead: its talker may send
nominal+1 samples on a wake.

The raw TX render (`StreamRawRenderFn`) is the fast path alongside the float
`StreamRenderFn`: the payload is interleaved big-endian samples of the
stream's AAF format, `payload.size() == frames x channels x bytes per sample`.

## Profile

`AudioIoConfig::milan` selects the ATDECC profile. Unset takes the model's
(the blob's ENTITY profile symbol, aemxml's entity `"milan"`); a model that
says nothing is Milan. Only behaviour that could break a classic controller
follows it: under Milan, ACQUIRE_ENTITY answers NOT_SUPPORTED (Milan v1.2
Section 5.4.2.1).

## Talker admission

With `gate_talker_on_listener` (the default) each talker stream emits its AVTP
stream only when the stream is Started and an MSRP Listener Ready or Listener
Ready Failed is registered for it (Milan v1.2 Section 5.3.7.3), held across an
MRP LeaveAll re-registration blip by a short grace window. Listener Asking
Failed does not admit. ACMP connections play no part: a Milan listener binds
with PROBE_TX and makes none, and a classic listener declares Ready after
CONNECT_RX anyway. With no listener the talker stays silent. The CRF media
clock is a first-class stream and gates on its own reservation; it does not
follow the audio talkers. Setting the flag false streams unconditionally from
link-up with no reservation (bench and debug only).

## Talker gating and MAAP

Each talker stream gates independently on its own reservation; a CRF stream is
first-class and does not transmit merely because an audio stream is admitted.
On a MAAP conflict the TX gate closes until a new block is defended (the
handler is already re-probing; `on_acquired` re-opens it). The media thread's
slot keeps its MAC while gated; the control plane's goes to zero.

## Talker Advertise declarations

In the Milan profile a talker stream declares its Talker Advertise (Milan v1.2
Section 4.3.3.1) only while MAAP has its address and a PROBE_TX_COMMAND came
within 15 s or a Listener is registered for it; classic declares once the
address is valid. After a withdrawal, a different declaration waits two
LeaveAll periods. The kit's own timer runs the declarations at each deadline (a
probe window closing, a hold ending).

The admitted set changes after an MSRP listener declaration for a talker stream
is registered or withdrawn, after the Talker declarations are withdrawn, or
when `poll_admission()` sees a grace window expire or a stream Started or
Stopped. The host's `on_listener_ready` hook lands in `note_listener_ready`.

The presentation time offset (Milan v1.2 Section 5.3.7.6) is changed with
SET_STREAM_INFO (MSRP_ACC_LAT_VALID) or SET_MAX_TRANSIT_TIME while the stream
is not streaming; GET_STREAM_INFO and GET_MAX_TRANSIT_TIME report it. The kit
refuses a change no entity hook applies: it never reports an offset the stream
does not use.

## Presentation offset

The presentation-time offset is how far in the future the listener presents
the samples. It must exceed the worst-case talker plus network transit time or
the listener flags LATE_TIMESTAMP. The default 2 ms is Milan v1.2 Section
5.3.7.6's default for every stream output; the deterministic GPS-rate media
clock (`ptpclient::MediaClockGenerator`) owns this lead. The kit's per-output
value starts from the config; a controller may change it (SET_STREAM_INFO /
SET_MAX_TRANSIT_TIME) when the entity applies it
(`AvbEntityKit::set_on_presentation_offset_changed`).

## Class A VID changes

The Class A VID in use is adopted from the bridge's MSRP Domain, or restored
to the configured VID on link up (Milan v1.2 Section 4.2.7.2.1). The host then
joins the new VLAN (Milan v1.2 Section 4.3.2), moves every ACMP talker stream's
stream_vlan_id, lets the entity re-tag its stream frames
(`TalkerStreams::set_vlan_id`) and re-advertises the talker streams.

GET_AVB_INFO of interface 0 is notified asynchronously (Milan v1.2 Table 5.22)
when a field the table lists changes; the events that can change one call
`refresh_avb_info()`, and a mirrored device's updates notify themselves.

## Listener VLAN

`bind_listener_vlan` keeps the SR-class VID in use (configured, or adopted from
the MSRP Domain) on the interface's receive VLAN filter through every VID
change, until `stop()` unbinds it. It is a no-op when the blob declares no
stream inputs (a talker only sends tagged frames). `start()` calls it with its
own `net::vlan::VlanLinkClient` unless it was called first (tests pass a
recorder).

## Stream formats and the sampling rate

SET_STREAM_FORMAT takes any format the stream's descriptor lists of the
stream's kind at the kit's rate. Milan v1.2 Section 5.4.2.7 refuses it
STREAM_IS_RUNNING on a bound input or a streaming output. The entity's hook
(`set_on_stream_format_changed`) applies it to its data plane, and the kit
re-shapes the stream (its spec, its TX or RX slot, a declared talker's TSpec).
Without a hook only the stream's current format is taken.

SET_SAMPLING_RATE (AUDIO_UNIT 0, the kit's one media clock) re-times every AAF
stream's current format to the new rate, so it is taken only when each stream
lists its format at that rate, and refused STREAM_IS_RUNNING while any input is
bound or any output streams (each re-timing is a SET_STREAM_FORMAT that would
be). The entity's hook (`set_on_sampling_rate_changed`) stops whatever reads the
kit's streams (its media thread, the RX drain), calls `apply_sampling_rate()`,
rebuilds its data plane, and answers; a refusal after applying applies the old
rate back, and a hook that accepts without applying has it applied for it. The
storage handler records the rate once the hook answers. The streams re-timed
are notified after the command's response to every registered controller (the
requester too: it did not ask for them), each as if a controller had sent its
SET_STREAM_FORMAT (Milan v1.2 Section 5.4.5.2). Without a hook only the kit's
rate is taken. Both are persisted, rate first, then formats.

`request_sampling_rate` and `request_entity_name` serve the same changes for a
host's own control socket; they are applied with the same checks and hooks as
the ATDECC commands, every registered controller is notified and the value is
saved.

## Dynamic input audio mappings

Every STREAM_PORT_INPUT without a static AUDIO_MAP keeps dynamic mappings
(nanoavb's ADD/REMOVE_AUDIO_MAPPINGS validate them against the model; Milan
v1.2 Sections 5.3.10, 5.4.2.26-28). The kit starts each such port k with its
default: STREAM_INPUT k's channels, in order, onto the port's cluster channels
in order, as far as both go (the identity a port-per-stream data plane already
routes); none for a port with no stream input of its index. A controller's
change goes to the entity's hook, which routes it or refuses it, and is
persisted while it differs from the default (restored after the stream formats,
before the bindings). A SET_STREAM_FORMAT that would drop a mapped channel of a
stream input is refused BAD_ARGUMENTS (Section 5.4.2.7): the controller removes
those mappings first. A saved set the data plane cannot route once its hook is
installed falls back to the default.

`request_audio_mappings(port, add, remove)` is the same change made locally
(a narrower input format's lost channels, a host's routing socket): a REMOVE of
`remove` then an ADD of `add` under the controller's rules, all or nothing, one
hook call, refused ENTITY_LOCKED while a controller holds the lock (Sections
5.4.2.27-28); a change is notified as the REMOVE and ADD it amounts to and saved.

## Persistence

With `persistent_state_path` set, `start()` loads the file before the control
plane advertises and applies what it holds through the same validation a
controller's command gets (a saved value the model no longer takes is dropped
with a warning). From then on the kit rewrites it after every change: a
successful SET_NAME, SET_STREAM_FORMAT, SET_SAMPLING_RATE, SET_CLOCK_SOURCE or
presentation offset command, a listener binding made or removed, a
SET_SYSTEM_UNIQUE_ID or SET_MEDIA_CLOCK_REFERENCE_INFO. Nothing is written when
the file already says it. See [PERSISTENT_STATE.md](PERSISTENT_STATE.md).

## GET_STREAM_INFO

Talker (Milan v1.2 Section 5.4.2.10.2, Tables 5.11-5.12): STREAM_FORMAT_VALID
and MSRP_ACC_LAT_VALID always; the stream ID and VID always (valid while
declaring, undefined otherwise); the destination address while MAAP has one;
REGISTERING (flags_ex) while declaring and registering any Listener,
REGISTERING_FAILED while that Listener is Asking Failed; FAST_CONNECT,
SAVED_STATE, STREAMING_WAIT and BOUND 0; pbsta and acmpsta 0. The entity never
declares Talker Failed, so MSRP_FAILURE_VALID stays 0. Classic: CONNECTED from
the ACMP connection count. A Milan listener queries this to verify the stream
before sustaining a connection.

Milan listener (Milan v1.2 Section 5.4.2.10.1, Tables 5.9-5.10):
STREAM_FORMAT_VALID always; BOUND, FAST_CONNECT and SAVED_STATE while bound,
STREAMING_WAIT while bound and stopped; the stream ID, destination address and
VLAN ID (and their VALID flags) while settled, as the PROBE_TX_RESPONSE had
them; REGISTERING (flags_ex) and MSRP_ACC_LAT_VALID while registering a
matching Talker Advertise or Talker Failed, with its accumulated latency (the
internal ingress delay counted as 0); for a Talker Failed REGISTERING_FAILED,
MSRP_FAILURE_VALID and its bridge and code; pbsta and acmpsta.

## GET_COUNTERS

Stream outputs: Milan (v1.2 Section 5.4.2.25, Table 5.17) STREAM_START,
STREAM_STOP, MEDIA_RESET, TIMESTAMP_UNCERTAIN and FRAMES_TX at bit values
0x01-0x10 from the interval counts. Classic (IEEE 1722.1-2021 Clause
7.4.42.2.5): STREAM_START and STREAM_STOP as Milan's, then the running totals
of mr toggles (bit 3), tu PDUs (bit 4) and PDUs (FRAMES_TX, bit 7).

The host accounts for ENTITY (no counters beyond ENTITY_SPECIFIC, so none
valid), AVB_INTERFACE 0 (LINK_UP, LINK_DOWN, GPTP_GM_CHANGED: changes after the
first grandmaster, the one found at start not being a change) and CLOCK_DOMAIN
0 (LOCKED, UNLOCKED: the media clock follows gPTP, so its gPTP lock). Other
interfaces and domains the model has answer with none valid. Only
configuration 0 is served.

## Local ATDECC socket

Controllers on the same host (la_avdecc's local protocol interface,
ATDECCSwift's `UnixSocketPort`, `statusbar-atdecc-monitor --socket`) reach the
entity over an AF_UNIX datagram socket, without raw-socket privileges
(`nanoavb_local_port.hpp`).

## Notes on the kit's event wiring

- SET_STREAM_FORMAT notifications after a sampling-rate change carry the
  response body of a SET_STREAM_FORMAT: a controller such as la_avdecc takes a
  format change from an unsolicited SET_STREAM_FORMAT, not from a GET's.
- A stream streams only while declaring its Talker Advertise (Milan v1.2
  Section 5.3.7.3): a Listener still registered for a withdrawn stream (in its
  re-declare hold, or with the control plane down) opens nothing. When the
  stream declares again, the gate re-learns the listener state from the live
  registration, since a withdraw forgot it and a registration that stayed in
  place raises no fresh MSRP event.
- A settled Milan input joins its stream's VID with MVRP (Milan v1.2 Section
  4.4.1). The Milan sink settles once per probe and is still intact when it
  unsettles, so the join count stays balanced. The Milan MSRP Listener
  declaration is made only once the talker's attribute is registered.
- A binding change (bound, rebound, unbound, its controller or started state)
  is saved once the reactor pass that made it is done, after the ACMP response.
- Until a talker's address is valid the stream has no destination address:
  the configured static default must not reach a listener (CONNECT_TX and
  PROBE_TX answer TALKER_DEST_MAC_FAIL, GET_TX_STATE and GET_STREAM_INFO show
  zero), or the listener would join a group the talker never streams to. A
  stream also waits for its MAAP address before declaring, since a pre-MAAP
  destination would not match the address ACMP hands the listener.

## See also

- [`ENTITY_CONSTRUCTION_KIT.md`](ENTITY_CONSTRUCTION_KIT.md) - the kit's design and build order
- [`PERSISTENT_STATE.md`](PERSISTENT_STATE.md) - the state file
- [`MILAN_TALKER.md`](MILAN_TALKER.md), [`MILAN_LISTENER.md`](MILAN_LISTENER.md) - the Milan stream paths
- [`SHAPER.md`](SHAPER.md) - talker traffic shaping and the listener VLAN
- [`STREAM_PORTS.md`](STREAM_PORTS.md) - the frame transport
- [`NANOAVB_ARCHITECTURE.md`](NANOAVB_ARCHITECTURE.md) - the entity stack underneath
