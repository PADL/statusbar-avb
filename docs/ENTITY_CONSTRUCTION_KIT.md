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
| `AvbEntityHost` (`avb_entity_host.hpp:85`) | Owns all 7 protocol SMs, lifecycle, SPSC logging; parameterized by blob + 3 stream-count integers; auto-wires IDENTIFY by scanning blob CONTROL descriptors (`avb_entity_host.cpp:45-70`). Serves GET_AVB_INFO and the AVB_INTERFACE runtime gPTP fields from observed state, or — `mirror_avb_info_from(target, controller)` — mirrored from another entity over 1722.1 (`avb_entity_avb_info_mirror.hpp`: one-target ATDECC controller reading its AVB_INTERFACE descriptors, registering for unsolicited notifications, polling GET_AVB_INFO). Control-plane complete. |
| aemxml pipeline | `model.py`/`flatten.py` cover **all 43** IEEE 1722.1-2021 descriptor types, 1:1 with the C++ structs (`atdecc_aem_descriptor.hpp`); symbol table; `${var}` expansion (json2bin `--set`); JSON schema; byte-exact wire tests. Only the JSON front-end is narrower (see gap table). |
| Handler hooks | Symbol-keyed generic `on_set/get_descriptor_value` (`nanoavb_aem_entity_handler.hpp:284-298`), unsolicited-notification fan-out, `apply_local_descriptor_value` for entity-originated changes. Foundation for control binding; used only by IDENTIFY today. |
| SIGNAL_SELECTOR + MATRIX runtimes | Full built-ins with veto/apply `inplace_function` callbacks and `matrix_cell()` readback (`nanoavb_aem_descriptor_storage_handler.hpp:354-437`). The template to replicate for CONTROL and MIXER. |
| `MediaClockGenerator` (`ptpclient_media_clock.hpp:47`) | Phase-accumulator clock taking a per-tick rate `r` from anywhere — the correct abstraction for slaving. CRF output timestamps come from the same accumulator as audio (`avb_entity_talker_streams.cpp:93-181`), so TX coherence is by construction. |
| `KalmanRatioTracker` (`ptpclient_freq_ratio.hpp:132`) | 3-state phase/freq/drift estimator with outlier gating — exactly the filter CRF clock recovery needs (today it eats gPTP−GPS pairs; feed it CRF−gPTP instead). |
| Depacketizers | AAF int16/24/32/f32 any-rate (`avtp_aaf_stream_input.hpp:37`), AM824 incl. an unused MIDI/SMPTE-capable variant (`avtp_am824_stream_input.hpp:150`), CRF complete-but-unwired. All reconstruct full 64-bit PTS. |
| StereoIO playout pattern | PTS-stamped SPSC pipe + consume-when-due against the local media clock (`avb_entity_stereo_io.cpp:464-483,540-572`) — the one real time-aligned renderer; generalize for the kit listener. |
| Diagnostics | STREAM_INPUT counters (media lock, seq, late/early — `avb_entity_stream_counters.cpp`), TX pcap recorder (`tx_pcap_recorder.hpp`), per-stream ACMP∧MSRP talker gate. |

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
| ACQUIRE_ENTITY | granted for the ENTITY descriptor (7.4.1) | NOT_SUPPORTED, never granted (5.4.2.1) |
| A SET to the value already in force | applied and notified: 7.5.2 notifies every SET answered SUCCESS | answered SUCCESS, not applied, not notified (5.4.5.3) |
| A controller's DEREGISTER_UNSOLICITED_NOTIFICATION | the remaining registered controllers are notified (7.5.2 lists the command) | nobody is told: each copy would carry the recipient's own controller_entity_id (5.4.5.1) and read as its own deregistration; Table 5.20 sends one only to the controller removed |
| Changes by non-ATDECC means while locked (front panel, `set_local_identify`) | allowed | refused ENTITY_LOCKED (5.3.4.1, 5.4.2.17) |
| MSRP Registrar receiving Lv in IN | starts the leavetimer (802.1Q Table 10-4) | goes straight to MT (4.2.7.2.2) |
| MRP timers (MSRP and MVRP) | 802.1Q Table 10-7: JoinTime 200 ms, LeaveTime 1000 ms, LeaveAll 10 to 15 s, periodic 1 s | Milan Table 4.3: the same with LeaveTime 5000 ms (4.2.7.1.1) |
| Connection management | IEEE 1722.1-2021 clause 8: the listener relays a controller's connect to the talker, the talker keeps a list of its listeners. `NanoAvbAcmpListener`, `NanoAvbAcmpTalker` | Milan v1.3 section 5.5, a separate implementation (`MILAN_ACMP.md`): a bind is saved and answered at once, the listener then probes its talker and follows the talker's SRP attribute by itself; the talker answers probes and keeps nothing. `NanoAvbMilanAcmpListener`, `NanoAvbMilanAcmpTalker`, chosen at start-up by `NanoAvbComponents::use_milan_acmp` |
| A bind or connect from a controller other than the one holding the lock | accepted: ACMP does not ask | refused CONTROLLER_NOT_AUTHORIZED (5.5.2.4, 5.5.2.5) |
| START_STREAMING and STOP_STREAMING on a stream input | the input takes or discards its frames (7.4.35, 7.4.36); the state is not saved | the started/stopped state of the input's sink (5.3.8.7), saved with its binding and also set by a bind's STREAMING_WAIT; no effect on an input that is not bound. A stopped input discards its frames and keeps its reservation |
| BIND_STREAM, UNBIND_STREAM and GET_STREAM_INPUT_INFO_EX, on a Milan model (`entity.milan`) | NOT_IMPLEMENTED, and MVU_BINDING clear in GET_MILAN_INFO | answered by the stream inputs' sinks (5.4.4.6 to 5.4.4.8), a bind from a controller the lock shuts out ENTITY_LOCKED; MVU_BINDING set |
| When a talker stream is put on the wire (`stream.gate_talker_on_listener`) | an ACMP connection, the stream started and MSRP Listener Ready, held five seconds past the last Listener Ready seen | the stream started and MSRP Listener Ready, with no grace: a Milan talker has no connections (5.5.4.1) and hears of no disconnect |
| Which Talker attribute a stream input's Listener declaration follows | any of the stream's StreamID (IEEE 802.1Q 35.1.2.2) | the one on the destination address and VLAN the talker's PROBE_TX_RESPONSE gave (5.3.8.5) |
| `listener.bindings` | off when empty; set, each successful connect is remembered and fast-connected again after a restart (file form v1) | the binding parameters of 5.3.8.3, written at every bind and unbind before the controller is answered, and each saved sink starts in PRB_W_AVAIL (file form v2; either form is read). Empty, they go to `listener-bindings` in the entity's state directory (below); with no state directory either, they last as long as the process |
| When a stream output's Talker attribute is declared | once the entity is ready and its stream addresses are final | only while a listener wants the stream: a PROBE_TX_COMMAND for it arrived in the last 15 s, or a Listener attribute for its StreamID is registered on the port; withdrawn when neither holds, and while MAAP reports the address lost (4.3.3.1). `MsrpHandler::set_talker_waits_for_listener` |
| Talker presentation offset when `presentation_offset_ns` is 0 (the default) | 1 ms | 2 ms (5.3.7.6); a value other than 0 is used as given under either |
| A Class A Domain registered from the wire with other parameters | ours is kept (802.1Q) | adopted: re-declared, the stream frames take its VLAN and priority, MVRP follows, GET_AVB_INFO is notified (4.2.7.2.1); a VID outside 1 to 4094 or a priority above 7 (802.1Q Table 9-2, 9-3) is left alone and the Domain in force kept |

### Saved state

What an entity must find again after a restart is kept in files, in one
place per entity. A machine has one state directory (`state.dir`); each
entity keeps its own inside it in a directory named by its entity ID,
sixteen lowercase hex digits with no separators, so that entities of
different programs on one machine never share a file
(`avb_entity_state_dir.hpp`):

```
/var/lib/statusbar-avb/2ccf67fffe0a0b0c/listener-bindings
/var/lib/statusbar-avb/2ccf67fffe0a0b0c/maap-range
```

| File | Written | By default |
|---|---|---|
| `listener-bindings` | the stream inputs' bindings (Milan v1.3 5.3.8.3) | under the Milan profile. The IEEE 1722.1 listener's fast-connect memory changes what a restart does, so it is saved only where `listener.bindings` names a file |
| `maap-range` | the MAAP block the entity defended, probed first at the next start | under `stream.address_mode = "maap"`, either profile |

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

Under the Milan profile the entity also answers BIND_STREAM,
UNBIND_STREAM and GET_STREAM_INPUT_INFO_EX (5.4.4.6 to 5.4.4.8) from its
stream inputs' sinks; `MILAN_ACMP.md` has how. The MVU_BINDING bit of the
features_flags says that the first two are implemented, so the entity
states it by itself: set under the Milan profile, clear under any other,
whatever `milan.features_flags` gives. A Milan model held to the classic
profile has no sink to bind, and answers the three NOT_IMPLEMENTED.

Every other MVU command answers NOT_IMPLEMENTED with its data given back.
Milan allows that of SET_SYSTEM_UNIQUE_ID and GET_SYSTEM_UNIQUE_ID, which
are optional. SET_MEDIA_CLOCK_REFERENCE_INFO and
GET_MEDIA_CLOCK_REFERENCE_INFO (5.4.4.4, 5.4.4.5) are required of every
PAAD-AE and are not implemented yet, and no MVU response is sent as an
unsolicited notification.

Without `entity.milan` the entity is a plain 1722.1 entity and lets every
MVU command pass unanswered, as 5.4.4.1 recommends of a Configuration
that is not Milan. The wire structs are in `atdecc_aecp_mvu.hpp`:
`atdecc::AecpMvuDu`, `MvuGetMilanInfoResponse`, `MvuBindStream`,
`MvuStreamInput` and `MvuGetStreamInputInfoExResponse`.

Profile-independent, because both documents agree: only the ENTITY
descriptor can be acquired or locked (NOT_SUPPORTED otherwise); while
acquired, only the acquirer may lock or unlock; a failed SET answers the
value in force; the unsolicited notification of a command goes to every
registered controller except the one that sent it (7.5.2, 5.4.5.3);
SET_CONTROL of the IDENTIFY control honours acquire and
lock like any other control (7.4.25.2, 5.4.2.17). The last one has a knob,
`atdecc.identify_bypasses_lock`, off by default, for installations that
want any controller to be able to make a held box show itself.

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
