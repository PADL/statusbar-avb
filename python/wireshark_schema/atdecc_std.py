# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""ATDECC wire layouts written from the standards rather than extracted from
this repository's C++ (which has no struct for them): the IEEE 1722.1 AEM
payloads of commands the stack does not implement, the control value families
(IEEE 1722.1-2021 7.3.5), the Milan vendor-unique (MVU) protocol (Milan
Specification 1.3, 5.4.3-5.4.4) and the AVC AECP message.

Member dicts have the same shape as the extracted tables in atdecc_table.py so
the same layout builder handles both.
"""

from __future__ import annotations


def _struct(name: str, doc: str, members: list[tuple]) -> dict:
    out = []
    off = 0
    for name_, size, kind, mdoc in members:
        out.append(
            {"name": name_, "offset": off, "size": size, "kind": kind, "doc": mdoc}
        )
        off += size
    return {"name": name, "doc": doc, "length": off, "members": out}


# ---------------------------------------------------------------------------
# AEM payloads (IEEE 1722.1-2021 7.4), for commands without a C++ struct
# ---------------------------------------------------------------------------

_DT = ("descriptor_type", 2, "u16", "descriptor_type")
_DI = ("descriptor_index", 2, "u16", "descriptor_index")
_RSV2 = ("reserved", 2, "u16", "reserved")

STD_PAYLOAD_STRUCTS: dict[str, dict] = {
    "AemVideoFormatPayload": _struct(
        "AemVideoFormatPayload",
        "SET_VIDEO_FORMAT command/response, GET_VIDEO_FORMAT response (7.4.14/7.4.15)",
        [
            _DT,
            _DI,
            ("format_specific", 4, "u32", "format_specific"),
            ("aspect_ratio", 2, "u16", "aspect_ratio (width:height)"),
            ("color_space", 2, "u16", "color_space"),
            ("frame_size", 4, "u32", "frame_size (width << 16 | height)"),
        ],
    ),
    "AemGetVideoFormatCommandPayload": _struct(
        "AemGetVideoFormatCommandPayload", "GET_VIDEO_FORMAT command", [_DT, _DI]
    ),
    "AemSensorFormatPayload": _struct(
        "AemSensorFormatPayload",
        "SET_SENSOR_FORMAT command/response, GET_SENSOR_FORMAT response (7.4.16/7.4.17)",
        [_DT, _DI, ("sensor_format", 8, "u64", "sensor_format")],
    ),
    "AemGetSensorFormatCommandPayload": _struct(
        "AemGetSensorFormatCommandPayload", "GET_SENSOR_FORMAT command", [_DT, _DI]
    ),
    "AemAssociationIdPayload": _struct(
        "AemAssociationIdPayload",
        "SET_ASSOCIATION_ID command/response, GET_ASSOCIATION_ID response (7.4.22/7.4.23)",
        [("association_id", 8, "u64", "association_id")],
    ),
    "AemGetAsPathCommandPayload": _struct(
        "AemGetAsPathCommandPayload",
        "GET_AS_PATH command (7.4.41)",
        [("descriptor_index", 2, "u16", "AVB_INTERFACE descriptor_index"), _RSV2],
    ),
    "AemGetAsPathResponseHeader": _struct(
        "AemGetAsPathResponseHeader",
        "GET_AS_PATH response (7.4.41), followed by count clock identities",
        [
            ("descriptor_index", 2, "u16", "AVB_INTERFACE descriptor_index"),
            ("count", 2, "u16", "path_sequence entries"),
        ],
    ),
    "AemGetVideoMapCommandPayload": _struct(
        "AemGetVideoMapCommandPayload",
        "GET_VIDEO_MAP command (7.4.47)",
        [_DT, _DI, ("map_index", 2, "u16", "map_index"), _RSV2],
    ),
    "AemVideoMapResponseHeader": _struct(
        "AemVideoMapResponseHeader",
        "GET_VIDEO_MAP response (7.4.47), followed by the mappings",
        [
            _DT,
            _DI,
            ("map_index", 2, "u16", "map_index"),
            ("number_of_maps", 2, "u16", "number_of_maps"),
            ("number_of_mappings", 2, "u16", "number_of_mappings"),
            _RSV2,
        ],
    ),
    "AemVideoMappingsCommandHeader": _struct(
        "AemVideoMappingsCommandHeader",
        "ADD/REMOVE_VIDEO_MAPPINGS command/response (7.4.48/7.4.49), followed by the mappings",
        [_DT, _DI, ("number_of_mappings", 2, "u16", "number_of_mappings"), _RSV2],
    ),
    "AemGetSensorMapCommandPayload": _struct(
        "AemGetSensorMapCommandPayload",
        "GET_SENSOR_MAP command (7.4.50)",
        [_DT, _DI, ("map_index", 2, "u16", "map_index"), _RSV2],
    ),
    "AemSensorMapResponseHeader": _struct(
        "AemSensorMapResponseHeader",
        "GET_SENSOR_MAP response (7.4.50), followed by the mappings",
        [
            _DT,
            _DI,
            ("map_index", 2, "u16", "map_index"),
            ("number_of_maps", 2, "u16", "number_of_maps"),
            ("number_of_mappings", 2, "u16", "number_of_mappings"),
            _RSV2,
        ],
    ),
    "AemSensorMappingsCommandHeader": _struct(
        "AemSensorMappingsCommandHeader",
        "ADD/REMOVE_SENSOR_MAPPINGS command/response (7.4.51/7.4.52), followed by the mappings",
        [_DT, _DI, ("number_of_mappings", 2, "u16", "number_of_mappings"), _RSV2],
    ),
    "AemStreamEncryptionPayload": _struct(
        "AemStreamEncryptionPayload",
        "ENABLE/DISABLE_STREAM_ENCRYPTION command/response (7.4.69/7.4.70)",
        [_DT, _DI, ("key_eui", 8, "u64", "key_eui")],
    ),
    "AemMemoryObjectLengthPayload": _struct(
        "AemMemoryObjectLengthPayload",
        "SET_MEMORY_OBJECT_LENGTH command/response, GET_MEMORY_OBJECT_LENGTH response (7.4.71/7.4.72)",
        [
            ("descriptor_index", 2, "u16", "MEMORY_OBJECT descriptor_index"),
            _RSV2,
            ("length", 8, "u64", "length (octets)"),
        ],
    ),
    "AemGetMemoryObjectLengthCommandPayload": _struct(
        "AemGetMemoryObjectLengthCommandPayload",
        "GET_MEMORY_OBJECT_LENGTH command",
        [("descriptor_index", 2, "u16", "MEMORY_OBJECT descriptor_index"), _RSV2],
    ),
    "AemStreamBackupPayload": _struct(
        "AemStreamBackupPayload",
        "SET_STREAM_BACKUP command/response, GET_STREAM_BACKUP response (7.4.73/7.4.74)",
        [
            _DT,
            _DI,
            ("backup_talker_entity_id_0", 8, "u64", "backup_talker_entity_id_0"),
            ("backup_talker_unique_id_0", 2, "u16", "backup_talker_unique_id_0"),
            ("reserved_0", 2, "u16", "reserved"),
            ("backup_talker_entity_id_1", 8, "u64", "backup_talker_entity_id_1"),
            ("backup_talker_unique_id_1", 2, "u16", "backup_talker_unique_id_1"),
            ("reserved_1", 2, "u16", "reserved"),
            ("backup_talker_entity_id_2", 8, "u64", "backup_talker_entity_id_2"),
            ("backup_talker_unique_id_2", 2, "u16", "backup_talker_unique_id_2"),
            ("reserved_2", 2, "u16", "reserved"),
            ("backedup_talker_entity_id", 8, "u64", "backedup_talker_entity_id"),
            ("backedup_talker_unique_id", 2, "u16", "backedup_talker_unique_id"),
            ("reserved_3", 2, "u16", "reserved"),
        ],
    ),
    "AemGetStreamBackupCommandPayload": _struct(
        "AemGetStreamBackupCommandPayload", "GET_STREAM_BACKUP command", [_DT, _DI]
    ),
}

#: command -> (command payload, response payload); "+elements:<element>:<count member>"
#: appends a counted list of trailer elements
STD_AEM_PAYLOADS: dict[str, tuple[str | None, str | None]] = {
    "SET_VIDEO_FORMAT": ("AemVideoFormatPayload", "AemVideoFormatPayload"),
    "GET_VIDEO_FORMAT": ("AemGetVideoFormatCommandPayload", "AemVideoFormatPayload"),
    "SET_SENSOR_FORMAT": ("AemSensorFormatPayload", "AemSensorFormatPayload"),
    "GET_SENSOR_FORMAT": ("AemGetSensorFormatCommandPayload", "AemSensorFormatPayload"),
    "SET_ASSOCIATION_ID": ("AemAssociationIdPayload", "AemAssociationIdPayload"),
    "GET_ASSOCIATION_ID": (None, "AemAssociationIdPayload"),
    "GET_AS_PATH": (
        "AemGetAsPathCommandPayload",
        "AemGetAsPathResponseHeader+elements:as_path_entry:count",
    ),
    "GET_VIDEO_MAP": (
        "AemGetVideoMapCommandPayload",
        "AemVideoMapResponseHeader+elements:video_mapping:number_of_mappings",
    ),
    "ADD_VIDEO_MAPPINGS": (
        "AemVideoMappingsCommandHeader+elements:video_mapping:number_of_mappings",
        "AemVideoMappingsCommandHeader+elements:video_mapping:number_of_mappings",
    ),
    "REMOVE_VIDEO_MAPPINGS": (
        "AemVideoMappingsCommandHeader+elements:video_mapping:number_of_mappings",
        "AemVideoMappingsCommandHeader+elements:video_mapping:number_of_mappings",
    ),
    "GET_SENSOR_MAP": (
        "AemGetSensorMapCommandPayload",
        "AemSensorMapResponseHeader+elements:sensor_mapping:number_of_mappings",
    ),
    "ADD_SENSOR_MAPPINGS": (
        "AemSensorMappingsCommandHeader+elements:sensor_mapping:number_of_mappings",
        "AemSensorMappingsCommandHeader+elements:sensor_mapping:number_of_mappings",
    ),
    "REMOVE_SENSOR_MAPPINGS": (
        "AemSensorMappingsCommandHeader+elements:sensor_mapping:number_of_mappings",
        "AemSensorMappingsCommandHeader+elements:sensor_mapping:number_of_mappings",
    ),
    "ENABLE_STREAM_ENCRYPTION": (
        "AemStreamEncryptionPayload",
        "AemStreamEncryptionPayload",
    ),
    "DISABLE_STREAM_ENCRYPTION": (
        "AemStreamEncryptionPayload",
        "AemStreamEncryptionPayload",
    ),
    "SET_MEMORY_OBJECT_LENGTH": (
        "AemMemoryObjectLengthPayload",
        "AemMemoryObjectLengthPayload",
    ),
    "GET_MEMORY_OBJECT_LENGTH": (
        "AemGetMemoryObjectLengthCommandPayload",
        "AemMemoryObjectLengthPayload",
    ),
    "SET_STREAM_BACKUP": ("AemStreamBackupPayload", "AemStreamBackupPayload"),
    "GET_STREAM_BACKUP": ("AemGetStreamBackupCommandPayload", "AemStreamBackupPayload"),
}

# ---------------------------------------------------------------------------
# Control value families (IEEE 1722.1-2021 7.3.5.2, Table 7.12)
# ---------------------------------------------------------------------------

_LINEAR_KINDS = ("i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64")
_KIND_SIZE = {
    "i8": 1,
    "u8": 1,
    "i16": 2,
    "u16": 2,
    "i32": 4,
    "u32": 4,
    "i64": 8,
    "u64": 8,
    "f32": 4,
    "f64": 8,
}

#: value_type -> (family, element size, element kind)
CONTROL_VALUE_FAMILY: dict[int, tuple[str, int, str | None]] = {}
for _i, _k in enumerate(_LINEAR_KINDS):
    CONTROL_VALUE_FAMILY[0x0000 + _i] = ("linear", _KIND_SIZE[_k], _k)
    CONTROL_VALUE_FAMILY[0x000A + _i] = ("selector", _KIND_SIZE[_k], _k)
    CONTROL_VALUE_FAMILY[0x0015 + _i] = ("array", _KIND_SIZE[_k], _k)
CONTROL_VALUE_FAMILY[0x0014] = ("selector", 2, "string_ref")
CONTROL_VALUE_FAMILY[0x001F] = ("utf8", 0, None)
CONTROL_VALUE_FAMILY[0x0020] = ("bode_plot", 12, None)
CONTROL_VALUE_FAMILY[0x0021] = ("smpte_time", 10, None)
CONTROL_VALUE_FAMILY[0x0022] = ("sample_rate", 16, None)
CONTROL_VALUE_FAMILY[0x0023] = ("gptp_time", 10, None)
CONTROL_VALUE_FAMILY[0x3FFE] = ("vendor", 0, None)
CONTROL_VALUE_FAMILY[0x3FFF] = ("expansion", 0, None)

# ---------------------------------------------------------------------------
# Milan vendor unique (MVU), Milan Specification 1.3 clause 5.4.3-5.4.4
# ---------------------------------------------------------------------------

MVU_PROTOCOL_ID = "001bc50ac100"
MVU_HEADER_LENGTH = 30  # AECP common 22 + protocol_id 6 + u/command_type 2

MVU_COMMANDS: dict[int, str] = {
    0x0000: "GET_MILAN_INFO",
    0x0001: "SET_SYSTEM_UNIQUE_ID",
    0x0002: "GET_SYSTEM_UNIQUE_ID",
    0x0003: "SET_MEDIA_CLOCK_REFERENCE_INFO",
    0x0004: "GET_MEDIA_CLOCK_REFERENCE_INFO",
    0x0005: "BIND_STREAM",
    0x0006: "UNBIND_STREAM",
    0x0007: "GET_STREAM_INPUT_INFO_EX",
}
MVU_STATUS: dict[int, str] = {
    0: "SUCCESS",
    1: "NOT_IMPLEMENTED",
    2: "NO_SUCH_DESCRIPTOR",
    3: "ENTITY_LOCKED",
    7: "BAD_ARGUMENTS",
}
MVU_FEATURES_FLAGS: tuple[tuple[str, int], ...] = (
    ("redundancy", 0x00000001),
    ("talker_dynamic_mappings", 0x00000002),
    ("mvu_binding", 0x00000004),
    ("talker_signal_presence", 0x00000008),
)
MVU_MCR_FLAGS: tuple[tuple[str, int], ...] = (
    ("media_clock_reference", 0x01),
    ("media_clock_domain", 0x02),
)

# offsets relative to MVU_HEADER_LENGTH; a 64-octet name that Milan 1.3 added
# after the fixed part is optional (OPTIONAL_NAME) so 1.2 frames still decode
MVU_PAYLOAD_STRUCTS: dict[str, dict] = {
    "MvuReserved": _struct(
        "MvuReserved", "GET_MILAN_INFO / GET_SYSTEM_UNIQUE_ID command", [_RSV2]
    ),
    "MvuGetMilanInfoResponse": _struct(
        "MvuGetMilanInfoResponse",
        "GET_MILAN_INFO response (Milan 1.3 Figure 5.6)",
        [
            _RSV2,
            ("protocol_version", 4, "u32", "protocol_version"),
            ("features_flags", 4, "u32", "features_flags (Table 5.17)"),
            ("certification_version", 4, "u32", "certification_version"),
            ("specification_version", 4, "u32", "specification_version"),
        ],
    ),
    "MvuSystemUniqueId": _struct(
        "MvuSystemUniqueId",
        "SET_SYSTEM_UNIQUE_ID command/response, GET_SYSTEM_UNIQUE_ID response (Figure 5.7)",
        [_RSV2, ("system_unique_id", 8, "u64", "system_unique_id")],
    ),
    "MvuMediaClockReferenceInfo": _struct(
        "MvuMediaClockReferenceInfo",
        "SET_MEDIA_CLOCK_REFERENCE_INFO command/response, GET_... response (Figure 5.8)",
        [
            ("clock_domain_index", 2, "u16", "CLOCK_DOMAIN descriptor_index"),
            ("mcr_flags", 1, "u8", "flags (Table 5.18)"),
            ("reserved_1", 1, "u8", "reserved"),
            ("default_mcr_prio", 1, "u8", "default media clock reference priority"),
            ("user_mcr_prio", 1, "u8", "user media clock reference priority"),
            ("reserved_2", 4, "u32", "reserved"),
        ],
    ),
    "MvuGetMediaClockReferenceInfoCommand": _struct(
        "MvuGetMediaClockReferenceInfoCommand",
        "GET_MEDIA_CLOCK_REFERENCE_INFO command (Figure 5.9)",
        [("clock_domain_index", 2, "u16", "CLOCK_DOMAIN descriptor_index")],
    ),
    "MvuBindStream": _struct(
        "MvuBindStream",
        "BIND_STREAM command/response (Figure 5.10)",
        [
            ("bind_flags", 2, "u16", "flags (Table 5.19)"),
            _DT,
            _DI,
            ("talker_entity_id", 8, "u64", "talker_entity_id"),
            ("talker_stream_index", 2, "u16", "talker STREAM_OUTPUT index"),
            _RSV2,
        ],
    ),
    "MvuStreamInputRef": _struct(
        "MvuStreamInputRef",
        "UNBIND_STREAM command/response, GET_STREAM_INPUT_INFO_EX command (Figure 5.11)",
        [_RSV2, _DT, _DI],
    ),
    "MvuStreamInputInfoExResponse": _struct(
        "MvuStreamInputInfoExResponse",
        "GET_STREAM_INPUT_INFO_EX response (Figure 5.12)",
        [
            _RSV2,
            _DT,
            _DI,
            ("talker_entity_id", 8, "u64", "talker_entity_id"),
            ("talker_unique_id", 2, "u16", "bound talker unique id"),
            ("pbsta", 1, "u8", "probing status"),
            ("acmpsta", 1, "u8", "ACMP status"),
        ],
    ),
}
MVU_OPTIONAL_NAME: dict[str, str] = {
    "MvuSystemUniqueId": "system_name",
    "MvuMediaClockReferenceInfo": "media_clock_domain_name",
}
MVU_PAYLOADS: dict[int, tuple[str | None, str | None]] = {
    0x0000: ("MvuReserved", "MvuGetMilanInfoResponse"),
    0x0001: ("MvuSystemUniqueId", "MvuSystemUniqueId"),
    0x0002: ("MvuReserved", "MvuSystemUniqueId"),
    0x0003: ("MvuMediaClockReferenceInfo", "MvuMediaClockReferenceInfo"),
    0x0004: ("MvuGetMediaClockReferenceInfoCommand", "MvuMediaClockReferenceInfo"),
    0x0005: ("MvuBindStream", "MvuBindStream"),
    0x0006: ("MvuStreamInputRef", "MvuStreamInputRef"),
    0x0007: ("MvuStreamInputRef", "MvuStreamInputInfoExResponse"),
}

# AVC AECP message (IEEE 1722.1-2021 9.2.1.3): avc_length then up to 512
# octets of IEEE 1394 AV/C command/response frame
AVC_HEADER_LENGTH = 24
