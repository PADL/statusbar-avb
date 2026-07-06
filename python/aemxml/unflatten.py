# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Unflatten a flat descriptor list back into a hierarchical AEM entity model.

This is the inverse of ``flatten.py``: it consumes the ``(descriptors, symbols)``
pair produced by ``blob_reader.read_blob`` (or by ``flatten.flatten``) and
rebuilds the ``model.Entity`` tree. Parent/child nesting is recovered from the
same base/count fields that ``flatten`` computes:

* AUDIO/VIDEO/SENSOR_UNIT owns its STREAM_PORT_* / EXTERNAL_PORT_* /
  INTERNAL_PORT_* children via ``base_*``/``number_of_*`` and its unit-level
  CONTROLs via ``base_control``/``number_of_controls``.
* STREAM_PORT_* owns AUDIO/VIDEO/SENSOR_CLUSTER + *_MAP via ``base_cluster`` /
  ``base_map`` and its port-level CONTROLs via ``base_control``.
* CONTROLs not referenced by any unit or port are configuration-level controls.
* CLOCK_DOMAIN references CLOCK_SOURCEs by index list.
* LOCALE owns STRINGS via ``base_strings``/``number_of_strings``.
* PTP_INSTANCE owns PTP_PORTs via ``base_ptp_port``/``number_of_ptp_ports``.

Symbols are stored on the wire as a one-way CRC32 code (see
``flatten.symbol_to_code``); the original symbol strings cannot be recovered, so
reconstructed descriptors carry ``symbol=None``. This does not affect the
descriptor wire bytes, only the (optional) human-readable symbol attribute.
"""

from __future__ import annotations

from .model import (
    DESCRIPTOR_ENTITY,
    DESCRIPTOR_CONFIGURATION,
    DESCRIPTOR_AUDIO_UNIT,
    DESCRIPTOR_VIDEO_UNIT,
    DESCRIPTOR_SENSOR_UNIT,
    DESCRIPTOR_STREAM_INPUT,
    DESCRIPTOR_STREAM_OUTPUT,
    DESCRIPTOR_JACK_INPUT,
    DESCRIPTOR_JACK_OUTPUT,
    DESCRIPTOR_AVB_INTERFACE,
    DESCRIPTOR_CLOCK_SOURCE,
    DESCRIPTOR_MEMORY_OBJECT,
    DESCRIPTOR_LOCALE,
    DESCRIPTOR_STRINGS,
    DESCRIPTOR_STREAM_PORT_INPUT,
    DESCRIPTOR_STREAM_PORT_OUTPUT,
    DESCRIPTOR_EXTERNAL_PORT_INPUT,
    DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
    DESCRIPTOR_INTERNAL_PORT_INPUT,
    DESCRIPTOR_INTERNAL_PORT_OUTPUT,
    DESCRIPTOR_AUDIO_CLUSTER,
    DESCRIPTOR_VIDEO_CLUSTER,
    DESCRIPTOR_SENSOR_CLUSTER,
    DESCRIPTOR_AUDIO_MAP,
    DESCRIPTOR_VIDEO_MAP,
    DESCRIPTOR_SENSOR_MAP,
    DESCRIPTOR_CONTROL,
    DESCRIPTOR_SIGNAL_SELECTOR,
    DESCRIPTOR_MIXER,
    DESCRIPTOR_MATRIX,
    DESCRIPTOR_SIGNAL_SPLITTER,
    DESCRIPTOR_SIGNAL_COMBINER,
    DESCRIPTOR_SIGNAL_DEMULTIPLEXER,
    DESCRIPTOR_SIGNAL_MULTIPLEXER,
    DESCRIPTOR_SIGNAL_TRANSCODER,
    DESCRIPTOR_CLOCK_DOMAIN,
    DESCRIPTOR_CONTROL_BLOCK,
    DESCRIPTOR_TIMING,
    DESCRIPTOR_PTP_INSTANCE,
    DESCRIPTOR_PTP_PORT,
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
)
from .wire import (
    unpack_u8,
    unpack_u16,
    unpack_u32,
    unpack_u64,
    unpack_string64,
    unpack_eui48,
    unpack_eui64,
    unpack_localized_string_ref,
)


def _loc(data: bytes, pos: int) -> LocalizedStringRef:
    offset, index = unpack_localized_string_ref(data, pos)
    return LocalizedStringRef(offset=offset, index=index)


# --- Per-descriptor parsers (inverse of flatten._serialize_*) ---


def _parse_entity(d: bytes) -> tuple[Entity, int]:
    """Return (Entity without configurations, configurations_count)."""
    entity = Entity(
        entity_id=unpack_eui64(d, 4),
        entity_model_id=unpack_eui64(d, 12),
        entity_capabilities=unpack_u32(d, 20),
        talker_stream_sources=unpack_u16(d, 24),
        talker_capabilities=unpack_u16(d, 26),
        listener_stream_sinks=unpack_u16(d, 28),
        listener_capabilities=unpack_u16(d, 30),
        controller_capabilities=unpack_u32(d, 32),
        available_index=unpack_u32(d, 36),
        association_id=unpack_eui64(d, 40),
        entity_name=unpack_string64(d, 48),
        vendor_name_string=_loc(d, 112),
        model_name_string=_loc(d, 114),
        firmware_version=unpack_string64(d, 116),
        group_name=unpack_string64(d, 180),
        serial_number=unpack_string64(d, 244),
    )
    configurations_count = unpack_u16(d, 308)
    return entity, configurations_count


def _parse_configuration(d: bytes) -> Configuration:
    return Configuration(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
    )


def _parse_audio_unit(d: bytes) -> tuple[AudioUnit, dict]:
    unit = AudioUnit(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        clock_domain_index=unpack_u16(d, 70),
        current_sampling_rate=unpack_u32(d, 136),
    )
    n_rates = unpack_u16(d, 142)
    unit.sampling_rates = [unpack_u32(d, 144 + 4 * i) for i in range(n_rates)]
    ranges = _unit_ranges(d)
    return unit, ranges


def _parse_video_unit(d: bytes) -> tuple[VideoUnit, dict]:
    unit = VideoUnit(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        clock_domain_index=unpack_u16(d, 70),
    )
    return unit, _unit_ranges(d)


def _parse_sensor_unit(d: bytes) -> tuple[SensorUnit, dict]:
    unit = SensorUnit(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        clock_domain_index=unpack_u16(d, 70),
    )
    return unit, _unit_ranges(d)


def _unit_ranges(d: bytes) -> dict:
    """Shared *_UNIT layout for the port/control base+count fields (offsets 72..99)."""
    return {
        "num_stream_input_ports": unpack_u16(d, 72),
        "base_stream_input_port": unpack_u16(d, 74),
        "num_stream_output_ports": unpack_u16(d, 76),
        "base_stream_output_port": unpack_u16(d, 78),
        "num_external_input_ports": unpack_u16(d, 80),
        "base_external_input_port": unpack_u16(d, 82),
        "num_external_output_ports": unpack_u16(d, 84),
        "base_external_output_port": unpack_u16(d, 86),
        "num_internal_input_ports": unpack_u16(d, 88),
        "base_internal_input_port": unpack_u16(d, 90),
        "num_internal_output_ports": unpack_u16(d, 92),
        "base_internal_output_port": unpack_u16(d, 94),
        "num_controls": unpack_u16(d, 96),
        "base_control": unpack_u16(d, 98),
    }


def _parse_stream(d: bytes) -> Stream:
    n_formats = unpack_u16(d, 84)
    n_redundant = unpack_u16(d, 134)
    stream = Stream(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        clock_domain_index=unpack_u16(d, 70),
        stream_flags=unpack_u16(d, 72),
        current_format=unpack_eui64(d, 74),
        backup_talker_entity_id_0=unpack_eui64(d, 86),
        backup_talker_unique_id_0=unpack_u16(d, 94),
        backup_talker_entity_id_1=unpack_eui64(d, 96),
        backup_talker_unique_id_1=unpack_u16(d, 104),
        backup_talker_entity_id_2=unpack_eui64(d, 106),
        backup_talker_unique_id_2=unpack_u16(d, 114),
        backedup_talker_entity_id=unpack_eui64(d, 116),
        backedup_talker_unique_id=unpack_u16(d, 124),
        avb_interface_index=unpack_u16(d, 126),
        buffer_length=unpack_u32(d, 128),
        redundant_offset=unpack_u16(d, 132),
        number_of_redundant_streams=n_redundant,
        timing=unpack_u16(d, 136),
    )
    formats_offset = 138
    stream.formats = [unpack_eui64(d, formats_offset + 8 * i) for i in range(n_formats)]
    redundant_offset = formats_offset + 8 * n_formats
    stream.redundant_streams = [
        unpack_u16(d, redundant_offset + 2 * i) for i in range(n_redundant)
    ]
    return stream


def _parse_jack(d: bytes) -> Jack:
    return Jack(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        jack_flags=unpack_u16(d, 70),
        jack_type=unpack_u16(d, 72),
    )


def _parse_avb_interface(d: bytes) -> AvbInterface:
    return AvbInterface(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        mac_address=unpack_eui48(d, 70),
        interface_flags=unpack_u16(d, 76),
        clock_identity=unpack_eui64(d, 78),
        priority1=unpack_u8(d, 86),
        clock_class=unpack_u8(d, 87),
        offset_scaled_log_variance=unpack_u16(d, 88),
        clock_accuracy=unpack_u8(d, 90),
        priority2=unpack_u8(d, 91),
        domain_number=unpack_u8(d, 92),
        log_sync_interval=unpack_u8(d, 93),
        log_announce_interval=unpack_u8(d, 94),
        log_pdelay_interval=unpack_u8(d, 95),
        port_number=unpack_u16(d, 96),
        number_of_controls=unpack_u16(d, 98),
        base_control=unpack_u16(d, 100),
    )


def _parse_clock_source(d: bytes) -> ClockSource:
    return ClockSource(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        clock_source_flags=unpack_u16(d, 70),
        clock_source_type=unpack_u16(d, 72),
        clock_source_identifier=unpack_eui64(d, 74),
        clock_source_location_type=unpack_u16(d, 82),
        clock_source_location_index=unpack_u16(d, 84),
    )


def _parse_clock_domain(d: bytes) -> ClockDomain:
    n = unpack_u16(d, 74)
    return ClockDomain(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        clock_source_index=unpack_u16(d, 70),
        clock_sources=[unpack_u16(d, 76 + 2 * i) for i in range(n)],
    )


def _parse_locale(d: bytes) -> tuple[Locale, int, int]:
    """Return (Locale without strings, number_of_strings, base_strings)."""
    locale = Locale(locale_identifier=unpack_string64(d, 4))
    number_of_strings = unpack_u16(d, 68)
    base_strings = unpack_u16(d, 70)
    return locale, number_of_strings, base_strings


def _parse_strings(d: bytes) -> StringsDescriptor:
    return StringsDescriptor(strings=[unpack_string64(d, 4 + 64 * i) for i in range(7)])


def _parse_memory_object(d: bytes) -> MemoryObject:
    return MemoryObject(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        memory_object_type=unpack_u16(d, 70),
        target_descriptor_type=unpack_u16(d, 72),
        target_descriptor_index=unpack_u16(d, 74),
        start_address=unpack_u64(d, 76),
        maximum_length=unpack_u64(d, 84),
        length=unpack_u64(d, 92),
        maximum_segment_length=unpack_u64(d, 100),
    )


def _parse_stream_port(d: bytes) -> dict:
    return {
        "clock_domain_index": unpack_u16(d, 4),
        "port_flags": unpack_u16(d, 6),
        "num_controls": unpack_u16(d, 8),
        "base_control": unpack_u16(d, 10),
        "num_clusters": unpack_u16(d, 12),
        "base_cluster": unpack_u16(d, 14),
        "num_maps": unpack_u16(d, 16),
        "base_map": unpack_u16(d, 18),
    }


def _parse_external_port(d: bytes) -> dict:
    return {
        "clock_domain_index": unpack_u16(d, 4),
        "port_flags": unpack_u16(d, 6),
        "num_controls": unpack_u16(d, 8),
        "base_control": unpack_u16(d, 10),
        "signal_type": unpack_u16(d, 12),
        "signal_index": unpack_u16(d, 14),
        "signal_output": unpack_u16(d, 16),
        "block_latency": unpack_u32(d, 18),
        "jack_index": unpack_u16(d, 22),
    }


def _parse_internal_port(d: bytes) -> dict:
    return {
        "clock_domain_index": unpack_u16(d, 4),
        "port_flags": unpack_u16(d, 6),
        "num_controls": unpack_u16(d, 8),
        "base_control": unpack_u16(d, 10),
        "signal_type": unpack_u16(d, 12),
        "signal_index": unpack_u16(d, 14),
        "signal_output": unpack_u16(d, 16),
        "block_latency": unpack_u32(d, 18),
        "internal_index": unpack_u16(d, 22),
    }


def _parse_audio_cluster(d: bytes) -> AudioCluster:
    return AudioCluster(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        signal_type=unpack_u16(d, 70),
        signal_index=unpack_u16(d, 72),
        signal_output=unpack_u16(d, 74),
        path_latency=unpack_u32(d, 76),
        block_latency=unpack_u32(d, 80),
        channel_count=unpack_u16(d, 84),
        format=unpack_u8(d, 86),
        aes3_data_type_reference=unpack_u8(d, 87),
        aes3_data_type=unpack_u16(d, 88),
    )


def _parse_audio_map(d: bytes) -> AudioMap:
    n = unpack_u16(d, 6)
    mappings = []
    for i in range(n):
        p = 8 + 8 * i
        mappings.append(
            AudioMapping(
                stream_index=unpack_u16(d, p),
                stream_channel=unpack_u16(d, p + 2),
                cluster_offset=unpack_u16(d, p + 4),
                cluster_channel=unpack_u16(d, p + 6),
            )
        )
    return AudioMap(mappings=mappings)


def _parse_video_map(d: bytes) -> VideoMap:
    n = unpack_u16(d, 6)
    mappings = []
    for i in range(n):
        p = 8 + 8 * i
        mappings.append(
            VideoMapping(
                stream_index=unpack_u16(d, p),
                stream_channel=unpack_u16(d, p + 2),
                cluster_offset=unpack_u16(d, p + 4),
                cluster_channel=unpack_u16(d, p + 6),
            )
        )
    return VideoMap(mappings=mappings)


def _parse_sensor_map(d: bytes) -> SensorMap:
    n = unpack_u16(d, 6)
    mappings = []
    for i in range(n):
        p = 8 + 8 * i
        mappings.append(
            SensorMapping(
                stream_index=unpack_u16(d, p),
                stream_channel=unpack_u16(d, p + 2),
                cluster_offset=unpack_u16(d, p + 4),
                cluster_channel=unpack_u16(d, p + 6),
            )
        )
    return SensorMap(mappings=mappings)


def _parse_video_cluster(d: bytes) -> VideoCluster:
    cluster = VideoCluster(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        signal_type=unpack_u16(d, 70),
        signal_index=unpack_u16(d, 72),
        signal_output=unpack_u16(d, 74),
        path_latency=unpack_u32(d, 76),
        block_latency=unpack_u32(d, 80),
        format=unpack_u16(d, 84),
    )
    # NOTE: format-specific blobs have no per-item length on the wire and are not
    # reconstructed here (not emitted by the C++ models). See module limitations.
    return cluster


def _parse_sensor_cluster(d: bytes) -> SensorCluster:
    return SensorCluster(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        signal_type=unpack_u16(d, 70),
        signal_index=unpack_u16(d, 72),
        signal_output=unpack_u16(d, 74),
        path_latency=unpack_u32(d, 76),
        block_latency=unpack_u32(d, 80),
        format=unpack_u16(d, 84),
    )


def _parse_control(d: bytes) -> Control:
    val_len = unpack_u16(d, 96)
    return Control(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        block_latency=unpack_u32(d, 70),
        control_latency=unpack_u32(d, 74),
        control_domain=unpack_u16(d, 78),
        control_value_type=unpack_u16(d, 80),
        control_type=unpack_eui64(d, 82),
        reset_time=unpack_u32(d, 90),
        signal_type=unpack_u16(d, 98),
        signal_index=unpack_u16(d, 100),
        signal_output=unpack_u16(d, 102),
        value_details=bytes(d[104 : 104 + val_len]),
    )


def _parse_signal_sources(d: bytes, offset: int, n: int) -> list[SignalSource]:
    out = []
    for i in range(n):
        p = offset + 6 * i
        out.append(
            SignalSource(
                signal_type=unpack_u16(d, p),
                signal_index=unpack_u16(d, p + 2),
                signal_output=unpack_u16(d, p + 4),
            )
        )
    return out


def _parse_signal_selector(d: bytes) -> SignalSelector:
    n = unpack_u16(d, 82)
    return SignalSelector(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        block_latency=unpack_u32(d, 70),
        control_latency=unpack_u32(d, 74),
        control_domain=unpack_u16(d, 78),
        current_signal_type=unpack_u16(d, 84),
        current_signal_index=unpack_u16(d, 86),
        current_signal_output=unpack_u16(d, 88),
        default_signal_type=unpack_u16(d, 90),
        default_signal_index=unpack_u16(d, 92),
        default_signal_output=unpack_u16(d, 94),
        sources=_parse_signal_sources(d, 96, n),
    )


def _parse_mixer(d: bytes) -> Mixer:
    n = unpack_u16(d, 82)
    value_offset = 88 + 6 * n
    return Mixer(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        block_latency=unpack_u32(d, 70),
        control_latency=unpack_u32(d, 74),
        control_domain=unpack_u16(d, 78),
        control_value_type=unpack_u16(d, 80),
        sources=_parse_signal_sources(d, 88, n),
        value_details=bytes(d[value_offset:]),
    )


def _parse_matrix(d: bytes) -> Matrix:
    return Matrix(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        block_latency=unpack_u32(d, 70),
        control_latency=unpack_u32(d, 74),
        control_domain=unpack_u16(d, 78),
        control_value_type=unpack_u16(d, 80),
        control_type=unpack_eui64(d, 82),
        width=unpack_u16(d, 90),
        height=unpack_u16(d, 92),
        number_of_values=unpack_u16(d, 96),
        number_of_sources=unpack_u16(d, 98),
        base_source=unpack_u16(d, 100),
        value_details=bytes(d[102:]),
    )


def _parse_signal_splitter(d: bytes) -> SignalSplitter:
    n = unpack_u16(d, 90)
    entries = []
    for i in range(n):
        p = 92 + 6 * i
        entries.append(
            SplitterMapEntry(
                sub_signal_start=unpack_u16(d, p),
                sub_signal_count=unpack_u16(d, p + 2),
                output_index=unpack_u16(d, p + 4),
            )
        )
    return SignalSplitter(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        block_latency=unpack_u32(d, 70),
        control_latency=unpack_u32(d, 74),
        control_domain=unpack_u16(d, 78),
        signal_type=unpack_u16(d, 80),
        signal_index=unpack_u16(d, 82),
        signal_output=unpack_u16(d, 84),
        number_of_outputs=unpack_u16(d, 86),
        splitter_map=entries,
    )


def _parse_signal_demultiplexer(d: bytes) -> SignalDemultiplexer:
    n = unpack_u16(d, 90)
    entries = []
    for i in range(n):
        p = 92 + 6 * i
        entries.append(
            SplitterMapEntry(
                sub_signal_start=unpack_u16(d, p),
                sub_signal_count=unpack_u16(d, p + 2),
                output_index=unpack_u16(d, p + 4),
            )
        )
    return SignalDemultiplexer(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        block_latency=unpack_u32(d, 70),
        control_latency=unpack_u32(d, 74),
        control_domain=unpack_u16(d, 78),
        signal_type=unpack_u16(d, 80),
        signal_index=unpack_u16(d, 82),
        signal_output=unpack_u16(d, 84),
        number_of_outputs=unpack_u16(d, 86),
        demultiplexer_map=entries,
    )


def _parse_signal_combiner(d: bytes) -> SignalCombiner:
    n_map = unpack_u16(d, 82)
    sources_offset = 88 + 6 * n_map
    n_src = unpack_u16(d, 86)
    entries = []
    for i in range(n_map):
        p = 88 + 6 * i
        entries.append(
            CombinerMapEntry(
                sub_signal_start=unpack_u16(d, p),
                sub_signal_count=unpack_u16(d, p + 2),
                input_index=unpack_u16(d, p + 4),
            )
        )
    return SignalCombiner(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        block_latency=unpack_u32(d, 70),
        control_latency=unpack_u32(d, 74),
        control_domain=unpack_u16(d, 78),
        combiner_map=entries,
        sources=_parse_signal_sources(d, sources_offset, n_src),
    )


def _parse_signal_multiplexer(d: bytes) -> SignalMultiplexer:
    n_map = unpack_u16(d, 82)
    sources_offset = 88 + 6 * n_map
    n_src = unpack_u16(d, 86)
    entries = []
    for i in range(n_map):
        p = 88 + 6 * i
        entries.append(
            CombinerMapEntry(
                sub_signal_start=unpack_u16(d, p),
                sub_signal_count=unpack_u16(d, p + 2),
                input_index=unpack_u16(d, p + 4),
            )
        )
    return SignalMultiplexer(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        block_latency=unpack_u32(d, 70),
        control_latency=unpack_u32(d, 74),
        control_domain=unpack_u16(d, 78),
        multiplexer_map=entries,
        sources=_parse_signal_sources(d, sources_offset, n_src),
    )


def _parse_signal_transcoder(d: bytes) -> SignalTranscoder:
    val_len = unpack_u16(d, 82)
    return SignalTranscoder(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        block_latency=unpack_u32(d, 70),
        control_latency=unpack_u32(d, 74),
        control_domain=unpack_u16(d, 78),
        control_value_type=unpack_u16(d, 80),
        signal_type=unpack_u16(d, 84),
        signal_index=unpack_u16(d, 86),
        signal_output=unpack_u16(d, 88),
        transcoder_type=unpack_eui64(d, 90),
        value_details=bytes(d[98 : 98 + val_len]),
    )


def _parse_control_block(d: bytes) -> ControlBlock:
    return ControlBlock(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        number_of_controls=unpack_u16(d, 70),
        base_control=unpack_u16(d, 72),
        final_control_index=unpack_u16(d, 74),
        signal_type=unpack_u16(d, 76),
        signal_index=unpack_u16(d, 78),
        signal_output=unpack_u16(d, 80),
    )


def _parse_timing(d: bytes) -> Timing:
    n = unpack_u16(d, 74)
    return Timing(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        algorithm=unpack_u16(d, 70),
        ptp_instance_indices=[unpack_u16(d, 76 + 2 * i) for i in range(n)],
    )


def _parse_ptp_instance(d: bytes) -> tuple[PtpInstance, int, int]:
    """Return (PtpInstance without ports, number_of_ptp_ports, base_ptp_port)."""
    pi = PtpInstance(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        clock_identity=unpack_eui64(d, 70),
        flags=unpack_u32(d, 78),
        number_of_controls=unpack_u16(d, 82),
        base_control=unpack_u16(d, 84),
    )
    number_of_ptp_ports = unpack_u16(d, 86)
    base_ptp_port = unpack_u16(d, 88)
    return pi, number_of_ptp_ports, base_ptp_port


def _parse_ptp_port(d: bytes) -> PtpPort:
    return PtpPort(
        object_name=unpack_string64(d, 4),
        localized_description=_loc(d, 68),
        port_number=unpack_u16(d, 70),
        port_type=unpack_u16(d, 72),
        flags=unpack_u32(d, 74),
        avb_interface_index=unpack_u16(d, 78),
        profile_identifier=unpack_eui48(d, 80),
    )


# --- Hierarchy assembly ---


def unflatten(descriptors, symbols) -> Entity:
    """Rebuild the hierarchical :class:`Entity` model from a flat descriptor list.

    ``descriptors`` and ``symbols`` are as returned by ``blob_reader.read_blob``
    (or ``flatten.flatten``). This is the inverse of :func:`flatten.flatten` for
    every field that ``flatten`` preserves; symbols carry a one-way CRC32 code so
    reconstructed descriptors carry ``symbol=None``.
    """
    # Group wire bytes by config, then by (type -> index -> bytes).
    by_config: dict[int, dict[int, dict[int, bytes]]] = {}
    for d in descriptors:
        by_config.setdefault(d.config_index, {}).setdefault(d.descriptor_type, {})[
            d.descriptor_index
        ] = d.wire_bytes

    # ENTITY lives at config 0, type 0, index 0.
    entity_bytes = by_config.get(0, {}).get(DESCRIPTOR_ENTITY, {}).get(0)
    if entity_bytes is None:
        raise ValueError("No ENTITY descriptor found in flat descriptor list")
    entity, configurations_count = _parse_entity(entity_bytes)

    for config_idx in range(configurations_count):
        types = by_config.get(config_idx, {})
        config = _build_configuration(config_idx, types)
        entity.configurations.append(config)

    return entity


def _indexed(types: dict[int, dict[int, bytes]], desc_type: int) -> list[bytes]:
    """Return descriptor wire bytes of ``desc_type`` ordered by descriptor index."""
    d = types.get(desc_type, {})
    return [d[i] for i in sorted(d)]


def _build_configuration(
    config_idx: int, types: dict[int, dict[int, bytes]]
) -> Configuration:
    config_bytes = types.get(DESCRIPTOR_CONFIGURATION, {}).get(config_idx)
    if config_bytes is None:
        # Some flat lists key CONFIGURATION by its own index; fall back to first.
        cfgs = types.get(DESCRIPTOR_CONFIGURATION, {})
        config_bytes = next(iter(cfgs.values())) if cfgs else b"\x00" * 74
    config = _parse_configuration(config_bytes)

    # Parse leaf/child pools keyed by descriptor index.
    controls = {
        i: _parse_control(b) for i, b in types.get(DESCRIPTOR_CONTROL, {}).items()
    }
    audio_clusters = {
        i: _parse_audio_cluster(b)
        for i, b in types.get(DESCRIPTOR_AUDIO_CLUSTER, {}).items()
    }
    audio_maps = {
        i: _parse_audio_map(b) for i, b in types.get(DESCRIPTOR_AUDIO_MAP, {}).items()
    }
    video_clusters = {
        i: _parse_video_cluster(b)
        for i, b in types.get(DESCRIPTOR_VIDEO_CLUSTER, {}).items()
    }
    video_maps = {
        i: _parse_video_map(b) for i, b in types.get(DESCRIPTOR_VIDEO_MAP, {}).items()
    }
    sensor_clusters = {
        i: _parse_sensor_cluster(b)
        for i, b in types.get(DESCRIPTOR_SENSOR_CLUSTER, {}).items()
    }
    sensor_maps = {
        i: _parse_sensor_map(b) for i, b in types.get(DESCRIPTOR_SENSOR_MAP, {}).items()
    }
    stream_ports_in = {
        i: _parse_stream_port(b)
        for i, b in types.get(DESCRIPTOR_STREAM_PORT_INPUT, {}).items()
    }
    stream_ports_out = {
        i: _parse_stream_port(b)
        for i, b in types.get(DESCRIPTOR_STREAM_PORT_OUTPUT, {}).items()
    }
    external_ports_in = {
        i: _parse_external_port(b)
        for i, b in types.get(DESCRIPTOR_EXTERNAL_PORT_INPUT, {}).items()
    }
    external_ports_out = {
        i: _parse_external_port(b)
        for i, b in types.get(DESCRIPTOR_EXTERNAL_PORT_OUTPUT, {}).items()
    }
    internal_ports_in = {
        i: _parse_internal_port(b)
        for i, b in types.get(DESCRIPTOR_INTERNAL_PORT_INPUT, {}).items()
    }
    internal_ports_out = {
        i: _parse_internal_port(b)
        for i, b in types.get(DESCRIPTOR_INTERNAL_PORT_OUTPUT, {}).items()
    }
    strings = {
        i: _parse_strings(b) for i, b in types.get(DESCRIPTOR_STRINGS, {}).items()
    }
    ptp_ports = {
        i: _parse_ptp_port(b) for i, b in types.get(DESCRIPTOR_PTP_PORT, {}).items()
    }

    referenced_controls: set[int] = set()

    def take_controls(base: int, count: int) -> list[Control]:
        result = []
        for i in range(base, base + count):
            referenced_controls.add(i)
            if i in controls:
                result.append(controls[i])
        return result

    def build_audio_port(pd: dict) -> AudioStreamPort:
        return AudioStreamPort(
            clock_domain_index=pd["clock_domain_index"],
            port_flags=pd["port_flags"],
            controls=take_controls(pd["base_control"], pd["num_controls"]),
            clusters=[
                audio_clusters[i]
                for i in range(
                    pd["base_cluster"], pd["base_cluster"] + pd["num_clusters"]
                )
                if i in audio_clusters
            ],
            maps=[
                audio_maps[i]
                for i in range(pd["base_map"], pd["base_map"] + pd["num_maps"])
                if i in audio_maps
            ],
        )

    def build_video_port(pd: dict) -> VideoStreamPort:
        return VideoStreamPort(
            clock_domain_index=pd["clock_domain_index"],
            port_flags=pd["port_flags"],
            controls=take_controls(pd["base_control"], pd["num_controls"]),
            clusters=[
                video_clusters[i]
                for i in range(
                    pd["base_cluster"], pd["base_cluster"] + pd["num_clusters"]
                )
                if i in video_clusters
            ],
            maps=[
                video_maps[i]
                for i in range(pd["base_map"], pd["base_map"] + pd["num_maps"])
                if i in video_maps
            ],
        )

    def build_sensor_port(pd: dict) -> SensorStreamPort:
        return SensorStreamPort(
            clock_domain_index=pd["clock_domain_index"],
            port_flags=pd["port_flags"],
            controls=take_controls(pd["base_control"], pd["num_controls"]),
            clusters=[
                sensor_clusters[i]
                for i in range(
                    pd["base_cluster"], pd["base_cluster"] + pd["num_clusters"]
                )
                if i in sensor_clusters
            ],
            maps=[
                sensor_maps[i]
                for i in range(pd["base_map"], pd["base_map"] + pd["num_maps"])
                if i in sensor_maps
            ],
        )

    def build_external_port(pd: dict) -> ExternalPort:
        return ExternalPort(
            clock_domain_index=pd["clock_domain_index"],
            port_flags=pd["port_flags"],
            signal_type=pd["signal_type"],
            signal_index=pd["signal_index"],
            signal_output=pd["signal_output"],
            block_latency=pd["block_latency"],
            jack_index=pd["jack_index"],
            controls=take_controls(pd["base_control"], pd["num_controls"]),
        )

    def build_internal_port(pd: dict) -> InternalPort:
        return InternalPort(
            clock_domain_index=pd["clock_domain_index"],
            port_flags=pd["port_flags"],
            signal_type=pd["signal_type"],
            signal_index=pd["signal_index"],
            signal_output=pd["signal_output"],
            block_latency=pd["block_latency"],
            internal_index=pd["internal_index"],
            controls=take_controls(pd["base_control"], pd["num_controls"]),
        )

    def fill_unit_ports(unit, rng: dict, in_ports, out_ports, build_stream_port):
        unit.controls = take_controls(rng["base_control"], rng["num_controls"])
        base = rng["base_stream_input_port"]
        for i in range(base, base + rng["num_stream_input_ports"]):
            if i in in_ports:
                unit.input_stream_ports.append(build_stream_port(in_ports[i]))
        base = rng["base_stream_output_port"]
        for i in range(base, base + rng["num_stream_output_ports"]):
            if i in out_ports:
                unit.output_stream_ports.append(build_stream_port(out_ports[i]))
        base = rng["base_external_input_port"]
        for i in range(base, base + rng["num_external_input_ports"]):
            if i in external_ports_in:
                unit.input_external_ports.append(
                    build_external_port(external_ports_in[i])
                )
        base = rng["base_external_output_port"]
        for i in range(base, base + rng["num_external_output_ports"]):
            if i in external_ports_out:
                unit.output_external_ports.append(
                    build_external_port(external_ports_out[i])
                )
        base = rng["base_internal_input_port"]
        for i in range(base, base + rng["num_internal_input_ports"]):
            if i in internal_ports_in:
                unit.input_internal_ports.append(
                    build_internal_port(internal_ports_in[i])
                )
        base = rng["base_internal_output_port"]
        for i in range(base, base + rng["num_internal_output_ports"]):
            if i in internal_ports_out:
                unit.output_internal_ports.append(
                    build_internal_port(internal_ports_out[i])
                )

    # Audio units.
    for i in sorted(types.get(DESCRIPTOR_AUDIO_UNIT, {})):
        unit, rng = _parse_audio_unit(types[DESCRIPTOR_AUDIO_UNIT][i])
        fill_unit_ports(unit, rng, stream_ports_in, stream_ports_out, build_audio_port)
        config.audio_units.append(unit)

    # Video units.
    for i in sorted(types.get(DESCRIPTOR_VIDEO_UNIT, {})):
        unit, rng = _parse_video_unit(types[DESCRIPTOR_VIDEO_UNIT][i])
        fill_unit_ports(unit, rng, stream_ports_in, stream_ports_out, build_video_port)
        config.video_units.append(unit)

    # Sensor units.
    for i in sorted(types.get(DESCRIPTOR_SENSOR_UNIT, {})):
        unit, rng = _parse_sensor_unit(types[DESCRIPTOR_SENSOR_UNIT][i])
        fill_unit_ports(unit, rng, stream_ports_in, stream_ports_out, build_sensor_port)
        config.sensor_units.append(unit)

    # Configuration-level controls = those not referenced by any unit/port.
    config.controls = [
        controls[i] for i in sorted(controls) if i not in referenced_controls
    ]

    # Direct config-level descriptor lists (ordered by descriptor index).
    config.streams_input = [
        _parse_stream(b) for b in _indexed(types, DESCRIPTOR_STREAM_INPUT)
    ]
    config.streams_output = [
        _parse_stream(b) for b in _indexed(types, DESCRIPTOR_STREAM_OUTPUT)
    ]
    config.jacks_input = [
        _parse_jack(b) for b in _indexed(types, DESCRIPTOR_JACK_INPUT)
    ]
    config.jacks_output = [
        _parse_jack(b) for b in _indexed(types, DESCRIPTOR_JACK_OUTPUT)
    ]
    config.avb_interfaces = [
        _parse_avb_interface(b) for b in _indexed(types, DESCRIPTOR_AVB_INTERFACE)
    ]
    config.clock_sources = [
        _parse_clock_source(b) for b in _indexed(types, DESCRIPTOR_CLOCK_SOURCE)
    ]
    config.clock_domains = [
        _parse_clock_domain(b) for b in _indexed(types, DESCRIPTOR_CLOCK_DOMAIN)
    ]
    config.memory_objects = [
        _parse_memory_object(b) for b in _indexed(types, DESCRIPTOR_MEMORY_OBJECT)
    ]
    config.signal_selectors = [
        _parse_signal_selector(b) for b in _indexed(types, DESCRIPTOR_SIGNAL_SELECTOR)
    ]
    config.mixers = [_parse_mixer(b) for b in _indexed(types, DESCRIPTOR_MIXER)]
    config.matrices = [_parse_matrix(b) for b in _indexed(types, DESCRIPTOR_MATRIX)]
    config.splitters = [
        _parse_signal_splitter(b) for b in _indexed(types, DESCRIPTOR_SIGNAL_SPLITTER)
    ]
    config.combiners = [
        _parse_signal_combiner(b) for b in _indexed(types, DESCRIPTOR_SIGNAL_COMBINER)
    ]
    config.demultiplexers = [
        _parse_signal_demultiplexer(b)
        for b in _indexed(types, DESCRIPTOR_SIGNAL_DEMULTIPLEXER)
    ]
    config.multiplexers = [
        _parse_signal_multiplexer(b)
        for b in _indexed(types, DESCRIPTOR_SIGNAL_MULTIPLEXER)
    ]
    config.transcoders = [
        _parse_signal_transcoder(b)
        for b in _indexed(types, DESCRIPTOR_SIGNAL_TRANSCODER)
    ]
    config.control_blocks = [
        _parse_control_block(b) for b in _indexed(types, DESCRIPTOR_CONTROL_BLOCK)
    ]
    config.timings = [_parse_timing(b) for b in _indexed(types, DESCRIPTOR_TIMING)]

    # Locales own their STRINGS via base_strings/number_of_strings.
    for i in sorted(types.get(DESCRIPTOR_LOCALE, {})):
        locale, num_strings, base_strings = _parse_locale(types[DESCRIPTOR_LOCALE][i])
        for s in range(base_strings, base_strings + num_strings):
            if s in strings:
                locale.strings_descriptors.append(strings[s])
        config.locales.append(locale)

    # PTP instances own their PTP_PORTs via base_ptp_port/number_of_ptp_ports.
    for i in sorted(types.get(DESCRIPTOR_PTP_INSTANCE, {})):
        pi, num_ports, base_port = _parse_ptp_instance(
            types[DESCRIPTOR_PTP_INSTANCE][i]
        )
        for p in range(base_port, base_port + num_ports):
            if p in ptp_ports:
                pi.ptp_ports.append(ptp_ports[p])
        config.ptp_instances.append(pi)

    return config
