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

Today the shipped `.aem` blobs are generated at build time by the **C++**
`aem_entity_blob_tool`, whose five model shapes are hardcoded in
`statusbar/tools/aem_entity_blob_tool.cpp`. These JSON files are the declarative,
data-driven replacement — one source of truth, editable without recompiling, and able to
express arbitrary entities (the Python model covers Mixer/Matrix/Selector/Splitter
descriptors the C++ tool cannot).

`verify_models.py` (run by the ctest `statusbar/avb/aem_json_models_match_cpp`) compiles
each model and asserts its **descriptor-type multiset matches the C++ generator's** — the
regression net that lets the C++ model builders be retired later (the C++
`DescriptorStorage` *parser* stays; it is the runtime consumer). To flip the switch, point
the `entity_*.bin` `add_custom_command`s in `statusbar/tools/CMakeLists.txt` at
`json2bin <model>.json` instead of `aem_entity_blob_tool <flag>`.

## Known gaps (see the JSON-model investigation notes)

- **`bin2xml` is a stub** (`aemxml_tool.py`: "full model reconstruction not yet implemented
  (Phase 2)") — so an existing device blob / Hive export can't yet be imported back into
  JSON automatically. Finishing it enables round-trip import/export.
- **`tone-aaf-crf.json` omits the per-descriptor localized-name references.** The C++
  model sets `localized_description = 1..6` cross-refs into STRINGS for controller UI
  labels; `json_reader.py` reads a config-level `strings` array but not a per-descriptor
  `localized_description` key. The stream/clock/port descriptor set matches; only the
  name-ref wiring is missing (`model.py` already carries the field — only the reader needs
  it).
- Stream formats use literal hex for AM824/CRF; only AAF has a structured
  (`{"type":"AAF",...}`) encoder in `stream_formats.py`, and that encoder currently
  disagrees byte-for-byte with the C++ `aaf_8ch_96k_32bit()` layout — worth reconciling.
