# AVB Entity Construction Kit — investigation and design direction

Status: investigation report (2026-07-14). No code has changed; this document
records the current-state survey and the proposed build order for evolving
`statusbar/avb_entity` + `python/aemxml` into an "Entity Construction Kit":
a developer writes a JSON entity model plus a handful of lambdas
(`inplace_function` hooks) and gets a powerful, spec-correct AVB entity —
stream input/output combinations, tone/noise sources, control types, mixers,
signal selectors, CRF inputs and outputs, DSP insertion, and media-clock
synchronization — with minimal C++.

The survey below was produced by four parallel deep-reads of the tree:
entity assembly (`avb_entity`), media-clock architecture (`ptpclient`,
`avtp`, `gptp`), authoring-vs-runtime coverage (`python/aemxml` vs
`nanoavb`), and the listener/RX path.

## Executive summary

The codebase is half a kit already — split down the middle:

- The **control plane** (discovery, enumeration, blob-driven entity model,
  symbol table, AECP handler architecture) is kit-shaped and needs mostly
  thin wrappers.
- The **data plane** (stream topology, formats, audio sources, clocking) is
  compile-time C++ duplicated per entity: a new entity costs roughly
  1,000–1,800 lines (a 450–900-line entity class plus a 550–950-line
  `_tool.cpp`, both largely copy-paste) plus a JSON model that must
  *manually agree* with the C++ in six places.

Two suspected gaps are confirmed exactly:

- **CRF input is completely unwired.** `avtp::CrfStreamInputContext`
  (`avtp/avtp_crf_stream_input.hpp:28`) parses everything, but its only
  consumers are a test and a bench tool. `ListenerStreams::on_stream_rx_frame`
  has no subtype-0x04 branch (`avb_entity_listener_streams.cpp:54,102`), no
  entity authors a CRF STREAM_INPUT, and no CRF multicast group is joined.
- **Media-clock synchronization from incoming AM824/AAF/CRF does not
  exist.** All entities free-run on gPTP. Decoded RX presentation timestamps
  are discarded (the callback parameter is literally named `/*pts*/`,
  `avb_entity_listener_streams.cpp:79,124`). The CLOCK_SOURCE/CLOCK_DOMAIN
  descriptors bind to nothing at runtime: SET/GET_CLOCK_SOURCE return
  NOT_IMPLEMENTED (`nanoavb/nanoavb_entity.cpp:192`), and the audio_io blob
  even advertises an INPUT_STREAM clock source the runtime cannot honor.

For nearly every gap the right low-level brick already exists and is
production-quality; the kit is mostly a composition-and-hook-surface
project, not an algorithms project. The two genuine greenfield items are
phase-steering the media clock and ASRC.

## What is already kit-shaped

| Asset | State |
|---|---|
| `AvbEntityHost` (`avb_entity_host.hpp:85`) | Owns all 7 protocol SMs, lifecycle, SPSC logging; parameterized by blob + 3 stream-count integers; auto-wires IDENTIFY by scanning blob CONTROL descriptors (`avb_entity_host.cpp:45-70`). Serves GET_AVB_INFO and the AVB_INTERFACE runtime gPTP fields from observed state, or — `mirror_avb_info_from(target, controller)` — mirrored from another entity over 1722.1 (`avb_entity_avb_info_mirror.hpp`: one-target ATDECC controller reading its AVB_INTERFACE descriptors, registering for unsolicited notifications, polling GET_AVB_INFO). Answers GET_AS_PATH (IEEE 1722.1-2021 7.4.41) with the gPTP Announces' path trace and the clock identity AVB_INTERFACE reports, and notifies every change of path, a lost grandmaster's included; the notification is installed apart from `GptpAnnounceCallbacks`, so a tool replacing those keeps it. Control-plane complete. |
| aemxml pipeline | `model.py`/`flatten.py` cover **all 43** IEEE 1722.1-2021 descriptor types, 1:1 with the C++ structs (`atdecc_aem_descriptor.hpp`); symbol table; `${var}` expansion (json2bin `--set`); JSON schema; byte-exact wire tests. Only the JSON front-end is narrower (see gap table). |
| Handler hooks | Symbol-keyed generic `on_set/get_descriptor_value` (`nanoavb_aem_entity_handler.hpp:284-298`), unsolicited-notification fan-out, `apply_local_descriptor_value` for entity-originated changes. Foundation for control binding; used only by IDENTIFY today. |
| SIGNAL_SELECTOR + MATRIX runtimes | Full built-ins with veto/apply `inplace_function` callbacks and `matrix_cell()` readback (`nanoavb_aem_descriptor_storage_handler.hpp:354-437`). The template to replicate for CONTROL and MIXER. |
| `MediaClockGenerator` (`ptpclient_media_clock.hpp:47`) | Phase-accumulator clock taking a per-tick rate `r` from anywhere — the correct abstraction for slaving. CRF output timestamps come from the same accumulator as audio (`avb_entity_talker_streams.cpp:93-181`), so TX coherence is by construction. |
| `KalmanRatioTracker` (`ptpclient_freq_ratio.hpp:132`) | 3-state phase/freq/drift estimator with outlier gating — exactly the filter CRF clock recovery needs (today it eats gPTP−GPS pairs; feed it CRF−gPTP instead). |
| Depacketizers | AAF int16/24/32/f32 any-rate (`avtp_aaf_stream_input.hpp:37`), AM824 incl. an unused MIDI/SMPTE-capable variant (`avtp_am824_stream_input.hpp:150`), CRF complete-but-unwired. All reconstruct full 64-bit PTS. |
| StereoIO playout pattern | PTS-stamped SPSC pipe + consume-when-due against the local media clock (`avb_entity_stereo_io.cpp:464-483,540-572`) — the one real time-aligned renderer; generalize for the kit listener. |
| Diagnostics | STREAM_INPUT counters (media lock, seq, late/early — `avb_entity_stream_counters.cpp`), TX pcap recorder (`tx_pcap_recorder.hpp`), per-stream MSRP (Listener Ready / Ready Failed) talker gate. |

## Gap map by kit feature

### Stream input/output combinations — the central gap

- `TalkerStreams` has exactly three *named* slots (one AM824, one AAF, one
  CRF — `avb_entity_talker_streams.hpp:110-115`); `ListenerStreams` exactly
  two fixed inputs (`avb_entity_listener_streams.hpp:61-62`); `TalkerGate`
  has `STREAM_COUNT = 3` compile-time (`avb_entity_talker_gate.hpp:56`);
  host stream counts are retyped integers (`avb_entity_audio_io.cpp:144`).
- ToneGenerator needs a C++ `StreamSet` enum **and three JSON files** to
  offer three stream combinations — the clearest symptom.
- Formats/rates are `static constexpr` per entity class
  (`avb_entity_audio_io.hpp:86,114-117`) and must manually match the blob;
  `create()` never validates the agreement.
- MSRP TSpec math and MAAP block offsets are duplicated per entity and
  derive from the C++ constants rather than the descriptor `current_format`.

Fix: derive a `StreamSpec[] {kind, index, format}` from the blob's
STREAM_INPUT/OUTPUT `current_format` words; counts, gate size, TSpec math,
and MAAP offsets follow. Dissolves the `StreamSet` enum and most blob/C++
duplication.

### Tone / noise generators (audio sources)

- Sources are inlined oscillator banks in each entity's `process_audio`
  (`avb_entity_tone_generator.cpp:492-495`, `avb_entity_audio_io.cpp:673-684`).
- The only hook (`set_audio_callback`, AudioIO/StereoIO only) runs *after*
  the sine is computed and lacks PTS/sample-index arguments.
- `LogSweepGenerator` exists but is wired only into the WAN tunnel ingest.

Fix: per-stream `render(stream, span<float>, frames, first_index, pts)`
lambda with a default source library (tone/noise/sweep/silence/file).

### Control types

- Authoring: good — all LINEAR_* value types, numeric SELECTOR_*, UTF8,
  ~30 standard control_type names, read_only flags, `${var}` expansion.
  Gaps: ARRAY_*/BODE_PLOT/SMPTE/GPTP_TIME only as raw hex; controls cannot
  be attached below configuration level (AUDIO_UNIT / stream-port /
  external-port `controls` keys are not parsed although the dataclasses
  have the fields).
- Runtime: a JSON-authored GAIN control enumerates fine but answers
  NOT_IMPLEMENTED to SET/GET_CONTROL — only IDENTIFY has a built-in
  (`nanoavb_aem_descriptor_storage_handler.hpp:289-311`).
  INCREMENT/DECREMENT_CONTROL unimplemented.

Fix: `on_control(symbol, setter, getter)` registry layered on the existing
symbol-keyed hooks, plus a generic fallback that stores/serves/clamps values
using the blob's own min/max/step.

### Mixers — the fully missing quadrant

Not JSON-authorable, no runtime dispatch, no state. The MATRIX built-in
(in-RAM grid, Clause 7.4.33/34 region writes, veto callback) is the direct
pattern to clone.

### Audio selectors

Best-covered feature: SIGNAL_SELECTOR is authorable and has a complete
runtime with veto/apply callbacks. Missing: an example model, and the
audio-routing *consequence* (nothing connects a selector change to which
samples flow where).

### CRF inputs

Parser complete, wholly unwired (details in executive summary). Wiring
needs: a CRF branch in the RX dispatch, a CRF STREAM_INPUT descriptor
convention (no audio channels), multicast join on connect, STREAM_INPUT
counters. Prerequisite for clock recovery.

### Media clock synchronization from incoming streams

Build path (each step small except the last two):

1. CRF input wiring (above).
2. `CrfClockRecovery` glue: feed `KalmanRatioTracker` with
   (CRF_timestamp − local_gPTP) pairs; its `r` becomes
   remote_media/local_gPTP; `filtered_offset_ns` provides phase. Threading
   via the existing `AtomicTripleBuffer`/seqlock patterns
   (`avb_entity_audio_io.hpp:299`).
3. Frequency slaving: pass recovered `r` into
   `MediaClockGenerator::advance` — the GPS tracker already proves this
   plumbing (`avb_entity_audio_io.cpp:667,763-804`).
4. Phase steering (Milan-style presentation-phase lock): small extension to
   `MediaClockGenerator` (anchor/offset steering) — greenfield, localized.
5. Runtime ClockDomain object owning {source selection, generator,
   r-provider}, streams referencing it by clock_domain_index; SET/GET_
   CLOCK_SOURCE dispatch cases + unsolicited notifications (clone the
   SET_CONTROL pattern, `nanoavb_entity.cpp:339-341`).

Same recipe applies with AAF/AM824 PTS as the reference. Hardware
constraint: one PHC per NIC and it belongs to gPTP (`GPTP_LINUX_PHC.md`,
passthrough ops) — recovered clocks must remain software rates on top of
it, which is exactly what the `r` architecture provides. **ASRC is pure
greenfield** (no resampler exists in avb/ or core/); without it,
cross-domain audio needs drop/insert concealment (`udptun_audio_egress.hpp`
silence-fill is the only precedent).

### DSP processing

- TX insertion point exists (post-source, pre-serialization;
  `avb_entity_audio_io.cpp:728-730`) but is post-hoc, global, and lacks
  timing arguments.
- RX side is worse: sinks receive **raw wire bytes** (undecoded MBLA/int32,
  no PTS) via `StreamRxAudioSink::on_listener_audio`
  (`avb_entity_stream_rx_sink.hpp:35-48`) even though decoded per-channel
  floats with PTS are computed and discarded two lines earlier
  (`avb_entity_listener_streams.cpp:79-81,124-126`).

### Additional findings (kit-relevant, discovered en route)

- `SET_STREAM_FORMAT`, `SET_SAMPLING_RATE`, `START/STOP_STREAMING`,
  `SET_CONFIGURATION` are not dispatched entity-side (the controller side
  can *send* them all — asymmetric, `nanoavb_controller.cpp:321-358`).
- Several serve paths hardcode configuration 0 (GET_STREAM_FORMAT
  `nanoavb_entity.cpp:378`, GET_SAMPLING_RATE `:412`, value dispatch
  `nanoavb_aem_entity_model.cpp:256,282`, `wire_identify_control`), so
  multi-config entities — fully authorable — would present inconsistent
  state.
- `GET_STREAM_INFO` on STREAM_INPUT returns false — controllers cannot read
  what an input is connected to (`avb_entity_audio_io.cpp:849-851`).
- **No listener fast-connect / saved-state**: connections are purely
  controller-driven and evaporate on entity restart. This is precisely the
  Galaxy half-open deadlock observed in the field on 2026-07-13 (Q2 kept
  MSRP listener-Ready attached while ACMP state was gone after a service
  restart; the gate never opened until a manual reconnect). A kit listener
  with persisted bindings + fast-connect fixes a real operational pain.
- MSRP `listener_asking_failed` exists but is never used; no local
  audio-device (ALSA) render path exists — RX audio terminates in the WAN
  tunnel or in counters.
- `nanoavb_listener_engine_sm.hpp` (Off/Listening/Syncing/Playing/Muted)
  exists as a tested skeleton but is instantiated by no entity — a natural
  home for the kit listener lifecycle.

## Authoring coverage table (JSON front-end vs full IR)

`model.py` + `flatten.py` serialize every descriptor type; the JSON reader
and schema support:

- ENTITY, CONFIGURATION (arrays), AUDIO_UNIT (+rates), STREAM_PORT_IN/OUT,
  AUDIO_CLUSTER (MBLA only), AUDIO_MAP (one per port), STREAM_IN/OUT,
  JACK_IN/OUT, AVB_INTERFACE, CLOCK_SOURCE, CLOCK_DOMAIN, EXTERNAL_PORT
  (no example), CONTROL, SIGNAL_SELECTOR, MATRIX (+inline MATRIX_SIGNAL),
  CONTROL_BLOCK, LOCALE/STRINGS (incl. dict-form localized strings).

Not yet JSON-authorable (IR + wire support already exist): MIXER,
SIGNAL_SPLITTER/COMBINER/DEMULTIPLEXER/MULTIPLEXER/TRANSCODER,
MEMORY_OBJECT, VIDEO_UNIT/SENSOR_UNIT (+clusters/maps),
INTERNAL_PORT_IN/OUT, TIMING, PTP_INSTANCE, PTP_PORT. Partial gaps:
controls on units/ports, multiple audio maps per port, non-MBLA cluster
formats, stream redundancy/backup fields.

## Design principle: the code is a menu, the model is the selection

Symbol-keyed registrations (controls, stream render/consume bindings,
selector/matrix hooks) must be **legal and inert when the blob does not
declare the symbol**. One binary can then register every capability it
implements — dozens of controls, alternate stream renderers, DSP options —
and each deployment ships a minimal JSON model that activates only the
subset it wants. Concretely:

- `kit.on_control("gain", ...)` / `kit.render("aux_out", ...)` for a symbol
  absent from the loaded blob is NOT an error: the binding is recorded and
  simply never fires. Binding resolution is lazy (at dispatch/start time,
  keyed by the blob's symbol table), never an eager create-time rejection.
- The reverse direction is tolerant too: a blob symbol with no registered
  callback falls back to generic built-in handling (serve/store/clamp from
  the blob's own value_details) rather than NOT_IMPLEMENTED, so a minimal
  program still enumerates correctly.
- Diagnostics, not errors: at start the kit logs the registrations left
  unbound ("registered but not in model: aux_out, phantom_1 ...") and the
  model symbols left ungoverned, at info level, so typos are findable
  without breaking the menu/selection pattern.
- The existing plumbing already leans this way — symbols are CRC32 codes of
  authored names (`flatten.py symbol_to_code`), `DescriptorId.symbol` is
  attached at dispatch time from the blob's symbol table, and callbacks
  registered for absent symbols naturally never match. The rule here is a
  constraint on future API layers: do not add eager symbol validation that
  would reject a registration merely because the current model omits it.

Validation stays loud where it protects correctness (stream format vs
cluster mismatch, unknown format words, over-capacity tables) — the
menu/selection tolerance applies to *optional capability bindings*, not to
shape contradictions.

## Target developer experience

```cpp
// ~40 lines instead of ~1,500
auto kit = AvbEntityKit::from_blob("mixer8.aem");        // topology, formats, counts,
                                                          // controls, clocking — all derived
// Registrations for symbols the model omits are recorded but inert — the
// same binary serves maximal and minimal models alike.
kit.render("main_out", [&](AudioBlock b) { synth.fill(b); });       // per-stream TX, pts/first_index
kit.consume("aes_in", [&](AudioBlockConst b) { recorder.push(b); });// decoded floats + pts
kit.on_control("gain",  [&](auto v) { gain.store(v.as<float>()); });
kit.on_selector("clock_pick", ...);                       // runtime exists, needs surfacing
kit.clock_domain(0).slave_to("crf_in");                   // recovered-rate media clock
return run_avb_entity(kit, argc, argv);                   // shared main loop, args, logging, timers
```

Every line maps onto an existing seam; nothing requires re-architecting.

## Suggested build order (dependencies, not schedule)

1. **Blob-driven N-stream composition** — `StreamSpec[]` from STREAM
   descriptors, create-time shape validation, TSpec-from-format. Dissolves
   the `StreamSet` enum, the index constants, and most blob/C++
   duplication. Everything else stacks on this.
2. **Per-stream render/consume lambdas** with PTS + decoded-float RX
   delivery + default source library + generalized StereoIO playout pipe.
3. **Clocking** — CRF input wiring → CRF/PTS clock recovery → media-timer
   slaving → runtime ClockDomain honoring CLOCK_SOURCE + SET/GET_
   CLOCK_SOURCE dispatch.
4. **Control surface** — `on_control` registry over the existing symbol
   hooks, generic value-storage fallback, MIXER (clone the MATRIX pattern,
   add JSON authoring), controls-on-units/ports in JSON.
5. **Lifecycle** — shared `run_avb_entity` main loop (cuts each tool to
   ~100 lines), SET_STREAM_FORMAT / SET_SAMPLING_RATE /
   START/STOP_STREAMING, SET_CONFIGURATION + current-config plumbing,
   listener saved-bindings/fast-connect, GET_STREAM_INFO for inputs,
   AskingFailed.

Phases 1+2 are the bulk of the payoff; 3 is the CRF/media-clock feature
set; 4 unlocks mixers and DSP controls; 5 removes operational pain observed
in the field.

## Profiles: classic and milan13

IEEE 1722.1-2021 and Milan v1.3 disagree in a few places, so the entity kit
answers to one of them at a time: `atdecc.profile = "classic"` (1722.1 as
written) or `"milan13"`. Left empty, the default, the model decides: a Milan
model (`entity.milan = true`) answers to Milan v1.3, any other to 1722.1 as
written; an explicit value wins either way. The AEM handler
(`AemCommandHandler::set_profile`), the MSRP handler and connection
management take the choice from the kit (`AvbEntityKit::milan_profile`).
Everything not listed here is the same under both.

| Point | classic (1722.1-2021) | milan13 (Milan v1.3) |
|---|---|---|
| ACQUIRE_ENTITY | granted for the ENTITY descriptor (7.4.1); while another controller holds it, the holder is sent CONTROLLER_AVAILABLE and the contender granted the entity only if no answer with that sequence id comes | NOT_SUPPORTED, never granted (5.4.2.1) |
| A SET to the value already in force | applied and notified: 7.5.2 notifies every SET answered SUCCESS | answered SUCCESS, not applied, not notified (5.4.5.3) |
| A registered controller that falls silent | kept until it deregisters or departs (ADP) | sent CONTROLLER_AVAILABLE 30 to 60 s after its last AECP command, AEM or MVU; when neither that nor its one retry (9.3.6) is answered, deregistered and told so alone (5.4.5.4) |
| A controller's DEREGISTER_UNSOLICITED_NOTIFICATION | the remaining registered controllers are notified (7.5.2 lists the command) | nobody is told: each copy would carry the recipient's own controller_entity_id (5.4.5.1) and read as its own deregistration; Table 5.20 sends one only to the controller removed |
| Changes by non-ATDECC means while locked (front panel, `set_local_identify`, `apply_local_descriptor_value`) | allowed | refused ENTITY_LOCKED (5.3.4.1, 5.4.2.17); `AemCommandHandler::local_change_refusal()` states the rule, for a host to ask before a change of its own |
| MSRP Registrar receiving Lv in IN | starts the leavetimer (802.1Q Table 10-4) | goes straight to MT (4.2.7.2.2) |
| MRP timers (MSRP and MVRP) | 802.1Q Table 10-7: JoinTime 200 ms, LeaveTime 1000 ms, LeaveAll 10 to 15 s, periodic 1 s | Milan Table 4.3: the same with LeaveTime 5000 ms (4.2.7.1.1) |
| Connection management | IEEE 1722.1-2021 clause 8: the listener relays a controller's connect to the talker, the talker keeps a list of its listeners. `NanoAvbAcmpListener`, `NanoAvbAcmpTalker` | Milan v1.3 section 5.5, a separate implementation (`MILAN_ACMP.md`): a bind is saved and answered at once, the listener then probes its talker and follows the talker's SRP attribute by itself; the talker answers probes and keeps nothing. `NanoAvbMilanAcmpListener`, `NanoAvbMilanAcmpTalker`, chosen at start-up by `NanoAvbComponents::use_milan_acmp` |
| A bind or connect from a controller other than the one holding the lock | accepted: ACMP does not ask | refused CONTROLLER_NOT_AUTHORIZED (5.5.2.4, 5.5.2.5) |
| SET_STREAM_FORMAT on a stream input in use | STREAM_IS_RUNNING while the input is connected (7.4.9.2) | STREAM_IS_RUNNING while the input's sink is bound, settled or not (5.4.2.7) |
| START_STREAMING and STOP_STREAMING on a stream input | the input takes or discards its frames (7.4.35, 7.4.36); the state is not saved | the started/stopped state of the input's sink (5.3.8.7), saved with its binding and also set by a bind's STREAMING_WAIT; no effect on an input that is not bound. A stopped input discards its frames and keeps its reservation |
| START_STREAMING and STOP_STREAMING on a stream output | the talker's stream is stopped and started; GET_STREAM_INFO reports STREAMING_WAIT (7.4.35, 7.4.36, Table 7-145) | NOT_SUPPORTED, nothing changed: an output cannot be stopped (5.4.2.19, 5.4.2.20, 5.3.7.3) |
| GET_STREAM_INFO response (`AemCommandHandler::stream_info_length`) | IEEE 1722.1-2013's 48 octets, which older controllers expect | IEEE 1722.1-2021's 84 (7.4.15.1, 5.4.2.10): the IP fields after the 48 are zero, the streams being AVTP over Ethernet |
| BIND_STREAM, UNBIND_STREAM and GET_STREAM_INPUT_INFO_EX, on a Milan model (`entity.milan`) | NOT_IMPLEMENTED, and MVU_BINDING clear in GET_MILAN_INFO | answered by the stream inputs' sinks (5.4.4.6 to 5.4.4.8), a bind from a controller the lock shuts out ENTITY_LOCKED; MVU_BINDING set |
| When a talker stream is put on the wire (`stream.gate_talker_on_listener`) | an ACMP connection, the stream started and MSRP Listener Ready, held five seconds past the withdrawal of its Listener Ready | the stream started and MSRP Listener Ready, with no grace: a Milan talker has no connections (5.5.4.1) and hears of no disconnect |
| Which Talker attribute a stream input's Listener declaration follows | any of the stream's StreamID (IEEE 802.1Q 35.1.2.2) | the one on the destination address and VLAN the talker's PROBE_TX_RESPONSE gave (5.3.8.5) |
| `listener.bindings` | off when empty; set, each successful connect is remembered and fast-connected again after a restart (file form v1) | the binding parameters of 5.3.8.3, written at every bind and unbind before the controller is answered, and each saved sink starts in PRB_W_AVAIL (file form v2; either form is read). Empty, they go to `listener-bindings` in the entity's state directory (below); with no state directory either, they last as long as the process |
| When a stream output's Talker attribute is declared | once the entity is ready and its stream addresses are final | only while a listener wants the stream: a PROBE_TX_COMMAND for it arrived in the last 15 s, or a Listener attribute for its StreamID is registered on the port; withdrawn when neither holds, and while MAAP reports the address lost (4.3.3.1). `MsrpHandler::set_talker_waits_for_listener` |
| Talker presentation offset when `presentation_offset_ns` is 0 (the default) | 1 ms | 2 ms (5.3.7.6); a value other than 0 is used as given under either |
| A Class A Domain registered from the wire with other parameters | ours is kept (802.1Q) | adopted: re-declared (a talker's Talker Advertise two LeaveAll periods later, 802.1Q 35.2.2.8), the stream frames carry its VLAN and priority, MVRP follows, GET_AVB_INFO is notified (4.2.7.2.1); a VID outside 1 to 4094 or a priority above 7 (802.1Q Table 9-2, 9-3) is left alone and the Domain in force kept |

### Stream counters

A stream input counts each AVTPDU it receives (`StreamInputCounters`, on
the RX thread), an audio input and a CRF media clock input alike (Milan
v1.3 5.3.8.10 asks Table 5.6 "for each Stream Input"). What a CRF AVTPDU
gives the counters:

| Counter | From a CRF AVTPDU |
|---|---|
| FRAMES_RX | every AVTPDU on the stream, a refused one too |
| SEQ_NUM_MISMATCH, MEDIA_RESET, TIMESTAMP_UNCERTAIN | sequence_num, mr and tu of its header |
| UNSUPPORTED_FORMAT | its type, base frequency, pull, timestamp interval or number of timestamps is not the input's format's, or the frame is too short for them. Such an AVTPDU reaches no consumer: a clock of another format is not this input's clock |
| TIMESTAMP_VALID | each accepted one: a CRF AVTPDU always carries its timestamps |
| LATE_TIMESTAMP, EARLY_TIMESTAMP | its first timestamp against the receive time, as an AAF AVTPDU's avtp_timestamp. A late one is counted and still feeds the clock recovery |
| MEDIA_LOCKED, MEDIA_UNLOCKED | the steady step between first timestamps, judged at the stream's rate: timestamps a PDU times the timestamp interval over the base frequency, 2 ms for Milan's format whatever the entity's sampling rate |

`AvbEntityKit::end_cycle(now_ns)` ends an
observation interval on the reactor thread once `COUNTERS_INTERVAL_NS`,
half a second, has passed since the last: `StreamInputIntervals::fold()`
makes the Milan values from the running counts, and an input still locked
that received nothing for the whole interval has lost its stream
(MEDIA_UNLOCKED and STREAM_INTERRUPTED, but not for an input stopped by
STOP_STREAMING) and starts its next run afresh. An interval ends at the
first reactor cycle after its half second is up, so it lasts half a
second and that cycle's wake-up latency: under the second that Milan
v1.3 Tables 5.4 and 5.6 allow, with room to spare. Under the Milan
profile each stream descriptor whose values changed is notified with an
unsolicited GET_COUNTERS at every second end of an interval
(`COUNTERS_NOTIFY_FOLDS`), so two notifications of one descriptor are
never less than a second apart (Table 5.20). An IEEE 1722.1 entity
notifies none: a controller reads its counters.
Under the Milan profile GET_COUNTERS answers MEDIA_LOCKED,
MEDIA_UNLOCKED and STREAM_INTERRUPTED as they stand and the interval
counters as of the last fold; a bind's reset is answered at once and
notified at the end of its interval. The entity runner calls
`end_cycle()` once each reactor cycle; an entity with a loop of its own
does the same and bounds its wait by `poll_timeout_ms()`, which counts
the end of the interval among its deadlines. A loop that never calls it
ends no interval: a Milan input then answers its event counters and zero
for every interval counter, and nothing is notified.

A stream output counts what its sender puts on the wire
(`TalkerTxCounters`, kept by `TalkerStreams` per STREAM_OUTPUT index):
the AVTPDUs, those with tu set, the mr toggles among them, and each start
and stop of the stream. The stream streams while its gate is open and it
is not stopped, as the media thread finds at each tick
(`transmit_if_due`), and the end of a run (`close_tx`) is a stop. The
sender is the one writer, so the counters are atomics written with a load
and a store, read by the reactor thread. GET_COUNTERS reports them at the
bits of IEEE 1722.1-2021 Table 7-158: STREAM_START, STREAM_STOP,
MEDIA_RESET, TIMESTAMP_UNCERTAIN and FRAMES_TX. Each fold makes the Milan
values of every output too (`StreamOutputIntervals`), notified as an
input's are; under the Milan profile GET_COUNTERS answers STREAM_START and
STREAM_STOP as they stand and the interval counters as of the last fold,
zero once the stream has started since.

A Milan output also reports the signal presence of its channels (Milan
v1.3 Section 5.3.7.7.1) once the entity says what it is:
`TalkerStreams::set_signal_presence(index, channels)`, bit k for channel
k, from whichever thread knows the signal (a store, safe on a real-time
thread). GET_COUNTERS answers the latest report in ENTITY_SPECIFIC_9
(channels 0 to 31) and ENTITY_SPECIFIC_10 (32 to 59), channel 0 at the
most significant bit, the bits past the stream's channels dropped. The
kit compares it at each fold, and a change is notified as any counter's
is. Until the first report both stay invalid. An entity that
reports it says so in GET_MILAN_INFO: `milan.features_flags` with
TALKER_SIGNAL_PRESENCE (Table 5.17). The kit itself measures no signal.

### tu and mr on the talker's AVTPDUs

Every AVTPDU the talker sends, AM824, AAF and CRF alike, carries tu (IEEE
1722-2016 Clause 4.4.4.7, Milan v1.3 Section 4.3.5.2) while no gPTP
grandmaster is in force, and for 0.25 s after one comes into force
(`TalkerStreams::TU_AFTER_GM_CHANGE_NS`, Milan v1.3 Annex B.1.1): another
grandmaster, or the first after none. A start knows none until the first
Announce. The kit follows the Announce handler's grandmaster through its
own observer (`GptpAnnounceHandler::set_on_grandmaster_changed`), which an
integrator replacing the `GptpAnnounceCallbacks` leaves in place, and
tells its talker (`notify_grandmaster_changed`, `notify_grandmaster_lost`).
The window is timed on CLOCK_MONOTONIC_RAW, the clock a media wake is told
on, as gPTP time may step at the change. The talker reads no clock. The
kit gives `notify_grandmaster_changed()` the instant of the reactor cycle
(`tell_cycle_time()`), and `talker_should_transmit()`, which an entity
calls with the wake's instant before each `transmit_if_due()`, passes
that instant on (`TalkerStreams::tell_wake_time()`); each AVTPDU of the
wake takes its tu at it. An instant never told is in no window, so a
talker told no time sends tu only while no grandmaster is in force. The
reactor thread writes the state, one `itc::SeqlockValue`, and the media
thread reads it for each AVTPDU without a lock. A `TalkerStreams` used
without the kit sends tu clear until it is told.

mr (IEEE 1722-2016 Clause 4.4.4.3; Clause 10.4.3 for CRF) toggles on a
media clock restart of the stream. The talker owes one on every stream
when its media clock re-anchors on a gPTP timeline that stepped
(`MediaClockGenerator::step_count`), as its timestamps then jump, and
`TalkerStreams::toggle_media_clock_restart` asks for one on an open
STREAM_OUTPUT. The next AVTPDU carries the
toggled mr, and so does every one after it until the next restart. A
toggle holds for at least `MR_HOLD_PDUS` (eight) AVTPDUs sent; a restart
owed within the hold toggles mr on the first AVTPDU after it, and
restarts owed before an AVTPDU carries them toggle it once. Any thread
may ask, several at once and a real-time one too: a request is one atomic
load and one atomic add, with no lock and no system call, so a data plane
that re-steps a media clock on its own thread asks at the step. The
requests are counted, and the media thread compares that count with the
one it last followed, so no side waits or writes another's state. A
stream opened again after a stop starts
with mr clear, and a restart asked of the run that stopped is not carried
into the next. MEDIA_RESET counts each toggle sent.

`AvbEntityKit::restart_media_clock(domain)` asks for a restart on each
open stream output of a clock domain. The kit calls it:

| When | Why |
|---|---|
| SET_CLOCK_SOURCE makes another source active in clock domain 0 (the source in force named again is none) | the media clock's source changed |
| the CRF stream the media clock follows toggled mr, was interrupted, or the CRF input lost its stream or was given one | IEEE 1722-2025 4.7.4.4: "any streams deriving timestamps from the CRF stream shall toggle the mr bit if a disruption of the CRF stream occurs or if the mr bit in the CRF stream has been toggled" (10.4.3 says the same of a CRF listener) |

and, per stream output and not per domain, when a SET_SAMPLING_RATE
re-times it, once the entity's data plane has accepted the rate.

The CRF row is looked at once a reactor cycle (`end_cycle()`): the CRF
input counts those events on the RX path
(`ListenerStreams::crf_media_clock_events()`), and the kit follows the
count only while the CRF clock source is the one in force, which is when
`media_rate()` takes the recovery's rate. Several events inside one cycle
are one restart. The first look after the CRF source came into force only
takes the count, since the change of source has toggled mr already, and a
CONNECT repeated for the stream the input is on is no event at all.

`restart_media_clock()` is public for an entity whose data plane sees a
restart the kit cannot: one that receives the CRF stream itself and is
told of a toggled mr. It is the reactor's; from another thread the call is
`TalkerStreams::toggle_media_clock_restart()` per stream.

### The talker's transmit gate

`TalkerGate` decides, stream by stream, whether the talker sends: an ACMP
connection (not under Milan), the stream started, and an MSRP Listener
Ready registered or withdrawn less than the grace ago (five seconds; none
under Milan). The reactor thread writes it. At each Listener Ready
registered or withdrawn it stores the instant on CLOCK_MONOTONIC_RAW when
the declaration was last known registered, then the flag, with release.
The gate reads no clock: the instant is its caller's, or the one the loop
told it for the reactor cycle (`tell_time()`, below). `admitted(idx, now)`
loads the flag with acquire,
then that stamp; `should_transmit()` is `admitted()`, told the media
wake's RAW instant. The media thread and the reactor therefore read one
predicate and agree on when a grace runs out however the wakes fall: a
Ready registered and withdrawn between two wakes still opens its grace,
and one missed over a long gap without wakes is checked against its
stamp. `grace_ends(idx)` is the instant a withdrawn Listener's grace runs
out. Only `note_listener_ready()` writes the stamp: the Listener's
registration alone decides the gate, and a Talker withdrawal leaves it as
it is.

The kit keeps the admitted set on the reactor thread:
`admitted_talker_streams()` lists, in table order, the talker streams the
gate admits, and `set_on_admission_changed()` installs the hook told the
set each time it changes. It is re-evaluated at each gate input: a
Listener Ready registered or withdrawn (`notify_listener_ready()`, which
the host's MSRP hook calls with the instant of the reactor cycle) and an
ACMP connection count changing. A classic stream's grace can run out with no
event at all, so each evaluation also arms one deadline, on the gate's
clock, at the earliest `grace_ends()` of a stream still admitted whose
Listener was withdrawn (`admission_deadline()`; none under Milan, which
has no grace).
`end_cycle(now_ns)` evaluates the set again once it is reached, at the
gate's clock carried forward to `now_ns` (`raw_at()`), in the same call
that ends the counters' interval: one call per reactor cycle, after the
reactor served it, so there is one thing for an entity to forward.
`poll_timeout_ms()` counts the deadline among the ones it does not sleep
past, and from the deadline until `end_cycle()` runs it answers 0, so an
entity that forwards `poll_timeout_ms()` forwards `end_cycle()`, and an
entity with a loop of its own calls it as the runner does. The hook is not told the set in force when it is
installed; a later install replaces an earlier one. STOP_STREAMING
stops the talker's slot, not the gate: the reservation stands, so the
stream stays in the set.

Neither the gate nor the kit reads a clock. The hooks that feed the gate
(MSRP's Listener Ready, ACMP's connect and disconnect) are not handed the
time of their event, and the reactor's clock is not the gate's. So the
loop that drives the reactor tells the entity the time of each cycle on
both clocks: `entity_reactor_clock(entity)` (`avb_entity_reactor_clock.hpp`)
is the clock to construct the `net::MessageReactor` with. Once per cycle,
when the reactor reads its clock and before any handler runs, it reads
CLOCK_MONOTONIC_RAW as well and calls the entity's
`tell_cycle_time(now_ns, raw_now)`. The kit stamps every gate input of the
cycle with `raw_now`; `admitted_talker_streams()` with no instant answers
for it; and `poll_timeout_ms(max_ms, now_ns)` finds the gate's clock at
`now_ns` by carrying the pair forward on the reactor clock (`raw_at()`).
An entity with a `TalkerGate` and no kit forwards the instant to
`TalkerGate::tell_time()`. A loop that tells no time still runs, with one
loss: a Listener Ready noted without an instant is not stamped, so its
withdrawal opens no grace, and the gate says so once in the log. A unit
test tells the pair it wants and steps it.

### The format of a stream input

A stream input starts on the format its model authors and takes any other
its formats list describes, set by SET_STREAM_FORMAT. An AAF entry of that
list with the "up to" bit describes every channel count from one to its
own (IEEE 1722-2025 I.2.4), which is how a model lists the five Base
channel counts Milan v1.3 6.5 asks of an input. The bit is zero in a
current format and in the command, so a command that names such an entry
itself is refused NOT_SUPPORTED, as is a format no entry describes.

| What | How it goes |
|---|---|
| What may change | the channel count of an AAF input. The sample rate is the media clock's, and the sample format, bit depth and samples per AVTPDU stay as the model authors them: a listed format that changes one of those is answered NOT_SUPPORTED |
| When | while the input is not in use (the profile table above). The format in force named again is accepted at any time |
| A stream output | keeps the one format its serializer was made for; any other is NOT_SUPPORTED |
| An AVTPDU that is not of the input's format | discarded and counted UNSUPPORTED_FORMAT (4.4.2.2): its nsr, sample format, bit depth and channels per frame are compared, and its length when the format names the samples per AVTPDU. Under both profiles |
| What the input feeds | keeps the width the model authors. A consumer (`set_consume`) is handed every channel a narrower stream does not carry as silence, and the raw audio sink gets frames of the authored channel count, zero in the channels the stream lacks |
| Where it is reported | GET_STREAM_FORMAT, the STREAM_INPUT descriptor's current_format and GET_STREAM_INFO all give the format in force; the registered controllers are notified of a change (7.5.2) |
| Across a restart | saved in `stream-formats` (below) and restored at the next start (5.3.8.1). A saved format the model no longer describes is left, with a status line, and the input keeps its authored one |

The kit's own input mappings are still the model's static ones: stream
channel *i* feeds cluster *i*, and a format with fewer channels leaves the
higher clusters silent. An entity that hands the handler a store of
dynamic mappings (5.3.10.1, below) gets Milan's check with it: a new
format that would leave a channel a held mapping names is answered
BAD_ARGUMENTS (5.4.2.7).

### Clock sources

A clock domain lists the CLOCK_SOURCE descriptors a controller may choose
between, and the entity runs its media clock from the one in force. The
kit reads what each source of the model is (`clock_source_kind()`,
`avb_entity_clock_sources.hpp`):

| Kind | The descriptor | SET_CLOCK_SOURCE to it |
|---|---|---|
| `Internal` | INTERNAL | accepted |
| `InputStream` | INPUT_STREAM: a stream input, the CRF Media Clock Input among them | accepted |
| `Gptp` | EXTERNAL located at a TIMING descriptor (Milan v1.3 7.5.2) | accepted |
| `External` | any other source | NOT_SUPPORTED, unless the entity named it with `run_clock_source(symbol)` |

A source of the `External` kind is one only the entity can know how to
follow: the system time, a word clock input. The model lists it, and the
entity's code says which of them it runs by the source's symbol, before
`start()` (the menu and selection pattern of the controls: a symbol the
blob lacks binds nothing). One nobody named is refused NOT_SUPPORTED,
"the target of the command is not supported", and the source in force
stays, as the response says. A source that is not of the domain's list
is BAD_ARGUMENTS, from the descriptor storage handler, before the kit is
asked.

`active_clock_source_kind()` tells the entity what its media clock is to
follow now; it may be read on the media thread. `media_rate()` still
knows one source by itself, the CRF input it recovers.

A CRF stream input hands each timestamp to its `StreamCrfFn` with the mr
bit of the AVTPDU that carried it, so a consumer sees the talker's media
clock restart as a toggle of mr (IEEE 1722-2016 Clause 10.4.3), and with a
source generation. That is never 0 and new for the input's first AVTPDU,
when the input is given a stream (another one, or its own again after
losing it) and on a change of the AVTPDUs' stream_id, and the listener
never hands one out twice, so a consumer kept across `close_rx()` and
`open_stream()` sees a new one too. A CONNECT repeated for the stream the
input is on steps nothing: it is the same talker and the same media
clock. mr alone does not show that another talker's timestamps follow, or
the same talker's after the input lost its stream. `set_crf()` also accepts a consumer of the four
arguments before the source generation. `CrfClockRecovery`'s consumer
starts its estimate again on a toggle of mr or a new source, and
`restart()` publishes the unlocked estimate at once.

`start()` binds `crf_recovery()` to one CRF input: the one the CRF clock
source locates, or the first CRF input when the entity names no CRF clock
source. A CRF clock source that locates no CRF input binds it to none,
since `media_rate()` follows that source alone. It leaves alone an input
the host bound with `ListenerStreams::set_crf()` before `start()`, and
the other CRF inputs are the host's.

The source chosen is saved in `clock-sources` (below) at each accepted
SET_CLOCK_SOURCE and put back in force at the next start (Milan v1.3
5.3.11.1). A saved source the model no longer lists, or one the entity
cannot run any more, is left with a status line, and the domain keeps
its authored source.

### Streams carried beside the stream port

Something beside the stream port, a NIC that sends or receives a stream
itself, may carry a stream's frames while the kit's port carries none of
them. The kit keeps the stream's control state (SRP, ACMP, the gate,
sequence, mr, tu and the counters); such a data plane follows it through
the seams below, each of which does nothing until used.

**Frame templates.** Every frame of one talker stream carries the same
bytes but for sequence_num, avtp_timestamp, mr and tu.
`TalkerStreams::frame_template()` gives the headers the talker sends for a
STREAM_OUTPUT now (its destination, the VID and PCP in force, its stream ID
and format) from a given source address, as a `StreamFrameTemplate`: 42
octets for AAF, with sequence_num at 20, avtp_timestamp at 30, mr in octet
19 and tu in octet 21; 38 for CRF, mr and tu both in octet 19, its
timestamps its payload (IEEE 1722-2016 Clauses 4.4.3, 7.2, 10.4).
`write()` copies it and stores the four. `aaf_frame_template()` and
`crf_frame_template()` build one from a serializer's context with the
header code `transmit_aaf()` and `transmit_crf()` use, and the tag is
`stream_frame_header()`'s, so a written template is the frame the talker
sends. AM824 has none.

**Gate.** `AvbEntityKit::set_on_talker_gate_changed()` tells, on the
reactor thread, each change of whether a talker stream streams, as the
media thread's `transmit_if_due()` finds it and STREAM_START and
STREAM_STOP count it: its slot open (from `start()` to `stop()`), MAAP
ready (`MaapAddressAcquirer::set_on_ready_changed()`), admitted, and not
stopped by STOP_STREAMING, which stops the slot and leaves the admitted set
as it is. The kit evaluates it at each evaluation of admission (a gate
input or the admission deadline), at STOP_STREAMING and START_STREAMING,
at each change of MAAP readiness, at the end of `start()`, which so tells
each stream that opens, and in `stop()`, which tells each that closes.
`talker_gate_open()` answers the state in force. The data plane sends a
stream while its gate is open.

**Sequence, mr and tu.** The media thread calls
`TalkerStreams::notify_offload_sent(slot, gate_open, sent)` for
such a stream where it would call `transmit_if_due()`, with the gate it
would pass and what the data plane sent since: the AVTPDUs, those with tu set
(counted at most all of them), and the next sequence_num. They are counted
in `tx_counters()` as the kit's own, so GET_COUNTERS goes on; the stream
starts and stops as `transmit_if_due()` would count it; the serializer's
sequence moves to the one given, and the AAF reframer and the CRF schedule
are left empty, so a stream handed back to the kit goes on from there. It
answers the header the next AVTPDUs carry: the sequence, mr with a
pending `toggle_media_clock_restart()` or media clock re-anchor applied as
`next_mr()` applies it for the kit's next AVTPDU, and tu as
`wake_timestamp_uncertain()` has it at the media wake last told, as the
kit's own AVTPDUs have theirs. The data plane is told the wake as the
port's sender is: through `AvbEntityKit::talker_should_transmit()`, or
`tell_wake_time()` if it does not ask the gate; the talker reads no
clock for it. On a tick the stream does not stream,
mr stays as it stands: the kit's own path asks `next_mr()` only for an
AVTPDU it sends, so restarts owed while nothing goes toggle mr once, when
the stream starts. `offload_header()` answers the header without a
report, as a stream starts. The mr hold runs down by the AVTPDUs reported:
with `OffloadSent::last_mr`, the mr of the last AVTPDU sent, only by those
known to carry the new mr (none before the wire shows it, the last one of
the report where it first does, all after), so it may last longer than
`MR_HOLD_PDUS`, never less (IEEE 1722-2016 Clause 4.4.4.3), and the toggle
is counted when the wire carries it; without it every AVTPDU reported
counts with the mr last answered. A report holds at most one toggle: mr
changes only at a header.

**One writer.** The media thread owns each stream's mr, its hold and its
`TalkerTxCounters`. Both calls are made on it, in `transmit_if_due()`'s
place, so a stream has one writer whichever path sends it: the media
thread calls one of the two for a stream each tick, never both.

**Frames.** `TalkerStreams::set_on_frames_changed()` tells each change of
an open stream's template, on the thread that makes it, the kit's
reactor: a VID or PCP `set_vlan()` moves (a Domain adopted from the wire,
Milan v1.3 4.2.7.2.1; every open stream), a destination
`set_destination()` moves (a MAAP block, `MaapAddressAcquirer::
publish_block()`). Not at `open_stream()`: the data plane reads the
template when the gate opens, and again at each change.

A listener stream may likewise be received beside the port, which then
receives none of it; the kit keeps the input's connection or binding, its
MSRP Listener declaration, the group joined, and its counters.

**What an input receives.** `ListenerStreams::set_on_input_changed()`
tells, on the reactor thread, an input's `InputState`: the stream ID
whose frames it receives (zero for none), whether it is stopped (Milan v1.3
5.3.8.7) and the format in force (5.3.8.1). It is told at `open_stream()`,
so the state in force at open, the stopped state kept by index included;
at each change of the stream (a classic input's connect and disconnect, a
Milan sink's settle and loss), of the stopped state and of the format;
and with stream ID 0 for each input `close_rx()` releases. An index with
no slot is told nothing; `input_state()` answers the state now. The data
plane receives the stream's frames as `frame_is_for_listener()` would,
discards those of a stopped input, and discards an AAF AVTPDU not of the
format in force itself, as the kit's own path does (4.4.2.2).

**Counters.** `ListenerStreams::notify_offload_received()` adds a
`StreamInputCounts`, the per-AVTPDU counts `tally_stream_input_packet()`
keeps and the media lock as the data plane checks it, to the input's
counters (`add_stream_input_counts()`), so GET_COUNTERS and the
observation interval go on, and the AVTPDUs accepted and refused and the
sample frames to its telemetry. A lock gained counts MEDIA_LOCKED, a lock
lost MEDIA_UNLOCKED and STREAM_INTERRUPTED, as at a packet. A stopped
input is not synchronized: its lock is dropped with no interruption
(Milan v1.3 5.3.8.7). An input with no stream leaves its lock to the
reactor, which drops it as it releases the stream, as an interruption
when the talker went (5.3.8.10, Table 5.6), so a report made from the
input hook as the stream goes changes nothing of that. Neither gains a
lock, and nothing of theirs reaches the telemetry. A report with AVTPDUs or a lock clears a pending
restart (a disconnect, a silent interval), as the RX thread's next AVTPDU
does, and gains no lock in that report. One thread reports a stream, in
the RX thread's place: the port receives none of it. It never reports
across `open_stream()` or `close_rx()`, which change the slots it walks. The interval reads
the counts as it reads the kit's own, so an interval with no AVTPDU
reported is silence: report each input at least twice an observation
interval (`COUNTERS_INTERVAL_NS`). The presentation margin statistics stay
the kit's own path's.

### Saved state

What an entity must find again after a restart is kept in files, in one
place per entity. A machine has one state directory (`state.dir`); each
entity keeps its own inside it in a directory named by its entity ID,
sixteen lowercase hex digits with no separators, so that entities of
different programs on one machine never share a file
(`avb_entity_state_dir.hpp`):

```
/var/lib/statusbar-avb/2ccf67fffe0a0b0c/clock-sources
/var/lib/statusbar-avb/2ccf67fffe0a0b0c/listener-bindings
/var/lib/statusbar-avb/2ccf67fffe0a0b0c/maap-range
/var/lib/statusbar-avb/2ccf67fffe0a0b0c/stream-formats
/var/lib/statusbar-avb/2ccf67fffe0a0b0c/talker-advertises
```

| File | Written | By default |
|---|---|---|
| `clock-sources` | the clock source of each clock domain (Milan v1.3 5.3.11.1), one line per domain | under either profile, from the first time a controller changes a clock source |
| `listener-bindings` | the stream inputs' bindings (Milan v1.3 5.3.8.3) | under the Milan profile. The IEEE 1722.1 listener's fast-connect memory changes what a restart does, so it is saved only where `listener.bindings` names a file |
| `maap-range` | the MAAP block the entity defended, probed first at the next start | wherever the addresses come from MAAP: `stream.address_mode = "maap"` under either profile, and the Milan profile when the key is left empty |
| `stream-formats` | the current format of each stream input (Milan v1.3 5.3.8.1), one line per input | under either profile, from the first time a controller changes a format; an entity nobody has set a format on has no such file |
| `talker-advertises` | the Talker Advertise of each stream output as it was last handed to MSRP, without its latency, one line per output (below) | under either profile, about a second after the stream addresses are final |

### A stream output held back after a restart

IEEE 802.1Q-2022 35.2.2.8: "MSRP does not support changes in any of the
FirstValue fields for an existing StreamID", and a Talker that brings a
stream up again with another FirstValue under the same StreamID must
leave "at least two LeaveAllTime ... time periods between when the
Talker removes the existing Stream registration and declares the new
Stream", or "the FDB may allow the new Stream data to start flowing while
the old Stream bandwidth constraints are still configured".

Within one run the MSRP participant keeps that wait itself. Across a
restart only the state directory can say that a stream's destination
address, VLAN, TSpec or priority is no longer what the bridges last had
registered (`avb_entity_talker_advertises.hpp`):

| When | What the kit does |
|---|---|
| At each start | reads `talker-advertises` and tells the MSRP handler what each stream output declared in the last run (`MsrpHandler::talker_hold_after_restart()`) |
| A stream handed to MSRP with the parameters of the last run | declared as ever; nothing waits |
| A stream handed to MSRP with other parameters | its Talker Advertise is held back two LeaveAll periods from the start and goes out with the first PDU after them, 30 to 45 s after the start. A status line says so at both ends |
| While a stream is held back | its gate is shut (`TalkerGate::note_advertise_held()`), with `gate_talker_on_listener` off too, so `talker_should_transmit()` and the gate hook say so and no frame of the stream is sent |
| About once a second | writes the file again when a stream's parameters changed, but keeps a stream's old line while a wait is running for it: a restart inside the wait holds the stream back again |

A changed AccumulatedLatency holds nothing back: it is the one field a
Talker may change in place (35.2.2.8.6), and the file does not keep it.
A file that cannot be read holds nothing back and is replaced; with no
state directory nothing is kept and nothing is held back.

The usual way to meet this is a MAAP block that changed between two
runs. Keeping `maap-range` (above) is what makes it rare: the entity
probes the block it defended last time first.

A key that names a file (`listener.bindings`, `stream.maap_range_file`)
is used as given and the state directory is left alone for that kind. A
new kind of saved state is a new file name in the entity's directory,
not a new path key.

The entity tools default `state.dir` to `/var/lib/statusbar-avb`, which
the package creates and a purge removes. The entity makes only its own
directory there: where the state directory is not there or may not be
written, one status line says so and nothing is saved that no key names.
`AvbEntityAudioIOConfig::state_dir` itself defaults to empty, so an
entity built from a bare configuration (a test, an embedding program)
writes nothing it was not told to.

A Milan model is also a Milan *device*, whichever profile it is held to:
with `entity.milan = true` the entity answers the Milan vendor-unique (MVU)
commands of Milan v1.3 5.4.3 and 5.4.4. GET_MILAN_INFO (5.4.4.1), the one a
Milan controller uses to tell a Milan device from any other ATDECC entity,
answers protocol_version 1, `milan.features_flags` (Table 5.17; 0 by
default), `milan.certification_version` ("0.0.0.0" by default: none
passed) and `milan.specification_version` ("1.3.0.0").

Because it answers them, a Milan model also says so: the kit sets
VENDOR_UNIQUE_SUPPORTED and AEM_INTERFACE_INDEX_VALID in the
entity_capabilities of the ENTITY descriptor it serves and of the ADPDU it
advertises (5.3.3.1, 5.6.2), on top of what the model authors. A model
does not author the two itself: the same blob serves a plain 1722.1
entity, which answers no MVU command and so must not claim to.

Under the Milan profile the entity also answers BIND_STREAM,
UNBIND_STREAM and GET_STREAM_INPUT_INFO_EX (5.4.4.6 to 5.4.4.8) from its
stream inputs' sinks; `MILAN_ACMP.md` has how. The MVU_BINDING bit of the
features_flags says that the first two are implemented, so the entity
states it by itself: set under the Milan profile, clear under any other,
whatever `milan.features_flags` gives. A Milan model held to the classic
profile has no sink to bind, and answers the three NOT_IMPLEMENTED.

SET_MEDIA_CLOCK_REFERENCE_INFO and GET_MEDIA_CLOCK_REFERENCE_INFO
(5.4.4.4, 5.4.4.5), required of every PAAD-AE, keep each CLOCK_DOMAIN's
user priority and media clock domain name (7.6) for the first
`AemCommandHandler::MAX_CLOCK_DOMAINS` (4) the model has; a later one, or
one the model lacks, is NO_SUCH_DESCRIPTOR. The default priority is
MVU_MCR_PRIO_DEFAULT (128) until the entity names its own with
`set_default_mcr_priority()` (Table 7.3), and the user priority follows it
until a controller sets one. A SET applies the fields its flags mark,
refuses a name that is not well-formed UTF-8 BAD_ARGUMENTS and a
controller the lock shuts out ENTITY_LOCKED, answers the settings in
force, and when it changed them sends that answer, after it, to the other
registered controllers as an MVU notification (5.4.5.3). The settings are
held in memory only: `media_clock_reference_info()` reads them, and
nothing yet reports a change to the entity or restores them, which an
entity that keeps them across a restart (as Milan asks) would need.

SET_SYSTEM_UNIQUE_ID and GET_SYSTEM_UNIQUE_ID (5.4.4.2, 5.4.4.3) keep the
system the entity belongs to, a u64 id and a name (Figure 5.7), 0 and
empty until a controller sets them; they belong to the entity, so a
configuration switch keeps them. A SET refuses a controller the lock
shuts out ENTITY_LOCKED and a name that is not well-formed UTF-8
BAD_ARGUMENTS, answers the id in force, and when it changed it notifies
the other registered controllers as above. The entity keeps the id
across power cycles (5.4.4.2) through two calls: the handler tells
`set_on_system_unique_id_changed()`'s hook each id or name a SET puts in
force, for the entity to save, and the entity puts the saved one back
with `restore_system_unique_id()` on the reactor before it starts, which
notifies nobody. `AemCommandHandler::system_unique_id()` reads the one in
force.

Every other MVU command answers NOT_IMPLEMENTED with its data given back.

Without `entity.milan` the entity is a plain 1722.1 entity and lets every
MVU command pass unanswered, as 5.4.4.1 recommends of a Configuration
that is not Milan. The wire structs are in `atdecc_aecp_mvu.hpp`:
`atdecc::AecpMvuDu`, `MvuGetMilanInfoResponse`, `MvuSystemUniqueIdPayload`,
`MvuGetMediaClockReferenceInfoCommand`, `MvuMediaClockReferenceInfoPayload`,
`MvuBindStream`, `MvuStreamInput` and `MvuGetStreamInputInfoExResponse`.

GET_MAX_TRANSIT_TIME (IEEE 1722.1-2021 7.4.77) answers a stream output's
presentation offset: the one above until a controller changes it.
SET_MAX_TRANSIT_TIME (7.4.76) changes it only through the entity's
`AvbEntityKit::set_on_presentation_offset_changed` hook, which puts it in
force in the data plane or refuses it (STREAM_IS_RUNNING while it cannot);
without the hook the offset is fixed and the SET answers NOT_SUPPORTED.
Either way an offset past 0x7FFFFFFF ns is BAD_ARGUMENTS (Milan v1.3
5.4.2.30), a refusal carries the time in force, and a change is notified.

SET_SAMPLING_RATE (IEEE 1722.1-2021 7.4.21) to the rate in force answers
SUCCESS. Another rate the AUDIO_UNIT lists answers NOT_SUPPORTED unless
the entity gives the kit `AvbEntityKit::set_on_unit_rate_changed`, so no
unit reports a rate its data plane does not run. With the hook, a change
of AUDIO_UNIT 0 re-times its streams, the audio streams at its rate in
its clock domain (7.2.3): each AAF stream to its format at the new rate,
channels kept (`aaf_format_at_rate()`), when its descriptor lists that
format; an AM824 stream, or one that lists none, makes it NOT_SUPPORTED,
and a bound Milan input or a connected IEEE 1722.1 input
STREAM_IS_RUNNING (Milan v1.3 5.4.2.7), as does a dynamic audio mapping
held on a stream at the old rate (5.4.2.13). The hook moves the data
plane, all or none, and answers for a talker that cannot change while it
streams. On SUCCESS GET_STREAM_FORMAT and READ_DESCRIPTOR follow, and
each re-timed stream is notified as a SET_STREAM_FORMAT after the
response (`AemCommandHandler::call_after_response()`) to every registered
controller, the requester included: a change beyond what it requested
(Milan v1.3 5.4.5.3). The kit's own
`TalkerStreams` and `ListenerStreams`, its stream specs, `sample_rate()`
and the TSpecs it declares keep the rate they were built with: the media
thread reads them, and they have no way to change rate. An entity whose
streams run on them answers the hook NOT_SUPPORTED; the hook is for one
that runs its streams itself. Another unit, or a unit 0 whose clock
domain another unit shares, keeps its rate. The kit installs its hook on
the storage handler when it is built: one the entity installed before is
replaced, one installed after replaces the kit's.

A STREAM_PORT without AUDIO_MAP descriptors has dynamic audio mappings
(IEEE 1722.1-2021 7.4.44 to 7.4.46, Milan v1.3 5.4.2.26 to 5.4.2.28),
kept in a `DynamicAudioMaps` store (`nanoavb_dynamic_audio_maps.hpp`, up to
1024 mappings over every port, about 20 KB) that the entity owns and hands
to its `AemCommandHandler` with `set_dynamic_audio_maps`; without one,
GET_AUDIO_MAP, ADD and REMOVE answer NOT_IMPLEMENTED, as they did before,
and the handler holds no mapping memory. A port with AUDIO_MAP descriptors
answers them NOT_SUPPORTED. GET_AUDIO_MAP partitions an input port's
cluster channels, and an output port's stream channels (each STREAM_OUTPUT
counted at the most channels any format it lists has, so the partition is
fixed for the configuration), in order, into maps of the size the table
above gives, and answers map_index P with the mappings of partition P.
On an input port, ADD_AUDIO_MAPPINGS accepts a command whose every mapping
names a cluster channel of the port and a channel of a stream input's
current format at the audio unit's rate (unless the port converts rates),
no cluster channel mapped from two stream channels; on an output port the
same, but no stream channel fed from two cluster channels (a cluster
channel may feed several stream channels); REMOVE_AUDIO_MAPPINGS
removes what it lists, a mapping listed twice ignored, one not held
refusing the whole command. Anything else is BAD_ARGUMENTS, past the
store's capacity NO_RESOURCES, with nothing changed. A change goes to the
entity's `set_on_audio_mappings_changed` hook first, all of the port's
mappings at once, and is kept only when the hook answers SUCCESS; without
the hook both commands answer NOT_IMPLEMENTED. The response echoes the
command and a change is notified as it, after it. The entity changes a
port itself with `apply_local_audio_mappings` (a REMOVE then an ADD, all
or nothing, through the same hook, refused while `local_change_refusal()`
refuses), which notifies every registered controller of what changed as
REMOVE then ADD in AECPDUs of at most 63 mappings, and seeds a port's
defaults, or mappings it restores, with `seed_audio_mappings`, which
notifies nobody. A configuration switch forgets every port's mappings.
A SET_STREAM_FORMAT that would leave a channel a held mapping names, or
move its stream off the unit's rate, is BAD_ARGUMENTS (5.4.2.7), and a
SET_SAMPLING_RATE to a rate a held mapping's stream does not run at, on a
port that does not convert rates, NOT_SUPPORTED (5.4.2.13), from a
controller or the entity alike, so every mapping held still holds
(`mappings_hold()`).
The kit hands the handler no store: an entity that
routes channels gives `components().aem_handler` its store and hook.

Profile-independent, because both documents agree: only the ENTITY
descriptor can be acquired or locked (NOT_SUPPORTED otherwise); while
acquired, only the acquirer may lock or unlock; a failed SET answers the
value in force; the unsolicited notification of a command goes to every
registered controller except the one that sent it (7.5.2, 5.4.5.3), and
after the command's response, with those its hooks raise (four held at
most, the command's own last; one of the command's type past those is not
sent, each controller's sequence id spent so it sees the gap, 5.4.5.1), and
after those what a hook left with `AemCommandHandler::call_after_response()`,
which notifies every registered controller, the requester included (5.4.5.3);
SET_CONTROL of the IDENTIFY control honours acquire and
lock like any other control (7.4.25.2, 5.4.2.17). The last one has a knob,
`atdecc.identify_bypasses_lock`, off by default, for installations that
want any controller to be able to make a held box show itself.

An acquisition has a knob of its own, `atdecc.acquire_shared`, off by
default. Some controllers acquire every entity they use and keep it, and
an acquired entity answers every other controller's SET with
ENTITY_ACQUIRED (IEEE 1722.1-2021 7.4.1). Milan settles that by granting
no acquisition at all (v1.3 5.4.2.1), which such a controller may not
accept. With the key set, the classic profile grants the acquisition and
shuts nobody out with it:

| | `acquire_shared` off | `acquire_shared` on |
|---|---|---|
| ACQUIRE_ENTITY of a free entity | SUCCESS | SUCCESS |
| ACQUIRE_ENTITY by a second controller | the CONTROLLER_AVAILABLE handshake, then ENTITY_ACQUIRED or the entity | the same |
| the owner the entity reports | the acquirer | the acquirer |
| a SET from another controller | ENTITY_ACQUIRED | answered as the acquirer's is |
| LOCK_ENTITY from another controller | ENTITY_ACQUIRED | SUCCESS: the lock is then the exclusive access, the acquirer shut out with the rest |
| under the Milan profile | NOT_SUPPORTED to every ACQUIRE_ENTITY | the same: the key has nothing to act on |

## Constraints to preserve

- RT discipline: hooks on the media/RX threads must be `inplace_function`
  (no allocation), following the existing SPSC/seqlock/triple-buffer
  patterns.
- The aemxml build path stays pure-stdlib Python (CMake invokes bare
  `python3` for blob generation; dev tooling via uv is separate).
- One PHC per NIC, owned by gPTP — media clock domains are software rates.
- gPTP profile caveat: the media-clock stack is exercised with the Standard
  profile + free-running switch GM (`GPS_MEDIA_CLOCK.md`); conclusions do
  not automatically transfer to the AVnu Automotive profile.
