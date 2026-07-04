# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Parse simplified JSON into the AEM Entity data model."""

from __future__ import annotations

import json
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
    Configuration,
    Control,
    Entity,
    ExternalPort,
    Jack,
    Locale,
    LocalizedStringRef,
    Stream,
    StringsDescriptor,
    DESCRIPTOR_TYPE_NAMES,
    ENTITY_CAPABILITIES_NAMES,
    TALKER_CAPABILITIES_NAMES,
    LISTENER_CAPABILITIES_NAMES,
    STREAM_FLAGS_NAMES,
    JACK_FLAGS_NAMES,
    JACK_TYPE_NAMES,
    CLOCK_SOURCE_TYPE_NAMES,
    INTERFACE_FLAGS_NAMES,
)
from .stream_formats import parse_stream_format


def _parse_flags(names: list, name_map: dict[str, int], context: str) -> int:
    """Convert a list of flag name strings to a bitmask integer."""
    result = 0
    for name in names:
        if name not in name_map:
            valid = ", ".join(sorted(name_map.keys()))
            raise ValueError(f"Unknown flag '{name}' in {context}. Valid: {valid}")
        result |= name_map[name]
    return result


def _parse_enum(name: str, name_map: dict[str, int], context: str) -> int:
    """Convert an enum name string to integer value."""
    if name not in name_map:
        valid = ", ".join(sorted(name_map.keys()))
        raise ValueError(f"Unknown value '{name}' in {context}. Valid: {valid}")
    return name_map[name]


def _parse_localized(obj: dict, key: str = "localized_description") -> LocalizedStringRef:
    """Parse a per-descriptor localized-string reference. Accepts either the raw 16-bit
    wire value (offset << 3 | index -- what the C++ models set, e.g. 5) or an explicit
    {"offset": O, "index": I} object. Absent -> the default (0,0 = no localized name)."""
    v = obj.get(key)
    if v is None:
        return LocalizedStringRef()
    if isinstance(v, dict):
        return LocalizedStringRef(offset=int(v.get("offset", 0)), index=int(v.get("index", 0)))
    return LocalizedStringRef(offset=(int(v) >> 3) & 0x1FFF, index=int(v) & 0x07)


def _get_list(obj: dict, singular: str, plural: str) -> list:
    """Get a list from dict accepting singular (object) or plural (array) form."""
    if plural in obj:
        val = obj[plural]
        return val if isinstance(val, list) else [val]
    if singular in obj:
        val = obj[singular]
        return [val] if not isinstance(val, list) else val
    return []


PULL_NAMES: dict[str, int] = {
    "1.0": 0,
    "1/1.001": 1,
    "1.001": 2,
    "24/25": 3,
    "25/24": 4,
}


def _encode_sampling_rate(rate) -> int:
    """Encode a sampling rate.

    Accepts:
      - int: plain Hz value (pull=0)
      - str: hex string "0x..."
      - dict: {"base": 44100, "pull": "1/1.001"}
    """
    if isinstance(rate, dict):
        base = rate["base"]
        pull_str = str(rate.get("pull", "1.0"))
        pull = PULL_NAMES.get(pull_str)
        if pull is None:
            raise ValueError(
                f"Unknown pull value '{pull_str}', valid: {list(PULL_NAMES.keys())}"
            )
        return (pull << 29) | (base & 0x1FFFFFFF)
    if isinstance(rate, str):
        return int(rate, 16)
    # pull=0, base_frequency=rate
    return rate & 0x1FFFFFFF


def _parse_stream_format_field(fmt_obj, context: str) -> int:
    """Parse a stream format from JSON format field."""
    try:
        return parse_stream_format(fmt_obj)
    except (ValueError, KeyError) as e:
        raise ValueError(f"Invalid stream format in {context}: {e}") from e


def _parse_mapping(m: dict, context: str) -> AudioMapping:
    """Parse an audio mapping entry from JSON."""
    return AudioMapping(
        stream_index=m.get("stream", 0),
        stream_channel=m.get("stream_ch", 0),
        cluster_offset=m.get("cluster", 0),
        cluster_channel=m.get("cluster_ch", 0),
    )


def _parse_cluster(c: dict, context: str) -> AudioCluster:
    """Parse an audio cluster from JSON."""
    return AudioCluster(
        object_name=c.get("name", ""),
        localized_description=_parse_localized(c),
        channel_count=c.get("channels", 0),
        format=0x40,  # MBLA default
        symbol=c.get("symbol"),
    )


def _parse_port(port_obj: dict, context: str) -> AudioStreamPort:
    """Parse a stream port (input_port or output_port) from JSON."""
    clusters = [
        _parse_cluster(c, f"{context}.clusters[{i}]")
        for i, c in enumerate(port_obj.get("clusters", []))
    ]
    maps_data = port_obj.get("maps", [])
    audio_maps = []
    if maps_data:
        mappings = [
            _parse_mapping(m, f"{context}.maps[{i}]") for i, m in enumerate(maps_data)
        ]
        audio_maps = [AudioMap(mappings=mappings)]
    return AudioStreamPort(
        clusters=clusters, maps=audio_maps, symbol=port_obj.get("symbol")
    )


def _parse_stream(s: dict, context: str) -> Stream:
    """Parse a stream descriptor from JSON."""
    fmt = s.get("format")
    current_format = 0
    if fmt is not None:
        current_format = _parse_stream_format_field(fmt, f"{context}.format")

    formats_list = s.get("formats", [])
    formats = []
    for i, f in enumerate(formats_list):
        formats.append(_parse_stream_format_field(f, f"{context}.formats[{i}]"))
    if not formats and current_format:
        formats = [current_format]

    flags = _parse_flags(s.get("flags", []), STREAM_FLAGS_NAMES, f"{context}.flags")

    return Stream(
        object_name=s.get("name", ""),
        localized_description=_parse_localized(s),
        current_format=current_format,
        formats=formats,
        stream_flags=flags,
        avb_interface_index=s.get("avb_interface_index", 0),
        clock_domain_index=s.get("clock_domain_index", 0),
        symbol=s.get("symbol"),
    )


def _parse_external_port(ep: dict, context: str) -> ExternalPort:
    """Parse an external port from JSON."""
    sig_type = _parse_enum(
        ep.get("signal_type", "AUDIO_CLUSTER"),
        DESCRIPTOR_TYPE_NAMES,
        f"{context}.signal_type",
    )
    return ExternalPort(
        signal_type=sig_type,
        signal_index=ep.get("signal_index", 0),
        signal_output=ep.get("signal_output", 0),
        jack_index=ep.get("jack_index", 0),
        symbol=ep.get("symbol"),
    )


def _parse_control(c: dict, context: str) -> Control:
    """Parse a control descriptor from JSON."""
    sig_type = _parse_enum(
        c.get("signal_type", "ENTITY"),
        DESCRIPTOR_TYPE_NAMES,
        f"{context}.signal_type",
    )
    control_type = (
        int(c.get("control_type", "0x0"), 16)
        if isinstance(c.get("control_type"), str)
        else c.get("control_type", 0)
    )
    value_details = (
        bytes.fromhex(c["value_details"].replace("0x", ""))
        if "value_details" in c
        else b""
    )
    return Control(
        object_name=c.get("name", ""),
        control_value_type=c.get("value_type", 0),
        control_type=control_type,
        signal_type=sig_type,
        signal_index=c.get("signal_index", 0),
        signal_output=c.get("signal_output", 0),
        value_details=value_details,
        symbol=c.get("symbol"),
    )


def _parse_audio_unit(au: dict, context: str) -> AudioUnit:
    """Parse an audio unit from JSON."""
    rates = [_encode_sampling_rate(r) for r in au.get("rates", [])]
    current_rate = rates[0] if rates else 0

    input_ports = [
        _parse_port(p, f"{context}.input_ports[{i}]")
        for i, p in enumerate(au.get("input_ports", []))
    ]
    output_ports = [
        _parse_port(p, f"{context}.output_ports[{i}]")
        for i, p in enumerate(au.get("output_ports", []))
    ]

    ext_in = [
        _parse_external_port(ep, f"{context}.external_ports_in[{i}]")
        for i, ep in enumerate(au.get("external_ports_in", []))
    ]
    ext_out = [
        _parse_external_port(ep, f"{context}.external_ports_out[{i}]")
        for i, ep in enumerate(au.get("external_ports_out", []))
    ]

    return AudioUnit(
        object_name=au.get("name", ""),
        localized_description=_parse_localized(au),
        current_sampling_rate=current_rate,
        sampling_rates=rates,
        input_stream_ports=input_ports,
        output_stream_ports=output_ports,
        input_external_ports=ext_in,
        output_external_ports=ext_out,
        symbol=au.get("symbol"),
    )


def _parse_avb_interface(iface: dict, context: str) -> AvbInterface:
    """Parse an AVB interface from JSON."""
    flags = _parse_flags(
        iface.get("flags", []), INTERFACE_FLAGS_NAMES, f"{context}.flags"
    )
    return AvbInterface(
        object_name=iface.get("name", ""),
        interface_flags=flags,
        symbol=iface.get("symbol"),
    )


def _parse_clock_source(cs: dict, context: str) -> ClockSource:
    """Parse a clock source from JSON."""
    cs_type = cs.get("type", "INTERNAL")
    return ClockSource(
        object_name=cs.get("name", ""),
        localized_description=_parse_localized(cs),
        clock_source_type=_parse_enum(
            cs_type, CLOCK_SOURCE_TYPE_NAMES, f"{context}.type"
        ),
        clock_source_location_type=cs.get("location_type", 0),
        clock_source_location_index=cs.get("location_index", 0),
        symbol=cs.get("symbol"),
    )


def _parse_clock_domain(cd: dict, context: str) -> ClockDomain:
    """Parse a clock domain from JSON."""
    return ClockDomain(
        object_name=cd.get("name", ""),
        localized_description=_parse_localized(cd),
        clock_source_index=cd.get("source", 0),
        clock_sources=cd.get("sources", [cd.get("source", 0)]),
        symbol=cd.get("symbol"),
    )


def _parse_jack(j: dict, context: str) -> Jack:
    """Parse a jack descriptor from JSON."""
    jack_type = j.get("type", "BALANCED_ANALOG")
    flags = _parse_flags(j.get("flags", []), JACK_FLAGS_NAMES, f"{context}.flags")
    return Jack(
        object_name=j.get("name", ""),
        jack_type=_parse_enum(jack_type, JACK_TYPE_NAMES, f"{context}.type"),
        jack_flags=flags,
        symbol=j.get("symbol"),
    )


def _build_strings_infrastructure(strings: list[str]) -> list[Locale]:
    """Build LOCALE + STRINGS descriptors from a flat string list.

    Strings are packed 7 per STRINGS descriptor per SMPTE/IEEE convention.
    """
    strings_descs = []
    for i in range(0, len(strings), 7):
        chunk = strings[i : i + 7]
        strings_descs.append(StringsDescriptor(strings=chunk))
    return [Locale(locale_identifier="en", strings_descriptors=strings_descs)]


def _collect_strings(entity_obj: dict, config_obj: dict) -> list[str]:
    """Auto-collect strings from entity vendor/model and config name fields."""
    strings = []
    vendor = entity_obj.get("vendor", "")
    model = entity_obj.get("model", "")
    config_name = config_obj.get("name", "")
    strings.append(vendor)
    strings.append(model)
    strings.append(config_name)
    return strings


def _parse_configuration(
    config_obj: dict, entity_obj: dict, context: str
) -> Configuration:
    """Parse a configuration from JSON."""
    # Streams
    streams_in = [
        _parse_stream(s, f"{context}.streams_in[{i}]")
        for i, s in enumerate(config_obj.get("streams_in", []))
    ]
    streams_out = [
        _parse_stream(s, f"{context}.streams_out[{i}]")
        for i, s in enumerate(config_obj.get("streams_out", []))
    ]

    # Audio units (singular/plural)
    au_list = _get_list(config_obj, "audio_unit", "audio_units")
    audio_units = [
        _parse_audio_unit(au, f"{context}.audio_units[{i}]")
        for i, au in enumerate(au_list)
    ]

    # AVB interfaces (singular/plural)
    iface_list = _get_list(config_obj, "avb_interface", "avb_interfaces")
    avb_interfaces = [
        _parse_avb_interface(iface, f"{context}.avb_interfaces[{i}]")
        for i, iface in enumerate(iface_list)
    ]

    # Clock sources (singular/plural)
    cs_list = _get_list(config_obj, "clock_source", "clock_sources")
    clock_sources = [
        _parse_clock_source(cs, f"{context}.clock_sources[{i}]")
        for i, cs in enumerate(cs_list)
    ]

    # Clock domains (singular/plural)
    cd_list = _get_list(config_obj, "clock_domain", "clock_domains")
    clock_domains = [
        _parse_clock_domain(cd, f"{context}.clock_domains[{i}]")
        for i, cd in enumerate(cd_list)
    ]

    # Jacks
    jacks_in = [
        _parse_jack(j, f"{context}.jacks_in[{i}]")
        for i, j in enumerate(config_obj.get("jacks_in", []))
    ]
    jacks_out = [
        _parse_jack(j, f"{context}.jacks_out[{i}]")
        for i, j in enumerate(config_obj.get("jacks_out", []))
    ]

    # Controls
    controls = [
        _parse_control(c, f"{context}.controls[{i}]")
        for i, c in enumerate(config_obj.get("controls", []))
    ]

    # Strings infrastructure
    explicit_strings = config_obj.get("strings")
    if explicit_strings is not None:
        strings = explicit_strings
    else:
        strings = _collect_strings(entity_obj, config_obj)
    locales = _build_strings_infrastructure(strings)

    return Configuration(
        object_name=config_obj.get("name", ""),
        streams_input=streams_in,
        streams_output=streams_out,
        audio_units=audio_units,
        avb_interfaces=avb_interfaces,
        clock_sources=clock_sources,
        clock_domains=clock_domains,
        jacks_input=jacks_in,
        jacks_output=jacks_out,
        controls=controls,
        locales=locales,
        symbol=config_obj.get("symbol"),
    )


def read_json(path_or_string: str) -> Entity:
    """Read a simplified JSON file or string and return an Entity.

    Args:
        path_or_string: Either a file path or a JSON string.

    Returns:
        Entity data model populated from the JSON.
    """
    # Try as file first
    if not path_or_string.lstrip().startswith("{"):
        if os.path.exists(path_or_string):
            with open(path_or_string, "r") as f:
                data = json.load(f)
        else:
            data = json.loads(path_or_string)
    else:
        data = json.loads(path_or_string)

    entity_obj = data.get("entity", data)

    # Entity-level fields
    entity_id = (
        int(entity_obj.get("entity_id", "0x0"), 16)
        if isinstance(entity_obj.get("entity_id"), str)
        else entity_obj.get("entity_id", 0)
    )

    model_id = (
        int(entity_obj.get("model_id", "0x0"), 16)
        if isinstance(entity_obj.get("model_id"), str)
        else entity_obj.get("model_id", 0)
    )

    entity_caps = _parse_flags(
        entity_obj.get("capabilities", []),
        ENTITY_CAPABILITIES_NAMES,
        "entity.capabilities",
    )

    talker = entity_obj.get("talker", {})
    talker_caps = _parse_flags(
        talker.get("capabilities", []),
        TALKER_CAPABILITIES_NAMES,
        "entity.talker.capabilities",
    )
    talker_sources = talker.get("stream_sources", 0)

    listener = entity_obj.get("listener", {})
    listener_caps = _parse_flags(
        listener.get("capabilities", []),
        LISTENER_CAPABILITIES_NAMES,
        "entity.listener.capabilities",
    )
    listener_sinks = listener.get("stream_sinks", 0)

    vendor = entity_obj.get("vendor", "")
    model = entity_obj.get("model", "")

    # Vendor name string ref -> position 0 in strings
    vendor_ref = (
        LocalizedStringRef(offset=0, index=0) if vendor else LocalizedStringRef()
    )
    model_ref = LocalizedStringRef(offset=0, index=1) if model else LocalizedStringRef()

    # Configurations (singular/plural)
    config_list = _get_list(entity_obj, "configuration", "configurations")
    configurations = [
        _parse_configuration(c, entity_obj, f"entity.configurations[{i}]")
        for i, c in enumerate(config_list)
    ]

    return Entity(
        entity_id=entity_id,
        entity_model_id=model_id,
        entity_capabilities=entity_caps,
        talker_stream_sources=talker_sources,
        talker_capabilities=talker_caps,
        listener_stream_sinks=listener_sinks,
        listener_capabilities=listener_caps,
        entity_name=entity_obj.get("name", ""),
        vendor_name_string=vendor_ref,
        model_name_string=model_ref,
        firmware_version=entity_obj.get("firmware", ""),
        group_name=entity_obj.get("group", ""),
        serial_number=entity_obj.get("serial", ""),
        configurations=configurations,
    )
