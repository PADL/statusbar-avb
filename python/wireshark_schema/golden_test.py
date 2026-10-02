#!/usr/bin/env python3
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Golden-capture test: tshark + the Lua dissectors against the reference decoder.

Usage: golden_test.py --loader wireshark/statusbar_avb.lua --pcap golden.pcap
                      [--tshark /path/to/tshark]

Exit 0 when every avb.* field tshark reports matches the reference decoder
for every frame (and every reference field is present); 1 on a mismatch;
77 (ctest SKIP_RETURN_CODE) when tshark cannot be found.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from wireshark_schema import decode, pcap  # noqa: E402

SKIP = 77


def normalise(value: object) -> int | str | None:
    """A tshark JSON field value as the reference decoder would state it."""
    if isinstance(value, list):
        value = value[0]
    if not isinstance(value, str):
        return None
    text = value.strip()
    lowered = text.lower()
    if lowered in ("true", "false"):
        return 1 if lowered == "true" else 0
    try:
        return int(text, 0)
    except ValueError:
        return text.replace(":", "").lower()


def tshark_layers(tshark: str, loader: str, capture: str) -> list[dict]:
    """Per-frame layer dictionaries from tshark -T json."""
    # Everything tshark reads is staged into one temporary directory that is
    # also its HOME. The isolated HOME keeps the user's personal plugins and
    # preferences out of the run (a stray plugin that prints at load would
    # corrupt the JSON; a preference could re-enable the builtin dissectors),
    # and staging matters on Ubuntu, whose AppArmor profile for tshark denies
    # reading files under arbitrary paths such as a CI work tree but allows
    # the temporary directory.
    with tempfile.TemporaryDirectory(prefix="statusbar-avb-wireshark-") as home:
        staged_dir = os.path.join(home, "wireshark")
        shutil.copytree(os.path.dirname(os.path.abspath(loader)), staged_dir)
        staged_loader = os.path.join(staged_dir, os.path.basename(loader))
        staged_capture = os.path.join(home, os.path.basename(capture))
        shutil.copyfile(capture, staged_capture)
        command = [
            tshark,
            "-X",
            f"lua_script:{staged_loader}",
            "-r",
            staged_capture,
            "-T",
            "json",
        ]
        env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home, XDG_DATA_HOME=home)
        result = subprocess.run(
            command, capture_output=True, text=True, check=False, env=env
        )
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        raise RuntimeError(f"tshark exited {result.returncode}")
    if "Lua Error" in result.stderr or "Lua:" in result.stderr:
        print(result.stderr, file=sys.stderr)
        raise RuntimeError("tshark reported a Lua error")
    text = result.stdout
    start = text.find("[")
    if start < 0:
        raise RuntimeError("tshark produced no JSON")
    return [entry["_source"]["layers"] for entry in json.loads(text[start:])]


def collect_avb_fields(layers: dict) -> dict[str, object]:
    """Every avb.* field in a frame's layers, flattened."""
    found: dict[str, object] = {}

    def walk(node: object) -> None:
        if isinstance(node, dict):
            for key, value in node.items():
                if key.startswith("avb.") and not isinstance(value, dict):
                    found[key] = value
                walk(value)
        elif isinstance(node, list):
            for item in node:
                walk(item)

    walk(layers)
    return found


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--loader", required=True)
    ap.add_argument("--pcap", required=True)
    ap.add_argument("--tshark", default=None)
    opts = ap.parse_args()

    tshark = opts.tshark or shutil.which("tshark")
    if not tshark or not os.path.exists(tshark):
        print("tshark not found: skipping", file=sys.stderr)
        return SKIP

    frames = pcap.avtp_frames(opts.pcap)
    layers = tshark_layers(tshark, opts.loader, opts.pcap)
    failures = 0
    checked = 0
    for frame in frames:
        expected = (
            decode.decode_ip_avtpdu(frame.avtp)
            if frame.transport == "udp"
            else decode.decode_avtpdu(frame.avtp)
        )
        actual = collect_avb_fields(layers[frame.number - 1])
        if not actual:
            print(f"frame {frame.number}: tshark shows no avb.* fields")
            failures += 1
            continue
        for name, want in expected.items():
            if name not in actual:
                print(f"frame {frame.number}: missing {name} (expected {want!r})")
                failures += 1
                continue
            got = normalise(actual[name])
            if got != want:
                print(
                    f"frame {frame.number}: {name}: tshark {got!r} != reference {want!r}"
                )
                failures += 1
            checked += 1
    print(f"{len(frames)} frames, {checked} fields checked, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
