#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overloads for ATDECC AEM descriptor types defined in
/// atdecc_aem_descriptor.hpp. Split out so consumers that only need the data
/// structures do not pay the compile-time cost of <format>.

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/ieee/ieee_ethernet_format.hpp"
#include "statusbar/tsn/tsn_clock_identity_format.hpp"

#include <cstddef>
#include <cstdint>
#include <format>

namespace statusbar::atdecc::aem {

/// Format a descriptor type/index pair
/// @param out Output iterator to write formatted text to
/// @param type Descriptor type code
/// @param index Descriptor index
template <typename OutputIt>
auto format_descriptor_header_to(OutputIt out, doublet_t type, doublet_t index) -> OutputIt
{
    return std::format_to(out, "{} [{}]", descriptor_type_name(static_cast<uint16_t>(type)), static_cast<uint16_t>(index));
}

/// Format an AtdeccString to an output iterator
/// @param out Output iterator to write formatted text to
/// @param str ATDECC string to format
template <typename OutputIt>
auto format_to(OutputIt out, AtdeccString const& str) -> OutputIt
{
    return std::format_to(out, "\"{}\"", str.as_string_view());
}

/// Format a descriptor-type reference as "NAME (0xHHHH)" for descriptor
/// fields that point at another descriptor by type — signal_type,
/// clock_source_location_type, target_descriptor_type, etc. Invalid or
/// unknown codes still render their numeric value so the raw wire value
/// is never hidden.
/// @param out Output iterator to write formatted text to
/// @param type Descriptor type code
template <typename OutputIt>
auto format_descriptor_type_ref(OutputIt out, uint16_t type) -> OutputIt
{
    return std::format_to(out, "{} ({:#06x})", descriptor_type_name(type), type);
}

/// Format a DescriptorRef as a compact single-line identifier
/// (e.g. "STREAM_INPUT[3]")
/// @param out Output iterator to write formatted text to
/// @param d Descriptor reference to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorRef const& d) -> OutputIt
{
    return std::format_to(out, "{}[{}]", descriptor_type_name(d.descriptor_type), d.descriptor_index);
}

/// Format a single descriptor_counts entry (for CONFIGURATION descriptor)
/// @param out Output iterator to write formatted text to
/// @param e Descriptor count entry to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorCountEntry const& e) -> OutputIt
{
    uint16_t const type = e.descriptor_type.get();
    return std::format_to(out, "{} ({:#06x}): {}", descriptor_type_name(type), type, e.count.get());
}

/// Format a single AudioMapping entry
/// @param out Output iterator to write formatted text to
/// @param m Audio mapping entry to format
template <typename OutputIt>
auto format_to(OutputIt out, AudioMapping const& m) -> OutputIt
{
    return std::format_to(
        out,
        "stream[{}]ch{} -> cluster[{}]ch{}",
        m.mapping_stream_index.get(),
        m.mapping_stream_channel.get(),
        m.mapping_cluster_offset.get(),
        m.mapping_cluster_channel.get());
}

/// Format a single VideoMapping entry
/// @param out Output iterator to write formatted text to
/// @param m Video mapping entry to format
template <typename OutputIt>
auto format_to(OutputIt out, VideoMapping const& m) -> OutputIt
{
    return std::format_to(
        out,
        "stream[{}] prog={} es={} -> cluster[{}]",
        m.mapping_stream_index.get(),
        m.mapping_program_stream.get(),
        m.mapping_elementary_stream.get(),
        m.mapping_cluster_offset.get());
}

/// Format a single SensorMapping entry
/// @param out Output iterator to write formatted text to
/// @param m Sensor mapping entry to format
template <typename OutputIt>
auto format_to(OutputIt out, SensorMapping const& m) -> OutputIt
{
    return std::format_to(
        out,
        "stream[{}]ch{} -> cluster[{}]ch{}",
        m.mapping_stream_index.get(),
        m.mapping_stream_channel.get(),
        m.mapping_cluster_offset.get(),
        m.mapping_cluster_channel.get());
}

/// @param out Output iterator to write formatted text to
/// @param d Entity descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorEntity const& d) -> OutputIt
{
    out = std::format_to(out, "        ENTITY Descriptor:\n");
    out = std::format_to(out, "          entity_id: ");
    out = ieee::format_to(out, d.entity_id);
    out = std::format_to(out, "\n          entity_model_id: ");
    out = ieee::format_to(out, d.entity_model_id);
    out = std::format_to(out, "\n          entity_name: \"{}\"\n", d.entity_name.as_string_view());
    out = std::format_to(out, "          firmware_version: \"{}\"\n", d.firmware_version.as_string_view());
    out = std::format_to(out, "          group_name: \"{}\"\n", d.group_name.as_string_view());
    out = std::format_to(out, "          serial_number: \"{}\"\n", d.serial_number.as_string_view());
    out = std::format_to(
        out,
        "          capabilities: {:#010x} talker_sources={} listener_sinks={}\n",
        d.entity_capabilities.get(),
        d.talker_stream_sources.get(),
        d.listener_stream_sinks.get());
    out = std::format_to(
        out, "          configurations: count={} current={}\n", d.configurations_count.get(), d.current_configuration.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Configuration descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorConfiguration const& d) -> OutputIt
{
    out = std::format_to(out, "        CONFIGURATION Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(
        out,
        "          descriptor_counts: count={} offset={}\n",
        d.descriptor_counts_count.get(),
        d.descriptor_counts_offset.get());
    auto const entries = d.used_descriptor_counts();
    for (size_t i = 0; i < entries.size(); ++i) {
        out = std::format_to(out, "            [{}] = ", i);
        out = format_to(out, entries[i]);
        out = std::format_to(out, "\n");
    }
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Audio unit descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorAudioUnit const& d) -> OutputIt
{
    out = std::format_to(out, "        AUDIO_UNIT Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          clock_domain_index: {}\n", d.clock_domain_index.get());
    out = std::format_to(out, "          current_sampling_rate: {}\n", d.current_sampling_rate.get());
    out = std::format_to(
        out, "          stream_input_ports: {} (base={})\n", d.number_of_stream_input_ports.get(), d.base_stream_input_port.get());
    out = std::format_to(
        out,
        "          stream_output_ports: {} (base={})\n",
        d.number_of_stream_output_ports.get(),
        d.base_stream_output_port.get());
    out = std::format_to(
        out, "          sampling_rates: count={} offset={}\n", d.sampling_rates_count.get(), d.sampling_rates_offset.get());
    auto const rates = d.used_sampling_rates();
    for (size_t i = 0; i < rates.size(); ++i) {
        out = std::format_to(out, "            [{}] = {:#010x}\n", i, rates[i].get());
    }
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Video unit descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorVideoUnit const& d) -> OutputIt
{
    out = std::format_to(out, "        VIDEO_UNIT Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          clock_domain_index: {}\n", d.clock_domain_index.get());
    out = std::format_to(
        out, "          stream_input_ports: {} (base={})\n", d.number_of_stream_input_ports.get(), d.base_stream_input_port.get());
    out = std::format_to(
        out,
        "          stream_output_ports: {} (base={})\n",
        d.number_of_stream_output_ports.get(),
        d.base_stream_output_port.get());
    out = std::format_to(out, "          controls: {} (base={})\n", d.number_of_controls.get(), d.base_control.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Sensor unit descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorSensorUnit const& d) -> OutputIt
{
    out = std::format_to(out, "        SENSOR_UNIT Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          clock_domain_index: {}\n", d.clock_domain_index.get());
    out = std::format_to(
        out, "          stream_input_ports: {} (base={})\n", d.number_of_stream_input_ports.get(), d.base_stream_input_port.get());
    out = std::format_to(
        out,
        "          stream_output_ports: {} (base={})\n",
        d.number_of_stream_output_ports.get(),
        d.base_stream_output_port.get());
    out = std::format_to(out, "          controls: {} (base={})\n", d.number_of_controls.get(), d.base_control.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Stream descriptor to format
/// @param type_name Human-readable stream type name
template <typename OutputIt>
auto format_stream_descriptor(OutputIt out, DescriptorStream const& d, char const* type_name) -> OutputIt
{
    out = std::format_to(out, "        {} Descriptor [{}]:\n", type_name, d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          clock_domain_index: {}\n", d.clock_domain_index.get());
    out = std::format_to(out, "          stream_flags: {:#06x}\n", d.stream_flags.get());
    out = std::format_to(out, "          current_format: ");
    for (size_t i = 0; i < 8; ++i) {
        out = std::format_to(out, "{:02x} ", d.current_format.span()[i]);
    }
    out = std::format_to(out, "\n");
    out = std::format_to(out, "          formats: count={} offset={}\n", d.number_of_formats.get(), d.formats_offset.get());
    auto const formats = d.used_stream_formats();
    for (size_t i = 0; i < formats.size(); ++i) {
        out = std::format_to(out, "            [{}] = ", i);
        out = ieee::format_to(out, formats[i]);
        out = std::format_to(out, "\n");
    }
    out = std::format_to(out, "          avb_interface_index: {}\n", d.avb_interface_index.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Stream descriptor to format (uses STREAM as generic banner)
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorStream const& d) -> OutputIt
{
    uint16_t const type = d.descriptor_type.get();
    if (type == DESCRIPTOR_STREAM_INPUT) {
        return format_stream_descriptor(out, d, "STREAM_INPUT");
    }
    if (type == DESCRIPTOR_STREAM_OUTPUT) {
        return format_stream_descriptor(out, d, "STREAM_OUTPUT");
    }
    return format_stream_descriptor(out, d, "STREAM");
}

/// @param out Output iterator to write formatted text to
/// @param d Jack descriptor to format
/// @param type_name Human-readable jack type name
template <typename OutputIt>
auto format_jack_descriptor(OutputIt out, DescriptorJack const& d, char const* type_name) -> OutputIt
{
    out = std::format_to(out, "        {} Descriptor [{}]:\n", type_name, d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          jack_type: {:#06x} flags: {:#06x}\n", d.jack_type.get(), d.jack_flags.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Jack descriptor to format (dispatches on descriptor_type)
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorJack const& d) -> OutputIt
{
    uint16_t const type = d.descriptor_type.get();
    if (type == DESCRIPTOR_JACK_INPUT) {
        return format_jack_descriptor(out, d, "JACK_INPUT");
    }
    if (type == DESCRIPTOR_JACK_OUTPUT) {
        return format_jack_descriptor(out, d, "JACK_OUTPUT");
    }
    return format_jack_descriptor(out, d, "JACK");
}

/// @param out Output iterator to write formatted text to
/// @param d AVB interface descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorAvbInterface const& d) -> OutputIt
{
    out = std::format_to(out, "        AVB_INTERFACE Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          mac_address: ");
    out = ieee::format_to(out, d.mac_address);
    out = std::format_to(out, "\n          interface_flags: {:#06x}\n", d.interface_flags.get());
    out = std::format_to(out, "          clock_identity: ");
    out = ieee::format_to(out, d.clock_identity);
    out = std::format_to(out, "\n          domain_number: {}\n", d.domain_number.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Clock source descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorClockSource const& d) -> OutputIt
{
    out = std::format_to(out, "        CLOCK_SOURCE Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(
        out, "          clock_source_type: {:#06x} flags: {:#06x}\n", d.clock_source_type.get(), d.clock_source_flags.get());
    out = std::format_to(out, "          clock_source_identifier: ");
    out = ieee::format_to(out, d.clock_source_identifier);
    out = std::format_to(out, "\n          location: type=");
    out = format_descriptor_type_ref(out, d.clock_source_location_type.get());
    out = std::format_to(out, " index={}\n", d.clock_source_location_index.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Memory object descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorMemoryObject const& d) -> OutputIt
{
    out = std::format_to(out, "        MEMORY_OBJECT Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          memory_object_type: {:#06x}\n", d.memory_object_type.get());
    out = std::format_to(out, "          target: type=");
    out = format_descriptor_type_ref(out, d.target_descriptor_type.get());
    out = std::format_to(out, " index={}\n", d.target_descriptor_index.get());
    out = std::format_to(out, "          start_address: {}\n", d.start_address.get());
    out = std::format_to(out, "          length: {} (max={})\n", d.length.get(), static_cast<uint64_t>(d.maximum_length));
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Locale descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorLocale const& d) -> OutputIt
{
    out = std::format_to(out, "        LOCALE Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          locale_identifier: \"{}\"\n", d.locale_identifier.as_string_view());
    out = std::format_to(out, "          strings: count={} base={}\n", d.number_of_strings.get(), d.base_strings.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Strings descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorStrings const& d) -> OutputIt
{
    out = std::format_to(out, "        STRINGS Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          string_0: \"{}\"\n", d.string_0.as_string_view());
    out = std::format_to(out, "          string_1: \"{}\"\n", d.string_1.as_string_view());
    out = std::format_to(out, "          string_2: \"{}\"\n", d.string_2.as_string_view());
    out = std::format_to(out, "          string_3: \"{}\"\n", d.string_3.as_string_view());
    out = std::format_to(out, "          string_4: \"{}\"\n", d.string_4.as_string_view());
    out = std::format_to(out, "          string_5: \"{}\"\n", d.string_5.as_string_view());
    out = std::format_to(out, "          string_6: \"{}\"\n", d.string_6.as_string_view());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Stream port descriptor to format
/// @param type_name Human-readable port type name
template <typename OutputIt>
auto format_stream_port_descriptor(OutputIt out, DescriptorStreamPort const& d, char const* type_name) -> OutputIt
{
    out = std::format_to(out, "        {} Descriptor [{}]:\n", type_name, d.descriptor_index.get());
    out = std::format_to(out, "          clock_domain_index: {}\n", d.clock_domain_index.get());
    out = std::format_to(out, "          port_flags: {:#06x}\n", d.port_flags.get());
    out = std::format_to(out, "          clusters: count={} base={}\n", d.number_of_clusters.get(), d.base_cluster.get());
    out = std::format_to(out, "          maps: count={} base={}\n", d.number_of_maps.get(), static_cast<uint16_t>(d.base_map));
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Stream port descriptor to format (dispatches on descriptor_type)
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorStreamPort const& d) -> OutputIt
{
    uint16_t const type = d.descriptor_type.get();
    if (type == DESCRIPTOR_STREAM_PORT_INPUT) {
        return format_stream_port_descriptor(out, d, "STREAM_PORT_INPUT");
    }
    if (type == DESCRIPTOR_STREAM_PORT_OUTPUT) {
        return format_stream_port_descriptor(out, d, "STREAM_PORT_OUTPUT");
    }
    return format_stream_port_descriptor(out, d, "STREAM_PORT");
}

/// @param out Output iterator to write formatted text to
/// @param d External port descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorExternalPort const& d) -> OutputIt
{
    uint16_t const type = d.descriptor_type.get();
    char const* label = (type == DESCRIPTOR_EXTERNAL_PORT_OUTPUT) ? "EXTERNAL_PORT_OUTPUT" : "EXTERNAL_PORT_INPUT";
    out = std::format_to(out, "        {} Descriptor [{}]:\n", label, d.descriptor_index.get());
    out = std::format_to(out, "          clock_domain_index: {}\n", d.clock_domain_index.get());
    out = std::format_to(out, "          port_flags: {:#06x}\n", d.port_flags.get());
    out = std::format_to(out, "          controls: count={} base={}\n", d.number_of_controls.get(), d.base_control.get());
    out = std::format_to(out, "          signal: type=");
    out = format_descriptor_type_ref(out, d.signal_source.signal_type.get());
    out = std::format_to(out, " index={} output={}\n", d.signal_source.signal_index.get(), d.signal_source.signal_output.get());
    out = std::format_to(out, "          block_latency: {}\n", d.block_latency.get());
    out = std::format_to(out, "          jack_index: {}\n", d.jack_index.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Internal port descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorInternalPort const& d) -> OutputIt
{
    uint16_t const type = d.descriptor_type.get();
    char const* label = (type == DESCRIPTOR_INTERNAL_PORT_OUTPUT) ? "INTERNAL_PORT_OUTPUT" : "INTERNAL_PORT_INPUT";
    out = std::format_to(out, "        {} Descriptor [{}]:\n", label, d.descriptor_index.get());
    out = std::format_to(out, "          clock_domain_index: {}\n", d.clock_domain_index.get());
    out = std::format_to(out, "          port_flags: {:#06x}\n", d.port_flags.get());
    out = std::format_to(out, "          controls: count={} base={}\n", d.number_of_controls.get(), d.base_control.get());
    out = std::format_to(out, "          signal: type=");
    out = format_descriptor_type_ref(out, d.signal_source.signal_type.get());
    out = std::format_to(out, " index={} output={}\n", d.signal_source.signal_index.get(), d.signal_source.signal_output.get());
    out = std::format_to(out, "          block_latency: {}\n", d.block_latency.get());
    out = std::format_to(out, "          internal_index: {}\n", d.internal_index.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Audio cluster descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorAudioCluster const& d) -> OutputIt
{
    out = std::format_to(out, "        AUDIO_CLUSTER Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          channel_count: {}\n", d.channel_count.get());
    out = std::format_to(out, "          format: {:#04x}\n", d.format.get());
    out = std::format_to(out, "          signal: type=");
    out = format_descriptor_type_ref(out, d.signal_source.signal_type.get());
    out = std::format_to(out, " index={} output={}\n", d.signal_source.signal_index.get(), d.signal_source.signal_output.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Video cluster descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorVideoCluster const& d) -> OutputIt
{
    out = std::format_to(out, "        VIDEO_CLUSTER Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          signal: type=");
    out = format_descriptor_type_ref(out, d.signal_source.signal_type.get());
    out = std::format_to(out, " index={} output={}\n", d.signal_source.signal_index.get(), d.signal_source.signal_output.get());
    out = std::format_to(out, "          format: {:#04x}\n", d.format.get());
    out = std::format_to(out, "          current_sampling_rate: {}\n", d.current_sampling_rate.get());
    out = std::format_to(out, "          current_aspect_ratio: {}\n", d.current_aspect_ratio.get());
    out = std::format_to(out, "          current_size: {}\n", d.current_size.get());
    out = std::format_to(out, "          current_color_space: {}\n", d.current_color_space.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Sensor cluster descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorSensorCluster const& d) -> OutputIt
{
    out = std::format_to(out, "        SENSOR_CLUSTER Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          signal: type=");
    out = format_descriptor_type_ref(out, d.signal_source.signal_type.get());
    out = std::format_to(out, " index={} output={}\n", d.signal_source.signal_index.get(), d.signal_source.signal_output.get());
    out = std::format_to(out, "          current_format: ");
    for (size_t i = 0; i < 8; ++i) {
        out = std::format_to(out, "{:02x} ", d.current_format.span()[i]);
    }
    out = std::format_to(out, "\n");
    out = std::format_to(out, "          current_sampling_rate: {}\n", d.current_sampling_rate.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Audio map descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorAudioMap const& d) -> OutputIt
{
    out = std::format_to(out, "        AUDIO_MAP Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          mappings: count={} offset={}\n", d.number_of_mappings.get(), d.mappings_offset.get());
    auto const mappings = d.used_mappings();
    for (size_t i = 0; i < mappings.size(); ++i) {
        out = std::format_to(out, "            [{}] = ", i);
        out = format_to(out, mappings[i]);
        out = std::format_to(out, "\n");
    }
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Video map descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorVideoMap const& d) -> OutputIt
{
    out = std::format_to(out, "        VIDEO_MAP Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          mappings: count={} offset={}\n", d.number_of_mappings.get(), d.mappings_offset.get());
    auto const mappings = d.used_mappings();
    for (size_t i = 0; i < mappings.size(); ++i) {
        out = std::format_to(out, "            [{}] = ", i);
        out = format_to(out, mappings[i]);
        out = std::format_to(out, "\n");
    }
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Sensor map descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorSensorMap const& d) -> OutputIt
{
    out = std::format_to(out, "        SENSOR_MAP Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          mappings: count={} offset={}\n", d.number_of_mappings.get(), d.mappings_offset.get());
    auto const mappings = d.used_mappings();
    for (size_t i = 0; i < mappings.size(); ++i) {
        out = std::format_to(out, "            [{}] = ", i);
        out = format_to(out, mappings[i]);
        out = std::format_to(out, "\n");
    }
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Control descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorControl const& d) -> OutputIt
{
    out = std::format_to(out, "        CONTROL Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          control_value_type: {:#06x}\n", d.control_value_type.get());
    out = std::format_to(out, "          control_type: ");
    out = ieee::format_to(out, d.control_type);
    out = std::format_to(out, "\n          values: count={} offset={}\n", d.number_of_values.get(), d.values_offset.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Signal selector descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorSignalSelector const& d) -> OutputIt
{
    out = std::format_to(out, "        SIGNAL_SELECTOR Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          current_signal: type=");
    out = format_descriptor_type_ref(out, d.current_signal.signal_type.get());
    out = std::format_to(out, " index={} output={}\n", d.current_signal.signal_index.get(), d.current_signal.signal_output.get());
    out = std::format_to(out, "          default_signal: type=");
    out = format_descriptor_type_ref(out, d.default_signal.signal_type.get());
    out = std::format_to(out, " index={} output={}\n", d.default_signal.signal_index.get(), d.default_signal.signal_output.get());
    out = std::format_to(out, "          sources: count={} offset={}\n", d.number_of_sources.get(), d.sources_offset.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Mixer descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorMixer const& d) -> OutputIt
{
    out = std::format_to(out, "        MIXER Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          control_value_type: {:#06x}\n", d.control_value_type.get());
    out = std::format_to(out, "          sources: count={} offset={}\n", d.number_of_sources.get(), d.sources_offset.get());
    out = std::format_to(out, "          value_offset: {}\n", d.value_offset.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Matrix descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorMatrix const& d) -> OutputIt
{
    out = std::format_to(out, "        MATRIX Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          control_value_type: {:#06x}\n", d.control_value_type.get());
    out = std::format_to(out, "          control_type: ");
    out = ieee::format_to(out, d.control_type);
    out = std::format_to(out, "\n          dimensions: width={} height={}\n", d.width.get(), d.height.get());
    out = std::format_to(out, "          values: count={} offset={}\n", d.number_of_values.get(), d.values_offset.get());
    out = std::format_to(out, "          sources: count={} base={}\n", d.number_of_sources.get(), d.base_source.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Matrix signal descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorMatrixSignal const& d) -> OutputIt
{
    out = std::format_to(out, "        MATRIX_SIGNAL Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          signals: count={} offset={}\n", d.signals_count.get(), d.signals_offset.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Signal splitter descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorSignalSplitter const& d) -> OutputIt
{
    out = std::format_to(out, "        SIGNAL_SPLITTER Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          signal: type=");
    out = format_descriptor_type_ref(out, d.signal_source.signal_type.get());
    out = std::format_to(out, " index={} output={}\n", d.signal_source.signal_index.get(), d.signal_source.signal_output.get());
    out = std::format_to(out, "          number_of_outputs: {}\n", d.number_of_outputs.get());
    out = std::format_to(
        out, "          splitter_map: count={} offset={}\n", d.splitter_map_count.get(), d.splitter_map_offset.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Signal combiner descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorSignalCombiner const& d) -> OutputIt
{
    out = std::format_to(out, "        SIGNAL_COMBINER Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(
        out, "          combiner_map: count={} offset={}\n", d.combiner_map_count.get(), d.combiner_map_offset.get());
    out = std::format_to(out, "          sources: count={} offset={}\n", d.number_of_sources.get(), d.sources_offset.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Signal demultiplexer descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorSignalDemultiplexer const& d) -> OutputIt
{
    out = std::format_to(out, "        SIGNAL_DEMULTIPLEXER Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          signal: type=");
    out = format_descriptor_type_ref(out, d.signal_source.signal_type.get());
    out = std::format_to(out, " index={} output={}\n", d.signal_source.signal_index.get(), d.signal_source.signal_output.get());
    out = std::format_to(out, "          number_of_outputs: {}\n", d.number_of_outputs.get());
    out = std::format_to(
        out,
        "          demultiplexer_map: count={} offset={}\n",
        d.demultiplexer_map_count.get(),
        d.demultiplexer_map_offset.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Signal multiplexer descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorSignalMultiplexer const& d) -> OutputIt
{
    out = std::format_to(out, "        SIGNAL_MULTIPLEXER Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(
        out, "          multiplexer_map: count={} offset={}\n", d.multiplexer_map_count.get(), d.multiplexer_map_offset.get());
    out = std::format_to(out, "          sources: count={} offset={}\n", d.number_of_sources.get(), d.sources_offset.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Signal transcoder descriptor to format
/// @param data_size Size of the backing buffer; must be at least MINIMUM_LENGTH.
///        When smaller than LENGTH the descriptor is a 2013-compat input that
///        omits transcoder_type — reading it would run past the buffer.
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorSignalTranscoder const& d, size_t data_size = DescriptorSignalTranscoder::LENGTH) -> OutputIt
{
    out = std::format_to(out, "        SIGNAL_TRANSCODER Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          control_value_type: {:#06x}\n", d.control_value_type.get());
    if (data_size >= DescriptorSignalTranscoder::LENGTH) {
        out = std::format_to(out, "          transcoder_type: ");
        out = ieee::format_to(out, d.transcoder_type);
        out = std::format_to(out, "\n");
    } else {
        out = std::format_to(out, "          transcoder_type: <none (pre-2021)>\n");
    }
    out = std::format_to(out, "          signal: type=");
    out = format_descriptor_type_ref(out, d.signal_source.signal_type.get());
    out = std::format_to(out, " index={} output={}\n", d.signal_source.signal_index.get(), d.signal_source.signal_output.get());
    out = std::format_to(out, "          values: count={} offset={}\n", d.number_of_values.get(), d.values_offset.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Clock domain descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorClockDomain const& d) -> OutputIt
{
    out = std::format_to(out, "        CLOCK_DOMAIN Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          clock_source_index: {}\n", d.clock_source_index.get());
    out = std::format_to(
        out, "          clock_sources: count={} offset={}\n", d.clock_sources_count.get(), d.clock_sources_offset.get());
    auto const sources = d.used_clock_sources();
    for (size_t i = 0; i < sources.size(); ++i) {
        out = std::format_to(out, "            [{}] = {}\n", i, sources[i].get());
    }
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Control block descriptor to format
/// @param data_size Size of the backing buffer; must be at least MINIMUM_LENGTH.
///        When smaller than LENGTH the descriptor is a 2013-compat input that
///        omits signal_type/signal_index/signal_output — reading them would
///        run past the buffer.
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorControlBlock const& d, size_t data_size = DescriptorControlBlock::LENGTH) -> OutputIt
{
    out = std::format_to(out, "        CONTROL_BLOCK Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          controls: count={} base={}\n", d.number_of_controls.get(), d.base_control.get());
    out = std::format_to(out, "          final_control_index: {}\n", d.final_control_index.get());
    if (data_size >= DescriptorControlBlock::LENGTH) {
        out = std::format_to(out, "          signal: type=");
        out = format_descriptor_type_ref(out, d.signal_source.signal_type.get());
        out = std::format_to(out, " index={} output={}\n", d.signal_source.signal_index.get(), d.signal_source.signal_output.get());
    } else {
        out = std::format_to(out, "          signal: <none (pre-2021)>\n");
    }
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d Timing descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorTiming const& d) -> OutputIt
{
    out = std::format_to(out, "        TIMING Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          algorithm: {:#06x}\n", d.algorithm.get());
    out = std::format_to(
        out, "          ptp_instances: count={} offset={}\n", d.number_of_ptp_instances.get(), d.ptp_instances_offset.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d PTP instance descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorPtpInstance const& d) -> OutputIt
{
    out = std::format_to(out, "        PTP_INSTANCE Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          clock_identity: ");
    out = ieee::format_to(out, d.clock_identity);
    out = std::format_to(out, "\n          flags: {:#010x}\n", d.flags.get());
    out = std::format_to(out, "          controls: count={} base={}\n", d.number_of_controls.get(), d.base_control.get());
    out = std::format_to(out, "          ptp_ports: count={} base={}\n", d.number_of_ptp_ports.get(), d.base_ptp_port.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param d PTP port descriptor to format
template <typename OutputIt>
auto format_to(OutputIt out, DescriptorPtpPort const& d) -> OutputIt
{
    out = std::format_to(out, "        PTP_PORT Descriptor [{}]:\n", d.descriptor_index.get());
    out = std::format_to(out, "          object_name: \"{}\"\n", d.object_name.as_string_view());
    out = std::format_to(out, "          port_number: {}\n", d.port_number.get());
    out = std::format_to(out, "          port_type: {:#06x}\n", d.port_type.get());
    out = std::format_to(out, "          flags: {:#010x}\n", d.flags.get());
    out = std::format_to(out, "          avb_interface_index: {}\n", d.avb_interface_index.get());
    out = std::format_to(out, "          profile_identifier: ");
    out = ieee::format_to(out, d.profile_identifier);
    out = std::format_to(out, "\n");
    return out;
}

}  // namespace statusbar::atdecc::aem
