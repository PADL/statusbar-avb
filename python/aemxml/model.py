# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Data model for AEM descriptors (Phase 1+2: audio + signal processing + 2021 types)."""

from __future__ import annotations

from dataclasses import dataclass, field

# Descriptor type codes (IEEE 1722.1-2021 Clause 7.2)
DESCRIPTOR_ENTITY = 0x0000
DESCRIPTOR_CONFIGURATION = 0x0001
DESCRIPTOR_AUDIO_UNIT = 0x0002
DESCRIPTOR_VIDEO_UNIT = 0x0003
DESCRIPTOR_SENSOR_UNIT = 0x0004
DESCRIPTOR_STREAM_INPUT = 0x0005
DESCRIPTOR_STREAM_OUTPUT = 0x0006
DESCRIPTOR_JACK_INPUT = 0x0007
DESCRIPTOR_JACK_OUTPUT = 0x0008
DESCRIPTOR_AVB_INTERFACE = 0x0009
DESCRIPTOR_CLOCK_SOURCE = 0x000A
DESCRIPTOR_MEMORY_OBJECT = 0x000B
DESCRIPTOR_LOCALE = 0x000C
DESCRIPTOR_STRINGS = 0x000D
DESCRIPTOR_STREAM_PORT_INPUT = 0x000E
DESCRIPTOR_STREAM_PORT_OUTPUT = 0x000F
DESCRIPTOR_EXTERNAL_PORT_INPUT = 0x0010
DESCRIPTOR_EXTERNAL_PORT_OUTPUT = 0x0011
DESCRIPTOR_INTERNAL_PORT_INPUT = 0x0012
DESCRIPTOR_INTERNAL_PORT_OUTPUT = 0x0013
DESCRIPTOR_AUDIO_CLUSTER = 0x0014
DESCRIPTOR_VIDEO_CLUSTER = 0x0015
DESCRIPTOR_SENSOR_CLUSTER = 0x0016
DESCRIPTOR_AUDIO_MAP = 0x0017
DESCRIPTOR_VIDEO_MAP = 0x0018
DESCRIPTOR_SENSOR_MAP = 0x0019
DESCRIPTOR_CONTROL = 0x001A
DESCRIPTOR_SIGNAL_SELECTOR = 0x001B
DESCRIPTOR_MIXER = 0x001C
DESCRIPTOR_MATRIX = 0x001D
DESCRIPTOR_MATRIX_SIGNAL = 0x001E
DESCRIPTOR_SIGNAL_SPLITTER = 0x001F
DESCRIPTOR_SIGNAL_COMBINER = 0x0020
DESCRIPTOR_SIGNAL_DEMULTIPLEXER = 0x0021
DESCRIPTOR_SIGNAL_MULTIPLEXER = 0x0022
DESCRIPTOR_SIGNAL_TRANSCODER = 0x0023
DESCRIPTOR_CLOCK_DOMAIN = 0x0024
DESCRIPTOR_CONTROL_BLOCK = 0x0025
DESCRIPTOR_TIMING = 0x0040
DESCRIPTOR_PTP_INSTANCE = 0x0041
DESCRIPTOR_PTP_PORT = 0x0042
DESCRIPTOR_INVALID = 0xFFFF

# --- Enum and flag name mappings (name -> int value) ---

DESCRIPTOR_TYPE_NAMES: dict[str, int] = {
    "ENTITY": 0x0000,
    "CONFIGURATION": 0x0001,
    "AUDIO_UNIT": 0x0002,
    "VIDEO_UNIT": 0x0003,
    "SENSOR_UNIT": 0x0004,
    "STREAM_INPUT": 0x0005,
    "STREAM_OUTPUT": 0x0006,
    "JACK_INPUT": 0x0007,
    "JACK_OUTPUT": 0x0008,
    "AVB_INTERFACE": 0x0009,
    "CLOCK_SOURCE": 0x000A,
    "MEMORY_OBJECT": 0x000B,
    "LOCALE": 0x000C,
    "STRINGS": 0x000D,
    "STREAM_PORT_INPUT": 0x000E,
    "STREAM_PORT_OUTPUT": 0x000F,
    "EXTERNAL_PORT_INPUT": 0x0010,
    "EXTERNAL_PORT_OUTPUT": 0x0011,
    "INTERNAL_PORT_INPUT": 0x0012,
    "INTERNAL_PORT_OUTPUT": 0x0013,
    "AUDIO_CLUSTER": 0x0014,
    "VIDEO_CLUSTER": 0x0015,
    "SENSOR_CLUSTER": 0x0016,
    "AUDIO_MAP": 0x0017,
    "VIDEO_MAP": 0x0018,
    "SENSOR_MAP": 0x0019,
    "CONTROL": 0x001A,
    "SIGNAL_SELECTOR": 0x001B,
    "MIXER": 0x001C,
    "MATRIX": 0x001D,
    "MATRIX_SIGNAL": 0x001E,
    "SIGNAL_SPLITTER": 0x001F,
    "SIGNAL_COMBINER": 0x0020,
    "SIGNAL_DEMULTIPLEXER": 0x0021,
    "SIGNAL_MULTIPLEXER": 0x0022,
    "SIGNAL_TRANSCODER": 0x0023,
    "CLOCK_DOMAIN": 0x0024,
    "CONTROL_BLOCK": 0x0025,
    "TIMING": 0x0040,
    "PTP_INSTANCE": 0x0041,
    "PTP_PORT": 0x0042,
    "INVALID": 0xFFFF,
}
DESCRIPTOR_TYPE_VALUES: dict[int, str] = {
    v: k for k, v in DESCRIPTOR_TYPE_NAMES.items()
}

JACK_TYPE_NAMES: dict[str, int] = {
    "SPEAKER": 0x0000,
    "HEADPHONE": 0x0001,
    "ANALOG_MICROPHONE": 0x0002,
    "SPDIF": 0x0003,
    "ADAT": 0x0004,
    "TDIF": 0x0005,
    "MADI": 0x0006,
    "UNBALANCED_ANALOG": 0x0007,
    "BALANCED_ANALOG": 0x0008,
    "DIGITAL": 0x0009,
    "MIDI": 0x000A,
    "AES_EBU": 0x000B,
    "COMPOSITE_VIDEO": 0x000C,
    "S_VHS_VIDEO": 0x000D,
    "COMPONENT_VIDEO": 0x000E,
    "DVI": 0x000F,
    "HDMI": 0x0010,
    "UDI": 0x0011,
    "DISPLAYPORT": 0x0012,
    "ANTENNA": 0x0013,
    "ANALOG_TUNER": 0x0014,
    "ETHERNET": 0x0015,
    "WIFI": 0x0016,
    "USB": 0x0017,
    "PCI": 0x0018,
    "PCI_E": 0x0019,
    "SCSI": 0x001A,
    "ATA": 0x001B,
    "IMAGER": 0x001C,
    "IR": 0x001D,
    "THUNDERBOLT": 0x001E,
    "SATA": 0x001F,
    "SMPTE_LTC": 0x0020,
    "DIGITAL_MICROPHONE": 0x0021,
    "AUDIO_MEDIA_CLOCK": 0x0022,
    "VIDEO_MEDIA_CLOCK": 0x0023,
    "GNSS_CLOCK": 0x0024,
    "PPS": 0x0025,
}
JACK_TYPE_VALUES: dict[int, str] = {v: k for k, v in JACK_TYPE_NAMES.items()}

CLOCK_SOURCE_TYPE_NAMES: dict[str, int] = {
    "INTERNAL": 0x0000,
    "EXTERNAL": 0x0001,
    "INPUT_STREAM": 0x0002,
    "MEDIA_CLOCK_STREAM": 0x0003,
}
CLOCK_SOURCE_TYPE_VALUES: dict[int, str] = {
    v: k for k, v in CLOCK_SOURCE_TYPE_NAMES.items()
}

AUDIO_CLUSTER_FORMAT_NAMES: dict[str, int] = {
    "IEC_60958": 0x00,
    "MBLA": 0x40,
    "MIDI": 0x80,
    "SMPTE": 0xC0,
}
AUDIO_CLUSTER_FORMAT_VALUES: dict[int, str] = {
    v: k for k, v in AUDIO_CLUSTER_FORMAT_NAMES.items()
}

MEMORY_OBJECT_TYPE_NAMES: dict[str, int] = {
    "FIRMWARE_IMAGE": 0x0000,
    "VENDOR_SPECIFIC": 0x0001,
    "CRASH_DUMP": 0x0002,
    "LOG_OBJECT": 0x0003,
    "AUTOSTART_SETTINGS": 0x0004,
    "SNAPSHOT_SETTINGS": 0x0005,
}
MEMORY_OBJECT_TYPE_VALUES: dict[int, str] = {
    v: k for k, v in MEMORY_OBJECT_TYPE_NAMES.items()
}

TIMING_ALGORITHM_NAMES: dict[str, int] = {
    "SINGLE": 0x0000,
    "FALLBACK": 0x0001,
    "COMBINED": 0x0002,
}
TIMING_ALGORITHM_VALUES: dict[int, str] = {
    v: k for k, v in TIMING_ALGORITHM_NAMES.items()
}

PTP_PORT_TYPE_NAMES: dict[str, int] = {
    "P2P_LINK_LAYER": 0x0000,
    "P2P_MULTICAST_UDPV4": 0x0001,
    "P2P_MULTICAST_UDPV6": 0x0002,
    "TIMING_MEASUREMENT": 0x0003,
    "FINE_TIMING_MEASUREMENT": 0x0004,
    "E2E_LINK_LAYER": 0x0005,
    "E2E_MULTICAST_UDPV4": 0x0006,
    "E2E_MULTICAST_UDPV6": 0x0007,
    "P2P_UNICAST_UDPV4": 0x0008,
    "P2P_UNICAST_UDPV6": 0x0009,
    "E2E_UNICAST_UDPV4": 0x000A,
    "E2E_UNICAST_UDPV6": 0x000B,
}
PTP_PORT_TYPE_VALUES: dict[int, str] = {v: k for k, v in PTP_PORT_TYPE_NAMES.items()}

# Bitmask flag mappings (name -> bit value)

# Bitmask values match C++ constants in atdecc_adp.hpp and atdecc_aem_descriptor.hpp

ENTITY_CAPABILITIES_NAMES: dict[str, int] = {
    "EFU_MODE": 0x00000001,
    "ADDRESS_ACCESS_SUPPORTED": 0x00000002,
    "GATEWAY_ENTITY": 0x00000004,
    "AEM_SUPPORTED": 0x00000008,
    "LEGACY_AVC": 0x00000010,
    "ASSOCIATION_ID_SUPPORTED": 0x00000020,
    "ASSOCIATION_ID_VALID": 0x00000040,
    "VENDOR_UNIQUE_SUPPORTED": 0x00000080,
    "CLASS_A_SUPPORTED": 0x00000100,
    "CLASS_B_SUPPORTED": 0x00000200,
    "GPTP_SUPPORTED": 0x00000400,
    "AEM_AUTHENTICATION_SUPPORTED": 0x00000800,
    "AEM_AUTHENTICATION_REQUIRED": 0x00001000,
    "AEM_PERSISTENT_ACQUIRE_SUPPORTED": 0x00002000,
    "AEM_IDENTIFY_CONTROL_INDEX_VALID": 0x00004000,
    "AEM_INTERFACE_INDEX_VALID": 0x00008000,
    "GENERAL_CONTROLLER_IGNORE": 0x00010000,
    "ENTITY_NOT_READY": 0x00020000,
}

TALKER_CAPABILITIES_NAMES: dict[str, int] = {
    "IMPLEMENTED": 0x0001,
    "OTHER_SOURCE": 0x0200,
    "CONTROL_SOURCE": 0x0400,
    "MEDIA_CLOCK_SOURCE": 0x0800,
    "SMPTE_SOURCE": 0x1000,
    "MIDI_SOURCE": 0x2000,
    "AUDIO_SOURCE": 0x4000,
    "VIDEO_SOURCE": 0x8000,
}

LISTENER_CAPABILITIES_NAMES: dict[str, int] = {
    "IMPLEMENTED": 0x0001,
    "OTHER_SINK": 0x0200,
    "CONTROL_SINK": 0x0400,
    "MEDIA_CLOCK_SINK": 0x0800,
    "SMPTE_SINK": 0x1000,
    "MIDI_SINK": 0x2000,
    "AUDIO_SINK": 0x4000,
    "VIDEO_SINK": 0x8000,
}

CONTROLLER_CAPABILITIES_NAMES: dict[str, int] = {
    "IMPLEMENTED": 0x00000001,
}

STREAM_FLAGS_NAMES: dict[str, int] = {
    "CLOCK_SYNC_SOURCE": 0x0001,
    "CLASS_A": 0x0002,
    "CLASS_B": 0x0004,
    "SUPPORTS_ENCRYPTED": 0x0008,
    "PRIMARY_BACKUP_SUPPORTED": 0x0010,
    "PRIMARY_BACKUP_VALID": 0x0020,
    "SECONDARY_BACKUP_SUPPORTED": 0x0040,
    "SECONDARY_BACKUP_VALID": 0x0080,
    "TERTIARY_BACKUP_SUPPORTED": 0x0100,
    "TERTIARY_BACKUP_VALID": 0x0200,
    "SUPPORTS_AVTP_UDPV4": 0x0400,
    "SUPPORTS_AVTP_UDPV6": 0x0800,
    "NO_SUPPORT_AVTP_NATIVE": 0x1000,
    "TIMING_FIELD_VALID": 0x2000,
    "NO_MEDIA_CLOCK": 0x4000,
    "SUPPORTS_NO_SRP": 0x8000,
}

JACK_FLAGS_NAMES: dict[str, int] = {
    "CLOCK_SYNC_SOURCE": 0x0001,
    "CAPTIVE": 0x0002,
}

INTERFACE_FLAGS_NAMES: dict[str, int] = {
    "GPTP_GRANDMASTER_SUPPORTED": 0x0001,
    "GPTP_SUPPORTED": 0x0002,
    "SRP_SUPPORTED": 0x0004,
    "FQTSS_NOT_SUPPORTED": 0x0008,
    "SCHEDULED_TRAFFIC_SUPPORTED": 0x0010,
    "CAN_LISTEN_TO_SELF": 0x0020,
    "CAN_LISTEN_TO_OTHER_SELF": 0x0040,
}

PTP_INSTANCE_FLAGS_NAMES: dict[str, int] = {
    "CAN_SET_INSTANCE_ENABLE": 0x00000001,
    "CAN_SET_PRIORITY1": 0x00000002,
    "CAN_SET_PRIORITY2": 0x00000004,
    "CAN_SET_DOMAIN_NUMBER": 0x00000008,
    "CAN_SET_EXTERNAL_PORT_CONFIGURATION": 0x00000010,
    "CAN_SET_SLAVE_ONLY": 0x00000020,
    "CAN_ENABLE_PERFORMANCE": 0x00000040,
    "PERFORMANCE_MONITORING": 0x40000000,
    "GRANDMASTER_CAPABLE": 0x80000000,
}


@dataclass
class LocalizedStringRef:
    """Reference to a localized string: offset into STRINGS descriptors + index within."""

    offset: int = 0
    index: int = 0


@dataclass
class AudioMapping:
    """One entry in an AUDIO_MAP descriptor."""

    stream_index: int = 0
    stream_channel: int = 0
    cluster_offset: int = 0
    cluster_channel: int = 0


@dataclass
class AudioCluster:
    """AUDIO_CLUSTER descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    signal_type: int = 0
    signal_index: int = 0
    signal_output: int = 0
    path_latency: int = 0
    block_latency: int = 0
    channel_count: int = 0
    format: int = 0x40  # MBLA
    aes3_data_type_reference: int = 0
    aes3_data_type: int = 0
    symbol: str | None = None


@dataclass
class AudioMap:
    """AUDIO_MAP descriptor data."""

    mappings: list[AudioMapping] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class Control:
    """CONTROL descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    block_latency: int = 0
    control_latency: int = 0
    control_domain: int = 0
    control_value_type: int = 0
    control_type: int = 0  # EUI64
    reset_time: int = 0
    signal_type: int = 0
    signal_index: int = 0
    signal_output: int = 0
    value_details: bytes = b""
    symbol: str | None = None


@dataclass
class SignalSource:
    """A signal source reference (6 bytes on wire)."""

    signal_type: int = 0
    signal_index: int = 0
    signal_output: int = 0


@dataclass
class SignalSelector:
    """SIGNAL_SELECTOR descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    block_latency: int = 0
    control_latency: int = 0
    control_domain: int = 0
    current_signal_type: int = 0
    current_signal_index: int = 0
    current_signal_output: int = 0
    default_signal_type: int = 0
    default_signal_index: int = 0
    default_signal_output: int = 0
    sources: list[SignalSource] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class Mixer:
    """MIXER descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    block_latency: int = 0
    control_latency: int = 0
    control_domain: int = 0
    control_value_type: int = 0
    sources: list[SignalSource] = field(default_factory=list)
    value_details: bytes = b""
    symbol: str | None = None


@dataclass
class Matrix:
    """MATRIX descriptor data.

    When `matrix_signals` is non-empty (JSON authoring), flatten emits those
    MATRIX_SIGNAL descriptors contiguously and fills number_of_sources /
    base_source automatically; the two count fields are only honored as-is
    when `matrix_signals` is empty (legacy XML authoring with hand-managed
    indices).
    """

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    block_latency: int = 0
    control_latency: int = 0
    control_domain: int = 0
    control_value_type: int = 0
    control_type: int = 0  # EUI64
    width: int = 0
    height: int = 0
    number_of_values: int = 0
    number_of_sources: int = 0
    base_source: int = 0
    value_details: bytes = b""
    matrix_signals: list[MatrixSignal] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class MatrixSignal:
    """MATRIX_SIGNAL descriptor data (part of Matrix sources)."""

    descriptor_type: int = 0
    descriptor_index: int = 0
    signals: list[SignalSource] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class SplitterMapEntry:
    """One entry in a SignalSplitter map (6 bytes on wire)."""

    sub_signal_start: int = 0
    sub_signal_count: int = 0
    output_index: int = 0


@dataclass
class SignalSplitter:
    """SIGNAL_SPLITTER descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    block_latency: int = 0
    control_latency: int = 0
    control_domain: int = 0
    signal_type: int = 0
    signal_index: int = 0
    signal_output: int = 0
    number_of_outputs: int = 0
    splitter_map: list[SplitterMapEntry] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class CombinerMapEntry:
    """One entry in a SignalCombiner map (6 bytes on wire)."""

    sub_signal_start: int = 0
    sub_signal_count: int = 0
    input_index: int = 0


@dataclass
class SignalCombiner:
    """SIGNAL_COMBINER descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    block_latency: int = 0
    control_latency: int = 0
    control_domain: int = 0
    combiner_map: list[CombinerMapEntry] = field(default_factory=list)
    sources: list[SignalSource] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class SignalDemultiplexer:
    """SIGNAL_DEMULTIPLEXER descriptor data (same structure as SignalSplitter)."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    block_latency: int = 0
    control_latency: int = 0
    control_domain: int = 0
    signal_type: int = 0
    signal_index: int = 0
    signal_output: int = 0
    number_of_outputs: int = 0
    demultiplexer_map: list[SplitterMapEntry] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class SignalMultiplexer:
    """SIGNAL_MULTIPLEXER descriptor data (same structure as SignalCombiner)."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    block_latency: int = 0
    control_latency: int = 0
    control_domain: int = 0
    multiplexer_map: list[CombinerMapEntry] = field(default_factory=list)
    sources: list[SignalSource] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class SignalTranscoder:
    """SIGNAL_TRANSCODER descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    block_latency: int = 0
    control_latency: int = 0
    control_domain: int = 0
    control_value_type: int = 0
    signal_type: int = 0
    signal_index: int = 0
    signal_output: int = 0
    transcoder_type: int = 0  # EUI64
    value_details: bytes = b""
    symbol: str | None = None


@dataclass
class ControlBlock:
    """CONTROL_BLOCK descriptor data.

    When `controls` is non-empty (JSON authoring), flatten emits those
    CONTROL descriptors contiguously and fills number_of_controls /
    base_control / final_control_index automatically; the three count
    fields are only honored as-is when `controls` is empty (legacy XML
    authoring with hand-managed indices).
    """

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    number_of_controls: int = 0
    base_control: int = 0
    final_control_index: int = 0
    signal_type: int = 0
    signal_index: int = 0
    signal_output: int = 0
    controls: list[Control] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class ExternalPort:
    """EXTERNAL_PORT_INPUT/OUTPUT descriptor data (24 bytes wire)."""

    clock_domain_index: int = 0
    port_flags: int = 0
    signal_type: int = 0
    signal_index: int = 0
    signal_output: int = 0
    block_latency: int = 0
    jack_index: int = 0
    controls: list[Control] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class InternalPort:
    """INTERNAL_PORT_INPUT/OUTPUT descriptor data (24 bytes wire)."""

    clock_domain_index: int = 0
    port_flags: int = 0
    signal_type: int = 0
    signal_index: int = 0
    signal_output: int = 0
    block_latency: int = 0
    internal_index: int = 0
    controls: list[Control] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class MemoryObject:
    """MEMORY_OBJECT descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    memory_object_type: int = 0
    target_descriptor_type: int = 0
    target_descriptor_index: int = 0
    start_address: int = 0
    maximum_length: int = 0
    length: int = 0
    maximum_segment_length: int = 0
    symbol: str | None = None


@dataclass
class VideoCluster:
    """VIDEO_CLUSTER descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    signal_type: int = 0
    signal_index: int = 0
    signal_output: int = 0
    path_latency: int = 0
    block_latency: int = 0
    format: int = 0
    current_format_specific: bytes = b""
    supported_format_specifics: list[bytes] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class SensorCluster:
    """SENSOR_CLUSTER descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    signal_type: int = 0
    signal_index: int = 0
    signal_output: int = 0
    path_latency: int = 0
    block_latency: int = 0
    format: int = 0
    current_format_specific: bytes = b""
    supported_format_specifics: list[bytes] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class VideoMapping:
    """One entry in a VIDEO_MAP descriptor."""

    stream_index: int = 0
    stream_channel: int = 0
    cluster_offset: int = 0
    cluster_channel: int = 0


@dataclass
class VideoMap:
    """VIDEO_MAP descriptor data."""

    mappings: list[VideoMapping] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class SensorMapping:
    """One entry in a SENSOR_MAP descriptor."""

    stream_index: int = 0
    stream_channel: int = 0
    cluster_offset: int = 0
    cluster_channel: int = 0


@dataclass
class SensorMap:
    """SENSOR_MAP descriptor data."""

    mappings: list[SensorMapping] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class Timing:
    """TIMING descriptor data (IEEE 1722.1-2021)."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    algorithm: int = 0
    ptp_instance_indices: list[int] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class PtpPort:
    """PTP_PORT descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    port_number: int = 0
    port_type: int = 0
    flags: int = 0
    avb_interface_index: int = 0
    profile_identifier: int = 0  # 6-byte EUI48 as int
    symbol: str | None = None


@dataclass
class PtpInstance:
    """PTP_INSTANCE descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    clock_identity: int = 0
    flags: int = 0
    number_of_controls: int = 0
    base_control: int = 0
    ptp_ports: list[PtpPort] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class VideoStreamPort:
    """Video stream port (same structure as AudioStreamPort but with video clusters/maps)."""

    clock_domain_index: int = 0
    port_flags: int = 0
    controls: list[Control] = field(default_factory=list)
    clusters: list[VideoCluster] = field(default_factory=list)
    maps: list[VideoMap] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class SensorStreamPort:
    """Sensor stream port (same structure as AudioStreamPort but with sensor clusters/maps)."""

    clock_domain_index: int = 0
    port_flags: int = 0
    controls: list[Control] = field(default_factory=list)
    clusters: list[SensorCluster] = field(default_factory=list)
    maps: list[SensorMap] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class AudioStreamPort:
    """STREAM_PORT_INPUT/OUTPUT descriptor data (audio)."""

    clock_domain_index: int = 0
    port_flags: int = 0
    controls: list[Control] = field(default_factory=list)
    clusters: list[AudioCluster] = field(default_factory=list)
    maps: list[AudioMap] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class AudioUnit:
    """AUDIO_UNIT descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    clock_domain_index: int = 0
    input_stream_ports: list[AudioStreamPort] = field(default_factory=list)
    output_stream_ports: list[AudioStreamPort] = field(default_factory=list)
    input_external_ports: list[ExternalPort] = field(default_factory=list)
    output_external_ports: list[ExternalPort] = field(default_factory=list)
    input_internal_ports: list[InternalPort] = field(default_factory=list)
    output_internal_ports: list[InternalPort] = field(default_factory=list)
    controls: list[Control] = field(default_factory=list)
    current_sampling_rate: int = 0  # pull << 29 | base_freq
    sampling_rates: list[int] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class VideoUnit:
    """VIDEO_UNIT descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    clock_domain_index: int = 0
    input_stream_ports: list[VideoStreamPort] = field(default_factory=list)
    output_stream_ports: list[VideoStreamPort] = field(default_factory=list)
    input_external_ports: list[ExternalPort] = field(default_factory=list)
    output_external_ports: list[ExternalPort] = field(default_factory=list)
    input_internal_ports: list[InternalPort] = field(default_factory=list)
    output_internal_ports: list[InternalPort] = field(default_factory=list)
    controls: list[Control] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class SensorUnit:
    """SENSOR_UNIT descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    clock_domain_index: int = 0
    input_stream_ports: list[SensorStreamPort] = field(default_factory=list)
    output_stream_ports: list[SensorStreamPort] = field(default_factory=list)
    input_external_ports: list[ExternalPort] = field(default_factory=list)
    output_external_ports: list[ExternalPort] = field(default_factory=list)
    input_internal_ports: list[InternalPort] = field(default_factory=list)
    output_internal_ports: list[InternalPort] = field(default_factory=list)
    controls: list[Control] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class Stream:
    """STREAM_INPUT/OUTPUT descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    clock_domain_index: int = 0
    stream_flags: int = 0
    current_format: int = 0  # 8-byte stream format as int
    backup_talker_entity_id_0: int = 0
    backup_talker_unique_id_0: int = 0
    backup_talker_entity_id_1: int = 0
    backup_talker_unique_id_1: int = 0
    backup_talker_entity_id_2: int = 0
    backup_talker_unique_id_2: int = 0
    backedup_talker_entity_id: int = 0
    backedup_talker_unique_id: int = 0
    avb_interface_index: int = 0
    buffer_length: int = 0
    redundant_offset: int = 0
    number_of_redundant_streams: int = 0
    timing: int = 0
    formats: list[int] = field(default_factory=list)
    redundant_streams: list[int] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class Jack:
    """JACK_INPUT/OUTPUT descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    jack_flags: int = 0
    jack_type: int = 0
    symbol: str | None = None


@dataclass
class AvbInterface:
    """AVB_INTERFACE descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    mac_address: int = 0  # 6-byte MAC as int
    interface_flags: int = 0
    clock_identity: int = 0
    priority1: int = 0xFF
    clock_class: int = 0xFF
    offset_scaled_log_variance: int = 0
    clock_accuracy: int = 0xFF
    priority2: int = 0xFF
    domain_number: int = 0
    log_sync_interval: int = 0
    log_announce_interval: int = 0
    log_pdelay_interval: int = 0
    port_number: int = 0
    number_of_controls: int = 0
    base_control: int = 0
    controls: list[Control] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class ClockSource:
    """CLOCK_SOURCE descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    clock_source_flags: int = 0
    clock_source_type: int = 0
    clock_source_identifier: int = 0  # EUI64
    clock_source_location_type: int = 0
    clock_source_location_index: int = 0
    symbol: str | None = None


@dataclass
class ClockDomain:
    """CLOCK_DOMAIN descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    clock_source_index: int = 0
    clock_sources: list[int] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class StringsDescriptor:
    """STRINGS descriptor data — up to 7 localized strings."""

    strings: list[str] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class Locale:
    """LOCALE descriptor data."""

    locale_identifier: str = ""
    strings_descriptors: list[StringsDescriptor] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class Configuration:
    """CONFIGURATION descriptor data."""

    object_name: str = ""
    localized_description: LocalizedStringRef = field(
        default_factory=LocalizedStringRef
    )
    audio_units: list[AudioUnit] = field(default_factory=list)
    video_units: list[VideoUnit] = field(default_factory=list)
    sensor_units: list[SensorUnit] = field(default_factory=list)
    streams_input: list[Stream] = field(default_factory=list)
    streams_output: list[Stream] = field(default_factory=list)
    jacks_input: list[Jack] = field(default_factory=list)
    jacks_output: list[Jack] = field(default_factory=list)
    avb_interfaces: list[AvbInterface] = field(default_factory=list)
    clock_sources: list[ClockSource] = field(default_factory=list)
    memory_objects: list[MemoryObject] = field(default_factory=list)
    controls: list[Control] = field(default_factory=list)
    signal_selectors: list[SignalSelector] = field(default_factory=list)
    mixers: list[Mixer] = field(default_factory=list)
    matrices: list[Matrix] = field(default_factory=list)
    splitters: list[SignalSplitter] = field(default_factory=list)
    combiners: list[SignalCombiner] = field(default_factory=list)
    demultiplexers: list[SignalDemultiplexer] = field(default_factory=list)
    multiplexers: list[SignalMultiplexer] = field(default_factory=list)
    transcoders: list[SignalTranscoder] = field(default_factory=list)
    control_blocks: list[ControlBlock] = field(default_factory=list)
    locales: list[Locale] = field(default_factory=list)
    clock_domains: list[ClockDomain] = field(default_factory=list)
    timings: list[Timing] = field(default_factory=list)
    ptp_instances: list[PtpInstance] = field(default_factory=list)
    symbol: str | None = None


@dataclass
class Entity:
    """Top-level ENTITY descriptor data."""

    entity_id: int = 0
    entity_model_id: int = 0
    entity_capabilities: int = 0
    talker_stream_sources: int = 0
    talker_capabilities: int = 0
    listener_stream_sinks: int = 0
    listener_capabilities: int = 0
    controller_capabilities: int = 0
    available_index: int = 0
    association_id: int = 0
    entity_name: str = ""
    vendor_name_string: LocalizedStringRef = field(default_factory=LocalizedStringRef)
    model_name_string: LocalizedStringRef = field(default_factory=LocalizedStringRef)
    firmware_version: str = ""
    group_name: str = ""
    serial_number: str = ""
    schema_year: int = 2021
    configurations: list[Configuration] = field(default_factory=list)
