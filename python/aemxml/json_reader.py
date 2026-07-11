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
    ControlBlock,
    Entity,
    ExternalPort,
    Jack,
    Locale,
    LocalizedStringRef,
    SignalSelector,
    SignalSource,
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
from .control_values import (
    CONTROL_TYPE_NAMES,
    ControlValueType,
    LinearValue,
    SelectorValue,
    UnitsCode,
    Utf8Value,
    is_linear_type,
    is_selector_type,
    linear_item_size,
    serialize_value_details,
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


def _no_string_ref() -> LocalizedStringRef:
    """The AEM NO_STRING localized reference (packs to 0xFFFF)."""
    return LocalizedStringRef(offset=0x1FFF, index=7)


def _as_locale_dict(value, context: str = "") -> dict:
    """Normalize a vendor/model/localized value to {locale-or-None: text}. A plain
    string is language-independent (key None): it appears verbatim in every locale."""
    if isinstance(value, dict):
        if not value:
            raise ValueError(f"localized strings dict in {context} is empty")
        for k, v in value.items():
            if not isinstance(k, str) or not isinstance(v, str):
                raise ValueError(
                    f"localized strings dict in {context} must map locale "
                    f"identifiers to strings"
                )
        return dict(value)
    return {None: str(value or "")}


def _string_for_locale(entry: dict, lang: str | None) -> str:
    """The entry's text for `lang`, falling back to the language-independent text
    or the entry's first-listed language (so a missing translation shows the
    primary text rather than an empty name)."""
    if lang in entry:
        return entry[lang]
    if None in entry:
        return entry[None]
    return next(iter(entry.values()))


class _LocalizedStrings:
    """Per-configuration collector for dict-form localized strings.

    A localized_description may be authored as a {locale: text} dict right at the
    field site (e.g. {"en-US": "AAF Audio", "de-DE": "AAF-Audio"}). Each unique
    dict gets a slot in an auto-generated strings table, and the LOCALE + STRINGS
    descriptors for every referenced language are synthesized from that table with
    an identical layout per locale, so a single reference resolves in all of them.
    Slots 0 and 1 are reserved for the entity vendor and model names, keeping the
    ENTITY descriptor's fixed (0,0)/(0,1) references valid.
    """

    def __init__(self, vendor, model) -> None:
        self._entries: list[dict] = [_as_locale_dict(vendor), _as_locale_dict(model)]
        self._slots: dict[tuple, int] = {}
        self.authored = False

    def add(self, value: dict, context: str) -> LocalizedStringRef:
        entry = _as_locale_dict(value, context)
        key = tuple(sorted(entry.items()))
        slot = self._slots.get(key)
        if slot is None:
            self._entries.append(entry)
            slot = len(self._entries) - 1
            self._slots[key] = slot
        self.authored = True
        return LocalizedStringRef(offset=slot // 7, index=slot % 7)

    def build_locales(self) -> list[Locale]:
        # Language order = first appearance in authoring order.
        languages: list[str] = []
        for entry in self._entries:
            for lang in entry:
                if lang is not None and lang not in languages:
                    languages.append(lang)
        if not languages:
            languages = ["en"]
        locales = []
        for lang in languages:
            strings = [_string_for_locale(entry, lang) for entry in self._entries]
            descs = [
                StringsDescriptor(strings=strings[i : i + 7])
                for i in range(0, len(strings), 7)
            ]
            locales.append(Locale(locale_identifier=lang, strings_descriptors=descs))
        return locales


def _parse_localized(
    obj: dict,
    strings: _LocalizedStrings,
    key: str = "localized_description",
    context: str = "",
) -> LocalizedStringRef:
    """Parse a per-descriptor localized-string reference. Accepts:
    - a {locale: text} dict, e.g. {"en-US": "AAF Audio"} -- the string table
      and the LOCALE + STRINGS descriptors are generated automatically;
    - the raw 16-bit wire value (offset << 3 | index -- what the retired C++
      models set, e.g. 5), for hand-managed 'strings' tables;
    - an explicit {"offset": O, "index": I} object;
    - absent -> NO_STRING (0xFFFF)."""
    v = obj.get(key)
    if v is None:
        return _no_string_ref()
    if isinstance(v, dict):
        if set(v.keys()) <= {"offset", "index"}:
            return LocalizedStringRef(
                offset=int(v.get("offset", 0)), index=int(v.get("index", 0))
            )
        return strings.add(v, context or key)
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


def _parse_cluster(c: dict, strings: _LocalizedStrings, context: str) -> AudioCluster:
    """Parse an audio cluster from JSON."""
    return AudioCluster(
        object_name=c.get("name", ""),
        localized_description=_parse_localized(c, strings, context=context),
        channel_count=c.get("channels", 0),
        format=0x40,  # MBLA default
        symbol=c.get("symbol"),
    )


def _parse_port(
    port_obj: dict, strings: _LocalizedStrings, context: str
) -> AudioStreamPort:
    """Parse a stream port (input_port or output_port) from JSON."""
    clusters = [
        _parse_cluster(c, strings, f"{context}.clusters[{i}]")
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


def _parse_stream(s: dict, strings: _LocalizedStrings, context: str) -> Stream:
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
        localized_description=_parse_localized(s, strings, context=context),
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


def _parse_control_value_type(c: dict, context: str) -> int:
    """value_type: a ControlValueType name (e.g. "LINEAR_UINT8") or the raw
    integer code. The read_only / unsettable booleans set the R/U flag bits."""
    v = c.get("value_type", 0)
    if isinstance(v, str):
        try:
            value_type = int(ControlValueType[v])
        except KeyError:
            valid = ", ".join(m.name for m in ControlValueType)
            raise ValueError(
                f"Unknown value_type '{v}' in {context}. Valid: {valid}"
            ) from None
    else:
        value_type = int(v)
    if c.get("read_only"):
        value_type |= 0x8000
    if c.get("unsettable"):
        value_type |= 0x4000
    return value_type


def _parse_control_type(c: dict, context: str) -> int:
    """control_type: a standard control-type name (e.g. "IDENTIFY"), a hex
    EUI-64 string, or a raw integer."""
    v = c.get("control_type", 0)
    if isinstance(v, str):
        if v.lower().startswith("0x"):
            return int(v, 16)
        ct = CONTROL_TYPE_NAMES.get(v)
        if ct is None:
            raise ValueError(
                f"Unknown control_type '{v}' in {context}. Use a standard name "
                f"({', '.join(sorted(CONTROL_TYPE_NAMES))}) or a 0x… EUI-64."
            )
        return ct
    return int(v)


def _parse_unit(v, context: str) -> int:
    """A UnitsCode name (e.g. "LEVEL_DB") or the raw integer code."""
    if isinstance(v, str):
        try:
            return int(UnitsCode[v])
        except KeyError:
            valid = ", ".join(m.name for m in UnitsCode)
            raise ValueError(
                f"Unknown unit '{v}' in {context}. Valid: {valid}"
            ) from None
    return int(v)


def _parse_value_string_ref(
    item: dict, strings: _LocalizedStrings, context: str
) -> int:
    """A control value item's string_ref, packed to its 16-bit wire form.
    Accepts the same forms as localized_description; absent -> NO_STRING."""
    ref = _parse_localized(item, strings, key="string_ref", context=context)
    return ((ref.offset & 0x1FFF) << 3) | (ref.index & 0x07)


def _parse_control_values(
    c: dict, value_type: int, strings: _LocalizedStrings, context: str
) -> bytes:
    """Encode a control's value payload. The typed 'values' form covers the
    LINEAR_* types (list of {current/min/max/step/default/unit/string_ref}),
    the numeric SELECTOR_* types ({current/default/options/unit/string_ref}),
    and UTF8 (a plain string); anything else is authored as raw hex bytes in
    'value_details'."""
    values = c.get("values")
    if values is None:
        if "value_details" not in c:
            return b""
        raw = bytes.fromhex(c["value_details"].replace("0x", ""))
        item_size = linear_item_size(value_type)
        if item_size is not None and len(raw) % item_size != 0:
            raise ValueError(
                f"{context}: value_details is {len(raw)} bytes but this LINEAR "
                f"type's value items are {item_size} bytes each"
            )
        return raw
    if "value_details" in c:
        raise ValueError(
            f"{context}: give either typed 'values' or raw 'value_details', not both"
        )
    if is_linear_type(value_type):
        items = values if isinstance(values, list) else [values]
        linear = [
            LinearValue(
                current=i.get("current", i.get("default", 0)),
                minimum=i.get("min", i.get("minimum", 0)),
                maximum=i.get("max", i.get("maximum", 0)),
                step=i.get("step", 0),
                default_value=i.get("default", i.get("default_value", 0)),
                unit=_parse_unit(i.get("unit", 0), f"{context}.values"),
                string_ref=_parse_value_string_ref(i, strings, f"{context}.values"),
            )
            for i in items
        ]
        return serialize_value_details(value_type, linear)
    if is_selector_type(value_type):
        items = values if isinstance(values, list) else [values]
        selector = [
            SelectorValue(
                current=i.get("current", i.get("default", 0)),
                default_value=i.get("default", i.get("default_value", 0)),
                options=i.get("options", []),
                unit=_parse_unit(i.get("unit", 0), f"{context}.values"),
                string_ref=_parse_value_string_ref(i, strings, f"{context}.values"),
            )
            for i in items
        ]
        return serialize_value_details(value_type, selector)
    if (value_type & 0x3FFF) == ControlValueType.UTF8:
        if not isinstance(values, str):
            raise ValueError(f"{context}: a UTF8 control's 'values' is a string")
        return serialize_value_details(value_type, Utf8Value(values))
    raise ValueError(
        f"{context}: typed 'values' is not supported for this value_type; "
        f"author raw 'value_details' hex bytes instead"
    )


def _parse_control(c: dict, strings: _LocalizedStrings, context: str) -> Control:
    """Parse a control descriptor from JSON."""
    sig_type = _parse_enum(
        c.get("signal_type", "ENTITY"),
        DESCRIPTOR_TYPE_NAMES,
        f"{context}.signal_type",
    )
    value_type = _parse_control_value_type(c, context)
    return Control(
        object_name=c.get("name", ""),
        localized_description=_parse_localized(c, strings, context=context),
        block_latency=c.get("block_latency", 0),
        control_latency=c.get("control_latency", 0),
        control_domain=c.get("control_domain", 0),
        control_value_type=value_type,
        control_type=_parse_control_type(c, context),
        reset_time=c.get("reset_time", 0),
        signal_type=sig_type,
        signal_index=c.get("signal_index", 0),
        signal_output=c.get("signal_output", 0),
        value_details=_parse_control_values(c, value_type, strings, context),
        symbol=c.get("symbol"),
    )


def _parse_signal_source(s: dict, context: str) -> SignalSource:
    """Parse a {signal_type, signal_index, signal_output} source reference."""
    if not isinstance(s, dict) or "signal_type" not in s:
        raise ValueError(
            f"{context}: a signal source is an object with a "
            f"'signal_type' (and optional signal_index/signal_output)"
        )
    return SignalSource(
        signal_type=_parse_enum(
            s["signal_type"], DESCRIPTOR_TYPE_NAMES, f"{context}.signal_type"
        ),
        signal_index=s.get("signal_index", 0),
        signal_output=s.get("signal_output", 0),
    )


def _resolve_source_ref(
    ref, sources: list[SignalSource], field_name: str, context: str
) -> SignalSource:
    """A 'current'/'default' selector position: an integer index into
    `sources`, or an inline {signal_type, ...} object."""
    if isinstance(ref, int):
        if not 0 <= ref < len(sources):
            raise ValueError(
                f"{context}.{field_name}: index {ref} is out of range for "
                f"{len(sources)} sources"
            )
        return sources[ref]
    if isinstance(ref, dict):
        return _parse_signal_source(ref, f"{context}.{field_name}")
    raise ValueError(
        f"{context}.{field_name}: expected a source index or a signal object"
    )


def _parse_signal_selector(
    sel: dict, strings: _LocalizedStrings, context: str
) -> SignalSelector:
    """Parse a SIGNAL_SELECTOR descriptor from JSON."""
    sources = [
        _parse_signal_source(s, f"{context}.sources[{i}]")
        for i, s in enumerate(sel.get("sources", []))
    ]
    if not sources:
        raise ValueError(f"{context}: a signal selector needs at least one source")
    current = _resolve_source_ref(sel.get("current", 0), sources, "current", context)
    default = _resolve_source_ref(
        sel.get("default", sel.get("current", 0)), sources, "default", context
    )
    return SignalSelector(
        object_name=sel.get("name", ""),
        localized_description=_parse_localized(sel, strings, context=context),
        block_latency=sel.get("block_latency", 0),
        control_latency=sel.get("control_latency", 0),
        control_domain=sel.get("control_domain", 0),
        current_signal_type=current.signal_type,
        current_signal_index=current.signal_index,
        current_signal_output=current.signal_output,
        default_signal_type=default.signal_type,
        default_signal_index=default.signal_index,
        default_signal_output=default.signal_output,
        sources=sources,
        symbol=sel.get("symbol"),
    )


def _parse_control_block(
    cb: dict, strings: _LocalizedStrings, context: str
) -> ControlBlock:
    """Parse a CONTROL_BLOCK descriptor from JSON. Member controls are
    authored inline; flatten assigns their indices and fills the
    number_of_controls / base_control / final_control_index fields."""
    controls = [
        _parse_control(c, strings, f"{context}.controls[{i}]")
        for i, c in enumerate(cb.get("controls", []))
    ]
    if not controls:
        raise ValueError(f"{context}: a control block needs at least one control")
    sig_type = _parse_enum(
        cb.get("signal_type", "ENTITY"),
        DESCRIPTOR_TYPE_NAMES,
        f"{context}.signal_type",
    )
    return ControlBlock(
        object_name=cb.get("name", ""),
        localized_description=_parse_localized(cb, strings, context=context),
        signal_type=sig_type,
        signal_index=cb.get("signal_index", 0),
        signal_output=cb.get("signal_output", 0),
        controls=controls,
        symbol=cb.get("symbol"),
    )


def _parse_audio_unit(au: dict, strings: _LocalizedStrings, context: str) -> AudioUnit:
    """Parse an audio unit from JSON."""
    rates = [_encode_sampling_rate(r) for r in au.get("rates", [])]
    current_rate = rates[0] if rates else 0

    input_ports = [
        _parse_port(p, strings, f"{context}.input_ports[{i}]")
        for i, p in enumerate(au.get("input_ports", []))
    ]
    output_ports = [
        _parse_port(p, strings, f"{context}.output_ports[{i}]")
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
        localized_description=_parse_localized(au, strings, context=context),
        current_sampling_rate=current_rate,
        sampling_rates=rates,
        input_stream_ports=input_ports,
        output_stream_ports=output_ports,
        input_external_ports=ext_in,
        output_external_ports=ext_out,
        symbol=au.get("symbol"),
    )


def _parse_avb_interface(
    iface: dict, strings: _LocalizedStrings, context: str
) -> AvbInterface:
    """Parse an AVB interface from JSON."""
    flags = _parse_flags(
        iface.get("flags", []), INTERFACE_FLAGS_NAMES, f"{context}.flags"
    )
    controls = [
        _parse_control(c, strings, f"{context}.controls[{i}]")
        for i, c in enumerate(iface.get("controls", []))
    ]
    return AvbInterface(
        object_name=iface.get("name", ""),
        localized_description=_parse_localized(iface, strings, context=context),
        interface_flags=flags,
        controls=controls,
        symbol=iface.get("symbol"),
    )


def _parse_clock_source(
    cs: dict, strings: _LocalizedStrings, context: str
) -> ClockSource:
    """Parse a clock source from JSON."""
    cs_type = cs.get("type", "INTERNAL")
    return ClockSource(
        object_name=cs.get("name", ""),
        localized_description=_parse_localized(cs, strings, context=context),
        clock_source_type=_parse_enum(
            cs_type, CLOCK_SOURCE_TYPE_NAMES, f"{context}.type"
        ),
        clock_source_location_type=cs.get("location_type", 0),
        clock_source_location_index=cs.get("location_index", 0),
        symbol=cs.get("symbol"),
    )


def _parse_clock_domain(
    cd: dict, strings: _LocalizedStrings, context: str
) -> ClockDomain:
    """Parse a clock domain from JSON."""
    return ClockDomain(
        object_name=cd.get("name", ""),
        localized_description=_parse_localized(cd, strings, context=context),
        clock_source_index=cd.get("source", 0),
        clock_sources=cd.get("sources", [cd.get("source", 0)]),
        symbol=cd.get("symbol"),
    )


def _parse_jack(j: dict, strings: _LocalizedStrings, context: str) -> Jack:
    """Parse a jack descriptor from JSON."""
    jack_type = j.get("type", "BALANCED_ANALOG")
    flags = _parse_flags(j.get("flags", []), JACK_FLAGS_NAMES, f"{context}.flags")
    return Jack(
        object_name=j.get("name", ""),
        localized_description=_parse_localized(j, strings, context=context),
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
    vendor = _string_for_locale(_as_locale_dict(entity_obj.get("vendor", "")), "en")
    model = _string_for_locale(_as_locale_dict(entity_obj.get("model", "")), "en")
    config_name = config_obj.get("name", "")
    strings.append(vendor)
    strings.append(model)
    strings.append(config_name)
    return strings


def _parse_configuration(
    config_obj: dict, entity_obj: dict, context: str
) -> Configuration:
    """Parse a configuration from JSON."""
    # Dict-form localized strings authored anywhere in this configuration are
    # collected here; the LOCALE + STRINGS descriptors are generated at the end.
    loc = _LocalizedStrings(entity_obj.get("vendor", ""), entity_obj.get("model", ""))

    localized_description = _parse_localized(
        config_obj, loc, context=f"{context}.localized_description"
    )

    # Streams
    streams_in = [
        _parse_stream(s, loc, f"{context}.streams_in[{i}]")
        for i, s in enumerate(config_obj.get("streams_in", []))
    ]
    streams_out = [
        _parse_stream(s, loc, f"{context}.streams_out[{i}]")
        for i, s in enumerate(config_obj.get("streams_out", []))
    ]

    # Audio units (singular/plural)
    au_list = _get_list(config_obj, "audio_unit", "audio_units")
    audio_units = [
        _parse_audio_unit(au, loc, f"{context}.audio_units[{i}]")
        for i, au in enumerate(au_list)
    ]

    # AVB interfaces (singular/plural)
    iface_list = _get_list(config_obj, "avb_interface", "avb_interfaces")
    avb_interfaces = [
        _parse_avb_interface(iface, loc, f"{context}.avb_interfaces[{i}]")
        for i, iface in enumerate(iface_list)
    ]

    # Clock sources (singular/plural)
    cs_list = _get_list(config_obj, "clock_source", "clock_sources")
    clock_sources = [
        _parse_clock_source(cs, loc, f"{context}.clock_sources[{i}]")
        for i, cs in enumerate(cs_list)
    ]

    # Clock domains (singular/plural)
    cd_list = _get_list(config_obj, "clock_domain", "clock_domains")
    clock_domains = [
        _parse_clock_domain(cd, loc, f"{context}.clock_domains[{i}]")
        for i, cd in enumerate(cd_list)
    ]

    # Jacks
    jacks_in = [
        _parse_jack(j, loc, f"{context}.jacks_in[{i}]")
        for i, j in enumerate(config_obj.get("jacks_in", []))
    ]
    jacks_out = [
        _parse_jack(j, loc, f"{context}.jacks_out[{i}]")
        for i, j in enumerate(config_obj.get("jacks_out", []))
    ]

    # Controls
    controls = [
        _parse_control(c, loc, f"{context}.controls[{i}]")
        for i, c in enumerate(config_obj.get("controls", []))
    ]

    # Signal selectors (singular/plural)
    sel_list = _get_list(config_obj, "signal_selector", "signal_selectors")
    signal_selectors = [
        _parse_signal_selector(s, loc, f"{context}.signal_selectors[{i}]")
        for i, s in enumerate(sel_list)
    ]

    # Control blocks (singular/plural)
    cb_list = _get_list(config_obj, "control_block", "control_blocks")
    control_blocks = [
        _parse_control_block(cb, loc, f"{context}.control_blocks[{i}]")
        for i, cb in enumerate(cb_list)
    ]

    # Strings infrastructure: dict-form localized strings own the table when
    # present; otherwise an explicit 'strings' array (with hand-managed integer
    # references), or the legacy vendor/model/config-name fallback.
    explicit_strings = config_obj.get("strings")
    if loc.authored:
        if explicit_strings is not None:
            raise ValueError(
                f"{context}: cannot mix an explicit 'strings' table with "
                f"dict-form localized strings"
            )
        locales = loc.build_locales()
    elif explicit_strings is not None:
        locales = _build_strings_infrastructure(explicit_strings)
    else:
        locales = _build_strings_infrastructure(
            _collect_strings(entity_obj, config_obj)
        )

    return Configuration(
        object_name=config_obj.get("name", ""),
        localized_description=localized_description,
        streams_input=streams_in,
        streams_output=streams_out,
        audio_units=audio_units,
        avb_interfaces=avb_interfaces,
        clock_sources=clock_sources,
        clock_domains=clock_domains,
        jacks_input=jacks_in,
        jacks_output=jacks_out,
        controls=controls,
        signal_selectors=signal_selectors,
        control_blocks=control_blocks,
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
