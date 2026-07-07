# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

"""Typed control value types per IEEE 1722.1 Clause 7.3.5.

Each control value type has a corresponding dataclass with proper
field-level serialization/deserialization. The value_details field
in Control, Mixer, Matrix, and SignalTranscoder descriptors can be
parsed/serialized via these types.

Wire format for LINEAR types (per item):
  current (N bytes) | minimum (N bytes) | maximum (N bytes) | step (N bytes) |
  default_value (N bytes) | unit (2 bytes) | string_ref (2 bytes)

Wire format for SELECTOR types (per item):
  current (N bytes) | default_value (N bytes) | number_of_options (2 bytes) |
  options[...] (N bytes each) | unit (2 bytes) | string_ref (2 bytes)
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from enum import IntEnum


class ControlValueType(IntEnum):
    """Control value type codes — IEEE 1722.1 Clause 7.3.5."""

    LINEAR_INT8 = 0x0000
    LINEAR_UINT8 = 0x0001
    LINEAR_INT16 = 0x0002
    LINEAR_UINT16 = 0x0003
    LINEAR_INT32 = 0x0004
    LINEAR_UINT32 = 0x0005
    LINEAR_INT64 = 0x0006
    LINEAR_UINT64 = 0x0007
    LINEAR_FLOAT = 0x0008
    LINEAR_DOUBLE = 0x0009
    SELECTOR_INT8 = 0x000A
    SELECTOR_UINT8 = 0x000B
    SELECTOR_INT16 = 0x000C
    SELECTOR_UINT16 = 0x000D
    SELECTOR_INT32 = 0x000E
    SELECTOR_UINT32 = 0x000F
    SELECTOR_INT64 = 0x0010
    SELECTOR_UINT64 = 0x0011
    SELECTOR_FLOAT = 0x0012
    SELECTOR_DOUBLE = 0x0013
    SELECTOR_STRING = 0x0014
    ARRAY_INT8 = 0x0015
    ARRAY_UINT8 = 0x0016
    ARRAY_INT16 = 0x0017
    ARRAY_UINT16 = 0x0018
    ARRAY_INT32 = 0x0019
    ARRAY_UINT32 = 0x001A
    ARRAY_INT64 = 0x001B
    ARRAY_UINT64 = 0x001C
    ARRAY_FLOAT = 0x001D
    ARRAY_DOUBLE = 0x001E
    UTF8 = 0x001F
    BODE_PLOT = 0x0020
    SMPTE_TIME = 0x0021
    SAMPLE_RATE = 0x0022
    GPTP_TIME = 0x0023
    VENDOR = 0x3FFE
    EXPANSION = 0x3FFF


class UnitsCode(IntEnum):
    """Unit codes — IEEE 1722.1 Clause 7.3.3."""

    UNITLESS = 0x00
    COUNT = 0x01
    PERCENT = 0x02
    FSTOP = 0x03
    TIME_SECONDS = 0x08
    TIME_MINUTES = 0x09
    TIME_HOURS = 0x0A
    TIME_DAYS = 0x0B
    TIME_MONTHS = 0x0C
    TIME_YEARS = 0x0D
    TIME_SAMPLES = 0x0E
    TIME_FRAMES = 0x0F
    FREQUENCY_HERTZ = 0x10
    FREQUENCY_SEMITONES = 0x11
    FREQUENCY_CENTS = 0x12
    FREQUENCY_OCTAVES = 0x13
    FREQUENCY_FPS = 0x14
    DISTANCE_METRES = 0x18
    TEMPERATURE_KELVIN = 0x20
    MASS_GRAMS = 0x28
    VOLTAGE_VOLTS = 0x30
    VOLTAGE_DBV = 0x31
    VOLTAGE_DBU = 0x32
    CURRENT_AMPS = 0x38
    POWER_WATTS = 0x40
    POWER_DBM = 0x41
    POWER_DBW = 0x42
    PRESSURE_PASCALS = 0x48
    MEMORY_BITS = 0x50
    MEMORY_BYTES = 0x51
    LEVEL_DB = 0xB0
    LEVEL_DB_PEAK = 0xB1
    LEVEL_DB_RMS = 0xB2
    LEVEL_DBFS = 0xB3
    LEVEL_DBFS_PEAK = 0xB4
    LEVEL_DBFS_RMS = 0xB5
    LEVEL_DBTP = 0xB6
    LEVEL_DBA = 0xB7
    LEVEL_DBB = 0xB8
    LEVEL_DBC = 0xB9
    LEVEL_DBSPL = 0xBA
    LEVEL_LU = 0xBB
    LEVEL_LUFS = 0xBC


# Type metadata: (struct_format, size_in_bytes, is_signed)
_LINEAR_TYPE_INFO: dict[int, tuple[str, int]] = {
    ControlValueType.LINEAR_INT8: (">b", 1),
    ControlValueType.LINEAR_UINT8: (">B", 1),
    ControlValueType.LINEAR_INT16: (">h", 2),
    ControlValueType.LINEAR_UINT16: (">H", 2),
    ControlValueType.LINEAR_INT32: (">i", 4),
    ControlValueType.LINEAR_UINT32: (">I", 4),
    ControlValueType.LINEAR_INT64: (">q", 8),
    ControlValueType.LINEAR_UINT64: (">Q", 8),
    ControlValueType.LINEAR_FLOAT: (">f", 4),
    ControlValueType.LINEAR_DOUBLE: (">d", 8),
}

_SELECTOR_TYPE_INFO: dict[int, tuple[str, int]] = {
    ControlValueType.SELECTOR_INT8: (">b", 1),
    ControlValueType.SELECTOR_UINT8: (">B", 1),
    ControlValueType.SELECTOR_INT16: (">h", 2),
    ControlValueType.SELECTOR_UINT16: (">H", 2),
    ControlValueType.SELECTOR_INT32: (">i", 4),
    ControlValueType.SELECTOR_UINT32: (">I", 4),
    ControlValueType.SELECTOR_INT64: (">q", 8),
    ControlValueType.SELECTOR_UINT64: (">Q", 8),
    ControlValueType.SELECTOR_FLOAT: (">f", 4),
    ControlValueType.SELECTOR_DOUBLE: (">d", 8),
}


# Standard AEM control_type EUI-64 values — IEEE 1722.1 Clause 7.3.4.
# Keep in lockstep with statusbar/atdecc/atdecc_aem_control_types.hpp.
_CT = 0x90E0F00000000000  # standard control-type OUI-24 base (90:e0:f0)
CONTROL_TYPE_NAMES: dict[str, int] = {
    "ENABLE": _CT | 0x00,
    "IDENTIFY": _CT | 0x01,
    "MUTE": _CT | 0x02,
    "INVERT": _CT | 0x03,
    "GAIN": _CT | 0x04,
    "ATTENUATE": _CT | 0x05,
    "DELAY": _CT | 0x06,
    "SRC_MODE": _CT | 0x07,
    "SNAPSHOT": _CT | 0x08,
    "POW_LINE_FREQ": _CT | 0x09,
    "POWER_STATUS": _CT | 0x0A,
    "FAN_STATUS": _CT | 0x0B,
    "TEMPERATURE": _CT | 0x0C,
    "ALTITUDE": _CT | 0x0D,
    "ABSOLUTE_HUMIDITY": _CT | 0x0E,
    "RELATIVE_HUMIDITY": _CT | 0x0F,
    "ORIENTATION": _CT | 0x10,
    "VELOCITY": _CT | 0x11,
    "ACCELERATION": _CT | 0x12,
    "FILTER_RESPONSE": _CT | 0x13,
    "BAROMETRIC_PRESSURE": _CT | 0x14,
    "MANUFACTURER_URL": _CT | 0x15,
    "ENTITY_URL": _CT | 0x16,
    "CONFIGURATION_URL": _CT | 0x17,
    "GENERIC_URL": _CT | 0x18,
    "FAULT": _CT | 0x19,
    "CONTROLLER_TARGET_ENTITY": _CT | 0x1A,
    "CONTROLLER_TARGET_OBJECT": _CT | 0x1B,
    "LATENCY_COMPENSATION": _CT | 0x1C,
    "PANPOT": _CT | 0x00010000,
    "PHANTOM": _CT | 0x00010001,
    "AUDIO_SCALE": _CT | 0x00010002,
    "AUDIO_METERS": _CT | 0x00010003,
    "AUDIO_SPECTRUM": _CT | 0x00010004,
    "SCANNING_MODE": _CT | 0x00020000,
    "AUTO_EXP_MODE": _CT | 0x00020001,
    "AUTO_EXP_PRIO": _CT | 0x00020002,
    "EXP_TIME": _CT | 0x00020003,
    "FOCUS": _CT | 0x00020004,
    "FOCUS_AUTO": _CT | 0x00020005,
    "IRIS": _CT | 0x00020006,
    "ZOOM": _CT | 0x00020007,
    "PRIVACY": _CT | 0x00020008,
    "BACKLIGHT": _CT | 0x00020009,
    "BRIGHTNESS": _CT | 0x0002000A,
    "CONTRAST": _CT | 0x0002000B,
    "HUE": _CT | 0x0002000C,
    "SATURATION": _CT | 0x0002000D,
    "SHARPNESS": _CT | 0x0002000E,
    "GAMMA": _CT | 0x0002000F,
    "WHITE_BAL_TEMP": _CT | 0x00020010,
    "WHITE_BAL_TEMP_AUTO": _CT | 0x00020011,
    "WHITE_BAL_COMP": _CT | 0x00020012,
    "WHITE_BAL_COMP_AUTO": _CT | 0x00020013,
    "DIGITAL_ZOOM": _CT | 0x00020014,
    "MEDIA_PLAYLIST": _CT | 0x00030000,
    "MEDIA_PLAYLIST_NAME": _CT | 0x00030001,
    "MEDIA_DISK": _CT | 0x00030002,
    "MEDIA_DISK_NAME": _CT | 0x00030003,
    "MEDIA_TRACK": _CT | 0x00030004,
    "MEDIA_TRACK_NAME": _CT | 0x00030005,
    "MEDIA_SPEED": _CT | 0x00030006,
    "MEDIA_SAMPLE_POSITION": _CT | 0x00030007,
    "MEDIA_PLAYBACK_TRANSPORT": _CT | 0x00030008,
    "MEDIA_RECORD_TRANSPORT": _CT | 0x00030009,
    "FREQUENCY": _CT | 0x00040000,
    "MODULATION": _CT | 0x00040001,
    "POLARIZATION": _CT | 0x00040002,
    "BAUD_RATE": _CT | 0x00050000,
    "BIT_WIDTH": _CT | 0x00050001,
    "PARITY": _CT | 0x00050002,
    "STOP_BITS": _CT | 0x00050003,
    "INTERFACE_OPERATIONAL": _CT | 0x00060000,
    "INTERFACE_MEDIA_OPTIONS": _CT | 0x00060001,
    "INTERFACE_MEDIA_STATUS": _CT | 0x00060002,
    "INTERFACE_NETWORK_NAME": _CT | 0x00060003,
    "FQTSS_DELTA_BANDWIDTH": _CT | 0x00060004,
    "FQTSS_ADMIN_IDLE_SLOPE": _CT | 0x00060005,
    "FQTSS_OPER_IDLE_SLOPE": _CT | 0x00060006,
    "FQTSS_PORT_TRANSMIT_RATE": _CT | 0x00060007,
    "FQTSS_CLASS_MEASUREMENT_INTERVAL": _CT | 0x00060008,
    "FQTSS_LOCK_CLASS_BANDWIDTH": _CT | 0x00060009,
}


@dataclass
class LinearValue:
    """A single LINEAR control value item."""

    current: int | float = 0
    minimum: int | float = 0
    maximum: int | float = 0
    step: int | float = 0
    default_value: int | float = 0
    unit: int = 0
    string_ref: int = 0


@dataclass
class SelectorValue:
    """A single SELECTOR control value item."""

    current: int | float = 0
    default_value: int | float = 0
    options: list[int | float] = field(default_factory=list)
    unit: int = 0
    string_ref: int = 0


@dataclass
class Utf8Value:
    """A UTF8 control value."""

    text: str = ""


@dataclass
class VendorValue:
    """A VENDOR (0x3FFE) control value — opaque bytes."""

    data: bytes = b""


@dataclass
class UnknownValue:
    """An unrecognised control value type — opaque bytes."""

    data: bytes = b""


def parse_linear_values(
    control_value_type: int, data: bytes, number_of_values: int
) -> list[LinearValue]:
    """Parse LINEAR control value items from wire bytes."""
    info = _LINEAR_TYPE_INFO.get(control_value_type)
    if info is None:
        return []
    fmt, val_size = info
    # Each item: 5 values + unit(2) + string_ref(2)
    item_size = val_size * 5 + 4
    values = []
    offset = 0
    for _ in range(number_of_values):
        if offset + item_size > len(data):
            break
        current = struct.unpack_from(fmt, data, offset)[0]
        minimum = struct.unpack_from(fmt, data, offset + val_size)[0]
        maximum = struct.unpack_from(fmt, data, offset + val_size * 2)[0]
        step = struct.unpack_from(fmt, data, offset + val_size * 3)[0]
        default_value = struct.unpack_from(fmt, data, offset + val_size * 4)[0]
        unit = struct.unpack_from(">H", data, offset + val_size * 5)[0]
        string_ref = struct.unpack_from(">H", data, offset + val_size * 5 + 2)[0]
        values.append(
            LinearValue(
                current, minimum, maximum, step, default_value, unit, string_ref
            )
        )
        offset += item_size
    return values


def serialize_linear_values(
    control_value_type: int, values: list[LinearValue]
) -> bytes:
    """Serialize LINEAR control value items to wire bytes."""
    info = _LINEAR_TYPE_INFO.get(control_value_type)
    if info is None:
        return b""
    fmt, _ = info
    parts: list[bytes] = []
    for v in values:
        parts.append(struct.pack(fmt, v.current))
        parts.append(struct.pack(fmt, v.minimum))
        parts.append(struct.pack(fmt, v.maximum))
        parts.append(struct.pack(fmt, v.step))
        parts.append(struct.pack(fmt, v.default_value))
        parts.append(struct.pack(">HH", v.unit, v.string_ref))
    return b"".join(parts)


def parse_selector_values(
    control_value_type: int, data: bytes, number_of_values: int
) -> list[SelectorValue]:
    """Parse SELECTOR control value items from wire bytes."""
    info = _SELECTOR_TYPE_INFO.get(control_value_type)
    if info is None:
        return []
    fmt, val_size = info
    values = []
    offset = 0
    for _ in range(number_of_values):
        if offset + val_size * 2 + 2 > len(data):
            break
        current = struct.unpack_from(fmt, data, offset)[0]
        default_value = struct.unpack_from(fmt, data, offset + val_size)[0]
        num_options = struct.unpack_from(">H", data, offset + val_size * 2)[0]
        offset += val_size * 2 + 2
        options = []
        for _ in range(num_options):
            if offset + val_size > len(data):
                break
            options.append(struct.unpack_from(fmt, data, offset)[0])
            offset += val_size
        # unit + string_ref after options
        unit = 0
        string_ref = 0
        if offset + 4 <= len(data):
            unit = struct.unpack_from(">H", data, offset)[0]
            string_ref = struct.unpack_from(">H", data, offset + 2)[0]
            offset += 4
        values.append(SelectorValue(current, default_value, options, unit, string_ref))
    return values


def serialize_selector_values(
    control_value_type: int, values: list[SelectorValue]
) -> bytes:
    """Serialize SELECTOR control value items to wire bytes."""
    info = _SELECTOR_TYPE_INFO.get(control_value_type)
    if info is None:
        return b""
    fmt, _ = info
    parts: list[bytes] = []
    for v in values:
        parts.append(struct.pack(fmt, v.current))
        parts.append(struct.pack(fmt, v.default_value))
        parts.append(struct.pack(">H", len(v.options)))
        for opt in v.options:
            parts.append(struct.pack(fmt, opt))
        parts.append(struct.pack(">HH", v.unit, v.string_ref))
    return b"".join(parts)


def is_linear_type(control_value_type: int) -> bool:
    """True if the base type (flag bits masked) is a LINEAR_* value type."""
    return (control_value_type & 0x3FFF) in _LINEAR_TYPE_INFO


def linear_item_size(control_value_type: int) -> int | None:
    """Wire size of one LINEAR value item (5 fields + unit + string_ref), or
    None if the type is not LINEAR."""
    info = _LINEAR_TYPE_INFO.get(control_value_type & 0x3FFF)
    return None if info is None else info[1] * 5 + 4


def is_selector_type(control_value_type: int) -> bool:
    """True if the base type (flag bits masked) is a numeric SELECTOR_* type."""
    return (control_value_type & 0x3FFF) in _SELECTOR_TYPE_INFO


def count_values(control_value_type: int, data: bytes) -> int:
    """Number of value items encoded in value_details wire bytes — the CONTROL
    descriptor's number_of_values field. LINEAR items are fixed-size; SELECTOR
    items are walked (variable-length options list); UTF8 and VENDOR/unknown
    payloads count as one value when non-empty."""
    if not data:
        return 0
    base_type = control_value_type & 0x3FFF
    info = _LINEAR_TYPE_INFO.get(base_type)
    if info is not None:
        item_size = info[1] * 5 + 4
        return len(data) // item_size
    info = _SELECTOR_TYPE_INFO.get(base_type)
    if info is not None:
        val_size = info[1]
        count = 0
        offset = 0
        while offset + val_size * 2 + 2 <= len(data):
            num_options = struct.unpack_from(">H", data, offset + val_size * 2)[0]
            offset += val_size * 2 + 2 + num_options * val_size + 4
            count += 1
        return count
    return 1


def parse_value_details(
    control_value_type: int, data: bytes, number_of_values: int = 1
) -> list[LinearValue] | list[SelectorValue] | Utf8Value | VendorValue | UnknownValue:
    """Parse value_details bytes into typed control values."""
    base_type = control_value_type & 0x3FFF
    if base_type in _LINEAR_TYPE_INFO:
        return parse_linear_values(base_type, data, number_of_values)
    if base_type in _SELECTOR_TYPE_INFO:
        return parse_selector_values(base_type, data, number_of_values)
    if base_type == ControlValueType.UTF8:
        text = data.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
        return Utf8Value(text)
    if base_type == ControlValueType.VENDOR:
        return VendorValue(data)
    return UnknownValue(data)


def serialize_value_details(
    control_value_type: int,
    values: list[LinearValue]
    | list[SelectorValue]
    | Utf8Value
    | VendorValue
    | UnknownValue,
) -> bytes:
    """Serialize typed control values back to wire bytes."""
    base_type = control_value_type & 0x3FFF
    if base_type in _LINEAR_TYPE_INFO and isinstance(values, list):
        return serialize_linear_values(base_type, values)  # type: ignore[arg-type]
    if base_type in _SELECTOR_TYPE_INFO and isinstance(values, list):
        return serialize_selector_values(base_type, values)  # type: ignore[arg-type]
    if isinstance(values, Utf8Value):
        return values.text.encode("utf-8") + b"\x00"
    if isinstance(values, VendorValue):
        return values.data
    if isinstance(values, UnknownValue):
        return values.data
    return b""
