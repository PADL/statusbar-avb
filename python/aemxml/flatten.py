# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Flatten an AEM entity model tree into wire-format descriptors with computed indices."""

from __future__ import annotations

import zlib
from dataclasses import dataclass

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
    DESCRIPTOR_MATRIX_SIGNAL,
    DESCRIPTOR_SIGNAL_SPLITTER,
    DESCRIPTOR_SIGNAL_COMBINER,
    DESCRIPTOR_SIGNAL_DEMULTIPLEXER,
    DESCRIPTOR_SIGNAL_MULTIPLEXER,
    DESCRIPTOR_SIGNAL_TRANSCODER,
    DESCRIPTOR_EXTERNAL_PORT_INPUT,
    DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
    DESCRIPTOR_INTERNAL_PORT_INPUT,
    DESCRIPTOR_INTERNAL_PORT_OUTPUT,
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
    MatrixSignal,
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
    pack_u8,
    pack_u16,
    pack_u32,
    pack_u64,
    pack_string64,
    pack_eui48,
    pack_eui64,
    pack_localized_string_ref,
)


@dataclass
class FlatDescriptor:
    config_index: int
    descriptor_type: int
    descriptor_index: int
    wire_bytes: bytes


@dataclass
class FlatSymbol:
    config_index: int
    descriptor_type: int
    descriptor_index: int
    symbol_code: int


def symbol_to_code(symbol: str) -> int:
    """Convert a symbol string to a 32-bit code via CRC32."""
    return zlib.crc32(symbol.encode("utf-8")) & 0xFFFFFFFF


def _pack_localized_desc(ref: LocalizedStringRef) -> bytes:
    return pack_localized_string_ref(ref.offset, ref.index)


def _serialize_entity(entity: Entity) -> bytes:
    """Serialize ENTITY descriptor (312 bytes)."""
    parts = [
        pack_u16(DESCRIPTOR_ENTITY),  # 0: descriptor_type
        pack_u16(0),  # 2: descriptor_index
        pack_eui64(entity.entity_id),  # 4: entity_id
        pack_eui64(entity.entity_model_id),  # 12: entity_model_id
        pack_u32(entity.entity_capabilities),  # 20: entity_capabilities
        pack_u16(entity.talker_stream_sources),  # 24
        pack_u16(entity.talker_capabilities),  # 26
        pack_u16(entity.listener_stream_sinks),  # 28
        pack_u16(entity.listener_capabilities),  # 30
        pack_u32(entity.controller_capabilities),  # 32
        pack_u32(entity.available_index),  # 36
        pack_eui64(entity.association_id),  # 40
        pack_string64(entity.entity_name),  # 48
        _pack_localized_desc(entity.vendor_name_string),  # 112
        _pack_localized_desc(entity.model_name_string),  # 114
        pack_string64(entity.firmware_version),  # 116
        pack_string64(entity.group_name),  # 180
        pack_string64(entity.serial_number),  # 244
        pack_u16(len(entity.configurations)),  # 308: configurations_count
        pack_u16(0),  # 310: current_configuration
    ]
    result = b"".join(parts)
    assert len(result) == 312, f"Entity descriptor must be 312 bytes, got {len(result)}"
    return result


def _serialize_configuration(
    config: Configuration, config_index: int, descriptor_counts: list[tuple[int, int]]
) -> bytes:
    """Serialize CONFIGURATION descriptor (74 + 4*N bytes)."""
    n = len(descriptor_counts)
    parts = [
        pack_u16(DESCRIPTOR_CONFIGURATION),
        pack_u16(config_index),
        pack_string64(config.object_name),
        _pack_localized_desc(config.localized_description),
        pack_u16(n),  # descriptor_counts_count
        pack_u16(74),  # descriptor_counts_offset
    ]
    # Emit the counts in ascending descriptor-type order (the C++ generator + the AEM
    # convention), so a JSON-generated blob is byte-identical to the C++ one.
    for desc_type, count in sorted(descriptor_counts):
        parts.append(pack_u16(desc_type))
        parts.append(pack_u16(count))
    return b"".join(parts)


def _serialize_audio_unit(
    unit: AudioUnit,
    desc_index: int,
    num_stream_input_ports: int,
    base_stream_input_port: int,
    num_stream_output_ports: int,
    base_stream_output_port: int,
    num_external_input_ports: int,
    base_external_input_port: int,
    num_external_output_ports: int,
    base_external_output_port: int,
    num_internal_input_ports: int,
    base_internal_input_port: int,
    num_internal_output_ports: int,
    base_internal_output_port: int,
    num_controls: int,
    base_control: int,
) -> bytes:
    """Serialize AUDIO_UNIT descriptor (144 + 4*S bytes)."""
    n_rates = len(unit.sampling_rates)
    parts = [
        pack_u16(DESCRIPTOR_AUDIO_UNIT),
        pack_u16(desc_index),
        pack_string64(unit.object_name),
        _pack_localized_desc(unit.localized_description),
        pack_u16(unit.clock_domain_index),
        pack_u16(num_stream_input_ports),
        pack_u16(base_stream_input_port),
        pack_u16(num_stream_output_ports),
        pack_u16(base_stream_output_port),
        pack_u16(num_external_input_ports),
        pack_u16(base_external_input_port),
        pack_u16(num_external_output_ports),
        pack_u16(base_external_output_port),
        pack_u16(num_internal_input_ports),
        pack_u16(base_internal_input_port),
        pack_u16(num_internal_output_ports),
        pack_u16(base_internal_output_port),
        pack_u16(num_controls),
        pack_u16(base_control),
        pack_u16(0),
        pack_u16(0),  # signal selectors
        pack_u16(0),
        pack_u16(0),  # mixers
        pack_u16(0),
        pack_u16(0),  # matrices
        pack_u16(0),
        pack_u16(0),  # splitters
        pack_u16(0),
        pack_u16(0),  # combiners
        pack_u16(0),
        pack_u16(0),  # demultiplexers
        pack_u16(0),
        pack_u16(0),  # multiplexers
        pack_u16(0),
        pack_u16(0),  # transcoders
        pack_u16(0),
        pack_u16(0),  # control blocks
        pack_u32(unit.current_sampling_rate),
        pack_u16(144),  # sampling_rates_offset
        pack_u16(n_rates),
    ]
    for rate in unit.sampling_rates:
        parts.append(pack_u32(rate))
    return b"".join(parts)


def _serialize_stream(stream: Stream, desc_type: int, desc_index: int) -> bytes:
    """Serialize STREAM_INPUT/OUTPUT descriptor (138 + 8*N + 2*R bytes)."""
    n_formats = len(stream.formats)
    n_redundant = len(stream.redundant_streams)
    formats_offset = 138
    # The redundant-streams list follows the formats list. With no redundant trailer,
    # emit 0 (as the C++ generator + the spec convention do) rather than an offset
    # pointing past the descriptor -- a strict controller (Hive) would otherwise read a
    # redundant list at that offset and mis-enumerate.
    redundant_offset = (formats_offset + 8 * n_formats) if n_redundant > 0 else 0
    parts = [
        pack_u16(desc_type),
        pack_u16(desc_index),
        pack_string64(stream.object_name),
        _pack_localized_desc(stream.localized_description),
        pack_u16(stream.clock_domain_index),
        pack_u16(stream.stream_flags),
        pack_eui64(stream.current_format),
        pack_u16(formats_offset),
        pack_u16(n_formats),
        pack_eui64(stream.backup_talker_entity_id_0),
        pack_u16(stream.backup_talker_unique_id_0),
        pack_eui64(stream.backup_talker_entity_id_1),
        pack_u16(stream.backup_talker_unique_id_1),
        pack_eui64(stream.backup_talker_entity_id_2),
        pack_u16(stream.backup_talker_unique_id_2),
        pack_eui64(stream.backedup_talker_entity_id),
        pack_u16(stream.backedup_talker_unique_id),
        pack_u16(stream.avb_interface_index),
        pack_u32(stream.buffer_length),
        pack_u16(redundant_offset),
        pack_u16(n_redundant),
        pack_u16(stream.timing),
    ]
    for fmt in stream.formats:
        parts.append(pack_eui64(fmt))
    for rs in stream.redundant_streams:
        parts.append(pack_u16(rs))
    return b"".join(parts)


def _serialize_jack(jack: Jack, desc_type: int, desc_index: int) -> bytes:
    """Serialize JACK_INPUT/OUTPUT descriptor (78 bytes)."""
    return b"".join(
        [
            pack_u16(desc_type),
            pack_u16(desc_index),
            pack_string64(jack.object_name),
            _pack_localized_desc(jack.localized_description),
            pack_u16(jack.jack_flags),
            pack_u16(jack.jack_type),
            pack_u16(0),  # number_of_controls
            pack_u16(0),  # base_control
        ]
    )


def _serialize_avb_interface(intf: AvbInterface, desc_index: int) -> bytes:
    """Serialize AVB_INTERFACE descriptor (102 bytes)."""
    return b"".join(
        [
            pack_u16(DESCRIPTOR_AVB_INTERFACE),
            pack_u16(desc_index),
            pack_string64(intf.object_name),
            _pack_localized_desc(intf.localized_description),
            pack_eui48(intf.mac_address),
            pack_u16(intf.interface_flags),
            pack_eui64(intf.clock_identity),
            pack_u8(intf.priority1),
            pack_u8(intf.clock_class),
            pack_u16(intf.offset_scaled_log_variance),
            pack_u8(intf.clock_accuracy),
            pack_u8(intf.priority2),
            pack_u8(intf.domain_number),
            pack_u8(intf.log_sync_interval),
            pack_u8(intf.log_announce_interval),
            pack_u8(intf.log_pdelay_interval),
            pack_u16(intf.port_number),
            pack_u16(intf.number_of_controls),
            pack_u16(intf.base_control),
        ]
    )


def _serialize_clock_source(cs: ClockSource, desc_index: int) -> bytes:
    """Serialize CLOCK_SOURCE descriptor (86 bytes)."""
    return b"".join(
        [
            pack_u16(DESCRIPTOR_CLOCK_SOURCE),
            pack_u16(desc_index),
            pack_string64(cs.object_name),
            _pack_localized_desc(cs.localized_description),
            pack_u16(cs.clock_source_flags),
            pack_u16(cs.clock_source_type),
            pack_eui64(cs.clock_source_identifier),
            pack_u16(cs.clock_source_location_type),
            pack_u16(cs.clock_source_location_index),
        ]
    )


def _serialize_clock_domain(cd: ClockDomain, desc_index: int) -> bytes:
    """Serialize CLOCK_DOMAIN descriptor (76 + 2*C bytes)."""
    n = len(cd.clock_sources)
    parts = [
        pack_u16(DESCRIPTOR_CLOCK_DOMAIN),
        pack_u16(desc_index),
        pack_string64(cd.object_name),
        _pack_localized_desc(cd.localized_description),
        pack_u16(cd.clock_source_index),
        pack_u16(76),  # clock_sources_offset
        pack_u16(n),
    ]
    for cs_idx in cd.clock_sources:
        parts.append(pack_u16(cs_idx))
    return b"".join(parts)


def _serialize_locale(locale: Locale, desc_index: int, base_strings: int) -> bytes:
    """Serialize LOCALE descriptor (72 bytes)."""
    return b"".join(
        [
            pack_u16(DESCRIPTOR_LOCALE),
            pack_u16(desc_index),
            pack_string64(locale.locale_identifier),
            pack_u16(len(locale.strings_descriptors)),
            pack_u16(base_strings),
        ]
    )


def _serialize_strings(sd: StringsDescriptor, desc_index: int) -> bytes:
    """Serialize STRINGS descriptor (452 bytes)."""
    parts = [
        pack_u16(DESCRIPTOR_STRINGS),
        pack_u16(desc_index),
    ]
    for i in range(7):
        s = sd.strings[i] if i < len(sd.strings) else ""
        parts.append(pack_string64(s))
    return b"".join(parts)


def _serialize_stream_port(
    port: AudioStreamPort,
    desc_type: int,
    desc_index: int,
    num_controls: int,
    base_control: int,
    num_clusters: int,
    base_cluster: int,
    num_maps: int,
    base_map: int,
) -> bytes:
    """Serialize STREAM_PORT_INPUT/OUTPUT descriptor (20 bytes)."""
    return b"".join(
        [
            pack_u16(desc_type),
            pack_u16(desc_index),
            pack_u16(port.clock_domain_index),
            pack_u16(port.port_flags),
            pack_u16(num_controls),
            pack_u16(base_control),
            pack_u16(num_clusters),
            pack_u16(base_cluster),
            pack_u16(num_maps),
            pack_u16(base_map),
        ]
    )


def _serialize_external_port(
    port: ExternalPort,
    desc_type: int,
    desc_index: int,
    num_controls: int,
    base_control: int,
) -> bytes:
    """Serialize EXTERNAL_PORT_INPUT/OUTPUT descriptor (24 bytes)."""
    return b"".join(
        [
            pack_u16(desc_type),
            pack_u16(desc_index),
            pack_u16(port.clock_domain_index),
            pack_u16(port.port_flags),
            pack_u16(num_controls),
            pack_u16(base_control),
            pack_u16(port.signal_type),
            pack_u16(port.signal_index),
            pack_u16(port.signal_output),
            pack_u32(port.block_latency),
            pack_u16(port.jack_index),
        ]
    )


def _serialize_internal_port(
    port: InternalPort,
    desc_type: int,
    desc_index: int,
    num_controls: int,
    base_control: int,
) -> bytes:
    """Serialize INTERNAL_PORT_INPUT/OUTPUT descriptor (24 bytes)."""
    return b"".join(
        [
            pack_u16(desc_type),
            pack_u16(desc_index),
            pack_u16(port.clock_domain_index),
            pack_u16(port.port_flags),
            pack_u16(num_controls),
            pack_u16(base_control),
            pack_u16(port.signal_type),
            pack_u16(port.signal_index),
            pack_u16(port.signal_output),
            pack_u32(port.block_latency),
            pack_u16(port.internal_index),
        ]
    )


def _serialize_audio_cluster(cluster: AudioCluster, desc_index: int) -> bytes:
    """Serialize AUDIO_CLUSTER descriptor (90 bytes)."""
    return b"".join(
        [
            pack_u16(DESCRIPTOR_AUDIO_CLUSTER),
            pack_u16(desc_index),
            pack_string64(cluster.object_name),
            _pack_localized_desc(cluster.localized_description),
            pack_u16(cluster.signal_type),
            pack_u16(cluster.signal_index),
            pack_u16(cluster.signal_output),
            pack_u32(cluster.path_latency),
            pack_u32(cluster.block_latency),
            pack_u16(cluster.channel_count),
            pack_u8(cluster.format),
            pack_u8(cluster.aes3_data_type_reference),
            pack_u16(cluster.aes3_data_type),
        ]
    )


def _serialize_audio_map(am: AudioMap, desc_index: int) -> bytes:
    """Serialize AUDIO_MAP descriptor (8 + 8*N bytes)."""
    n = len(am.mappings)
    parts = [
        pack_u16(DESCRIPTOR_AUDIO_MAP),
        pack_u16(desc_index),
        pack_u16(8),  # mappings_offset
        pack_u16(n),
    ]
    for m in am.mappings:
        parts.append(pack_u16(m.stream_index))
        parts.append(pack_u16(m.stream_channel))
        parts.append(pack_u16(m.cluster_offset))
        parts.append(pack_u16(m.cluster_channel))
    return b"".join(parts)


def _serialize_control(ctrl: Control, desc_index: int) -> bytes:
    """Serialize CONTROL descriptor (104 + L bytes)."""
    val_len = len(ctrl.value_details)
    parts = [
        pack_u16(DESCRIPTOR_CONTROL),
        pack_u16(desc_index),
        pack_string64(ctrl.object_name),
        _pack_localized_desc(ctrl.localized_description),
        pack_u32(ctrl.block_latency),
        pack_u32(ctrl.control_latency),
        pack_u16(ctrl.control_domain),
        pack_u16(ctrl.control_value_type),
        pack_eui64(ctrl.control_type),
        pack_u32(ctrl.reset_time),
        pack_u16(104),  # values_offset
        pack_u16(val_len),
        pack_u16(ctrl.signal_type),
        pack_u16(ctrl.signal_index),
        pack_u16(ctrl.signal_output),
    ]
    if val_len > 0:
        parts.append(ctrl.value_details)
    return b"".join(parts)


def _serialize_signal_selector(sel: SignalSelector, desc_index: int) -> bytes:
    """Serialize SIGNAL_SELECTOR descriptor (96 + 6*N bytes)."""
    n = len(sel.sources)
    parts = [
        pack_u16(DESCRIPTOR_SIGNAL_SELECTOR),
        pack_u16(desc_index),
        pack_string64(sel.object_name),
        _pack_localized_desc(sel.localized_description),
        pack_u32(sel.block_latency),
        pack_u32(sel.control_latency),
        pack_u16(sel.control_domain),
        pack_u16(96),  # sources_offset
        pack_u16(n),
        pack_u16(sel.current_signal_type),
        pack_u16(sel.current_signal_index),
        pack_u16(sel.current_signal_output),
        pack_u16(sel.default_signal_type),
        pack_u16(sel.default_signal_index),
        pack_u16(sel.default_signal_output),
    ]
    for src in sel.sources:
        parts.append(pack_u16(src.signal_type))
        parts.append(pack_u16(src.signal_index))
        parts.append(pack_u16(src.signal_output))
    return b"".join(parts)


def _serialize_mixer(mixer: Mixer, desc_index: int) -> bytes:
    """Serialize MIXER descriptor (88 + 6*N + L bytes)."""
    n = len(mixer.sources)
    val_len = len(mixer.value_details)
    value_offset = 88 + 6 * n
    parts = [
        pack_u16(DESCRIPTOR_MIXER),
        pack_u16(desc_index),
        pack_string64(mixer.object_name),
        _pack_localized_desc(mixer.localized_description),
        pack_u32(mixer.block_latency),
        pack_u32(mixer.control_latency),
        pack_u16(mixer.control_domain),
        pack_u16(mixer.control_value_type),
        pack_u16(88),  # sources_offset
        pack_u16(n),
        pack_u16(value_offset),
    ]
    for src in mixer.sources:
        parts.append(pack_u16(src.signal_type))
        parts.append(pack_u16(src.signal_index))
        parts.append(pack_u16(src.signal_output))
    if val_len > 0:
        parts.append(mixer.value_details)
    return b"".join(parts)


def _serialize_matrix(matrix: Matrix, desc_index: int) -> bytes:
    """Serialize MATRIX descriptor (102 + L bytes)."""
    val_len = len(matrix.value_details)
    parts = [
        pack_u16(DESCRIPTOR_MATRIX),
        pack_u16(desc_index),
        pack_string64(matrix.object_name),
        _pack_localized_desc(matrix.localized_description),
        pack_u32(matrix.block_latency),
        pack_u32(matrix.control_latency),
        pack_u16(matrix.control_domain),
        pack_u16(matrix.control_value_type),
        pack_eui64(matrix.control_type),
        pack_u16(matrix.width),
        pack_u16(matrix.height),
        pack_u16(102),  # values_offset
        pack_u16(matrix.number_of_values),
        pack_u16(matrix.number_of_sources),
        pack_u16(matrix.base_source),
    ]
    if val_len > 0:
        parts.append(matrix.value_details)
    return b"".join(parts)


def _serialize_matrix_signal(ms: MatrixSignal, desc_index: int) -> bytes:
    """Serialize MATRIX_SIGNAL descriptor (8 + 6*N bytes)."""
    n = len(ms.signals)
    parts = [
        pack_u16(DESCRIPTOR_MATRIX_SIGNAL),
        pack_u16(desc_index),
        pack_u16(8),  # signals_offset
        pack_u16(n),
    ]
    for sig in ms.signals:
        parts.append(pack_u16(sig.signal_type))
        parts.append(pack_u16(sig.signal_index))
        parts.append(pack_u16(sig.signal_output))
    return b"".join(parts)


def _serialize_signal_splitter(sp: SignalSplitter, desc_index: int) -> bytes:
    """Serialize SIGNAL_SPLITTER descriptor (92 + 6*N bytes)."""
    n = len(sp.splitter_map)
    parts = [
        pack_u16(DESCRIPTOR_SIGNAL_SPLITTER),
        pack_u16(desc_index),
        pack_string64(sp.object_name),
        _pack_localized_desc(sp.localized_description),
        pack_u32(sp.block_latency),
        pack_u32(sp.control_latency),
        pack_u16(sp.control_domain),
        pack_u16(sp.signal_type),
        pack_u16(sp.signal_index),
        pack_u16(sp.signal_output),
        pack_u16(sp.number_of_outputs),
        pack_u16(n),
        pack_u16(92),  # splitter_map_offset
    ]
    for entry in sp.splitter_map:
        parts.append(pack_u16(entry.sub_signal_start))
        parts.append(pack_u16(entry.sub_signal_count))
        parts.append(pack_u16(entry.output_index))
    return b"".join(parts)


def _serialize_signal_combiner(comb: SignalCombiner, desc_index: int) -> bytes:
    """Serialize SIGNAL_COMBINER descriptor (88 + 6*M + 6*N bytes)."""
    n_map = len(comb.combiner_map)
    n_src = len(comb.sources)
    sources_offset = 88 + 6 * n_map
    parts = [
        pack_u16(DESCRIPTOR_SIGNAL_COMBINER),
        pack_u16(desc_index),
        pack_string64(comb.object_name),
        _pack_localized_desc(comb.localized_description),
        pack_u32(comb.block_latency),
        pack_u32(comb.control_latency),
        pack_u16(comb.control_domain),
        pack_u16(n_map),
        pack_u16(88),  # combiner_map_offset
        pack_u16(sources_offset),
        pack_u16(n_src),
    ]
    for entry in comb.combiner_map:
        parts.append(pack_u16(entry.sub_signal_start))
        parts.append(pack_u16(entry.sub_signal_count))
        parts.append(pack_u16(entry.input_index))
    for src in comb.sources:
        parts.append(pack_u16(src.signal_type))
        parts.append(pack_u16(src.signal_index))
        parts.append(pack_u16(src.signal_output))
    return b"".join(parts)


def _serialize_signal_demultiplexer(
    demux: SignalDemultiplexer, desc_index: int
) -> bytes:
    """Serialize SIGNAL_DEMULTIPLEXER descriptor (92 + 6*N bytes)."""
    n = len(demux.demultiplexer_map)
    parts = [
        pack_u16(DESCRIPTOR_SIGNAL_DEMULTIPLEXER),
        pack_u16(desc_index),
        pack_string64(demux.object_name),
        _pack_localized_desc(demux.localized_description),
        pack_u32(demux.block_latency),
        pack_u32(demux.control_latency),
        pack_u16(demux.control_domain),
        pack_u16(demux.signal_type),
        pack_u16(demux.signal_index),
        pack_u16(demux.signal_output),
        pack_u16(demux.number_of_outputs),
        pack_u16(n),
        pack_u16(92),  # demultiplexer_map_offset
    ]
    for entry in demux.demultiplexer_map:
        parts.append(pack_u16(entry.sub_signal_start))
        parts.append(pack_u16(entry.sub_signal_count))
        parts.append(pack_u16(entry.output_index))
    return b"".join(parts)


def _serialize_signal_multiplexer(mux: SignalMultiplexer, desc_index: int) -> bytes:
    """Serialize SIGNAL_MULTIPLEXER descriptor (88 + 6*M + 6*N bytes)."""
    n_map = len(mux.multiplexer_map)
    n_src = len(mux.sources)
    sources_offset = 88 + 6 * n_map
    parts = [
        pack_u16(DESCRIPTOR_SIGNAL_MULTIPLEXER),
        pack_u16(desc_index),
        pack_string64(mux.object_name),
        _pack_localized_desc(mux.localized_description),
        pack_u32(mux.block_latency),
        pack_u32(mux.control_latency),
        pack_u16(mux.control_domain),
        pack_u16(n_map),
        pack_u16(88),  # multiplexer_map_offset
        pack_u16(sources_offset),
        pack_u16(n_src),
    ]
    for entry in mux.multiplexer_map:
        parts.append(pack_u16(entry.sub_signal_start))
        parts.append(pack_u16(entry.sub_signal_count))
        parts.append(pack_u16(entry.input_index))
    for src in mux.sources:
        parts.append(pack_u16(src.signal_type))
        parts.append(pack_u16(src.signal_index))
        parts.append(pack_u16(src.signal_output))
    return b"".join(parts)


def _serialize_signal_transcoder(tc: SignalTranscoder, desc_index: int) -> bytes:
    """Serialize SIGNAL_TRANSCODER descriptor (100 + L bytes)."""
    val_len = len(tc.value_details)
    parts = [
        pack_u16(DESCRIPTOR_SIGNAL_TRANSCODER),
        pack_u16(desc_index),
        pack_string64(tc.object_name),
        _pack_localized_desc(tc.localized_description),
        pack_u32(tc.block_latency),
        pack_u32(tc.control_latency),
        pack_u16(tc.control_domain),
        pack_u16(tc.control_value_type),
        pack_u16(100),  # values_offset
        pack_u16(val_len),
        pack_u16(tc.signal_type),
        pack_u16(tc.signal_index),
        pack_u16(tc.signal_output),
        pack_eui64(tc.transcoder_type),
    ]
    if val_len > 0:
        parts.append(tc.value_details)
    return b"".join(parts)


def _serialize_control_block(cb: ControlBlock, desc_index: int) -> bytes:
    """Serialize CONTROL_BLOCK descriptor (82 bytes)."""
    return b"".join(
        [
            pack_u16(DESCRIPTOR_CONTROL_BLOCK),
            pack_u16(desc_index),
            pack_string64(cb.object_name),
            _pack_localized_desc(cb.localized_description),
            pack_u16(cb.number_of_controls),
            pack_u16(cb.base_control),
            pack_u16(cb.final_control_index),
            pack_u16(cb.signal_type),
            pack_u16(cb.signal_index),
            pack_u16(cb.signal_output),
        ]
    )


def _serialize_memory_object(mo: MemoryObject, desc_index: int) -> bytes:
    """Serialize MEMORY_OBJECT descriptor (108 bytes)."""
    return b"".join(
        [
            pack_u16(DESCRIPTOR_MEMORY_OBJECT),
            pack_u16(desc_index),
            pack_string64(mo.object_name),
            _pack_localized_desc(mo.localized_description),
            pack_u16(mo.memory_object_type),
            pack_u16(mo.target_descriptor_type),
            pack_u16(mo.target_descriptor_index),
            pack_u64(mo.start_address),
            pack_u64(mo.maximum_length),
            pack_u64(mo.length),
            pack_u64(mo.maximum_segment_length),
        ]
    )


def _serialize_video_unit(
    unit: VideoUnit,
    desc_index: int,
    num_stream_input_ports: int,
    base_stream_input_port: int,
    num_stream_output_ports: int,
    base_stream_output_port: int,
    num_external_input_ports: int,
    base_external_input_port: int,
    num_external_output_ports: int,
    base_external_output_port: int,
    num_internal_input_ports: int,
    base_internal_input_port: int,
    num_internal_output_ports: int,
    base_internal_output_port: int,
    num_controls: int,
    base_control: int,
) -> bytes:
    """Serialize VIDEO_UNIT descriptor (136 bytes, no sampling rates)."""
    parts = [
        pack_u16(DESCRIPTOR_VIDEO_UNIT),
        pack_u16(desc_index),
        pack_string64(unit.object_name),
        _pack_localized_desc(unit.localized_description),
        pack_u16(unit.clock_domain_index),
        pack_u16(num_stream_input_ports),
        pack_u16(base_stream_input_port),
        pack_u16(num_stream_output_ports),
        pack_u16(base_stream_output_port),
        pack_u16(num_external_input_ports),
        pack_u16(base_external_input_port),
        pack_u16(num_external_output_ports),
        pack_u16(base_external_output_port),
        pack_u16(num_internal_input_ports),
        pack_u16(base_internal_input_port),
        pack_u16(num_internal_output_ports),
        pack_u16(base_internal_output_port),
        pack_u16(num_controls),
        pack_u16(base_control),
        pack_u16(0),
        pack_u16(0),  # signal selectors
        pack_u16(0),
        pack_u16(0),  # mixers
        pack_u16(0),
        pack_u16(0),  # matrices
        pack_u16(0),
        pack_u16(0),  # splitters
        pack_u16(0),
        pack_u16(0),  # combiners
        pack_u16(0),
        pack_u16(0),  # demultiplexers
        pack_u16(0),
        pack_u16(0),  # multiplexers
        pack_u16(0),
        pack_u16(0),  # transcoders
        pack_u16(0),
        pack_u16(0),  # control blocks
    ]
    return b"".join(parts)


def _serialize_sensor_unit(
    unit: SensorUnit,
    desc_index: int,
    num_stream_input_ports: int,
    base_stream_input_port: int,
    num_stream_output_ports: int,
    base_stream_output_port: int,
    num_external_input_ports: int,
    base_external_input_port: int,
    num_external_output_ports: int,
    base_external_output_port: int,
    num_internal_input_ports: int,
    base_internal_input_port: int,
    num_internal_output_ports: int,
    base_internal_output_port: int,
    num_controls: int,
    base_control: int,
) -> bytes:
    """Serialize SENSOR_UNIT descriptor (136 bytes, no sampling rates)."""
    parts = [
        pack_u16(DESCRIPTOR_SENSOR_UNIT),
        pack_u16(desc_index),
        pack_string64(unit.object_name),
        _pack_localized_desc(unit.localized_description),
        pack_u16(unit.clock_domain_index),
        pack_u16(num_stream_input_ports),
        pack_u16(base_stream_input_port),
        pack_u16(num_stream_output_ports),
        pack_u16(base_stream_output_port),
        pack_u16(num_external_input_ports),
        pack_u16(base_external_input_port),
        pack_u16(num_external_output_ports),
        pack_u16(base_external_output_port),
        pack_u16(num_internal_input_ports),
        pack_u16(base_internal_input_port),
        pack_u16(num_internal_output_ports),
        pack_u16(base_internal_output_port),
        pack_u16(num_controls),
        pack_u16(base_control),
        pack_u16(0),
        pack_u16(0),  # signal selectors
        pack_u16(0),
        pack_u16(0),  # mixers
        pack_u16(0),
        pack_u16(0),  # matrices
        pack_u16(0),
        pack_u16(0),  # splitters
        pack_u16(0),
        pack_u16(0),  # combiners
        pack_u16(0),
        pack_u16(0),  # demultiplexers
        pack_u16(0),
        pack_u16(0),  # multiplexers
        pack_u16(0),
        pack_u16(0),  # transcoders
        pack_u16(0),
        pack_u16(0),  # control blocks
    ]
    return b"".join(parts)


def _serialize_video_cluster(cluster: VideoCluster, desc_index: int) -> bytes:
    """Serialize VIDEO_CLUSTER descriptor (variable bytes)."""
    n_fmts = len(cluster.supported_format_specifics)
    fmt_offset = 86  # after fixed fields
    parts = [
        pack_u16(DESCRIPTOR_VIDEO_CLUSTER),
        pack_u16(desc_index),
        pack_string64(cluster.object_name),
        _pack_localized_desc(cluster.localized_description),
        pack_u16(cluster.signal_type),
        pack_u16(cluster.signal_index),
        pack_u16(cluster.signal_output),
        pack_u32(cluster.path_latency),
        pack_u32(cluster.block_latency),
        pack_u16(cluster.format),
        pack_u16(fmt_offset),
        pack_u16(n_fmts),
    ]
    if cluster.current_format_specific:
        parts.append(cluster.current_format_specific)
    for fmt in cluster.supported_format_specifics:
        parts.append(fmt)
    return b"".join(parts)


def _serialize_sensor_cluster(cluster: SensorCluster, desc_index: int) -> bytes:
    """Serialize SENSOR_CLUSTER descriptor (variable bytes)."""
    n_fmts = len(cluster.supported_format_specifics)
    fmt_offset = 86  # after fixed fields
    parts = [
        pack_u16(DESCRIPTOR_SENSOR_CLUSTER),
        pack_u16(desc_index),
        pack_string64(cluster.object_name),
        _pack_localized_desc(cluster.localized_description),
        pack_u16(cluster.signal_type),
        pack_u16(cluster.signal_index),
        pack_u16(cluster.signal_output),
        pack_u32(cluster.path_latency),
        pack_u32(cluster.block_latency),
        pack_u16(cluster.format),
        pack_u16(fmt_offset),
        pack_u16(n_fmts),
    ]
    if cluster.current_format_specific:
        parts.append(cluster.current_format_specific)
    for fmt in cluster.supported_format_specifics:
        parts.append(fmt)
    return b"".join(parts)


def _serialize_video_map(vm: VideoMap, desc_index: int) -> bytes:
    """Serialize VIDEO_MAP descriptor (8 + 8*N bytes)."""
    n = len(vm.mappings)
    parts = [
        pack_u16(DESCRIPTOR_VIDEO_MAP),
        pack_u16(desc_index),
        pack_u16(8),  # mappings_offset
        pack_u16(n),
    ]
    for m in vm.mappings:
        parts.append(pack_u16(m.stream_index))
        parts.append(pack_u16(m.stream_channel))
        parts.append(pack_u16(m.cluster_offset))
        parts.append(pack_u16(m.cluster_channel))
    return b"".join(parts)


def _serialize_sensor_map(sm: SensorMap, desc_index: int) -> bytes:
    """Serialize SENSOR_MAP descriptor (8 + 8*N bytes)."""
    n = len(sm.mappings)
    parts = [
        pack_u16(DESCRIPTOR_SENSOR_MAP),
        pack_u16(desc_index),
        pack_u16(8),  # mappings_offset
        pack_u16(n),
    ]
    for m in sm.mappings:
        parts.append(pack_u16(m.stream_index))
        parts.append(pack_u16(m.stream_channel))
        parts.append(pack_u16(m.cluster_offset))
        parts.append(pack_u16(m.cluster_channel))
    return b"".join(parts)


def _serialize_timing(timing: Timing, desc_index: int) -> bytes:
    """Serialize TIMING descriptor (76 + 2*N bytes)."""
    n = len(timing.ptp_instance_indices)
    parts = [
        pack_u16(DESCRIPTOR_TIMING),
        pack_u16(desc_index),
        pack_string64(timing.object_name),
        _pack_localized_desc(timing.localized_description),
        pack_u16(timing.algorithm),
        pack_u16(76),  # ptp_instances_offset
        pack_u16(n),
    ]
    for idx in timing.ptp_instance_indices:
        parts.append(pack_u16(idx))
    return b"".join(parts)


def _serialize_ptp_instance(
    pi: PtpInstance, desc_index: int, num_ptp_ports: int, base_ptp_port: int
) -> bytes:
    """Serialize PTP_INSTANCE descriptor (90 bytes)."""
    return b"".join(
        [
            pack_u16(DESCRIPTOR_PTP_INSTANCE),
            pack_u16(desc_index),
            pack_string64(pi.object_name),
            _pack_localized_desc(pi.localized_description),
            pack_eui64(pi.clock_identity),
            pack_u32(pi.flags),
            pack_u16(pi.number_of_controls),
            pack_u16(pi.base_control),
            pack_u16(num_ptp_ports),
            pack_u16(base_ptp_port),
        ]
    )


def _serialize_ptp_port(pp: PtpPort, desc_index: int) -> bytes:
    """Serialize PTP_PORT descriptor (86 bytes)."""
    return b"".join(
        [
            pack_u16(DESCRIPTOR_PTP_PORT),
            pack_u16(desc_index),
            pack_string64(pp.object_name),
            _pack_localized_desc(pp.localized_description),
            pack_u16(pp.port_number),
            pack_u16(pp.port_type),
            pack_u32(pp.flags),
            pack_u16(pp.avb_interface_index),
            pack_eui48(pp.profile_identifier),
        ]
    )


def _add_symbol(
    symbols: list[FlatSymbol],
    config_index: int,
    desc_type: int,
    desc_index: int,
    symbol: str | None,
) -> None:
    if symbol is not None:
        symbols.append(
            FlatSymbol(config_index, desc_type, desc_index, symbol_to_code(symbol))
        )


def flatten(entity: Entity) -> tuple[list[FlatDescriptor], list[FlatSymbol]]:
    """Flatten an entity model tree into wire-format descriptors with computed indices.

    Returns (descriptors, symbols) sorted by (config, type, index).
    """
    descriptors: list[FlatDescriptor] = []
    symbols: list[FlatSymbol] = []

    # Entity descriptor is always config=0, type=0, index=0
    descriptors.append(
        FlatDescriptor(0, DESCRIPTOR_ENTITY, 0, _serialize_entity(entity))
    )

    for config_idx, config in enumerate(entity.configurations):
        # Track indices for each descriptor type within this configuration
        control_index = 0
        stream_port_input_index = 0
        stream_port_output_index = 0
        external_port_input_index = 0
        external_port_output_index = 0
        internal_port_input_index = 0
        internal_port_output_index = 0
        audio_cluster_index = 0
        audio_map_index = 0
        video_cluster_index = 0
        video_map_index = 0
        sensor_cluster_index = 0
        sensor_map_index = 0
        strings_index = 0
        ptp_port_index = 0

        # Collect descriptor counts for the configuration descriptor
        descriptor_counts: list[tuple[int, int]] = []

        def add_count(desc_type: int, count: int) -> None:
            if count > 0:
                descriptor_counts.append((desc_type, count))

        # Count top-level descriptors
        add_count(DESCRIPTOR_AUDIO_UNIT, len(config.audio_units))
        add_count(DESCRIPTOR_VIDEO_UNIT, len(config.video_units))
        add_count(DESCRIPTOR_SENSOR_UNIT, len(config.sensor_units))
        add_count(DESCRIPTOR_STREAM_INPUT, len(config.streams_input))
        add_count(DESCRIPTOR_STREAM_OUTPUT, len(config.streams_output))
        add_count(DESCRIPTOR_JACK_INPUT, len(config.jacks_input))
        add_count(DESCRIPTOR_JACK_OUTPUT, len(config.jacks_output))
        add_count(DESCRIPTOR_AVB_INTERFACE, len(config.avb_interfaces))
        add_count(DESCRIPTOR_CLOCK_SOURCE, len(config.clock_sources))
        add_count(DESCRIPTOR_MEMORY_OBJECT, len(config.memory_objects))
        add_count(DESCRIPTOR_CONTROL, _count_all_controls(config))
        add_count(DESCRIPTOR_SIGNAL_SELECTOR, len(config.signal_selectors))
        add_count(DESCRIPTOR_MIXER, len(config.mixers))
        add_count(DESCRIPTOR_MATRIX, len(config.matrices))
        add_count(DESCRIPTOR_SIGNAL_SPLITTER, len(config.splitters))
        add_count(DESCRIPTOR_SIGNAL_COMBINER, len(config.combiners))
        add_count(DESCRIPTOR_SIGNAL_DEMULTIPLEXER, len(config.demultiplexers))
        add_count(DESCRIPTOR_SIGNAL_MULTIPLEXER, len(config.multiplexers))
        add_count(DESCRIPTOR_SIGNAL_TRANSCODER, len(config.transcoders))
        add_count(DESCRIPTOR_CONTROL_BLOCK, len(config.control_blocks))
        add_count(DESCRIPTOR_LOCALE, len(config.locales))
        add_count(DESCRIPTOR_CLOCK_DOMAIN, len(config.clock_domains))
        add_count(DESCRIPTOR_TIMING, len(config.timings))
        add_count(DESCRIPTOR_PTP_INSTANCE, len(config.ptp_instances))

        # Count stream ports, external/internal ports, clusters, maps across all units
        all_units = (
            list(config.audio_units)
            + list(config.video_units)
            + list(config.sensor_units)
        )
        total_stream_port_inputs = sum(len(u.input_stream_ports) for u in all_units)
        total_stream_port_outputs = sum(len(u.output_stream_ports) for u in all_units)
        total_external_port_inputs = sum(len(u.input_external_ports) for u in all_units)
        total_external_port_outputs = sum(
            len(u.output_external_ports) for u in all_units
        )
        total_internal_port_inputs = sum(len(u.input_internal_ports) for u in all_units)
        total_internal_port_outputs = sum(
            len(u.output_internal_ports) for u in all_units
        )
        total_audio_clusters = sum(
            len(c)
            for u in config.audio_units
            for p in (u.input_stream_ports + u.output_stream_ports)
            for c in [p.clusters]
        )
        total_audio_maps = sum(
            len(m)
            for u in config.audio_units
            for p in (u.input_stream_ports + u.output_stream_ports)
            for m in [p.maps]
        )
        total_video_clusters = sum(
            len(c)
            for u in config.video_units
            for p in (u.input_stream_ports + u.output_stream_ports)
            for c in [p.clusters]
        )
        total_video_maps = sum(
            len(m)
            for u in config.video_units
            for p in (u.input_stream_ports + u.output_stream_ports)
            for m in [p.maps]
        )
        total_sensor_clusters = sum(
            len(c)
            for u in config.sensor_units
            for p in (u.input_stream_ports + u.output_stream_ports)
            for c in [p.clusters]
        )
        total_sensor_maps = sum(
            len(m)
            for u in config.sensor_units
            for p in (u.input_stream_ports + u.output_stream_ports)
            for m in [p.maps]
        )
        total_strings = sum(len(loc.strings_descriptors) for loc in config.locales)
        total_ptp_ports = sum(len(pi.ptp_ports) for pi in config.ptp_instances)

        add_count(DESCRIPTOR_STREAM_PORT_INPUT, total_stream_port_inputs)
        add_count(DESCRIPTOR_STREAM_PORT_OUTPUT, total_stream_port_outputs)
        add_count(DESCRIPTOR_EXTERNAL_PORT_INPUT, total_external_port_inputs)
        add_count(DESCRIPTOR_EXTERNAL_PORT_OUTPUT, total_external_port_outputs)
        add_count(DESCRIPTOR_INTERNAL_PORT_INPUT, total_internal_port_inputs)
        add_count(DESCRIPTOR_INTERNAL_PORT_OUTPUT, total_internal_port_outputs)
        add_count(DESCRIPTOR_AUDIO_CLUSTER, total_audio_clusters)
        add_count(DESCRIPTOR_VIDEO_CLUSTER, total_video_clusters)
        add_count(DESCRIPTOR_SENSOR_CLUSTER, total_sensor_clusters)
        add_count(DESCRIPTOR_AUDIO_MAP, total_audio_maps)
        add_count(DESCRIPTOR_VIDEO_MAP, total_video_maps)
        add_count(DESCRIPTOR_SENSOR_MAP, total_sensor_maps)
        add_count(DESCRIPTOR_STRINGS, total_strings)
        add_count(DESCRIPTOR_PTP_PORT, total_ptp_ports)

        # Serialize configuration descriptor
        descriptors.append(
            FlatDescriptor(
                config_idx,
                DESCRIPTOR_CONFIGURATION,
                config_idx,
                _serialize_configuration(config, config_idx, descriptor_counts),
            )
        )
        _add_symbol(
            symbols, config_idx, DESCRIPTOR_CONFIGURATION, config_idx, config.symbol
        )

        # Configuration-level controls
        config_base_control = control_index
        for ctrl in config.controls:
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_CONTROL,
                    control_index,
                    _serialize_control(ctrl, control_index),
                )
            )
            _add_symbol(
                symbols, config_idx, DESCRIPTOR_CONTROL, control_index, ctrl.symbol
            )
            control_index += 1

        # Streams
        for i, stream in enumerate(config.streams_input):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_STREAM_INPUT,
                    i,
                    _serialize_stream(stream, DESCRIPTOR_STREAM_INPUT, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_STREAM_INPUT, i, stream.symbol)

        for i, stream in enumerate(config.streams_output):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_STREAM_OUTPUT,
                    i,
                    _serialize_stream(stream, DESCRIPTOR_STREAM_OUTPUT, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_STREAM_OUTPUT, i, stream.symbol)

        # Jacks
        for i, jack in enumerate(config.jacks_input):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_JACK_INPUT,
                    i,
                    _serialize_jack(jack, DESCRIPTOR_JACK_INPUT, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_JACK_INPUT, i, jack.symbol)

        for i, jack in enumerate(config.jacks_output):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_JACK_OUTPUT,
                    i,
                    _serialize_jack(jack, DESCRIPTOR_JACK_OUTPUT, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_JACK_OUTPUT, i, jack.symbol)

        # AVB Interfaces
        for i, intf in enumerate(config.avb_interfaces):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_AVB_INTERFACE,
                    i,
                    _serialize_avb_interface(intf, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_AVB_INTERFACE, i, intf.symbol)

        # Clock Sources
        for i, cs in enumerate(config.clock_sources):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_CLOCK_SOURCE,
                    i,
                    _serialize_clock_source(cs, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_CLOCK_SOURCE, i, cs.symbol)

        # Clock Domains
        for i, cd in enumerate(config.clock_domains):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_CLOCK_DOMAIN,
                    i,
                    _serialize_clock_domain(cd, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_CLOCK_DOMAIN, i, cd.symbol)

        # Locales and Strings
        for i, locale in enumerate(config.locales):
            base_strings = strings_index
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_LOCALE,
                    i,
                    _serialize_locale(locale, i, base_strings),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_LOCALE, i, locale.symbol)

            for sd in locale.strings_descriptors:
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_STRINGS,
                        strings_index,
                        _serialize_strings(sd, strings_index),
                    )
                )
                _add_symbol(
                    symbols, config_idx, DESCRIPTOR_STRINGS, strings_index, sd.symbol
                )
                strings_index += 1

        # Signal Selectors
        for i, sel in enumerate(config.signal_selectors):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_SIGNAL_SELECTOR,
                    i,
                    _serialize_signal_selector(sel, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_SIGNAL_SELECTOR, i, sel.symbol)

        # Mixers
        for i, mixer in enumerate(config.mixers):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_MIXER,
                    i,
                    _serialize_mixer(mixer, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_MIXER, i, mixer.symbol)

        # Matrices
        for i, matrix in enumerate(config.matrices):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_MATRIX,
                    i,
                    _serialize_matrix(matrix, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_MATRIX, i, matrix.symbol)

        # Signal Splitters
        for i, sp in enumerate(config.splitters):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_SIGNAL_SPLITTER,
                    i,
                    _serialize_signal_splitter(sp, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_SIGNAL_SPLITTER, i, sp.symbol)

        # Signal Combiners
        for i, comb in enumerate(config.combiners):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_SIGNAL_COMBINER,
                    i,
                    _serialize_signal_combiner(comb, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_SIGNAL_COMBINER, i, comb.symbol)

        # Signal Demultiplexers
        for i, demux in enumerate(config.demultiplexers):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_SIGNAL_DEMULTIPLEXER,
                    i,
                    _serialize_signal_demultiplexer(demux, i),
                )
            )
            _add_symbol(
                symbols, config_idx, DESCRIPTOR_SIGNAL_DEMULTIPLEXER, i, demux.symbol
            )

        # Signal Multiplexers
        for i, mux in enumerate(config.multiplexers):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_SIGNAL_MULTIPLEXER,
                    i,
                    _serialize_signal_multiplexer(mux, i),
                )
            )
            _add_symbol(
                symbols, config_idx, DESCRIPTOR_SIGNAL_MULTIPLEXER, i, mux.symbol
            )

        # Signal Transcoders
        for i, tc in enumerate(config.transcoders):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_SIGNAL_TRANSCODER,
                    i,
                    _serialize_signal_transcoder(tc, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_SIGNAL_TRANSCODER, i, tc.symbol)

        # Control Blocks
        for i, cb in enumerate(config.control_blocks):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_CONTROL_BLOCK,
                    i,
                    _serialize_control_block(cb, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_CONTROL_BLOCK, i, cb.symbol)

        # Memory Objects
        for i, mo in enumerate(config.memory_objects):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_MEMORY_OBJECT,
                    i,
                    _serialize_memory_object(mo, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_MEMORY_OBJECT, i, mo.symbol)

        # Timings
        for i, timing in enumerate(config.timings):
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_TIMING,
                    i,
                    _serialize_timing(timing, i),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_TIMING, i, timing.symbol)

        # PTP Instances and their PTP Ports
        for i, pi in enumerate(config.ptp_instances):
            base_ptp_port = ptp_port_index
            for pp in pi.ptp_ports:
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_PTP_PORT,
                        ptp_port_index,
                        _serialize_ptp_port(pp, ptp_port_index),
                    )
                )
                _add_symbol(
                    symbols, config_idx, DESCRIPTOR_PTP_PORT, ptp_port_index, pp.symbol
                )
                ptp_port_index += 1

            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_PTP_INSTANCE,
                    i,
                    _serialize_ptp_instance(pi, i, len(pi.ptp_ports), base_ptp_port),
                )
            )
            _add_symbol(symbols, config_idx, DESCRIPTOR_PTP_INSTANCE, i, pi.symbol)

        # Audio Units and their children
        for unit_idx, unit in enumerate(config.audio_units):
            unit_base_control = control_index

            # Unit-level controls
            for ctrl in unit.controls:
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        _serialize_control(ctrl, control_index),
                    )
                )
                _add_symbol(
                    symbols, config_idx, DESCRIPTOR_CONTROL, control_index, ctrl.symbol
                )
                control_index += 1

            base_spi = stream_port_input_index
            base_spo = stream_port_output_index

            # Input stream ports
            for port in unit.input_stream_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1

                base_cluster = audio_cluster_index
                for cluster in port.clusters:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_AUDIO_CLUSTER,
                            audio_cluster_index,
                            _serialize_audio_cluster(cluster, audio_cluster_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_AUDIO_CLUSTER,
                        audio_cluster_index,
                        cluster.symbol,
                    )
                    audio_cluster_index += 1

                base_map = audio_map_index
                for am in port.maps:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_AUDIO_MAP,
                            audio_map_index,
                            _serialize_audio_map(am, audio_map_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_AUDIO_MAP,
                        audio_map_index,
                        am.symbol,
                    )
                    audio_map_index += 1

                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_STREAM_PORT_INPUT,
                        stream_port_input_index,
                        _serialize_stream_port(
                            port,
                            DESCRIPTOR_STREAM_PORT_INPUT,
                            stream_port_input_index,
                            len(port.controls),
                            port_base_control,
                            len(port.clusters),
                            base_cluster,
                            len(port.maps),
                            base_map,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_STREAM_PORT_INPUT,
                    stream_port_input_index,
                    port.symbol,
                )
                stream_port_input_index += 1

            # Output stream ports (same pattern)
            for port in unit.output_stream_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1

                base_cluster = audio_cluster_index
                for cluster in port.clusters:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_AUDIO_CLUSTER,
                            audio_cluster_index,
                            _serialize_audio_cluster(cluster, audio_cluster_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_AUDIO_CLUSTER,
                        audio_cluster_index,
                        cluster.symbol,
                    )
                    audio_cluster_index += 1

                base_map = audio_map_index
                for am in port.maps:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_AUDIO_MAP,
                            audio_map_index,
                            _serialize_audio_map(am, audio_map_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_AUDIO_MAP,
                        audio_map_index,
                        am.symbol,
                    )
                    audio_map_index += 1

                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_STREAM_PORT_OUTPUT,
                        stream_port_output_index,
                        _serialize_stream_port(
                            port,
                            DESCRIPTOR_STREAM_PORT_OUTPUT,
                            stream_port_output_index,
                            len(port.controls),
                            port_base_control,
                            len(port.clusters),
                            base_cluster,
                            len(port.maps),
                            base_map,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_STREAM_PORT_OUTPUT,
                    stream_port_output_index,
                    port.symbol,
                )
                stream_port_output_index += 1

            # External input ports
            base_epi = external_port_input_index
            for port in unit.input_external_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_EXTERNAL_PORT_INPUT,
                        external_port_input_index,
                        _serialize_external_port(
                            port,
                            DESCRIPTOR_EXTERNAL_PORT_INPUT,
                            external_port_input_index,
                            len(port.controls),
                            port_base_control,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_EXTERNAL_PORT_INPUT,
                    external_port_input_index,
                    port.symbol,
                )
                external_port_input_index += 1

            # External output ports
            base_epo = external_port_output_index
            for port in unit.output_external_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
                        external_port_output_index,
                        _serialize_external_port(
                            port,
                            DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
                            external_port_output_index,
                            len(port.controls),
                            port_base_control,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
                    external_port_output_index,
                    port.symbol,
                )
                external_port_output_index += 1

            # Internal input ports
            base_ipi = internal_port_input_index
            for port in unit.input_internal_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_INTERNAL_PORT_INPUT,
                        internal_port_input_index,
                        _serialize_internal_port(
                            port,
                            DESCRIPTOR_INTERNAL_PORT_INPUT,
                            internal_port_input_index,
                            len(port.controls),
                            port_base_control,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_INTERNAL_PORT_INPUT,
                    internal_port_input_index,
                    port.symbol,
                )
                internal_port_input_index += 1

            # Internal output ports
            base_ipo = internal_port_output_index
            for port in unit.output_internal_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_INTERNAL_PORT_OUTPUT,
                        internal_port_output_index,
                        _serialize_internal_port(
                            port,
                            DESCRIPTOR_INTERNAL_PORT_OUTPUT,
                            internal_port_output_index,
                            len(port.controls),
                            port_base_control,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_INTERNAL_PORT_OUTPUT,
                    internal_port_output_index,
                    port.symbol,
                )
                internal_port_output_index += 1

            # Now serialize the audio unit itself with computed base indices
            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_AUDIO_UNIT,
                    unit_idx,
                    _serialize_audio_unit(
                        unit,
                        unit_idx,
                        len(unit.input_stream_ports),
                        base_spi,
                        len(unit.output_stream_ports),
                        base_spo,
                        len(unit.input_external_ports),
                        base_epi,
                        len(unit.output_external_ports),
                        base_epo,
                        len(unit.input_internal_ports),
                        base_ipi,
                        len(unit.output_internal_ports),
                        base_ipo,
                        len(unit.controls),
                        unit_base_control,
                    ),
                )
            )
            _add_symbol(
                symbols, config_idx, DESCRIPTOR_AUDIO_UNIT, unit_idx, unit.symbol
            )

        # Video Units and their children
        for unit_idx, unit in enumerate(config.video_units):
            unit_base_control = control_index

            # Unit-level controls
            for ctrl in unit.controls:
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        _serialize_control(ctrl, control_index),
                    )
                )
                _add_symbol(
                    symbols, config_idx, DESCRIPTOR_CONTROL, control_index, ctrl.symbol
                )
                control_index += 1

            base_spi = stream_port_input_index
            base_spo = stream_port_output_index

            # Input stream ports (video)
            for port in unit.input_stream_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1

                base_cluster = video_cluster_index
                for cluster in port.clusters:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_VIDEO_CLUSTER,
                            video_cluster_index,
                            _serialize_video_cluster(cluster, video_cluster_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_VIDEO_CLUSTER,
                        video_cluster_index,
                        cluster.symbol,
                    )
                    video_cluster_index += 1

                base_map = video_map_index
                for vm in port.maps:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_VIDEO_MAP,
                            video_map_index,
                            _serialize_video_map(vm, video_map_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_VIDEO_MAP,
                        video_map_index,
                        vm.symbol,
                    )
                    video_map_index += 1

                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_STREAM_PORT_INPUT,
                        stream_port_input_index,
                        _serialize_stream_port(
                            AudioStreamPort(
                                clock_domain_index=port.clock_domain_index,
                                port_flags=port.port_flags,
                            ),
                            DESCRIPTOR_STREAM_PORT_INPUT,
                            stream_port_input_index,
                            len(port.controls),
                            port_base_control,
                            len(port.clusters),
                            base_cluster,
                            len(port.maps),
                            base_map,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_STREAM_PORT_INPUT,
                    stream_port_input_index,
                    port.symbol,
                )
                stream_port_input_index += 1

            # Output stream ports (video)
            for port in unit.output_stream_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1

                base_cluster = video_cluster_index
                for cluster in port.clusters:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_VIDEO_CLUSTER,
                            video_cluster_index,
                            _serialize_video_cluster(cluster, video_cluster_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_VIDEO_CLUSTER,
                        video_cluster_index,
                        cluster.symbol,
                    )
                    video_cluster_index += 1

                base_map = video_map_index
                for vm in port.maps:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_VIDEO_MAP,
                            video_map_index,
                            _serialize_video_map(vm, video_map_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_VIDEO_MAP,
                        video_map_index,
                        vm.symbol,
                    )
                    video_map_index += 1

                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_STREAM_PORT_OUTPUT,
                        stream_port_output_index,
                        _serialize_stream_port(
                            AudioStreamPort(
                                clock_domain_index=port.clock_domain_index,
                                port_flags=port.port_flags,
                            ),
                            DESCRIPTOR_STREAM_PORT_OUTPUT,
                            stream_port_output_index,
                            len(port.controls),
                            port_base_control,
                            len(port.clusters),
                            base_cluster,
                            len(port.maps),
                            base_map,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_STREAM_PORT_OUTPUT,
                    stream_port_output_index,
                    port.symbol,
                )
                stream_port_output_index += 1

            # External/internal ports (video unit)
            base_epi = external_port_input_index
            for port in unit.input_external_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_EXTERNAL_PORT_INPUT,
                        external_port_input_index,
                        _serialize_external_port(
                            port,
                            DESCRIPTOR_EXTERNAL_PORT_INPUT,
                            external_port_input_index,
                            len(port.controls),
                            port_base_control,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_EXTERNAL_PORT_INPUT,
                    external_port_input_index,
                    port.symbol,
                )
                external_port_input_index += 1
            base_epo = external_port_output_index
            for port in unit.output_external_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
                        external_port_output_index,
                        _serialize_external_port(
                            port,
                            DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
                            external_port_output_index,
                            len(port.controls),
                            port_base_control,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
                    external_port_output_index,
                    port.symbol,
                )
                external_port_output_index += 1
            base_ipi = internal_port_input_index
            for port in unit.input_internal_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_INTERNAL_PORT_INPUT,
                        internal_port_input_index,
                        _serialize_internal_port(
                            port,
                            DESCRIPTOR_INTERNAL_PORT_INPUT,
                            internal_port_input_index,
                            len(port.controls),
                            port_base_control,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_INTERNAL_PORT_INPUT,
                    internal_port_input_index,
                    port.symbol,
                )
                internal_port_input_index += 1
            base_ipo = internal_port_output_index
            for port in unit.output_internal_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_INTERNAL_PORT_OUTPUT,
                        internal_port_output_index,
                        _serialize_internal_port(
                            port,
                            DESCRIPTOR_INTERNAL_PORT_OUTPUT,
                            internal_port_output_index,
                            len(port.controls),
                            port_base_control,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_INTERNAL_PORT_OUTPUT,
                    internal_port_output_index,
                    port.symbol,
                )
                internal_port_output_index += 1

            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_VIDEO_UNIT,
                    unit_idx,
                    _serialize_video_unit(
                        unit,
                        unit_idx,
                        len(unit.input_stream_ports),
                        base_spi,
                        len(unit.output_stream_ports),
                        base_spo,
                        len(unit.input_external_ports),
                        base_epi,
                        len(unit.output_external_ports),
                        base_epo,
                        len(unit.input_internal_ports),
                        base_ipi,
                        len(unit.output_internal_ports),
                        base_ipo,
                        len(unit.controls),
                        unit_base_control,
                    ),
                )
            )
            _add_symbol(
                symbols, config_idx, DESCRIPTOR_VIDEO_UNIT, unit_idx, unit.symbol
            )

        # Sensor Units and their children
        for unit_idx, unit in enumerate(config.sensor_units):
            unit_base_control = control_index

            # Unit-level controls
            for ctrl in unit.controls:
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        _serialize_control(ctrl, control_index),
                    )
                )
                _add_symbol(
                    symbols, config_idx, DESCRIPTOR_CONTROL, control_index, ctrl.symbol
                )
                control_index += 1

            base_spi = stream_port_input_index
            base_spo = stream_port_output_index

            # Input stream ports (sensor)
            for port in unit.input_stream_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1

                base_cluster = sensor_cluster_index
                for cluster in port.clusters:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_SENSOR_CLUSTER,
                            sensor_cluster_index,
                            _serialize_sensor_cluster(cluster, sensor_cluster_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_SENSOR_CLUSTER,
                        sensor_cluster_index,
                        cluster.symbol,
                    )
                    sensor_cluster_index += 1

                base_map = sensor_map_index
                for sm in port.maps:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_SENSOR_MAP,
                            sensor_map_index,
                            _serialize_sensor_map(sm, sensor_map_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_SENSOR_MAP,
                        sensor_map_index,
                        sm.symbol,
                    )
                    sensor_map_index += 1

                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_STREAM_PORT_INPUT,
                        stream_port_input_index,
                        _serialize_stream_port(
                            AudioStreamPort(
                                clock_domain_index=port.clock_domain_index,
                                port_flags=port.port_flags,
                            ),
                            DESCRIPTOR_STREAM_PORT_INPUT,
                            stream_port_input_index,
                            len(port.controls),
                            port_base_control,
                            len(port.clusters),
                            base_cluster,
                            len(port.maps),
                            base_map,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_STREAM_PORT_INPUT,
                    stream_port_input_index,
                    port.symbol,
                )
                stream_port_input_index += 1

            # Output stream ports (sensor)
            for port in unit.output_stream_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1

                base_cluster = sensor_cluster_index
                for cluster in port.clusters:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_SENSOR_CLUSTER,
                            sensor_cluster_index,
                            _serialize_sensor_cluster(cluster, sensor_cluster_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_SENSOR_CLUSTER,
                        sensor_cluster_index,
                        cluster.symbol,
                    )
                    sensor_cluster_index += 1

                base_map = sensor_map_index
                for sm in port.maps:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_SENSOR_MAP,
                            sensor_map_index,
                            _serialize_sensor_map(sm, sensor_map_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_SENSOR_MAP,
                        sensor_map_index,
                        sm.symbol,
                    )
                    sensor_map_index += 1

                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_STREAM_PORT_OUTPUT,
                        stream_port_output_index,
                        _serialize_stream_port(
                            AudioStreamPort(
                                clock_domain_index=port.clock_domain_index,
                                port_flags=port.port_flags,
                            ),
                            DESCRIPTOR_STREAM_PORT_OUTPUT,
                            stream_port_output_index,
                            len(port.controls),
                            port_base_control,
                            len(port.clusters),
                            base_cluster,
                            len(port.maps),
                            base_map,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_STREAM_PORT_OUTPUT,
                    stream_port_output_index,
                    port.symbol,
                )
                stream_port_output_index += 1

            # External/internal ports (sensor unit)
            base_epi = external_port_input_index
            for port in unit.input_external_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_EXTERNAL_PORT_INPUT,
                        external_port_input_index,
                        _serialize_external_port(
                            port,
                            DESCRIPTOR_EXTERNAL_PORT_INPUT,
                            external_port_input_index,
                            len(port.controls),
                            port_base_control,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_EXTERNAL_PORT_INPUT,
                    external_port_input_index,
                    port.symbol,
                )
                external_port_input_index += 1
            base_epo = external_port_output_index
            for port in unit.output_external_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
                        external_port_output_index,
                        _serialize_external_port(
                            port,
                            DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
                            external_port_output_index,
                            len(port.controls),
                            port_base_control,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
                    external_port_output_index,
                    port.symbol,
                )
                external_port_output_index += 1
            base_ipi = internal_port_input_index
            for port in unit.input_internal_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_INTERNAL_PORT_INPUT,
                        internal_port_input_index,
                        _serialize_internal_port(
                            port,
                            DESCRIPTOR_INTERNAL_PORT_INPUT,
                            internal_port_input_index,
                            len(port.controls),
                            port_base_control,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_INTERNAL_PORT_INPUT,
                    internal_port_input_index,
                    port.symbol,
                )
                internal_port_input_index += 1
            base_ipo = internal_port_output_index
            for port in unit.output_internal_ports:
                port_base_control = control_index
                for ctrl in port.controls:
                    descriptors.append(
                        FlatDescriptor(
                            config_idx,
                            DESCRIPTOR_CONTROL,
                            control_index,
                            _serialize_control(ctrl, control_index),
                        )
                    )
                    _add_symbol(
                        symbols,
                        config_idx,
                        DESCRIPTOR_CONTROL,
                        control_index,
                        ctrl.symbol,
                    )
                    control_index += 1
                descriptors.append(
                    FlatDescriptor(
                        config_idx,
                        DESCRIPTOR_INTERNAL_PORT_OUTPUT,
                        internal_port_output_index,
                        _serialize_internal_port(
                            port,
                            DESCRIPTOR_INTERNAL_PORT_OUTPUT,
                            internal_port_output_index,
                            len(port.controls),
                            port_base_control,
                        ),
                    )
                )
                _add_symbol(
                    symbols,
                    config_idx,
                    DESCRIPTOR_INTERNAL_PORT_OUTPUT,
                    internal_port_output_index,
                    port.symbol,
                )
                internal_port_output_index += 1

            descriptors.append(
                FlatDescriptor(
                    config_idx,
                    DESCRIPTOR_SENSOR_UNIT,
                    unit_idx,
                    _serialize_sensor_unit(
                        unit,
                        unit_idx,
                        len(unit.input_stream_ports),
                        base_spi,
                        len(unit.output_stream_ports),
                        base_spo,
                        len(unit.input_external_ports),
                        base_epi,
                        len(unit.output_external_ports),
                        base_epo,
                        len(unit.input_internal_ports),
                        base_ipi,
                        len(unit.output_internal_ports),
                        base_ipo,
                        len(unit.controls),
                        unit_base_control,
                    ),
                )
            )
            _add_symbol(
                symbols, config_idx, DESCRIPTOR_SENSOR_UNIT, unit_idx, unit.symbol
            )

    # Sort by (config, type, index)
    descriptors.sort(
        key=lambda d: (d.config_index, d.descriptor_type, d.descriptor_index)
    )
    symbols.sort(key=lambda s: (s.config_index, s.descriptor_type, s.descriptor_index))

    return descriptors, symbols


def _count_all_controls(config: Configuration) -> int:
    """Count total controls across all levels in a configuration."""
    count = len(config.controls)
    for unit in config.audio_units:
        count += len(unit.controls)
        for port in unit.input_stream_ports + unit.output_stream_ports:
            count += len(port.controls)
        for port in unit.input_external_ports + unit.output_external_ports:
            count += len(port.controls)
        for port in unit.input_internal_ports + unit.output_internal_ports:
            count += len(port.controls)
    for unit in config.video_units:
        count += len(unit.controls)
        for port in unit.input_stream_ports + unit.output_stream_ports:
            count += len(port.controls)
        for port in unit.input_external_ports + unit.output_external_ports:
            count += len(port.controls)
        for port in unit.input_internal_ports + unit.output_internal_ports:
            count += len(port.controls)
    for unit in config.sensor_units:
        count += len(unit.controls)
        for port in unit.input_stream_ports + unit.output_stream_ports:
            count += len(port.controls)
        for port in unit.input_external_ports + unit.output_external_ports:
            count += len(port.controls)
        for port in unit.input_internal_ports + unit.output_internal_ports:
            count += len(port.controls)
    return count
