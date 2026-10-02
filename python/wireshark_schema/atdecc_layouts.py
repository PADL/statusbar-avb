# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""ATDECC (IEEE 1722.1) layouts and dissection specs built from atdecc_table.py.

Everything the Lua generator and the reference decoder need to agree on lives
here: the Layout of every PDU, AEM payload struct and descriptor (with
bit-field expansions for the packed header fields and the capability/flag
words), plus the small dispatch tables (which payload a command carries, how a
descriptor's trailer is counted).
"""

from __future__ import annotations

import re
from dataclasses import dataclass

from . import atdecc_table as T
from .layouts import VALUE_TABLES, Field, Layout, ValueTable

PREFIX = "avb.atdecc"


def _snake(struct_name: str) -> str:
    """AemGetStreamInfoCommandPayload -> get_stream_info_command; DescriptorAudioUnit -> audio_unit."""
    name = re.sub(r"^(Aem|Descriptor)", "", struct_name)
    name = re.sub(r"(Payload|Header)$", "", name)
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


# ---------------------------------------------------------------------------
# Value tables
# ---------------------------------------------------------------------------

VALUE_TABLES["aem_command"] = ValueTable(
    "aem_command", {int(k): v for k, v in T.AEM_COMMANDS.items()}
)
VALUE_TABLES["descriptor_type"] = ValueTable(
    "descriptor_type", {int(k): v for k, v in T.DESCRIPTOR_TYPES.items()}
)
VALUE_TABLES["aem_status"] = ValueTable(
    "aem_status", {int(k): v for k, v in T.AEM_STATUS.items()}
)
VALUE_TABLES["aecp_message_type"] = ValueTable(
    "aecp_message_type", {int(k): v for k, v in T.AECP_MESSAGE_TYPES.items()}
)
VALUE_TABLES["acmp_message_type"] = ValueTable(
    "acmp_message_type", {int(k): v for k, v in T.ACMP_MESSAGE_TYPES.items()}
)
VALUE_TABLES["acmp_status"] = ValueTable(
    "acmp_status", {int(k): v for k, v in T.ACMP_STATUS.items()}
)
VALUE_TABLES["adp_message_type"] = ValueTable(
    "adp_message_type", {int(k): v for k, v in T.ADP_MESSAGE_TYPES.items()}
)
VALUE_TABLES["aa_mode"] = ValueTable(
    "aa_mode", {int(k): v for k, v in T.AA_MODES.items()}
)
VALUE_TABLES["aa_status"] = ValueTable(
    "aa_status", {int(k): v for k, v in T.AA_STATUS.items()}
)
VALUE_TABLES["jdks_log_priority"] = ValueTable(
    "jdks_log_priority", dict(T.JDKS_LOG_PRIORITIES)
)

AEM_COMMAND_BY_NAME: dict[str, int] = {v: int(k) for k, v in T.AEM_COMMANDS.items()}
DESCRIPTOR_TYPE_BY_NAME: dict[str, int] = {
    v: int(k) for k, v in T.DESCRIPTOR_TYPES.items()
}

# ---------------------------------------------------------------------------
# Bit-field expansions: (layout key, member) -> sub-fields over that member
# ---------------------------------------------------------------------------

_FLAG_WORDS: dict[tuple[str, str], list[tuple[str, int, str, str | None, str]]] = {
    # (sub-name, mask, kind, values, doc)
    ("adp", "sv_version_msgtype"): [
        ("message_type", 0x0F, "u8", "adp_message_type", "ADP message_type"),
    ],
    ("adp", "valid_time_cdl"): [
        ("valid_time", 0xF800, "u8", None, "valid_time (2-second units)"),
        ("control_data_length", 0x07FF, "u16", None, "control_data_length"),
    ],
    ("acmp", "sv_version_msgtype"): [
        ("message_type", 0x0F, "u8", "acmp_message_type", "ACMP message_type")
    ],
    ("acmp", "status_cdl"): [
        ("status", 0xF800, "u8", "acmp_status", "ACMP status"),
        ("control_data_length", 0x07FF, "u16", None, "control_data_length"),
    ],
    ("aecp", "sv_version_msgtype"): [
        ("message_type", 0x0F, "u8", "aecp_message_type", "AECP message_type")
    ],
    ("aecp", "status_cdl"): [
        ("status", 0xF800, "u8", "aem_status", "status (AEM status for AEM messages)"),
        ("control_data_length", 0x07FF, "u16", None, "control_data_length"),
    ],
    ("aem", "command_type"): [
        ("u", 0x8000, "bool", None, "unsolicited response"),
        ("cr", 0x4000, "bool", None, "controller request (2021)"),
        ("command_type", 0x3FFF, "u16", "aem_command", "AEM command_type"),
    ],
    ("aa.tlv", "mode_length"): [
        ("mode", 0xF000, "u8", "aa_mode", "TLV mode"),
        ("length", 0x0FFF, "u16", None, "TLV data length"),
    ],
}
for _name, _bit in T.ADP_ENTITY_CAPABILITIES.items():
    _FLAG_WORDS.setdefault(("adp", "entity_capabilities"), []).append(
        (
            f"entity_cap.{_name.lower()}",
            _bit,
            "bool",
            None,
            f"entity capability {_name}",
        )
    )
for _name, _bit in T.ADP_TALKER_CAPABILITIES.items():
    _FLAG_WORDS.setdefault(("adp", "talker_capabilities"), []).append(
        (
            f"talker_cap.{_name.lower()}",
            _bit,
            "bool",
            None,
            f"talker capability {_name}",
        )
    )
for _name, _bit in T.ADP_LISTENER_CAPABILITIES.items():
    _FLAG_WORDS.setdefault(("adp", "listener_capabilities"), []).append(
        (
            f"listener_cap.{_name.lower()}",
            _bit,
            "bool",
            None,
            f"listener capability {_name}",
        )
    )
for _name, _bit in T.ADP_CONTROLLER_CAPABILITIES.items():
    _FLAG_WORDS.setdefault(("adp", "controller_capabilities"), []).append(
        (
            f"controller_cap.{_name.lower()}",
            _bit,
            "bool",
            None,
            f"controller capability {_name}",
        )
    )
for _name, _bit in T.ACMP_FLAGS.items():
    _FLAG_WORDS.setdefault(("acmp", "flags"), []).append(
        (f"flag.{_name.lower()}", _bit, "bool", None, f"ACMP flag {_name}")
    )

_HEX_MEMBERS = {
    "entity_id",
    "entity_model_id",
    "association_id",
    "stream_id",
    "controller_entity_id",
    "talker_entity_id",
    "listener_entity_id",
    "target_entity_id",
    "owner_entity_id",
    "locked_entity_id",
    "gptp_grandmaster_id",
    "clock_identity",
    "msrp_failure_bridge_id",
    "vendor_eui64",
    "control_type",
    "clock_source_identifier",
    "backup_talker_entity_id_0",
    "backup_talker_entity_id_1",
    "backup_talker_entity_id_2",
    "backedup_talker_entity_id",
    "flags",
    "descriptor_type",
    "command_type",
    "counters_valid",
    "stream_format",
    "current_format",
}
_VALUE_MEMBERS = {"descriptor_type": "descriptor_type"}


def _fields_for(
    key: str, prefix: str, members: list[dict], base_offset: int = 0
) -> tuple[Field, ...]:
    """Fields for a struct's members; packed words expand into sub-fields."""
    fields: list[Field] = []
    for m in members:
        name, off, size, kind = (
            m["name"],
            base_offset + m["offset"],
            m["size"],
            m["kind"],
        )
        doc = m.get("doc", "")
        expansion = _FLAG_WORDS.get((key, name))
        if expansion:
            for sub, mask, skind, values, sdoc in expansion:
                fields.append(
                    Field(
                        sub,
                        off,
                        size,
                        skind,
                        sdoc,
                        mask=mask,
                        values=values,
                        base="dec",
                    )
                )
            continue
        if kind == "nested":
            continue  # composed by the caller
        if kind == "signal_source":
            fields += (
                Field(
                    f"{name}.signal_type",
                    off,
                    2,
                    "u16",
                    "signal_type",
                    values="descriptor_type",
                    base="hex",
                ),
                Field(f"{name}.signal_index", off + 2, 2, "u16", "signal_index"),
                Field(f"{name}.signal_output", off + 4, 2, "u16", "signal_output"),
            )
            continue
        if kind.startswith("repeat:"):
            ekind = kind.split(":", 1)[1]
            esize = {"u8": 1, "u16": 2, "u32": 4, "u64": 8}[ekind]
            fields.append(
                Field(
                    name,
                    off,
                    esize,
                    ekind,
                    doc or name,
                    base="hex",
                    repeat=size // esize,
                )
            )
            continue
        base = (
            "hex"
            if (name in _HEX_MEMBERS or kind in ("u64", "eui48", "bytes"))
            else "dec"
        )
        values = _VALUE_MEMBERS.get(name)
        fields.append(
            Field(name, off, size, kind, doc or name, base=base, values=values)
        )
    return tuple(fields)


def _layout(
    key: str, prefix: str, struct: dict, doc: str, base_offset: int = 0
) -> Layout:
    return Layout(
        key, prefix, doc, _fields_for(key, prefix, struct["members"], base_offset)
    )


# ---------------------------------------------------------------------------
# PDUs
# ---------------------------------------------------------------------------

ADP = _layout("adp", f"{PREFIX}.adp", T.ADP, "ADPDU (IEEE 1722.1-2021 6.2.1)")
ACMP = _layout("acmp", f"{PREFIX}.acmp", T.ACMP, "ACMPDU (IEEE 1722.1-2021 8.2.1)")
AECP = Layout(
    "aecp",
    f"{PREFIX}.aecp",
    "AECPDU common fields (IEEE 1722.1-2021 9.2.1)",
    _fields_for("aecp", f"{PREFIX}.aecp", T.AECP_COMMON["members"])
    + (Field("payload", 0, 0, "bytes", "undissected AECPDU payload octets"),),
)
AEM = Layout(
    "aem",
    f"{PREFIX}.aem",
    "AEM command/response header (IEEE 1722.1-2021 9.2.1.2)",
    _fields_for(
        "aem",
        f"{PREFIX}.aem",
        [m for m in T.AEM_DU["members"] if m["name"] == "command_type"],
    ),
)
AA = Layout(
    "aa",
    f"{PREFIX}.aa",
    "Address Access AECPDU (IEEE 1722.1-2021 9.2.1.3)",
    _fields_for(
        "aa", f"{PREFIX}.aa", [m for m in T.AA_DU["members"] if m["kind"] != "nested"]
    ),
)
AA_TLV = Layout(
    "aa.tlv",
    f"{PREFIX}.aa.tlv",
    "Address Access TLV (IEEE 1722.1-2021 9.2.1.3.2)",
    _fields_for("aa.tlv", f"{PREFIX}.aa.tlv", T.AA_TLV["members"])
    + (Field("data", 0, 0, "bytes", "TLV memory data"),),
)
VU = Layout(
    "vu",
    f"{PREFIX}.vu",
    "Vendor Unique AECPDU (IEEE 1722.1-2021 9.2.1.4)",
    (
        Field("protocol_id", 22, 6, "bytes", "protocol_id (vendor OUI-based)"),
        Field("payload", 0, 0, "bytes", "vendor-unique payload octets"),
    ),
)
JDKS_LOG = _layout(
    "jdks.log",
    f"{PREFIX}.jdks.log",
    T.JDKS_LOG_BLOB,
    "JDKS log message blob (SET_CONTROL values)",
)
JDKS_IPV4 = _layout(
    "jdks.ipv4",
    f"{PREFIX}.jdks.ipv4",
    T.JDKS_IPV4_PARAMS,
    "JDKS IPv4 parameters blob (SET_CONTROL values)",
)

AECP_HEADER_LENGTH = 22
AEM_HEADER_LENGTH = 24
AA_HEADER_LENGTH = 24
VU_HEADER_LENGTH = 28

# ---------------------------------------------------------------------------
# AEM payloads and descriptors
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class PayloadSpec:
    """A fixed payload struct after the AEM header, plus what follows it."""

    name: str  # snake name
    layout: Layout
    length: int
    trailer: str | None  # descriptor | values | raw | mappings | None


PAYLOAD_LAYOUTS: dict[str, Layout] = {}
for _sname, _struct in T.AEM_PAYLOAD_STRUCTS.items():
    if _sname == "AemAudioMapping":
        continue  # an element of a mappings trailer, laid out from offset 0 below
    _snake_name = _snake(_sname)
    PAYLOAD_LAYOUTS[_sname] = _layout(
        f"aem.{_snake_name}",
        f"{PREFIX}.aem.{_snake_name}",
        _struct,
        f"AEM {_sname} (IEEE 1722.1-2021 7.4)",
        AEM_HEADER_LENGTH,
    )


def _payload_spec(text: str | None) -> PayloadSpec | None:
    if text is None:
        return None
    sname, _, trailer = text.partition("+")
    struct = T.AEM_PAYLOAD_STRUCTS[sname]
    return PayloadSpec(
        _snake(sname), PAYLOAD_LAYOUTS[sname], struct["length"], trailer or None
    )


# command code -> (command spec, response spec); commands absent here show a raw payload
AEM_PAYLOADS: dict[int, tuple[PayloadSpec | None, PayloadSpec | None]] = {
    AEM_COMMAND_BY_NAME[name]: (_payload_spec(cmd), _payload_spec(rsp))
    for name, (cmd, rsp) in T.AEM_PAYLOADS.items()
}

AUDIO_MAPPING = _layout(
    "aem.audio_mapping",
    f"{PREFIX}.aem.audio_mapping",
    T.AEM_PAYLOAD_STRUCTS["AemAudioMapping"],
    "AEM audio mapping entry",
)
AEM_PAYLOAD_RAW = Field("payload", 0, 0, "bytes", "undissected AEM payload octets")
AECP_PAYLOAD = AECP.fields[-1]
AA_TLV_DATA = AA_TLV.fields[-1]
VU_PAYLOAD = VU.fields[-1]


@dataclass(frozen=True)
class TrailerSpec:
    """A descriptor's counted trailer."""

    count_field: str
    offset_field: str
    element: str  # count_entry | sampling_rate | stream_format | audio_mapping | video_mapping | sensor_mapping | clock_source_index | raw
    element_size: int


@dataclass(frozen=True)
class DescriptorSpec:
    """How one descriptor type is dissected."""

    name: str  # snake
    struct: str
    layout: Layout
    length: int
    trailer: TrailerSpec | None


DESCRIPTOR_LAYOUTS: dict[str, Layout] = {}
for _sname, _struct in T.DESCRIPTOR_STRUCTS.items():
    _snake_name = _snake(_sname)
    DESCRIPTOR_LAYOUTS[_sname] = _layout(
        f"desc.{_snake_name}",
        f"{PREFIX}.desc.{_snake_name}",
        _struct,
        f"{_sname} (IEEE 1722.1-2021 7.2)",
    )

DESCRIPTORS: dict[int, DescriptorSpec] = {}
for _tname, _sname in T.DESCRIPTOR_STRUCT_BY_TYPE.items():
    _trailer = None
    if _sname in T.DESCRIPTOR_TRAILERS:
        _count, _offset, (_elem, _esize) = T.DESCRIPTOR_TRAILERS[_sname]
        _trailer = TrailerSpec(_count, _offset, _elem, _esize)
    DESCRIPTORS[DESCRIPTOR_TYPE_BY_NAME[_tname]] = DescriptorSpec(
        _snake(_sname),
        _sname,
        DESCRIPTOR_LAYOUTS[_sname],
        T.DESCRIPTOR_STRUCTS[_sname]["length"],
        _trailer,
    )

# Trailer element layouts (offsets relative to the element)
COUNT_ENTRY = Layout(
    "desc.count_entry",
    f"{PREFIX}.desc.count_entry",
    "descriptor_counts entry",
    (
        Field(
            "descriptor_type",
            0,
            2,
            "u16",
            "descriptor_type",
            base="hex",
            values="descriptor_type",
        ),
        Field("count", 2, 2, "u16", "count"),
    ),
)
SAMPLING_RATE = Layout(
    "desc.sampling_rate",
    f"{PREFIX}.desc.sampling_rate",
    "sampling rate entry (pull | base_frequency)",
    (
        Field("pull", 0, 4, "u8", "pull", mask=0xE0000000),
        Field("base_frequency", 0, 4, "u32", "base_frequency (Hz)", mask=0x1FFFFFFF),
    ),
)
STREAM_FORMAT = Layout(
    "desc.stream_format",
    f"{PREFIX}.desc.stream_format",
    "stream format entry",
    (Field("format", 0, 8, "bytes", "stream_format (IEEE 1722 stream format)"),),
)
CLOCK_SOURCE_INDEX = Layout(
    "desc.clock_source_index",
    f"{PREFIX}.desc.clock_source_index",
    "clock source index entry",
    (Field("index", 0, 2, "u16", "CLOCK_SOURCE descriptor index"),),
)
_MAPPING_FIELDS = (
    Field("mapping_stream_index", 0, 2, "u16", "mapping_stream_index"),
    Field("mapping_stream_channel", 2, 2, "u16", "mapping_stream_channel"),
    Field("mapping_cluster_offset", 4, 2, "u16", "mapping_cluster_offset"),
    Field("mapping_cluster_channel", 6, 2, "u16", "mapping_cluster_channel"),
)
AUDIO_MAP_ENTRY = Layout(
    "desc.audio_mapping",
    f"{PREFIX}.desc.audio_mapping",
    "audio mapping entry",
    _MAPPING_FIELDS,
)
VIDEO_MAP_ENTRY = Layout(
    "desc.video_mapping",
    f"{PREFIX}.desc.video_mapping",
    "video mapping entry",
    (
        Field("mapping_stream_index", 0, 2, "u16", "mapping_stream_index"),
        Field("mapping_program_stream", 2, 2, "u16", "mapping_program_stream"),
        Field("mapping_elementary_stream", 4, 2, "u16", "mapping_elementary_stream"),
        Field("mapping_cluster_offset", 6, 2, "u16", "mapping_cluster_offset"),
    ),
)
SENSOR_MAP_ENTRY = Layout(
    "desc.sensor_mapping",
    f"{PREFIX}.desc.sensor_mapping",
    "sensor mapping entry",
    _MAPPING_FIELDS,
)
TRAILER_ELEMENTS: dict[str, Layout] = {
    "count_entry": COUNT_ENTRY,
    "sampling_rate": SAMPLING_RATE,
    "stream_format": STREAM_FORMAT,
    "clock_source_index": CLOCK_SOURCE_INDEX,
    "audio_mapping": AUDIO_MAP_ENTRY,
    "video_mapping": VIDEO_MAP_ENTRY,
    "sensor_mapping": SENSOR_MAP_ENTRY,
}
DESCRIPTOR_RAW = Field("value_details", 0, 0, "bytes", "raw descriptor trailer octets")
JDKS_LOG_TEXT = Field("text", 0, 0, "string", "log text")

ATDECC_LAYOUTS: tuple[Layout, ...] = (
    (ADP, ACMP, AECP, AEM, AA, AA_TLV, VU, JDKS_LOG, JDKS_IPV4, AUDIO_MAPPING)
    + tuple(PAYLOAD_LAYOUTS.values())
    + tuple(DESCRIPTOR_LAYOUTS.values())
    + tuple(TRAILER_ELEMENTS.values())
)
