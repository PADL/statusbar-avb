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
| `AvbEntityHost` (`avb_entity_host.hpp:85`) | Owns all 7 protocol SMs, lifecycle, SPSC logging; parameterized by blob + 3 stream-count integers; auto-wires IDENTIFY by scanning blob CONTROL descriptors (`avb_entity_host.cpp:45-70`). Control-plane complete. |
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

## Target developer experience

```cpp
// ~40 lines instead of ~1,500
auto kit = AvbEntityKit::from_blob("mixer8.aem");        // topology, formats, counts,
                                                          // controls, clocking — all derived
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
