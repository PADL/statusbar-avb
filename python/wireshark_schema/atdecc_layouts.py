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

from . import atdecc_std as STD
from . import atdecc_table as T
from . import atdecc_units as U
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
# 64-bit members that are EUI-64 identifiers (rendered colon-separated with OUI
# resolution, like Wireshark's own eui64 fields). Stream ids, formats, lengths,
# nonces, counters and the MSRP bridge id stay plain 64-bit numbers.
_EUI64_MEMBERS = {
    "entity_model_id",
    "gptp_grandmaster_id",
    "clock_identity",
    "gm_clock_identity",
    "parent_clock_identity",
    "clock_source_identifier",
    "control_type",
    "transcoder_type",
    "vendor_eui64",
    "key_id",
}


def _is_eui64(name: str, kind: str) -> bool:
    return kind == "u64" and (name.endswith("entity_id") or name in _EUI64_MEMBERS)


for (_sname, _member), _bits in STD.STD_FLAG_WORDS.items():
    _FLAG_WORDS[(f"aem.{_snake(_sname)}", _member)] = tuple(
        (sub, mask, kind, None, doc) for sub, mask, kind, doc in _bits
    )
_VALUE_MEMBERS["keychain_id"] = "keychain_id"


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
        if _is_eui64(name, kind):
            kind = "eui64"
        base = (
            "hex"
            if (name in _HEX_MEMBERS or kind in ("u64", "eui48", "eui64", "bytes"))
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
class PayloadTrailer:
    """What follows a fixed AEM payload struct."""

    kind: str  # descriptor | values | raw | elements
    element: str | None = None  # TRAILER_ELEMENTS key for kind == elements
    count_field: str | None = None  # member of the struct holding the element count


@dataclass(frozen=True)
class PayloadSpec:
    """A fixed payload struct after the AEM header, plus what follows it."""

    name: str  # snake name
    struct: str
    layout: Layout
    length: int
    trailer: PayloadTrailer | None


ALL_PAYLOAD_STRUCTS: dict[str, dict] = {
    **T.AEM_PAYLOAD_STRUCTS,
    **STD.STD_PAYLOAD_STRUCTS,
}
PAYLOAD_LAYOUTS: dict[str, Layout] = {}
for _sname, _struct in ALL_PAYLOAD_STRUCTS.items():
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
    sname, _, trailer_text = text.partition("+")
    struct = ALL_PAYLOAD_STRUCTS[sname]
    trailer = None
    if (
        trailer_text == "mappings"
    ):  # the extracted table's spelling of the audio mappings
        trailer = PayloadTrailer("elements", "audio_mapping", "number_of_mappings")
    elif trailer_text.startswith("elements:"):
        _, element, count = trailer_text.split(":")
        trailer = PayloadTrailer("elements", element, count)
    elif trailer_text.startswith("blob:"):
        _, field, count = trailer_text.split(":")
        trailer = PayloadTrailer("blob", field, count)
    elif trailer_text:
        trailer = PayloadTrailer(trailer_text)
    return PayloadSpec(
        _snake(sname), sname, PAYLOAD_LAYOUTS[sname], struct["length"], trailer
    )


# command code -> (command spec, response spec); commands absent here show a raw payload
AEM_PAYLOADS: dict[int, tuple[PayloadSpec | None, PayloadSpec | None]] = {
    AEM_COMMAND_BY_NAME[name]: (_payload_spec(cmd), _payload_spec(rsp))
    for name, (cmd, rsp) in {**T.AEM_PAYLOADS, **STD.STD_AEM_PAYLOADS}.items()
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


# ---------------------------------------------------------------------------
# Descriptors and their counted tables
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class TrailerSpec:
    """One counted table inside a descriptor (offset_field is relative to the
    descriptor start). count_field None means exactly one element; element
    "values" is a control value_details block typed by value_type_field."""

    count_field: str | None
    offset_field: str
    element: str
    element_size: int
    value_type_field: str | None = None


@dataclass(frozen=True)
class DescriptorSpec:
    """How one descriptor type is dissected."""

    name: str  # snake
    struct: str
    layout: Layout
    length: int
    trailers: tuple[TrailerSpec, ...]


DESCRIPTOR_LAYOUTS: dict[str, Layout] = {}
for _sname, _struct in T.DESCRIPTOR_STRUCTS.items():
    _snake_name = _snake(_sname)
    DESCRIPTOR_LAYOUTS[_sname] = _layout(
        f"desc.{_snake_name}",
        f"{PREFIX}.desc.{_snake_name}",
        _struct,
        f"{_sname} (IEEE 1722.1-2021 7.2)",
    )


def _t(count, offset, element, size, value_type=None) -> TrailerSpec:
    return TrailerSpec(count, offset, element, size, value_type)


#: struct -> its counted tables (IEEE 1722.1-2021 7.2.x); supersedes the
#: single-trailer table in atdecc_table
DESCRIPTOR_TABLES: dict[str, tuple[TrailerSpec, ...]] = {
    "DescriptorConfiguration": (
        _t("descriptor_counts_count", "descriptor_counts_offset", "count_entry", 4),
    ),
    "DescriptorAudioUnit": (
        _t("sampling_rates_count", "sampling_rates_offset", "sampling_rate", 4),
    ),
    "DescriptorStream": (
        _t("number_of_formats", "formats_offset", "stream_format", 8),
        _t(
            "number_of_redundant_streams",
            "redundant_offset",
            "redundant_stream_index",
            2,
        ),
    ),
    "DescriptorAudioMap": (
        _t("number_of_mappings", "mappings_offset", "audio_mapping", 8),
    ),
    "DescriptorVideoMap": (
        _t("number_of_mappings", "mappings_offset", "video_mapping", 8),
    ),
    "DescriptorSensorMap": (
        _t("number_of_mappings", "mappings_offset", "sensor_mapping", 6),
    ),
    "DescriptorClockDomain": (
        _t("clock_sources_count", "clock_sources_offset", "clock_source_index", 2),
    ),
    "DescriptorControl": (
        _t("number_of_values", "values_offset", "values", 0, "control_value_type"),
    ),
    "DescriptorMixer": (
        _t("number_of_sources", "sources_offset", "signal", 4),
        _t(None, "value_offset", "values", 0, "control_value_type"),
    ),
    "DescriptorMatrix": (
        _t("number_of_values", "values_offset", "values", 0, "control_value_type"),
    ),
    "DescriptorSignalTranscoder": (
        _t("number_of_values", "values_offset", "values", 0, "control_value_type"),
    ),
    "DescriptorSignalSelector": (
        _t("number_of_sources", "sources_offset", "signal", 4),
    ),
    "DescriptorMatrixSignal": (_t("signals_count", "signals_offset", "signal", 4),),
    "DescriptorSignalSplitter": (
        _t("splitter_map_count", "splitter_map_offset", "signal", 4),
    ),
    "DescriptorSignalDemultiplexer": (
        _t("demultiplexer_map_count", "demultiplexer_map_offset", "signal", 4),
    ),
    "DescriptorSignalCombiner": (
        _t("combiner_map_count", "combiner_map_offset", "signal", 4),
        _t("number_of_sources", "sources_offset", "signal", 4),
    ),
    "DescriptorSignalMultiplexer": (
        _t("multiplexer_map_count", "multiplexer_map_offset", "signal", 4),
        _t("number_of_sources", "sources_offset", "signal", 4),
    ),
    "DescriptorTiming": (
        _t("number_of_ptp_instances", "ptp_instances_offset", "ptp_instance_index", 2),
    ),
    "DescriptorVideoCluster": (
        _t(
            "supported_format_specifics_count",
            "supported_format_specifics_offset",
            "format_specific",
            4,
        ),
        _t(
            "supported_sampling_rates_count",
            "supported_sampling_rates_offset",
            "sampling_rate",
            4,
        ),
        _t(
            "supported_aspect_ratios_count",
            "supported_aspect_ratios_offset",
            "aspect_ratio",
            2,
        ),
        _t("supported_sizes_count", "supported_sizes_offset", "size", 4),
        _t(
            "supported_color_spaces_count",
            "supported_color_spaces_offset",
            "color_space",
            2,
        ),
    ),
    "DescriptorSensorCluster": (
        _t("supported_formats_count", "supported_formats_offset", "sensor_format", 8),
        _t(
            "supported_sampling_rates_count",
            "supported_sampling_rates_offset",
            "sampling_rate",
            4,
        ),
    ),
}

DESCRIPTORS: dict[int, DescriptorSpec] = {}
for _tname, _sname in T.DESCRIPTOR_STRUCT_BY_TYPE.items():
    DESCRIPTORS[DESCRIPTOR_TYPE_BY_NAME[_tname]] = DescriptorSpec(
        _snake(_sname),
        _sname,
        DESCRIPTOR_LAYOUTS[_sname],
        T.DESCRIPTOR_STRUCTS[_sname]["length"],
        DESCRIPTOR_TABLES.get(_sname, ()),
    )


# Table element layouts (offsets relative to the element)
def _elem(name: str, doc: str, fields: tuple[Field, ...]) -> Layout:
    return Layout(f"desc.{name}", f"{PREFIX}.desc.{name}", doc, fields)


COUNT_ENTRY = _elem(
    "count_entry",
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
SAMPLING_RATE = _elem(
    "sampling_rate",
    "sampling rate entry (pull | base_frequency)",
    (
        Field("pull", 0, 4, "u8", "pull", mask=0xE0000000),
        Field("base_frequency", 0, 4, "u32", "base_frequency (Hz)", mask=0x1FFFFFFF),
    ),
)
STREAM_FORMAT = _elem(
    "stream_format",
    "stream format entry",
    (Field("format", 0, 8, "bytes", "stream_format (IEEE 1722 stream format)"),),
)
CLOCK_SOURCE_INDEX = _elem(
    "clock_source_index",
    "clock source index entry",
    (Field("index", 0, 2, "u16", "CLOCK_SOURCE descriptor index"),),
)
_MAPPING_FIELDS = (
    Field("mapping_stream_index", 0, 2, "u16", "mapping_stream_index"),
    Field("mapping_stream_channel", 2, 2, "u16", "mapping_stream_channel"),
    Field("mapping_cluster_offset", 4, 2, "u16", "mapping_cluster_offset"),
    Field("mapping_cluster_channel", 6, 2, "u16", "mapping_cluster_channel"),
)
AUDIO_MAP_ENTRY = _elem("audio_mapping", "audio mapping entry", _MAPPING_FIELDS)
VIDEO_MAP_ENTRY = _elem(
    "video_mapping",
    "video mapping entry",
    (
        Field("mapping_stream_index", 0, 2, "u16", "mapping_stream_index"),
        Field("mapping_program_stream", 2, 2, "u16", "mapping_program_stream"),
        Field("mapping_elementary_stream", 4, 2, "u16", "mapping_elementary_stream"),
        Field("mapping_cluster_offset", 6, 2, "u16", "mapping_cluster_offset"),
    ),
)
SENSOR_MAP_ENTRY = _elem(
    "sensor_mapping",
    "sensor mapping entry (IEEE 1722.1-2021 Table 7-37: 6 octets)",
    (
        Field("mapping_stream_index", 0, 2, "u16", "mapping_stream_index"),
        Field("mapping_stream_signal", 2, 2, "u16", "mapping_stream_signal"),
        Field("mapping_cluster_offset", 4, 2, "u16", "mapping_cluster_offset"),
    ),
)
SIGNAL_ENTRY = _elem(
    "signal",
    "signal reference entry (source / map / signal list)",
    (
        Field(
            "signal_type",
            0,
            2,
            "u16",
            "signal descriptor_type",
            base="hex",
            values="descriptor_type",
        ),
        Field("signal_index", 2, 2, "u16", "signal descriptor_index"),
    ),
)
PTP_INSTANCE_INDEX = _elem(
    "ptp_instance_index",
    "PTP_INSTANCE index entry",
    (Field("index", 0, 2, "u16", "PTP_INSTANCE descriptor index"),),
)
REDUNDANT_STREAM_INDEX = _elem(
    "redundant_stream_index",
    "redundant stream entry",
    (Field("index", 0, 2, "u16", "redundant STREAM descriptor index"),),
)
FORMAT_SPECIFIC = _elem(
    "format_specific",
    "video format specific entry",
    (Field("value", 0, 4, "u32", "format_specific", base="hex"),),
)
ASPECT_RATIO = _elem(
    "aspect_ratio",
    "video aspect ratio entry",
    (
        Field("width", 0, 1, "u8", "aspect width"),
        Field("height", 1, 1, "u8", "aspect height"),
    ),
)
SIZE_ENTRY = _elem(
    "size",
    "video size entry",
    (
        Field("width", 0, 2, "u16", "width (pixels)"),
        Field("height", 2, 2, "u16", "height (pixels)"),
    ),
)
COLOR_SPACE = _elem(
    "color_space",
    "video color space entry",
    (Field("value", 0, 2, "u16", "color_space", base="hex"),),
)
SENSOR_FORMAT = _elem(
    "sensor_format",
    "sensor format entry",
    (Field("value", 0, 8, "u64", "sensor_format", base="hex"),),
)
AS_PATH_ENTRY = _elem(
    "as_path_entry",
    "gPTP path sequence entry",
    (Field("clock_identity", 0, 8, "eui64", "clock identity"),),
)
CLOCK_IDENTITY_ENTRY = _elem(
    "clock_identity",
    "PTP path trace entry",
    (Field("value", 0, 8, "eui64", "clock identity"),),
)
KEY_EUI_ENTRY = _elem(
    "key_eui",
    "keychain key entry",
    (Field("value", 0, 8, "eui64", "key EUI-64"),),
)
TRAILER_ELEMENTS: dict[str, Layout] = {
    "count_entry": COUNT_ENTRY,
    "sampling_rate": SAMPLING_RATE,
    "stream_format": STREAM_FORMAT,
    "clock_source_index": CLOCK_SOURCE_INDEX,
    "audio_mapping": AUDIO_MAP_ENTRY,
    "video_mapping": VIDEO_MAP_ENTRY,
    "sensor_mapping": SENSOR_MAP_ENTRY,
    "signal": SIGNAL_ENTRY,
    "ptp_instance_index": PTP_INSTANCE_INDEX,
    "redundant_stream_index": REDUNDANT_STREAM_INDEX,
    "format_specific": FORMAT_SPECIFIC,
    "aspect_ratio": ASPECT_RATIO,
    "size": SIZE_ENTRY,
    "color_space": COLOR_SPACE,
    "sensor_format": SENSOR_FORMAT,
    "as_path_entry": AS_PATH_ENTRY,
    "clock_identity": CLOCK_IDENTITY_ENTRY,
    "key_eui": KEY_EUI_ENTRY,
}
ELEMENT_SIZE: dict[str, int] = {
    "count_entry": 4,
    "sampling_rate": 4,
    "stream_format": 8,
    "clock_source_index": 2,
    "audio_mapping": 8,
    "video_mapping": 8,
    "sensor_mapping": 6,
    "signal": 4,
    "ptp_instance_index": 2,
    "redundant_stream_index": 2,
    "format_specific": 4,
    "aspect_ratio": 2,
    "size": 4,
    "color_space": 2,
    "sensor_format": 8,
    "as_path_entry": 8,
    "clock_identity": 8,
    "key_eui": 8,
}
DESCRIPTOR_RAW = Field("value_details", 0, 0, "bytes", "raw descriptor trailer octets")
JDKS_LOG_TEXT = Field("text", 0, 0, "string", "log text")

# ---------------------------------------------------------------------------
# Control values (IEEE 1722.1-2021 7.3.5.2): typed per-element items plus the
# fixed layouts of the special families; offsets relative to the value
# ---------------------------------------------------------------------------

VALUE_TABLES["control_value_type"] = ValueTable(
    "control_value_type", dict(U.CONTROL_VALUE_TYPES)
)
VALUE_TABLES["control_unit_code"] = ValueTable("control_unit_code", dict(U.UNIT_CODES))
VALUE_TABLES["mvu_command"] = ValueTable("mvu_command", dict(STD.MVU_COMMANDS))
VALUE_TABLES["mvu_status"] = ValueTable("mvu_status", dict(STD.MVU_STATUS))

CONTROL_PREFIX = f"{PREFIX}.control"
CONTROL_VALUE = Layout(
    "control",
    CONTROL_PREFIX,
    "control value elements",
    (
        Field("value_i8", 0, 1, "i8", "INT8 value"),
        Field("value_u8", 0, 1, "u8", "UINT8 value"),
        Field("value_i16", 0, 2, "i16", "INT16 value"),
        Field("value_u16", 0, 2, "u16", "UINT16 value"),
        Field("value_i32", 0, 4, "i32", "INT32 value"),
        Field("value_u32", 0, 4, "u32", "UINT32 value"),
        Field("value_i64", 0, 8, "i64", "INT64 value"),
        Field("value_u64", 0, 8, "u64", "UINT64 value"),
        Field("value_f32", 0, 4, "f32", "FLOAT value"),
        Field("value_f64", 0, 8, "f64", "DOUBLE value"),
        Field(
            "value_string_ref",
            0,
            2,
            "u16",
            "localized string reference option",
            base="hex",
        ),
        Field("units_multiplier", 0, 1, "i8", "units multiplier (power of ten)"),
        Field(
            "units_code",
            1,
            1,
            "u8",
            "units code",
            base="hex",
            values="control_unit_code",
        ),
        Field(
            "localized_string", 0, 2, "u16", "localized string reference", base="hex"
        ),
        Field("utf8", 0, 0, "string", "UTF-8 value"),
        Field("vendor_values", 0, 0, "bytes", "vendor / expansion value_details"),
    ),
)
CONTROL_SMPTE = Layout(
    "control.smpte",
    f"{CONTROL_PREFIX}.smpte",
    "CONTROL_SMPTE_TIME value (Table 7.18)",
    (
        Field("hours", 0, 2, "u16", "hours"),
        Field("minutes", 2, 1, "u8", "minutes"),
        Field("seconds", 3, 1, "u8", "seconds"),
        Field("frames", 4, 1, "u8", "frames"),
        Field("subframes", 5, 2, "u16", "subframes"),
        Field("frames_per_second", 7, 1, "u8", "frames per second"),
        Field("drop_frame", 8, 1, "u8", "drop frame"),
        Field("pull", 9, 1, "u8", "pull"),
    ),
)
CONTROL_SAMPLE_RATE = Layout(
    "control.sample_rate",
    f"{CONTROL_PREFIX}.sample_rate",
    "CONTROL_SAMPLE_RATE value (Table 7.19)",
    (
        Field("current_pull", 0, 4, "u8", "current pull", mask=0xE0000000),
        Field(
            "current_base_frequency",
            0,
            4,
            "u32",
            "current base_frequency (Hz)",
            mask=0x1FFFFFFF,
        ),
        Field("default_pull", 4, 4, "u8", "default pull", mask=0xE0000000),
        Field(
            "default_base_frequency",
            4,
            4,
            "u32",
            "default base_frequency (Hz)",
            mask=0x1FFFFFFF,
        ),
        Field("minimum_pull", 8, 4, "u8", "minimum pull", mask=0xE0000000),
        Field(
            "minimum_base_frequency",
            8,
            4,
            "u32",
            "minimum base_frequency (Hz)",
            mask=0x1FFFFFFF,
        ),
        Field("maximum_pull", 12, 4, "u8", "maximum pull", mask=0xE0000000),
        Field(
            "maximum_base_frequency",
            12,
            4,
            "u32",
            "maximum base_frequency (Hz)",
            mask=0x1FFFFFFF,
        ),
    ),
)
CONTROL_GPTP = Layout(
    "control.gptp",
    f"{CONTROL_PREFIX}.gptp",
    "CONTROL_GPTP_TIME value (Table 7.20)",
    (
        Field("seconds", 0, 6, "u64", "gptp seconds (48-bit)"),
        Field("nanoseconds", 6, 4, "u32", "gptp nanoseconds"),
    ),
)
CONTROL_BODE_HEADER = Layout(
    "control.bode",
    f"{CONTROL_PREFIX}.bode",
    "CONTROL_BODE_PLOT ranges (Table 7.17)",
    tuple(
        Field(f"{axis}_{role}", (i * 4) + (j * 16), 4, "f32", f"{axis} {role}")
        for j, axis in enumerate(("frequency", "magnitude", "phase"))
        for i, role in enumerate(("minimum", "maximum", "step", "default"))
    ),
)
CONTROL_BODE_POINT = Layout(
    "control.bode.point",
    f"{CONTROL_PREFIX}.bode.point",
    "CONTROL_BODE_PLOT current point",
    (
        Field("frequency", 0, 4, "f32", "frequency (Hz)"),
        Field("magnitude", 4, 4, "f32", "magnitude (dB)"),
        Field("phase", 8, 4, "f32", "phase (degrees)"),
    ),
)
CONTROL_UTF8 = CONTROL_VALUE.fields[-2]
CONTROL_VENDOR = CONTROL_VALUE.fields[-1]
CONTROL_VALUE_FAMILY = STD.CONTROL_VALUE_FAMILY

# ---------------------------------------------------------------------------
# Stream format EUI-64 sub-fields (avtp_stream_format.hpp; offsets relative
# to the 8-octet value)
# ---------------------------------------------------------------------------

SF_PREFIX = f"{PREFIX}.stream_format"
SF_AAF = Layout(
    "stream_format.aaf",
    f"{SF_PREFIX}.aaf",
    "AAF stream format (IEEE 1722 7.3.4 as carried in AEM)",
    (
        Field("nsr", 1, 1, "u8", "nominal sample rate", mask=0x0F, values="aaf_nsr"),
        Field("format", 2, 1, "u8", "sample format", values="aaf_format"),
        Field("bit_depth", 3, 1, "u8", "bit depth"),
        Field("channels_per_frame", 4, 4, "u16", "channels per frame", mask=0xFFC00000),
        Field("samples_per_frame", 4, 4, "u16", "samples per frame", mask=0x003FF000),
    ),
)
SF_IEC61883 = Layout(
    "stream_format.iec61883",
    f"{SF_PREFIX}.iec61883",
    "IEC 61883-6 stream format (IEEE 1722.1 Annex A.5)",
    (
        Field("sf", 1, 1, "bool", "sf (1 = IEC 61883)", mask=0x80),
        Field("fmt", 1, 1, "u8", "FMT", mask=0x7E, base="hex"),
        Field("sfc", 2, 1, "u8", "sampling frequency code", values="am824_fdf"),
        Field("dbs", 3, 1, "u8", "data block size (channels)"),
        Field("b", 4, 1, "bool", "blocking", mask=0x80),
        Field("nb", 4, 1, "bool", "non-blocking", mask=0x40),
        Field("sph", 4, 1, "bool", "source packet header", mask=0x02),
        Field("label", 5, 1, "u8", "AM824 label", base="hex", values="am824_label"),
    ),
)
SF_CRF = Layout(
    "stream_format.crf",
    f"{SF_PREFIX}.crf",
    "CRF stream format (IEEE 1722 10.4 as carried in AEM)",
    (
        Field("type", 1, 1, "u8", "CRF type", mask=0xF0, values="crf_type"),
        Field("timestamp_interval", 1, 2, "u16", "timestamp interval", mask=0x0FFF),
        Field("timestamps_per_pdu", 3, 1, "u8", "timestamps per PDU"),
        Field("pull", 4, 4, "u8", "pull", mask=0xE0000000, values="crf_pull"),
        Field("base_frequency", 4, 4, "u32", "base frequency (Hz)", mask=0x1FFFFFFF),
    ),
)
STREAM_FORMAT_LAYOUTS: dict[int, Layout] = {
    0x02: SF_AAF,
    0x00: SF_IEC61883,
    0x04: SF_CRF,
}

#: spec name -> absolute offsets (payloads) / descriptor-relative offsets
#: (descriptors) of 8-octet stream format members
STREAM_FORMAT_AT: dict[str, tuple[int, ...]] = {}
for _sname, _struct in ALL_PAYLOAD_STRUCTS.items():
    _offs = tuple(
        AEM_HEADER_LENGTH + m["offset"]
        for m in _struct["members"]
        if m["name"] in ("stream_format", "current_format") and m["size"] == 8
    )
    if _offs:
        STREAM_FORMAT_AT[_snake(_sname)] = _offs
for _sname, _struct in T.DESCRIPTOR_STRUCTS.items():
    _offs = tuple(
        m["offset"]
        for m in _struct["members"]
        if m["name"] in ("stream_format", "current_format")
        and m["size"] == 8
        and _sname != "DescriptorSensorCluster"
    )
    if _offs:
        STREAM_FORMAT_AT[_snake(_sname)] = _offs

# ---------------------------------------------------------------------------
# Milan vendor unique and AVC
# ---------------------------------------------------------------------------

MVU = Layout(
    "mvu",
    f"{PREFIX}.mvu",
    "Milan vendor unique header (Milan 1.3 Figure 5.4)",
    (
        Field("u", 28, 1, "bool", "u (unsolicited)", mask=0x80),
        Field(
            "command_type",
            28,
            2,
            "u16",
            "command_type (Table 5.15)",
            mask=0x7FFF,
            values="mvu_command",
        ),
    ),
)
MVU_HEADER_LENGTH = STD.MVU_HEADER_LENGTH
MVU_PAYLOAD_LAYOUTS: dict[str, Layout] = {}
for _sname, _struct in STD.MVU_PAYLOAD_STRUCTS.items():
    _fields = list(
        _fields_for(
            f"mvu.{_snake(_sname)}",
            f"{PREFIX}.mvu",
            _struct["members"],
            MVU_HEADER_LENGTH,
        )
    )
    if _sname in STD.MVU_OPTIONAL_NAME:
        _fields.append(
            Field(
                STD.MVU_OPTIONAL_NAME[_sname],
                MVU_HEADER_LENGTH + _struct["length"],
                0,
                "string",
                "name (Milan 1.3, 64 octets, optional)",
            )
        )
    MVU_PAYLOAD_LAYOUTS[_sname] = Layout(
        f"mvu.{_snake(_sname)}", f"{PREFIX}.mvu", _struct["doc"], tuple(_fields)
    )
_MVU_FLAG_FIELDS = tuple(
    Field(f"features_flags.{name}", MVU_HEADER_LENGTH + 6, 4, "bool", name, mask=mask)
    for name, mask in STD.MVU_FEATURES_FLAGS
) + tuple(
    Field(f"mcr_flags.{name}", MVU_HEADER_LENGTH + 2, 1, "bool", name, mask=mask)
    for name, mask in STD.MVU_MCR_FLAGS
)
MVU_FLAGS = Layout("mvu.flags", f"{PREFIX}.mvu", "MVU flag bits", _MVU_FLAG_FIELDS)


@dataclass(frozen=True)
class MvuSpec:
    name: str
    struct: str
    layout: Layout
    length: int
    optional_name: str | None


def _mvu_spec(sname: str | None) -> MvuSpec | None:
    if sname is None:
        return None
    return MvuSpec(
        _snake(sname),
        sname,
        MVU_PAYLOAD_LAYOUTS[sname],
        STD.MVU_PAYLOAD_STRUCTS[sname]["length"],
        STD.MVU_OPTIONAL_NAME.get(sname),
    )


MVU_PAYLOADS: dict[int, tuple[MvuSpec | None, MvuSpec | None]] = {
    code: (_mvu_spec(cmd), _mvu_spec(rsp))
    for code, (cmd, rsp) in STD.MVU_PAYLOADS.items()
}

VALUE_TABLES["keychain_id"] = ValueTable("keychain_id", dict(STD.KEYCHAIN_IDS))

# byte-string trailers of the AUTH commands (length from a header member)
AEM_BLOBS = Layout(
    "aem.blobs",
    f"{PREFIX}.aem",
    "AEM byte-string payload trailers",
    (
        Field("key", 0, 0, "bytes", "key data (key_length octets)"),
        Field(
            "authentication_token",
            0,
            0,
            "bytes",
            "authentication token (token_length octets)",
        ),
    ),
)
AEM_BLOB_FIELDS: dict[str, Field] = {f.name: f for f in AEM_BLOBS.fields}

# GET_DYNAMIC_INFO entry header (Figure 7-94), offsets relative to the entry
DYNAMIC_INFO = Layout(
    "aem.dynamic_info",
    f"{PREFIX}.aem.dynamic_info",
    "dynamic_info entry (IEEE 1722.1-2021 7.4.76)",
    (
        Field(
            "info_command_specific_data_length",
            0,
            2,
            "u16",
            "octets of info_command_specific_data",
        ),
        Field(
            "info_status",
            4,
            2,
            "u8",
            "status of this entry",
            mask=0xF800,
            values="aem_status",
        ),
        Field(
            "info_command_type",
            6,
            2,
            "u16",
            "command of this entry",
            values="aem_command",
            base="hex",
        ),
    ),
)
DYNAMIC_INFO_HEADER_LENGTH = STD.DYNAMIC_INFO_HEADER_LENGTH

HDCP_APM = Layout(
    "hdcp_apm",
    f"{PREFIX}.hdcp_apm",
    "HDCP IIA Authentication Protocol AECP message (IEEE 1722.1-2021 9.7.2)",
    (
        Field("length", 22, 2, "u16", "hdcp_apm_length (whole message, octets)"),
        Field("mf", 24, 1, "bool", "more fragments follow", mask=0x01),
        Field(
            "fragment_offset", 26, 2, "u16", "offset of this fragment in the message"
        ),
        Field("message_data", 0, 0, "bytes", "HDCP IIA message fragment"),
    ),
)
HDCP_APM_HEADER_LENGTH = STD.HDCP_APM_HEADER_LENGTH
HDCP_APM_DATA = HDCP_APM.fields[-1]

AVC = Layout(
    "avc",
    f"{PREFIX}.avc",
    "AVC AECP message (IEEE 1722.1-2021 9.2.1.3)",
    (
        Field("length", 22, 2, "u16", "avc_length"),
        Field("command_response", 0, 0, "bytes", "AV/C command/response frame"),
    ),
)
AVC_HEADER_LENGTH = STD.AVC_HEADER_LENGTH
AVC_PAYLOAD = AVC.fields[-1]

#: EXTENDED_COMMAND / EXTENDED_RESPONSE (message types 14 and 15) are listed in
#: IEEE 1722.1-2021 Table 9-1 as "reserved for future use": no payload format
#: exists, so everything after the common AECP header is one data field.
EXTENDED = Layout(
    "extended",
    f"{PREFIX}.extended",
    "Extended AECP message (IEEE 1722.1-2021 Table 9-1, reserved for future use)",
    (
        Field(
            "data",
            0,
            0,
            "bytes",
            "extended command/response data; IEEE 1722.1-2021 defines no format",
        ),
    ),
)
EXTENDED_DATA = EXTENDED.fields[-1]

ATDECC_LAYOUTS: tuple[Layout, ...] = (
    (
        ADP,
        ACMP,
        AECP,
        AEM,
        AA,
        AA_TLV,
        VU,
        JDKS_LOG,
        JDKS_IPV4,
        AUDIO_MAPPING,
        MVU,
        MVU_FLAGS,
        AVC,
        HDCP_APM,
        EXTENDED,
        AEM_BLOBS,
        DYNAMIC_INFO,
    )
    + tuple(PAYLOAD_LAYOUTS.values())
    + tuple(DESCRIPTOR_LAYOUTS.values())
    + tuple(TRAILER_ELEMENTS.values())
    + (
        CONTROL_VALUE,
        CONTROL_SMPTE,
        CONTROL_SAMPLE_RATE,
        CONTROL_GPTP,
        CONTROL_BODE_HEADER,
        CONTROL_BODE_POINT,
    )
    + (SF_AAF, SF_IEC61883, SF_CRF)
    + tuple(MVU_PAYLOAD_LAYOUTS.values())
)
