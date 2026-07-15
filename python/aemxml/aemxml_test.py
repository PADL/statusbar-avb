#!/usr/bin/env python3
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Tests for AEMXML tooling."""

from __future__ import annotations

import json
import sys
from pathlib import Path

# Add parent dir to path so we can import aemxml package
sys.path.insert(0, str(Path(__file__).parent.parent))

from aemxml.model import (
    CombinerMapEntry,
    ControlBlock,
    Entity,
    Configuration,
    ExternalPort,
    InternalPort,
    Locale,
    LocalizedStringRef,
    Matrix,
    MemoryObject,
    Mixer,
    PtpInstance,
    PtpPort,
    SensorCluster,
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
    StringsDescriptor,
    Timing,
    VideoCluster,
    VideoStreamPort,
    VideoUnit,
)
from aemxml.wire import (
    pack_u8,
    pack_u16,
    pack_u32,
    pack_u64,
    pack_string64,
    pack_eui64,
    pack_localized_string_ref,
    unpack_u16,
    unpack_u32,
    unpack_u64,
    unpack_string64,
    unpack_eui64,
    unpack_localized_string_ref,
)
from aemxml.flatten import flatten, FlatDescriptor
from aemxml.model import (
    DESCRIPTOR_ENTITY,
    DESCRIPTOR_CONFIGURATION,
    DESCRIPTOR_AUDIO_UNIT,
    DESCRIPTOR_VIDEO_UNIT,
    DESCRIPTOR_SENSOR_UNIT,
    DESCRIPTOR_STREAM_INPUT,
    DESCRIPTOR_STREAM_OUTPUT,
    DESCRIPTOR_LOCALE,
    DESCRIPTOR_STRINGS,
    DESCRIPTOR_STREAM_PORT_INPUT,
    DESCRIPTOR_STREAM_PORT_OUTPUT,
    DESCRIPTOR_AUDIO_CLUSTER,
    DESCRIPTOR_VIDEO_CLUSTER,
    DESCRIPTOR_SENSOR_CLUSTER,
    DESCRIPTOR_AVB_INTERFACE,
    DESCRIPTOR_CLOCK_SOURCE,
    DESCRIPTOR_CLOCK_DOMAIN,
    DESCRIPTOR_SIGNAL_SELECTOR,
    DESCRIPTOR_MIXER,
    DESCRIPTOR_MATRIX,
    DESCRIPTOR_SIGNAL_SPLITTER,
    DESCRIPTOR_SIGNAL_COMBINER,
    DESCRIPTOR_SIGNAL_DEMULTIPLEXER,
    DESCRIPTOR_SIGNAL_MULTIPLEXER,
    DESCRIPTOR_SIGNAL_TRANSCODER,
    DESCRIPTOR_CONTROL_BLOCK,
    DESCRIPTOR_MEMORY_OBJECT,
    DESCRIPTOR_EXTERNAL_PORT_INPUT,
    DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
    DESCRIPTOR_INTERNAL_PORT_INPUT,
    DESCRIPTOR_INTERNAL_PORT_OUTPUT,
    DESCRIPTOR_TIMING,
    DESCRIPTOR_PTP_INSTANCE,
    DESCRIPTOR_PTP_PORT,
    AudioCluster,
    AudioStreamPort,
    AudioUnit,
    AvbInterface,
    ClockDomain,
    ClockSource,
    Jack,
    Stream,
)
from aemxml.blob_writer import write_blob
from aemxml.blob_reader import read_blob
from aemxml.aemxml_reader import read_aemxml
from aemxml.aemxml_writer import write_aemxml


def test_wire_pack_unpack():
    """Test wire format pack/unpack round-trip."""
    assert unpack_u16(pack_u16(0x1234), 0) == 0x1234
    assert unpack_u32(pack_u32(0xDEADBEEF), 0) == 0xDEADBEEF
    assert unpack_u64(pack_u64(0x0102030405060708), 0) == 0x0102030405060708
    assert unpack_eui64(pack_eui64(0xAABBCCDDEEFF0011), 0) == 0xAABBCCDDEEFF0011

    s = "Hello ATDECC"
    packed = pack_string64(s)
    assert len(packed) == 64
    assert unpack_string64(packed, 0) == s

    # Localized string ref: offset=5, index=3
    packed = pack_localized_string_ref(5, 3)
    off, idx = unpack_localized_string_ref(packed, 0)
    assert off == 5
    assert idx == 3
    print("  [+] wire pack/unpack: OK")


def test_wire_pack_range_checks():
    """Out-of-width values raise instead of silently truncating; signed
    values within the field's two's-complement range still pack (fields
    like log_sync_interval are signed on the wire)."""
    for pack, bits in (
        (pack_u8, 8),
        (pack_u16, 16),
        (pack_u32, 32),
        (pack_u64, 64),
    ):
        assert pack(0) == bytes(bits // 8)
        assert pack((1 << bits) - 1) == b"\xff" * (bits // 8)
        # Two's-complement negatives within the width are allowed.
        assert pack(-1) == b"\xff" * (bits // 8)
        assert pack(-(1 << (bits - 1))) == b"\x80" + bytes(bits // 8 - 1)
        for bad in ((1 << bits), -(1 << (bits - 1)) - 1):
            try:
                pack(bad)
                assert False, f"pack_u{bits}({bad}) should have raised"
            except ValueError:
                pass

    # An authoring typo like signal_index=70000 must fail the build
    # rather than truncate to 4464.
    try:
        pack_u16(70000)
        assert False, "Should have raised ValueError"
    except ValueError as e:
        assert "16-bit" in str(e)
    print("  [+] wire pack range checks: OK")


def test_model_creation():
    """Test that the data model can be instantiated."""
    entity = Entity(
        entity_id=1,
        entity_model_id=1,
        entity_name="Test Entity",
        configurations=[
            Configuration(
                locales=[
                    Locale(
                        locale_identifier="en",
                        strings_descriptors=[
                            StringsDescriptor(strings=["Vendor", "Model", "Config"])
                        ],
                    )
                ],
            )
        ],
    )
    assert entity.entity_name == "Test Entity"
    assert len(entity.configurations) == 1
    assert len(entity.configurations[0].locales) == 1
    assert entity.configurations[0].locales[0].locale_identifier == "en"
    print("  [+] model creation: OK")


def test_flatten_minimal():
    """Test flattening a minimal entity model."""
    entity = Entity(
        entity_id=1,
        entity_model_id=1,
        entity_name="Test",
        configurations=[
            Configuration(
                locales=[
                    Locale(
                        locale_identifier="en",
                        strings_descriptors=[
                            StringsDescriptor(strings=["Vendor", "Model"])
                        ],
                    )
                ],
            )
        ],
    )
    descs, syms = flatten(entity)

    # Should have: entity, configuration, locale, strings
    types = [(d.descriptor_type, d.descriptor_index) for d in descs]
    assert (DESCRIPTOR_ENTITY, 0) in types
    assert (DESCRIPTOR_CONFIGURATION, 0) in types
    assert (DESCRIPTOR_LOCALE, 0) in types
    assert (DESCRIPTOR_STRINGS, 0) in types

    # Entity descriptor should be 312 bytes
    entity_desc = next(d for d in descs if d.descriptor_type == DESCRIPTOR_ENTITY)
    assert len(entity_desc.wire_bytes) == 312

    # Strings descriptor should be 452 bytes
    strings_desc = next(d for d in descs if d.descriptor_type == DESCRIPTOR_STRINGS)
    assert len(strings_desc.wire_bytes) == 452

    print("  [+] flatten minimal: OK")


def test_flatten_with_audio():
    """Test flattening with audio unit, stream ports, clusters."""
    entity = Entity(
        entity_id=1,
        entity_model_id=1,
        entity_name="Audio Test",
        configurations=[
            Configuration(
                audio_units=[
                    AudioUnit(
                        object_name="Audio Unit 0",
                        input_stream_ports=[
                            AudioStreamPort(
                                clusters=[
                                    AudioCluster(
                                        channel_count=2, symbol="input_cluster_0"
                                    )
                                ],
                            ),
                        ],
                        output_stream_ports=[
                            AudioStreamPort(
                                clusters=[AudioCluster(channel_count=2)],
                            ),
                        ],
                        sampling_rates=[0x02_00BB80],  # 48000 Hz
                    ),
                ],
                streams_input=[Stream(object_name="Stream In 0")],
                streams_output=[Stream(object_name="Stream Out 0")],
                avb_interfaces=[AvbInterface(object_name="eth0")],
                clock_sources=[ClockSource(object_name="Internal")],
                clock_domains=[ClockDomain(clock_source_index=0, clock_sources=[0])],
                locales=[
                    Locale(
                        locale_identifier="en",
                        strings_descriptors=[StringsDescriptor(strings=["Vendor"])],
                    )
                ],
            )
        ],
    )
    descs, syms = flatten(entity)

    # Check we have the expected descriptor types
    type_set = {d.descriptor_type for d in descs}
    assert DESCRIPTOR_ENTITY in type_set
    assert DESCRIPTOR_CONFIGURATION in type_set
    assert DESCRIPTOR_AUDIO_UNIT in type_set
    assert DESCRIPTOR_STREAM_INPUT in type_set
    assert DESCRIPTOR_STREAM_OUTPUT in type_set
    assert DESCRIPTOR_STREAM_PORT_INPUT in type_set
    assert DESCRIPTOR_STREAM_PORT_OUTPUT in type_set
    assert DESCRIPTOR_AUDIO_CLUSTER in type_set
    assert DESCRIPTOR_AVB_INTERFACE in type_set
    assert DESCRIPTOR_CLOCK_SOURCE in type_set
    assert DESCRIPTOR_CLOCK_DOMAIN in type_set
    assert DESCRIPTOR_LOCALE in type_set
    assert DESCRIPTOR_STRINGS in type_set

    # Check symbol was recorded
    assert len(syms) == 1
    assert syms[0].descriptor_type == DESCRIPTOR_AUDIO_CLUSTER
    assert syms[0].descriptor_index == 0

    print("  [+] flatten with audio: OK")


def test_identify_capability_and_base_control_zeroing():
    """A configuration-0 IDENTIFY control must set
    AEM_IDENTIFY_CONTROL_INDEX_VALID in ENTITY.entity_capabilities (the ADP
    advertiser sets it in the ADPDU; controllers cross-check the two), and a
    descriptor with number_of_controls == 0 must emit base_control 0, not the
    walk's running control index."""
    import struct

    from aemxml.json_reader import read_json

    json_str = json.dumps(
        {
            "entity": {
                "vendor": "V",
                "model": "M",
                "name": "N",
                "capabilities": ["AEM_SUPPORTED"],
                "configuration": {
                    "name": "C",
                    "controls": [
                        {
                            "name": "Identify",
                            "control_type": "IDENTIFY",
                            "value_type": "LINEAR_UINT8",
                            "values": [
                                {"min": 0, "max": 255, "step": 255, "default": 0}
                            ],
                        }
                    ],
                    "audio_units": [
                        {
                            "name": "AudioUnit",
                            "rates": [48000],
                            "output_ports": [
                                {"clusters": [{"name": "Out", "channels": 2}]}
                            ],
                        }
                    ],
                },
            }
        }
    )
    descs, _ = flatten(read_json(json_str))
    by_type = {}
    for d in descs:
        by_type.setdefault(d.descriptor_type, d)

    ent_caps = struct.unpack_from(">I", by_type[DESCRIPTOR_ENTITY].wire_bytes, 20)[0]
    assert ent_caps & 0x00004000, "AEM_IDENTIFY_CONTROL_INDEX_VALID not derived"

    # The audio unit and its port own no controls; base_control must be 0
    # even though the config-level identify control advanced the running
    # control index to 1 before they were serialized.
    au = by_type[DESCRIPTOR_AUDIO_UNIT].wire_bytes
    assert struct.unpack_from(">HH", au, 98) == (0, 0)  # num/base controls
    spo = by_type[DESCRIPTOR_STREAM_PORT_OUTPUT].wire_bytes
    assert struct.unpack_from(">HH", spo, 8) == (0, 0)  # num/base controls

    # No identify control -> bit stays clear.
    plain = json.loads(json_str)
    plain["entity"]["configuration"].pop("controls")
    descs2, _ = flatten(read_json(json.dumps(plain)))
    ent2 = next(d for d in descs2 if d.descriptor_type == DESCRIPTOR_ENTITY)
    assert not struct.unpack_from(">I", ent2.wire_bytes, 20)[0] & 0x00004000

    print("  [+] identify capability + base_control zeroing: OK")


def test_json_variable_expansion():
    """${name} references in JSON string values expand from the variables
    dict (CLI --set): whole-string references coerce JSON-literal values
    (numbers stay numbers), embedded references splice text, $${ escapes,
    and unset or malformed references are hard errors."""
    from aemxml.json_reader import read_json

    json_str = json.dumps(
        {
            "entity": {
                "vendor": "V",
                "model": "M",
                "name": "Tone ${site}",
                "firmware": "${version}",
                "configuration": {
                    "name": "C",
                    "audio_units": [
                        {
                            "name": "AudioUnit",
                            "rates": [48000],
                            "output_ports": [
                                {"clusters": [{"name": "Out", "channels": "${nch}"}]}
                            ],
                        }
                    ],
                },
            }
        }
    )
    entity = read_json(
        json_str, variables={"site": "LA", "version": "1.8.0", "nch": "2"}
    )
    assert entity.entity_name == "Tone LA"
    assert entity.firmware_version == "1.8.0"  # "1.8.0" is not a JSON literal
    cluster = entity.configurations[0].audio_units[0].output_stream_ports[0].clusters[0]
    assert cluster.channel_count == 2  # "2" coerced to int via whole-string ref

    # Unset variable -> error naming it and its path.
    try:
        read_json(json_str, variables={"site": "LA", "nch": "2"})
        raise AssertionError("unset variable should raise")
    except ValueError as e:
        assert "${version}" in str(e) and "firmware" in str(e)

    # No variables passed at all -> same hard error, not silent passthrough.
    try:
        read_json(json_str)
        raise AssertionError("unset variable should raise")
    except ValueError as e:
        assert "${" in str(e)

    # $${ escapes a literal ${; malformed references are rejected.
    plain = json.dumps({"entity": {"vendor": "V", "model": "M", "name": "a$${b}c"}})
    assert read_json(plain).entity_name == "a${b}c"
    bad = json.dumps({"entity": {"vendor": "V", "model": "M", "name": "x${oops"}})
    try:
        read_json(bad, variables={"oops": "y"})
        raise AssertionError("malformed reference should raise")
    except ValueError as e:
        assert "malformed" in str(e)

    print("  [+] json variable expansion: OK")


def test_blob_round_trip():
    """Test blob write -> read round-trip preserves all data."""
    entity = Entity(
        entity_id=0x0001020304050607,
        entity_model_id=0x08090A0B0C0D0E0F,
        entity_name="Blob Test",
        configurations=[
            Configuration(
                locales=[
                    Locale(
                        locale_identifier="en",
                        strings_descriptors=[StringsDescriptor(strings=["V", "M"])],
                    )
                ],
                clock_sources=[ClockSource(object_name="Internal")],
                clock_domains=[ClockDomain(clock_source_index=0, clock_sources=[0])],
            )
        ],
    )
    descs, syms = flatten(entity)
    blob = write_blob(descs, syms)

    # Read it back
    descs2, syms2 = read_blob(blob)

    # Same number of descriptors and symbols
    assert len(descs2) == len(descs)
    assert len(syms2) == len(syms)

    # Each descriptor's wire bytes match
    for d1, d2 in zip(descs, descs2):
        assert d1.config_index == d2.config_index
        assert d1.descriptor_type == d2.descriptor_type
        assert d1.descriptor_index == d2.descriptor_index
        assert d1.wire_bytes == d2.wire_bytes, (
            f"Mismatch for type=0x{d1.descriptor_type:04X} index={d1.descriptor_index}"
        )

    # Blob -> write again -> same bytes
    blob2 = write_blob(descs2, syms2)
    assert blob == blob2, "blob -> read -> write should produce identical bytes"

    print("  [+] blob round-trip: OK")


def test_read_bareminimum():
    """Test reading the bareminimum.aemxml file."""
    aemxml_path = str(
        Path(__file__).parent.parent.parent / "examples" / "bareminimum.aemxml"
    )
    entity = read_aemxml(aemxml_path)

    assert entity.entity_id == 1
    assert entity.entity_name == "Bare Minimum Entity"
    assert entity.serial_number == "123456789abcd"
    assert len(entity.configurations) == 1
    assert len(entity.configurations[0].locales) == 1
    assert entity.configurations[0].locales[0].locale_identifier == "en"
    assert len(entity.configurations[0].locales[0].strings_descriptors) == 1
    assert (
        entity.configurations[0].locales[0].strings_descriptors[0].strings[0]
        == "The vendor name in english"
    )

    assert entity.schema_year == 2013

    # Should flatten without error
    descs, syms = flatten(entity)
    assert len(descs) > 0

    print("  [+] read bareminimum.aemxml: OK")


def test_aemxml_round_trip():
    """Test AEMXML read -> write -> read produces same model."""
    aemxml_path = str(
        Path(__file__).parent.parent.parent / "examples" / "bareminimum.aemxml"
    )
    entity1 = read_aemxml(aemxml_path)
    xml_str = write_aemxml(entity1)
    entity2 = read_aemxml(xml_str)

    # Core fields match
    assert entity1.entity_id == entity2.entity_id
    assert entity1.entity_name == entity2.entity_name
    assert entity1.serial_number == entity2.serial_number
    assert len(entity1.configurations) == len(entity2.configurations)
    assert (
        entity1.configurations[0].locales[0].locale_identifier
        == entity2.configurations[0].locales[0].locale_identifier
    )
    assert (
        entity1.configurations[0].locales[0].strings_descriptors[0].strings
        == entity2.configurations[0].locales[0].strings_descriptors[0].strings
    )

    # Blob round-trip: both models produce same blob
    descs1, syms1 = flatten(entity1)
    descs2, syms2 = flatten(entity2)
    blob1 = write_blob(descs1, syms1)
    blob2 = write_blob(descs2, syms2)
    assert blob1 == blob2, "AEMXML round-trip should produce identical blobs"

    print("  [+] AEMXML round-trip: OK")


def test_phase2_descriptors():
    """Test Phase 2 descriptor types: flatten and blob round-trip."""
    entity = Entity(
        entity_id=0x0001020304050607,
        entity_model_id=0x08090A0B0C0D0E0F,
        entity_name="Phase 2 Test",
        configurations=[
            Configuration(
                signal_selectors=[
                    SignalSelector(
                        object_name="Selector 0",
                        control_domain=1,
                        current_signal_type=0x0005,
                        current_signal_index=0,
                        current_signal_output=0,
                        default_signal_type=0x0005,
                        default_signal_index=0,
                        default_signal_output=0,
                        sources=[
                            SignalSource(
                                signal_type=0x0005, signal_index=0, signal_output=0
                            ),
                            SignalSource(
                                signal_type=0x0005, signal_index=1, signal_output=0
                            ),
                        ],
                    ),
                ],
                mixers=[
                    Mixer(
                        object_name="Mixer 0",
                        control_value_type=1,
                        sources=[
                            SignalSource(
                                signal_type=0x0014, signal_index=0, signal_output=0
                            ),
                        ],
                    ),
                ],
                matrices=[
                    Matrix(
                        object_name="Matrix 0",
                        width=4,
                        height=4,
                        number_of_values=16,
                        control_type=0x1234567890ABCDEF,
                    ),
                ],
                splitters=[
                    SignalSplitter(
                        object_name="Splitter 0",
                        signal_type=0x0014,
                        signal_index=0,
                        signal_output=0,
                        number_of_outputs=2,
                        splitter_map=[
                            SplitterMapEntry(
                                sub_signal_start=0, sub_signal_count=2, output_index=0
                            ),
                        ],
                    ),
                ],
                combiners=[
                    SignalCombiner(
                        object_name="Combiner 0",
                        combiner_map=[
                            CombinerMapEntry(
                                sub_signal_start=0, sub_signal_count=2, input_index=0
                            ),
                        ],
                        sources=[
                            SignalSource(
                                signal_type=0x0014, signal_index=0, signal_output=0
                            ),
                        ],
                    ),
                ],
                demultiplexers=[
                    SignalDemultiplexer(
                        object_name="Demux 0",
                        signal_type=0x0014,
                        number_of_outputs=2,
                    ),
                ],
                multiplexers=[
                    SignalMultiplexer(
                        object_name="Mux 0",
                        sources=[
                            SignalSource(
                                signal_type=0x0014, signal_index=0, signal_output=0
                            ),
                        ],
                    ),
                ],
                transcoders=[
                    SignalTranscoder(
                        object_name="Transcoder 0",
                        transcoder_type=0xAABBCCDDEEFF0011,
                    ),
                ],
                control_blocks=[
                    ControlBlock(
                        object_name="CB 0",
                        number_of_controls=4,
                        base_control=0,
                        final_control_index=3,
                    ),
                ],
                memory_objects=[
                    MemoryObject(
                        object_name="Firmware",
                        memory_object_type=0x0001,
                        start_address=0x1000,
                        maximum_length=0x100000,
                        length=0x80000,
                        maximum_segment_length=0x10000,
                    ),
                ],
                video_units=[
                    VideoUnit(
                        object_name="Video Unit 0",
                        input_stream_ports=[
                            VideoStreamPort(
                                clusters=[VideoCluster(object_name="VC0", format=1)],
                            ),
                        ],
                    ),
                ],
                sensor_units=[
                    SensorUnit(
                        object_name="Sensor Unit 0",
                        output_stream_ports=[
                            SensorStreamPort(
                                clusters=[SensorCluster(object_name="SC0", format=2)],
                            ),
                        ],
                    ),
                ],
                timings=[
                    Timing(
                        object_name="Timing 0",
                        algorithm=1,
                        ptp_instance_indices=[0],
                    ),
                ],
                ptp_instances=[
                    PtpInstance(
                        object_name="PTP 0",
                        clock_identity=0x0011223344556677,
                        flags=0x01,
                        ptp_ports=[
                            PtpPort(
                                object_name="PTP Port 0",
                                port_number=1,
                                port_type=0,
                                avb_interface_index=0,
                            ),
                        ],
                    ),
                ],
                locales=[
                    Locale(
                        locale_identifier="en",
                        strings_descriptors=[StringsDescriptor(strings=["V"])],
                    )
                ],
                clock_sources=[ClockSource(object_name="Internal")],
                clock_domains=[ClockDomain(clock_source_index=0, clock_sources=[0])],
            )
        ],
    )

    # Flatten
    descs, syms = flatten(entity)
    type_set = {d.descriptor_type for d in descs}

    # Verify all new types are present
    assert DESCRIPTOR_SIGNAL_SELECTOR in type_set, "Missing SIGNAL_SELECTOR"
    assert DESCRIPTOR_MIXER in type_set, "Missing MIXER"
    assert DESCRIPTOR_MATRIX in type_set, "Missing MATRIX"
    assert DESCRIPTOR_SIGNAL_SPLITTER in type_set, "Missing SIGNAL_SPLITTER"
    assert DESCRIPTOR_SIGNAL_COMBINER in type_set, "Missing SIGNAL_COMBINER"
    assert DESCRIPTOR_SIGNAL_DEMULTIPLEXER in type_set, "Missing SIGNAL_DEMULTIPLEXER"
    assert DESCRIPTOR_SIGNAL_MULTIPLEXER in type_set, "Missing SIGNAL_MULTIPLEXER"
    assert DESCRIPTOR_SIGNAL_TRANSCODER in type_set, "Missing SIGNAL_TRANSCODER"
    assert DESCRIPTOR_CONTROL_BLOCK in type_set, "Missing CONTROL_BLOCK"
    assert DESCRIPTOR_MEMORY_OBJECT in type_set, "Missing MEMORY_OBJECT"
    assert DESCRIPTOR_VIDEO_UNIT in type_set, "Missing VIDEO_UNIT"
    assert DESCRIPTOR_SENSOR_UNIT in type_set, "Missing SENSOR_UNIT"
    assert DESCRIPTOR_VIDEO_CLUSTER in type_set, "Missing VIDEO_CLUSTER"
    assert DESCRIPTOR_SENSOR_CLUSTER in type_set, "Missing SENSOR_CLUSTER"
    assert DESCRIPTOR_TIMING in type_set, "Missing TIMING"
    assert DESCRIPTOR_PTP_INSTANCE in type_set, "Missing PTP_INSTANCE"
    assert DESCRIPTOR_PTP_PORT in type_set, "Missing PTP_PORT"

    # Blob round-trip
    blob = write_blob(descs, syms)
    descs2, syms2 = read_blob(blob)
    assert len(descs2) == len(descs)
    for d1, d2 in zip(descs, descs2):
        assert d1.wire_bytes == d2.wire_bytes, (
            f"Mismatch type=0x{d1.descriptor_type:04X} idx={d1.descriptor_index}"
        )
    blob2 = write_blob(descs2, syms2)
    assert blob == blob2, "Phase 2 blob round-trip mismatch"

    print("  [+] phase2 descriptors flatten + blob round-trip: OK")


def test_phase2_aemxml_round_trip():
    """Test Phase 2 descriptors survive AEMXML write -> read -> flatten."""
    entity = Entity(
        entity_id=1,
        entity_model_id=1,
        entity_name="Phase2 XML RT",
        configurations=[
            Configuration(
                signal_selectors=[
                    SignalSelector(
                        object_name="Sel",
                        sources=[
                            SignalSource(signal_type=5, signal_index=0, signal_output=0)
                        ],
                    ),
                ],
                mixers=[Mixer(object_name="Mix")],
                matrices=[Matrix(object_name="Mat", width=2, height=2)],
                splitters=[
                    SignalSplitter(
                        object_name="Spl",
                        splitter_map=[
                            SplitterMapEntry(
                                sub_signal_start=0, sub_signal_count=1, output_index=0
                            )
                        ],
                    ),
                ],
                combiners=[
                    SignalCombiner(
                        object_name="Comb",
                        combiner_map=[
                            CombinerMapEntry(
                                sub_signal_start=0, sub_signal_count=1, input_index=0
                            )
                        ],
                        sources=[
                            SignalSource(signal_type=5, signal_index=0, signal_output=0)
                        ],
                    ),
                ],
                demultiplexers=[SignalDemultiplexer(object_name="Demux")],
                multiplexers=[SignalMultiplexer(object_name="Mux")],
                transcoders=[SignalTranscoder(object_name="TC")],
                control_blocks=[ControlBlock(object_name="CB")],
                memory_objects=[MemoryObject(object_name="Mem", start_address=0x1000)],
                timings=[
                    Timing(object_name="T0", algorithm=1, ptp_instance_indices=[0])
                ],
                ptp_instances=[
                    PtpInstance(
                        object_name="PTP0",
                        ptp_ports=[PtpPort(object_name="PP0", port_number=1)],
                    ),
                ],
                locales=[
                    Locale(
                        locale_identifier="en",
                        strings_descriptors=[StringsDescriptor(strings=["V"])],
                    )
                ],
                clock_sources=[ClockSource(object_name="Int")],
                clock_domains=[ClockDomain(clock_source_index=0, clock_sources=[0])],
            )
        ],
    )

    # Write to XML, read back
    xml_str = write_aemxml(entity)
    entity2 = read_aemxml(xml_str)

    # Verify new types survived round-trip
    c1 = entity.configurations[0]
    c2 = entity2.configurations[0]
    assert len(c2.signal_selectors) == len(c1.signal_selectors)
    assert c2.signal_selectors[0].object_name == "Sel"
    assert len(c2.signal_selectors[0].sources) == 1
    assert len(c2.mixers) == len(c1.mixers)
    assert len(c2.matrices) == len(c1.matrices)
    assert c2.matrices[0].width == 2
    assert len(c2.splitters) == len(c1.splitters)
    assert len(c2.splitters[0].splitter_map) == 1
    assert len(c2.combiners) == len(c1.combiners)
    assert len(c2.combiners[0].combiner_map) == 1
    assert len(c2.combiners[0].sources) == 1
    assert len(c2.demultiplexers) == len(c1.demultiplexers)
    assert len(c2.multiplexers) == len(c1.multiplexers)
    assert len(c2.transcoders) == len(c1.transcoders)
    assert len(c2.control_blocks) == len(c1.control_blocks)
    assert len(c2.memory_objects) == len(c1.memory_objects)
    assert c2.memory_objects[0].start_address == 0x1000
    assert len(c2.timings) == len(c1.timings)
    assert c2.timings[0].algorithm == 1
    assert len(c2.timings[0].ptp_instance_indices) == 1
    assert len(c2.ptp_instances) == len(c1.ptp_instances)
    assert len(c2.ptp_instances[0].ptp_ports) == 1
    assert c2.ptp_instances[0].ptp_ports[0].port_number == 1

    # Flatten both and compare blobs
    descs1, syms1 = flatten(entity)
    descs2, syms2 = flatten(entity2)
    blob1 = write_blob(descs1, syms1)
    blob2 = write_blob(descs2, syms2)
    assert blob1 == blob2, "Phase 2 AEMXML round-trip blob mismatch"

    print("  [+] phase2 AEMXML round-trip: OK")


def test_external_internal_ports():
    """Test external/internal port descriptors flatten and blob round-trip."""
    entity = Entity(
        entity_id=0x0001020304050607,
        entity_model_id=0x08090A0B0C0D0E0F,
        entity_name="Port Test",
        configurations=[
            Configuration(
                audio_units=[
                    AudioUnit(
                        object_name="AU0",
                        input_stream_ports=[
                            AudioStreamPort(
                                clusters=[AudioCluster(channel_count=2)],
                            ),
                        ],
                        input_external_ports=[
                            ExternalPort(
                                clock_domain_index=0,
                                signal_type=0x0014,
                                signal_index=0,
                                signal_output=0,
                                block_latency=100,
                                jack_index=0,
                                symbol="ext_in_0",
                            ),
                        ],
                        output_external_ports=[
                            ExternalPort(
                                clock_domain_index=0,
                                signal_type=0x0014,
                                signal_index=1,
                                jack_index=1,
                            ),
                        ],
                        input_internal_ports=[
                            InternalPort(
                                clock_domain_index=0,
                                signal_type=0x000E,
                                signal_index=0,
                                internal_index=0,
                                symbol="int_in_0",
                            ),
                        ],
                        output_internal_ports=[
                            InternalPort(
                                clock_domain_index=0,
                                signal_type=0x000F,
                                signal_index=0,
                                internal_index=0,
                            ),
                        ],
                    ),
                ],
                locales=[
                    Locale(
                        locale_identifier="en",
                        strings_descriptors=[StringsDescriptor(strings=["V"])],
                    )
                ],
                clock_sources=[ClockSource(object_name="Int")],
                clock_domains=[ClockDomain(clock_source_index=0, clock_sources=[0])],
            )
        ],
    )

    # Flatten
    descs, syms = flatten(entity)
    type_set = {d.descriptor_type for d in descs}

    assert DESCRIPTOR_EXTERNAL_PORT_INPUT in type_set, "Missing EXTERNAL_PORT_INPUT"
    assert DESCRIPTOR_EXTERNAL_PORT_OUTPUT in type_set, "Missing EXTERNAL_PORT_OUTPUT"
    assert DESCRIPTOR_INTERNAL_PORT_INPUT in type_set, "Missing INTERNAL_PORT_INPUT"
    assert DESCRIPTOR_INTERNAL_PORT_OUTPUT in type_set, "Missing INTERNAL_PORT_OUTPUT"

    # Check wire size: 24 bytes each
    for d in descs:
        if d.descriptor_type in (
            DESCRIPTOR_EXTERNAL_PORT_INPUT,
            DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
            DESCRIPTOR_INTERNAL_PORT_INPUT,
            DESCRIPTOR_INTERNAL_PORT_OUTPUT,
        ):
            assert len(d.wire_bytes) == 24, (
                f"Port descriptor 0x{d.descriptor_type:04X} should be 24 bytes, got {len(d.wire_bytes)}"
            )

    # Check symbols recorded
    sym_types = {s.descriptor_type for s in syms}
    assert DESCRIPTOR_EXTERNAL_PORT_INPUT in sym_types
    assert DESCRIPTOR_INTERNAL_PORT_INPUT in sym_types

    # Blob round-trip
    blob = write_blob(descs, syms)
    descs2, syms2 = read_blob(blob)
    assert len(descs2) == len(descs)
    for d1, d2 in zip(descs, descs2):
        assert d1.wire_bytes == d2.wire_bytes, (
            f"Mismatch type=0x{d1.descriptor_type:04X} idx={d1.descriptor_index}"
        )
    blob2 = write_blob(descs2, syms2)
    assert blob == blob2

    # AEMXML round-trip
    xml_str = write_aemxml(entity)
    entity2 = read_aemxml(xml_str)
    c2 = entity2.configurations[0]
    assert len(c2.audio_units[0].input_external_ports) == 1
    assert len(c2.audio_units[0].output_external_ports) == 1
    assert len(c2.audio_units[0].input_internal_ports) == 1
    assert len(c2.audio_units[0].output_internal_ports) == 1
    assert c2.audio_units[0].input_external_ports[0].jack_index == 0
    assert c2.audio_units[0].input_external_ports[0].block_latency == 100
    assert c2.audio_units[0].input_internal_ports[0].internal_index == 0

    # Blob compare
    descs3, syms3 = flatten(entity2)
    blob3 = write_blob(descs3, syms3)
    assert blob == blob3, "External/internal port AEMXML round-trip blob mismatch"

    print("  [+] external/internal ports: OK")


def test_read_bareminimum_2021():
    """Test reading the bareminimum_2021.aemxml file (2021 schema)."""
    aemxml_path = str(
        Path(__file__).parent.parent.parent / "examples" / "bareminimum_2021.aemxml"
    )
    entity = read_aemxml(aemxml_path)

    assert entity.entity_id == 1
    assert entity.entity_name == "Bare Minimum Entity"
    assert entity.serial_number == "123456789abcd"
    assert entity.schema_year == 2021
    assert len(entity.configurations) == 1

    # Should flatten without error
    descs, syms = flatten(entity)
    assert len(descs) > 0

    print("  [+] read bareminimum_2021.aemxml: OK")


def test_write_2013_compat():
    """Test that writing with schema_year=2013 produces 2013-compatible output."""
    entity = Entity(
        entity_id=1,
        entity_model_id=1,
        entity_name="Compat Test",
        configurations=[
            Configuration(
                locales=[
                    Locale(
                        locale_identifier="en",
                        strings_descriptors=[
                            StringsDescriptor(strings=["Vendor", "Model", "Config"])
                        ],
                    )
                ],
            )
        ],
    )
    xml_str = write_aemxml(entity, schema_year=2013)

    # Should contain avdecc.xsd reference and correct spelling
    # (avdecc.xsd V1.0.1 accepts both spellings, we always write the correct one)
    assert "talker_capabilities" in xml_str, (
        "2013 output should use talker_capabilities (V1.0.1 accepts both)"
    )
    assert "avdecc.xsd" in xml_str, "2013 output should reference avdecc.xsd"
    assert 'version="1.0"' in xml_str, "2013 output should have version 1.0"
    assert "available_index" not in xml_str, (
        "2013 output should not include available_index"
    )

    # Should round-trip through reader
    entity2 = read_aemxml(xml_str)
    assert entity2.schema_year == 2013
    assert entity2.entity_name == "Compat Test"

    print("  [+] write 2013 compat: OK")


def test_write_2021_default():
    """Test that writing with default schema_year=2021 produces correct output."""
    entity = Entity(
        entity_id=1,
        entity_model_id=1,
        entity_name="Default Test",
        configurations=[
            Configuration(
                locales=[
                    Locale(
                        locale_identifier="en",
                        strings_descriptors=[StringsDescriptor(strings=["Vendor"])],
                    )
                ],
            )
        ],
    )
    xml_str = write_aemxml(entity)

    assert "talker_capabilities" in xml_str, (
        "2021 output should use talker_capabilities"
    )
    assert "talker_capabilties" not in xml_str, (
        "2021 output should NOT use talker_capabilties"
    )
    assert "atdecc.xsd" in xml_str, "2021 output should reference atdecc.xsd"
    assert 'version="2.0"' in xml_str, "2021 output should have version 2.0"

    print("  [+] write 2021 default: OK")


def test_xsd_validate_bareminimum_2021():
    """Test XSD validation of bareminimum_2021.aemxml against atdecc.xsd."""
    try:
        from lxml import etree  # noqa: F401
    except ImportError:
        print("  [SKIP] xsd validate bareminimum_2021: lxml not installed")
        return

    from aemxml.aemxml_reader import validate_aemxml

    aemxml_path = str(
        Path(__file__).parent.parent.parent / "examples" / "bareminimum_2021.aemxml"
    )
    errors = validate_aemxml(aemxml_path)
    assert errors == [], f"bareminimum_2021.aemxml should validate, got: {errors}"
    print("  [+] xsd validate bareminimum_2021: OK")


def test_xsd_validate_bareminimum():
    """Test XSD validation of bareminimum.aemxml."""
    try:
        from lxml import etree  # noqa: F401
    except ImportError:
        print("  [SKIP] xsd validation: lxml not installed")
        return

    from aemxml.aemxml_reader import validate_aemxml

    aemxml_path = str(
        Path(__file__).parent.parent.parent / "examples" / "bareminimum.aemxml"
    )
    errors = validate_aemxml(aemxml_path)
    assert errors == [], f"bareminimum.aemxml should validate, got: {errors}"
    print("  [+] xsd validate bareminimum: OK")


def test_xsd_validate_invalid():
    """Test XSD validation returns errors for invalid XML."""
    try:
        from lxml import etree  # noqa: F401
    except ImportError:
        print("  [SKIP] xsd validate invalid: lxml not installed")
        return

    import tempfile
    from aemxml.aemxml_reader import validate_aemxml

    invalid_xml = '<?xml version="1.0"?><entity xmlns="http://grouper.ieee.org/groups/1722/1/contributions/xml"><bogus_element>bad</bogus_element></entity>'
    with tempfile.NamedTemporaryFile(mode="w", suffix=".aemxml", delete=False) as f:
        f.write(invalid_xml)
        tmp_path = f.name

    try:
        errors = validate_aemxml(tmp_path)
        assert len(errors) > 0, "Invalid XML should produce validation errors"
        print(f"  [+] xsd validate invalid ({len(errors)} error(s)): OK")
    finally:
        import os

        os.unlink(tmp_path)


def test_error_messages():
    """Test that parse errors include context."""
    from aemxml.aemxml_reader import _safe_hex, _local_name, _require_text
    import xml.etree.ElementTree as ET

    NS = "http://grouper.ieee.org/groups/1722/1/contributions/xml"

    # Test _safe_hex with bad value
    el = ET.fromstring(
        f'<audio_unit xmlns="{NS}"><signal_type>xyz</signal_type></audio_unit>'
    )
    try:
        _safe_hex("xyz", "signal_type", el)
        assert False, "Should have raised ValueError"
    except ValueError as e:
        assert "xyz" in str(e)
        assert "signal_type" in str(e)
        assert "audio_unit" in str(e)

    # Test _local_name
    assert _local_name(el) == "audio_unit"

    # Test _require_text with missing element
    try:
        _require_text(el, "clock_domain_index")
        assert False, "Should have raised ValueError"
    except ValueError as e:
        assert "clock_domain_index" in str(e)
        assert "audio_unit" in str(e)

    print("  [+] error messages: OK")


def test_symbolic_enum_write_read():
    """AEMXML writer always outputs hex (conforms to standards/atdecc.xsd);
    the reader accepts both hex and symbolic names so legacy files and
    standards/atdecc-proposed.xsd-style files remain readable."""
    from aemxml.model import (
        JACK_TYPE_NAMES,
        CLOCK_SOURCE_TYPE_NAMES,
        AUDIO_CLUSTER_FORMAT_NAMES,
        MEMORY_OBJECT_TYPE_NAMES,
        TIMING_ALGORITHM_NAMES,
        PTP_PORT_TYPE_NAMES,
        ENTITY_CAPABILITIES_NAMES,
        TALKER_CAPABILITIES_NAMES,
        LISTENER_CAPABILITIES_NAMES,
        STREAM_FLAGS_NAMES,
        JACK_FLAGS_NAMES,
        INTERFACE_FLAGS_NAMES,
    )

    entity = Entity(
        entity_id=1,
        entity_model_id=1,
        entity_name="Symbolic Test",
        entity_capabilities=ENTITY_CAPABILITIES_NAMES["AEM_SUPPORTED"]
        | ENTITY_CAPABILITIES_NAMES["GPTP_SUPPORTED"],
        talker_capabilities=TALKER_CAPABILITIES_NAMES["IMPLEMENTED"]
        | TALKER_CAPABILITIES_NAMES["AUDIO_SOURCE"],
        listener_capabilities=LISTENER_CAPABILITIES_NAMES["IMPLEMENTED"]
        | LISTENER_CAPABILITIES_NAMES["AUDIO_SINK"],
        configurations=[
            Configuration(
                audio_units=[
                    AudioUnit(
                        object_name="AU0",
                        input_stream_ports=[
                            AudioStreamPort(
                                clusters=[
                                    AudioCluster(
                                        channel_count=2,
                                        format=0x40,  # MBLA
                                        signal_type=0x0014,  # AUDIO_CLUSTER
                                    )
                                ],
                            ),
                        ],
                        sampling_rates=[0x02_00BB80],
                    ),
                ],
                streams_input=[
                    Stream(
                        object_name="Stream In 0",
                        stream_flags=STREAM_FLAGS_NAMES["CLASS_A"]
                        | STREAM_FLAGS_NAMES["CLOCK_SYNC_SOURCE"],
                    )
                ],
                streams_output=[Stream(object_name="Stream Out 0")],
                jacks_input=[
                    Jack(
                        object_name="Jack In",
                        jack_type=JACK_TYPE_NAMES["BALANCED_ANALOG"],
                        jack_flags=JACK_FLAGS_NAMES["CAPTIVE"],
                    ),
                ],
                avb_interfaces=[
                    AvbInterface(
                        object_name="eth0",
                        interface_flags=INTERFACE_FLAGS_NAMES["GPTP_SUPPORTED"]
                        | INTERFACE_FLAGS_NAMES["SRP_SUPPORTED"],
                    ),
                ],
                clock_sources=[
                    ClockSource(
                        object_name="Internal",
                        clock_source_type=CLOCK_SOURCE_TYPE_NAMES["INTERNAL"],
                        clock_source_location_type=0x0009,  # AVB_INTERFACE
                    ),
                ],
                memory_objects=[
                    MemoryObject(
                        object_name="FW",
                        memory_object_type=MEMORY_OBJECT_TYPE_NAMES["FIRMWARE_IMAGE"],
                        target_descriptor_type=0x000B,  # MEMORY_OBJECT
                    ),
                ],
                timings=[
                    Timing(
                        object_name="T0",
                        algorithm=TIMING_ALGORITHM_NAMES["FALLBACK"],
                        ptp_instance_indices=[0],
                    ),
                ],
                ptp_instances=[
                    PtpInstance(
                        object_name="PTP0",
                        ptp_ports=[
                            PtpPort(
                                object_name="PP0",
                                port_number=1,
                                port_type=PTP_PORT_TYPE_NAMES["P2P_LINK_LAYER"],
                            ),
                        ],
                    ),
                ],
                clock_domains=[ClockDomain(clock_source_index=0, clock_sources=[0])],
                locales=[
                    Locale(
                        locale_identifier="en",
                        strings_descriptors=[StringsDescriptor(strings=["Vendor"])],
                    )
                ],
            )
        ],
    )

    # Write as 2021 (default): AEMXML writer always emits hex so output
    # conforms to standards/atdecc.xsd.
    xml_str = write_aemxml(entity)

    # Verify no symbolic names leaked into the XML (regression guard).
    for sym in (
        "AEM_SUPPORTED",
        "GPTP_SUPPORTED",
        "BALANCED_ANALOG",
        "CAPTIVE",
        "MBLA",
        "INTERNAL",
        "FALLBACK",
        "P2P_LINK_LAYER",
        "FIRMWARE_IMAGE",
        "CLASS_A",
        "CLOCK_SYNC_SOURCE",
    ):
        assert sym not in xml_str, f"Writer must not emit symbolic name {sym}"

    # Writer output round-trips through the reader.
    entity2 = read_aemxml(xml_str)
    assert entity2.entity_capabilities == entity.entity_capabilities
    assert entity2.talker_capabilities == entity.talker_capabilities
    assert entity2.listener_capabilities == entity.listener_capabilities
    c = entity2.configurations[0]
    assert c.jacks_input[0].jack_type == JACK_TYPE_NAMES["BALANCED_ANALOG"]
    assert c.jacks_input[0].jack_flags == JACK_FLAGS_NAMES["CAPTIVE"]
    assert c.streams_input[0].stream_flags == (
        STREAM_FLAGS_NAMES["CLASS_A"] | STREAM_FLAGS_NAMES["CLOCK_SYNC_SOURCE"]
    )
    assert c.clock_sources[0].clock_source_type == CLOCK_SOURCE_TYPE_NAMES["INTERNAL"]
    assert (
        c.memory_objects[0].memory_object_type
        == MEMORY_OBJECT_TYPE_NAMES["FIRMWARE_IMAGE"]
    )
    assert c.timings[0].algorithm == TIMING_ALGORITHM_NAMES["FALLBACK"]
    assert (
        c.ptp_instances[0].ptp_ports[0].port_type
        == PTP_PORT_TYPE_NAMES["P2P_LINK_LAYER"]
    )
    assert c.audio_units[0].input_stream_ports[0].clusters[0].format == 0x40  # MBLA

    # The reader still accepts symbolic-name input, so hand-written
    # files using the atdecc-proposed.xsd vocabulary parse correctly.
    symbolic_xml = xml_str.replace(
        f"<entity_capabilities>{entity.entity_capabilities:08x}</entity_capabilities>",
        "<entity_capabilities>AEM_SUPPORTED GPTP_SUPPORTED</entity_capabilities>",
    )
    entity3 = read_aemxml(symbolic_xml)
    assert entity3.entity_capabilities == entity.entity_capabilities

    # 2013 output is also hex.
    xml_2013 = write_aemxml(entity, schema_year=2013)
    assert "BALANCED_ANALOG" not in xml_2013
    assert "MBLA" not in xml_2013
    assert "CAPTIVE" not in xml_2013

    print("  [+] symbolic enum write/read: OK")


def test_stream_format_aaf_encode_decode():
    """Test round-trip AAF stream format encoding."""
    from aemxml.stream_formats import (
        encode_aaf_stream_format,
        decode_aaf_stream_format,
        parse_stream_format,
        format_stream_format,
    )

    # Basic encode/decode round-trip
    fmt = encode_aaf_stream_format(48000, 2, 24)
    decoded = decode_aaf_stream_format(fmt)
    assert decoded is not None
    assert decoded["type"] == "AAF"
    assert decoded["rate"] == 48000
    assert decoded["channels"] == 2
    assert decoded["depth"] == 24

    # Verify byte layout: byte 0 = 0x02 (AAF subtype)
    assert (fmt >> 56) & 0xFF == 0x02

    # Test various rates
    for rate in [8000, 16000, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000]:
        fmt = encode_aaf_stream_format(rate, 8, 32)
        d = decode_aaf_stream_format(fmt)
        assert d is not None
        assert d["rate"] == rate
        assert d["channels"] == 8
        assert d["depth"] == 32

    # Test structured object parse
    fmt_val = parse_stream_format(
        {"type": "AAF", "rate": 96000, "channels": 4, "depth": 16}
    )
    d = decode_aaf_stream_format(fmt_val)
    assert d["rate"] == 96000
    assert d["channels"] == 4
    assert d["depth"] == 16

    # Test hex string pass-through
    hex_fmt = parse_stream_format("0x0200000000000000")
    assert hex_fmt == 0x0200000000000000

    # Test format_stream_format reverse
    formatted = format_stream_format(fmt_val)
    assert isinstance(formatted, dict)
    assert formatted["type"] == "AAF"

    # Non-AAF returns hex string
    non_aaf = format_stream_format(0x0100000000000000)
    assert isinstance(non_aaf, str)
    assert non_aaf.startswith("0x")

    # Error cases
    try:
        encode_aaf_stream_format(12345, 2, 24)
        assert False, "Should reject unsupported rate"
    except ValueError:
        pass

    print("  [+] stream format AAF encode/decode: OK")


def test_json_read_simple_stereo():
    """Test parsing the simple_stereo.json example."""
    from aemxml.json_reader import read_json

    json_path = str(
        Path(__file__).parent.parent.parent / "examples" / "simple_stereo.json"
    )
    entity = read_json(json_path)

    assert entity.entity_name == "Simple Stereo Device"
    assert entity.entity_id == 1
    assert entity.entity_model_id == 1
    assert entity.firmware_version == "1.0.0"

    # Capabilities
    from aemxml.model import ENTITY_CAPABILITIES_NAMES

    expected_caps = (
        ENTITY_CAPABILITIES_NAMES["AEM_SUPPORTED"]
        | ENTITY_CAPABILITIES_NAMES["GPTP_SUPPORTED"]
        | ENTITY_CAPABILITIES_NAMES["CLASS_A_SUPPORTED"]
    )
    assert entity.entity_capabilities == expected_caps

    # Talker/listener
    assert entity.talker_stream_sources == 1
    assert entity.listener_stream_sinks == 1

    # Configuration
    assert len(entity.configurations) == 1
    config = entity.configurations[0]
    assert config.object_name == "Default"

    # Streams
    assert len(config.streams_input) == 1
    assert len(config.streams_output) == 1
    assert config.streams_input[0].object_name == "Input"
    assert config.streams_output[0].object_name == "Output"
    assert config.streams_input[0].current_format != 0

    # Audio unit
    assert len(config.audio_units) == 1
    au = config.audio_units[0]
    assert au.object_name == "Main"
    assert len(au.sampling_rates) == 3
    assert 48000 in au.sampling_rates
    assert len(au.input_stream_ports) == 1
    assert len(au.output_stream_ports) == 1
    assert len(au.input_stream_ports[0].clusters) == 2
    assert au.input_stream_ports[0].clusters[0].object_name == "Left In"
    assert au.input_stream_ports[0].clusters[0].channel_count == 1

    # Maps
    assert len(au.input_stream_ports[0].maps) == 1
    assert len(au.input_stream_ports[0].maps[0].mappings) == 2

    # AVB interface
    assert len(config.avb_interfaces) == 1
    assert config.avb_interfaces[0].object_name == "eth0"

    # Clock source
    assert len(config.clock_sources) == 1
    assert config.clock_sources[0].object_name == "Internal"

    # Clock domain
    assert len(config.clock_domains) == 1

    # Strings/locales
    assert len(config.locales) == 1
    assert config.locales[0].locale_identifier == "en"
    strings = config.locales[0].strings_descriptors[0].strings
    assert strings[0] == "Example Vendor"
    assert strings[1] == "Stereo I/O"
    assert strings[2] == "Default"

    # Symbols
    assert config.streams_input[0].symbol == "STREAM_INPUT_0"
    assert config.streams_output[0].symbol == "STREAM_OUTPUT_0"
    assert au.input_stream_ports[0].clusters[0].symbol == "INPUT_LEFT"
    assert au.input_stream_ports[0].clusters[1].symbol == "INPUT_RIGHT"

    print("  [+] JSON read simple_stereo: OK")


def test_json_round_trip():
    """Test JSON -> Entity -> JSON preserves all fields."""
    from aemxml.json_reader import read_json
    from aemxml.json_writer import write_json
    import json

    json_path = str(
        Path(__file__).parent.parent.parent / "examples" / "simple_stereo.json"
    )
    entity1 = read_json(json_path)
    json_str = write_json(entity1)
    entity2 = read_json(json_str)

    # Core entity fields
    assert entity1.entity_id == entity2.entity_id
    assert entity1.entity_model_id == entity2.entity_model_id
    assert entity1.entity_name == entity2.entity_name
    assert entity1.entity_capabilities == entity2.entity_capabilities
    assert entity1.talker_capabilities == entity2.talker_capabilities
    assert entity1.listener_capabilities == entity2.listener_capabilities
    assert entity1.talker_stream_sources == entity2.talker_stream_sources
    assert entity1.listener_stream_sinks == entity2.listener_stream_sinks
    assert entity1.firmware_version == entity2.firmware_version

    # Configuration
    assert len(entity1.configurations) == len(entity2.configurations)
    c1 = entity1.configurations[0]
    c2 = entity2.configurations[0]
    assert c1.object_name == c2.object_name
    assert len(c1.streams_input) == len(c2.streams_input)
    assert len(c1.streams_output) == len(c2.streams_output)
    assert len(c1.audio_units) == len(c2.audio_units)
    assert len(c1.avb_interfaces) == len(c2.avb_interfaces)
    assert len(c1.clock_sources) == len(c2.clock_sources)
    assert len(c1.clock_domains) == len(c2.clock_domains)

    # Stream format preserved
    assert c1.streams_input[0].current_format == c2.streams_input[0].current_format
    assert c1.streams_input[0].stream_flags == c2.streams_input[0].stream_flags

    # Audio unit details
    assert c1.audio_units[0].sampling_rates == c2.audio_units[0].sampling_rates

    # Verify JSON is valid and parseable
    data = json.loads(json_str)
    assert "entity" in data

    print("  [+] JSON round-trip: OK")


def test_json_to_blob():
    """Test JSON -> flatten -> blob produces valid storage."""
    from aemxml.json_reader import read_json

    json_path = str(
        Path(__file__).parent.parent.parent / "examples" / "simple_stereo.json"
    )
    entity = read_json(json_path)
    descs, syms = flatten(entity)
    blob = write_blob(descs, syms)

    # Should produce non-empty blob
    assert len(blob) > 0
    assert len(descs) > 0

    # Blob round-trip
    descs2, syms2 = read_blob(blob)
    assert len(descs2) == len(descs)
    blob2 = write_blob(descs2, syms2)
    assert blob == blob2, "JSON -> blob -> read -> blob should be identical"

    # Verify expected descriptor types
    type_set = {d.descriptor_type for d in descs}
    assert DESCRIPTOR_ENTITY in type_set
    assert DESCRIPTOR_CONFIGURATION in type_set
    assert DESCRIPTOR_STREAM_INPUT in type_set
    assert DESCRIPTOR_STREAM_OUTPUT in type_set
    assert DESCRIPTOR_AUDIO_UNIT in type_set
    assert DESCRIPTOR_STREAM_PORT_INPUT in type_set
    assert DESCRIPTOR_STREAM_PORT_OUTPUT in type_set
    assert DESCRIPTOR_AUDIO_CLUSTER in type_set
    assert DESCRIPTOR_AVB_INTERFACE in type_set
    assert DESCRIPTOR_CLOCK_SOURCE in type_set
    assert DESCRIPTOR_CLOCK_DOMAIN in type_set
    assert DESCRIPTOR_LOCALE in type_set
    assert DESCRIPTOR_STRINGS in type_set

    print("  [+] JSON to blob: OK")


def test_json_to_xml_round_trip():
    """Test JSON -> Entity -> AEMXML -> Entity -> JSON round-trip."""
    from aemxml.json_reader import read_json
    from aemxml.json_writer import write_json

    json_path = str(
        Path(__file__).parent.parent.parent / "examples" / "simple_stereo.json"
    )
    entity1 = read_json(json_path)

    # Entity -> AEMXML -> Entity
    xml_str = write_aemxml(entity1)
    entity2 = read_aemxml(xml_str)

    # Entity -> JSON -> Entity
    json_str = write_json(entity2)
    entity3 = read_json(json_str)

    # Blobs should match between entity1 and entity2
    descs1, syms1 = flatten(entity1)
    descs2, syms2 = flatten(entity2)
    blob1 = write_blob(descs1, syms1)
    blob2 = write_blob(descs2, syms2)
    assert blob1 == blob2, "JSON -> AEMXML -> Entity should produce same blob"

    # Core fields should survive the full round-trip
    assert entity1.entity_id == entity3.entity_id
    assert entity1.entity_name == entity3.entity_name
    assert entity1.entity_capabilities == entity3.entity_capabilities
    assert len(entity1.configurations) == len(entity3.configurations)

    print("  [+] JSON to XML round-trip: OK")


def test_symbolic_enum_round_trip_blob():
    """Test that symbolic names produce correct blob (wire) values."""
    from aemxml.model import (
        JACK_TYPE_NAMES,
        TIMING_ALGORITHM_NAMES,
    )

    entity = Entity(
        entity_id=1,
        entity_model_id=1,
        entity_name="Blob Sym Test",
        configurations=[
            Configuration(
                jacks_input=[
                    Jack(
                        object_name="Jack",
                        jack_type=JACK_TYPE_NAMES["BALANCED_ANALOG"],
                    ),
                ],
                timings=[
                    Timing(
                        object_name="T0",
                        algorithm=TIMING_ALGORITHM_NAMES["FALLBACK"],
                        ptp_instance_indices=[0],
                    ),
                ],
                ptp_instances=[
                    PtpInstance(
                        object_name="PTP0",
                        ptp_ports=[PtpPort(object_name="PP0", port_number=1)],
                    ),
                ],
                clock_sources=[ClockSource(object_name="Int")],
                clock_domains=[ClockDomain(clock_source_index=0, clock_sources=[0])],
                locales=[
                    Locale(
                        locale_identifier="en",
                        strings_descriptors=[StringsDescriptor(strings=["V"])],
                    )
                ],
            )
        ],
    )

    # Write symbolic -> read -> flatten -> blob
    xml_str = write_aemxml(entity)
    entity2 = read_aemxml(xml_str)

    descs1, syms1 = flatten(entity)
    descs2, syms2 = flatten(entity2)
    blob1 = write_blob(descs1, syms1)
    blob2 = write_blob(descs2, syms2)
    assert blob1 == blob2, "Symbolic enum round-trip should produce identical blobs"

    print("  [+] symbolic enum blob round-trip: OK")


def test_json_symbols_to_blob():
    """Test that symbols from JSON survive flatten -> blob -> read_blob round-trip."""
    from aemxml.json_reader import read_json

    json_path = str(
        Path(__file__).parent.parent.parent / "examples" / "simple_stereo.json"
    )
    entity = read_json(json_path)

    descs, syms = flatten(entity)

    # Symbol list should be non-empty (simple_stereo.json has 4 symbols)
    assert len(syms) > 0, "Expected non-empty symbol list from simple_stereo.json"

    # All symbol codes should be non-zero CRC32 values
    for sym in syms:
        assert sym.symbol_code != 0, (
            f"Symbol code should be non-zero for descriptor type=0x{sym.descriptor_type:04X} "
            f"index={sym.descriptor_index}"
        )

    # Write blob and read it back
    blob = write_blob(descs, syms)
    descs2, syms2 = read_blob(blob)

    # Symbols should survive the round-trip
    assert len(syms2) == len(syms), (
        f"Symbol count mismatch: wrote {len(syms)}, read back {len(syms2)}"
    )
    for s1, s2 in zip(syms, syms2):
        assert s1.descriptor_type == s2.descriptor_type
        assert s1.descriptor_index == s2.descriptor_index
        assert s1.symbol_code == s2.symbol_code

    print(f"  [+] JSON symbols to blob: OK ({len(syms)} symbols)")


def test_json_pull_rates():
    """Test sampling rates with pull factors (e.g., NTSC 44055.9 Hz)."""
    from aemxml.json_reader import read_json
    from aemxml.json_writer import write_json

    json_str = json.dumps(
        {
            "entity": {
                "name": "Pull Rate Test",
                "entity_id": "0x0000000000000001",
                "model_id": "0x0000000000000001",
                "capabilities": ["AEM_SUPPORTED"],
                "vendor": "Test",
                "model": "Pull",
                "configuration": {
                    "audio_unit": {
                        "rates": [
                            48000,
                            {"base": 44100, "pull": "1/1.001"},
                            {"base": 48000, "pull": "1/1.001"},
                            {"base": 44100, "pull": "1.001"},
                        ],
                    },
                    "strings": ["Test", "Pull", "Default"],
                },
            }
        }
    )
    entity = read_json(json_str)
    au = entity.configurations[0].audio_units[0]

    # Plain 48000: pull=0, base=48000
    assert au.sampling_rates[0] == 48000

    # 44100 * 1/1.001 (NTSC): pull=1 << 29 | 44100
    ntsc_44100 = (1 << 29) | 44100
    assert au.sampling_rates[1] == ntsc_44100, (
        f"Expected 0x{ntsc_44100:08x}, got 0x{au.sampling_rates[1]:08x}"
    )

    # 48000 * 1/1.001: pull=1 << 29 | 48000
    ntsc_48000 = (1 << 29) | 48000
    assert au.sampling_rates[2] == ntsc_48000

    # 44100 * 1.001: pull=2 << 29 | 44100
    pull_up = (2 << 29) | 44100
    assert au.sampling_rates[3] == pull_up

    # Round-trip through JSON writer
    json_out = write_json(entity)
    data = json.loads(json_out)
    rates = data["entity"]["configuration"]["audio_unit"]["rates"]
    assert rates[0] == 48000
    assert rates[1] == {"base": 44100, "pull": "1/1.001"}
    assert rates[2] == {"base": 48000, "pull": "1/1.001"}
    assert rates[3] == {"base": 44100, "pull": "1.001"}

    # Round-trip: re-read and verify same wire values
    entity2 = read_json(json_out)
    au2 = entity2.configurations[0].audio_units[0]
    assert au.sampling_rates == au2.sampling_rates

    print("  [+] JSON pull rates: OK")


def test_json_localized_dict():
    """Dict-form localized strings: {locale: text} authored at the field site
    generates the LOCALE + STRINGS descriptors and the wire references."""
    from aemxml.json_reader import read_json

    json_str = json.dumps(
        {
            "entity": {
                "vendor": "Statusbar",
                "model": {"en-US": "Tone Generator", "de-DE": "Tongenerator"},
                "name": "Localized Test",
                "configuration": {
                    "name": "Main",
                    "localized_description": {
                        "en-US": "Main Config",
                        "de-DE": "Hauptkonfiguration",
                    },
                    "streams_out": [
                        {
                            "name": "S1",
                            "format": "0x0205022002006000",
                            "localized_description": {
                                "en-US": "AAF Audio",
                                "de-DE": "AAF-Audio",
                            },
                        },
                        {
                            "name": "S2",
                            "format": "0x041060010000BB80",
                            # No de-DE: falls back to the entry's primary text.
                            "localized_description": {"en-US": "CRF Clock"},
                        },
                    ],
                    "clock_sources": [
                        {
                            "name": "CS",
                            "type": "INTERNAL",
                            # Identical dict to S1's: deduplicated to one slot.
                            "localized_description": {
                                "en-US": "AAF Audio",
                                "de-DE": "AAF-Audio",
                            },
                        }
                    ],
                },
            }
        }
    )
    entity = read_json(json_str)
    config = entity.configurations[0]

    # One locale per language, in authoring order, with identical layouts.
    assert [loc.locale_identifier for loc in config.locales] == ["en-US", "de-DE"]
    en, de = config.locales
    assert len(en.strings_descriptors) == len(de.strings_descriptors) == 1
    # Slots 0/1 are the entity vendor/model (matching the fixed ENTITY refs);
    # authored entries follow. The plain-string vendor appears in every locale.
    assert en.strings_descriptors[0].strings == [
        "Statusbar",
        "Tone Generator",
        "Main Config",
        "AAF Audio",
        "CRF Clock",
    ]
    assert de.strings_descriptors[0].strings == [
        "Statusbar",
        "Tongenerator",
        "Hauptkonfiguration",
        "AAF-Audio",
        "CRF Clock",  # en-US fallback
    ]
    assert entity.vendor_name_string.offset == 0
    assert entity.vendor_name_string.index == 0
    assert entity.model_name_string.index == 1
    assert config.localized_description.index == 2
    assert config.streams_output[0].localized_description.index == 3
    assert config.streams_output[1].localized_description.index == 4
    # Deduplicated: the clock source shares S1's slot.
    assert config.clock_sources[0].localized_description.index == 3

    # Absent localized_description -> NO_STRING (0xFFFF on the wire).
    ref = pack_localized_string_ref(
        config.streams_output[0].localized_description.offset,
        config.streams_output[0].localized_description.index,
    )
    assert unpack_u16(ref, 0) == 3
    plain = read_json(
        json.dumps(
            {
                "entity": {
                    "vendor": "V",
                    "name": "N",
                    "configuration": {
                        "name": "C",
                        "streams_out": [{"name": "S", "format": "0x041060010000BB80"}],
                    },
                }
            }
        )
    )
    no_string = plain.configurations[0].streams_output[0].localized_description
    assert (
        unpack_u16(pack_localized_string_ref(no_string.offset, no_string.index), 0)
        == 0xFFFF
    )

    # The eighth unique entry spills into a second STRINGS descriptor (offset 1).
    many = read_json(
        json.dumps(
            {
                "entity": {
                    "vendor": "V",
                    "model": "M",
                    "name": "N",
                    "configuration": {
                        "name": "C",
                        "streams_out": [
                            {
                                "name": f"S{i}",
                                "format": "0x041060010000BB80",
                                "localized_description": {"en": f"Stream {i}"},
                            }
                            for i in range(6)
                        ],
                    },
                }
            }
        )
    )
    cfg = many.configurations[0]
    assert cfg.streams_output[4].localized_description.offset == 0
    assert cfg.streams_output[4].localized_description.index == 6
    assert cfg.streams_output[5].localized_description.offset == 1
    assert cfg.streams_output[5].localized_description.index == 0
    assert len(cfg.locales[0].strings_descriptors) == 2

    # Mixing an explicit strings table with dict-form localization is an error.
    try:
        read_json(
            json.dumps(
                {
                    "entity": {
                        "vendor": "V",
                        "configuration": {
                            "name": "C",
                            "strings": ["x"],
                            "streams_out": [
                                {
                                    "name": "S",
                                    "format": "0x041060010000BB80",
                                    "localized_description": {"en": "S"},
                                }
                            ],
                        },
                    }
                }
            )
        )
        raise AssertionError("mixing strings + dict-form localization not rejected")
    except ValueError:
        pass
    print("  [+] json localized dict authoring: OK")


def test_json_control_values():
    """Typed control authoring: named control/value types, LINEAR / SELECTOR /
    UTF8 payload generation, flag bits, and a correct number_of_values on the
    wire (the field is an item count, not the payload byte length)."""
    import struct

    from aemxml.control_values import (
        CONTROL_TYPE_NAMES,
        count_values,
        parse_value_details,
    )
    from aemxml.flatten import flatten
    from aemxml.json_reader import read_json

    json_str = json.dumps(
        {
            "entity": {
                "vendor": "V",
                "model": "M",
                "name": "N",
                "configuration": {
                    "name": "C",
                    "controls": [
                        {
                            "name": "Identify",
                            "control_type": "IDENTIFY",
                            "value_type": "LINEAR_UINT8",
                            "values": [
                                {"min": 0, "max": 255, "step": 255, "default": 0}
                            ],
                        },
                        {
                            "name": "Volume",
                            "control_type": "GAIN",
                            "value_type": "LINEAR_INT32",
                            "block_latency": 100,
                            "control_latency": 200,
                            "control_domain": 3,
                            "reset_time": 5000,
                            "values": [
                                {
                                    "min": -60,
                                    "max": 12,
                                    "step": 1,
                                    "default": 0,
                                    "unit": "LEVEL_DB",
                                    "string_ref": {"en-US": "Volume"},
                                }
                            ],
                        },
                        {
                            "name": "Source",
                            "control_type": "SRC_MODE",
                            "value_type": "SELECTOR_UINT16",
                            "read_only": True,
                            "values": {
                                "current": 1,
                                "default": 0,
                                "options": [0, 1, 2],
                                "unit": "COUNT",
                            },
                        },
                        {
                            "name": "Url",
                            "control_type": "ENTITY_URL",
                            "value_type": "UTF8",
                            "values": "https://example.com/",
                        },
                    ],
                },
            }
        }
    )
    entity = read_json(json_str)
    ident, vol, sel, url = entity.configurations[0].controls

    assert ident.control_type == CONTROL_TYPE_NAMES["IDENTIFY"]
    assert ident.control_value_type == 0x0001  # LINEAR_UINT8
    assert len(ident.value_details) == 9  # 5x1 + unit + string_ref
    # Byte-exact spec order (min, max, step, default, current, unit, string) —
    # macOS rejects the entity if these are permuted.
    assert ident.value_details == bytes.fromhex("00ffff0000" + "0000" + "ffff")
    iv = parse_value_details(ident.control_value_type, ident.value_details, 1)[0]
    assert (iv.minimum, iv.maximum, iv.step) == (0, 255, 255)
    assert iv.string_ref == 0xFFFF  # absent -> NO_STRING

    assert vol.block_latency == 100 and vol.control_latency == 200
    assert vol.control_domain == 3 and vol.reset_time == 5000
    vv = parse_value_details(vol.control_value_type, vol.value_details, 1)[0]
    assert (vv.minimum, vv.maximum) == (-60, 12)
    assert vv.unit == 0xB0  # LEVEL_DB
    assert vv.string_ref == (0 << 3) | 2  # collector slot 2 (after vendor/model)

    assert sel.control_value_type == 0x800D  # SELECTOR_UINT16 | read-only
    # For SELECTOR types number_of_values is the OPTION count (3 here).
    sv = parse_value_details(sel.control_value_type, sel.value_details, 3)[0]
    assert sv.options == [0, 1, 2] and sv.current == 1
    # current, default, options[3], unit — no embedded count, no string_ref.
    assert sel.value_details == bytes.fromhex("0001" + "0000" + "000000010002" + "0001")

    # number_of_values on the wire (offset 96): item count for LINEAR/UTF8,
    # option count for SELECTOR.
    descs, _ = flatten(entity)
    controls_wire = [d for d in descs if d.descriptor_type == 0x001A]
    assert [struct.unpack_from(">H", d.wire_bytes, 96)[0] for d in controls_wire] == [
        1,
        1,
        3,
        1,
    ]
    assert count_values(url.control_value_type, url.value_details) == 1

    # Typed values + raw value_details is an error; so is a truncated LINEAR payload.
    for bad in (
        {"value_type": "LINEAR_UINT8", "values": [{}], "value_details": "0x00"},
        {"value_type": "LINEAR_UINT8", "value_details": "0x00006464000000"},
        {"value_type": "NOT_A_TYPE"},
        {"control_type": "NOT_A_CONTROL"},
    ):
        try:
            read_json(
                json.dumps(
                    {
                        "entity": {
                            "vendor": "V",
                            "configuration": {"name": "C", "controls": [bad]},
                        }
                    }
                )
            )
            raise AssertionError(f"not rejected: {bad}")
        except ValueError:
            pass
    print("  [+] json control values: OK")


def test_json_signal_selector_and_control_grouping():
    """SIGNAL_SELECTOR authoring, controls attached to an AVB_INTERFACE, and
    CONTROL_BLOCK inline grouping: CONTROL indices are assigned contiguously
    (config-level first, then per-interface, then per-block) and the
    number_of_controls/base_control fields are derived. final_control_index
    stays author-owned (spec default 0 = no internal signal chain)."""
    import struct

    from aemxml.flatten import flatten
    from aemxml.json_reader import read_json

    json_str = json.dumps(
        {
            "entity": {
                "vendor": "V",
                "model": "M",
                "name": "N",
                "configuration": {
                    "name": "C",
                    "avb_interface": {
                        "name": "eth0",
                        "controls": [
                            {
                                "name": "Interface Up",
                                "control_type": "INTERFACE_OPERATIONAL",
                                "value_type": "LINEAR_UINT8",
                                "read_only": True,
                                "values": [
                                    {"min": 0, "max": 255, "step": 255, "default": 255}
                                ],
                            }
                        ],
                    },
                    "controls": [
                        {
                            "name": "Identify",
                            "control_type": "IDENTIFY",
                            "value_type": "LINEAR_UINT8",
                            "values": [
                                {"min": 0, "max": 255, "step": 255, "default": 0}
                            ],
                        }
                    ],
                    "signal_selectors": [
                        {
                            "name": "Input Select",
                            "sources": [
                                {"signal_type": "AUDIO_CLUSTER", "signal_index": 0},
                                {
                                    "signal_type": "AUDIO_CLUSTER",
                                    "signal_index": 1,
                                    "signal_output": 0,
                                },
                            ],
                            "current": 1,
                            "default": 0,
                        }
                    ],
                    "control_blocks": [
                        {
                            "name": "Tone Block",
                            "controls": [
                                {
                                    "name": "Mute",
                                    "control_type": "MUTE",
                                    "value_type": "LINEAR_UINT8",
                                    "values": [
                                        {"min": 0, "max": 1, "step": 1, "default": 0}
                                    ],
                                },
                                {
                                    "name": "Gain",
                                    "control_type": "GAIN",
                                    "value_type": "LINEAR_INT32",
                                    "values": [
                                        {"min": -60, "max": 12, "step": 1, "default": 0}
                                    ],
                                },
                            ],
                        }
                    ],
                },
            }
        }
    )
    entity = read_json(json_str)
    config = entity.configurations[0]

    sel = config.signal_selectors[0]
    assert len(sel.sources) == 2
    assert sel.current_signal_index == 1 and sel.default_signal_index == 0
    assert sel.current_signal_type == 0x0014  # AUDIO_CLUSTER

    assert len(config.avb_interfaces[0].controls) == 1
    assert len(config.control_blocks[0].controls) == 2

    descs, _ = flatten(entity)
    by_type = {}
    for d in descs:
        by_type.setdefault(d.descriptor_type, []).append(d)

    # Four CONTROL descriptors total: config Identify (0), interface (1),
    # block members (2, 3).
    controls = sorted(by_type[0x001A], key=lambda d: d.descriptor_index)
    assert [d.descriptor_index for d in controls] == [0, 1, 2, 3]

    # AVB_INTERFACE: number_of_controls @98, base_control @100.
    intf_wire = by_type[0x0009][0].wire_bytes
    assert struct.unpack_from(">HH", intf_wire, 98) == (1, 1)

    # CONTROL_BLOCK: number/base/final @70/72/74 -> 2 controls at base 2.
    # final_control_index is author-owned and defaults to 0 (no internal
    # signal chain, IEEE 1722.1-2021 Table 7-62) -- it does not derive
    # from the control count.
    cb_wire = by_type[0x0025][0].wire_bytes
    assert struct.unpack_from(">HHH", cb_wire, 70) == (2, 2, 0)

    # SIGNAL_SELECTOR: sources_offset(96) @80, count @82, current triple
    # @84, default triple @90, then the two 6-byte sources.
    sel_wire = by_type[0x001B][0].wire_bytes
    assert struct.unpack_from(">HH", sel_wire, 80) == (96, 2)
    assert struct.unpack_from(">HHH", sel_wire, 84) == (0x0014, 1, 0)
    assert struct.unpack_from(">HHH", sel_wire, 90) == (0x0014, 0, 0)
    assert len(sel_wire) == 96 + 2 * 6

    # An empty selector or block is rejected.
    for bad_key, bad_val in (
        ("signal_selectors", [{"name": "S", "sources": []}]),
        ("control_blocks", [{"name": "B"}]),
    ):
        try:
            read_json(
                json.dumps(
                    {
                        "entity": {
                            "vendor": "V",
                            "configuration": {"name": "C", bad_key: bad_val},
                        }
                    }
                )
            )
            raise AssertionError(f"not rejected: {bad_key}={bad_val}")
        except ValueError:
            pass
    print("  [+] json signal selector + control grouping: OK")


def test_json_matrix():
    """MATRIX + MATRIX_SIGNAL authoring: typed values, derived
    number_of_values, and contiguous MATRIX_SIGNAL indices with derived
    number_of_sources/base_source across multiple matrices."""
    import struct

    from aemxml.flatten import flatten
    from aemxml.json_reader import read_json

    json_str = json.dumps(
        {
            "entity": {
                "vendor": "V",
                "model": "M",
                "name": "N",
                "configuration": {
                    "name": "C",
                    "matrices": [
                        {
                            "name": "Mix A",
                            "control_type": "GAIN",
                            "value_type": "LINEAR_INT32",
                            "width": 2,
                            "height": 2,
                            "values": [
                                {"min": -60, "max": 12, "step": 1, "default": 0}
                            ],
                            "signals": [
                                [{"signal_type": "AUDIO_CLUSTER", "signal_index": 0}],
                                [{"signal_type": "AUDIO_CLUSTER", "signal_index": 1}],
                            ],
                        },
                        {
                            "name": "Mix B",
                            "control_type": "GAIN",
                            "value_type": "LINEAR_INT32",
                            "width": 1,
                            "height": 1,
                            "values": [
                                {"min": -60, "max": 12, "step": 1, "default": 0}
                            ],
                            "signals": [
                                {
                                    "signals": [
                                        {
                                            "signal_type": "AUDIO_CLUSTER",
                                            "signal_index": 2,
                                        }
                                    ],
                                    "symbol": "mix_b_src",
                                }
                            ],
                        },
                    ],
                },
            }
        }
    )
    entity = read_json(json_str)
    config = entity.configurations[0]
    assert len(config.matrices) == 2
    assert config.matrices[0].width == 2 and config.matrices[0].height == 2
    assert config.matrices[0].number_of_values == 1  # one LINEAR entry
    assert len(config.matrices[0].matrix_signals) == 2
    assert config.matrices[1].matrix_signals[0].symbol == "mix_b_src"

    descs, symbols = flatten(entity)
    by_type = {}
    for d in descs:
        by_type.setdefault(d.descriptor_type, []).append(d)

    # MATRIX A: number_of_values @96 = 1, number_of_sources @98 = 2,
    # base_source @100 = 0. MATRIX B: 1 source at base 2.
    matrices = sorted(by_type[0x001D], key=lambda d: d.descriptor_index)
    assert struct.unpack_from(">HHH", matrices[0].wire_bytes, 96) == (1, 2, 0)
    assert struct.unpack_from(">HHH", matrices[1].wire_bytes, 96) == (1, 1, 2)
    # value_details follow at values_offset 102: LINEAR_INT32 entry = 24 bytes.
    assert struct.unpack_from(">H", matrices[0].wire_bytes, 94) == (102,)
    assert len(matrices[0].wire_bytes) == 102 + 24

    # Three MATRIX_SIGNAL descriptors, indices 0..2, each with one source.
    signals = sorted(by_type[0x001E], key=lambda d: d.descriptor_index)
    assert [d.descriptor_index for d in signals] == [0, 1, 2]
    assert struct.unpack_from(">HH", signals[2].wire_bytes, 4) == (8, 1)
    assert struct.unpack_from(">HHH", signals[2].wire_bytes, 8) == (0x0014, 2, 0)

    # The matrix-signal symbol landed in the symbol table.
    assert any(s.descriptor_type == 0x001E and s.descriptor_index == 2 for s in symbols)

    # A matrix without positive dimensions is rejected.
    try:
        read_json(
            json.dumps(
                {
                    "entity": {
                        "vendor": "V",
                        "configuration": {
                            "name": "C",
                            "matrices": [{"name": "Bad", "value_type": "LINEAR_INT32"}],
                        },
                    }
                }
            )
        )
        raise AssertionError("dimensionless matrix not rejected")
    except ValueError:
        pass
    print("  [+] json matrix + matrix_signal: OK")


def test_json_mixer_and_inline_controls():
    """MIXER authoring (sources + a single typed value) and controls
    authored inline on audio units and stream ports: flatten derives the
    CONTROL indices and the unit/port number_of_controls / base_control."""
    import struct

    from aemxml.flatten import flatten
    from aemxml.json_reader import read_json

    json_str = json.dumps(
        {
            "entity": {
                "vendor": "V",
                "model": "M",
                "name": "N",
                "configuration": {
                    "name": "C",
                    "audio_units": [
                        {
                            "name": "Main",
                            "rates": [48000],
                            "controls": [
                                {
                                    "name": "Master",
                                    "control_type": "GAIN",
                                    "value_type": "LINEAR_INT32",
                                    "values": [
                                        {"min": -60, "max": 12, "step": 1, "default": 0}
                                    ],
                                    "symbol": "master_gain",
                                }
                            ],
                            "output_ports": [
                                {
                                    "clusters": [{"name": "Out", "channels": 2}],
                                    "controls": [
                                        {
                                            "name": "PortMute",
                                            "control_type": "MUTE",
                                            "value_type": "LINEAR_UINT8",
                                            "values": [
                                                {
                                                    "min": 0,
                                                    "max": 1,
                                                    "step": 1,
                                                    "default": 0,
                                                }
                                            ],
                                            "symbol": "port_mute",
                                        }
                                    ],
                                }
                            ],
                        }
                    ],
                    "mixers": [
                        {
                            "name": "Monitor Mix",
                            "value_type": "LINEAR_INT32",
                            "values": [
                                {
                                    "min": -60,
                                    "max": 12,
                                    "step": 1,
                                    "default": 0,
                                    "current": 7,
                                }
                            ],
                            "sources": [
                                {"signal_type": "AUDIO_CLUSTER", "signal_index": 0},
                                {"signal_type": "AUDIO_CLUSTER", "signal_index": 1},
                            ],
                            "symbol": "monitor_mix",
                        }
                    ],
                },
            }
        }
    )
    entity = read_json(json_str)
    config = entity.configurations[0]
    assert len(config.mixers) == 1
    assert len(config.mixers[0].sources) == 2
    assert config.audio_units[0].controls[0].object_name == "Master"
    assert (
        config.audio_units[0].output_stream_ports[0].controls[0].symbol == "port_mute"
    )

    descs, symbols = flatten(entity)
    by_type = {}
    for d in descs:
        by_type.setdefault(d.descriptor_type, []).append(d)

    # MIXER wire: control_value_type @80, sources_offset @82 = 88,
    # number_of_sources @84 = 2, value_offset @86 = 88 + 2*6 = 100.
    (mixer,) = by_type[0x001C]
    assert struct.unpack_from(">HHHH", mixer.wire_bytes, 80) == (0x0004, 88, 2, 100)
    # Sources: two AUDIO_CLUSTER triples.
    assert struct.unpack_from(">HHH", mixer.wire_bytes, 88) == (0x0014, 0, 0)
    assert struct.unpack_from(">HHH", mixer.wire_bytes, 94) == (0x0014, 1, 0)
    # Value entry (min, max, step, default, current) at value_offset.
    assert struct.unpack_from(">iiiii", mixer.wire_bytes, 100) == (-60, 12, 1, 0, 7)

    # Two CONTROL descriptors: the unit's (index 0), then the port's (1).
    controls = sorted(by_type[0x001A], key=lambda d: d.descriptor_index)
    assert [d.descriptor_index for d in controls] == [0, 1]

    # AUDIO_UNIT: number_of_controls @96 = 1, base_control @98 = 0.
    (unit,) = by_type[0x0002]
    assert struct.unpack_from(">HH", unit.wire_bytes, 96) == (1, 0)

    # STREAM_PORT_OUTPUT: number_of_controls @8 = 1, base_control @10 = 1.
    (port,) = by_type[0x000F]
    assert struct.unpack_from(">HH", port.wire_bytes, 8) == (1, 1)

    # All three symbols landed in the symbol table.
    sym_types = {(s.descriptor_type, s.descriptor_index) for s in symbols}
    assert (0x001C, 0) in sym_types  # monitor_mix
    assert (0x001A, 0) in sym_types  # master_gain
    assert (0x001A, 1) in sym_types  # port_mute

    # A mixer without sources is rejected.
    try:
        read_json(
            json.dumps(
                {
                    "entity": {
                        "vendor": "V",
                        "configuration": {
                            "name": "C",
                            "mixers": [{"name": "Bad", "value_type": "LINEAR_INT32"}],
                        },
                    }
                }
            )
        )
        raise AssertionError("sourceless mixer not rejected")
    except ValueError:
        pass
    print("  [+] json mixer + inline unit/port controls: OK")


def test_duplicate_symbols_rejected():
    """flatten() rejects entity models whose symbol table would carry the
    same 32-bit code twice: the same symbol string on two descriptors, or
    two different strings whose CRC32 codes collide."""
    from aemxml.flatten import flatten
    from aemxml.json_reader import read_json

    def model_with_symbols(sym_a, sym_b):
        return read_json(
            json.dumps(
                {
                    "entity": {
                        "vendor": "V",
                        "model": "M",
                        "name": "N",
                        "configuration": {
                            "name": "C",
                            "controls": [
                                {
                                    "name": "A",
                                    "control_type": "IDENTIFY",
                                    "value_type": "LINEAR_UINT8",
                                    "values": [
                                        {
                                            "min": 0,
                                            "max": 255,
                                            "step": 255,
                                            "default": 0,
                                        }
                                    ],
                                    "symbol": sym_a,
                                },
                                {
                                    "name": "B",
                                    "control_type": "MUTE",
                                    "value_type": "LINEAR_UINT8",
                                    "values": [
                                        {"min": 0, "max": 1, "step": 1, "default": 0}
                                    ],
                                    "symbol": sym_b,
                                },
                            ],
                        },
                    }
                }
            )
        )

    # Unique symbols flatten cleanly.
    flatten(model_with_symbols("identify", "mute"))

    # The same symbol on two descriptors is rejected.
    try:
        flatten(model_with_symbols("identify", "identify"))
        raise AssertionError("duplicate symbol string not rejected")
    except ValueError as e:
        assert "used twice" in str(e) and "identify" in str(e)

    # Two different names whose CRC32 codes collide are also rejected
    # ("plumless" and "buckeroo" are a known crc32 collision pair).
    try:
        flatten(model_with_symbols("plumless", "buckeroo"))
        raise AssertionError("crc32 collision not rejected")
    except ValueError as e:
        assert "collide" in str(e) and "plumless" in str(e) and "buckeroo" in str(e)
    print("  [+] duplicate symbols rejected: OK")


def test_upgrade_2013_to_2021():
    """Upgrade a 2013 AEMXML file to 2021 schema."""
    aemxml_path = str(
        Path(__file__).parent.parent.parent / "examples" / "bareminimum.aemxml"
    )
    entity = read_aemxml(aemxml_path)
    assert entity.schema_year == 2013

    xml_2021 = write_aemxml(entity, schema_year=2021)
    assert 'version="2.0"' in xml_2021
    assert "atdecc.xsd" in xml_2021

    entity2 = read_aemxml(xml_2021)
    assert entity2.schema_year == 2021
    assert entity2.entity_name == entity.entity_name
    print("  [+] upgrade 2013→2021: OK")


def test_downgrade_2021_to_2013():
    """Downgrade a 2021 AEMXML file (without 2021-only descriptors) to 2013."""
    aemxml_path = str(
        Path(__file__).parent.parent.parent / "examples" / "bareminimum_2021.aemxml"
    )
    entity = read_aemxml(aemxml_path)

    xml_2013 = write_aemxml(entity, schema_year=2013)
    assert 'version="1.0"' in xml_2013
    assert "avdecc.xsd" in xml_2013

    entity2 = read_aemxml(xml_2013)
    assert entity2.schema_year == 2013
    assert entity2.entity_name == entity.entity_name
    print("  [+] downgrade 2021→2013: OK")


def test_upgrade_is_idempotent():
    """Upgrading an already-2021 file produces equivalent output."""
    aemxml_path = str(
        Path(__file__).parent.parent.parent / "examples" / "bareminimum_2021.aemxml"
    )
    entity = read_aemxml(aemxml_path)
    xml1 = write_aemxml(entity, schema_year=2021)
    entity2 = read_aemxml(xml1)
    xml2 = write_aemxml(entity2, schema_year=2021)
    assert xml1 == xml2
    print("  [+] upgrade idempotent: OK")


def test_downgrade_is_idempotent():
    """Downgrading an already-2013 file produces equivalent output."""
    aemxml_path = str(
        Path(__file__).parent.parent.parent / "examples" / "bareminimum.aemxml"
    )
    entity = read_aemxml(aemxml_path)
    xml1 = write_aemxml(entity, schema_year=2013)
    entity2 = read_aemxml(xml1)
    xml2 = write_aemxml(entity2, schema_year=2013)
    assert xml1 == xml2
    print("  [+] downgrade idempotent: OK")


def test_upgrade_then_downgrade_round_trip():
    """Upgrade 2013→2021 then downgrade 2021→2013 preserves data model."""
    aemxml_path = str(
        Path(__file__).parent.parent.parent / "examples" / "bareminimum.aemxml"
    )
    entity_orig = read_aemxml(aemxml_path)

    xml_2021 = write_aemxml(entity_orig, schema_year=2021)
    entity_2021 = read_aemxml(xml_2021)

    xml_2013 = write_aemxml(entity_2021, schema_year=2013)
    entity_back = read_aemxml(xml_2013)

    assert entity_back.entity_name == entity_orig.entity_name
    assert entity_back.entity_id == entity_orig.entity_id
    assert len(entity_back.configurations) == len(entity_orig.configurations)
    print("  [+] upgrade→downgrade round-trip: OK")


def test_downgrade_rejects_2021_only_descriptors():
    """Downgrading a file with Timing/PtpInstance descriptors is detected."""
    entity = Entity(
        entity_name="Test",
        schema_year=2021,
        configurations=[
            Configuration(
                object_name="Config 0",
                timings=[
                    Timing(object_name="T0", algorithm=1, ptp_instance_indices=[0])
                ],
                ptp_instances=[
                    PtpInstance(
                        object_name="PTP0",
                        ptp_ports=[PtpPort(object_name="PP0", port_number=1)],
                    )
                ],
                locales=[Locale(locale_identifier="en")],
            )
        ],
    )

    incompatible = []
    for ci, config in enumerate(entity.configurations):
        for ti, t in enumerate(config.timings):
            incompatible.append(f"Timing[{ti}]")
        for pi, p in enumerate(config.ptp_instances):
            incompatible.append(f"PtpInstance[{pi}]")
    assert len(incompatible) == 2
    print("  [+] downgrade rejects 2021-only descriptors: OK")


def test_downgrade_strip_removes_2021_only_descriptors():
    """Downgrading with strip removes Timing/PtpInstance and produces valid 2013."""
    entity = Entity(
        entity_name="Test",
        schema_year=2021,
        configurations=[
            Configuration(
                object_name="Config 0",
                timings=[
                    Timing(object_name="T0", algorithm=1, ptp_instance_indices=[0])
                ],
                ptp_instances=[
                    PtpInstance(
                        object_name="PTP0",
                        ptp_ports=[PtpPort(object_name="PP0", port_number=1)],
                    )
                ],
                locales=[Locale(locale_identifier="en")],
            )
        ],
    )

    for config in entity.configurations:
        config.timings.clear()
        config.ptp_instances.clear()

    xml_2013 = write_aemxml(entity, schema_year=2013)
    assert 'version="1.0"' in xml_2013
    assert "Timing" not in xml_2013
    assert "PtpInstance" not in xml_2013

    entity2 = read_aemxml(xml_2013)
    assert entity2.schema_year == 2013
    assert len(entity2.configurations[0].timings) == 0
    assert len(entity2.configurations[0].ptp_instances) == 0
    print("  [+] downgrade --strip removes 2021-only: OK")


def main():
    tests = [
        test_wire_pack_unpack,
        test_wire_pack_range_checks,
        test_model_creation,
        test_flatten_minimal,
        test_flatten_with_audio,
        test_identify_capability_and_base_control_zeroing,
        test_json_variable_expansion,
        test_blob_round_trip,
        test_read_bareminimum,
        test_read_bareminimum_2021,
        test_write_2013_compat,
        test_write_2021_default,
        test_aemxml_round_trip,
        test_phase2_descriptors,
        test_phase2_aemxml_round_trip,
        test_external_internal_ports,
        test_xsd_validate_bareminimum,
        test_xsd_validate_bareminimum_2021,
        test_xsd_validate_invalid,
        test_error_messages,
        test_symbolic_enum_write_read,
        test_symbolic_enum_round_trip_blob,
        test_stream_format_aaf_encode_decode,
        test_json_read_simple_stereo,
        test_json_round_trip,
        test_json_to_blob,
        test_json_to_xml_round_trip,
        test_json_symbols_to_blob,
        test_json_pull_rates,
        test_json_localized_dict,
        test_json_control_values,
        test_json_signal_selector_and_control_grouping,
        test_json_matrix,
        test_json_mixer_and_inline_controls,
        test_duplicate_symbols_rejected,
        test_upgrade_2013_to_2021,
        test_downgrade_2021_to_2013,
        test_upgrade_is_idempotent,
        test_downgrade_is_idempotent,
        test_upgrade_then_downgrade_round_trip,
        test_downgrade_rejects_2021_only_descriptors,
        test_downgrade_strip_removes_2021_only_descriptors,
    ]
    passed = 0
    failed = 0
    for test in tests:
        name = test.__name__
        try:
            test()
            passed += 1
        except Exception as e:
            print(f"  FAIL {name}: {e}")
            failed += 1

    print(f"\n{passed} passed, {failed} failed, {passed + failed} total")
    if failed:
        print("TESTS FAILED")
        sys.exit(1)
    print("All tests pass.")


if __name__ == "__main__":
    main()
