# AEM entity models (declarative)

Declarative JSON descriptions of the AVB entities' IEEE 1722.1 AEM descriptor models.
They are compiled to the binary `.aem` descriptor-storage blob the entities load at
startup, by the pure-stdlib Python tool:

```
python3 ../python/aemxml/aemxml_tool.py json2bin dual.json dual.aem
```

The same tool also converts to/from AEMXML (`json2xml`, `xml2json`, `xml2bin`) — AEMXML
being the interchange format other ATDECC controllers read and write — so these models are
portable, not a private schema. `../python/aemxml/model.py` is the underlying Python AEM model (IR) that
all three front-ends (JSON, AEMXML, direct Python) feed.

## Variables

String values in a model may reference build-time variables as `${name}`, supplied with
repeatable `--set name=value` options:

```
python3 ../python/aemxml/aemxml_tool.py json2bin tone.json tone.aem --set version=1.8.0
```

A model that references an unset variable fails to compile (the error names the variable
and its JSON path), so a `${...}` can never silently reach the wire. A string that is
exactly one reference takes the value with JSON type coercion (`--set nch=8` +
`"channels": "${nch}"` → the number 8); embedded references splice as text
(`"jdk-${site}"`); `$${` escapes a literal `${`. Typical use: `"firmware": "${version}"`
stamped by the build so the blob's ENTITY descriptor matches the package version.

## Models

| File | Shape | Entity |
|------|-------|--------|
| `bridge.json` | 1 AM824 in + 1 out | `AvbEntityAm824IO` |
| `dual.json` | 2 in (AM824, AAF) + 3 out (AM824, AAF, CRF) | `AvbEntityAudioIO` |
| `tone.json` | talker, 3 out (AM824, AAF, CRF) + a CRF clock input | `AvbEntityToneGenerator` (All) |
| `tone-aaf.json` | talker, 1 out (AAF) + a CRF clock input | ToneGenerator (AafOnly) |
| `tone-aaf-crf.json` | talker, 2 out (AAF, CRF) + a CRF clock input | ToneGenerator (AafCrf) |
| `simple_stereo.json` | 2-ch stereo AM824 in + out | the `descriptor_storage_cross_check` fixture |

## Source of truth

These JSON files are the **single source of truth** for the shipped `.aem` blobs.
`statusbar/tools/CMakeLists.txt` generates the runtime `entity_*.bin` blobs from them at
build time via `aemxml_tool.py json2bin` (`dual.json -> entity_audio.bin`,
`tone.json -> entity_tone.bin`, `tone-aaf.json -> entity_tone_aaf.bin`,
`tone-aaf-crf.json -> entity_tone_aaf_crf.bin`). They are declarative, editable without
recompiling, and able to express arbitrary entities (Mixer/Matrix/Selector/Splitter
descriptors included). Because the generator is host Python, cross builds need no
build-host-runnable generator binary. The C++ `DescriptorStorage` *parser*
(`statusbar/atdecc`) is the runtime consumer.

(The retired C++ generator, `aem_entity_blob_tool.cpp` with five hardcoded model shapes,
was kept during the migration as a CONTENT-parity reference until every model was proven
byte-equivalent at the descriptor level, then deleted.)

## Milan models

Every model here keeps the Milan v1.3 descriptor rules that a model decides
(section 5.3.3), and `verify_models.py` fails a model that breaks one:

| Rule | In the model |
|---|---|
| An AUDIO_CLUSTER has one channel (5.3.3.8) | a stream port of eight channels is eight clusters of `"channels": 1`, and its maps pair stream channel *i* with cluster *i*, `cluster_ch` 0 |
| A channel of a Stream Output has one static mapping at most (5.3.3.9) | each stream port's maps name its own stream: port 0 maps stream 0, port 1 stream 1 |
| A STREAM_INPUT's buffer_length is at least 2.126 ms (5.3.3.4) | `"buffer_length": 2126000` (nanoseconds) on every entry of `streams_in`; left out, the field is 0 |
| An AVB_INTERFACE has GPTP_SUPPORTED and SRP_SUPPORTED (5.3.3.5) | both in `avb_interfaces[].flags` |
| A Stream Input that lists the Base format at a rate lists it for 1, 2, 4, 6 and 8 channels (6.5) | one entry of `formats` with `"up_to": true` beside the `format` the input starts on (below) |
| A stream's `format` is one its `formats` describe (5.3.3.4) | follows from the two keys; a `formats` list that leaves the `format` out fails the model |
| An AAF Media Talker has a CRF Media Clock Input in each clock domain (7.2.2) | every model with an AAF output has a `streams_in` entry in the Milan Clock Reference Format, `"format": "0x041060010000BB80"` |
| An AAF Media Listener with two or more AAF inputs in a clock domain has a CRF Media Clock Input and Output there (7.2.2, 7.2.3) | no model has two AAF inputs; one that gains a second needs both |
| Each CRF input is one CLOCK_SOURCE, an INPUT_STREAM located at that STREAM_INPUT, and a configuration with a stream output has an INTERNAL one (5.3.3.6) | `clock_sources`: `"type": "INPUT_STREAM", "location_type": 5, "location_index": <the input>` beside the `"INTERNAL"` entry, both in the clock domain's `sources` |
| gPTP as a media clock source is an EXTERNAL CLOCK_SOURCE with no flags, located at a TIMING that is SINGLE with one PTP_INSTANCE; that instance is GRANDMASTER_CAPABLE with one PTP_PORT on AVB_INTERFACE 0; the configuration has one AVB_INTERFACE (5.3.3.12 to 5.3.3.14, 7.5) | no shipped model has it yet; see "gPTP as a media clock source" below |

### gPTP as a media clock source

A clock domain that can follow gPTP says so with four descriptors (Milan
v1.3 7.5). In a model they are a clock source and two more keys of the
configuration:

```json
"clock_sources": [
  { "name": "Internal", "type": "INTERNAL" },
  { "name": "gPTP", "symbol": "gptp_clock", "type": "EXTERNAL",
    "location_type": "TIMING", "location_index": 0 }
],
"timing": { "localized_description": { "en-US": "gPTP" } },
"ptp_instance": {
  "flags": ["GRANDMASTER_CAPABLE"],
  "ptp_port": { "port_number": 1, "type": "P2P_LINK_LAYER",
                "avb_interface": 0, "profile_identifier": "00-80-C2-00-01-00" }
}
```

- `location_type` takes a descriptor type's name or its number.
- `timing` (or `timings`, an array) is a TIMING descriptor: `algorithm`
  is SINGLE unless given, and `ptp_instances` names PTP_INSTANCE 0 unless
  given.
- `ptp_instance` (or `ptp_instances`) is a PTP_INSTANCE with the PTP_PORT
  descriptors it owns in `ptp_port` or `ptp_ports`. PTP_PORT indices run
  through the instances in order.
- A model leaves `clock_identity` out. The entity reads the instance with
  its interface's clock identity, the one the AVB_INTERFACE reports. An
  instance that names a clock identity keeps it.
- `verify_models.py` holds a clock source located at a TIMING to the
  rules of the table above.

`buffer_length` says how much audio the input can hold. The entity kit
hands received samples on with their presentation time and holds none
itself, so the figure is a promise made for whatever consumes them: a
consumer that plays out has to keep 2.126 ms of room for it to be true.

The generator keeps one more rule for every model: a CONFIGURATION's
descriptor_counts list the top level descriptors only (IEEE 1722.1-2021
7.2.2, Milan v1.3 5.3.3.2). STRINGS, the stream ports, the clusters and
the maps are not counted there: each is reached from the descriptor that
owns it. A controller has to follow those owners to see them, as
`statusbar-atdecc-controller` and `statusbar-atdecc-monitor --enumerate`
do.

What a model does not decide is not authored. The entity adds
VENDOR_UNIQUE_SUPPORTED and AEM_INTERFACE_INDEX_VALID to its
entity_capabilities when it runs as a Milan device (`entity.milan`), and
sends the 2021 descriptor lengths under the Milan profile; see
`../docs/ENTITY_CONSTRUCTION_KIT.md`. The same blobs serve a plain
IEEE 1722.1 entity unchanged.

An AAF stream input says which channel counts it takes with one entry of
its `formats` list (6.6, IEEE 1722-2025 I.2.4):

```json
"format":  { "type": "AAF", "rate": 96000, "channels": 8, "depth": 32 },
"formats": [ { "type": "AAF", "rate": 96000, "channels": 8, "depth": 32, "up_to": true } ]
```

`format` is the format the input uses until a controller sets another.
The `up_to` entry describes every channel count from one to eight at that
rate, so a SET_STREAM_FORMAT may name any of them. `up_to` belongs in
`formats` only: a current format never has the bit. With `formats` left
out the list is the `format` alone, which is what a stream output wants:
its static mappings hold in that one format.

Not yet Milan in the models: the stream port inputs carry static maps,
where 5.3.3.9 has the input mappings dynamic.

The data plane takes its channel count from the first stream port: the
channels of its clusters, summed.

Four ctests guard the pipeline: `statusbar/avb/aem_json_models_compile`
(`verify_models.py` — every model compiles and parses back, and keeps the rules above),
`statusbar/avb/aem_json_models_schema` (`validate_schema.py` — every model matches the
authoring schema; skips without jsonschema — `uv sync --group validate` in `../python/`,
or Debian python3-jsonschema, activates it),
`statusbar/avb/aem_descriptor_storage_cross_check` (`descriptor_storage_cross_check.py` —
the C++ DescriptorStorage parser reads a Python-written blob and sees the same
descriptor/symbol tables), and `statusbar/avb/aemxml_python_unit` (the aemxml unit
suite).

## Editor support (JSON Schema)

Each model's `"$schema"` key points at
[`../standards/ieee1722.1-schema/atdecc_aem.schema.json`](../standards/ieee1722.1-schema/atdecc_aem.schema.json),
so editors with JSON-Schema support (VS Code out of the box) get completion, hover
documentation, enum validation for every flag/type name, and typo detection
(`additionalProperties: false`) while authoring a model. The schema mirrors exactly what
`json_reader.py` accepts — including the dict-form `localized_description` — and the
`aem_json_models_schema` ctest keeps it from drifting.

## Localized strings

Author localized names as a `{locale: text}` dict directly at any
`localized_description` field — the string table and the per-language LOCALE + STRINGS
descriptors are generated automatically:

```json
"streams_out": [
  {
    "name": "StreamOutputAAF",
    "localized_description": { "en-US": "AAF Audio", "de-DE": "AAF-Audio" }
  }
]
```

Rules (see `tone-aaf-crf.json` for a full multi-language model):

- Every descriptor with a localized name takes the dict form: configurations, streams,
  jacks, AVB interfaces, audio units, clusters, controls, clock sources, clock domains.
- One LOCALE (plus its STRINGS descriptors) is generated per language, in order of first
  appearance; every locale has the same table layout, so one wire reference resolves in
  all of them. A language missing from some entry falls back to that entry's first-listed
  text, never an empty name.
- Identical dicts share one table slot. Slots 0 and 1 hold the entity `vendor` and
  `model` names (which may themselves be dicts), matching the ENTITY descriptor's fixed
  vendor/model string references.
- Omitting `localized_description` means NO_STRING (`0xFFFF`) — no more authoring
  `65535` by hand.
- The old forms still work when a model manages its own table: a config-level `strings`
  array with raw integer references (`offset << 3 | index`) or `{"offset": O, "index":
  I}` objects. Mixing an explicit `strings` array with dict-form localization in the same
  configuration is an error. With neither present, the table defaults to
  `[vendor, model, configuration-name]` in an `en` locale.

## Controls

CONTROL descriptors are authored with symbolic names and typed values — the
`value_details` wire payload and the descriptor's `number_of_values` are generated:

```json
"controls": [
  {
    "name": "Volume",
    "control_type": "GAIN",
    "value_type": "LINEAR_INT32",
    "localized_description": { "en-US": "Volume", "de-DE": "Lautstärke" },
    "values": [
      { "min": -60, "max": 12, "step": 1, "default": 0, "unit": "LEVEL_DB" }
    ]
  }
]
```

`control_type` takes a standard name (ENABLE, IDENTIFY, MUTE, GAIN, …, per IEEE 1722.1
Clause 7.3.4) or a hex EUI-64 for vendor types. `value_type` takes a ControlValueType
name; `read_only: true` / `unsettable: true` set the R/U flag bits. The typed `values`
form covers LINEAR_* (list of `{current/min/max/step/default/unit/string_ref}` items),
numeric SELECTOR_* (`{current/default/options/unit}`), and UTF8 (a plain string); each
value item's `string_ref` accepts the dict form. `unit` takes a UnitsCode name
(LEVEL_DB, PERCENT, …). Other value types are authored as raw hex in `value_details`.
`block_latency`, `control_latency`, `control_domain`, and `reset_time` are plain
integers. The schema completes and validates all of these names in the editor.

## Descriptor fields that are not authored content

A handful of per-descriptor bytes are *not* authored by these models and are filled in
elsewhere:

- **AVB_INTERFACE** `mac_address`, `clock_identity`, and the gPTP BMCA parameters
  (`priority1`, `clock_class`, `clock_accuracy`, `priority2`, …, `port_number`) — filled
  at runtime from the live NIC and gPTP.
- **STREAM_INPUT/OUTPUT** the 2021 `redundant_offset` / `number_of_redundant_streams` /
  `timing` trailer-offset fields (bytes 132–137) — computed by `flatten.py` during
  serialization.

## Known gaps

- **`bin2xml` is a stub** (`aemxml_tool.py`: "full model reconstruction not yet implemented
  (Phase 2)") — so an existing device blob or AEMXML export can't yet be imported back into
  JSON automatically. Finishing it enables round-trip import/export.
- **`bin2json` flattens locales**: `json_writer.py` emits a single explicit `strings`
  table and does not reconstruct the dict form, so converting a *multi-locale* blob back
  to JSON loses the per-language grouping.
