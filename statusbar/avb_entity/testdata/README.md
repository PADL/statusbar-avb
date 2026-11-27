<!-- SPDX-License-Identifier: MIT -->
# Test data

## `simple2.bin`

ATDECC AEM descriptor-storage blob — a binary serialisation of an AVDECC
entity model consumed by `AvbEntityAm824IO::create` via the
`descriptor_storage_blob` configuration field. Loaded as a fixture by
`statusbar/avb_entity/avb_entity_am824_io_test.cpp`.

The blob describes a single 8-channel AM824 entity:

- 1 entity
- 1 configuration
- 1 stream input
- 1 stream output
- 1 AVB interface

These counts are asserted by the test that consumes it.

The blob is **generated externally** from an AEMXML model description; no
in-tree generator currently exists, so this file is a committed fixture.
If regenerated, the expected descriptor counts in
`avb_entity_am824_io_test.cpp` must be updated to match.

To inspect the blob, run `statusbar-descriptor-storage` (see
`statusbar/atdecc/atdecc_descriptor_storage_tool.cpp`).

## `entity_audio.bin`, `entity_tone.bin`, `entity_tone_aaf.bin`, `entity_tone_aaf_crf.bin`

The shipped models of `examples/` (`dual.json`, `tone.json`,
`tone-aaf.json`, `tone-aaf-crf.json`), compiled with the version the
tests expect. After a model changes, write them again from the top of the
repository and commit them with it:

```
for pair in dual:entity_audio tone:entity_tone tone-aaf:entity_tone_aaf tone-aaf-crf:entity_tone_aaf_crf; do
    python3 python/aemxml/aemxml_tool.py json2bin "examples/${pair%%:*}.json" \
        "statusbar/avb_entity/testdata/${pair##*:}.bin" --set version=0.0.0-test
done
```

## `entity_crf_generator.bin`

`examples/crf-generator.json` compiled the same way: one CRF stream
output and no audio, and a clock domain of four sources: the local
oscillator, gPTP (an EXTERNAL source located at a TIMING, Milan v1.3
7.5), the system time and IEEE 1588 PTP. No tool runs it yet; the tests
of the clock sources read it.

```
python3 python/aemxml/aemxml_tool.py json2bin examples/crf-generator.json \
    statusbar/avb_entity/testdata/entity_crf_generator.bin --set version=0.0.0-test
```

## `entity_rates.bin`

The rate model `entity_rates.json` beside it, compiled the same way: an
AUDIO_UNIT listing 48, 96 and 192 kHz, an AAF stream input and output
listing their 8-channel format at 48 and 96 kHz, a CRF stream output,
and stream ports of eight one-channel clusters without AUDIO_MAP
descriptors. It keeps the Milan v1.3 5.3.3 model rules
(`examples/verify_models.py`'s `milan_model_faults`). Loaded by
`avb_entity_kit_test.cpp` for SET_SAMPLING_RATE, and compiled into
`nanoavb_aem_fuzzer.cpp` with `#embed`. Write it again from the top of
the repository:

```
python3 python/aemxml/aemxml_tool.py json2bin statusbar/avb_entity/testdata/entity_rates.json \
    statusbar/avb_entity/testdata/entity_rates.bin --set version=0.0.0-test
```
