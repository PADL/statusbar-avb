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
For an AM824 stream it is `frames x channels` quadlets, block by block, each
the label (0x40, MBLA) and a 24-bit two's-complement sample, big-endian.

## IEC 61883-6 (AM824) streams

The IEC 61883-6 stream-format word (IEEE 1722-2016 Annex I,
`avtp::Am824StreamFormat`) is: octet 0 the subtype (0x00), octet 1 sf (1),
fmt (0x10) and r, octet 2 the FDF (evt[7:3], sfc[2:0]), octet 3 DBS, octet 4
b, nb, ut (up to DBS channels) and sc (synchronous clock), octet 5 the IEC 60958
label count, octet 6 the MBLA label count, octet 7 the MIDI (high nibble) and
SMPTE (low nibble) counts. `0x00a0020840000800` is 48 kHz, non-blocking, 8
MBLA; the MOTU 16A lists `0x00a00N0860000800` (nb, ut) on its inputs and
`0x00a00N0870000800` (nb, ut, sc) on its outputs.

`decode_stream_format` decodes as `StreamKind::am824` only what the kit
carries (`am824_format_supported`): AM824 data (evt 0), non-blocking, MBLA
only (DBS equal to the MBLA count, no IEC 60958, MIDI or SMPTE), 32 to 192
kHz, and 1, 2, 4, 6 or 8 channels. Any other 61883 word is `StreamKind::other`,
so SET_STREAM_FORMAT refuses it NOT_SUPPORTED and a model naming it opens no
slot. The `ut` and `sc` flags are read (`avtp::Am824StreamFormat`) but not
honoured: the stream carries exactly DBS channels, and an input refuses packets
with another DBS.

`transmit_am824_raw` sends one packet on the caller's sample-index grid: 6, 12
or 24 data blocks at 48, 96 or 192 kHz, the class A cadence AAF uses. It keeps
no DBC of its own: the DBC is the first sample index mod 256, so the
SYT_INTERVAL boundary (8, 16, 32 blocks) is on the index grid and a restart
lands on the same count. `pts_ns` is the presentation time of the first block;
the AVTP timestamp and the SYT carry that of the block whose DBC is a multiple
of SYT_INTERVAL (IEEE 1722-2016 Equation (7)), `pts_ns` plus that many integer
sample periods (`Am824StreamOutputContext::build_packet_header`, the same
arithmetic the receiver extrapolates with), tv set, 3 packets in 4 at each
rate; a packet without one has tv clear and SYT 0xFFFF. The FDF is the stream's
own rate. Both AM824 send paths set tu as the AAF ones do.

With a timed sink and no `StreamConsumeFn`, an AM824 input is not decoded to
float either (`am824_rx_packet`): the sink gets the quadlets as received and
the first block's presentation time, extrapolated by DBC from the last tv
packet. A packet whose DBS or SFC is not the input's current format is
discarded and counted in UNSUPPORTED_FORMAT; a DBC that does not follow the last
packet's is counted in the context's `dbc_discontinuities`, a statistic beside
`sequence_gaps` (no IEEE 1722.1 counter carries it).

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
stream's kind, at any rate: a stream's rate is set through its format. Milan
v1.2 Section 5.4.2.7 refuses it STREAM_IS_RUNNING on a bound input or a
streaming output. A format at another rate than the AUDIO_UNIT's is taken only
while no port map names the stream (an input's dynamic mappings, a static
AUDIO_MAP of either direction), else BAD_ARGUMENTS: Section 5.4.2.27 maps no
stream at another rate than its unit's onto a port without rate conversion, so
such a stream exists unmapped (Section 5.4.2.7 names BAD_ARGUMENTS for a format
the mappings forbid). A controller removes the mappings first. The entity's hook
(`set_on_stream_format_changed`) applies it to its data plane, and the kit
re-shapes the stream (its spec, its TX or RX slot at the format's rate, a
declared talker's TSpec at that rate's samples per frame). Without a hook only
the stream's current format is taken; an entity whose data plane runs at the
unit's rate refuses another (`TalkerStreams::transmit_if_due` feeds streams at
its media clock's rate; a stream at another rate is sent from the entity's own
clock with `transmit_aaf_raw`). `ListenerStreams` measures each input's media
lock at its own format's rate.

A talker format that raises every talker's TSpec bandwidth together (Milan
v1.2 Table 4.4, the shaper's arithmetic) past the class A budget, 75% of
`link_mbps()` (IEEE 802.1Q-2022 Clause 34.3.1; the shaper keeps the speed
current, 1000 Mbit/s until it does), is refused NO_RESOURCES. Every
talker counts, admitted or not: an output's format changes only while it does
not stream, so the check is the last chance to keep them all admissible.
Milan v1.2 Section 5.4.2.7 names no status for this. Listeners do not count.

SET_SAMPLING_RATE to the unit's current rate is a no-op SUCCESS; any other rate
is NOT_SUPPORTED, whether the AUDIO_UNIT lists it (the kit) or not (the storage
handler). The kit re-times no unit: Milan v1.2 Section 5.4.2.13 requires the
command per Audio Unit and Section 5.3.3.3 only that the list report the rates
the unit supports, so a unit lists one rate and keeps it, and a stream runs at
another rate through its own format (above). A model whose unit lists more is
answered honestly: the other rates are refused, never taken without the data
plane following. The unit's rate is not persisted.

`request_entity_name` serves SET_NAME for a host's own control socket: it is
applied with the same checks as the ATDECC command, every registered controller
is notified and the value is saved.

## Dynamic audio mappings

Every STREAM_PORT_INPUT without a static AUDIO_MAP keeps dynamic mappings
(nanoavb's ADD/REMOVE_AUDIO_MAPPINGS validate them against the model; Milan
v1.2 Sections 5.3.10, 5.4.2.26-28). The kit starts each such port k with its
default: STREAM_INPUT k's channels, in order, onto the port's cluster channels
in order, as far as both go (the identity a port-per-stream data plane already
routes); none for a port with no stream input of its index, and none while
stream k is at another rate than the unit's (Section 5.4.2.27). A stream leaves
the unit's rate only with no mappings (above); when it comes back, the port
stays empty until its owner restores them (a controller's ADD, or the entity's
`request_audio_mappings`).

Every STREAM_PORT_OUTPUT without a static AUDIO_MAP carries the kit's own
identity, built as the input default from STREAM_OUTPUT k, while stream k is at
the unit's rate, and none otherwise; the kit re-derives it after every format
change, told to the registered controllers as unsolicited
REMOVE/ADD_AUDIO_MAPPINGS. GET_AUDIO_MAP answers it; a controller's ADD or
REMOVE is NOT_SUPPORTED (Section 5.3.9.1). It is not persisted: it follows the
formats. A model's static output maps stay as they are and keep their streams
at the unit's rate. A controller's
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
successful SET_NAME, SET_STREAM_FORMAT, SET_CLOCK_SOURCE or
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
