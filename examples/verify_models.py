#!/usr/bin/env python3
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Verify the declarative examples/*.json AEM models compile to blobs whose descriptor
set matches the C++ statusbar-aem-entity-blob output for the same model.

This is the migration regression net: while both the C++ generator (hardcoded model
builders) and the Python aemxml pipeline (JSON -> model -> blob) exist, we prove they
agree on the descriptor-type multiset for every shipped model. Byte-identity is NOT
required (STRINGS packing / ordering differ) -- only that the C++ DescriptorStorage the
entities parse at runtime sees the same descriptors.

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

# model name -> the C++ tool flag(s) that build the equivalent model.
MODELS: dict[str, list[str]] = {
    "bridge": [],
    "dual": ["--dual"],
    "tone": ["--tone"],
    "tone-aaf": ["--tone-aaf"],
    "tone-aaf-crf": ["--tone-aaf-crf"],
}


def json_blob(name: str) -> bytes:
    descs, syms = flatten(read_json(str(HERE / f"{name}.json")))
    return write_blob(descs, syms)


def type_counter(blob: bytes) -> Counter:
    return Counter(d.descriptor_type for d in read_blob(blob)[0])


def main() -> int:
    cpp_tool = sys.argv[1] if len(sys.argv) > 1 else None
    failures = 0
    for name, flag in MODELS.items():
        py = type_counter(json_blob(name))
        if cpp_tool:
            with tempfile.NamedTemporaryFile(suffix=".bin") as f:
                subprocess.run([cpp_tool, *flag, "--out", f.name], check=True, capture_output=True)
                cpp = type_counter(Path(f.name).read_bytes())
            ok = py == cpp
            failures += not ok
            print(f"  {name:14s} py={sum(py.values()):>2} cpp={sum(cpp.values()):>2}  {'MATCH' if ok else 'MISMATCH ' + str((py - cpp) + (cpp - py))}")
        else:
            print(f"  {name:14s} py={sum(py.values()):>2}  (parse-only, no C++ tool)")
    # simple_stereo has no C++ equivalent -- just prove it compiles + parses.
    if (HERE / "simple_stereo.json").exists():
        n = sum(type_counter(json_blob("simple_stereo")).values())
        print(f"  {'simple_stereo':14s} py={n:>2}  (parse-only)")
    if failures:
        print(f"\nFAILED: {failures} model(s) diverge from the C++ generator", file=sys.stderr)
        return 1
    print("\nAll models match the C++ generator." if cpp_tool else "\nAll models compile + parse.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
