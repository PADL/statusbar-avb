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


def main() -> int:
    failures = 0
    for path in sorted(HERE.glob("*.json")):
        try:
            descs, syms = flatten(read_json(str(path)))
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
    print("\nAll models compile + parse.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
