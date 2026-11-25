#!/usr/bin/env python3
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Verify every declarative examples/*.json AEM model compiles and parses.

Each model is run through the aemxml pipeline (read_json -> flatten ->
write_blob) and the resulting blob is parsed back with blob_reader.read_blob.
This is the build gate that keeps the JSON models — the single source of truth
for the shipped entity_*.bin descriptor-storage blobs — compilable at all
times. Deeper coverage lives in aemxml_test.py (pipeline unit suite) and
descriptor_storage_cross_check.py (Python writer vs the C++ DescriptorStorage
parser, the runtime consumer).

Each model is also held to the Milan v1.3 descriptor rules that a model alone
can meet or break (section 5.3.3; see milan_model_faults), so an edit that
breaks one of them fails the build and not a certification run.

Usage: verify_models.py
"""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent  # avb/examples
sys.path.insert(0, str(HERE.parent / "python"))

from aemxml.blob_reader import read_blob  # noqa: E402
from aemxml.blob_writer import write_blob  # noqa: E402
from aemxml.flatten import flatten  # noqa: E402
from aemxml.json_reader import read_json  # noqa: E402
from aemxml.model import INTERFACE_FLAGS_NAMES  # noqa: E402

# Milan v1.3 5.3.3.4: a STREAM_INPUT's buffer_length is at least 2.126 ms.
MILAN_MIN_BUFFER_LENGTH_NS = 2126000
# Milan v1.3 5.3.3.5: both set in an AVB_INTERFACE's interface_flags.
MILAN_INTERFACE_FLAGS = (
    INTERFACE_FLAGS_NAMES["GPTP_SUPPORTED"] | INTERFACE_FLAGS_NAMES["SRP_SUPPORTED"]
)


def _output_mapping_faults(config) -> list[str]:
    """5.3.3.9: one static mapping at most for a channel of a Stream Output."""
    seen: set[tuple[int, int]] = set()
    faults = []
    for unit in config.audio_units:
        for port in unit.output_stream_ports:
            for mapping in (m for amap in port.maps for m in amap.mappings):
                key = (mapping.stream_index, mapping.stream_channel)
                if key in seen:
                    faults.append(
                        f"STREAM_OUTPUT {key[0]} channel {key[1]} is mapped twice"
                    )
                seen.add(key)
    return faults


def _cluster_faults(config) -> list[str]:
    """5.3.3.8: an AUDIO_CLUSTER has one channel."""
    return [
        f"AUDIO_CLUSTER '{cluster.object_name}' has {cluster.channel_count} channels"
        for unit in config.audio_units
        for port in unit.input_stream_ports + unit.output_stream_ports
        for cluster in port.clusters
        if cluster.channel_count != 1
    ]


def milan_model_faults(entity) -> list[str]:
    """What in @p entity breaks a Milan v1.3 5.3.3 rule a model decides.

    Not held here: what the entity adds when it runs as a Milan one (the
    ENTITY capabilities of 5.3.3.1, the 2021 descriptor lengths) and what its
    data plane has to back (the formats of section 6).
    """
    faults = []
    for config in entity.configurations:
        faults += _cluster_faults(config)
        faults += _output_mapping_faults(config)
        faults += [
            f"STREAM_INPUT '{s.object_name}' buffer_length {s.buffer_length} ns"
            for s in config.streams_input
            if s.buffer_length < MILAN_MIN_BUFFER_LENGTH_NS
        ]
        faults += [
            f"AVB_INTERFACE '{i.object_name}' lacks GPTP_SUPPORTED or SRP_SUPPORTED"
            for i in config.avb_interfaces
            if i.interface_flags & MILAN_INTERFACE_FLAGS != MILAN_INTERFACE_FLAGS
        ]
    return faults


def main() -> int:
    failures = 0
    # Variables the models may reference (the build passes the real values via
    # json2bin --set); any OTHER unset ${...} reference still fails the model.
    variables = {"version": "0.0.0-verify"}
    for path in sorted(HERE.glob("*.json")):
        try:
            entity = read_json(str(path), variables=variables)
            if faults := milan_model_faults(entity):
                raise ValueError("not a Milan model: " + "; ".join(faults))
            descs, syms = flatten(entity)
            blob = write_blob(descs, syms)
            parsed_descs, parsed_syms = read_blob(blob)
            if len(parsed_descs) != len(descs) or len(parsed_syms) != len(syms):
                raise ValueError(
                    f"round-trip mismatch: wrote {len(descs)} descriptors / "
                    f"{len(syms)} symbols, read back {len(parsed_descs)} / "
                    f"{len(parsed_syms)}"
                )
            if not parsed_descs:
                raise ValueError("blob contains no descriptors")
        except Exception as e:  # noqa: BLE001 -- report every model, then fail
            failures += 1
            print(f"  {path.stem:14s} FAILED: {e}")
            continue
        print(
            f"  {path.stem:14s} {len(blob):>6} bytes, "
            f"{len(descs):>2} descriptors, {len(syms):>2} symbols"
        )
    if failures:
        print(f"\nFAILED: {failures} model(s) do not compile/parse", file=sys.stderr)
        return 1
    print("\nAll models compile + parse, and keep the Milan model rules.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
