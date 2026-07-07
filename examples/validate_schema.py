#!/usr/bin/env python3
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = ["jsonschema"]
# ///
"""Validate every examples/*.json AEM model against the simplified-JSON schema.

The schema (standards/ieee1722.1-schema/atdecc_aem.schema.json) is what editors
use for authoring assistance (each model's "$schema" key points at it), so it
must stay in lockstep with what json_reader.py accepts. This gate catches drift
in the schema direction; aemxml_test.py and verify_models.py cover the reader.

Needs the third-party `jsonschema` package (Debian: python3-jsonschema); exits
77 (ctest SKIP) when it is not installed, so the aemxml pipeline itself stays
pure-stdlib.

Usage: validate_schema.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

try:
    import jsonschema
except ImportError:
    print("SKIP: python3-jsonschema is not installed")
    sys.exit(77)

HERE = Path(__file__).resolve().parent  # avb/examples
SCHEMA = HERE.parent / "standards" / "ieee1722.1-schema" / "atdecc_aem.schema.json"


def main() -> int:
    schema = json.loads(SCHEMA.read_text())
    validator = jsonschema.Draft202012Validator(schema)
    failures = 0
    for path in sorted(HERE.glob("*.json")):
        errors = sorted(
            validator.iter_errors(json.loads(path.read_text())),
            key=lambda e: list(e.absolute_path),
        )
        if errors:
            failures += 1
            print(f"  {path.stem:14s} INVALID:")
            for e in errors[:8]:
                where = "/".join(str(p) for p in e.absolute_path) or "(root)"
                print(f"    {where}: {e.message}")
            if len(errors) > 8:
                print(f"    ... +{len(errors) - 8} more error(s)")
        else:
            print(f"  {path.stem:14s} valid")
    if failures:
        print(f"\nFAILED: {failures} model(s) do not match the schema", file=sys.stderr)
        return 1
    print("\nAll models match the schema.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
