# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""CLI: ``gen-lua <wireshark-dir>`` writes the generated modules; ``check`` diffs them.

``check`` exits 1 when a committed generated file differs from what the
schema produces now (the drift guard the test suite runs).
"""

from __future__ import annotations

import argparse
import os
import sys

from . import luagen

GENERATED: dict[str, object] = {
    "statusbar_avb/gen/avtp_fields.lua": luagen.generate_avtp_fields,
    "statusbar_avb/gen/acf_fields.lua": luagen.generate_acf_fields,
}


def main() -> int:
    ap = argparse.ArgumentParser(prog="wireshark_schema")
    sub = ap.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("gen-lua", help="write the generated Lua modules")
    gen.add_argument("wireshark_dir")
    chk = sub.add_parser("check", help="verify the generated Lua modules are current")
    chk.add_argument("wireshark_dir")
    opts = ap.parse_args()

    drift = 0
    for relative, producer in GENERATED.items():
        path = os.path.join(opts.wireshark_dir, relative)
        text = producer()
        if opts.command == "gen-lua":
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(text)
            print(f"wrote {path}")
        else:
            try:
                with open(path, encoding="utf-8") as fh:
                    current = fh.read()
            except FileNotFoundError:
                current = ""
            if current != text:
                print(f"out of date: {path} (regenerate with gen-lua)")
                drift += 1
    return 1 if drift else 0


if __name__ == "__main__":
    sys.exit(main())
