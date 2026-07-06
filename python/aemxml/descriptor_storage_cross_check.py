#!/usr/bin/env python3
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""
Cross-validation: verify C++ DescriptorStorage reads Python-generated .aem blobs.

Generates a blob from simple_stereo.json, runs the C++ descriptor_storage_tool,
and verifies the output matches what Python wrote.

Usage:
    python3 descriptor_storage_cross_check.py --tool path/to/statusbar-descriptor-storage
    python3 descriptor_storage_cross_check.py --build-dir build/build-Debug
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from aemxml.json_reader import read_json
from aemxml.flatten import flatten
from aemxml.blob_writer import write_blob


def find_tool(build_dir: str) -> Path:
    """Find the descriptor_storage_tool executable."""
    candidates = [
        Path(build_dir) / "statusbar" / "atdecc" / "statusbar-descriptor-storage",
        Path(build_dir) / "statusbar" / "atdecc" / "descriptor_storage_tool",  # legacy
    ]
    for c in candidates:
        if c.exists():
            return c
    raise FileNotFoundError(
        f"descriptor_storage_tool not found in {build_dir}. Run 'make build' first."
    )


def run_tool(tool_path: Path, blob_path: str) -> str:
    """Run descriptor_storage_tool and return stdout."""
    result = subprocess.run(
        [str(tool_path), blob_path],
        capture_output=True,
        text=True,
        timeout=30,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"descriptor_storage_tool failed (exit {result.returncode}):\n{result.stderr}"
        )
    return result.stdout


def parse_tool_output(output: str) -> tuple[dict, list[dict], list[dict]]:
    """Parse descriptor_storage_tool output into structured data.

    Returns (header, descriptors, symbols).
    """
    header = {}
    descriptors = []
    symbols = []

    for line in output.strip().splitlines():
        line = line.strip()
        if line == "DESCRIPTOR_STORAGE" or line == "END":
            continue
        if (
            "=" in line
            and not line.startswith("DESCRIPTOR")
            and not line.startswith("SYMBOL")
        ):
            key, val = line.split("=", 1)
            header[key] = int(val)
        elif line.startswith("DESCRIPTOR "):
            parts = {}
            for token in line.split()[1:]:
                k, v = token.split("=")
                parts[k] = int(v, 16) if v.startswith("0x") else int(v)
            descriptors.append(parts)
        elif line.startswith("SYMBOL "):
            parts = {}
            for token in line.split()[1:]:
                k, v = token.split("=")
                parts[k] = int(v, 16) if v.startswith("0x") else int(v)
            symbols.append(parts)

    return header, descriptors, symbols


def main():
    parser = argparse.ArgumentParser(
        description="Cross-validate Python blobs with C++ DescriptorStorage"
    )
    parser.add_argument(
        "--tool", help="Path to the statusbar-descriptor-storage executable"
    )
    parser.add_argument(
        "--build-dir", default="build/build-Debug", help="Path to CMake build directory"
    )
    args = parser.parse_args()

    repo_root = Path(__file__).parent.parent.parent
    os.chdir(repo_root)

    # Find C++ tool
    tool_path = Path(args.tool) if args.tool else find_tool(args.build_dir)
    print(f"Using tool: {tool_path}")

    # Read JSON and generate blob
    json_path = str(repo_root / "examples" / "simple_stereo.json")
    entity = read_json(json_path)
    descs, syms = flatten(entity)
    blob = write_blob(descs, syms)

    print(
        f"Generated blob: {len(blob)} bytes, {len(descs)} descriptors, {len(syms)} symbols"
    )

    # Write blob to temp file
    with tempfile.NamedTemporaryFile(suffix=".aem", delete=False) as f:
        f.write(blob)
        blob_path = f.name

    try:
        # Run C++ tool
        output = run_tool(tool_path, blob_path)
        header, cpp_descs, cpp_syms = parse_tool_output(output)

        # Verify header
        assert header["blob_size"] == len(blob), (
            f"Blob size mismatch: C++ got {header['blob_size']}, Python wrote {len(blob)}"
        )
        assert header["config_count"] == 1, (
            f"Config count mismatch: {header['config_count']}"
        )
        print(
            f"  [+] header: blob_size={header['blob_size']}, config_count={header['config_count']}"
        )

        # Verify descriptor count
        assert len(cpp_descs) == len(descs), (
            f"Descriptor count mismatch: C++ found {len(cpp_descs)}, Python wrote {len(descs)}"
        )
        print(f"  [+] descriptor count: {len(cpp_descs)}")

        # Verify each descriptor's type, index, and size match
        # Sort both by (config, type, index) for comparison
        py_sorted = sorted(
            [
                (
                    d.config_index,
                    d.descriptor_type,
                    d.descriptor_index,
                    len(d.wire_bytes),
                )
                for d in descs
            ],
            key=lambda x: (x[0], x[1], x[2]),
        )
        cpp_sorted = sorted(
            [(d["config"], d["type"], d["index"], d["size"]) for d in cpp_descs],
            key=lambda x: (x[0], x[1], x[2]),
        )

        for py, cpp in zip(py_sorted, cpp_sorted):
            assert py == cpp, f"Descriptor mismatch: Python={py}, C++={cpp}"
        print(f"  [+] all descriptor type/index/size match")

        # Verify symbol count
        assert len(cpp_syms) == len(syms), (
            f"Symbol count mismatch: C++ found {len(cpp_syms)}, Python wrote {len(syms)}"
        )
        print(f"  [+] symbol count: {len(cpp_syms)}")

        # Verify each symbol
        py_sym_sorted = sorted(
            [
                (s.config_index, s.descriptor_type, s.descriptor_index, s.symbol_code)
                for s in syms
            ],
            key=lambda x: (x[0], x[1], x[2]),
        )
        cpp_sym_sorted = sorted(
            [(s["config"], s["type"], s["index"], s["code"]) for s in cpp_syms],
            key=lambda x: (x[0], x[1], x[2]),
        )

        for py, cpp in zip(py_sym_sorted, cpp_sym_sorted):
            assert py == cpp, f"Symbol mismatch: Python={py}, C++={cpp}"
        print(f"  [+] all symbol type/index/code match")

        print(f"\nCross-validation: PASSED")
        print(f"  {len(cpp_descs)} descriptors verified")
        print(f"  {len(cpp_syms)} symbols verified")

    finally:
        os.unlink(blob_path)


if __name__ == "__main__":
    main()
