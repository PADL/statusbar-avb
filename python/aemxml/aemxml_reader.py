# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Parse AEMXML files into the AEM data model."""

from __future__ import annotations

import xml.etree.ElementTree as ET

import os

from .model import (
    AudioCluster,
    AudioMap,
    AudioMapping,
    AudioStreamPort,
    AudioUnit,
    AvbInterface,
    ClockDomain,
    ClockSource,
    CombinerMapEntry,
    Configuration,
    Control,
    ControlBlock,
    Entity,
    ExternalPort,
    InternalPort,
    Jack,
    Locale,
    LocalizedStringRef,
    Matrix,
    MemoryObject,
    Mixer,
    PtpInstance,
    PtpPort,
    SensorCluster,
    SensorMap,
    SensorMapping,
    SensorStreamPort,
    SensorUnit,
    SignalCombiner,
    SignalDemultiplexer,
    SignalMultiplexer,
    SignalSelector,
    SignalSource,
    SignalSplitter,
    SignalTranscoder,
    SplitterMapEntry,
    Stream,
    StringsDescriptor,
    Timing,
    VideoCluster,
    VideoMap,
    VideoMapping,
    VideoStreamPort,
    VideoUnit,
    # Enum/flag name mappings
    DESCRIPTOR_TYPE_NAMES,
    JACK_TYPE_NAMES,
    CLOCK_SOURCE_TYPE_NAMES,
    AUDIO_CLUSTER_FORMAT_NAMES,
    MEMORY_OBJECT_TYPE_NAMES,
    TIMING_ALGORITHM_NAMES,
    PTP_PORT_TYPE_NAMES,
    ENTITY_CAPABILITIES_NAMES,
    TALKER_CAPABILITIES_NAMES,
    LISTENER_CAPABILITIES_NAMES,
    CONTROLLER_CAPABILITIES_NAMES,
    STREAM_FLAGS_NAMES,
    JACK_FLAGS_NAMES,
    INTERFACE_FLAGS_NAMES,
    PTP_INSTANCE_FLAGS_NAMES,
)

NS = "http://grouper.ieee.org/groups/1722/1/contributions/xml"


def _ns(tag: str) -> str:
    return f"{{{NS}}}{tag}"


def _local_name(el: ET.Element) -> str:
    """Return the local name of an element (strip namespace)."""
    tag = el.tag
    if tag.startswith("{"):
        return tag.split("}", 1)[1]
    return tag


def _require_text(el: ET.Element, tag: str) -> str:
    """Return text of a required child element, or raise with context."""
    child = el.find(_ns(tag))
    if child is None or child.text is None:
        raise ValueError(f"Missing required element '{tag}' in {_local_name(el)}")
    return child.text.strip()


def _text(el: ET.Element, tag: str, default: str = "") -> str:
    child = el.find(_ns(tag))
    if child is None or child.text is None:
        return default
    return child.text.strip()


def _safe_hex(value: str, tag: str, parent: ET.Element) -> int:
    """Parse a hex string, raising ValueError with context on failure."""
    try:
        return int(value, 16)
    except ValueError:
        raise ValueError(
            f"Invalid hex value '{value}' for '{tag}' in {_local_name(parent)}"
        )


def _hex16(el: ET.Element, tag: str) -> int:
    val = _text(el, tag, "0")
    return _safe_hex(val, tag, el)


def _hex32(el: ET.Element, tag: str) -> int:
    val = _text(el, tag, "0")
    return _safe_hex(val, tag, el)


def _hex64(el: ET.Element, tag: str) -> int:
    val = _text(el, tag, "0")
    return _safe_hex(val, tag, el)


def _symbol(el: ET.Element) -> str | None:
    return el.get("symbol")


def _parse_enum(text: str, name_map: dict[str, int]) -> int:
    """Parse a value that may be a symbolic name or hex string."""
    text = text.strip()
    if text in name_map:
        return name_map[text]
    return int(text, 16)


def _parse_flags(text: str, name_map: dict[str, int]) -> int:
    """Parse flags that may be space-separated names or a hex string."""
    text = text.strip()
    if not text:
        return 0
    # Try hex first (all hex chars, no spaces)
    if " " not in text:
        try:
            return int(text, 16)
        except ValueError:
            pass
    # Parse as space-separated flag names
    result = 0
    for name in text.split():
        if name not in name_map:
            raise ValueError(f"Unknown flag '{name}'")
        result |= name_map[name]
    return result


def _enum16(el: ET.Element, tag: str, name_map: dict[str, int]) -> int:
    val = _text(el, tag, "0")
    return _parse_enum(val, name_map)


def _enum8(el: ET.Element, tag: str, name_map: dict[str, int]) -> int:
    val = _text(el, tag, "0")
    return _parse_enum(val, name_map)


def _flags16(el: ET.Element, tag: str, name_map: dict[str, int]) -> int:
    val = _text(el, tag, "0")
    return _parse_flags(val, name_map)


def _flags32(el: ET.Element, tag: str, name_map: dict[str, int]) -> int:
    val = _text(el, tag, "0")
    return _parse_flags(val, name_map)


def _parse_localized_string_ref(el: ET.Element, tag: str) -> LocalizedStringRef:
    container = el.find(_ns(tag))
    if container is None:
        return LocalizedStringRef()
    ls = container.find(_ns("localized_string"))
    if ls is None:
        return LocalizedStringRef()
    return LocalizedStringRef(
        offset=_safe_hex(_text(ls, "offset", "0"), "offset", ls),
        index=_safe_hex(_text(ls, "index", "0"), "index", ls),
    )


def _parse_audio_cluster(el: ET.Element) -> AudioCluster:
    return AudioCluster(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        signal_type=_enum16(el, "signal_type", DESCRIPTOR_TYPE_NAMES),
        signal_index=_hex16(el, "signal_index"),
        signal_output=_hex16(el, "signal_output"),
        path_latency=_hex32(el, "path_latency"),
        block_latency=_hex32(el, "block_latency"),
        channel_count=_hex16(el, "channel_count"),
        format=_enum8(el, "format", AUDIO_CLUSTER_FORMAT_NAMES),
        aes3_data_type_reference=int(_text(el, "aes3_data_type_reference", "0"), 16),
        aes3_data_type=_hex16(el, "aes3_data_type"),
        symbol=_symbol(el),
    )


def _parse_audio_mapping(el: ET.Element) -> AudioMapping:
    return AudioMapping(
        stream_index=_hex16(el, "stream_index"),
        stream_channel=_hex16(el, "stream_channel"),
        cluster_offset=_hex16(el, "cluster_offset"),
        cluster_channel=_hex16(el, "cluster_channel"),
    )


def _parse_audio_map(el: ET.Element) -> AudioMap:
    mappings = [_parse_audio_mapping(m) for m in el.findall(_ns("audio_mapping"))]
    return AudioMap(mappings=mappings, symbol=_symbol(el))


def _parse_control(el: ET.Element) -> Control:
    return Control(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        block_latency=_hex32(el, "block_latency"),
        control_latency=_hex32(el, "control_latency"),
        control_domain=_hex16(el, "control_domain"),
        control_value_type=_hex16(el, "control_value_type"),
        control_type=_hex64(el, "control_type"),
        reset_time=_hex32(el, "reset_time"),
        signal_type=_enum16(el, "signal_type", DESCRIPTOR_TYPE_NAMES),
        signal_index=_hex16(el, "signal_index"),
        signal_output=_hex16(el, "signal_output"),
        symbol=_symbol(el),
    )


def _parse_controls(el: ET.Element, tag: str = "controls") -> list[Control]:
    container = el.find(_ns(tag))
    if container is None:
        return []
    return [_parse_control(c) for c in container.findall(_ns("control"))]


def _parse_audio_stream_port(el: ET.Element) -> AudioStreamPort:
    clusters_el = el.find(_ns("clusters"))
    clusters = [
        _parse_audio_cluster(c)
        for c in (clusters_el.findall(_ns("audio_cluster")) if clusters_el else [])
    ]
    maps_el = el.find(_ns("maps"))
    maps = [
        _parse_audio_map(m)
        for m in (maps_el.findall(_ns("audio_map")) if maps_el else [])
    ]
    return AudioStreamPort(
        clock_domain_index=_hex16(el, "clock_domain_index"),
        port_flags=_hex16(el, "port_flags"),
        controls=_parse_controls(el),
        clusters=clusters,
        maps=maps,
        symbol=_symbol(el),
    )


def _parse_sampling_rate(el: ET.Element) -> int:
    pull = int(_text(el, "pull", "0"), 16)
    base_freq = int(_text(el, "base_frequency", "0"), 16)
    return (pull << 29) | (base_freq & 0x1FFFFFFF)


def _parse_audio_unit(el: ET.Element) -> AudioUnit:
    isp = el.find(_ns("input_stream_ports"))
    osp = el.find(_ns("output_stream_ports"))
    rates_el = el.find(_ns("sampling_rates"))
    cur_rate_el = el.find(_ns("current_sampling_rate"))
    return AudioUnit(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        clock_domain_index=_hex16(el, "clock_domain_index"),
        input_stream_ports=[
            _parse_audio_stream_port(p)
            for p in (isp.findall(_ns("stream_port")) if isp else [])
        ],
        output_stream_ports=[
            _parse_audio_stream_port(p)
            for p in (osp.findall(_ns("stream_port")) if osp else [])
        ],
        input_external_ports=_parse_external_ports(el, "input_external_ports"),
        output_external_ports=_parse_external_ports(el, "output_external_ports"),
        input_internal_ports=_parse_internal_ports(el, "input_internal_ports"),
        output_internal_ports=_parse_internal_ports(el, "output_internal_ports"),
        controls=_parse_controls(el),
        current_sampling_rate=_parse_sampling_rate(cur_rate_el)
        if cur_rate_el is not None
        else 0,
        sampling_rates=[
            _parse_sampling_rate(r)
            for r in (rates_el.findall(_ns("sampling_rate")) if rates_el else [])
        ],
        symbol=_symbol(el),
    )


def _parse_stream_formats(el: ET.Element) -> list[int]:
    container = el.find(_ns("stream_formats"))
    if container is None:
        return []
    return [
        int(f.text.strip(), 16)
        for f in container.findall(_ns("stream_format"))
        if f.text
    ]


def _parse_stream(el: ET.Element) -> Stream:
    return Stream(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        clock_domain_index=_hex16(el, "clock_domain_index"),
        stream_flags=_flags16(el, "stream_flags", STREAM_FLAGS_NAMES),
        current_format=_hex64(el, "current_stream_format"),
        backup_talker_entity_id_0=_hex64(el, "backup_talker_entity_id0"),
        backup_talker_unique_id_0=_hex16(el, "backup_talker_uniqueid0"),
        backup_talker_entity_id_1=_hex64(el, "backup_talker_entity_id1"),
        backup_talker_unique_id_1=_hex16(el, "backup_talker_uniqueid1"),
        backup_talker_entity_id_2=_hex64(el, "backup_talker_entity_id2"),
        backup_talker_unique_id_2=_hex16(el, "backup_talker_uniqueid2"),
        backedup_talker_entity_id=_hex64(el, "backed_up_talker_entity_id"),
        backedup_talker_unique_id=_hex16(el, "backed_up_talker_uniqueid"),
        avb_interface_index=_hex16(el, "avb_interface_index"),
        buffer_length=_hex32(el, "buffer_length"),
        timing=_hex16(el, "timing"),
        formats=_parse_stream_formats(el),
        symbol=_symbol(el),
    )


def _parse_jack(el: ET.Element) -> Jack:
    return Jack(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        jack_flags=_flags16(el, "jack_flags", JACK_FLAGS_NAMES),
        jack_type=_enum16(el, "jack_type", JACK_TYPE_NAMES),
        symbol=_symbol(el),
    )


def _parse_avb_interface(el: ET.Element) -> AvbInterface:
    mac_str = _text(el, "mac_address", "00:00:00:00:00:00")
    mac_int = int(mac_str.replace(":", ""), 16)
    return AvbInterface(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        mac_address=mac_int,
        interface_flags=_flags16(el, "interface_flags", INTERFACE_FLAGS_NAMES),
        clock_identity=_hex64(el, "clock_identity"),
        priority1=int(_text(el, "priority1", "ff"), 16),
        clock_class=int(_text(el, "clock_class", "ff"), 16),
        offset_scaled_log_variance=_hex16(el, "offset_scaled_log_variance"),
        clock_accuracy=int(_text(el, "clock_accuracy", "ff"), 16),
        priority2=int(_text(el, "priority2", "ff"), 16),
        domain_number=int(_text(el, "domain_number", "0"), 16),
        log_sync_interval=int(_text(el, "log_sync_interval", "0"), 16),
        log_announce_interval=int(_text(el, "log_announce_interval", "0"), 16),
        log_pdelay_interval=int(_text(el, "log_pdelay_interval", "0"), 16),
        port_number=_hex16(el, "port_number"),
        symbol=_symbol(el),
    )


def _parse_clock_source(el: ET.Element) -> ClockSource:
    return ClockSource(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        clock_source_flags=_hex16(el, "clock_source_flags"),
        clock_source_type=_enum16(el, "clock_source_type", CLOCK_SOURCE_TYPE_NAMES),
        clock_source_identifier=_hex64(el, "clock_source_id"),
        clock_source_location_type=_enum16(
            el, "clock_source_location_type", DESCRIPTOR_TYPE_NAMES
        ),
        clock_source_location_index=_hex16(el, "clock_source_location_index"),
        symbol=_symbol(el),
    )


def _parse_clock_domain(el: ET.Element) -> ClockDomain:
    cs_el = el.find(_ns("clock_sources"))
    indices = []
    if cs_el is not None:
        for idx_el in cs_el.findall(_ns("clock_source_index")):
            if idx_el.text:
                indices.append(int(idx_el.text.strip(), 16))
    return ClockDomain(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        clock_source_index=_hex16(el, "clock_source_index"),
        clock_sources=indices,
        symbol=_symbol(el),
    )


def _parse_signal_source(el: ET.Element) -> SignalSource:
    return SignalSource(
        signal_type=_enum16(el, "signal_type", DESCRIPTOR_TYPE_NAMES),
        signal_index=_hex16(el, "signal_index"),
        signal_output=_hex16(el, "signal_output"),
    )


def _parse_signal_sources(el: ET.Element, tag: str = "sources") -> list[SignalSource]:
    container = el.find(_ns(tag))
    if container is None:
        return []
    return [_parse_signal_source(s) for s in container.findall(_ns("source"))]


def _parse_signal_selector(el: ET.Element) -> SignalSelector:
    return SignalSelector(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        block_latency=_hex32(el, "block_latency"),
        control_latency=_hex32(el, "control_latency"),
        control_domain=_hex16(el, "control_domain"),
        current_signal_type=_enum16(el, "current_signal_type", DESCRIPTOR_TYPE_NAMES),
        current_signal_index=_hex16(el, "current_signal_index"),
        current_signal_output=_hex16(el, "current_signal_output"),
        default_signal_type=_enum16(el, "default_signal_type", DESCRIPTOR_TYPE_NAMES),
        default_signal_index=_hex16(el, "default_signal_index"),
        default_signal_output=_hex16(el, "default_signal_output"),
        sources=_parse_signal_sources(el),
        symbol=_symbol(el),
    )


def _parse_mixer(el: ET.Element) -> Mixer:
    return Mixer(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        block_latency=_hex32(el, "block_latency"),
        control_latency=_hex32(el, "control_latency"),
        control_domain=_hex16(el, "control_domain"),
        control_value_type=_hex16(el, "control_value_type"),
        sources=_parse_signal_sources(el),
        symbol=_symbol(el),
    )


def _parse_matrix(el: ET.Element) -> Matrix:
    return Matrix(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        block_latency=_hex32(el, "block_latency"),
        control_latency=_hex32(el, "control_latency"),
        control_domain=_hex16(el, "control_domain"),
        control_value_type=_hex16(el, "control_value_type"),
        control_type=_hex64(el, "control_type"),
        width=_hex16(el, "width"),
        height=_hex16(el, "height"),
        number_of_values=_hex16(el, "number_of_values"),
        number_of_sources=_hex16(el, "number_of_sources"),
        base_source=_hex16(el, "base_source"),
        symbol=_symbol(el),
    )


def _parse_splitter_map_entry(el: ET.Element) -> SplitterMapEntry:
    return SplitterMapEntry(
        sub_signal_start=_hex16(el, "sub_signal_start"),
        sub_signal_count=_hex16(el, "sub_signal_count"),
        output_index=_hex16(el, "output_index"),
    )


def _parse_signal_splitter(el: ET.Element) -> SignalSplitter:
    map_el = el.find(_ns("splitter_map"))
    entries = []
    if map_el is not None:
        entries = [
            _parse_splitter_map_entry(e) for e in map_el.findall(_ns("map_entry"))
        ]
    return SignalSplitter(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        block_latency=_hex32(el, "block_latency"),
        control_latency=_hex32(el, "control_latency"),
        control_domain=_hex16(el, "control_domain"),
        signal_type=_enum16(el, "signal_type", DESCRIPTOR_TYPE_NAMES),
        signal_index=_hex16(el, "signal_index"),
        signal_output=_hex16(el, "signal_output"),
        number_of_outputs=_hex16(el, "number_of_outputs"),
        splitter_map=entries,
        symbol=_symbol(el),
    )


def _parse_combiner_map_entry(el: ET.Element) -> CombinerMapEntry:
    return CombinerMapEntry(
        sub_signal_start=_hex16(el, "sub_signal_start"),
        sub_signal_count=_hex16(el, "sub_signal_count"),
        input_index=_hex16(el, "input_index"),
    )


def _parse_signal_combiner(el: ET.Element) -> SignalCombiner:
    map_el = el.find(_ns("combiner_map"))
    entries = []
    if map_el is not None:
        entries = [
            _parse_combiner_map_entry(e) for e in map_el.findall(_ns("map_entry"))
        ]
    return SignalCombiner(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        block_latency=_hex32(el, "block_latency"),
        control_latency=_hex32(el, "control_latency"),
        control_domain=_hex16(el, "control_domain"),
        combiner_map=entries,
        sources=_parse_signal_sources(el),
        symbol=_symbol(el),
    )


def _parse_signal_demultiplexer(el: ET.Element) -> SignalDemultiplexer:
    map_el = el.find(_ns("demultiplexer_map"))
    entries = []
    if map_el is not None:
        entries = [
            _parse_splitter_map_entry(e) for e in map_el.findall(_ns("map_entry"))
        ]
    return SignalDemultiplexer(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        block_latency=_hex32(el, "block_latency"),
        control_latency=_hex32(el, "control_latency"),
        control_domain=_hex16(el, "control_domain"),
        signal_type=_enum16(el, "signal_type", DESCRIPTOR_TYPE_NAMES),
        signal_index=_hex16(el, "signal_index"),
        signal_output=_hex16(el, "signal_output"),
        number_of_outputs=_hex16(el, "number_of_outputs"),
        demultiplexer_map=entries,
        symbol=_symbol(el),
    )


def _parse_signal_multiplexer(el: ET.Element) -> SignalMultiplexer:
    map_el = el.find(_ns("multiplexer_map"))
    entries = []
    if map_el is not None:
        entries = [
            _parse_combiner_map_entry(e) for e in map_el.findall(_ns("map_entry"))
        ]
    return SignalMultiplexer(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        block_latency=_hex32(el, "block_latency"),
        control_latency=_hex32(el, "control_latency"),
        control_domain=_hex16(el, "control_domain"),
        multiplexer_map=entries,
        sources=_parse_signal_sources(el),
        symbol=_symbol(el),
    )


def _parse_signal_transcoder(el: ET.Element) -> SignalTranscoder:
    return SignalTranscoder(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        block_latency=_hex32(el, "block_latency"),
        control_latency=_hex32(el, "control_latency"),
        control_domain=_hex16(el, "control_domain"),
        control_value_type=_hex16(el, "control_value_type"),
        signal_type=_enum16(el, "signal_type", DESCRIPTOR_TYPE_NAMES),
        signal_index=_hex16(el, "signal_index"),
        signal_output=_hex16(el, "signal_output"),
        transcoder_type=_hex64(el, "transcoder_type"),
        symbol=_symbol(el),
    )


def _parse_control_block(el: ET.Element) -> ControlBlock:
    return ControlBlock(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        number_of_controls=_hex16(el, "number_of_controls"),
        base_control=_hex16(el, "base_control"),
        final_control_index=_hex16(el, "final_control_index"),
        signal_type=_enum16(el, "signal_type", DESCRIPTOR_TYPE_NAMES),
        signal_index=_hex16(el, "signal_index"),
        signal_output=_hex16(el, "signal_output"),
        symbol=_symbol(el),
    )


def _parse_memory_object(el: ET.Element) -> MemoryObject:
    return MemoryObject(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        memory_object_type=_enum16(el, "memory_object_type", MEMORY_OBJECT_TYPE_NAMES),
        target_descriptor_type=_enum16(
            el, "target_descriptor_type", DESCRIPTOR_TYPE_NAMES
        ),
        target_descriptor_index=_hex16(el, "target_descriptor_index"),
        start_address=_hex64(el, "start_address"),
        maximum_length=_hex64(el, "maximum_length"),
        length=_hex64(el, "length"),
        maximum_segment_length=_hex64(el, "maximum_segment_length"),
        symbol=_symbol(el),
    )


def _parse_video_cluster(el: ET.Element) -> VideoCluster:
    return VideoCluster(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        signal_type=_enum16(el, "signal_type", DESCRIPTOR_TYPE_NAMES),
        signal_index=_hex16(el, "signal_index"),
        signal_output=_hex16(el, "signal_output"),
        path_latency=_hex32(el, "path_latency"),
        block_latency=_hex32(el, "block_latency"),
        format=_hex16(el, "format"),
        symbol=_symbol(el),
    )


def _parse_sensor_cluster(el: ET.Element) -> SensorCluster:
    return SensorCluster(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        signal_type=_enum16(el, "signal_type", DESCRIPTOR_TYPE_NAMES),
        signal_index=_hex16(el, "signal_index"),
        signal_output=_hex16(el, "signal_output"),
        path_latency=_hex32(el, "path_latency"),
        block_latency=_hex32(el, "block_latency"),
        format=_hex16(el, "format"),
        symbol=_symbol(el),
    )


def _parse_video_mapping(el: ET.Element) -> VideoMapping:
    return VideoMapping(
        stream_index=_hex16(el, "stream_index"),
        stream_channel=_hex16(el, "stream_channel"),
        cluster_offset=_hex16(el, "cluster_offset"),
        cluster_channel=_hex16(el, "cluster_channel"),
    )


def _parse_video_map(el: ET.Element) -> VideoMap:
    mappings = [_parse_video_mapping(m) for m in el.findall(_ns("video_mapping"))]
    return VideoMap(mappings=mappings, symbol=_symbol(el))


def _parse_sensor_mapping(el: ET.Element) -> SensorMapping:
    # stream_channel is the pre-2026-10 spelling of stream_signal
    signal_tag = (
        "stream_signal"
        if el.find(_ns("stream_signal")) is not None
        else "stream_channel"
    )
    return SensorMapping(
        stream_index=_hex16(el, "stream_index"),
        stream_signal=_hex16(el, signal_tag),
        cluster_offset=_hex16(el, "cluster_offset"),
    )


def _parse_sensor_map(el: ET.Element) -> SensorMap:
    mappings = [_parse_sensor_mapping(m) for m in el.findall(_ns("sensor_mapping"))]
    return SensorMap(mappings=mappings, symbol=_symbol(el))


def _parse_video_stream_port(el: ET.Element) -> VideoStreamPort:
    clusters_el = el.find(_ns("clusters"))
    clusters = [
        _parse_video_cluster(c)
        for c in (clusters_el.findall(_ns("video_cluster")) if clusters_el else [])
    ]
    maps_el = el.find(_ns("maps"))
    maps = [
        _parse_video_map(m)
        for m in (maps_el.findall(_ns("video_map")) if maps_el else [])
    ]
    return VideoStreamPort(
        clock_domain_index=_hex16(el, "clock_domain_index"),
        port_flags=_hex16(el, "port_flags"),
        controls=_parse_controls(el),
        clusters=clusters,
        maps=maps,
        symbol=_symbol(el),
    )


def _parse_sensor_stream_port(el: ET.Element) -> SensorStreamPort:
    clusters_el = el.find(_ns("clusters"))
    clusters = [
        _parse_sensor_cluster(c)
        for c in (clusters_el.findall(_ns("sensor_cluster")) if clusters_el else [])
    ]
    maps_el = el.find(_ns("maps"))
    maps = [
        _parse_sensor_map(m)
        for m in (maps_el.findall(_ns("sensor_map")) if maps_el else [])
    ]
    return SensorStreamPort(
        clock_domain_index=_hex16(el, "clock_domain_index"),
        port_flags=_hex16(el, "port_flags"),
        controls=_parse_controls(el),
        clusters=clusters,
        maps=maps,
        symbol=_symbol(el),
    )


def _parse_external_port(el: ET.Element) -> ExternalPort:
    return ExternalPort(
        clock_domain_index=_hex16(el, "clock_domain_index"),
        port_flags=_hex16(el, "port_flags"),
        signal_type=_enum16(el, "signal_type", DESCRIPTOR_TYPE_NAMES),
        signal_index=_hex16(el, "signal_index"),
        signal_output=_hex16(el, "signal_output"),
        block_latency=_hex32(el, "block_latency"),
        jack_index=_hex16(el, "jack_index"),
        controls=_parse_controls(el),
        symbol=_symbol(el),
    )


def _parse_internal_port(el: ET.Element) -> InternalPort:
    return InternalPort(
        clock_domain_index=_hex16(el, "clock_domain_index"),
        port_flags=_hex16(el, "port_flags"),
        signal_type=_enum16(el, "signal_type", DESCRIPTOR_TYPE_NAMES),
        signal_index=_hex16(el, "signal_index"),
        signal_output=_hex16(el, "signal_output"),
        block_latency=_hex32(el, "block_latency"),
        internal_index=_hex16(el, "internal_index"),
        controls=_parse_controls(el),
        symbol=_symbol(el),
    )


def _parse_external_ports(el: ET.Element, tag: str) -> list[ExternalPort]:
    container = el.find(_ns(tag))
    if container is None:
        return []
    return [_parse_external_port(p) for p in container.findall(_ns("external_port"))]


def _parse_internal_ports(el: ET.Element, tag: str) -> list[InternalPort]:
    container = el.find(_ns(tag))
    if container is None:
        return []
    return [_parse_internal_port(p) for p in container.findall(_ns("internal_port"))]


def _parse_video_unit(el: ET.Element) -> VideoUnit:
    isp = el.find(_ns("input_stream_ports"))
    osp = el.find(_ns("output_stream_ports"))
    return VideoUnit(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        clock_domain_index=_hex16(el, "clock_domain_index"),
        input_stream_ports=[
            _parse_video_stream_port(p)
            for p in (isp.findall(_ns("stream_port")) if isp else [])
        ],
        output_stream_ports=[
            _parse_video_stream_port(p)
            for p in (osp.findall(_ns("stream_port")) if osp else [])
        ],
        input_external_ports=_parse_external_ports(el, "input_external_ports"),
        output_external_ports=_parse_external_ports(el, "output_external_ports"),
        input_internal_ports=_parse_internal_ports(el, "input_internal_ports"),
        output_internal_ports=_parse_internal_ports(el, "output_internal_ports"),
        controls=_parse_controls(el),
        symbol=_symbol(el),
    )


def _parse_sensor_unit(el: ET.Element) -> SensorUnit:
    isp = el.find(_ns("input_stream_ports"))
    osp = el.find(_ns("output_stream_ports"))
    return SensorUnit(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        clock_domain_index=_hex16(el, "clock_domain_index"),
        input_stream_ports=[
            _parse_sensor_stream_port(p)
            for p in (isp.findall(_ns("stream_port")) if isp else [])
        ],
        output_stream_ports=[
            _parse_sensor_stream_port(p)
            for p in (osp.findall(_ns("stream_port")) if osp else [])
        ],
        input_external_ports=_parse_external_ports(el, "input_external_ports"),
        output_external_ports=_parse_external_ports(el, "output_external_ports"),
        input_internal_ports=_parse_internal_ports(el, "input_internal_ports"),
        output_internal_ports=_parse_internal_ports(el, "output_internal_ports"),
        controls=_parse_controls(el),
        symbol=_symbol(el),
    )


def _parse_timing(el: ET.Element) -> Timing:
    ptp_el = el.find(_ns("ptp_instances"))
    indices = []
    if ptp_el is not None:
        for idx_el in ptp_el.findall(_ns("ptp_instance_index")):
            if idx_el.text:
                indices.append(int(idx_el.text.strip(), 16))
    return Timing(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        algorithm=_enum16(el, "algorithm", TIMING_ALGORITHM_NAMES),
        ptp_instance_indices=indices,
        symbol=_symbol(el),
    )


def _parse_ptp_port(el: ET.Element) -> PtpPort:
    prof_str = _text(el, "profile_identifier", "00:00:00:00:00:00")
    prof_int = int(prof_str.replace(":", ""), 16)
    return PtpPort(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        port_number=_hex16(el, "port_number"),
        port_type=_enum16(el, "port_type", PTP_PORT_TYPE_NAMES),
        flags=_flags32(el, "flags", PTP_INSTANCE_FLAGS_NAMES),
        avb_interface_index=_hex16(el, "avb_interface_index"),
        profile_identifier=prof_int,
        symbol=_symbol(el),
    )


def _parse_ptp_instance(el: ET.Element) -> PtpInstance:
    ports_el = el.find(_ns("ptp_ports"))
    ports = []
    if ports_el is not None:
        ports = [_parse_ptp_port(p) for p in ports_el.findall(_ns("ptp_port"))]
    return PtpInstance(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        clock_identity=_hex64(el, "clock_identity"),
        flags=_flags32(el, "flags", PTP_INSTANCE_FLAGS_NAMES),
        number_of_controls=_hex16(el, "number_of_controls"),
        base_control=_hex16(el, "base_control"),
        ptp_ports=ports,
        symbol=_symbol(el),
    )


def _parse_strings(el: ET.Element) -> StringsDescriptor:
    return StringsDescriptor(
        strings=[s.text.strip() if s.text else "" for s in el.findall(_ns("string"))],
        symbol=_symbol(el),
    )


def _parse_locale(el: ET.Element) -> Locale:
    ls_el = el.find(_ns("locale_strings"))
    strings_descs = []
    if ls_el is not None:
        for s in ls_el.findall(_ns("strings")):
            strings_descs.append(_parse_strings(s))
    return Locale(
        locale_identifier=_text(el, "locale_identifier"),
        strings_descriptors=strings_descs,
        symbol=_symbol(el),
    )


def _parse_configuration(el: ET.Element) -> Configuration:
    def _find_all(parent_tag: str, child_tag: str, parser):
        container = el.find(_ns(parent_tag))
        if container is None:
            return []
        return [parser(c) for c in container.findall(_ns(child_tag))]

    return Configuration(
        object_name=_text(el, "object_name"),
        localized_description=_parse_localized_string_ref(el, "localized_description"),
        audio_units=_find_all("audio_units", "audio_unit", _parse_audio_unit),
        video_units=_find_all("video_units", "video_unit", _parse_video_unit),
        sensor_units=_find_all("sensor_units", "sensor_unit", _parse_sensor_unit),
        streams_input=_find_all("input_streams", "stream", _parse_stream),
        streams_output=_find_all("output_streams", "stream", _parse_stream),
        jacks_input=_find_all("input_jacks", "jack", _parse_jack),
        jacks_output=_find_all("output_jacks", "jack", _parse_jack),
        avb_interfaces=_find_all(
            "avb_interfaces", "avb_interface", _parse_avb_interface
        ),
        clock_sources=_find_all("clock_sources", "clock_source", _parse_clock_source),
        memory_objects=_find_all(
            "memory_objects", "memory_object", _parse_memory_object
        ),
        controls=_parse_controls(el),
        signal_selectors=_find_all(
            "signal_selectors", "signal_selector", _parse_signal_selector
        ),
        mixers=_find_all("mixers", "mixer", _parse_mixer),
        matrices=_find_all("matrices", "matrix", _parse_matrix),
        splitters=_find_all("splitters", "signal_splitter", _parse_signal_splitter),
        combiners=_find_all("combiners", "signal_combiner", _parse_signal_combiner),
        demultiplexers=_find_all(
            "demultiplexers", "signal_demultiplexer", _parse_signal_demultiplexer
        ),
        multiplexers=_find_all(
            "multiplexers", "signal_multiplexer", _parse_signal_multiplexer
        ),
        transcoders=_find_all(
            "transcoders", "signal_transcoder", _parse_signal_transcoder
        ),
        control_blocks=_find_all(
            "control_blocks", "control_block", _parse_control_block
        ),
        locales=_find_all("locales", "locale", _parse_locale),
        clock_domains=_find_all("clock_domains", "clock_domain", _parse_clock_domain),
        timings=_find_all("timings", "timing", _parse_timing),
        ptp_instances=_find_all("ptp_instances", "ptp_instance", _parse_ptp_instance),
        symbol=_symbol(el),
    )


def read_aemxml(path_or_string: str) -> Entity:
    """Read an AEMXML file or string and return an Entity data model.

    If path_or_string starts with '<', it's treated as XML string; otherwise as a file path.
    """
    if path_or_string.lstrip().startswith("<"):
        root = ET.fromstring(path_or_string)
    else:
        tree = ET.parse(path_or_string)
        root = tree.getroot()

    configs_el = root.find(_ns("configurations"))
    configs = []
    if configs_el is not None:
        configs = [
            _parse_configuration(c) for c in configs_el.findall(_ns("configuration"))
        ]

    # Detect schema version from xsi:schemaLocation
    schema_loc = (
        root.get("{http://www.w3.org/2001/XMLSchema-instance}schemaLocation") or ""
    )
    schema_year = 2013 if "avdecc.xsd" in schema_loc else 2021
    # Try correct spelling first, fall back to typo (avdecc.xsd V1.0.1 accepts both)
    talker_cap_tag = "talker_capabilities"
    if (
        root.find(_ns("talker_capabilities")) is None
        and root.find(_ns("talker_capabilties")) is not None
    ):
        talker_cap_tag = "talker_capabilties"

    return Entity(
        entity_id=_hex64(root, "entity_id"),
        entity_model_id=_hex64(root, "entity_model_id"),
        entity_capabilities=_flags32(
            root, "entity_capabilities", ENTITY_CAPABILITIES_NAMES
        ),
        talker_stream_sources=_hex16(root, "talker_stream_sources"),
        talker_capabilities=_flags16(root, talker_cap_tag, TALKER_CAPABILITIES_NAMES),
        listener_stream_sinks=_hex16(root, "listener_stream_sinks"),
        listener_capabilities=_flags16(
            root, "listener_capabilities", LISTENER_CAPABILITIES_NAMES
        ),
        controller_capabilities=_flags32(
            root, "controller_capabilities", CONTROLLER_CAPABILITIES_NAMES
        ),
        available_index=_hex32(root, "available_index"),
        association_id=_hex64(root, "association_id"),
        entity_name=_text(root, "entity_name"),
        vendor_name_string=_parse_localized_string_ref(root, "vendor_name"),
        model_name_string=_parse_localized_string_ref(root, "model_name"),
        firmware_version=_text(root, "firmware_version"),
        group_name=_text(root, "group_name"),
        serial_number=_text(root, "serial_number"),
        schema_year=schema_year,
        configurations=configs,
    )


def validate_aemxml(path: str) -> list[str]:
    """Validate an AEMXML file against the XSD schema.

    Automatically selects the correct XSD based on the schemaLocation attribute:
    - avdecc.xsd for 2013-era files
    - atdecc.xsd for 2021-era files (default)

    Returns a list of validation error strings (empty list = valid).
    Requires lxml as an optional dependency.
    """
    from lxml import etree as lxml_etree

    # Find XSD relative to repo root (walk up from this file)
    this_dir = os.path.dirname(os.path.abspath(__file__))
    # python/aemxml/ -> repo root is 2 levels up
    repo_root = os.path.dirname(os.path.dirname(this_dir))

    # Detect schema version from document
    xml_doc = lxml_etree.parse(path)
    root = xml_doc.getroot()
    schema_loc = (
        root.get("{http://www.w3.org/2001/XMLSchema-instance}schemaLocation") or ""
    )
    xsd_name = "avdecc.xsd" if "avdecc.xsd" in schema_loc else "atdecc.xsd"
    xsd_path = os.path.join(repo_root, "standards", "ieee1722.1-schema", xsd_name)

    xsd_doc = lxml_etree.parse(xsd_path)
    schema = lxml_etree.XMLSchema(xsd_doc)

    if schema.validate(xml_doc):
        return []

    return [str(err) for err in schema.error_log]
