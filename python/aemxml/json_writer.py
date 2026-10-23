# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Write Entity data model to simplified JSON format."""

from __future__ import annotations

import json

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
    Entity,
    ExternalPort,
    Jack,
    Stream,
    DESCRIPTOR_TYPE_VALUES,
    ENTITY_CAPABILITIES_NAMES,
    TALKER_CAPABILITIES_NAMES,
    LISTENER_CAPABILITIES_NAMES,
    STREAM_FLAGS_NAMES,
    JACK_FLAGS_NAMES,
    JACK_TYPE_VALUES,
    CLOCK_SOURCE_TYPE_VALUES,
    INTERFACE_FLAGS_NAMES,
)
from .stream_formats import format_stream_format


def _flags_to_names(value: int, name_map: dict[str, int]) -> list[str]:
    """Convert a bitmask integer to a list of flag name strings."""
    names = []
    remaining = value
    for name, bit in name_map.items():
        if remaining & bit:
            names.append(name)
            remaining &= ~bit
    return names


PULL_VALUES: dict[int, str] = {
    0: "1.0",
    1: "1/1.001",
    2: "1.001",
    3: "24/25",
    4: "25/24",
}


def _decode_sampling_rate(rate: int) -> int | dict:
    """Decode a 4-byte sampling rate.

    Returns:
      - int: plain Hz if pull=0
      - dict: {"base": Hz, "pull": "1/1.001"} if pull != 0
    """
    pull = (rate >> 29) & 0x07
    base = rate & 0x1FFFFFFF
    if pull == 0:
        return base
    pull_name = PULL_VALUES.get(pull, f"0x{pull:02x}")
    return {"base": base, "pull": pull_name}


def _write_cluster(cluster: AudioCluster) -> dict:
    """Write an audio cluster to JSON dict."""
    result: dict = {}
    if cluster.object_name:
        result["name"] = cluster.object_name
    if cluster.channel_count:
        result["channels"] = cluster.channel_count
    if cluster.symbol:
        result["symbol"] = cluster.symbol
    return result


def _write_mapping(m) -> dict:
    """Write an audio mapping to JSON dict."""
    return {
        "stream": m.stream_index,
        "stream_ch": m.stream_channel,
        "cluster": m.cluster_offset,
        "cluster_ch": m.cluster_channel,
    }


def _write_port(port: AudioStreamPort) -> dict:
    """Write a stream port to JSON dict."""
    result: dict = {}
    if port.clusters:
        result["clusters"] = [_write_cluster(c) for c in port.clusters]
    if port.maps:
        # Flatten all mappings from all AudioMap descriptors
        all_maps = []
        for am in port.maps:
            all_maps.extend([_write_mapping(m) for m in am.mappings])
        if all_maps:
            result["maps"] = all_maps
    if port.symbol:
        result["symbol"] = port.symbol
    return result


def _write_stream(stream: Stream) -> dict:
    """Write a stream descriptor to JSON dict."""
    result: dict = {}
    if stream.object_name:
        result["name"] = stream.object_name
    if stream.current_format:
        result["format"] = format_stream_format(stream.current_format)
    if stream.formats:
        fmt_list = [format_stream_format(f) for f in stream.formats]
        # Only include formats if different from [format]
        if len(fmt_list) > 1 or (
            len(fmt_list) == 1
            and stream.current_format
            and fmt_list[0] != format_stream_format(stream.current_format)
        ):
            result["formats"] = fmt_list
    flags = _flags_to_names(stream.stream_flags, STREAM_FLAGS_NAMES)
    if flags:
        result["flags"] = flags
    if stream.buffer_length:
        result["buffer_length"] = stream.buffer_length
    if stream.symbol:
        result["symbol"] = stream.symbol
    return result


def _write_external_port(ep: ExternalPort) -> dict:
    """Write an external port to JSON dict."""
    result: dict = {}
    sig_type = DESCRIPTOR_TYPE_VALUES.get(ep.signal_type, "AUDIO_CLUSTER")
    result["signal_type"] = sig_type
    if ep.signal_index:
        result["signal_index"] = ep.signal_index
    if ep.signal_output:
        result["signal_output"] = ep.signal_output
    if ep.jack_index:
        result["jack_index"] = ep.jack_index
    if ep.symbol:
        result["symbol"] = ep.symbol
    return result


def _write_control(ctrl: Control) -> dict:
    """Write a control descriptor to JSON dict."""
    result: dict = {}
    if ctrl.object_name:
        result["name"] = ctrl.object_name
    if ctrl.control_type:
        result["control_type"] = f"0x{ctrl.control_type:016x}"
    if ctrl.control_value_type:
        result["value_type"] = ctrl.control_value_type
    sig_type = DESCRIPTOR_TYPE_VALUES.get(ctrl.signal_type, "ENTITY")
    result["signal_type"] = sig_type
    if ctrl.signal_index:
        result["signal_index"] = ctrl.signal_index
    if ctrl.signal_output:
        result["signal_output"] = ctrl.signal_output
    if ctrl.value_details:
        result["value_details"] = f"0x{ctrl.value_details.hex()}"
    if ctrl.symbol:
        result["symbol"] = ctrl.symbol
    return result


def _write_audio_unit(au: AudioUnit) -> dict:
    """Write an audio unit to JSON dict."""
    result: dict = {}
    if au.object_name:
        result["name"] = au.object_name
    if au.sampling_rates:
        result["rates"] = [_decode_sampling_rate(r) for r in au.sampling_rates]
    if au.input_stream_ports:
        result["input_ports"] = [_write_port(p) for p in au.input_stream_ports]
    if au.output_stream_ports:
        result["output_ports"] = [_write_port(p) for p in au.output_stream_ports]
    if au.input_external_ports:
        result["external_ports_in"] = [
            _write_external_port(ep) for ep in au.input_external_ports
        ]
    if au.output_external_ports:
        result["external_ports_out"] = [
            _write_external_port(ep) for ep in au.output_external_ports
        ]
    if au.symbol:
        result["symbol"] = au.symbol
    return result


def _write_avb_interface(iface: AvbInterface) -> dict:
    """Write an AVB interface to JSON dict."""
    result: dict = {}
    if iface.object_name:
        result["name"] = iface.object_name
    flags = _flags_to_names(iface.interface_flags, INTERFACE_FLAGS_NAMES)
    if flags:
        result["flags"] = flags
    if iface.symbol:
        result["symbol"] = iface.symbol
    return result


def _write_clock_source(cs: ClockSource) -> dict:
    """Write a clock source to JSON dict."""
    result: dict = {}
    if cs.object_name:
        result["name"] = cs.object_name
    cs_type = CLOCK_SOURCE_TYPE_VALUES.get(cs.clock_source_type, "INTERNAL")
    result["type"] = cs_type
    if cs.symbol:
        result["symbol"] = cs.symbol
    return result


def _write_clock_domain(cd: ClockDomain) -> dict:
    """Write a clock domain to JSON dict."""
    result: dict = {}
    if cd.object_name:
        result["name"] = cd.object_name
    result["source"] = cd.clock_source_index
    if cd.clock_sources:
        result["sources"] = cd.clock_sources
    if cd.symbol:
        result["symbol"] = cd.symbol
    return result


def _write_jack(j: Jack) -> dict:
    """Write a jack descriptor to JSON dict."""
    result: dict = {}
    if j.object_name:
        result["name"] = j.object_name
    jack_type = JACK_TYPE_VALUES.get(j.jack_type, "BALANCED_ANALOG")
    result["type"] = jack_type
    flags = _flags_to_names(j.jack_flags, JACK_FLAGS_NAMES)
    if flags:
        result["flags"] = flags
    if j.symbol:
        result["symbol"] = j.symbol
    return result


def _singular_or_plural(
    items: list, singular: str, plural: str, writer
) -> tuple[str, any]:
    """Return (key, value) using singular form for single item, plural for multiple."""
    written = [writer(item) for item in items]
    if len(written) == 1:
        return singular, written[0]
    return plural, written


def _collect_strings_from_locales(config: Configuration) -> list[str]:
    """Collect all strings from locale/strings descriptors."""
    strings = []
    for locale in config.locales:
        for sd in locale.strings_descriptors:
            strings.extend(sd.strings)
    return strings


def _write_configuration(config: Configuration, entity: Entity) -> dict:
    """Write a configuration to JSON dict."""
    result: dict = {}
    if config.object_name:
        result["name"] = config.object_name
    if config.symbol:
        result["symbol"] = config.symbol

    # Streams (always plural)
    if config.streams_input:
        result["streams_in"] = [_write_stream(s) for s in config.streams_input]
    if config.streams_output:
        result["streams_out"] = [_write_stream(s) for s in config.streams_output]

    # Audio units (singular/plural)
    if config.audio_units:
        key, val = _singular_or_plural(
            config.audio_units, "audio_unit", "audio_units", _write_audio_unit
        )
        result[key] = val

    # AVB interfaces (singular/plural)
    if config.avb_interfaces:
        key, val = _singular_or_plural(
            config.avb_interfaces,
            "avb_interface",
            "avb_interfaces",
            _write_avb_interface,
        )
        result[key] = val

    # Clock sources (singular/plural)
    if config.clock_sources:
        key, val = _singular_or_plural(
            config.clock_sources,
            "clock_source",
            "clock_sources",
            _write_clock_source,
        )
        result[key] = val

    # Clock domains (singular/plural)
    if config.clock_domains:
        key, val = _singular_or_plural(
            config.clock_domains,
            "clock_domain",
            "clock_domains",
            _write_clock_domain,
        )
        result[key] = val

    # Jacks
    if config.jacks_input:
        result["jacks_in"] = [_write_jack(j) for j in config.jacks_input]
    if config.jacks_output:
        result["jacks_out"] = [_write_jack(j) for j in config.jacks_output]

    # Controls
    if config.controls:
        result["controls"] = [_write_control(c) for c in config.controls]

    # Strings
    strings = _collect_strings_from_locales(config)
    if strings:
        result["strings"] = strings

    return result


def write_json(entity: Entity, path: str | None = None) -> str:
    """Write entity to simplified JSON. Returns JSON string.

    Args:
        entity: The Entity data model to serialize.
        path: Optional file path to write to.

    Returns:
        The JSON string.
    """
    entity_obj: dict = {}

    if entity.entity_name:
        entity_obj["name"] = entity.entity_name
    entity_obj["entity_id"] = f"0x{entity.entity_id:016X}"
    entity_obj["model_id"] = f"0x{entity.entity_model_id:016X}"

    caps = _flags_to_names(entity.entity_capabilities, ENTITY_CAPABILITIES_NAMES)
    if caps:
        entity_obj["capabilities"] = caps

    # Talker
    talker_caps = _flags_to_names(entity.talker_capabilities, TALKER_CAPABILITIES_NAMES)
    if talker_caps or entity.talker_stream_sources:
        talker: dict = {}
        if talker_caps:
            talker["capabilities"] = talker_caps
        if entity.talker_stream_sources:
            talker["stream_sources"] = entity.talker_stream_sources
        entity_obj["talker"] = talker

    # Listener
    listener_caps = _flags_to_names(
        entity.listener_capabilities, LISTENER_CAPABILITIES_NAMES
    )
    if listener_caps or entity.listener_stream_sinks:
        listener: dict = {}
        if listener_caps:
            listener["capabilities"] = listener_caps
        if entity.listener_stream_sinks:
            listener["stream_sinks"] = entity.listener_stream_sinks
        entity_obj["listener"] = listener

    # Vendor/model from strings
    vendor = ""
    model_name = ""
    if entity.configurations:
        strings = _collect_strings_from_locales(entity.configurations[0])
        if len(strings) > 0:
            vendor = strings[0]
        if len(strings) > 1:
            model_name = strings[1]

    if vendor:
        entity_obj["vendor"] = vendor
    if model_name:
        entity_obj["model"] = model_name
    if entity.firmware_version:
        entity_obj["firmware"] = entity.firmware_version
    if entity.group_name:
        entity_obj["group"] = entity.group_name
    if entity.serial_number:
        entity_obj["serial"] = entity.serial_number
    if entity.milan is not None:
        entity_obj["milan"] = entity.milan

    # Configurations (singular/plural)
    if entity.configurations:
        configs = [_write_configuration(c, entity) for c in entity.configurations]
        if len(configs) == 1:
            entity_obj["configuration"] = configs[0]
        else:
            entity_obj["configurations"] = configs

    data = {"entity": entity_obj}
    json_str = json.dumps(data, indent=2, ensure_ascii=False)

    if path is not None:
        with open(path, "w") as f:
            f.write(json_str)
            f.write("\n")

    return json_str
