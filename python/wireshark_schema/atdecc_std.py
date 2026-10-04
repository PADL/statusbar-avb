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
    "AemMapResponseHeader": _struct(
        "AemMapResponseHeader",
        "GET_AUDIO_MAP / GET_VIDEO_MAP / GET_SENSOR_MAP response (Figure 7-70), followed by the mappings",
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
    "AemSensorMappingsCommandHeader": _struct(
        "AemSensorMappingsCommandHeader",
        "ADD/REMOVE_SENSOR_MAPPINGS command/response (7.4.51/7.4.52), followed by the mappings",
        [_DT, _DI, ("number_of_mappings", 2, "u16", "number_of_mappings"), _RSV2],
    ),
    "AemStreamEncryptionPayload": _struct(
        "AemStreamEncryptionPayload",
        "ENABLE/DISABLE_STREAM_ENCRYPTION command/response (7.4.69/7.4.70)",
        [_DT, _DI, ("key_id", 8, "u64", "key_id (EUI-64 of the key)")],
    ),
    "AemMemoryObjectLengthPayload": _struct(
        "AemMemoryObjectLengthPayload",
        "SET_MEMORY_OBJECT_LENGTH command/response, GET_MEMORY_OBJECT_LENGTH response (7.4.71/7.4.72)",
        [
            ("descriptor_index", 2, "u16", "MEMORY_OBJECT descriptor_index"),
            ("configuration_index", 2, "u16", "CONFIGURATION descriptor_index"),
            ("length", 8, "u64", "length (octets)"),
        ],
    ),
    "AemGetMemoryObjectLengthCommandPayload": _struct(
        "AemGetMemoryObjectLengthCommandPayload",
        "GET_MEMORY_OBJECT_LENGTH command",
        [
            ("descriptor_index", 2, "u16", "MEMORY_OBJECT descriptor_index"),
            ("configuration_index", 2, "u16", "CONFIGURATION descriptor_index"),
        ],
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
        "AemMapResponseHeader+elements:video_mapping:number_of_mappings",
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
        "AemMapResponseHeader+elements:sensor_mapping:number_of_mappings",
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
    "DISABLE_STREAM_ENCRYPTION": ("AemDescriptorRefPayload", "AemDescriptorRefPayload"),
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


# ---------------------------------------------------------------------------
# Verified / added against IEEE 1722.1-2021 (docs/1722.1-2021.pdf)
# ---------------------------------------------------------------------------

_KEY = ("key_id", 8, "u64", "key_id (EUI-64 of the key)")
_LOG_INTERVALS = [
    ("log_announce_interval", 1, "i8", "log2 announce interval"),
    ("log_sync_interval", 1, "i8", "log2 sync interval"),
    ("log_pdelay_request_interval", 1, "i8", "log2 pdelay request interval"),
    ("log_gptp_capable_interval", 1, "i8", "log2 gPTP capable message interval"),
]
_GM_FIELDS = [
    ("gm_clock_identity", 8, "u64", "parentDS.grandmasterIdentity"),
    ("gm_clock_class", 1, "u8", "grandmaster clockClass"),
    ("gm_clock_accuracy", 1, "u8", "grandmaster clockAccuracy"),
    ("gm_offset_scaled_log_variance", 2, "u16", "grandmaster offsetScaledLogVariance"),
    ("gm_priority1", 1, "u8", "grandmaster priority1"),
    ("gm_priority2", 1, "u8", "grandmaster priority2"),
    ("gm_time_source", 1, "u8", "timePropertiesDS.timeSource"),
    ("gm_flags", 1, "u8", "gm_cv gm_l59 gm_l61 gm_tt gm_ft gm_pt"),
    ("gm_current_utc_offset", 2, "i16", "timePropertiesDS.currentUTCOffset"),
    ("reserved3", 2, "u16", "reserved"),
]
_INSTANCE_FIELDS = [
    _DT,
    _DI,
    ("clock_class", 1, "u8", "defaultDS.clockQuality.clockClass"),
    ("clock_accuracy", 1, "u8", "defaultDS.clockQuality.clockAccuracy"),
    (
        "offset_scaled_log_variance",
        2,
        "u16",
        "defaultDS.clockQuality.offsetScaledLogVariance",
    ),
    ("priority1", 1, "u8", "defaultDS.priority1"),
    ("priority2", 1, "u8", "defaultDS.priority2"),
    ("domain_number", 1, "u8", "defaultDS.domainNumber"),
    ("time_source", 1, "u8", "defaultDS.timeSource"),
    ("current_utc_offset", 2, "i16", "defaultDS.currentUTCOffset"),
    ("instance_flags", 2, "u16", "cv l59 l61 tt ft pt so ee ie"),
] + _GM_FIELDS
_PARENT_FIELDS = [
    ("parent_clock_identity", 8, "u64", "parentDS.parentPortIdentity.clockIdentity"),
    ("parent_port_number", 2, "u16", "parentDS.parentPortIdentity.portNumber"),
    ("steps_removed", 2, "u16", "currentDS.stepsRemoved"),
]
_PM_COUNTS = [
    _DT,
    _DI,
    ("max_count_of_24h", 2, "u16", "24-hour records the list can hold"),
    ("count_of_24h", 2, "u16", "valid 24-hour records"),
    ("max_count_of_15m", 2, "u16", "15-minute records the list can hold"),
    ("count_of_15m", 2, "u16", "valid 15-minute records"),
]
_RECORD_CMD = [_DT, _DI, ("record_index", 2, "u16", "record index"), _RSV2]
_RECORD_HEAD = [
    _DT,
    _DI,
    ("record_index", 2, "u16", "record index"),
    ("record_flags", 2, "u16", "record flags"),
    ("timestamp", 8, "u64", "record start (ns, entity epoch)"),
]
_PM_STATS = ("average", "minimum", "maximum", "std_dev")

STD_PAYLOAD_STRUCTS.update(
    {
        "AemEmptyPayload": _struct("AemEmptyPayload", "no fixed payload", []),
        "AemDescriptorRefPayload": _struct(
            "AemDescriptorRefPayload",
            "descriptor_type + descriptor_index only (DISABLE_STREAM_ENCRYPTION, AUTHENTICATE response, DEAUTHENTICATE, the GET_PTP_* commands)",
            [_DT, _DI],
        ),
        # --- authentication and security (7.4.56-7.4.69, 7.4.103-7.4.104) ---
        "AemAuthKeyPayload": _struct(
            "AemAuthKeyPayload",
            "AUTH_ADD_KEY command, AUTH_GET_KEY response (Figure 7-75), followed by key_length octets of key",
            [
                _KEY,
                ("key_type", 1, "u8", "key type (7.6.1.2)"),
                ("key_length", 2, "u16", "key length (octets)"),
                ("reserved", 1, "u8", "reserved"),
            ],
        ),
        "AemKeyIdPayload": _struct(
            "AemKeyIdPayload",
            "AUTH_ADD_KEY response, AUTH_GET_KEY command, AUTH_DELETE_KEY, ENABLE_TRANSPORT_SECURITY (Figures 7-76 / 7-87)",
            [_KEY],
        ),
        "AemAuthGetKeyListCommandPayload": _struct(
            "AemAuthGetKeyListCommandPayload",
            "AUTH_GET_KEY_LIST command (Figure 7-77)",
            [("keychain_id", 2, "u16", "keychain (Table 7-184)"), _RSV2],
        ),
        "AemAuthGetKeyListResponsePayload": _struct(
            "AemAuthGetKeyListResponsePayload",
            "AUTH_GET_KEY_LIST response (Figure 7-78)",
            [
                ("keychain_id", 2, "u16", "keychain (Table 7-184)"),
                ("number_of_keys", 2, "u16", "keys in the keychain"),
            ],
        ),
        "AemAuthKeychainKeyPayload": _struct(
            "AemAuthKeychainKeyPayload",
            "AUTH_ADD_KEY_TO_CHAIN / AUTH_DELETE_KEY_FROM_CHAIN command and response (Figure 7-79)",
            [("keychain_id", 2, "u16", "keychain (Table 7-184)"), _RSV2, _KEY],
        ),
        "AemAuthGetKeychainListCommandPayload": _struct(
            "AemAuthGetKeychainListCommandPayload",
            "AUTH_GET_KEYCHAIN_LIST command (Figure 7-80)",
            [
                ("keychain_id", 2, "u16", "keychain (Table 7-184)"),
                ("list_index", 2, "u16", "list subset index"),
            ],
        ),
        "AemAuthGetKeychainListResponseHeader": _struct(
            "AemAuthGetKeychainListResponseHeader",
            "AUTH_GET_KEYCHAIN_LIST response (Figure 7-81), followed by number_of_keys key EUI-64s",
            [
                ("keychain_id", 2, "u16", "keychain (Table 7-184)"),
                ("list_index", 2, "u16", "list subset index"),
                ("number_of_lists", 2, "u16", "list subsets"),
                ("number_of_keys", 2, "u16", "keys in this subset"),
            ],
        ),
        "AemAuthGetIdentityResponsePayload": _struct(
            "AemAuthGetIdentityResponsePayload",
            "AUTH_GET_IDENTITY response (Figure 7-82)",
            [
                _KEY,
                ("ecdsa_signature_c", 32, "bytes", "ECDSA signature c"),
                ("ecdsa_signature_d", 32, "bytes", "ECDSA signature d"),
            ],
        ),
        "AemAuthTokenPayload": _struct(
            "AemAuthTokenPayload",
            "AUTH_ADD_TOKEN command (Figure 7-83), followed by token_length octets of token",
            [("token_length", 2, "u16", "authentication token length (octets)"), _RSV2],
        ),
        "AemAuthenticateCommandPayload": _struct(
            "AemAuthenticateCommandPayload",
            "AUTHENTICATE command (Figure 7-84), followed by token_length octets of token",
            [
                _DT,
                _DI,
                ("token_length", 2, "u16", "authentication token length (octets)"),
                _RSV2,
            ],
        ),
        "AemAuthNonceCommandPayload": _struct(
            "AemAuthNonceCommandPayload",
            "AUTH_GET_NONCE command (Figure 7-138)",
            [("controller_nonce", 8, "u64", "controller nonce")],
        ),
        "AemAuthNonceResponsePayload": _struct(
            "AemAuthNonceResponsePayload",
            "AUTH_GET_NONCE response (Figure 7-139)",
            [
                ("controller_nonce", 8, "u64", "controller nonce"),
                ("target_nonce", 8, "u64", "target nonce"),
            ],
        ),
        "AemAuthAddKeyNoncePayload": _struct(
            "AemAuthAddKeyNoncePayload",
            "AUTH_ADD_KEY_NONCE command (Figure 7-140), followed by key_length octets of key",
            [
                ("controller_nonce", 8, "u64", "controller nonce"),
                ("target_nonce", 8, "u64", "target nonce"),
                _KEY,
                ("key_type", 1, "u8", "key type (7.6.1.2)"),
                ("key_length", 2, "u16", "key length (octets)"),
                ("reserved", 1, "u8", "reserved"),
            ],
        ),
        "AemAuthAddKeyNonceResponsePayload": _struct(
            "AemAuthAddKeyNonceResponsePayload",
            "AUTH_ADD_KEY_NONCE response (Figure 7-141)",
            [
                ("controller_nonce", 8, "u64", "controller nonce"),
                ("target_nonce", 8, "u64", "target nonce"),
                _KEY,
            ],
        ),
        # --- 7.4.79 / 7.4.80 sampling rate range (7.3.2) ---
        "AemSamplingRateRangePayload": _struct(
            "AemSamplingRateRangePayload",
            "SET_SAMPLING_RATE_RANGE command/response, GET_SAMPLING_RATE_RANGE response (Figure 7-97)",
            [
                _DT,
                _DI,
                (
                    "minimum_sampling_rate",
                    4,
                    "u32",
                    "minimum sampling rate (pull | base_frequency)",
                ),
                (
                    "maximum_sampling_rate",
                    4,
                    "u32",
                    "maximum sampling rate (pull | base_frequency)",
                ),
            ],
        ),
        # --- 7.4.81-7.4.88 PTP instance ---
        "AemSetPtpInstanceInfoPayload": _struct(
            "AemSetPtpInstanceInfoPayload",
            "SET_PTP_INSTANCE_INFO command/response (Figure 7-99)",
            [
                _DT,
                _DI,
                ("reserved1", 2, "u16", "reserved"),
                ("set_flags", 2, "u16", "flags (Table 7-165)"),
                ("priority1", 1, "u8", "defaultDS.priority1"),
                ("priority2", 1, "u8", "defaultDS.priority2"),
                ("domain_number", 1, "u8", "defaultDS.domainNumber"),
                ("instance_booleans", 1, "u8", "so ee ie"),
            ],
        ),
        "AemPtpInstanceInfoResponsePayload": _struct(
            "AemPtpInstanceInfoResponsePayload",
            "GET_PTP_INSTANCE_INFO response (Figure 7-101)",
            _INSTANCE_FIELDS,
        ),
        "AemPtpInstanceExtendedInfoResponsePayload": _struct(
            "AemPtpInstanceExtendedInfoResponsePayload",
            "GET_PTP_INSTANCE_EXTENDED_INFO response (Figure 7-103)",
            _INSTANCE_FIELDS
            + _PARENT_FIELDS
            + [
                ("cumulative_rate_ratio", 4, "i32", "parentDS.cumulativeRateRatio"),
                ("valid_flags", 2, "u16", "valid_flags (Table 7-166)"),
                ("gm_timebase_indicator", 2, "u16", "currentDS.gmTimebaseIndicator"),
                (
                    "offset_from_master",
                    12,
                    "bytes",
                    "currentDS.offsetFromMaster (ScaledNs)",
                ),
                (
                    "last_gm_phase_change",
                    12,
                    "bytes",
                    "currentDS.lastGmPhaseChange (ScaledNs)",
                ),
                ("last_gm_freq_change", 4, "f32", "currentDS.lastGmFreqChange"),
                ("gm_change_count", 4, "u32", "currentDS.gmChangeCount"),
                (
                    "time_of_last_gm_change",
                    4,
                    "u32",
                    "currentDS.timeOfLastGmChangeEvent",
                ),
                (
                    "time_of_last_gm_phase_change",
                    4,
                    "u32",
                    "currentDS.timeOfLastGmPhaseChangeEvent",
                ),
                (
                    "time_of_last_gm_freq_change",
                    4,
                    "u32",
                    "currentDS.timeOfLastGmFreqChangeEvent",
                ),
            ],
        ),
        "AemPtpInstanceGrandmasterInfoResponsePayload": _struct(
            "AemPtpInstanceGrandmasterInfoResponsePayload",
            "GET_PTP_INSTANCE_GRANDMASTER_INFO response (Figure 7-105)",
            [_DT, _DI] + _GM_FIELDS + _PARENT_FIELDS,
        ),
        "AemPtpPathCountResponsePayload": _struct(
            "AemPtpPathCountResponsePayload",
            "GET_PTP_INSTANCE_PATH_COUNT response (Figure 7-107)",
            [_DT, _DI, _RSV2, ("trace_count", 2, "u16", "pathTraceDS.list entries")],
        ),
        "AemPtpPathTraceCommandPayload": _struct(
            "AemPtpPathTraceCommandPayload",
            "GET_PTP_INSTANCE_PATH_TRACE command (Figure 7-108)",
            [
                _DT,
                _DI,
                ("start_index", 2, "u16", "first pathTraceDS.list entry"),
                _RSV2,
            ],
        ),
        "AemPtpPathTraceResponseHeader": _struct(
            "AemPtpPathTraceResponseHeader",
            "GET_PTP_INSTANCE_PATH_TRACE response (Figure 7-109), followed by entry_count clock identities",
            [
                _DT,
                _DI,
                ("start_index", 2, "u16", "first pathTraceDS.list entry"),
                ("entry_count", 2, "u16", "entries in path_trace"),
            ],
        ),
        "AemPtpPerfMonCountResponsePayload": _struct(
            "AemPtpPerfMonCountResponsePayload",
            "GET_PTP_INSTANCE_PERF_MON_COUNT / GET_PTP_PORT_PDELAY_MON_COUNT / GET_PTP_PORT_PERF_MON_COUNT response (Figures 7-111 / 7-129 / 7-133)",
            _PM_COUNTS,
        ),
        "AemPtpRecordCommandPayload": _struct(
            "AemPtpRecordCommandPayload",
            "GET_PTP_INSTANCE_PERF_MON_RECORD / GET_PTP_PORT_PDELAY_MON_RECORD / GET_PTP_PORT_PERF_MON_RECORD command (Figures 7-112 / 7-130 / 7-134)",
            _RECORD_CMD,
        ),
        "AemPtpInstancePerfMonRecordResponsePayload": _struct(
            "AemPtpInstancePerfMonRecordResponsePayload",
            "GET_PTP_INSTANCE_PERF_MON_RECORD response (Figure 7-113)",
            _RECORD_HEAD
            + [
                (
                    f"{stat}_{what}",
                    8,
                    "i64",
                    f"{stat} {what.replace('_', ' ')} (TimeInterval)",
                )
                for what in (
                    "master_slave_delay",
                    "slave_master_delay",
                    "mean_path_delay",
                    "offset_from_master",
                )
                for stat in _PM_STATS
            ],
        ),
        # --- 7.4.89-7.4.101 PTP port ---
        "AemPtpPortIntervalsPayload": _struct(
            "AemPtpPortIntervalsPayload",
            "SET/GET_PTP_PORT_INITIAL_INTERVALS, GET_PTP_PORT_CURRENT_INTERVALS, SET/GET_PTP_PORT_REMOTE_INTERVALS (Figures 7-114..7-121)",
            [
                _DT,
                _DI,
                ("reserved1", 2, "u16", "reserved"),
                ("interval_flags", 2, "u16", "flags (Tables 7-168..7-172)"),
            ]
            + _LOG_INTERVALS,
        ),
        "AemSetPtpPortOverridesPayload": _struct(
            "AemSetPtpPortOverridesPayload",
            "SET_PTP_PORT_OVERRIDES command/response (Figure 7-125)",
            [
                _DT,
                _DI,
                ("override_flags", 2, "u16", "flags (Table 7-175)"),
                ("override_booleans", 2, "u16", "booleans (Table 7-176)"),
            ]
            + _LOG_INTERVALS
            + [
                (
                    "desired_state",
                    1,
                    "u8",
                    "externalPortConfigurationPortDS.desiredState",
                ),
                ("reserved", 3, "bytes", "reserved"),
            ],
        ),
        "AemGetPtpPortOverridesResponsePayload": _struct(
            "AemGetPtpPortOverridesResponsePayload",
            "GET_PTP_PORT_OVERRIDES response (Figure 7-127)",
            [
                _DT,
                _DI,
                ("reserved1", 2, "u16", "reserved"),
                ("override_booleans", 2, "u16", "booleans (Table 7-177)"),
            ]
            + _LOG_INTERVALS
            + [
                (
                    "desired_state",
                    1,
                    "u8",
                    "externalPortConfigurationPortDS.desiredState",
                ),
                ("reserved2", 3, "bytes", "reserved"),
            ],
        ),
        "AemPtpPortPdelayMonRecordResponsePayload": _struct(
            "AemPtpPortPdelayMonRecordResponsePayload",
            "GET_PTP_PORT_PDELAY_MON_RECORD response (Figure 7-131)",
            _RECORD_HEAD
            + [
                (
                    f"{stat}_mean_link_delay",
                    8,
                    "i64",
                    f"{stat} mean link delay (TimeInterval)",
                )
                for stat in _PM_STATS
            ],
        ),
        "AemPtpPortPerfMonRecordResponsePayload": _struct(
            "AemPtpPortPerfMonRecordResponsePayload",
            "GET_PTP_PORT_PERF_MON_RECORD response (Figure 7-135)",
            _RECORD_HEAD
            + [
                (name, 4, "u32", name.replace("_", " ") + " count")
                for name in (
                    "announce_tx",
                    "announce_rx",
                    "announce_foreign_master_rx",
                    "sync_tx",
                    "sync_rx",
                    "followup_tx",
                    "followup_rx",
                    "delay_req_tx",
                    "delay_req_rx",
                    "delay_resp_tx",
                    "delay_resp_rx",
                    "pdelay_req_tx",
                    "pdelay_req_rx",
                    "pdelay_resp_tx",
                    "pdelay_resp_rx",
                    "pdelay_resp_followup_tx",
                    "pdelay_resp_followup_rx",
                )
            ],
        ),
        # --- 7.4.102 ---
        "AemPathLatencyResponsePayload": _struct(
            "AemPathLatencyResponsePayload",
            "GET_PATH_LATENCY response (Figure 7-137)",
            [_DT, _DI, ("path_latency", 4, "u32", "path latency (ns)")],
        ),
    }
)

STD_AEM_PAYLOADS.update(
    {
        # the C++ AemAudioMapResponseHeader has number_of_mappings and number_of_maps
        # swapped relative to Figure 7-70; the dissector follows the standard
        "GET_AUDIO_MAP": (
            "AemGetAudioMapCommandPayload",
            "AemMapResponseHeader+elements:audio_mapping:number_of_mappings",
        ),
        "AUTH_ADD_KEY": ("AemAuthKeyPayload+blob:key:key_length", "AemKeyIdPayload"),
        "AUTH_DELETE_KEY": ("AemKeyIdPayload", "AemKeyIdPayload"),
        "AUTH_GET_KEY_LIST": (
            "AemAuthGetKeyListCommandPayload",
            "AemAuthGetKeyListResponsePayload",
        ),
        "AUTH_GET_KEY": ("AemKeyIdPayload", "AemAuthKeyPayload+blob:key:key_length"),
        "AUTH_ADD_KEY_TO_CHAIN": (
            "AemAuthKeychainKeyPayload",
            "AemAuthKeychainKeyPayload",
        ),
        "AUTH_DELETE_KEY_FROM_CHAIN": (
            "AemAuthKeychainKeyPayload",
            "AemAuthKeychainKeyPayload",
        ),
        "AUTH_GET_KEYCHAIN_LIST": (
            "AemAuthGetKeychainListCommandPayload",
            "AemAuthGetKeychainListResponseHeader+elements:key_eui:number_of_keys",
        ),
        "AUTH_GET_IDENTITY": (None, "AemAuthGetIdentityResponsePayload"),
        "AUTH_ADD_TOKEN": (
            "AemAuthTokenPayload+blob:authentication_token:token_length",
            None,
        ),
        "AUTH_DELETE_TOKEN": (None, None),
        "AUTHENTICATE": (
            "AemAuthenticateCommandPayload+blob:authentication_token:token_length",
            "AemDescriptorRefPayload",
        ),
        "DEAUTHENTICATE": ("AemDescriptorRefPayload", "AemDescriptorRefPayload"),
        "ENABLE_TRANSPORT_SECURITY": ("AemKeyIdPayload", "AemKeyIdPayload"),
        "DISABLE_TRANSPORT_SECURITY": (None, None),
        "GET_DYNAMIC_INFO": (
            "AemEmptyPayload+dynamic_infos",
            "AemEmptyPayload+dynamic_infos",
        ),
        "SET_SAMPLING_RATE_RANGE": (
            "AemSamplingRateRangePayload",
            "AemSamplingRateRangePayload",
        ),
        "GET_SAMPLING_RATE_RANGE": (
            "AemDescriptorRefPayload",
            "AemSamplingRateRangePayload",
        ),
        "SET_PTP_INSTANCE_INFO": (
            "AemSetPtpInstanceInfoPayload",
            "AemSetPtpInstanceInfoPayload",
        ),
        "GET_PTP_INSTANCE_INFO": (
            "AemDescriptorRefPayload",
            "AemPtpInstanceInfoResponsePayload",
        ),
        "GET_PTP_INSTANCE_EXTENDED_INFO": (
            "AemDescriptorRefPayload",
            "AemPtpInstanceExtendedInfoResponsePayload",
        ),
        "GET_PTP_INSTANCE_GRANDMASTER_INFO": (
            "AemDescriptorRefPayload",
            "AemPtpInstanceGrandmasterInfoResponsePayload",
        ),
        "GET_PTP_INSTANCE_PATH_COUNT": (
            "AemDescriptorRefPayload",
            "AemPtpPathCountResponsePayload",
        ),
        "GET_PTP_INSTANCE_PATH_TRACE": (
            "AemPtpPathTraceCommandPayload",
            "AemPtpPathTraceResponseHeader+elements:clock_identity:entry_count",
        ),
        "GET_PTP_INSTANCE_PERF_MON_COUNT": (
            "AemDescriptorRefPayload",
            "AemPtpPerfMonCountResponsePayload",
        ),
        "GET_PTP_INSTANCE_PERF_MON_RECORD": (
            "AemPtpRecordCommandPayload",
            "AemPtpInstancePerfMonRecordResponsePayload",
        ),
        "SET_PTP_PORT_INITIAL_INTERVALS": (
            "AemPtpPortIntervalsPayload",
            "AemPtpPortIntervalsPayload",
        ),
        "GET_PTP_PORT_INITIAL_INTERVALS": (
            "AemDescriptorRefPayload",
            "AemPtpPortIntervalsPayload",
        ),
        "GET_PTP_PORT_CURRENT_INTERVALS": (
            "AemDescriptorRefPayload",
            "AemPtpPortIntervalsPayload",
        ),
        "SET_PTP_PORT_REMOTE_INTERVALS": (
            "AemPtpPortIntervalsPayload",
            "AemPtpPortIntervalsPayload",
        ),
        "GET_PTP_PORT_REMOTE_INTERVALS": (
            "AemDescriptorRefPayload",
            "AemPtpPortIntervalsPayload",
        ),
        "SET_PTP_PORT_OVERRIDES": (
            "AemSetPtpPortOverridesPayload",
            "AemSetPtpPortOverridesPayload",
        ),
        "GET_PTP_PORT_OVERRIDES": (
            "AemDescriptorRefPayload",
            "AemGetPtpPortOverridesResponsePayload",
        ),
        "GET_PTP_PORT_PDELAY_MON_COUNT": (
            "AemDescriptorRefPayload",
            "AemPtpPerfMonCountResponsePayload",
        ),
        "GET_PTP_PORT_PDELAY_MON_RECORD": (
            "AemPtpRecordCommandPayload",
            "AemPtpPortPdelayMonRecordResponsePayload",
        ),
        "GET_PTP_PORT_PERF_MON_COUNT": (
            "AemDescriptorRefPayload",
            "AemPtpPerfMonCountResponsePayload",
        ),
        "GET_PTP_PORT_PERF_MON_RECORD": (
            "AemPtpRecordCommandPayload",
            "AemPtpPortPerfMonRecordResponsePayload",
        ),
        "GET_PATH_LATENCY": (
            "AemDescriptorRefPayload",
            "AemPathLatencyResponsePayload",
        ),
        "AUTH_GET_NONCE": ("AemAuthNonceCommandPayload", "AemAuthNonceResponsePayload"),
        "AUTH_ADD_KEY_NONCE": (
            "AemAuthAddKeyNoncePayload+blob:key:key_length",
            "AemAuthAddKeyNonceResponsePayload",
        ),
    }
)

#: (struct, member) -> bit expansions ((sub-field, mask, kind, doc), ...) for the
#: flag words of the standard-derived structs
#: Masks are the tables' "Bit Value" column: IEEE 1722.1 numbers bit 15 of a
#: 16-bit word as 0x0001 (the last bit on the wire), bit 0 as 0x8000.
STD_FLAG_WORDS: dict[tuple[str, str], tuple[tuple[str, int, str, str], ...]] = {
    ("AemSetPtpInstanceInfoPayload", "set_flags"): (
        ("flags.ie", 0x0001, "bool", "IE: ie contains a value to be set"),
        ("flags.ee", 0x0002, "bool", "EE: ee contains a value to be set"),
        ("flags.so", 0x0004, "bool", "SO: so contains a value to be set"),
        ("flags.priority1", 0x0100, "bool", "PRIORITY1 is being set"),
        ("flags.priority2", 0x0200, "bool", "PRIORITY2 is being set"),
        ("flags.domain_number", 0x0400, "bool", "DOMAIN_NUMBER is being set"),
    ),
    ("AemSetPtpInstanceInfoPayload", "instance_booleans"): (
        ("so", 0x04, "bool", "defaultDS.slaveOnly"),
        ("ee", 0x02, "bool", "defaultDS.externalPortConfigurationEnabled"),
        ("ie", 0x01, "bool", "defaultDS.instanceEnabled"),
    ),
    ("AemPtpInstanceExtendedInfoResponsePayload", "valid_flags"): (
        ("valid.offset_from_master", 0x0001, "bool", "offset_from_master is valid"),
        ("valid.last_gm_phase_change", 0x0002, "bool", "last_gm_phase_change is valid"),
        ("valid.last_gm_freq_change", 0x0004, "bool", "last_gm_freq_change is valid"),
        ("valid.gm_change_count", 0x0008, "bool", "gm_change_count is valid"),
        (
            "valid.time_of_last_gm_change",
            0x0010,
            "bool",
            "time_of_last_gm_change is valid",
        ),
        (
            "valid.time_of_last_gm_phase_change",
            0x0020,
            "bool",
            "time_of_last_gm_phase_change is valid",
        ),
        (
            "valid.time_of_last_gm_freq_change",
            0x0040,
            "bool",
            "time_of_last_gm_freq_change is valid",
        ),
    ),
    ("AemPtpInstancePerfMonRecordResponsePayload", "record_flags"): (
        ("measurement_valid", 0x0001, "bool", "MEASUREMENT_VALID"),
        ("period_complete", 0x0002, "bool", "PERIOD_COMPLETE"),
        ("master_slave_delay_valid", 0x0004, "bool", "MASTER_SLAVE_DELAY_VALID"),
        ("slave_master_delay_valid", 0x0008, "bool", "SLAVE_MASTER_DELAY_VALID"),
        ("mean_path_delay_valid", 0x0010, "bool", "MEAN_PATH_DELAY_VALID"),
        ("offset_from_master_valid", 0x0020, "bool", "OFFSET_FROM_MASTER_VALID"),
    ),
    ("AemPtpPortPdelayMonRecordResponsePayload", "record_flags"): (
        ("measurement_valid", 0x0001, "bool", "MEASUREMENT_VALID"),
        ("period_complete", 0x0002, "bool", "PERIOD_COMPLETE"),
    ),
    ("AemPtpPortPerfMonRecordResponsePayload", "record_flags"): (
        ("measurement_valid", 0x0001, "bool", "MEASUREMENT_VALID"),
        ("period_complete", 0x0002, "bool", "PERIOD_COMPLETE"),
    ),
    ("AemPtpPortIntervalsPayload", "interval_flags"): (
        ("announce_valid", 0x0001, "bool", "ANNOUNCE_VALID"),
        ("sync_valid", 0x0002, "bool", "SYNC_VALID"),
        ("pdelay_valid", 0x0004, "bool", "PDELAY_VALID"),
        ("capable_valid", 0x0008, "bool", "CAPABLE_VALID"),
    ),
    ("AemSetPtpPortOverridesPayload", "override_flags"): (
        ("flags.announce_interval", 0x0001, "bool", "ANNOUNCE_INTERVAL"),
        ("flags.sync_interval", 0x0002, "bool", "SYNC_INTERVAL"),
        ("flags.pdelay_interval", 0x0004, "bool", "PDELAY_INTERVAL"),
        ("flags.gptp_capable_interval", 0x0008, "bool", "GPTP_CAPABLE_INTERVAL"),
        ("flags.compute_neighbor", 0x0010, "bool", "COMPUTE_NEIGHBOR"),
        ("flags.compute_mean_delay", 0x0020, "bool", "COMPUTE_MEAN_DELAY"),
        ("flags.onestep_tx_oper", 0x0040, "bool", "ONESTEP_TX_OPER"),
        ("flags.desired_state", 0x0080, "bool", "DESIRED_STATE"),
    ),
    ("AemSamplingRateRangePayload", "minimum_sampling_rate"): (
        ("minimum_pull", 0xE0000000, "u8", "minimum pull"),
        ("minimum_base_frequency", 0x1FFFFFFF, "u32", "minimum base_frequency (Hz)"),
    ),
    ("AemSamplingRateRangePayload", "maximum_sampling_rate"): (
        ("maximum_pull", 0xE0000000, "u8", "maximum pull"),
        ("maximum_base_frequency", 0x1FFFFFFF, "u32", "maximum base_frequency (Hz)"),
    ),
}
_BOOLEANS = (
    ("use_announce_interval", 0x0001, "bool", "USE_ANNOUNCE_INTERVAL"),
    ("use_sync_interval", 0x0002, "bool", "USE_SYNC_INTERVAL"),
    ("use_pdelay_interval", 0x0004, "bool", "USE_PDELAY_INTERVAL"),
    ("use_gptp_capable_interval", 0x0008, "bool", "USE_GPTP_CAPABLE_INTERVAL"),
    ("use_compute_neighbor", 0x0010, "bool", "USE_COMPUTE_NEIGHBOR"),
    ("use_compute_mean_delay", 0x0020, "bool", "USE_COMPUTE_MEAN_DELAY"),
    ("use_onestep_tx_oper", 0x0040, "bool", "USE_ONESTEP_TX_OPER"),
    ("compute_neighbor_rate", 0x0100, "bool", "COMPUTE_NEIGHBOR_RATE"),
    ("compute_mean_link_delay", 0x0200, "bool", "COMPUTE_MEAN_LINK_DELAY"),
    ("onestep_tx_oper", 0x0400, "bool", "ONESTEP_TX_OPER"),
)
STD_FLAG_WORDS[("AemSetPtpPortOverridesPayload", "override_booleans")] = _BOOLEANS
#: SET_PTP_PORT_INFO / GET_PTP_PORT_INFO (Tables 7-173 / 7-174, Figures 7-122 /
#: 7-124 as replaced by Cor 1-2025)
STD_FLAG_WORDS[("AemPtpPortInfoCommandPayload", "flags")] = (
    ("flags.set_enable", 0x0001, "bool", "SET_ENABLE"),
    ("flags.set_link_delay_threshold", 0x0002, "bool", "SET_LINK_DELAY_THRESHOLD"),
    ("flags.set_delay_mechanism", 0x0004, "bool", "SET_DELAY_MECHANISM"),
    ("flags.set_delay_asymmetry", 0x0008, "bool", "SET_DELAY_ASYMMETRY"),
    ("flags.set_announce_timeouts", 0x0010, "bool", "SET_ANNOUNCE_TIMEOUTS"),
    ("flags.set_sync_timeouts", 0x0020, "bool", "SET_SYNC_TIMEOUTS"),
    ("flags.set_gptp_capable_timeouts", 0x0040, "bool", "SET_GPTP_CAPABLE_TIMEOUTS"),
    ("flags.set_pdelay_timeouts", 0x0080, "bool", "SET_PDELAY_TIMEOUTS"),
    ("flags.set_ioto", 0x0100, "bool", "SET_IOTO"),
    ("flags.set_icmd", 0x0200, "bool", "SET_ICMD"),
    ("flags.set_icnr", 0x0400, "bool", "SET_ICNR"),
    ("flags.set_faults", 0x0800, "bool", "SET_FAULTS"),
)
STD_FLAG_WORDS[("AemPtpPortInfoCommandPayload", "port_flags")] = (
    ("port_flags.ioto", 0x08, "bool", "IOTO"),
    ("port_flags.icmd", 0x04, "bool", "ICMD"),
    ("port_flags.icnr", 0x02, "bool", "ICNR"),
    ("port_flags.pe", 0x01, "bool", "PE: portDS.ptpPortEnabled"),
)
STD_FLAG_WORDS[("AemGetPtpPortInfoResponsePayload", "flags")] = (
    ("flags.delay_mechanism", 0x0001, "bool", "DELAY_MECHANISM"),
    ("flags.delay_asymmetry", 0x0002, "bool", "DELAY_ASYMMETRY"),
    ("flags.gptp_capable_timeouts", 0x0004, "bool", "GPTP_CAPABLE_TIMEOUTS"),
    ("flags.pdelay_timeouts", 0x0008, "bool", "PDELAY_TIMEOUTS"),
    ("flags.ioto", 0x0010, "bool", "IOTO"),
    ("flags.faults", 0x0020, "bool", "FAULTS"),
    ("flags.osto", 0x0040, "bool", "OSTO"),
    ("flags.osr", 0x0080, "bool", "OSR"),
    ("flags.ost", 0x0100, "bool", "OST"),
    ("flags.coto", 0x0200, "bool", "COTO"),
    ("flags.sl", 0x0400, "bool", "SL"),
)
STD_FLAG_WORDS[("AemGetPtpPortInfoResponsePayload", "port_flags")] = (
    ("port_flags.cc", 0x80, "bool", "CC"),
    ("port_flags.ccnr", 0x40, "bool", "CCNR"),
    ("port_flags.asc", 0x20, "bool", "ASC"),
    ("port_flags.imd", 0x10, "bool", "IMD"),
    ("port_flags.ioto", 0x08, "bool", "IOTO"),
    ("port_flags.icmd", 0x04, "bool", "ICMD"),
    ("port_flags.icnr", 0x02, "bool", "ICNR"),
    ("port_flags.pe", 0x01, "bool", "PE: portDS.ptpPortEnabled"),
)
STD_FLAG_WORDS[("AemGetPtpPortInfoResponsePayload", "ext_port_flags")] = (
    ("ext_port_flags.osto", 0x10, "bool", "OSTO"),
    ("ext_port_flags.osr", 0x08, "bool", "OSR"),
    ("ext_port_flags.ost", 0x04, "bool", "OST"),
    ("ext_port_flags.coto", 0x02, "bool", "COTO"),
    ("ext_port_flags.sl", 0x01, "bool", "SL"),
)
STD_FLAG_WORDS[("AemGetPtpPortOverridesResponsePayload", "override_booleans")] = (
    _BOOLEANS
)
_INSTANCE_FLAGS = (
    ("cv", 0x0100, "bool", "defaultDS.currentUTCOffsetValid"),
    ("l59", 0x0080, "bool", "defaultDS.leap59"),
    ("l61", 0x0040, "bool", "defaultDS.leap61"),
    ("tt", 0x0020, "bool", "defaultDS.timeTraceable"),
    ("ft", 0x0010, "bool", "defaultDS.frequencyTraceable"),
    ("pt", 0x0008, "bool", "defaultDS.ptpTimescale"),
    ("so", 0x0004, "bool", "defaultDS.slaveOnly"),
    ("ee", 0x0002, "bool", "defaultDS.externalPortConfigurationEnabled"),
    ("ie", 0x0001, "bool", "defaultDS.instanceEnabled"),
)
_GM_FLAGS = (
    ("gm_cv", 0x20, "bool", "timePropertiesDS.currentUTCOffsetValid"),
    ("gm_l59", 0x10, "bool", "timePropertiesDS.leap59"),
    ("gm_l61", 0x08, "bool", "timePropertiesDS.leap61"),
    ("gm_tt", 0x04, "bool", "timePropertiesDS.timeTraceable"),
    ("gm_ft", 0x02, "bool", "timePropertiesDS.frequencyTraceable"),
    ("gm_pt", 0x01, "bool", "timePropertiesDS.ptpTimescale"),
)
for _name in (
    "AemPtpInstanceInfoResponsePayload",
    "AemPtpInstanceExtendedInfoResponsePayload",
):
    STD_FLAG_WORDS[(_name, "instance_flags")] = _INSTANCE_FLAGS
for _name in (
    "AemPtpInstanceInfoResponsePayload",
    "AemPtpInstanceExtendedInfoResponsePayload",
    "AemPtpInstanceGrandmasterInfoResponsePayload",
):
    STD_FLAG_WORDS[(_name, "gm_flags")] = _GM_FLAGS

KEYCHAIN_IDS: dict[int, str] = {
    0: "ENTITY_PUBLIC",
    1: "ENTITY_PRIVATE",
    2: "MANUFACTURER_PUBLIC",
    3: "CONTROLLERS",
}

#: GET_DYNAMIC_INFO (7.4.76, Figure 7-94): entries of an 8-octet header then
#: info_command_specific_data_length octets of the AEM payload of
#: info_command_type (fixed-size GET commands only)
DYNAMIC_INFO_HEADER_LENGTH = 8

#: HDCP APM AECP message (9.7.2, Figure 9-15): hdcp_apm_length at 22, flags /
#: reserved / fragment_offset at 24, message data from 28
HDCP_APM_HEADER_LENGTH = 28
