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

Three ctests keep the pipeline honest: `statusbar/avb/aem_json_models_compile`
(`verify_models.py` — every model compiles and parses back),
`statusbar/avb/aem_descriptor_storage_cross_check` (`descriptor_storage_cross_check.py` —
the C++ DescriptorStorage parser reads a Python-written blob and sees the same
descriptor/symbol tables), and `statusbar/avb/aemxml_python_unit` (the aemxml unit
suite).

## Descriptor fields that are not authored content

A handful of per-descriptor bytes are *not* authored by these models and are filled in
elsewhere:

- **AVB_INTERFACE** `mac_address`, `clock_identity`, and the gPTP BMCA parameters
  (`priority1`, `clock_class`, `clock_accuracy`, `priority2`, …, `port_number`) — filled
  at runtime from the live NIC and gPTP.
- **STREAM_INPUT/OUTPUT** the 2021 `redundant_offset` / `number_of_redundant_streams` /
  `timing` trailer-offset fields (bytes 132–137) — computed by `flatten.py` during
  serialization.
- **CONFIGURATION / AVB_INTERFACE / JACK** `localized_description` — `json_reader.py` has
  no per-descriptor key for these three descriptor types. (Streams, clusters, audio
  units, clock sources and clock domains *do* accept a `localized_description` key — see
  `tone-aaf-crf.json`, which cross-references STRINGS entries 1..6.)

## Known gaps

- **`bin2xml` is a stub** (`aemxml_tool.py`: "full model reconstruction not yet implemented
  (Phase 2)") — so an existing device blob / Hive export can't yet be imported back into
  JSON automatically. Finishing it enables round-trip import/export.
