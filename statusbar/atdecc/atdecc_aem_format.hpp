#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ATDECC Entity Model (AEM) Descriptor and Command Printing - IEEE 1722.1
/// Functions for formatting AEM descriptors and command payloads to output iterators

#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aecp_aem_format.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_command_format.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor_format.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/ieee/ieee_ethernet_format.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"
#include "statusbar/tsn/tsn_clock_identity_format.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace statusbar::atdecc::aem {

//
// Helper function for hex dump
//
/// @param out Output iterator to write formatted text to
/// @param data Raw bytes to dump as hex
template <typename OutputIt>
auto format_hex_dump(OutputIt out, std::span<uint8_t const> data) -> OutputIt
{
    for (auto const octet_value : data) {
        out = std::format_to(out, "{:02x} ", octet_value);
    }
    out = std::format_to(out, "\n");
    return out;
}

//
// Descriptor Parsing
//
/// Parsed descriptor result - contains the descriptor type and a union of all known descriptor structs
struct ParsedDescriptor
{
    union DescriptorData
    {
        struct
        {
            ieee::doublet_t descriptor_type;
            ieee::doublet_t descriptor_index;
        } common;
        DescriptorEntity entity;
        DescriptorConfiguration configuration;
        DescriptorAudioUnit audio_unit;
        DescriptorVideoUnit video_unit;
        DescriptorSensorUnit sensor_unit;
        DescriptorStream stream;
        DescriptorJack jack;
        DescriptorAvbInterface avb_interface;
        DescriptorClockSource clock_source;
        DescriptorMemoryObject memory_object;
        DescriptorLocale locale;
        DescriptorStrings strings;
        DescriptorStreamPort stream_port;
        DescriptorExternalPort external_port;
        DescriptorInternalPort internal_port;
        DescriptorAudioCluster audio_cluster;
        DescriptorVideoCluster video_cluster;
        DescriptorSensorCluster sensor_cluster;
        DescriptorAudioMap audio_map;
        DescriptorVideoMap video_map;
        DescriptorSensorMap sensor_map;
        DescriptorClockDomain clock_domain;
        DescriptorControl control;
        DescriptorSignalSelector signal_selector;
        DescriptorMixer mixer;
        DescriptorMatrix matrix;
        DescriptorMatrixSignal matrix_signal;
        DescriptorSignalSplitter signal_splitter;
        DescriptorSignalCombiner signal_combiner;
        DescriptorSignalDemultiplexer signal_demultiplexer;
        DescriptorSignalMultiplexer signal_multiplexer;
        DescriptorSignalTranscoder signal_transcoder;
        DescriptorControlBlock control_block;
        DescriptorTiming timing;
        DescriptorPtpInstance ptp_instance;
        DescriptorPtpPort ptp_port;
        std::array<uint8_t, AEM_DESCRIPTOR_SIZE> raw;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init) - memset initializes union
        DescriptorData() { std::memset(this, 0, sizeof(*this)); }
    } data;
};

/// Minimum length lookup table for descriptor types (indexed by descriptor type value)
/// A value of 0 means the descriptor type is not supported or has no fixed minimum length
constexpr std::array<size_t, NUM_DESCRIPTOR_TYPES> descriptor_min_lengths = []() -> std::array<size_t, NUM_DESCRIPTOR_TYPES> {
    std::array<size_t, NUM_DESCRIPTOR_TYPES> table{};
    table[DESCRIPTOR_ENTITY] = DescriptorEntity::LENGTH;
    table[DESCRIPTOR_CONFIGURATION] = DescriptorConfiguration::LENGTH;
    table[DESCRIPTOR_AUDIO_UNIT] = DescriptorAudioUnit::LENGTH;
    table[DESCRIPTOR_VIDEO_UNIT] = DescriptorVideoUnit::LENGTH;
    table[DESCRIPTOR_SENSOR_UNIT] = DescriptorSensorUnit::LENGTH;
    table[DESCRIPTOR_STREAM_INPUT] = DescriptorStream::MINIMUM_LENGTH;
    table[DESCRIPTOR_STREAM_OUTPUT] = DescriptorStream::MINIMUM_LENGTH;
    table[DESCRIPTOR_JACK_INPUT] = DescriptorJack::LENGTH;
    table[DESCRIPTOR_JACK_OUTPUT] = DescriptorJack::LENGTH;
    table[DESCRIPTOR_AVB_INTERFACE] = DescriptorAvbInterface::MINIMUM_LENGTH;
    table[DESCRIPTOR_CLOCK_SOURCE] = DescriptorClockSource::LENGTH;
    table[DESCRIPTOR_MEMORY_OBJECT] = DescriptorMemoryObject::LENGTH;
    table[DESCRIPTOR_LOCALE] = DescriptorLocale::LENGTH;
    table[DESCRIPTOR_STRINGS] = DescriptorStrings::LENGTH;
    table[DESCRIPTOR_STREAM_PORT_INPUT] = DescriptorStreamPort::LENGTH;
    table[DESCRIPTOR_STREAM_PORT_OUTPUT] = DescriptorStreamPort::LENGTH;
    table[DESCRIPTOR_EXTERNAL_PORT_INPUT] = DescriptorExternalPort::LENGTH;
    table[DESCRIPTOR_EXTERNAL_PORT_OUTPUT] = DescriptorExternalPort::LENGTH;
    table[DESCRIPTOR_INTERNAL_PORT_INPUT] = DescriptorInternalPort::LENGTH;
    table[DESCRIPTOR_INTERNAL_PORT_OUTPUT] = DescriptorInternalPort::LENGTH;
    table[DESCRIPTOR_AUDIO_CLUSTER] = DescriptorAudioCluster::MINIMUM_LENGTH;
    table[DESCRIPTOR_VIDEO_CLUSTER] = DescriptorVideoCluster::LENGTH;
    table[DESCRIPTOR_SENSOR_CLUSTER] = DescriptorSensorCluster::LENGTH;
    table[DESCRIPTOR_AUDIO_MAP] = DescriptorAudioMap::LENGTH;
    table[DESCRIPTOR_VIDEO_MAP] = DescriptorVideoMap::LENGTH;
    table[DESCRIPTOR_SENSOR_MAP] = DescriptorSensorMap::LENGTH;
    table[DESCRIPTOR_CONTROL] = DescriptorControl::LENGTH;
    table[DESCRIPTOR_SIGNAL_SELECTOR] = DescriptorSignalSelector::LENGTH;
    table[DESCRIPTOR_MIXER] = DescriptorMixer::LENGTH;
    table[DESCRIPTOR_MATRIX] = DescriptorMatrix::LENGTH;
    table[DESCRIPTOR_MATRIX_SIGNAL] = DescriptorMatrixSignal::LENGTH;
    table[DESCRIPTOR_SIGNAL_SPLITTER] = DescriptorSignalSplitter::LENGTH;
    table[DESCRIPTOR_SIGNAL_COMBINER] = DescriptorSignalCombiner::LENGTH;
    table[DESCRIPTOR_SIGNAL_DEMULTIPLEXER] = DescriptorSignalDemultiplexer::LENGTH;
    table[DESCRIPTOR_SIGNAL_MULTIPLEXER] = DescriptorSignalMultiplexer::LENGTH;
    table[DESCRIPTOR_SIGNAL_TRANSCODER] = DescriptorSignalTranscoder::MINIMUM_LENGTH;
    table[DESCRIPTOR_CLOCK_DOMAIN] = DescriptorClockDomain::LENGTH;
    table[DESCRIPTOR_CONTROL_BLOCK] = DescriptorControlBlock::MINIMUM_LENGTH;
    table[DESCRIPTOR_TIMING] = DescriptorTiming::LENGTH;
    table[DESCRIPTOR_PTP_INSTANCE] = DescriptorPtpInstance::LENGTH;
    table[DESCRIPTOR_PTP_PORT] = DescriptorPtpPort::LENGTH;
    return table;
}();

/// Parse a descriptor from raw bytes into a ParsedDescriptor struct
/// @param desc_data Raw descriptor bytes (at least 4 bytes for type and index)
[[nodiscard]] auto parse_descriptor(std::span<uint8_t const> desc_data) -> StatusValue<ParsedDescriptor const*>;

//
// Individual descriptor format_to() overloads have moved to
// atdecc_aem_descriptor_format.hpp. Only the parsed-descriptor dispatcher
// remains here since it depends on ParsedDescriptor.
//

/// Format a parsed descriptor to an output iterator
/// @param out Output iterator to write formatted text to
/// @param desc Parsed descriptor union to format
/// @param data_size Total size of the raw descriptor data
template <typename OutputIt>
auto format_descriptor(OutputIt out, ParsedDescriptor const& desc, size_t const data_size) -> OutputIt
{
    if (data_size < 4) {
        out = std::format_to(out, "        descriptor too short\n");
        return out;
    }

    uint16_t const desc_type = desc.data.common.descriptor_type.get();

    switch (desc_type) {
        case DESCRIPTOR_ENTITY:
            return format_to(out, desc.data.entity);
        case DESCRIPTOR_CONFIGURATION:
            return format_to(out, desc.data.configuration);
        case DESCRIPTOR_AUDIO_UNIT:
            return format_to(out, desc.data.audio_unit);
        case DESCRIPTOR_VIDEO_UNIT:
            return format_to(out, desc.data.video_unit);
        case DESCRIPTOR_SENSOR_UNIT:
            return format_to(out, desc.data.sensor_unit);
        case DESCRIPTOR_STREAM_INPUT:
            return format_stream_descriptor(out, desc.data.stream, "STREAM_INPUT");
        case DESCRIPTOR_STREAM_OUTPUT:
            return format_stream_descriptor(out, desc.data.stream, "STREAM_OUTPUT");
        case DESCRIPTOR_JACK_INPUT:
            return format_jack_descriptor(out, desc.data.jack, "JACK_INPUT");
        case DESCRIPTOR_JACK_OUTPUT:
            return format_jack_descriptor(out, desc.data.jack, "JACK_OUTPUT");
        case DESCRIPTOR_AVB_INTERFACE:
            return format_to(out, desc.data.avb_interface);
        case DESCRIPTOR_CLOCK_SOURCE:
            return format_to(out, desc.data.clock_source);
        case DESCRIPTOR_MEMORY_OBJECT:
            return format_to(out, desc.data.memory_object);
        case DESCRIPTOR_LOCALE:
            return format_to(out, desc.data.locale);
        case DESCRIPTOR_STRINGS:
            return format_to(out, desc.data.strings);
        case DESCRIPTOR_STREAM_PORT_INPUT:
            return format_stream_port_descriptor(out, desc.data.stream_port, "STREAM_PORT_INPUT");
        case DESCRIPTOR_STREAM_PORT_OUTPUT:
            return format_stream_port_descriptor(out, desc.data.stream_port, "STREAM_PORT_OUTPUT");
        case DESCRIPTOR_EXTERNAL_PORT_INPUT:
        case DESCRIPTOR_EXTERNAL_PORT_OUTPUT:
            return format_to(out, desc.data.external_port);
        case DESCRIPTOR_INTERNAL_PORT_INPUT:
        case DESCRIPTOR_INTERNAL_PORT_OUTPUT:
            return format_to(out, desc.data.internal_port);
        case DESCRIPTOR_AUDIO_CLUSTER:
            return format_to(out, desc.data.audio_cluster);
        case DESCRIPTOR_VIDEO_CLUSTER:
            return format_to(out, desc.data.video_cluster);
        case DESCRIPTOR_SENSOR_CLUSTER:
            return format_to(out, desc.data.sensor_cluster);
        case DESCRIPTOR_AUDIO_MAP:
            return format_to(out, desc.data.audio_map);
        case DESCRIPTOR_VIDEO_MAP:
            return format_to(out, desc.data.video_map);
        case DESCRIPTOR_SENSOR_MAP:
            return format_to(out, desc.data.sensor_map);
        case DESCRIPTOR_CONTROL:
            return format_to(out, desc.data.control);
        case DESCRIPTOR_SIGNAL_SELECTOR:
            return format_to(out, desc.data.signal_selector);
        case DESCRIPTOR_MIXER:
            return format_to(out, desc.data.mixer);
        case DESCRIPTOR_MATRIX:
            return format_to(out, desc.data.matrix);
        case DESCRIPTOR_MATRIX_SIGNAL:
            return format_to(out, desc.data.matrix_signal);
        case DESCRIPTOR_SIGNAL_SPLITTER:
            return format_to(out, desc.data.signal_splitter);
        case DESCRIPTOR_SIGNAL_COMBINER:
            return format_to(out, desc.data.signal_combiner);
        case DESCRIPTOR_SIGNAL_DEMULTIPLEXER:
            return format_to(out, desc.data.signal_demultiplexer);
        case DESCRIPTOR_SIGNAL_MULTIPLEXER:
            return format_to(out, desc.data.signal_multiplexer);
        case DESCRIPTOR_SIGNAL_TRANSCODER:
            return format_to(out, desc.data.signal_transcoder, data_size);
        case DESCRIPTOR_CLOCK_DOMAIN:
            return format_to(out, desc.data.clock_domain);
        case DESCRIPTOR_CONTROL_BLOCK:
            return format_to(out, desc.data.control_block, data_size);
        case DESCRIPTOR_TIMING:
            return format_to(out, desc.data.timing);
        case DESCRIPTOR_PTP_INSTANCE:
            return format_to(out, desc.data.ptp_instance);
        case DESCRIPTOR_PTP_PORT:
            return format_to(out, desc.data.ptp_port);
        default:
            // Unknown descriptor type: print the type/index and hex dump the raw bytes
            out = std::format_to(
                out,
                "        {} Descriptor [{}]: ({} bytes):\n          ",
                descriptor_type_name(desc_type),
                desc.data.common.descriptor_index.get(),
                data_size);
            return format_hex_dump(out, std::span<uint8_t const>(desc.data.raw.data(), data_size));
    }
}

/// Parse and format a descriptor (convenience function that combines parse and format)
/// @param out Output iterator to write formatted text to
/// @param desc_data Raw descriptor bytes to parse and format
template <typename OutputIt>
auto format_descriptor(OutputIt out, std::span<uint8_t const> desc_data) -> OutputIt
{
    auto const parsed = parse_descriptor(desc_data);
    if (parsed) {
        return format_descriptor(out, *parsed.value(), desc_data.size());
    }
    return std::format_to(out, "        Failed to parse descriptor.\n");
}

//
// AEM Payload Parsing and Formatting
//
/// Number of AEM commands in the lookup table (0x0000 - 0x004D + 1)
constexpr size_t NUM_AEM_COMMANDS = 0x4E;

/// Parsed AEM payload result - contains a union of all known AEM payload structs
struct ParsedAemPayload
{
    union PayloadData
    {
        AemAcquireEntityPayload acquire_entity;
        AemLockEntityPayload lock_entity;
        AemReadDescriptorCommandPayload read_descriptor_command;
        AemReadDescriptorResponsePayload read_descriptor_response;
        AemSetConfigurationPayload set_configuration;
        AemStreamFormatPayload stream_format;
        AemGetStreamFormatCommandPayload get_stream_format_command;
        AemGetStreamInfoCommandPayload get_stream_info_command;
        AemStreamInfoPayload stream_info;
        AemNameCommandPayload name_command;
        AemNamePayload name;
        AemSamplingRatePayload sampling_rate;
        AemGetSamplingRateCommandPayload get_sampling_rate_command;
        AemClockSourcePayload clock_source;
        AemGetClockSourceCommandPayload get_clock_source_command;
        AemStreamingPayload streaming;
        AemIdentifyPayload identify;
        AemGetAvbInfoCommandPayload get_avb_info_command;
        AemAvbInfoPayload avb_info;
        AemGetCountersCommandPayload get_counters_command;
        AemCountersPayload counters;
        AemRebootPayload reboot;
        AemControlPayloadHeader control_header;
        AemSignalSelectorPayload signal_selector;
        AemGetSignalSelectorCommandPayload get_signal_selector_command;
        AemGetAudioMapCommandPayload get_audio_map_command;
        AemAudioMapResponseHeader audio_map_response;
        AemAudioMappingsCommandHeader audio_mappings_command;
        AemStartOperationCommandPayload start_operation_command;
        AemStartOperationResponsePayload start_operation_response;
        AemAbortOperationPayload abort_operation;
        AemOperationStatusPayload operation_status;
        AemSetMaxTransitTimePayload set_max_transit_time;
        AemGetMaxTransitTimeCommandPayload get_max_transit_time_command;
        std::array<uint8_t, 512> raw;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init) - memset initializes union
        PayloadData() { std::memset(this, 0, sizeof(*this)); }
    } data;
};

/// Minimum length info for AEM commands
/// For commands with different command/response lengths, we store both
struct AemMinLengths
{
    size_t command;   // Minimum length for command
    size_t response;  // Minimum length for response (0 means same as command)
};

}  // namespace statusbar::atdecc::aem

namespace statusbar::atdecc {

/// Minimum length lookup table for AEM commands (indexed by command code)
constexpr std::array<aem::AemMinLengths, aem::NUM_AEM_COMMANDS> aem_command_min_lengths =
    []() -> std::array<aem::AemMinLengths, aem::NUM_AEM_COMMANDS> {
    using namespace atdecc::aem;
    std::array<AemMinLengths, NUM_AEM_COMMANDS> table{};

    table[AEM_COMMAND_ACQUIRE_ENTITY] = {.command = AemAcquireEntityPayload::LENGTH, .response = 0};
    table[AEM_COMMAND_LOCK_ENTITY] = {.command = AemLockEntityPayload::LENGTH, .response = 0};
    // ENTITY_AVAILABLE (0x0002) - no payload
    // CONTROLLER_AVAILABLE (0x0003) - no payload
    table[AEM_COMMAND_READ_DESCRIPTOR] = {
        .command = AemReadDescriptorCommandPayload::LENGTH, .response = AemReadDescriptorResponsePayload::LENGTH};
    // WRITE_DESCRIPTOR (0x0005) - not implemented
    table[AEM_COMMAND_SET_CONFIGURATION] = {.command = AemSetConfigurationPayload::LENGTH, .response = 0};
    table[AEM_COMMAND_GET_CONFIGURATION] = {.command = 0, .response = AemSetConfigurationPayload::LENGTH};
    table[AEM_COMMAND_SET_STREAM_FORMAT] = {.command = AemStreamFormatPayload::LENGTH, .response = 0};
    table[AEM_COMMAND_GET_STREAM_FORMAT] = {
        .command = AemGetStreamFormatCommandPayload::LENGTH, .response = AemStreamFormatPayload::LENGTH};
    // SET_VIDEO_FORMAT (0x000A) - not implemented
    // GET_VIDEO_FORMAT (0x000B) - not implemented
    // SET_SENSOR_FORMAT (0x000C) - not implemented
    // GET_SENSOR_FORMAT (0x000D) - not implemented
    table[AEM_COMMAND_SET_STREAM_INFO] = {.command = AemStreamInfoPayload::LENGTH, .response = 0};
    table[AEM_COMMAND_GET_STREAM_INFO] = {
        .command = AemGetStreamInfoCommandPayload::LENGTH, .response = AemStreamInfoPayload::LENGTH};
    table[AEM_COMMAND_SET_NAME] = {.command = AemNamePayload::LENGTH, .response = 0};
    table[AEM_COMMAND_GET_NAME] = {.command = AemNameCommandPayload::LENGTH, .response = AemNamePayload::LENGTH};
    // SET_ASSOCIATION_ID (0x0012) - not implemented
    // GET_ASSOCIATION_ID (0x0013) - not implemented
    table[AEM_COMMAND_SET_SAMPLING_RATE] = {.command = AemSamplingRatePayload::LENGTH, .response = 0};
    table[AEM_COMMAND_GET_SAMPLING_RATE] = {
        .command = AemGetSamplingRateCommandPayload::LENGTH, .response = AemSamplingRatePayload::LENGTH};
    table[AEM_COMMAND_SET_CLOCK_SOURCE] = {.command = AemClockSourcePayload::LENGTH, .response = 0};
    table[AEM_COMMAND_GET_CLOCK_SOURCE] = {
        .command = AemGetClockSourceCommandPayload::LENGTH, .response = AemClockSourcePayload::LENGTH};
    table[AEM_COMMAND_SET_CONTROL] = {.command = AemControlPayloadHeader::LENGTH, .response = 0};
    table[AEM_COMMAND_GET_CONTROL] = {.command = AemControlPayloadHeader::LENGTH, .response = 0};
    table[AEM_COMMAND_INCREMENT_CONTROL] = {.command = AemControlPayloadHeader::LENGTH, .response = 0};
    table[AEM_COMMAND_DECREMENT_CONTROL] = {.command = AemControlPayloadHeader::LENGTH, .response = 0};
    table[AEM_COMMAND_SET_SIGNAL_SELECTOR] = {.command = AemSignalSelectorPayload::LENGTH, .response = 0};
    table[AEM_COMMAND_GET_SIGNAL_SELECTOR] = {
        .command = AemGetSignalSelectorCommandPayload::LENGTH, .response = AemSignalSelectorPayload::LENGTH};
    // SET_MIXER (0x001E) - not implemented
    // GET_MIXER (0x001F) - not implemented
    // SET_MATRIX (0x0020) - not implemented
    // GET_MATRIX (0x0021) - not implemented
    table[AEM_COMMAND_START_STREAMING] = {.command = AemStreamingPayload::LENGTH, .response = 0};
    table[AEM_COMMAND_STOP_STREAMING] = {.command = AemStreamingPayload::LENGTH, .response = 0};
    // REGISTER_UNSOLICITED_NOTIFICATION (0x0024) - no payload
    // DEREGISTER_UNSOLICITED_NOTIFICATION (0x0025) - no payload
    table[AEM_COMMAND_IDENTIFY_NOTIFICATION] = {.command = AemIdentifyPayload::LENGTH, .response = 0};
    table[AEM_COMMAND_GET_AVB_INFO] = {.command = AemGetAvbInfoCommandPayload::LENGTH, .response = AemAvbInfoPayload::LENGTH};
    // GET_AS_PATH (0x0028) - not implemented
    table[AEM_COMMAND_GET_COUNTERS] = {.command = AemGetCountersCommandPayload::LENGTH, .response = AemCountersPayload::LENGTH};
    table[AEM_COMMAND_REBOOT] = {.command = AemRebootPayload::LENGTH, .response = 0};
    table[AEM_COMMAND_GET_AUDIO_MAP] = {
        .command = AemGetAudioMapCommandPayload::LENGTH, .response = AemAudioMapResponseHeader::LENGTH};
    table[AEM_COMMAND_ADD_AUDIO_MAPPINGS] = {.command = AemAudioMappingsCommandHeader::LENGTH, .response = 0};
    table[AEM_COMMAND_REMOVE_AUDIO_MAPPINGS] = {.command = AemAudioMappingsCommandHeader::LENGTH, .response = 0};
    // GET_VIDEO_MAP (0x002E) - not implemented
    // ADD_VIDEO_MAPPINGS (0x002F) - not implemented
    // REMOVE_VIDEO_MAPPINGS (0x0030) - not implemented
    // GET_SENSOR_MAP (0x0031) - not implemented
    // ADD_SENSOR_MAPPINGS (0x0032) - not implemented
    // REMOVE_SENSOR_MAPPINGS (0x0033) - not implemented
    table[AEM_COMMAND_START_OPERATION] = {
        .command = AemStartOperationCommandPayload::LENGTH, .response = AemStartOperationResponsePayload::LENGTH};
    table[AEM_COMMAND_ABORT_OPERATION] = {.command = AemAbortOperationPayload::LENGTH, .response = 0};
    table[AEM_COMMAND_OPERATION_STATUS] = {.command = 0, .response = AemOperationStatusPayload::LENGTH};
    // Auth commands (0x0037-0x0046) - not implemented
    // SET_MEMORY_OBJECT_LENGTH (0x0047) - not implemented
    // GET_MEMORY_OBJECT_LENGTH (0x0048) - not implemented
    // SET_STREAM_BACKUP (0x0049) - not implemented
    // GET_STREAM_BACKUP (0x004A) - not implemented
    // 0x004B - reserved
    table[AEM_COMMAND_SET_MAX_TRANSIT_TIME] = {.command = AemSetMaxTransitTimePayload::LENGTH, .response = 0};
    table[AEM_COMMAND_GET_MAX_TRANSIT_TIME] = {
        .command = AemGetMaxTransitTimeCommandPayload::LENGTH, .response = AemSetMaxTransitTimePayload::LENGTH};

    return table;
}();

/// Parse an AEM payload from raw bytes into a ParsedAemPayload struct
/// @param cmd AEM command code
/// @param is_response True if parsing a response, false for command
/// @param payload Raw payload bytes after the AEM header
[[nodiscard]] auto parse_aem(uint16_t cmd, bool is_response, std::span<uint8_t const> payload)
    -> StatusValue<aem::ParsedAemPayload const*>;

}  // namespace statusbar::atdecc

namespace statusbar::atdecc::aem {

//
// Individual AEM format functions
//
/// Helper to format descriptor type/index
/// @param out Output iterator to write formatted text to
/// @param desc_type AEM descriptor type code
/// @param desc_index AEM descriptor index
template <typename OutputIt>
auto format_aem_descriptor(OutputIt out, uint16_t const desc_type, uint16_t const desc_index) -> OutputIt
{
    return std::format_to(
        out, "        descriptor: {} ({:#06x}) index={}\n", descriptor_type_name(desc_type), desc_type, desc_index);
}

/// @param out Output iterator to write formatted text to
/// @param p Acquire entity payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemAcquireEntityPayload const& p) -> OutputIt
{
    out = std::format_to(out, "        flags: {}{}\n", p.is_persistent() ? "PERSISTENT " : "", p.is_release() ? "RELEASE" : "");
    out = std::format_to(out, "        owner: ");
    out = ieee::format_to(out, p.owner_entity_id);
    out = std::format_to(out, "\n");
    return format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
}

/// @param out Output iterator to write formatted text to
/// @param p Lock entity payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemLockEntityPayload const& p) -> OutputIt
{
    out = std::format_to(out, "        flags: {}\n", p.is_unlock() ? "UNLOCK" : "LOCK");
    out = std::format_to(out, "        locked_entity: ");
    out = ieee::format_to(out, p.locked_entity_id);
    out = std::format_to(out, "\n");
    return format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
}

/// @param out Output iterator to write formatted text to
/// @param p Read descriptor command payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemReadDescriptorCommandPayload const& p) -> OutputIt
{
    out = std::format_to(out, "        config_index={}\n", p.configuration_index.get());
    return format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
}

/// @param out Output iterator to write formatted text to
/// @param p Read descriptor response header
/// @param payload Raw payload bytes containing the descriptor data
template <typename OutputIt>
auto format_read_descriptor_response(OutputIt out, AemReadDescriptorResponsePayload const& p, std::span<uint8_t const> payload)
    -> OutputIt
{
    out = std::format_to(out, "        config_index={}\n", p.configuration_index.get());
    auto desc_data = payload.subspan(AemReadDescriptorResponsePayload::LENGTH);
    if (!desc_data.empty()) {
        out = format_descriptor(out, desc_data);
    }
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param p Set configuration payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemSetConfigurationPayload const& p) -> OutputIt
{
    return std::format_to(out, "        config_index={}\n", p.configuration_index.get());
}

/// @param out Output iterator to write formatted text to
/// @param p Stream format payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemStreamFormatPayload const& p) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    out = std::format_to(out, "        stream_format: ");
    for (auto const& b : p.stream_format) {
        out = std::format_to(out, "{:02x} ", static_cast<uint8_t>(b));
    }
    return std::format_to(out, "\n");
}

/// @param out Output iterator to write formatted text to
/// @param p Get stream format command payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemGetStreamFormatCommandPayload const& p) -> OutputIt
{
    return format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
}

/// @param out Output iterator to write formatted text to
/// @param p Stream info payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemStreamInfoPayload const& p) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    out = std::format_to(out, "        flags: {:#010x}", p.flags.get());
    if (p.is_connected()) {
        out = std::format_to(out, " CONNECTED");
    }
    if (p.is_class_b()) {
        out = std::format_to(out, " CLASS_B");
    }
    out = std::format_to(out, "\n");
    if (p.is_stream_id_valid()) {
        out = std::format_to(out, "        stream_id: ");
        out = ieee::format_to(out, p.stream_id);
        out = std::format_to(out, "\n");
    }
    if (p.is_stream_format_valid()) {
        out = std::format_to(out, "        stream_format: ");
        for (auto const& b : p.stream_format) {
            out = std::format_to(out, "{:02x} ", static_cast<uint8_t>(b));
        }
        out = std::format_to(out, "\n");
    }
    out = std::format_to(out, "        vlan_id={} msrp_latency={}\n", p.stream_vlan_id.get(), p.msrp_accumulated_latency.get());
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param p Get stream info command payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemGetStreamInfoCommandPayload const& p) -> OutputIt
{
    return format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
}

/// @param out Output iterator to write formatted text to
/// @param p Name payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemNamePayload const& p) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    out = std::format_to(out, "        name_index={} config_index={}\n", p.name_index.get(), p.configuration_index.get());
    std::string_view const name(p.name.data(), strnlen(p.name.data(), p.name.size()));
    return std::format_to(out, "        name: \"{}\"\n", name);
}

/// @param out Output iterator to write formatted text to
/// @param p Name command payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemNameCommandPayload const& p) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    return std::format_to(out, "        name_index={} config_index={}\n", p.name_index.get(), p.configuration_index.get());
}

/// @param out Output iterator to write formatted text to
/// @param p Sampling rate payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemSamplingRatePayload const& p) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    return std::format_to(out, "        sampling_rate={}\n", p.sampling_rate.get());
}

/// @param out Output iterator to write formatted text to
/// @param p Get sampling rate command payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemGetSamplingRateCommandPayload const& p) -> OutputIt
{
    return format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
}

/// @param out Output iterator to write formatted text to
/// @param p Clock source payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemClockSourcePayload const& p) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    return std::format_to(out, "        clock_source_index={}\n", p.clock_source_index.get());
}

/// @param out Output iterator to write formatted text to
/// @param p Get clock source command payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemGetClockSourceCommandPayload const& p) -> OutputIt
{
    return format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
}

/// @param out Output iterator to write formatted text to
/// @param p Streaming payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemStreamingPayload const& p) -> OutputIt
{
    return format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
}

/// @param out Output iterator to write formatted text to
/// @param p Identify payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemIdentifyPayload const& p) -> OutputIt
{
    return format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
}

/// @param out Output iterator to write formatted text to
/// @param p AVB info payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemAvbInfoPayload const& p) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    out = std::format_to(out, "        gptp_grandmaster: ");
    out = tsn::format_to(out, p.gptp_grandmaster_id);
    out = std::format_to(
        out, "\n        propagation_delay={} gptp_domain={}\n", p.propagation_delay.get(), p.gptp_domain_number.get());
    out = std::format_to(out, "        flags:");
    if (p.is_as_capable()) {
        out = std::format_to(out, " AS_CAPABLE");
    }
    if (p.is_gptp_enabled()) {
        out = std::format_to(out, " GPTP_ENABLED");
    }
    if (p.is_srp_enabled()) {
        out = std::format_to(out, " SRP_ENABLED");
    }
    if (p.is_avtp_down_valid()) {
        out = std::format_to(out, " AVTP_DOWN_VALID");
        if (p.is_avtp_down()) {
            out = std::format_to(out, " AVTP_DOWN");
        }
    }
    out = std::format_to(out, "\n");
    return std::format_to(out, "        msrp_mappings_count={}\n", p.msrp_mappings_count.get());
}

/// @param out Output iterator to write formatted text to
/// @param p Get AVB info command payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemGetAvbInfoCommandPayload const& p) -> OutputIt
{
    return format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
}

/// @param out Output iterator to write formatted text to
/// @param p Counters payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemCountersPayload const& p) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    out = std::format_to(out, "        counters_valid={:#010x}\n", p.counters_valid.get());
    uint32_t const valid = p.counters_valid.get();
    for (size_t i = 0; i < 32; ++i) {
        if (valid & (1U << i)) {
            out = std::format_to(out, "        counter[{}]={}\n", i, static_cast<uint32_t>(p.counters[i]));
        }
    }
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param p Get counters command payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemGetCountersCommandPayload const& p) -> OutputIt
{
    return format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
}

/// @param out Output iterator to write formatted text to
/// @param p Reboot payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemRebootPayload const& p) -> OutputIt
{
    return format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
}

/// @param out Output iterator to write formatted text to
/// @param p Control payload header
/// @param payload Raw payload bytes containing control values
template <typename OutputIt>
auto format_control(OutputIt out, AemControlPayloadHeader const& p, std::span<uint8_t const> payload) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    auto control_data = payload.subspan(AemControlPayloadHeader::LENGTH);
    if (!control_data.empty()) {
        out = std::format_to(out, "        control_values ({} bytes): ", control_data.size());
        out = format_hex_dump(out, control_data);
    }
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param p Signal selector payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemSignalSelectorPayload const& p) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    return std::format_to(
        out, "        signal: type={:#06x} index={} output={}\n", p.signal_type.get(), p.signal_index.get(), p.signal_output.get());
}

/// @param out Output iterator to write formatted text to
/// @param p Get signal selector command payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemGetSignalSelectorCommandPayload const& p) -> OutputIt
{
    return format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
}

/// @param out Output iterator to write formatted text to
/// @param p Get audio map command payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemGetAudioMapCommandPayload const& p) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    return std::format_to(out, "        map_index={}\n", p.map_index.get());
}

/// @param out Output iterator to write formatted text to
/// @param p Audio map response header to format
template <typename OutputIt>
auto format_to(OutputIt out, AemAudioMapResponseHeader const& p) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    return std::format_to(
        out,
        "        map_index={} num_mappings={} num_maps={}\n",
        p.map_index.get(),
        p.number_of_mappings.get(),
        p.number_of_maps.get());
}

/// @param out Output iterator to write formatted text to
/// @param p Audio mappings command header to format
template <typename OutputIt>
auto format_to(OutputIt out, AemAudioMappingsCommandHeader const& p) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    return std::format_to(out, "        num_mappings={}\n", p.number_of_mappings.get());
}

/// @param out Output iterator to write formatted text to
/// @param p Start operation command payload header
/// @param payload Raw payload bytes containing operation-specific data
template <typename OutputIt>
auto format_start_operation(OutputIt out, AemStartOperationCommandPayload const& p, std::span<uint8_t const> payload) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    uint16_t const op_type = p.operation_type.get();
    char const* op_name = operation_type::name(op_type);
    out = std::format_to(out, "        operation_id={} operation_type={} ({:#06x})\n", p.operation_id.get(), op_name, op_type);
    auto op_data = payload.subspan(AemStartOperationCommandPayload::LENGTH);
    if (!op_data.empty()) {
        out = std::format_to(out, "        operation_data ({} bytes): ", op_data.size());
        out = format_hex_dump(out, op_data);
    }
    return out;
}

/// @param out Output iterator to write formatted text to
/// @param p Abort operation payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemAbortOperationPayload const& p) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    return std::format_to(out, "        operation_id={}\n", p.operation_id.get());
}

/// @param out Output iterator to write formatted text to
/// @param p Operation status payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemOperationStatusPayload const& p) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    uint16_t const pct = p.percent_complete.get();
    return std::format_to(out, "        operation_id={} percent_complete={}.{:02d}%\n", p.operation_id.get(), pct / 100, pct % 100);
}

/// @param out Output iterator to write formatted text to
/// @param p SET_MAX_TRANSIT_TIME payload to format
template <typename OutputIt>
auto format_aem_max_transit_time(OutputIt out, AemSetMaxTransitTimePayload const& p) -> OutputIt
{
    out = format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
    return std::format_to(out, "        max_transit_time={} ns\n", p.max_transit_time.get());
}

/// @param out Output iterator to write formatted text to
/// @param p GET_MAX_TRANSIT_TIME command payload to format
template <typename OutputIt>
auto format_aem_get_max_transit_time_command(OutputIt out, AemGetMaxTransitTimeCommandPayload const& p) -> OutputIt
{
    return format_aem_descriptor(out, p.descriptor_type.get(), static_cast<uint16_t>(p.descriptor_index));
}

/// Format a parsed AEM payload to an output iterator
/// @param out Output iterator to write formatted text to
/// @param cmd AEM command code
/// @param is_response True if formatting a response, false for command
/// @param parsed Parsed AEM payload union
/// @param payload Raw payload bytes after the AEM header
template <typename OutputIt>
auto format_aem(
    OutputIt out, uint16_t const cmd, bool const is_response, ParsedAemPayload const& parsed, std::span<uint8_t const> payload)
    -> OutputIt
{
    using namespace atdecc;  // For AEM_COMMAND_* constants

    switch (cmd) {
        case AEM_COMMAND_ACQUIRE_ENTITY:
            return format_to(out, parsed.data.acquire_entity);

        case AEM_COMMAND_LOCK_ENTITY:
            return format_to(out, parsed.data.lock_entity);

        case AEM_COMMAND_ENTITY_AVAILABLE:
        case AEM_COMMAND_CONTROLLER_AVAILABLE:
        case AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION:
        case AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION:
            // No payload
            return out;

        case AEM_COMMAND_READ_DESCRIPTOR:
            if (is_response) {
                return format_read_descriptor_response(out, parsed.data.read_descriptor_response, payload);
            } else {
                return format_to(out, parsed.data.read_descriptor_command);
            }

        case AEM_COMMAND_SET_CONFIGURATION:
            return format_to(out, parsed.data.set_configuration);

        case AEM_COMMAND_GET_CONFIGURATION:
            // The GET_CONFIGURATION command carries no configuration_index (it is
            // reserved-only, min_length 0); only the response has the field. Reading
            // set_configuration for the command direction would over-read a
            // spec-compliant (2-byte) command payload -- parse_aem does not validate
            // a length of 0. Only format the struct for the response.
            if (!is_response) {
                return out;
            }
            return format_to(out, parsed.data.set_configuration);

        case AEM_COMMAND_SET_STREAM_FORMAT:
            return format_to(out, parsed.data.stream_format);

        case AEM_COMMAND_GET_STREAM_FORMAT:
            if (is_response) {
                return format_to(out, parsed.data.stream_format);
            } else {
                return format_to(out, parsed.data.get_stream_format_command);
            }

        case AEM_COMMAND_SET_STREAM_INFO:
            return format_to(out, parsed.data.stream_info);

        case AEM_COMMAND_GET_STREAM_INFO:
            if (is_response) {
                return format_to(out, parsed.data.stream_info);
            } else {
                return format_to(out, parsed.data.get_stream_info_command);
            }

        case AEM_COMMAND_SET_NAME:
            return format_to(out, parsed.data.name);

        case AEM_COMMAND_GET_NAME:
            if (is_response) {
                return format_to(out, parsed.data.name);
            } else {
                return format_to(out, parsed.data.name_command);
            }

        case AEM_COMMAND_SET_SAMPLING_RATE:
            return format_to(out, parsed.data.sampling_rate);

        case AEM_COMMAND_GET_SAMPLING_RATE:
            if (is_response) {
                return format_to(out, parsed.data.sampling_rate);
            } else {
                return format_to(out, parsed.data.get_sampling_rate_command);
            }

        case AEM_COMMAND_SET_CLOCK_SOURCE:
            return format_to(out, parsed.data.clock_source);

        case AEM_COMMAND_GET_CLOCK_SOURCE:
            if (is_response) {
                return format_to(out, parsed.data.clock_source);
            } else {
                return format_to(out, parsed.data.get_clock_source_command);
            }

        case AEM_COMMAND_SET_CONTROL:
        case AEM_COMMAND_GET_CONTROL:
        case AEM_COMMAND_INCREMENT_CONTROL:
        case AEM_COMMAND_DECREMENT_CONTROL:
            return format_control(out, parsed.data.control_header, payload);

        case AEM_COMMAND_SET_SIGNAL_SELECTOR:
            return format_to(out, parsed.data.signal_selector);

        case AEM_COMMAND_GET_SIGNAL_SELECTOR:
            if (is_response) {
                return format_to(out, parsed.data.signal_selector);
            } else {
                return format_to(out, parsed.data.get_signal_selector_command);
            }

        case AEM_COMMAND_START_STREAMING:
        case AEM_COMMAND_STOP_STREAMING:
            return format_to(out, parsed.data.streaming);

        case AEM_COMMAND_IDENTIFY_NOTIFICATION:
            return format_to(out, parsed.data.identify);

        case AEM_COMMAND_GET_AVB_INFO:
            if (is_response) {
                return format_to(out, parsed.data.avb_info);
            } else {
                return format_to(out, parsed.data.get_avb_info_command);
            }

        case AEM_COMMAND_GET_COUNTERS:
            if (is_response) {
                return format_to(out, parsed.data.counters);
            } else {
                return format_to(out, parsed.data.get_counters_command);
            }

        case AEM_COMMAND_REBOOT:
            return format_to(out, parsed.data.reboot);

        case AEM_COMMAND_GET_AUDIO_MAP:
            if (is_response) {
                return format_to(out, parsed.data.audio_map_response);
            } else {
                return format_to(out, parsed.data.get_audio_map_command);
            }

        case AEM_COMMAND_ADD_AUDIO_MAPPINGS:
        case AEM_COMMAND_REMOVE_AUDIO_MAPPINGS:
            return format_to(out, parsed.data.audio_mappings_command);

        case AEM_COMMAND_START_OPERATION:
            return format_start_operation(out, parsed.data.start_operation_command, payload);

        case AEM_COMMAND_ABORT_OPERATION:
            return format_to(out, parsed.data.abort_operation);

        case AEM_COMMAND_OPERATION_STATUS:
            // OPERATION_STATUS is an unsolicited response; its command direction
            // (min_length 0) carries no operation_status payload, so reading the
            // 8-byte struct there would over-read a short command payload.
            if (!is_response) {
                return out;
            }
            return format_to(out, parsed.data.operation_status);

        case AEM_COMMAND_SET_MAX_TRANSIT_TIME:
            return format_aem_max_transit_time(out, parsed.data.set_max_transit_time);

        case AEM_COMMAND_GET_MAX_TRANSIT_TIME:
            if (is_response) {
                return format_aem_max_transit_time(out, parsed.data.set_max_transit_time);
            } else {
                return format_aem_get_max_transit_time_command(out, parsed.data.get_max_transit_time_command);
            }

        default:
            // Unknown command - just print hex dump if there's payload data
            if (!payload.empty()) {
                out = std::format_to(out, "        payload ({} bytes): ", payload.size());
                out = format_hex_dump(out, payload);
            }
            return out;
    }
}

}  // namespace statusbar::atdecc::aem

namespace statusbar::atdecc {

/// Parse and format an AEM payload (convenience function that combines parse and format)
/// @param out Output iterator to write formatted text to
/// @param aem AEM PDU header with command/response info
/// @param payload Raw payload bytes after the AEM header
template <typename OutputIt>
auto format_aem_payload(OutputIt out, AemDu const& aem, std::span<uint8_t const> payload) -> OutputIt
{
    uint16_t const cmd = aem.command_code();
    bool const is_response = aem.is_response();

    auto const parsed = parse_aem(cmd, is_response, payload);
    if (parsed) {
        out = aem::format_aem(out, cmd, is_response, *parsed.value(), payload);
    }
    // If parsing fails (insufficient data), we silently skip formatting
    // The command header has already been formatted by the caller
    return out;
}

}  // namespace statusbar::atdecc
