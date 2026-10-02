# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Wire layouts: fields, layouts and value tables.

A ``Field`` is one dissectable value: its filter abbreviation, display name,
byte offset and length within its layout, wire type, an optional bit mask
inside the containing integer, display base and value-name table. A
``Layout`` is an ordered list of fields with a shared abbreviation prefix.
Everything is authored from IEEE Std 1722-2025 / 1722.1 and this repository's
C++ wire structs - never from another dissector.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class Field:
    """One field of a layout."""

    name: str
    offset: int
    length: int
    kind: str  # u8 u16 u32 u64 bool bytes eui48
    doc: str = ""
    mask: int | None = None
    base: str = "dec"  # dec hex
    values: str | None = None  # name of a value table in VALUE_TABLES

    @property
    def shift(self) -> int:
        """Right shift that brings a masked field down to bit 0."""
        if self.mask is None:
            return 0
        shift = 0
        mask = self.mask
        while mask & 1 == 0:
            mask >>= 1
            shift += 1
        return shift


@dataclass(frozen=True)
class Layout:
    """A named group of fields sharing an abbreviation prefix."""

    name: str
    prefix: str
    doc: str
    fields: tuple[Field, ...]

    def abbr(self, f: Field) -> str:
        """The Wireshark filter name of ``f`` in this layout."""
        return f"{self.prefix}.{f.name}"


@dataclass(frozen=True)
class ValueTable:
    """A code -> name table used for display and for the reference decoder."""

    name: str
    entries: dict[int, str] = field(default_factory=dict)


# IEEE 1722-2025 Table 6 - AVTP subtypes, with the header kind the C++
# avtp_get_header_type() assigns (stream / control / alternative).
AVTP_SUBTYPES: list[tuple[int, str, str, str]] = [
    (0x00, "iec_61883_iidc", "IEC 61883/IIDC", "stream"),
    (0x01, "mma_stream", "MMA Stream", "stream"),
    (0x02, "aaf", "AVTP Audio Format", "stream"),
    (0x03, "cvf", "Compressed Video Format", "stream"),
    (0x04, "crf", "Clock Reference Format", "alternative"),
    (0x05, "tscf", "Time-Synchronous Control Format", "stream"),
    (0x06, "svf", "SDI Video Format", "stream"),
    (0x07, "rvf", "Raw Video Format", "stream"),
    (0x6E, "aef_continuous", "AES Encrypted Format Continuous", "alternative"),
    (0x6F, "vsf_stream", "Vendor Specific Format Stream", "stream"),
    (0x7F, "ef_stream", "Experimental Format Stream", "stream"),
    (0x82, "ntscf", "Non-Time-Synchronous Control Format", "alternative"),
    (0xEC, "escf", "ECC Signed Control Format", "alternative"),
    (0xED, "eecf", "ECC Encrypted Control Format", "alternative"),
    (0xEE, "aef_discrete", "AES Encrypted Format Discrete", "alternative"),
    (0xFA, "adp", "ATDECC Discovery Protocol", "control"),
    (0xFB, "aecp", "ATDECC Enumeration and Control Protocol", "control"),
    (0xFC, "acmp", "ATDECC Connection Management Protocol", "control"),
    (0xFE, "maap", "MAAP", "control"),
    (0xFF, "ef_control", "Experimental Format Control", "control"),
]

VALUE_TABLES: dict[str, ValueTable] = {
    "avtp_subtype": ValueTable(
        "avtp_subtype", {code: title for code, _, title, _ in AVTP_SUBTYPES}
    ),
}

HEADER_KIND: dict[int, str] = {code: kind for code, _, _, kind in AVTP_SUBTYPES}


def _common() -> tuple[Field, ...]:
    """Bytes 0-1 and 4-11 that every AVTPDU shares (4.4.3)."""
    return (
        Field(
            "subtype",
            0,
            1,
            "u8",
            "AVTP subtype (Table 6)",
            base="hex",
            values="avtp_subtype",
        ),
        Field("sv", 1, 1, "bool", "stream_id valid", mask=0x80),
        Field("version", 1, 1, "u8", "AVTP version", mask=0x70),
    )


STREAM_ID = Field("stream_id", 4, 8, "u64", "stream_id", base="hex")

# Stream data header, version 0 (4.4.4.1): 16 octets.
AVTP_STREAM_V0 = Layout(
    "stream_v0",
    "avb.avtp",
    "AVTP stream data header, version 0 (IEEE 1722-2025 4.4.4.1)",
    _common()
    + (
        Field("mr", 1, 1, "bool", "media clock restart", mask=0x08),
        Field("r", 1, 1, "u8", "reserved", mask=0x04),
        Field("fsd1", 1, 1, "u8", "format specific data (gv / fs / ...)", mask=0x02),
        Field("tv", 1, 1, "bool", "avtp_timestamp valid", mask=0x01),
        Field("sequence_num", 2, 1, "u8", "sequence_num (8-bit)"),
        Field(
            "fsd2",
            3,
            1,
            "u8",
            "format specific data (reserved / crf_type / ...)",
            mask=0xFE,
        ),
        Field("tu", 3, 1, "bool", "timestamp uncertain", mask=0x01),
        STREAM_ID,
        Field("avtp_timestamp", 12, 4, "u32", "avtp_timestamp (32-bit, ns)"),
    ),
)

# Stream data header, version 1 (4.7.4): 32 octets before the format-specific part.
AVTP_STREAM_V1 = Layout(
    "stream_v1",
    "avb.avtp",
    "AVTP stream data header, version 1 (IEEE 1722-2025 4.7.4)",
    _common()
    + (
        Field("mr", 1, 1, "bool", "media clock restart", mask=0x08),
        Field("r", 1, 1, "u8", "reserved", mask=0x04),
        Field("fsd1", 1, 1, "u8", "format specific data (gv / fs / ...)", mask=0x02),
        Field("tv", 1, 1, "bool", "avtp_timestamp valid", mask=0x01),
        Field(
            "format_specific_data_0", 2, 1, "u8", "format_specific_data_0", base="hex"
        ),
        Field(
            "fsd2",
            3,
            1,
            "u8",
            "format specific data (reserved / crf_type / ...)",
            mask=0xFE,
        ),
        Field("tu", 3, 1, "bool", "timestamp uncertain", mask=0x01),
        STREAM_ID,
        Field("sequence_num32", 12, 4, "u32", "sequence_num (32-bit)"),
        Field("avtp_timestamp64", 16, 8, "u64", "avtp_timestamp (64-bit, ns)"),
        Field(
            "ptp_grandmaster_identity",
            24,
            8,
            "u64",
            "ptp_grandmaster_identity",
            base="hex",
        ),
    ),
)

# Control data header (4.4.5): ADP, AECP, ACMP, MAAP, EF_CONTROL.
AVTP_CONTROL = Layout(
    "control",
    "avb.avtp",
    "AVTP control data header (IEEE 1722-2025 4.4.5)",
    _common()
    + (
        Field("control_data", 1, 1, "u8", "control_data / message_type", mask=0x0F),
        Field("status", 2, 2, "u8", "status (5 bits)", mask=0xF800),
        Field(
            "control_data_length",
            2,
            2,
            "u16",
            "control_data_length (11 bits)",
            mask=0x07FF,
        ),
        STREAM_ID,
    ),
)

# Alternative header (4.4.6): CRF, NTSCF, AEF, ESCF, EECF.
AVTP_ALTERNATIVE = Layout(
    "alternative",
    "avb.avtp",
    "AVTP alternative header (IEEE 1722-2025 4.4.6)",
    _common()
    + (
        Field(
            "subtype_data1",
            1,
            1,
            "u8",
            "subtype_data_1 (4 bits)",
            mask=0x0F,
            base="hex",
        ),
        Field("subtype_data2", 2, 2, "u16", "subtype_data_2 (16 bits)", base="hex"),
        STREAM_ID,
    ),
)

# Common prefix only, for reserved subtypes.
AVTP_COMMON = Layout(
    "common",
    "avb.avtp",
    "AVTP common header prefix (IEEE 1722-2025 4.4.3)",
    _common() + (STREAM_ID,),
)

# Annex J IP encapsulation: the IP AVTPDU header before the AVTPDU.
IP_AVTPDU = Layout(
    "ip_avtpdu",
    "avb.ipavtp",
    "IP AVTPDU header (IEEE 1722-2025 Annex J)",
    (Field("encapsulation_sequence_num", 0, 4, "u32", "encapsulation_sequence_num"),),
)

# Bytes after the decoded header when no subtype dissector exists yet.
PAYLOAD = Field("payload", 0, 0, "bytes", "undissected AVTPDU payload")

AVTP_LAYOUTS: tuple[Layout, ...] = (
    AVTP_COMMON,
    AVTP_STREAM_V0,
    AVTP_STREAM_V1,
    AVTP_CONTROL,
    AVTP_ALTERNATIVE,
)
