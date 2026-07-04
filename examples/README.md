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

| File | Equivalent C++ `statusbar-aem-entity-blob` flag | Entity |
|------|-------------------------------------------------|--------|
| `bridge.json` | *(default)* — 1 AM824 in + 1 out | `AvbEntityAm824IO` |
| `dual.json` | `--dual` — 2 in (AM824, AAF) + 3 out (AM824, AAF, CRF) | `AvbEntityAudioIO` |
| `tone.json` | `--tone` — talker-only, 3 out (AM824, AAF, CRF) | `AvbEntityToneGenerator` (All) |
| `tone-aaf.json` | `--tone-aaf` — talker-only, 1 out (AAF) | ToneGenerator (AafOnly) |
| `tone-aaf-crf.json` | `--tone-aaf-crf` — talker-only, 2 out (AAF, CRF) | ToneGenerator (AafCrf) |
| `simple_stereo.json` | *(none)* — 2-ch stereo AM824 in + out | the `descriptor_storage_cross_check` fixture |

## Why these exist / migration

These JSON files are now the **single source of truth** for the shipped `.aem` blobs.
`statusbar/tools/CMakeLists.txt` generates the runtime `entity_*.bin` blobs from them at
build time via `aemxml_tool.py json2bin` (`dual.json -> entity_audio.bin`,
`tone.json -> entity_tone.bin`, `tone-aaf.json -> entity_tone_aaf.bin`,
`tone-aaf-crf.json -> entity_tone_aaf_crf.bin`). They are declarative, editable without
recompiling, and able to express arbitrary entities (the Python model covers
Mixer/Matrix/Selector/Splitter descriptors the C++ tool cannot).

The **C++** `aem_entity_blob_tool` (five hardcoded model shapes in
`statusbar/tools/aem_entity_blob_tool.cpp`) is retained for two reasons: it is the
reference the CONTENT-parity test compares against, and it is the fallback blob generator
when Python is unavailable (or when the CMake option `STATUSBAR_AVB_BLOBS_FROM_JSON` is
set `OFF` — it defaults `ON`). The C++ `DescriptorStorage` *parser* is the runtime
consumer in every case.

`verify_models.py` (ctest `statusbar/avb/aem_json_models_match_cpp`) compiles each model
and asserts its descriptor **CONTENT** matches the C++ generator's — a multiset of
`(descriptor_type, descriptor_index, wire_bytes)`, byte-for-byte per descriptor. Fields
that are not authored content are canonicalized before the comparison (see below and the
`NEUTRAL_FIELDS` table in `verify_models.py`). The aemxml Python unit suite runs as the
ctest `statusbar/avb/aemxml_python_unit`.

## Descriptor fields the JSON models cannot byte-match (canonicalized in `verify_models.py`)

The five JSON models CONTENT-match the C++ generator. A handful of per-descriptor bytes
are *not* authored content and are normalized away before comparison, because they are
either runtime-populated, pure serialization artifacts, an unordered set in a different
order, or a field the JSON front-end has no key for:

- **AVB_INTERFACE** `mac_address`, `clock_identity`, and the gPTP BMCA parameters
  (`priority1`, `clock_class`, `clock_accuracy`, `priority2`, …, `port_number`) — filled
  at runtime from the live NIC and gPTP. The Python model defaults several of these to
  `0xFF`, the C++ struct to `0`.
- **STREAM_INPUT/OUTPUT** the 2021 `redundant_offset` / `number_of_redundant_streams` /
  `timing` trailer-offset fields (bytes 132–137) — `flatten.py` computes
  `redundant_offset`; the C++ struct leaves it `0` because the (empty) trailer is not
  modeled.
- **CONFIGURATION** the `descriptor_counts` pairs — the same multiset of (type, count)
  pairs emitted in a different but equally valid order (STRINGS vs CLOCK_DOMAIN position).
- **CONFIGURATION / AVB_INTERFACE / JACK** `localized_description` — the C++ sets
  `0xFFFF` (NO_STRING); `json_reader.py` has no per-descriptor key for these three
  descriptor types, so it stays `0`. (Streams, clusters, audio units, clock sources and
  clock domains *do* accept a `localized_description` key and are matched exactly — see
  `tone-aaf-crf.json`, which reproduces the C++ `localized_description = 1..6` STRINGS
  cross-refs.)

## Known gaps

- **`bin2xml` is a stub** (`aemxml_tool.py`: "full model reconstruction not yet implemented
  (Phase 2)") — so an existing device blob / Hive export can't yet be imported back into
  JSON automatically. Finishing it enables round-trip import/export.
