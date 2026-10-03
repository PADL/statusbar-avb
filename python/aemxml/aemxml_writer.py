# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Write AEM data model to AEMXML format."""

from __future__ import annotations

import xml.etree.ElementTree as ET

from .model import (
    AudioCluster,
    AudioMap,
    AudioStreamPort,
    AudioUnit,
    AvbInterface,
    ClockDomain,
    ClockSource,
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
    SensorStreamPort,
    SensorUnit,
    SignalCombiner,
    SignalDemultiplexer,
    SignalMultiplexer,
    SignalSelector,
    SignalSplitter,
    SignalTranscoder,
    Stream,
    StringsDescriptor,
    Timing,
    VideoCluster,
    VideoMap,
    VideoStreamPort,
    VideoUnit,
    # Enum/flag value->name mappings
    DESCRIPTOR_TYPE_VALUES,
    JACK_TYPE_VALUES,
    CLOCK_SOURCE_TYPE_VALUES,
    AUDIO_CLUSTER_FORMAT_VALUES,
    MEMORY_OBJECT_TYPE_VALUES,
    TIMING_ALGORITHM_VALUES,
    PTP_PORT_TYPE_VALUES,
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

# Module-level context for schema year, set by write_aemxml()
_schema_year: int = 2021


def _set_text(parent: ET.Element, tag: str, value: str) -> None:
    el = ET.SubElement(parent, tag)
    el.text = value


def _hex16(parent: ET.Element, tag: str, val: int) -> None:
    _set_text(parent, tag, f"{val:04x}")


def _hex32(parent: ET.Element, tag: str, val: int) -> None:
    _set_text(parent, tag, f"{val:08x}")


def _hex64(parent: ET.Element, tag: str, val: int) -> None:
    _set_text(parent, tag, f"{val:016x}")


def _hex8(parent: ET.Element, tag: str, val: int) -> None:
    _set_text(parent, tag, f"{val:02x}")


def _add_symbol(el: ET.Element, symbol: str | None) -> None:
    if symbol is not None:
        el.set("symbol", symbol)


def _format_enum16(value: int, value_map: dict[int, str]) -> str:
    """Format a 16-bit enum value as a 4-digit hex string.

    Per standards/atdecc.xsd (hex-only), AEMXML output never uses symbolic
    names. value_map is kept in the signature for call-site compatibility
    and is intentionally unused. The symbolic-name XSD variant lives in
    standards/atdecc-proposed.xsd.
    """
    del value_map
    return f"{value:04x}"


def _format_enum8(value: int, value_map: dict[int, str]) -> str:
    """Format an 8-bit enum value as a 2-digit hex string (see _format_enum16)."""
    del value_map
    return f"{value:02x}"


def _format_flags16(value: int, name_map: dict[str, int]) -> str:
    """Format 16-bit bitmask flags as a 4-digit hex string (see _format_enum16)."""
    del name_map
    return f"{value:04x}"


def _format_flags32(value: int, name_map: dict[str, int]) -> str:
    """Format 32-bit bitmask flags as an 8-digit hex string (see _format_enum16)."""
    del name_map
    return f"{value:08x}"


def _write_enum16(
    parent: ET.Element, tag: str, val: int, value_map: dict[int, str]
) -> None:
    _set_text(parent, tag, _format_enum16(val, value_map))


def _write_enum8(
    parent: ET.Element, tag: str, val: int, value_map: dict[int, str]
) -> None:
    _set_text(parent, tag, _format_enum8(val, value_map))


def _write_flags16(
    parent: ET.Element, tag: str, val: int, name_map: dict[str, int]
) -> None:
    _set_text(parent, tag, _format_flags16(val, name_map))


def _write_flags32(
    parent: ET.Element, tag: str, val: int, name_map: dict[str, int]
) -> None:
    _set_text(parent, tag, _format_flags32(val, name_map))


def _write_localized_ref(parent: ET.Element, tag: str, ref: LocalizedStringRef) -> None:
    container = ET.SubElement(parent, tag)
    ls = ET.SubElement(container, "localized_string")
    _hex16(ls, "offset", ref.offset)
    _hex8(ls, "index", ref.index)


def _write_audio_cluster(parent: ET.Element, cluster: AudioCluster) -> None:
    el = ET.SubElement(parent, "audio_cluster")
    _add_symbol(el, cluster.symbol)
    if cluster.object_name:
        _set_text(el, "object_name", cluster.object_name)
    _write_localized_ref(el, "localized_description", cluster.localized_description)
    _write_enum16(el, "signal_type", cluster.signal_type, DESCRIPTOR_TYPE_VALUES)
    _hex16(el, "signal_index", cluster.signal_index)
    _hex16(el, "signal_output", cluster.signal_output)
    _hex32(el, "path_latency", cluster.path_latency)
    _hex32(el, "block_latency", cluster.block_latency)
    _hex16(el, "channel_count", cluster.channel_count)
    _write_enum8(el, "format", cluster.format, AUDIO_CLUSTER_FORMAT_VALUES)
    if _schema_year != 2013 and (
        cluster.aes3_data_type_reference or cluster.aes3_data_type
    ):
        _hex8(el, "aes3_data_type_reference", cluster.aes3_data_type_reference)
        _hex16(el, "aes3_data_type", cluster.aes3_data_type)


def _write_audio_map(parent: ET.Element, am: AudioMap) -> None:
    el = ET.SubElement(parent, "audio_map")
    _add_symbol(el, am.symbol)
    for m in am.mappings:
        mel = ET.SubElement(el, "audio_mapping")
        _hex16(mel, "stream_index", m.stream_index)
        _hex16(mel, "stream_channel", m.stream_channel)
        _hex16(mel, "cluster_offset", m.cluster_offset)
        _hex16(mel, "cluster_channel", m.cluster_channel)


def _write_control(parent: ET.Element, ctrl: Control) -> None:
    el = ET.SubElement(parent, "control")
    _add_symbol(el, ctrl.symbol)
    if ctrl.object_name:
        _set_text(el, "object_name", ctrl.object_name)
    _write_localized_ref(el, "localized_description", ctrl.localized_description)
    _hex32(el, "block_latency", ctrl.block_latency)
    _hex32(el, "control_latency", ctrl.control_latency)
    _hex16(el, "control_domain", ctrl.control_domain)
    _hex16(el, "control_value_type", ctrl.control_value_type)
    _hex64(el, "control_type", ctrl.control_type)
    _hex32(el, "reset_time", ctrl.reset_time)
    _write_enum16(el, "signal_type", ctrl.signal_type, DESCRIPTOR_TYPE_VALUES)
    _hex16(el, "signal_index", ctrl.signal_index)
    _hex16(el, "signal_output", ctrl.signal_output)
    ET.SubElement(el, "values")


def _write_stream_port(parent: ET.Element, port: AudioStreamPort) -> None:
    el = ET.SubElement(parent, "stream_port")
    _add_symbol(el, port.symbol)
    _hex16(el, "clock_domain_index", port.clock_domain_index)
    _hex16(el, "port_flags", port.port_flags)
    if port.controls:
        ctrls = ET.SubElement(el, "controls")
        for ctrl in port.controls:
            _write_control(ctrls, ctrl)
    clusters = ET.SubElement(el, "clusters")
    for cluster in port.clusters:
        _write_audio_cluster(clusters, cluster)
    if port.maps:
        maps = ET.SubElement(el, "maps")
        for am in port.maps:
            _write_audio_map(maps, am)


def _write_sampling_rate(parent: ET.Element, tag: str, rate: int) -> None:
    el = ET.SubElement(parent, tag)
    pull = (rate >> 29) & 0x07
    base = rate & 0x1FFFFFFF
    _hex8(el, "pull", pull)
    _hex32(el, "base_frequency", base)


def _write_external_port(parent: ET.Element, port: ExternalPort) -> None:
    el = ET.SubElement(parent, "external_port")
    _add_symbol(el, port.symbol)
    _hex16(el, "clock_domain_index", port.clock_domain_index)
    _hex16(el, "port_flags", port.port_flags)
    _write_enum16(el, "signal_type", port.signal_type, DESCRIPTOR_TYPE_VALUES)
    _hex16(el, "signal_index", port.signal_index)
    _hex16(el, "signal_output", port.signal_output)
    _hex32(el, "block_latency", port.block_latency)
    _hex16(el, "jack_index", port.jack_index)
    if port.controls:
        ctrls = ET.SubElement(el, "controls")
        for ctrl in port.controls:
            _write_control(ctrls, ctrl)


def _write_internal_port(parent: ET.Element, port: InternalPort) -> None:
    el = ET.SubElement(parent, "internal_port")
    _add_symbol(el, port.symbol)
    _hex16(el, "clock_domain_index", port.clock_domain_index)
    _hex16(el, "port_flags", port.port_flags)
    _write_enum16(el, "signal_type", port.signal_type, DESCRIPTOR_TYPE_VALUES)
    _hex16(el, "signal_index", port.signal_index)
    _hex16(el, "signal_output", port.signal_output)
    _hex32(el, "block_latency", port.block_latency)
    _hex16(el, "internal_index", port.internal_index)
    if port.controls:
        ctrls = ET.SubElement(el, "controls")
        for ctrl in port.controls:
            _write_control(ctrls, ctrl)


def _write_audio_unit(parent: ET.Element, unit: AudioUnit) -> None:
    el = ET.SubElement(parent, "audio_unit")
    _add_symbol(el, unit.symbol)
    if unit.object_name:
        _set_text(el, "object_name", unit.object_name)
    _write_localized_ref(el, "localized_description", unit.localized_description)
    _hex16(el, "clock_domain_index", unit.clock_domain_index)
    if unit.input_stream_ports:
        isp = ET.SubElement(el, "input_stream_ports")
        for port in unit.input_stream_ports:
            _write_stream_port(isp, port)
    if unit.output_stream_ports:
        osp = ET.SubElement(el, "output_stream_ports")
        for port in unit.output_stream_ports:
            _write_stream_port(osp, port)
    if unit.input_external_ports:
        iep = ET.SubElement(el, "input_external_ports")
        for port in unit.input_external_ports:
            _write_external_port(iep, port)
    if unit.output_external_ports:
        oep = ET.SubElement(el, "output_external_ports")
        for port in unit.output_external_ports:
            _write_external_port(oep, port)
    if unit.input_internal_ports:
        iip = ET.SubElement(el, "input_internal_ports")
        for port in unit.input_internal_ports:
            _write_internal_port(iip, port)
    if unit.output_internal_ports:
        oip = ET.SubElement(el, "output_internal_ports")
        for port in unit.output_internal_ports:
            _write_internal_port(oip, port)
    if unit.controls:
        ctrls = ET.SubElement(el, "controls")
        for ctrl in unit.controls:
            _write_control(ctrls, ctrl)
    _write_sampling_rate(el, "current_sampling_rate", unit.current_sampling_rate)
    rates = ET.SubElement(el, "sampling_rates")
    for rate in unit.sampling_rates:
        _write_sampling_rate(rates, "sampling_rate", rate)


def _write_stream(parent: ET.Element, stream: Stream) -> None:
    el = ET.SubElement(parent, "stream")
    _add_symbol(el, stream.symbol)
    if stream.object_name:
        _set_text(el, "object_name", stream.object_name)
    _write_localized_ref(el, "localized_description", stream.localized_description)
    _hex16(el, "clock_domain_index", stream.clock_domain_index)
    _write_flags16(el, "stream_flags", stream.stream_flags, STREAM_FLAGS_NAMES)
    _hex64(el, "current_stream_format", stream.current_format)
    _hex64(el, "backup_talker_entity_id0", stream.backup_talker_entity_id_0)
    _hex16(el, "backup_talker_uniqueid0", stream.backup_talker_unique_id_0)
    _hex64(el, "backup_talker_entity_id1", stream.backup_talker_entity_id_1)
    _hex16(el, "backup_talker_uniqueid1", stream.backup_talker_unique_id_1)
    _hex64(el, "backup_talker_entity_id2", stream.backup_talker_entity_id_2)
    _hex16(el, "backup_talker_uniqueid2", stream.backup_talker_unique_id_2)
    _hex64(el, "backed_up_talker_entity_id", stream.backedup_talker_entity_id)
    _hex16(el, "backed_up_talker_uniqueid", stream.backedup_talker_unique_id)
    _hex16(el, "avb_interface_index", stream.avb_interface_index)
    _hex32(el, "buffer_length", stream.buffer_length)
    if stream.timing:
        _hex16(el, "timing", stream.timing)
    fmts = ET.SubElement(el, "stream_formats")
    for fmt in stream.formats:
        _hex64(fmts, "stream_format", fmt)
    if stream.redundant_streams:
        rs = ET.SubElement(el, "redundant_streams")
        for idx in stream.redundant_streams:
            _hex16(rs, "redundant_stream_index", idx)


def _write_jack(parent: ET.Element, jack: Jack) -> None:
    el = ET.SubElement(parent, "jack")
    _add_symbol(el, jack.symbol)
    if jack.object_name:
        _set_text(el, "object_name", jack.object_name)
    _write_localized_ref(el, "localized_description", jack.localized_description)
    _write_flags16(el, "jack_flags", jack.jack_flags, JACK_FLAGS_NAMES)
    _write_enum16(el, "jack_type", jack.jack_type, JACK_TYPE_VALUES)


def _write_avb_interface(parent: ET.Element, intf: AvbInterface) -> None:
    el = ET.SubElement(parent, "avb_interface")
    _add_symbol(el, intf.symbol)
    if intf.object_name:
        _set_text(el, "object_name", intf.object_name)
    _write_localized_ref(el, "localized_description", intf.localized_description)
    mac_bytes = intf.mac_address.to_bytes(6, "big")
    _set_text(el, "mac_address", ":".join(f"{b:02x}" for b in mac_bytes))
    _write_flags16(el, "interface_flags", intf.interface_flags, INTERFACE_FLAGS_NAMES)
    _hex64(el, "clock_identity", intf.clock_identity)
    _hex8(el, "priority1", intf.priority1)
    _hex8(el, "clock_class", intf.clock_class)
    _hex16(el, "offset_scaled_log_variance", intf.offset_scaled_log_variance)
    _hex8(el, "clock_accuracy", intf.clock_accuracy)
    _hex8(el, "priority2", intf.priority2)
    _hex8(el, "domain_number", intf.domain_number)
    _hex8(el, "log_sync_interval", intf.log_sync_interval)
    _hex8(el, "log_announce_interval", intf.log_announce_interval)
    _hex8(el, "log_pdelay_interval", intf.log_pdelay_interval)
    _hex16(el, "port_number", intf.port_number)


def _write_clock_source(parent: ET.Element, cs: ClockSource) -> None:
    el = ET.SubElement(parent, "clock_source")
    _add_symbol(el, cs.symbol)
    if cs.object_name:
        _set_text(el, "object_name", cs.object_name)
    _write_localized_ref(el, "localized_description", cs.localized_description)
    _hex16(el, "clock_source_flags", cs.clock_source_flags)
    _write_enum16(
        el, "clock_source_type", cs.clock_source_type, CLOCK_SOURCE_TYPE_VALUES
    )
    _hex64(el, "clock_source_id", cs.clock_source_identifier)
    _write_enum16(
        el,
        "clock_source_location_type",
        cs.clock_source_location_type,
        DESCRIPTOR_TYPE_VALUES,
    )
    _hex16(el, "clock_source_location_index", cs.clock_source_location_index)


def _write_clock_domain(parent: ET.Element, cd: ClockDomain) -> None:
    el = ET.SubElement(parent, "clock_domain")
    _add_symbol(el, cd.symbol)
    if cd.object_name:
        _set_text(el, "object_name", cd.object_name)
    _write_localized_ref(el, "localized_description", cd.localized_description)
    _hex16(el, "clock_source_index", cd.clock_source_index)
    cs = ET.SubElement(el, "clock_sources")
    for idx in cd.clock_sources:
        _hex16(cs, "clock_source_index", idx)


def _write_locale(parent: ET.Element, locale: Locale) -> None:
    el = ET.SubElement(parent, "locale")
    _add_symbol(el, locale.symbol)
    _set_text(el, "locale_identifier", locale.locale_identifier)
    ls = ET.SubElement(el, "locale_strings")
    for sd in locale.strings_descriptors:
        sel = ET.SubElement(ls, "strings")
        _add_symbol(sel, sd.symbol)
        for s in sd.strings:
            _set_text(sel, "string", s)


def _write_signal_source(parent: ET.Element, src) -> None:
    el = ET.SubElement(parent, "source")
    _write_enum16(el, "signal_type", src.signal_type, DESCRIPTOR_TYPE_VALUES)
    _hex16(el, "signal_index", src.signal_index)
    _hex16(el, "signal_output", src.signal_output)


def _write_signal_selector(parent: ET.Element, sel: SignalSelector) -> None:
    el = ET.SubElement(parent, "signal_selector")
    _add_symbol(el, sel.symbol)
    if sel.object_name:
        _set_text(el, "object_name", sel.object_name)
    _write_localized_ref(el, "localized_description", sel.localized_description)
    _hex32(el, "block_latency", sel.block_latency)
    _hex32(el, "control_latency", sel.control_latency)
    _hex16(el, "control_domain", sel.control_domain)
    _write_enum16(
        el, "current_signal_type", sel.current_signal_type, DESCRIPTOR_TYPE_VALUES
    )
    _hex16(el, "current_signal_index", sel.current_signal_index)
    _hex16(el, "current_signal_output", sel.current_signal_output)
    _write_enum16(
        el, "default_signal_type", sel.default_signal_type, DESCRIPTOR_TYPE_VALUES
    )
    _hex16(el, "default_signal_index", sel.default_signal_index)
    _hex16(el, "default_signal_output", sel.default_signal_output)
    if sel.sources:
        srcs = ET.SubElement(el, "sources")
        for src in sel.sources:
            _write_signal_source(srcs, src)


def _write_mixer(parent: ET.Element, mixer: Mixer) -> None:
    el = ET.SubElement(parent, "mixer")
    _add_symbol(el, mixer.symbol)
    if mixer.object_name:
        _set_text(el, "object_name", mixer.object_name)
    _write_localized_ref(el, "localized_description", mixer.localized_description)
    _hex32(el, "block_latency", mixer.block_latency)
    _hex32(el, "control_latency", mixer.control_latency)
    _hex16(el, "control_domain", mixer.control_domain)
    _hex16(el, "control_value_type", mixer.control_value_type)
    if mixer.sources:
        srcs = ET.SubElement(el, "sources")
        for src in mixer.sources:
            _write_signal_source(srcs, src)


def _write_matrix(parent: ET.Element, matrix: Matrix) -> None:
    el = ET.SubElement(parent, "matrix")
    _add_symbol(el, matrix.symbol)
    if matrix.object_name:
        _set_text(el, "object_name", matrix.object_name)
    _write_localized_ref(el, "localized_description", matrix.localized_description)
    _hex32(el, "block_latency", matrix.block_latency)
    _hex32(el, "control_latency", matrix.control_latency)
    _hex16(el, "control_domain", matrix.control_domain)
    _hex16(el, "control_value_type", matrix.control_value_type)
    _hex64(el, "control_type", matrix.control_type)
    _hex16(el, "width", matrix.width)
    _hex16(el, "height", matrix.height)
    _hex16(el, "number_of_values", matrix.number_of_values)
    _hex16(el, "number_of_sources", matrix.number_of_sources)
    _hex16(el, "base_source", matrix.base_source)


def _write_splitter_map_entry(parent: ET.Element, entry) -> None:
    el = ET.SubElement(parent, "map_entry")
    _hex16(el, "sub_signal_start", entry.sub_signal_start)
    _hex16(el, "sub_signal_count", entry.sub_signal_count)
    _hex16(el, "output_index", entry.output_index)


def _write_signal_splitter(parent: ET.Element, sp: SignalSplitter) -> None:
    el = ET.SubElement(parent, "signal_splitter")
    _add_symbol(el, sp.symbol)
    if sp.object_name:
        _set_text(el, "object_name", sp.object_name)
    _write_localized_ref(el, "localized_description", sp.localized_description)
    _hex32(el, "block_latency", sp.block_latency)
    _hex32(el, "control_latency", sp.control_latency)
    _hex16(el, "control_domain", sp.control_domain)
    _write_enum16(el, "signal_type", sp.signal_type, DESCRIPTOR_TYPE_VALUES)
    _hex16(el, "signal_index", sp.signal_index)
    _hex16(el, "signal_output", sp.signal_output)
    _hex16(el, "number_of_outputs", sp.number_of_outputs)
    if sp.splitter_map:
        smap = ET.SubElement(el, "splitter_map")
        for entry in sp.splitter_map:
            _write_splitter_map_entry(smap, entry)


def _write_combiner_map_entry(parent: ET.Element, entry) -> None:
    el = ET.SubElement(parent, "map_entry")
    _hex16(el, "sub_signal_start", entry.sub_signal_start)
    _hex16(el, "sub_signal_count", entry.sub_signal_count)
    _hex16(el, "input_index", entry.input_index)


def _write_signal_combiner(parent: ET.Element, comb: SignalCombiner) -> None:
    el = ET.SubElement(parent, "signal_combiner")
    _add_symbol(el, comb.symbol)
    if comb.object_name:
        _set_text(el, "object_name", comb.object_name)
    _write_localized_ref(el, "localized_description", comb.localized_description)
    _hex32(el, "block_latency", comb.block_latency)
    _hex32(el, "control_latency", comb.control_latency)
    _hex16(el, "control_domain", comb.control_domain)
    if comb.combiner_map:
        cmap = ET.SubElement(el, "combiner_map")
        for entry in comb.combiner_map:
            _write_combiner_map_entry(cmap, entry)
    if comb.sources:
        srcs = ET.SubElement(el, "sources")
        for src in comb.sources:
            _write_signal_source(srcs, src)


def _write_signal_demultiplexer(parent: ET.Element, demux: SignalDemultiplexer) -> None:
    el = ET.SubElement(parent, "signal_demultiplexer")
    _add_symbol(el, demux.symbol)
    if demux.object_name:
        _set_text(el, "object_name", demux.object_name)
    _write_localized_ref(el, "localized_description", demux.localized_description)
    _hex32(el, "block_latency", demux.block_latency)
    _hex32(el, "control_latency", demux.control_latency)
    _hex16(el, "control_domain", demux.control_domain)
    _write_enum16(el, "signal_type", demux.signal_type, DESCRIPTOR_TYPE_VALUES)
    _hex16(el, "signal_index", demux.signal_index)
    _hex16(el, "signal_output", demux.signal_output)
    _hex16(el, "number_of_outputs", demux.number_of_outputs)
    if demux.demultiplexer_map:
        dmap = ET.SubElement(el, "demultiplexer_map")
        for entry in demux.demultiplexer_map:
            _write_splitter_map_entry(dmap, entry)


def _write_signal_multiplexer(parent: ET.Element, mux: SignalMultiplexer) -> None:
    el = ET.SubElement(parent, "signal_multiplexer")
    _add_symbol(el, mux.symbol)
    if mux.object_name:
        _set_text(el, "object_name", mux.object_name)
    _write_localized_ref(el, "localized_description", mux.localized_description)
    _hex32(el, "block_latency", mux.block_latency)
    _hex32(el, "control_latency", mux.control_latency)
    _hex16(el, "control_domain", mux.control_domain)
    if mux.multiplexer_map:
        mmap = ET.SubElement(el, "multiplexer_map")
        for entry in mux.multiplexer_map:
            _write_combiner_map_entry(mmap, entry)
    if mux.sources:
        srcs = ET.SubElement(el, "sources")
        for src in mux.sources:
            _write_signal_source(srcs, src)


def _write_signal_transcoder(parent: ET.Element, tc: SignalTranscoder) -> None:
    el = ET.SubElement(parent, "signal_transcoder")
    _add_symbol(el, tc.symbol)
    if tc.object_name:
        _set_text(el, "object_name", tc.object_name)
    _write_localized_ref(el, "localized_description", tc.localized_description)
    _hex32(el, "block_latency", tc.block_latency)
    _hex32(el, "control_latency", tc.control_latency)
    _hex16(el, "control_domain", tc.control_domain)
    _hex16(el, "control_value_type", tc.control_value_type)
    _write_enum16(el, "signal_type", tc.signal_type, DESCRIPTOR_TYPE_VALUES)
    _hex16(el, "signal_index", tc.signal_index)
    _hex16(el, "signal_output", tc.signal_output)
    _hex64(el, "transcoder_type", tc.transcoder_type)


def _write_control_block(parent: ET.Element, cb: ControlBlock) -> None:
    el = ET.SubElement(parent, "control_block")
    _add_symbol(el, cb.symbol)
    if cb.object_name:
        _set_text(el, "object_name", cb.object_name)
    _write_localized_ref(el, "localized_description", cb.localized_description)
    _hex16(el, "number_of_controls", cb.number_of_controls)
    _hex16(el, "base_control", cb.base_control)
    _hex16(el, "final_control_index", cb.final_control_index)
    _write_enum16(el, "signal_type", cb.signal_type, DESCRIPTOR_TYPE_VALUES)
    _hex16(el, "signal_index", cb.signal_index)
    _hex16(el, "signal_output", cb.signal_output)


def _write_memory_object(parent: ET.Element, mo: MemoryObject) -> None:
    el = ET.SubElement(parent, "memory_object")
    _add_symbol(el, mo.symbol)
    if mo.object_name:
        _set_text(el, "object_name", mo.object_name)
    _write_localized_ref(el, "localized_description", mo.localized_description)
    _write_enum16(
        el, "memory_object_type", mo.memory_object_type, MEMORY_OBJECT_TYPE_VALUES
    )
    _write_enum16(
        el, "target_descriptor_type", mo.target_descriptor_type, DESCRIPTOR_TYPE_VALUES
    )
    _hex16(el, "target_descriptor_index", mo.target_descriptor_index)
    _hex64(el, "start_address", mo.start_address)
    _hex64(el, "maximum_length", mo.maximum_length)
    _hex64(el, "length", mo.length)
    _hex64(el, "maximum_segment_length", mo.maximum_segment_length)


def _write_video_cluster(parent: ET.Element, cluster: VideoCluster) -> None:
    el = ET.SubElement(parent, "video_cluster")
    _add_symbol(el, cluster.symbol)
    if cluster.object_name:
        _set_text(el, "object_name", cluster.object_name)
    _write_localized_ref(el, "localized_description", cluster.localized_description)
    _write_enum16(el, "signal_type", cluster.signal_type, DESCRIPTOR_TYPE_VALUES)
    _hex16(el, "signal_index", cluster.signal_index)
    _hex16(el, "signal_output", cluster.signal_output)
    _hex32(el, "path_latency", cluster.path_latency)
    _hex32(el, "block_latency", cluster.block_latency)
    _hex16(el, "format", cluster.format)


def _write_sensor_cluster(parent: ET.Element, cluster: SensorCluster) -> None:
    el = ET.SubElement(parent, "sensor_cluster")
    _add_symbol(el, cluster.symbol)
    if cluster.object_name:
        _set_text(el, "object_name", cluster.object_name)
    _write_localized_ref(el, "localized_description", cluster.localized_description)
    _write_enum16(el, "signal_type", cluster.signal_type, DESCRIPTOR_TYPE_VALUES)
    _hex16(el, "signal_index", cluster.signal_index)
    _hex16(el, "signal_output", cluster.signal_output)
    _hex32(el, "path_latency", cluster.path_latency)
    _hex32(el, "block_latency", cluster.block_latency)
    _hex16(el, "format", cluster.format)


def _write_video_map(parent: ET.Element, vm: VideoMap) -> None:
    el = ET.SubElement(parent, "video_map")
    _add_symbol(el, vm.symbol)
    for m in vm.mappings:
        mel = ET.SubElement(el, "video_mapping")
        _hex16(mel, "stream_index", m.stream_index)
        _hex16(mel, "stream_channel", m.stream_channel)
        _hex16(mel, "cluster_offset", m.cluster_offset)
        _hex16(mel, "cluster_channel", m.cluster_channel)


def _write_sensor_map(parent: ET.Element, sm: SensorMap) -> None:
    el = ET.SubElement(parent, "sensor_map")
    _add_symbol(el, sm.symbol)
    for m in sm.mappings:
        mel = ET.SubElement(el, "sensor_mapping")
        _hex16(mel, "stream_index", m.stream_index)
        _hex16(mel, "stream_signal", m.stream_signal)
        _hex16(mel, "cluster_offset", m.cluster_offset)


def _write_video_stream_port(parent: ET.Element, port: VideoStreamPort) -> None:
    el = ET.SubElement(parent, "stream_port")
    _add_symbol(el, port.symbol)
    _hex16(el, "clock_domain_index", port.clock_domain_index)
    _hex16(el, "port_flags", port.port_flags)
    if port.controls:
        ctrls = ET.SubElement(el, "controls")
        for ctrl in port.controls:
            _write_control(ctrls, ctrl)
    clusters = ET.SubElement(el, "clusters")
    for cluster in port.clusters:
        _write_video_cluster(clusters, cluster)
    if port.maps:
        maps = ET.SubElement(el, "maps")
        for vm in port.maps:
            _write_video_map(maps, vm)


def _write_sensor_stream_port(parent: ET.Element, port: SensorStreamPort) -> None:
    el = ET.SubElement(parent, "stream_port")
    _add_symbol(el, port.symbol)
    _hex16(el, "clock_domain_index", port.clock_domain_index)
    _hex16(el, "port_flags", port.port_flags)
    if port.controls:
        ctrls = ET.SubElement(el, "controls")
        for ctrl in port.controls:
            _write_control(ctrls, ctrl)
    clusters = ET.SubElement(el, "clusters")
    for cluster in port.clusters:
        _write_sensor_cluster(clusters, cluster)
    if port.maps:
        maps = ET.SubElement(el, "maps")
        for sm in port.maps:
            _write_sensor_map(maps, sm)


def _write_video_unit(parent: ET.Element, unit: VideoUnit) -> None:
    el = ET.SubElement(parent, "video_unit")
    _add_symbol(el, unit.symbol)
    if unit.object_name:
        _set_text(el, "object_name", unit.object_name)
    _write_localized_ref(el, "localized_description", unit.localized_description)
    _hex16(el, "clock_domain_index", unit.clock_domain_index)
    if unit.input_stream_ports:
        isp = ET.SubElement(el, "input_stream_ports")
        for port in unit.input_stream_ports:
            _write_video_stream_port(isp, port)
    if unit.output_stream_ports:
        osp = ET.SubElement(el, "output_stream_ports")
        for port in unit.output_stream_ports:
            _write_video_stream_port(osp, port)
    if unit.input_external_ports:
        iep = ET.SubElement(el, "input_external_ports")
        for port in unit.input_external_ports:
            _write_external_port(iep, port)
    if unit.output_external_ports:
        oep = ET.SubElement(el, "output_external_ports")
        for port in unit.output_external_ports:
            _write_external_port(oep, port)
    if unit.input_internal_ports:
        iip = ET.SubElement(el, "input_internal_ports")
        for port in unit.input_internal_ports:
            _write_internal_port(iip, port)
    if unit.output_internal_ports:
        oip = ET.SubElement(el, "output_internal_ports")
        for port in unit.output_internal_ports:
            _write_internal_port(oip, port)
    if unit.controls:
        ctrls = ET.SubElement(el, "controls")
        for ctrl in unit.controls:
            _write_control(ctrls, ctrl)


def _write_sensor_unit(parent: ET.Element, unit: SensorUnit) -> None:
    el = ET.SubElement(parent, "sensor_unit")
    _add_symbol(el, unit.symbol)
    if unit.object_name:
        _set_text(el, "object_name", unit.object_name)
    _write_localized_ref(el, "localized_description", unit.localized_description)
    _hex16(el, "clock_domain_index", unit.clock_domain_index)
    if unit.input_stream_ports:
        isp = ET.SubElement(el, "input_stream_ports")
        for port in unit.input_stream_ports:
            _write_sensor_stream_port(isp, port)
    if unit.output_stream_ports:
        osp = ET.SubElement(el, "output_stream_ports")
        for port in unit.output_stream_ports:
            _write_sensor_stream_port(osp, port)
    if unit.input_external_ports:
        iep = ET.SubElement(el, "input_external_ports")
        for port in unit.input_external_ports:
            _write_external_port(iep, port)
    if unit.output_external_ports:
        oep = ET.SubElement(el, "output_external_ports")
        for port in unit.output_external_ports:
            _write_external_port(oep, port)
    if unit.input_internal_ports:
        iip = ET.SubElement(el, "input_internal_ports")
        for port in unit.input_internal_ports:
            _write_internal_port(iip, port)
    if unit.output_internal_ports:
        oip = ET.SubElement(el, "output_internal_ports")
        for port in unit.output_internal_ports:
            _write_internal_port(oip, port)
    if unit.controls:
        ctrls = ET.SubElement(el, "controls")
        for ctrl in unit.controls:
            _write_control(ctrls, ctrl)


def _write_timing(parent: ET.Element, timing: Timing) -> None:
    el = ET.SubElement(parent, "timing")
    _add_symbol(el, timing.symbol)
    if timing.object_name:
        _set_text(el, "object_name", timing.object_name)
    _write_localized_ref(el, "localized_description", timing.localized_description)
    _write_enum16(el, "algorithm", timing.algorithm, TIMING_ALGORITHM_VALUES)
    if timing.ptp_instance_indices:
        ptp = ET.SubElement(el, "ptp_instances")
        for idx in timing.ptp_instance_indices:
            _hex16(ptp, "ptp_instance_index", idx)


def _write_ptp_port(parent: ET.Element, pp: PtpPort) -> None:
    el = ET.SubElement(parent, "ptp_port")
    _add_symbol(el, pp.symbol)
    if pp.object_name:
        _set_text(el, "object_name", pp.object_name)
    _write_localized_ref(el, "localized_description", pp.localized_description)
    _hex16(el, "port_number", pp.port_number)
    _write_enum16(el, "port_type", pp.port_type, PTP_PORT_TYPE_VALUES)
    _write_flags32(el, "flags", pp.flags, PTP_INSTANCE_FLAGS_NAMES)
    _hex16(el, "avb_interface_index", pp.avb_interface_index)
    prof_bytes = pp.profile_identifier.to_bytes(6, "big")
    _set_text(el, "profile_identifier", ":".join(f"{b:02x}" for b in prof_bytes))


def _write_ptp_instance(parent: ET.Element, pi: PtpInstance) -> None:
    el = ET.SubElement(parent, "ptp_instance")
    _add_symbol(el, pi.symbol)
    if pi.object_name:
        _set_text(el, "object_name", pi.object_name)
    _write_localized_ref(el, "localized_description", pi.localized_description)
    _hex64(el, "clock_identity", pi.clock_identity)
    _write_flags32(el, "flags", pi.flags, PTP_INSTANCE_FLAGS_NAMES)
    _hex16(el, "number_of_controls", pi.number_of_controls)
    _hex16(el, "base_control", pi.base_control)
    if pi.ptp_ports:
        ports = ET.SubElement(el, "ptp_ports")
        for pp in pi.ptp_ports:
            _write_ptp_port(ports, pp)


def _write_configuration(parent: ET.Element, config: Configuration) -> None:
    el = ET.SubElement(parent, "configuration")
    _add_symbol(el, config.symbol)
    if config.object_name:
        _set_text(el, "object_name", config.object_name)
    _write_localized_ref(el, "localized_description", config.localized_description)
    if config.audio_units:
        aus = ET.SubElement(el, "audio_units")
        for au in config.audio_units:
            _write_audio_unit(aus, au)
    if config.video_units:
        vus = ET.SubElement(el, "video_units")
        for vu in config.video_units:
            _write_video_unit(vus, vu)
    if config.sensor_units:
        sus = ET.SubElement(el, "sensor_units")
        for su in config.sensor_units:
            _write_sensor_unit(sus, su)
    if config.streams_input:
        sis = ET.SubElement(el, "input_streams")
        for s in config.streams_input:
            _write_stream(sis, s)
    if config.streams_output:
        sos = ET.SubElement(el, "output_streams")
        for s in config.streams_output:
            _write_stream(sos, s)
    if config.jacks_input:
        jis = ET.SubElement(el, "input_jacks")
        for j in config.jacks_input:
            _write_jack(jis, j)
    if config.jacks_output:
        jos = ET.SubElement(el, "output_jacks")
        for j in config.jacks_output:
            _write_jack(jos, j)
    if config.avb_interfaces:
        ais = ET.SubElement(el, "avb_interfaces")
        for a in config.avb_interfaces:
            _write_avb_interface(ais, a)
    if config.clock_sources:
        css = ET.SubElement(el, "clock_sources")
        for cs in config.clock_sources:
            _write_clock_source(css, cs)
    if config.memory_objects:
        mos = ET.SubElement(el, "memory_objects")
        for mo in config.memory_objects:
            _write_memory_object(mos, mo)
    if config.controls:
        ctrls = ET.SubElement(el, "controls")
        for ctrl in config.controls:
            _write_control(ctrls, ctrl)
    if config.signal_selectors:
        sels = ET.SubElement(el, "signal_selectors")
        for sel in config.signal_selectors:
            _write_signal_selector(sels, sel)
    if config.mixers:
        mixs = ET.SubElement(el, "mixers")
        for mixer in config.mixers:
            _write_mixer(mixs, mixer)
    if config.matrices:
        mats = ET.SubElement(el, "matrices")
        for matrix in config.matrices:
            _write_matrix(mats, matrix)
    if config.splitters:
        sps = ET.SubElement(el, "splitters")
        for sp in config.splitters:
            _write_signal_splitter(sps, sp)
    if config.combiners:
        combs = ET.SubElement(el, "combiners")
        for comb in config.combiners:
            _write_signal_combiner(combs, comb)
    if config.demultiplexers:
        demuxs = ET.SubElement(el, "demultiplexers")
        for demux in config.demultiplexers:
            _write_signal_demultiplexer(demuxs, demux)
    if config.multiplexers:
        muxs = ET.SubElement(el, "multiplexers")
        for mux in config.multiplexers:
            _write_signal_multiplexer(muxs, mux)
    if config.transcoders:
        tcs = ET.SubElement(el, "transcoders")
        for tc in config.transcoders:
            _write_signal_transcoder(tcs, tc)
    if config.control_blocks:
        cbs = ET.SubElement(el, "control_blocks")
        for cb in config.control_blocks:
            _write_control_block(cbs, cb)
    if config.locales:
        locs = ET.SubElement(el, "locales")
        for loc in config.locales:
            _write_locale(locs, loc)
    if config.clock_domains:
        cds = ET.SubElement(el, "clock_domains")
        for cd in config.clock_domains:
            _write_clock_domain(cds, cd)
    if config.timings:
        tms = ET.SubElement(el, "timings")
        for timing in config.timings:
            _write_timing(tms, timing)
    if config.ptp_instances:
        pis = ET.SubElement(el, "ptp_instances")
        for pi in config.ptp_instances:
            _write_ptp_instance(pis, pi)


def write_aemxml(
    entity: Entity, path: str | None = None, schema_year: int | None = None
) -> str:
    """Write entity model to AEMXML string. Optionally write to file.

    Args:
        entity: The entity model to serialize.
        path: Optional file path to write to.
        schema_year: Schema version year (2013 or 2021). If None, uses entity.schema_year.

    Returns the XML string.
    """
    global _schema_year
    if schema_year is None:
        schema_year = entity.schema_year
    _schema_year = schema_year

    root = ET.Element("entity")
    root.set("xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance")
    root.set("xmlns", NS)
    if schema_year == 2013:
        root.set("xsi:schemaLocation", f"{NS} {NS}/avdecc.xsd")
        root.set("version", "1.0")
    else:
        root.set("xsi:schemaLocation", f"{NS} {NS}/atdecc.xsd")
        root.set("version", "2.0")

    _hex64(root, "entity_id", entity.entity_id)
    _hex64(root, "entity_model_id", entity.entity_model_id)
    _write_flags32(
        root,
        "entity_capabilities",
        entity.entity_capabilities,
        ENTITY_CAPABILITIES_NAMES,
    )
    _hex16(root, "talker_stream_sources", entity.talker_stream_sources)
    _write_flags16(
        root,
        "talker_capabilities",
        entity.talker_capabilities,
        TALKER_CAPABILITIES_NAMES,
    )
    _hex16(root, "listener_stream_sinks", entity.listener_stream_sinks)
    _write_flags16(
        root,
        "listener_capabilities",
        entity.listener_capabilities,
        LISTENER_CAPABILITIES_NAMES,
    )
    _write_flags32(
        root,
        "controller_capabilities",
        entity.controller_capabilities,
        CONTROLLER_CAPABILITIES_NAMES,
    )
    if schema_year != 2013 and entity.available_index:
        _hex32(root, "available_index", entity.available_index)
    _hex64(root, "association_id", entity.association_id)
    _set_text(root, "entity_name", entity.entity_name)
    _write_localized_ref(root, "vendor_name", entity.vendor_name_string)
    _write_localized_ref(root, "model_name", entity.model_name_string)
    _set_text(root, "firmware_version", entity.firmware_version)
    _set_text(root, "group_name", entity.group_name)
    _set_text(root, "serial_number", entity.serial_number)
    _hex16(root, "current_configuration", 0)

    configs = ET.SubElement(root, "configurations")
    for config in entity.configurations:
        _write_configuration(configs, config)

    ET.indent(root, space="    ")
    xml_str = ET.tostring(root, encoding="unicode", xml_declaration=True)

    if path:
        with open(path, "w", encoding="utf-8") as f:
            f.write(xml_str)
            f.write("\n")

    return xml_str
