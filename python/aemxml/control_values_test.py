#!/usr/bin/env python3
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Tests for typed control value serialization/deserialization."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from aemxml.control_values import (
    ControlValueType,
    LinearValue,
    SelectorValue,
    Utf8Value,
    VendorValue,
    UnknownValue,
    parse_linear_values,
    serialize_linear_values,
    parse_selector_values,
    serialize_selector_values,
    parse_value_details,
    serialize_value_details,
    count_values,
)


def test_linear_int16_roundtrip():
    values = [
        LinearValue(
            current=100,
            minimum=-1000,
            maximum=1000,
            step=1,
            default_value=0,
            unit=0xB0,
            string_ref=0,
        ),
        LinearValue(
            current=200,
            minimum=-500,
            maximum=500,
            step=10,
            default_value=0,
            unit=0xB0,
            string_ref=1,
        ),
    ]
    data = serialize_linear_values(ControlValueType.LINEAR_INT16, values)
    parsed = parse_linear_values(ControlValueType.LINEAR_INT16, data, 2)
    assert len(parsed) == 2
    assert parsed[0].current == 100
    assert parsed[0].minimum == -1000
    assert parsed[0].maximum == 1000
    assert parsed[0].unit == 0xB0
    assert parsed[1].current == 200
    assert parsed[1].string_ref == 1


def test_linear_float_roundtrip():
    values = [
        LinearValue(
            current=0.5,
            minimum=0.0,
            maximum=1.0,
            step=0.01,
            default_value=0.75,
            unit=2,
            string_ref=0,
        )
    ]
    data = serialize_linear_values(ControlValueType.LINEAR_FLOAT, values)
    parsed = parse_linear_values(ControlValueType.LINEAR_FLOAT, data, 1)
    assert len(parsed) == 1
    assert abs(parsed[0].current - 0.5) < 1e-6
    assert abs(parsed[0].maximum - 1.0) < 1e-6


def test_linear_uint32_roundtrip():
    values = [
        LinearValue(
            current=48000,
            minimum=8000,
            maximum=192000,
            step=1,
            default_value=48000,
            unit=0x10,
            string_ref=0,
        )
    ]
    data = serialize_linear_values(ControlValueType.LINEAR_UINT32, values)
    parsed = parse_linear_values(ControlValueType.LINEAR_UINT32, data, 1)
    assert parsed[0].current == 48000
    assert parsed[0].maximum == 192000


def test_selector_uint16_roundtrip():
    values = [
        SelectorValue(
            current=48000,
            default_value=48000,
            options=[44100, 48000, 32000],
            unit=0x10,
        )
    ]
    data = serialize_selector_values(ControlValueType.SELECTOR_UINT16, values)
    # number_of_values for SELECTOR types is the option count.
    parsed = parse_selector_values(ControlValueType.SELECTOR_UINT16, data, 3)
    assert len(parsed) == 1
    assert parsed[0].current == 48000
    assert parsed[0].options == [44100, 48000, 32000]


def test_linear_wire_field_order():
    """Byte-exact spec order (IEEE 1722.1-2021 Table 7-14): min, max, step,
    default, current, unit, string. Round-trip tests alone cannot catch a
    symmetric order bug — this is what made macOS reject the IDENTIFY
    control (it parsed max=0, default=255)."""
    identify = [
        LinearValue(
            minimum=0,
            maximum=255,
            step=255,
            default_value=0,
            current=0,
            unit=0x0000,
            string_ref=0xFFFF,
        )
    ]
    data = serialize_linear_values(ControlValueType.LINEAR_UINT8, identify)
    assert data == bytes.fromhex("00 ff ff 00 00 00 00 ff ff".replace(" ", ""))
    wide = [
        LinearValue(
            minimum=1,
            maximum=2,
            step=3,
            default_value=4,
            current=5,
            unit=0xB0,
            string_ref=0x1234,
        )
    ]
    data = serialize_linear_values(ControlValueType.LINEAR_UINT16, wide)
    assert data == bytes.fromhex("00010002000300040005" + "00b0" + "1234")


def test_selector_wire_layout():
    """Byte-exact spec layout (IEEE 1722.1-2021 Table 7-15): current, default,
    options..., unit — no embedded option count and no string_ref field; the
    CONTROL descriptor's number_of_values carries the option count."""
    values = [SelectorValue(current=1, default_value=0, options=[0, 1, 2], unit=0x01)]
    data = serialize_selector_values(ControlValueType.SELECTOR_UINT16, values)
    assert data == bytes.fromhex("0001" + "0000" + "000000010002" + "0001")
    # (N + 2) * V + 2, matching the C++ side's length formula.
    assert len(data) == (3 + 2) * 2 + 2
    assert count_values(ControlValueType.SELECTOR_UINT16, data) == 3


def test_parse_value_details_linear():
    values = [
        LinearValue(
            current=42,
            minimum=0,
            maximum=100,
            step=1,
            default_value=50,
            unit=0,
            string_ref=0,
        )
    ]
    data = serialize_linear_values(ControlValueType.LINEAR_INT32, values)
    result = parse_value_details(ControlValueType.LINEAR_INT32, data, 1)
    assert isinstance(result, list)
    assert result[0].current == 42


def test_parse_value_details_utf8():
    data = b"Hello World\x00"
    result = parse_value_details(ControlValueType.UTF8, data)
    assert isinstance(result, Utf8Value)
    assert result.text == "Hello World"


def test_parse_value_details_vendor():
    data = b"\x01\x02\x03\x04"
    result = parse_value_details(ControlValueType.VENDOR, data)
    assert isinstance(result, VendorValue)
    assert result.data == data


def test_parse_value_details_unknown():
    data = b"\xff\xfe\xfd"
    result = parse_value_details(0x2000, data)
    assert isinstance(result, UnknownValue)


def test_serialize_value_details_roundtrip():
    values = [
        LinearValue(
            current=-10,
            minimum=-128,
            maximum=127,
            step=1,
            default_value=0,
            unit=0,
            string_ref=0,
        )
    ]
    data = serialize_value_details(ControlValueType.LINEAR_INT8, values)
    parsed = parse_value_details(ControlValueType.LINEAR_INT8, data, 1)
    assert isinstance(parsed, list)
    assert parsed[0].current == -10
    assert parsed[0].minimum == -128


def test_readonly_flag_stripped():
    """READONLY flag (0x8000) should be stripped for type dispatch."""
    values = [
        LinearValue(
            current=50,
            minimum=0,
            maximum=100,
            step=1,
            default_value=50,
            unit=0,
            string_ref=0,
        )
    ]
    data = serialize_value_details(ControlValueType.LINEAR_UINT8, values)
    parsed = parse_value_details(ControlValueType.LINEAR_UINT8 | 0x8000, data, 1)
    assert isinstance(parsed, list)
    assert parsed[0].current == 50


def test_count_values():
    """count_values derives the number_of_values item count from wire bytes."""
    # LINEAR: fixed item size (5 fields + unit + string_ref).
    two = serialize_linear_values(
        ControlValueType.LINEAR_UINT8, [LinearValue(), LinearValue()]
    )
    assert count_values(ControlValueType.LINEAR_UINT8, two) == 2
    one64 = serialize_linear_values(ControlValueType.LINEAR_INT64, [LinearValue()])
    assert count_values(ControlValueType.LINEAR_INT64, one64) == 1
    # Flag bits are masked off.
    assert count_values(0x8000 | ControlValueType.LINEAR_UINT8, two) == 2
    # SELECTOR: number_of_values is the option count, recovered from length.
    sel = serialize_selector_values(
        ControlValueType.SELECTOR_UINT16,
        [SelectorValue(options=[1, 2, 3])],
    )
    assert count_values(ControlValueType.SELECTOR_UINT16, sel) == 3
    # UTF8 / VENDOR payloads are one value; empty payloads are zero.
    assert count_values(ControlValueType.UTF8, b"hi\x00") == 1
    assert count_values(ControlValueType.VENDOR, b"\x01\x02") == 1
    assert count_values(ControlValueType.LINEAR_UINT8, b"") == 0


def main():
    tests = [
        test_linear_int16_roundtrip,
        test_linear_float_roundtrip,
        test_linear_uint32_roundtrip,
        test_selector_uint16_roundtrip,
        test_linear_wire_field_order,
        test_selector_wire_layout,
        test_parse_value_details_linear,
        test_parse_value_details_utf8,
        test_parse_value_details_vendor,
        test_parse_value_details_unknown,
        test_serialize_value_details_roundtrip,
        test_readonly_flag_stripped,
        test_count_values,
    ]
    passed = 0
    failed = 0
    for test in tests:
        try:
            test()
            passed += 1
            print(f"  PASS  {test.__name__}")
        except Exception as e:
            failed += 1
            print(f"  FAIL  {test.__name__}: {e}")

    print(f"\n{passed} passed, {failed} failed out of {len(tests)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
