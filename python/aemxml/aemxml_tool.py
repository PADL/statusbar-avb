#!/usr/bin/env python3
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""AEMXML command-line tool: convert between AEMXML and binary descriptor storage."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from aemxml.aemxml_reader import read_aemxml
from aemxml.aemxml_writer import write_aemxml
from aemxml.blob_reader import read_blob
from aemxml.blob_writer import write_blob
from aemxml.flatten import flatten
from aemxml.json_reader import read_json
from aemxml.json_writer import write_json
from aemxml.unflatten import unflatten


def cmd_xml2bin(args: argparse.Namespace) -> None:
    entity = read_aemxml(args.input)
    descs, syms = flatten(entity)
    blob = write_blob(descs, syms)
    with open(args.output, "wb") as f:
        f.write(blob)
    print(
        f"Wrote {len(blob)} bytes ({len(descs)} descriptors, {len(syms)} symbols) to {args.output}"
    )


def cmd_bin2xml(args: argparse.Namespace) -> None:
    with open(args.input, "rb") as f:
        data = f.read()
    descs, syms = read_blob(data)
    entity = unflatten(descs, syms)
    write_aemxml(entity, path=args.output, schema_year=args.schema_year)
    print(
        f"Wrote AEMXML ({len(descs)} descriptors, {len(syms)} symbols) to {args.output}"
    )


def cmd_bin2json(args: argparse.Namespace) -> None:
    with open(args.input, "rb") as f:
        data = f.read()
    descs, syms = read_blob(data)
    entity = unflatten(descs, syms)
    write_json(entity, args.output)
    print(
        f"Wrote simplified JSON ({len(descs)} descriptors, {len(syms)} symbols) to {args.output}"
    )


def cmd_validate(args: argparse.Namespace) -> None:
    from aemxml.aemxml_reader import validate_aemxml

    errors = validate_aemxml(args.input)
    if not errors:
        print("Valid")
    else:
        for err in errors:
            print(f"  ERROR: {err}")
        print(f"\n{len(errors)} validation error(s)")
        sys.exit(1)


def cmd_dump(args: argparse.Namespace) -> None:
    with open(args.input, "rb") as f:
        data = f.read()
    descs, syms = read_blob(data)

    print(f"Descriptor Storage: {len(data)} bytes")
    print(f"  Descriptors: {len(descs)}")
    print(f"  Symbols: {len(syms)}")
    print()

    desc_type_names = {
        0x0000: "ENTITY",
        0x0001: "CONFIGURATION",
        0x0002: "AUDIO_UNIT",
        0x0005: "STREAM_INPUT",
        0x0006: "STREAM_OUTPUT",
        0x0007: "JACK_INPUT",
        0x0008: "JACK_OUTPUT",
        0x0009: "AVB_INTERFACE",
        0x000A: "CLOCK_SOURCE",
        0x000C: "LOCALE",
        0x000D: "STRINGS",
        0x000E: "STREAM_PORT_INPUT",
        0x000F: "STREAM_PORT_OUTPUT",
        0x0014: "AUDIO_CLUSTER",
        0x0017: "AUDIO_MAP",
        0x001A: "CONTROL",
        0x0024: "CLOCK_DOMAIN",
    }

    for d in descs:
        name = desc_type_names.get(d.descriptor_type, f"0x{d.descriptor_type:04X}")
        print(
            f"  [{d.config_index}] {name}[{d.descriptor_index}] = {len(d.wire_bytes)} bytes"
        )

    if syms:
        print()
        print("  Symbols:")
        for s in syms:
            name = desc_type_names.get(s.descriptor_type, f"0x{s.descriptor_type:04X}")
            print(
                f"  [{s.config_index}] {name}[{s.descriptor_index}] = 0x{s.symbol_code:08X}"
            )


def _parse_set_vars(pairs: list[str] | None) -> dict[str, str]:
    """Parse repeated --set NAME=VALUE options into a variables dict for
    ${name} expansion in the JSON model."""
    variables: dict[str, str] = {}
    for pair in pairs or []:
        name, sep, value = pair.partition("=")
        if not sep or not name:
            raise SystemExit(f"error: --set expects NAME=VALUE, got '{pair}'")
        variables[name] = value
    return variables


def cmd_json2bin(args: argparse.Namespace) -> None:
    entity = read_json(args.input, variables=_parse_set_vars(args.set))
    descs, syms = flatten(entity)
    blob = write_blob(descs, syms)
    with open(args.output, "wb") as f:
        f.write(blob)
    print(
        f"Wrote {len(blob)} bytes ({len(descs)} descriptors, {len(syms)} symbols) to {args.output}"
    )


def cmd_json2xml(args: argparse.Namespace) -> None:
    entity = read_json(args.input, variables=_parse_set_vars(args.set))
    xml_str = write_aemxml(entity)
    with open(args.output, "w") as f:
        f.write(xml_str)
    print(f"Wrote AEMXML to {args.output}")


def cmd_xml2json(args: argparse.Namespace) -> None:
    entity = read_aemxml(args.input)
    json_str = write_json(entity, args.output)
    print(f"Wrote simplified JSON to {args.output}")


def cmd_upgrade(args: argparse.Namespace) -> None:
    """Convert AEMXML from 2013 (avdecc.xsd) to 2021 (atdecc.xsd) schema."""
    entity = read_aemxml(args.input)
    xml_str = write_aemxml(entity, path=args.output, schema_year=2021)
    if args.output:
        print(f"Upgraded to 2021 schema: {args.output}", file=sys.stderr)
    else:
        print(xml_str)


def cmd_downgrade(args: argparse.Namespace) -> None:
    """Convert AEMXML from 2021 (atdecc.xsd) to 2013 (avdecc.xsd) schema."""
    entity = read_aemxml(args.input)

    # Check for 2021-only descriptors
    incompatible = []
    for ci, config in enumerate(entity.configurations):
        for ti, t in enumerate(config.timings):
            incompatible.append(
                f"  Configuration[{ci}]/Timing[{ti}]: {t.object_name!r}"
            )
        for pi, p in enumerate(config.ptp_instances):
            incompatible.append(
                f"  Configuration[{ci}]/PtpInstance[{pi}]: {p.object_name!r}"
            )

    if incompatible and not args.strip:
        print("Error: cannot downgrade — 2021-only descriptors found:", file=sys.stderr)
        for desc in incompatible:
            print(desc, file=sys.stderr)
        print(
            f"\n{len(incompatible)} incompatible descriptor(s). Use --strip to remove them.",
            file=sys.stderr,
        )
        sys.exit(1)

    if incompatible and args.strip:
        for desc in incompatible:
            print(f"Warning: stripping {desc}", file=sys.stderr)
        for config in entity.configurations:
            config.timings.clear()
            config.ptp_instances.clear()

    xml_str = write_aemxml(entity, path=args.output, schema_year=2013)
    if args.output:
        print(f"Downgraded to 2013 schema: {args.output}", file=sys.stderr)
    else:
        print(xml_str)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="AEMXML tool: convert between AEMXML and binary descriptor storage"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_xml2bin = sub.add_parser("xml2bin", help="Convert AEMXML to binary .aem blob")
    p_xml2bin.add_argument("input", help="Input .aemxml file")
    p_xml2bin.add_argument("output", help="Output .aem file")

    p_bin2xml = sub.add_parser("bin2xml", help="Convert binary .aem blob to AEMXML")
    p_bin2xml.add_argument("input", help="Input .aem file")
    p_bin2xml.add_argument("output", help="Output .aemxml file")
    p_bin2xml.add_argument(
        "--schema-year",
        type=int,
        choices=[2013, 2021],
        default=2021,
        help="Schema version year (default: 2021)",
    )

    p_bin2json = sub.add_parser(
        "bin2json", help="Convert binary .aem blob to simplified JSON"
    )
    p_bin2json.add_argument("input", help="Input .aem file")
    p_bin2json.add_argument("output", help="Output .json file")

    p_validate = sub.add_parser("validate", help="Validate AEMXML against XSD schema")
    p_validate.add_argument("input", help="Input .aemxml file")

    p_dump = sub.add_parser("dump", help="Dump binary .aem blob contents")
    p_dump.add_argument("input", help="Input .aem file")

    _set_help = (
        "Define a variable for ${name} references in the JSON model "
        "(repeatable). A model referencing an unset variable is an error."
    )

    p_json2bin = sub.add_parser(
        "json2bin", help="Convert simplified JSON to binary .aem blob"
    )
    p_json2bin.add_argument("input", help="Input .json file")
    p_json2bin.add_argument("output", help="Output .aem file")
    p_json2bin.add_argument(
        "--set", action="append", metavar="NAME=VALUE", help=_set_help
    )

    p_json2xml = sub.add_parser("json2xml", help="Convert simplified JSON to AEMXML")
    p_json2xml.add_argument("input", help="Input .json file")
    p_json2xml.add_argument("output", help="Output .aemxml file")
    p_json2xml.add_argument(
        "--set", action="append", metavar="NAME=VALUE", help=_set_help
    )

    p_xml2json = sub.add_parser("xml2json", help="Convert AEMXML to simplified JSON")
    p_xml2json.add_argument("input", help="Input .aemxml file")
    p_xml2json.add_argument("output", help="Output .json file")

    p_upgrade = sub.add_parser(
        "upgrade", help="Convert AEMXML from 2013 to 2021 schema"
    )
    p_upgrade.add_argument("input", help="Input .aemxml file")
    p_upgrade.add_argument(
        "-o", "--output", help="Output .aemxml file (default: stdout)"
    )

    p_downgrade = sub.add_parser(
        "downgrade", help="Convert AEMXML from 2021 to 2013 schema"
    )
    p_downgrade.add_argument("input", help="Input .aemxml file")
    p_downgrade.add_argument(
        "-o", "--output", help="Output .aemxml file (default: stdout)"
    )
    p_downgrade.add_argument(
        "--strip",
        action="store_true",
        help="Remove 2021-only descriptors (Timing, PtpInstance) instead of failing",
    )

    args = parser.parse_args()
    {
        "xml2bin": cmd_xml2bin,
        "bin2xml": cmd_bin2xml,
        "bin2json": cmd_bin2json,
        "validate": cmd_validate,
        "dump": cmd_dump,
        "json2bin": cmd_json2bin,
        "json2xml": cmd_json2xml,
        "xml2json": cmd_xml2json,
        "upgrade": cmd_upgrade,
        "downgrade": cmd_downgrade,
    }[args.command](args)


if __name__ == "__main__":
    main()
