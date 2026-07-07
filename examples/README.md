# AEM entity models (declarative)

Declarative JSON descriptions of the AVB entities' IEEE 1722.1 AEM descriptor models.
They are compiled to the binary `.aem` descriptor-storage blob the entities load at
startup, by the pure-stdlib Python tool:

```
python3 ../python/aemxml/aemxml_tool.py json2bin dual.json dual.aem
```

The same tool also converts to/from AEMXML (`json2xml`, `xml2json`, `xml2bin`) — AEMXML
being the interchange format Hive / la_avdecc speak — so these models are portable, not a
private schema. `../python/aemxml/model.py` is the underlying Python AEM model (IR) that
all three front-ends (JSON, AEMXML, direct Python) feed.

## Models

| File | Shape | Entity |
|------|-------|--------|
| `bridge.json` | 1 AM824 in + 1 out | `AvbEntityAm824IO` |
| `dual.json` | 2 in (AM824, AAF) + 3 out (AM824, AAF, CRF) | `AvbEntityAudioIO` |
| `tone.json` | talker-only, 3 out (AM824, AAF, CRF) | `AvbEntityToneGenerator` (All) |
| `tone-aaf.json` | talker-only, 1 out (AAF) | ToneGenerator (AafOnly) |
| `tone-aaf-crf.json` | talker-only, 2 out (AAF, CRF) | ToneGenerator (AafCrf) |
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

Four ctests keep the pipeline honest: `statusbar/avb/aem_json_models_compile`
(`verify_models.py` — every model compiles and parses back),
`statusbar/avb/aem_json_models_schema` (`validate_schema.py` — every model matches the
authoring schema; skips without python3-jsonschema),
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
  (Phase 2)") — so an existing device blob / Hive export can't yet be imported back into
  JSON automatically. Finishing it enables round-trip import/export.
- **`bin2json` flattens locales**: `json_writer.py` emits a single explicit `strings`
  table and does not reconstruct the dict form, so converting a *multi-locale* blob back
  to JSON loses the per-language grouping.
