#!/usr/bin/env python3
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Verify every declarative examples/*.json AEM model compiles and parses.

Each model is run through the aemxml pipeline (read_json -> flatten ->
write_blob) and the resulting blob is parsed back with blob_reader.read_blob.
This is the build gate that keeps the JSON models — the single source of truth
for the shipped entity_*.bin descriptor-storage blobs — compilable at all
times. Deeper coverage lives in aemxml_test.py (pipeline unit suite) and
descriptor_storage_cross_check.py (Python writer vs the C++ DescriptorStorage
parser, the runtime consumer).

Each model is also held to the Milan v1.3 descriptor rules that a model alone
can meet or break (section 5.3.3; see milan_model_faults), so an edit that
breaks one of them fails the build and not a certification run.

Usage: verify_models.py
"""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent  # avb/examples
sys.path.insert(0, str(HERE.parent / "python"))

from aemxml.blob_reader import read_blob  # noqa: E402
from aemxml.blob_writer import write_blob  # noqa: E402
from aemxml.flatten import flatten  # noqa: E402
from aemxml.json_reader import read_json  # noqa: E402
from aemxml.model import (  # noqa: E402
    CLOCK_SOURCE_TYPE_NAMES,
    DESCRIPTOR_STREAM_INPUT,
    DESCRIPTOR_TIMING,
    INTERFACE_FLAGS_NAMES,
    PTP_INSTANCE_FLAGS_NAMES,
    TIMING_ALGORITHM_NAMES,
)
from aemxml.stream_formats import (  # noqa: E402
    AAF_FORMAT_INT_32,
    decode_aaf_stream_format,
    stream_format_covers,
)

# Milan v1.3 5.3.3.4: a STREAM_INPUT's buffer_length is at least 2.126 ms.
MILAN_MIN_BUFFER_LENGTH_NS = 2126000
# Milan v1.3 5.3.3.5: both set in an AVB_INTERFACE's interface_flags.
MILAN_INTERFACE_FLAGS = (
    INTERFACE_FLAGS_NAMES["GPTP_SUPPORTED"] | INTERFACE_FLAGS_NAMES["SRP_SUPPORTED"]
)
# Milan v1.3 Table 6.1: the channel counts of the Base format, and its samples
# per AVTPDU at each sampling rate.
MILAN_BASE_CHANNELS = frozenset({1, 2, 4, 6, 8})
MILAN_BASE_SAMPLES = {
    44100: 6,
    48000: 6,
    88200: 12,
    96000: 12,
    176400: 24,
    192000: 24,
}


def _base_formats_described(fmt: int) -> tuple[int, set[int]] | None:
    """The rate and the channel counts of the Base formats a formats-list
    entry describes; None for an entry that is no Base format."""
    aaf = decode_aaf_stream_format(fmt)
    if (
        aaf is None
        or aaf["format"] != AAF_FORMAT_INT_32
        or aaf["depth"] != 32
        or MILAN_BASE_SAMPLES.get(aaf["rate"]) != aaf["samples_per_frame"]
    ):
        return None
    top = aaf["channels"]
    up_to = aaf.get("up_to", False)
    counts = {n for n in MILAN_BASE_CHANNELS if (n <= top if up_to else n == top)}
    return (aaf["rate"], counts) if counts else None


def _input_format_faults(config) -> list[str]:
    """6.5: a Stream Input that lists a Base format at a rate lists them all,
    1, 2, 4, 6 and 8 channels."""
    faults = []
    for stream in config.streams_input:
        by_rate: dict[int, set[int]] = {}
        for described in map(_base_formats_described, stream.formats):
            if described is not None:
                by_rate.setdefault(described[0], set()).update(described[1])
        faults += [
            f"STREAM_INPUT '{stream.object_name}' lists the Base format at {rate} Hz "
            f"for {sorted(counts)} channels and not for {sorted(MILAN_BASE_CHANNELS - counts)}"
            for rate, counts in by_rate.items()
            if counts != MILAN_BASE_CHANNELS
        ]
    return faults


# The AVTP subtype octet of a stream format: AAF, and CRF.
AAF_SUBTYPE = 0x02
CRF_SUBTYPE = 0x04


def _streams_of(streams, subtype: int, domain: int) -> list:
    """The streams of @p streams in clock domain @p domain whose current
    format has the AVTP subtype @p subtype."""
    return [
        s
        for s in streams
        if (s.current_format >> 56) == subtype and s.clock_domain_index == domain
    ]


def _media_clock_stream_faults(config) -> list[str]:
    """7.2.2 and 7.2.3: per clock domain, an AAF Media Talker has a CRF Media
    Clock Input, and an AAF Media Listener with two or more AAF Media Inputs
    has a CRF Media Clock Input and a CRF Media Clock Output."""
    faults = []
    for domain in range(len(config.clock_domains)):
        aaf_out = _streams_of(config.streams_output, AAF_SUBTYPE, domain)
        aaf_in = _streams_of(config.streams_input, AAF_SUBTYPE, domain)
        crf_in = _streams_of(config.streams_input, CRF_SUBTYPE, domain)
        crf_out = _streams_of(config.streams_output, CRF_SUBTYPE, domain)
        if aaf_out and not crf_in:
            faults.append(
                f"clock domain {domain} has an AAF output and no CRF Media Clock Input"
            )
        if len(aaf_in) >= 2 and not (crf_in and crf_out):
            faults.append(
                f"clock domain {domain} has {len(aaf_in)} AAF inputs and lacks a CRF "
                "Media Clock Input or Output"
            )
    return faults


def _stream_input_clock_sources(config, index: int) -> int:
    """How many CLOCK_SOURCE descriptors are STREAM_INPUT @p index as an INPUT_STREAM."""
    return sum(
        1
        for source in config.clock_sources
        if source.clock_source_type == CLOCK_SOURCE_TYPE_NAMES["INPUT_STREAM"]
        and source.clock_source_location_type == DESCRIPTOR_STREAM_INPUT
        and source.clock_source_location_index == index
    )


def _clock_source_faults(config) -> list[str]:
    """5.3.3.6: one INPUT_STREAM CLOCK_SOURCE for each CRF input, or for the
    single AAF input of a configuration with no CRF input; and an INTERNAL one
    where the configuration has a stream output."""
    inputs = list(enumerate(config.streams_input))
    crf = [i for i, s in inputs if (s.current_format >> 56) == CRF_SUBTYPE]
    aaf = [i for i, s in inputs if (s.current_format >> 56) == AAF_SUBTYPE]
    wanted = crf if crf else (aaf if len(aaf) == 1 else [])
    faults = [
        f"STREAM_INPUT {i} has {_stream_input_clock_sources(config, i)} INPUT_STREAM "
        "CLOCK_SOURCE descriptors, not one"
        for i in wanted
        if _stream_input_clock_sources(config, i) != 1
    ]
    internal = CLOCK_SOURCE_TYPE_NAMES["INTERNAL"]
    if config.streams_output and not any(
        s.clock_source_type == internal for s in config.clock_sources
    ):
        faults.append(
            "a configuration with a stream output has no INTERNAL CLOCK_SOURCE"
        )
    return faults


def _timing_chain_faults(config) -> list[str]:
    """5.3.3.13 and 5.3.3.14: a configuration with a TIMING has a
    PTP_INSTANCE, and one with a PTP_INSTANCE has a PTP_PORT. Here every
    PTP_INSTANCE has a port of its own, and a TIMING names instances there are."""
    faults = []
    if config.timings and not config.ptp_instances:
        faults.append("a configuration with a TIMING has no PTP_INSTANCE")
    faults += [
        f"PTP_INSTANCE {i} has no PTP_PORT"
        for i, instance in enumerate(config.ptp_instances)
        if not instance.ptp_ports
    ]
    faults += [
        f"TIMING {i} names PTP_INSTANCE {n}, which the configuration lacks"
        for i, timing in enumerate(config.timings)
        for n in timing.ptp_instance_indices
        if n >= len(config.ptp_instances)
    ]
    return faults


def _gptp_instance_faults(config, instance_index: int) -> list[str]:
    """7.5.4 and 7.5.5: the PTP_INSTANCE of a gPTP media clock source is
    GRANDMASTER_CAPABLE and has one PTP_PORT, on AVB_INTERFACE 0."""
    if instance_index >= len(config.ptp_instances):
        return []  # _timing_chain_faults says so
    instance = config.ptp_instances[instance_index]
    faults = []
    if not instance.flags & PTP_INSTANCE_FLAGS_NAMES["GRANDMASTER_CAPABLE"]:
        faults.append(f"PTP_INSTANCE {instance_index} is not GRANDMASTER_CAPABLE")
    if len(instance.ptp_ports) != 1:
        faults.append(
            f"PTP_INSTANCE {instance_index} has {len(instance.ptp_ports)} PTP_PORTs, not one"
        )
    elif instance.ptp_ports[0].avb_interface_index != 0:
        faults.append(
            f"the PTP_PORT of PTP_INSTANCE {instance_index} is not on AVB_INTERFACE 0"
        )
    return faults


def _gptp_timing_faults(config, timing_index: int) -> list[str]:
    """7.5.3: the TIMING a gPTP media clock source is located at is SINGLE
    with one PTP_INSTANCE."""
    if timing_index >= len(config.timings):
        return [
            f"a CLOCK_SOURCE is located at TIMING {timing_index}, which the "
            "configuration lacks"
        ]
    timing = config.timings[timing_index]
    if (
        timing.algorithm != TIMING_ALGORITHM_NAMES["SINGLE"]
        or len(timing.ptp_instance_indices) != 1
    ):
        return [f"TIMING {timing_index} is not SINGLE with one PTP_INSTANCE"]
    return _gptp_instance_faults(config, timing.ptp_instance_indices[0])


def _gptp_clock_source_faults(config) -> list[str]:
    """5.3.3.6 and 7.5: gPTP as a media clock source. Its CLOCK_SOURCE is
    EXTERNAL with no flags and located at a TIMING (7.5.2), the configuration
    has a single AVB_INTERFACE (7.5.1), and the TIMING, its PTP_INSTANCE and
    its PTP_PORT are as 7.5.3 to 7.5.5 have them."""
    located = [
        (i, source)
        for i, source in enumerate(config.clock_sources)
        if source.clock_source_location_type == DESCRIPTOR_TIMING
    ]
    faults = []
    for i, source in located:
        if (
            source.clock_source_type != CLOCK_SOURCE_TYPE_NAMES["EXTERNAL"]
            or source.clock_source_flags != 0
        ):
            faults.append(
                f"CLOCK_SOURCE {i} is located at a TIMING and is not EXTERNAL with flags 0"
            )
        faults += _gptp_timing_faults(config, source.clock_source_location_index)
    if located and len(config.avb_interfaces) != 1:
        faults.append(
            "gPTP is a media clock source of a configuration with "
            f"{len(config.avb_interfaces)} AVB_INTERFACEs, not one"
        )
    return faults


def _current_format_faults(config) -> list[str]:
    """5.3.3.4: current_format is one of the formats the formats list describes."""
    return [
        f"{kind} '{stream.object_name}' has a current_format its formats do not describe"
        for kind, streams in (
            ("STREAM_INPUT", config.streams_input),
            ("STREAM_OUTPUT", config.streams_output),
        )
        for stream in streams
        if not any(
            stream_format_covers(f, stream.current_format) for f in stream.formats
        )
    ]


def _output_mapping_faults(config) -> list[str]:
    """5.3.3.9: one static mapping at most for a channel of a Stream Output."""
    seen: set[tuple[int, int]] = set()
    faults = []
    for unit in config.audio_units:
        for port in unit.output_stream_ports:
            for mapping in (m for amap in port.maps for m in amap.mappings):
                key = (mapping.stream_index, mapping.stream_channel)
                if key in seen:
                    faults.append(
                        f"STREAM_OUTPUT {key[0]} channel {key[1]} is mapped twice"
                    )
                seen.add(key)
    return faults


def _cluster_faults(config) -> list[str]:
    """5.3.3.8: an AUDIO_CLUSTER has one channel."""
    return [
        f"AUDIO_CLUSTER '{cluster.object_name}' has {cluster.channel_count} channels"
        for unit in config.audio_units
        for port in unit.input_stream_ports + unit.output_stream_ports
        for cluster in port.clusters
        if cluster.channel_count != 1
    ]


def milan_model_faults(entity) -> list[str]:
    """What in @p entity breaks a Milan v1.3 5.3.3 rule a model decides.

    Not held here: what the entity adds when it runs as a Milan one (the
    ENTITY capabilities of 5.3.3.1, the 2021 descriptor lengths).
    """
    faults = []
    for config in entity.configurations:
        faults += _cluster_faults(config)
        faults += _output_mapping_faults(config)
        faults += _input_format_faults(config)
        faults += _current_format_faults(config)
        faults += _media_clock_stream_faults(config)
        faults += _clock_source_faults(config)
        faults += _timing_chain_faults(config)
        faults += _gptp_clock_source_faults(config)
        faults += [
            f"STREAM_INPUT '{s.object_name}' buffer_length {s.buffer_length} ns"
            for s in config.streams_input
            if s.buffer_length < MILAN_MIN_BUFFER_LENGTH_NS
        ]
        faults += [
            f"AVB_INTERFACE '{i.object_name}' lacks GPTP_SUPPORTED or SRP_SUPPORTED"
            for i in config.avb_interfaces
            if i.interface_flags & MILAN_INTERFACE_FLAGS != MILAN_INTERFACE_FLAGS
        ]
    return faults


def main() -> int:
    failures = 0
    # Variables the models may reference (the build passes the real values via
    # json2bin --set); any OTHER unset ${...} reference still fails the model.
    variables = {"version": "0.0.0-verify"}
    for path in sorted(HERE.glob("*.json")):
        try:
            entity = read_json(str(path), variables=variables)
            if faults := milan_model_faults(entity):
                raise ValueError("not a Milan model: " + "; ".join(faults))
            descs, syms = flatten(entity)
            blob = write_blob(descs, syms)
            parsed_descs, parsed_syms = read_blob(blob)
            if len(parsed_descs) != len(descs) or len(parsed_syms) != len(syms):
                raise ValueError(
                    f"round-trip mismatch: wrote {len(descs)} descriptors / "
                    f"{len(syms)} symbols, read back {len(parsed_descs)} / "
                    f"{len(parsed_syms)}"
                )
            if not parsed_descs:
                raise ValueError("blob contains no descriptors")
        except Exception as e:  # noqa: BLE001 -- report every model, then fail
            failures += 1
            print(f"  {path.stem:14s} FAILED: {e}")
            continue
        print(
            f"  {path.stem:14s} {len(blob):>6} bytes, "
            f"{len(descs):>2} descriptors, {len(syms):>2} symbols"
        )
    if failures:
        print(f"\nFAILED: {failures} model(s) do not compile/parse", file=sys.stderr)
        return 1
    print("\nAll models compile + parse, and keep the Milan model rules.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
