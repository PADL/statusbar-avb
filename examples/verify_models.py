#!/usr/bin/env python3
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Verify the declarative examples/*.json AEM models compile to blobs whose descriptor
CONTENT matches the C++ statusbar-aem-entity-blob output for the same model.

This is the migration regression net: while both the C++ generator (hardcoded model
builders) and the Python aemxml pipeline (JSON -> model -> blob) exist, we prove they
agree on the descriptor CONTENT for every shipped model -- a multiset (Counter) of
(descriptor_type, descriptor_index, wire_bytes) built with aemxml.blob_reader.read_blob.
Whole-blob byte-identity is NOT required (STRINGS packing / symbol-table ordering differ
at the blob level); what must match is the set of descriptors the C++ DescriptorStorage
parser sees at runtime, byte-for-byte per (type, index).

Non-authored fields are canonicalized before comparison (see NEUTRAL_FIELDS below). These
are fields whose bytes are NOT authored content -- they are either populated at runtime
by the entity (MAC / clock_identity / gPTP BMCA parameters, all filled from the live NIC
and gPTP), pure serialization artifacts (the STREAM 2021 redundant/timing trailer offset,
which flatten.py computes and the C++ struct leaves zero when the trailer is empty), an
unordered set emitted in a different but equivalent order (the CONFIGURATION
descriptor_counts pairs), or a field the JSON front-end cannot express (the C++ sets a
config/jack/avb localized_description of 0xFFFF = NO_STRING, but json_reader.py has no key
for it on those descriptors, so it stays 0). Canonicalizing them keeps the comparison at
the level of authored content while still catching any real model divergence.

Usage: verify_models.py [PATH_TO_statusbar-aem-entity-blob]
  With the C++ tool path  -> compare each JSON blob to the C++ blob (fails on mismatch).
  Without it              -> parse-only (just confirm every JSON compiles + parses).
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent  # avb/examples
sys.path.insert(0, str(HERE.parent / "python"))

from aemxml.blob_reader import read_blob  # noqa: E402
from aemxml.blob_writer import write_blob  # noqa: E402
from aemxml.flatten import flatten  # noqa: E402
from aemxml.json_reader import read_json  # noqa: E402
from aemxml.model import (  # noqa: E402
    DESCRIPTOR_TYPE_NAMES,
    DESCRIPTOR_CONFIGURATION,
    DESCRIPTOR_STREAM_INPUT,
    DESCRIPTOR_STREAM_OUTPUT,
    DESCRIPTOR_AVB_INTERFACE,
    DESCRIPTOR_JACK_INPUT,
    DESCRIPTOR_JACK_OUTPUT,
)

# model name -> the C++ tool flag(s) that build the equivalent model.
MODELS: dict[str, list[str]] = {
    "bridge": [],
    "dual": ["--dual"],
    "tone": ["--tone"],
    "tone-aaf": ["--tone-aaf"],
    "tone-aaf-crf": ["--tone-aaf-crf"],
}

# The C++ CMake build passes these --name defaults to the tool. We match them here so the
# ENTITY/STRINGS name bytes line up with the shipped blobs (--channels/--sample-rate are
# the tool defaults 8 / 96000, so they need no override).
MODEL_ARGS: dict[str, list[str]] = {
    "bridge": ["--name", "Statusbar AVB Bridge"],
    "dual": ["--name", "AVB Audio IO"],
    "tone": ["--name", "Statusbar Tone Generator"],
    "tone-aaf": ["--name", "Statusbar Tone Generator"],
    "tone-aaf-crf": ["--name", "Statusbar Tone Generator"],
}

# Per-descriptor-type byte ranges that are NOT authored content and are zeroed before the
# multiset comparison (see the module docstring for the rationale). Ranges are [start,end).
NEUTRAL_FIELDS: dict[int, list[tuple[int, int]]] = {
    # localized_description (json_reader has no config-level key for it).
    DESCRIPTOR_CONFIGURATION: [(68, 70)],
    # 2021 redundant/timing trailer OFFSET fields: flatten computes redundant_offset,
    # the C++ struct leaves it 0 because the (empty) trailer is not modeled.
    DESCRIPTOR_STREAM_INPUT: [(132, 138)],
    DESCRIPTOR_STREAM_OUTPUT: [(132, 138)],
    # localized_description (68) + mac_address (70) + clock_identity/gPTP BMCA params +
    # port_number (78..97): all filled at runtime from the live NIC and gPTP. Keep the
    # authored interface_flags at [76:78].
    DESCRIPTOR_AVB_INTERFACE: [(68, 70), (70, 76), (78, 98)],
    # localized_description (json_reader has no jack-level key for it).
    DESCRIPTOR_JACK_INPUT: [(68, 70)],
    DESCRIPTOR_JACK_OUTPUT: [(68, 70)],
}

_TYPE_NAME = {v: k for k, v in DESCRIPTOR_TYPE_NAMES.items()}


def _tname(t: int) -> str:
    return _TYPE_NAME.get(t, f"0x{t:04X}")


def canon(desc_type: int, wire: bytes) -> bytes:
    """Return the authored-content form of a descriptor: zero the NEUTRAL_FIELDS byte
    ranges and, for CONFIGURATION, sort the descriptor_counts pairs (an unordered set the
    two generators emit in different order)."""
    b = bytearray(wire)
    for start, end in NEUTRAL_FIELDS.get(desc_type, []):
        for i in range(start, min(end, len(b))):
            b[i] = 0
    if desc_type == DESCRIPTOR_CONFIGURATION:
        # Header is 74 bytes; the descriptor_counts pairs (4 bytes each) follow.
        head = bytes(b[:74])
        pairs = sorted(bytes(b[i : i + 4]) for i in range(74, len(b), 4))
        return head + b"".join(pairs)
    return bytes(b)


def json_blob(name: str) -> bytes:
    descs, syms = flatten(read_json(str(HERE / f"{name}.json")))
    return write_blob(descs, syms)


def content_counter(blob: bytes) -> Counter:
    """Multiset of (descriptor_type, descriptor_index, canon(wire_bytes)) for a blob."""
    return Counter(
        (d.descriptor_type, d.descriptor_index, canon(d.descriptor_type, d.wire_bytes))
        for d in read_blob(blob)[0]
    )


def _hexdiff(py_bytes: bytes | None, cpp_bytes: bytes | None) -> str:
    if py_bytes is None:
        return "    (missing in Python blob)"
    if cpp_bytes is None:
        return "    (missing in C++ blob)"
    if len(py_bytes) != len(cpp_bytes):
        return f"    length py={len(py_bytes)} cpp={len(cpp_bytes)}"
    diffs = [i for i in range(len(py_bytes)) if py_bytes[i] != cpp_bytes[i]]
    out = [
        f"    @{off}: py={py_bytes[off]:02x} cpp={cpp_bytes[off]:02x}"
        for off in diffs[:8]
    ]
    if len(diffs) > 8:
        out.append(f"    ... +{len(diffs) - 8} more byte(s)")
    return "\n".join(out)


def report_diff(py_blob: bytes, cpp_blob: bytes, limit: int = 6) -> None:
    """Print the first `limit` descriptors that differ AFTER canonicalization, showing the
    RAW wire bytes so the real difference is visible."""
    py_raw = {
        (d.descriptor_type, d.descriptor_index): d.wire_bytes
        for d in read_blob(py_blob)[0]
    }
    cpp_raw = {
        (d.descriptor_type, d.descriptor_index): d.wire_bytes
        for d in read_blob(cpp_blob)[0]
    }
    py_canon = {k: canon(k[0], v) for k, v in py_raw.items()}
    cpp_canon = {k: canon(k[0], v) for k, v in cpp_raw.items()}
    keys = sorted(set(py_canon) | set(cpp_canon))
    shown = 0
    for key in keys:
        if py_canon.get(key) == cpp_canon.get(key):
            continue
        t, i = key
        print(f"    DIFF {_tname(t)} index={i}:")
        print(_hexdiff(py_raw.get(key), cpp_raw.get(key)))
        shown += 1
        if shown >= limit:
            remaining = (
                sum(1 for k in keys if py_canon.get(k) != cpp_canon.get(k)) - shown
            )
            if remaining > 0:
                print(f"    ... +{remaining} more differing descriptor(s)")
            break


def main() -> int:
    cpp_tool = sys.argv[1] if len(sys.argv) > 1 else None
    if cpp_tool:
        print(
            "Comparing JSON json2bin blobs to the C++ generator at the descriptor CONTENT level."
        )
        print("Non-authored fields are canonicalized before comparison: CONFIGURATION")
        print(
            "localized_description + descriptor_counts order; STREAM 2021 redundant/timing"
        )
        print(
            "trailer offsets; AVB_INTERFACE mac/clock_identity/gPTP + localized_description;"
        )
        print("JACK localized_description (all runtime-filled, serialization, or not")
        print(
            "expressible via json_reader.py). See NEUTRAL_FIELDS for exact byte ranges.\n"
        )
    failures = 0
    for name, flag in MODELS.items():
        pyb = json_blob(name)
        py = content_counter(pyb)
        if cpp_tool:
            with tempfile.NamedTemporaryFile(suffix=".bin") as f:
                subprocess.run(
                    [cpp_tool, *flag, *MODEL_ARGS.get(name, []), "--out", f.name],
                    check=True,
                    capture_output=True,
                )
                cppb = Path(f.name).read_bytes()
            cpp = content_counter(cppb)
            ok = py == cpp
            failures += not ok
            status = "CONTENT MATCH" if ok else "MISMATCH"
            print(
                f"  {name:14s} py={sum(py.values()):>2} descs, cpp={sum(cpp.values()):>2} descs  {status}"
            )
            if not ok:
                report_diff(pyb, cppb)
        else:
            print(f"  {name:14s} py={sum(py.values()):>2}  (parse-only, no C++ tool)")
    # simple_stereo has no C++ equivalent -- just prove it compiles + parses.
    if (HERE / "simple_stereo.json").exists():
        n = sum(content_counter(json_blob("simple_stereo")).values())
        print(f"  {'simple_stereo':14s} py={n:>2}  (parse-only)")
    if failures:
        print(
            f"\nFAILED: {failures} model(s) diverge from the C++ generator at the CONTENT level",
            file=sys.stderr,
        )
        return 1
    print(
        "\nAll models CONTENT-MATCH the C++ generator."
        if cpp_tool
        else "\nAll models compile + parse."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
