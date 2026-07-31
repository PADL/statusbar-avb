// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for ATDECC AEM descriptor and command print/format functions

#include "statusbar/atdecc/atdecc_aem_format.hpp"
#include "statusbar/atdecc/atdecc_format.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>
#include <string>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::ieee;

//
// Entity descriptor formatting
//

TEST(aem_print, format_entity_descriptor)
{
    DescriptorEntity entity{};
    entity.descriptor_type = DESCRIPTOR_ENTITY;
    entity.descriptor_index = 0;
    entity.entity_id = Eui64(0x00, 0x1b, 0x21, 0xff, 0xfe, 0x00, 0x01, 0x02);
    entity.entity_model_id = Eui64(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);
    entity.entity_capabilities = entity_capabilities::AEM_SUPPORTED;
    entity.talker_stream_sources = 2;
    entity.listener_stream_sinks = 4;
    entity.configurations_count = 1;
    entity.current_configuration = 0;

    std::string result;
    format_to(std::back_inserter(result), entity);
    EXPECT_TRUE(result.find("ENTITY Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("entity_id:") != std::string::npos);
    EXPECT_TRUE(result.find("entity_model_id:") != std::string::npos);
    EXPECT_TRUE(result.find("configurations:") != std::string::npos);
}

//
// Audio unit descriptor formatting
//

TEST(aem_print, format_audio_descriptor)
{
    DescriptorAudioUnit audio{};
    audio.descriptor_type = DESCRIPTOR_AUDIO_UNIT;
    audio.descriptor_index = 0;
    audio.clock_domain_index = 0;
    audio.current_sampling_rate = 48000;
    audio.number_of_stream_input_ports = 1;
    audio.base_stream_input_port = 0;
    audio.number_of_stream_output_ports = 1;
    audio.base_stream_output_port = 0;

    std::string result;
    format_to(std::back_inserter(result), audio);
    EXPECT_TRUE(result.find("AUDIO_UNIT Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("current_sampling_rate:") != std::string::npos);
    EXPECT_TRUE(result.find("stream_input_ports:") != std::string::npos);
    EXPECT_TRUE(result.find("stream_output_ports:") != std::string::npos);
}

//
// Unknown descriptor type handling via format_descriptor
//

TEST(aem_print, format_unknown_descriptor)
{
    // Use a descriptor type value that is not in the known switch-case
    // DESCRIPTOR_TIMING = 0x0026 is handled by format_descriptor as default hex dump path
    // Use a truly unknown type (e.g., 0x00FF)
    std::array<uint8_t, 16> raw{};
    raw[0] = 0x00;
    raw[1] = 0xFF;  // Unknown descriptor type
    raw[2] = 0x00;
    raw[3] = 0x00;  // descriptor_index = 0
    raw[4] = 0xDE;
    raw[5] = 0xAD;

    auto result_parsed = parse_descriptor(std::span<uint8_t const>(raw));
    EXPECT_TRUE(result_parsed.has_value());

    std::string result;
    format_descriptor(std::back_inserter(result), *result_parsed.value(), raw.size());
    // Unknown descriptor types get the type name and hex dump
    EXPECT_TRUE(result.find("Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("de") != std::string::npos || result.find("DE") != std::string::npos);
}

//
// Parse descriptor with insufficient data
//

TEST(aem_print, parse_descriptor_too_short)
{
    std::array<uint8_t, 2> tiny{};
    auto result = parse_descriptor(std::span<uint8_t const>(tiny));
    EXPECT_FALSE(result.has_value());
}

// Security regression: a crafted READ_DESCRIPTOR response whose fixed header is
// present but whose count field is huge must be REJECTED, not formatted — the
// count drives the variable trailer reads and would otherwise run past the
// buffer (OOB read / info leak). See descriptor_trailer_end in atdecc_aem_print.cpp.
TEST(aem_print, parse_descriptor_rejects_oversized_trailer_count)
{
    DescriptorConfiguration config{};
    config.descriptor_type = static_cast<uint16_t>(DESCRIPTOR_CONFIGURATION);
    config.descriptor_counts_count = 0xFFFF;  // attacker-controlled: claims 65535 entries
    static_assert(sizeof(config) >= DescriptorConfiguration::LENGTH);
    std::array<uint8_t, DescriptorConfiguration::LENGTH> buf{};  // fixed header ONLY, no trailer
    std::memcpy(buf.data(), &config, DescriptorConfiguration::LENGTH);
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_FALSE(result.has_value());
}

// Complement: a descriptor whose declared trailer count IS fully present must
// still parse — the hardening must not reject well-formed variable descriptors.
TEST(aem_print, parse_descriptor_accepts_present_trailer)
{
    DescriptorConfiguration config{};
    config.descriptor_type = static_cast<uint16_t>(DESCRIPTOR_CONFIGURATION);
    config.descriptor_counts_count = 2;
    constexpr size_t kFull = DescriptorConfiguration::LENGTH + (2 * sizeof(DescriptorCountEntry));
    std::array<uint8_t, kFull> buf{};  // header + 2 trailer entries
    std::memcpy(buf.data(), &config, DescriptorConfiguration::LENGTH);
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
}

//
// Entity descriptor parse and format roundtrip
//

TEST(aem_print, parse_and_format_entity_descriptor)
{
    // Create a properly sized buffer for an entity descriptor
    std::array<uint8_t, DescriptorEntity::LENGTH> buf{};
    buf[0] = 0x00;
    buf[1] = 0x00;  // DESCRIPTOR_ENTITY = 0x0000
    buf[2] = 0x00;
    buf[3] = 0x00;  // descriptor_index = 0

    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());

    std::string output;
    format_descriptor(std::back_inserter(output), std::span<uint8_t const>(buf));
    EXPECT_TRUE(output.find("ENTITY Descriptor") != std::string::npos);
}

//
// Configuration descriptor formatting
//

TEST(aem_print, format_configuration_descriptor)
{
    DescriptorConfiguration config{};
    config.descriptor_type = DESCRIPTOR_CONFIGURATION;
    config.descriptor_index = 0;
    config.descriptor_counts_count = 5;
    config.descriptor_counts_offset = 74;

    std::string result;
    format_to(std::back_inserter(result), config);
    EXPECT_TRUE(result.find("CONFIGURATION Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("descriptor_counts:") != std::string::npos);
}

//
// Capability flag string formatting (via entity capabilities in AdpDu)
//

TEST(aem_print, format_capabilities)
{
    // Test all entity capability flags produce output
    std::string result;

    // Single flag
    result.clear();
    format_entity_capabilities_to(std::back_inserter(result), entity_capabilities::EFU_MODE);
    EXPECT_TRUE(result == "EFU_MODE");

    // Multiple flags
    result.clear();
    format_entity_capabilities_to(
        std::back_inserter(result),
        entity_capabilities::AEM_SUPPORTED | entity_capabilities::CLASS_A_SUPPORTED | entity_capabilities::CLASS_B_SUPPORTED);
    EXPECT_TRUE(result.find("AEM") != std::string::npos);
    EXPECT_TRUE(result.find("CLASS_A") != std::string::npos);
    EXPECT_TRUE(result.find("CLASS_B") != std::string::npos);

    // All flags set
    result.clear();
    format_entity_capabilities_to(std::back_inserter(result), 0xFFFFFFFF);
    EXPECT_TRUE(result.find("EFU_MODE") != std::string::npos);
    EXPECT_TRUE(result.find("NOT_READY") != std::string::npos);
    EXPECT_TRUE(result.find("CTRL_IGNORE") != std::string::npos);
}

//
// Clock source descriptor formatting
//

TEST(aem_print, format_clock_source_descriptor)
{
    DescriptorClockSource cs{};
    cs.descriptor_type = DESCRIPTOR_CLOCK_SOURCE;
    cs.descriptor_index = 0;
    cs.clock_source_type = 0x0001;
    cs.clock_source_flags = 0x0000;

    std::string result;
    format_to(std::back_inserter(result), cs);
    EXPECT_TRUE(result.find("CLOCK_SOURCE Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("clock_source_type:") != std::string::npos);
}

//
// Clock domain descriptor formatting
//

TEST(aem_print, format_clock_domain_descriptor)
{
    DescriptorClockDomain cd{};
    cd.descriptor_type = DESCRIPTOR_CLOCK_DOMAIN;
    cd.descriptor_index = 0;
    cd.clock_source_index = 0;
    cd.clock_sources_count = 1;
    cd.clock_sources_offset = 76;

    std::string result;
    format_to(std::back_inserter(result), cd);
    EXPECT_TRUE(result.find("CLOCK_DOMAIN Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("clock_source_index:") != std::string::npos);
}

//
// Hex dump helper
//

TEST(aem_print, format_hex_dump)
{
    std::array<uint8_t, 4> data = {0xDE, 0xAD, 0xBE, 0xEF};
    std::string result;
    format_hex_dump(std::back_inserter(result), std::span<uint8_t const>(data));
    EXPECT_TRUE(result.find("de") != std::string::npos);
    EXPECT_TRUE(result.find("ad") != std::string::npos);
    EXPECT_TRUE(result.find("be") != std::string::npos);
    EXPECT_TRUE(result.find("ef") != std::string::npos);
}

//
// Descriptor too short for format_descriptor
//

TEST(aem_print, format_descriptor_too_short)
{
    ParsedDescriptor desc{};
    std::string result;
    format_descriptor(std::back_inserter(result), desc, 2);
    EXPECT_TRUE(result.find("too short") != std::string::npos);
}

//
// Stream descriptor formatting (via format_stream_descriptor)
//

TEST(aem_print, format_stream_input_descriptor)
{
    DescriptorStream stream{};
    stream.descriptor_type = DESCRIPTOR_STREAM_INPUT;
    stream.descriptor_index = 0;
    stream.clock_domain_index = 1;
    stream.stream_flags = 0x0004;
    stream.number_of_formats = 2;
    stream.formats_offset = 136;
    stream.avb_interface_index = 0;

    std::string result;
    format_stream_descriptor(std::back_inserter(result), stream, "STREAM_INPUT");
    EXPECT_TRUE(result.find("STREAM_INPUT Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("clock_domain_index:") != std::string::npos);
    EXPECT_TRUE(result.find("stream_flags:") != std::string::npos);
    EXPECT_TRUE(result.find("current_format:") != std::string::npos);
    EXPECT_TRUE(result.find("formats:") != std::string::npos);
    EXPECT_TRUE(result.find("avb_interface_index:") != std::string::npos);
}

TEST(aem_print, format_stream_output_descriptor)
{
    DescriptorStream stream{};
    stream.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
    stream.descriptor_index = 1;

    std::string result;
    format_stream_descriptor(std::back_inserter(result), stream, "STREAM_OUTPUT");
    EXPECT_TRUE(result.find("STREAM_OUTPUT Descriptor") != std::string::npos);
}

TEST(aem_print, format_jack_input_descriptor)
{
    DescriptorJack jack{};
    jack.descriptor_type = DESCRIPTOR_JACK_INPUT;
    jack.descriptor_index = 0;
    jack.jack_type = 0x0003;
    jack.jack_flags = 0x0001;

    std::string result;
    format_jack_descriptor(std::back_inserter(result), jack, "JACK_INPUT");
    EXPECT_TRUE(result.find("JACK_INPUT Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("jack_type:") != std::string::npos);
}

TEST(aem_print, format_jack_output_descriptor)
{
    DescriptorJack jack{};
    jack.descriptor_type = DESCRIPTOR_JACK_OUTPUT;
    jack.descriptor_index = 2;

    std::string result;
    format_jack_descriptor(std::back_inserter(result), jack, "JACK_OUTPUT");
    EXPECT_TRUE(result.find("JACK_OUTPUT Descriptor") != std::string::npos);
}

TEST(aem_print, format_avb_interface_descriptor)
{
    DescriptorAvbInterface avb{};
    avb.descriptor_type = DESCRIPTOR_AVB_INTERFACE;
    avb.descriptor_index = 0;
    avb.mac_address = Eui48(0x00, 0x1b, 0x21, 0x00, 0x01, 0x02);
    avb.interface_flags = 0x0003;
    avb.clock_identity = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22);
    avb.domain_number = 0;

    std::string result;
    format_to(std::back_inserter(result), avb);
    EXPECT_TRUE(result.find("AVB_INTERFACE Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("mac_address:") != std::string::npos);
    EXPECT_TRUE(result.find("clock_identity:") != std::string::npos);
    EXPECT_TRUE(result.find("domain_number:") != std::string::npos);
}

TEST(aem_print, format_memory_object_descriptor)
{
    DescriptorMemoryObject mem{};
    mem.descriptor_type = DESCRIPTOR_MEMORY_OBJECT;
    mem.descriptor_index = 0;
    mem.memory_object_type = 0x0001;
    mem.target_descriptor_type = DESCRIPTOR_ENTITY;
    mem.start_address = 0x1000;
    mem.length = 0x2000;

    std::string result;
    format_to(std::back_inserter(result), mem);
    EXPECT_TRUE(result.find("MEMORY_OBJECT Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("memory_object_type:") != std::string::npos);
    EXPECT_TRUE(result.find("start_address:") != std::string::npos);
    EXPECT_TRUE(result.find("length:") != std::string::npos);
}

TEST(aem_print, format_locale_descriptor)
{
    DescriptorLocale locale{};
    locale.descriptor_type = DESCRIPTOR_LOCALE;
    locale.descriptor_index = 0;
    locale.number_of_strings = 7;
    locale.base_strings = 0;

    std::string result;
    format_to(std::back_inserter(result), locale);
    EXPECT_TRUE(result.find("LOCALE Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("strings:") != std::string::npos);
}

TEST(aem_print, format_strings_descriptor)
{
    DescriptorStrings strings{};
    strings.descriptor_type = DESCRIPTOR_STRINGS;
    strings.descriptor_index = 0;

    std::string result;
    format_to(std::back_inserter(result), strings);
    EXPECT_TRUE(result.find("STRINGS Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("string_0:") != std::string::npos);
    EXPECT_TRUE(result.find("string_6:") != std::string::npos);
}

TEST(aem_print, format_stream_port_input_descriptor)
{
    DescriptorStreamPort port{};
    port.descriptor_type = DESCRIPTOR_STREAM_PORT_INPUT;
    port.descriptor_index = 0;
    port.clock_domain_index = 0;
    port.port_flags = 0x0002;
    port.number_of_clusters = 4;

    std::string result;
    format_stream_port_descriptor(std::back_inserter(result), port, "STREAM_PORT_INPUT");
    EXPECT_TRUE(result.find("STREAM_PORT_INPUT Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("clusters:") != std::string::npos);
    EXPECT_TRUE(result.find("maps:") != std::string::npos);
}

TEST(aem_print, format_stream_port_output_descriptor)
{
    DescriptorStreamPort port{};
    port.descriptor_type = DESCRIPTOR_STREAM_PORT_OUTPUT;
    port.descriptor_index = 1;

    std::string result;
    format_stream_port_descriptor(std::back_inserter(result), port, "STREAM_PORT_OUTPUT");
    EXPECT_TRUE(result.find("STREAM_PORT_OUTPUT Descriptor") != std::string::npos);
}

TEST(aem_print, format_audio_cluster_descriptor)
{
    DescriptorAudioCluster cluster{};
    cluster.descriptor_type = DESCRIPTOR_AUDIO_CLUSTER;
    cluster.descriptor_index = 0;
    cluster.channel_count = 2;
    cluster.format = 0x40;
    cluster.signal_source.signal_type = 0x0005;

    std::string result;
    format_to(std::back_inserter(result), cluster);
    EXPECT_TRUE(result.find("AUDIO_CLUSTER Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("channel_count:") != std::string::npos);
    EXPECT_TRUE(result.find("signal:") != std::string::npos);
}

TEST(aem_print, format_audio_map_descriptor)
{
    DescriptorAudioMap map{};
    map.descriptor_type = DESCRIPTOR_AUDIO_MAP;
    map.descriptor_index = 0;
    map.number_of_mappings = 8;

    std::string result;
    format_to(std::back_inserter(result), map);
    EXPECT_TRUE(result.find("AUDIO_MAP Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("mappings:") != std::string::npos);
}

TEST(aem_print, format_control_descriptor)
{
    DescriptorControl ctrl{};
    ctrl.descriptor_type = DESCRIPTOR_CONTROL;
    ctrl.descriptor_index = 0;
    ctrl.control_value_type = 0x0006;
    ctrl.control_type = Eui64(0x90, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x01);
    ctrl.number_of_values = 1;

    std::string result;
    format_to(std::back_inserter(result), ctrl);
    EXPECT_TRUE(result.find("CONTROL Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("control_value_type:") != std::string::npos);
    EXPECT_TRUE(result.find("control_type:") != std::string::npos);
}

TEST(aem_print, format_signal_selector_descriptor)
{
    DescriptorSignalSelector sel{};
    sel.descriptor_type = DESCRIPTOR_SIGNAL_SELECTOR;
    sel.descriptor_index = 0;
    sel.current_signal.signal_type = 0x0005;
    sel.number_of_sources = 3;

    std::string result;
    format_to(std::back_inserter(result), sel);
    EXPECT_TRUE(result.find("SIGNAL_SELECTOR Descriptor") != std::string::npos);
    EXPECT_TRUE(result.find("current_signal:") != std::string::npos);
    EXPECT_TRUE(result.find("sources:") != std::string::npos);
}

TEST(aem_print, format_descriptor_dispatch_stream_input)
{
    std::array<uint8_t, DescriptorStream::MINIMUM_LENGTH> buf{};
    buf[0] = 0x00;
    buf[1] = 0x05;
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
    std::string output;
    format_descriptor(std::back_inserter(output), *result.value(), buf.size());
    EXPECT_TRUE(output.find("STREAM_INPUT Descriptor") != std::string::npos);
}

TEST(aem_print, format_descriptor_dispatch_stream_output)
{
    std::array<uint8_t, DescriptorStream::MINIMUM_LENGTH> buf{};
    buf[0] = 0x00;
    buf[1] = 0x06;
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
    std::string output;
    format_descriptor(std::back_inserter(output), *result.value(), buf.size());
    EXPECT_TRUE(output.find("STREAM_OUTPUT Descriptor") != std::string::npos);
}

TEST(aem_print, format_descriptor_dispatch_jack_input)
{
    std::array<uint8_t, DescriptorJack::LENGTH> buf{};
    buf[0] = 0x00;
    buf[1] = 0x07;
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
    std::string output;
    format_descriptor(std::back_inserter(output), *result.value(), buf.size());
    EXPECT_TRUE(output.find("JACK_INPUT Descriptor") != std::string::npos);
}

TEST(aem_print, format_descriptor_dispatch_jack_output)
{
    std::array<uint8_t, DescriptorJack::LENGTH> buf{};
    buf[0] = 0x00;
    buf[1] = 0x08;
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
    std::string output;
    format_descriptor(std::back_inserter(output), *result.value(), buf.size());
    EXPECT_TRUE(output.find("JACK_OUTPUT Descriptor") != std::string::npos);
}

TEST(aem_print, format_descriptor_dispatch_avb_interface)
{
    std::array<uint8_t, DescriptorAvbInterface::MINIMUM_LENGTH> buf{};
    buf[0] = 0x00;
    buf[1] = 0x09;
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
    std::string output;
    format_descriptor(std::back_inserter(output), *result.value(), buf.size());
    EXPECT_TRUE(output.find("AVB_INTERFACE Descriptor") != std::string::npos);
}

TEST(aem_print, format_descriptor_dispatch_memory_object)
{
    std::array<uint8_t, DescriptorMemoryObject::LENGTH> buf{};
    buf[0] = 0x00;
    buf[1] = 0x0b;
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
    std::string output;
    format_descriptor(std::back_inserter(output), *result.value(), buf.size());
    EXPECT_TRUE(output.find("MEMORY_OBJECT Descriptor") != std::string::npos);
}

TEST(aem_print, format_descriptor_dispatch_locale)
{
    std::array<uint8_t, DescriptorLocale::LENGTH> buf{};
    buf[0] = 0x00;
    buf[1] = 0x0c;
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
    std::string output;
    format_descriptor(std::back_inserter(output), *result.value(), buf.size());
    EXPECT_TRUE(output.find("LOCALE Descriptor") != std::string::npos);
}

TEST(aem_print, format_descriptor_dispatch_strings)
{
    std::array<uint8_t, DescriptorStrings::LENGTH> buf{};
    buf[0] = 0x00;
    buf[1] = 0x0d;
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
    std::string output;
    format_descriptor(std::back_inserter(output), *result.value(), buf.size());
    EXPECT_TRUE(output.find("STRINGS Descriptor") != std::string::npos);
}

TEST(aem_print, format_descriptor_dispatch_stream_port_input)
{
    std::array<uint8_t, DescriptorStreamPort::LENGTH> buf{};
    buf[0] = 0x00;
    buf[1] = 0x0e;
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
    std::string output;
    format_descriptor(std::back_inserter(output), *result.value(), buf.size());
    EXPECT_TRUE(output.find("STREAM_PORT_INPUT Descriptor") != std::string::npos);
}

TEST(aem_print, format_descriptor_dispatch_stream_port_output)
{
    std::array<uint8_t, DescriptorStreamPort::LENGTH> buf{};
    buf[0] = 0x00;
    buf[1] = 0x0f;
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
    std::string output;
    format_descriptor(std::back_inserter(output), *result.value(), buf.size());
    EXPECT_TRUE(output.find("STREAM_PORT_OUTPUT Descriptor") != std::string::npos);
}

TEST(aem_print, format_descriptor_dispatch_audio_cluster)
{
    std::array<uint8_t, DescriptorAudioCluster::MINIMUM_LENGTH> buf{};
    buf[0] = 0x00;
    buf[1] = 0x14;
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
    std::string output;
    format_descriptor(std::back_inserter(output), *result.value(), buf.size());
    EXPECT_TRUE(output.find("AUDIO_CLUSTER Descriptor") != std::string::npos);
}

TEST(aem_print, format_descriptor_dispatch_audio_map)
{
    std::array<uint8_t, DescriptorAudioMap::LENGTH> buf{};
    buf[0] = 0x00;
    buf[1] = 0x17;
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
    std::string output;
    format_descriptor(std::back_inserter(output), *result.value(), buf.size());
    EXPECT_TRUE(output.find("AUDIO_MAP Descriptor") != std::string::npos);
}

TEST(aem_print, format_descriptor_dispatch_control)
{
    std::array<uint8_t, DescriptorControl::LENGTH> buf{};
    buf[0] = 0x00;
    buf[1] = 0x1a;
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
    std::string output;
    format_descriptor(std::back_inserter(output), *result.value(), buf.size());
    EXPECT_TRUE(output.find("CONTROL Descriptor") != std::string::npos);
}

TEST(aem_print, format_descriptor_dispatch_signal_selector)
{
    std::array<uint8_t, DescriptorSignalSelector::LENGTH> buf{};
    buf[0] = 0x00;
    buf[1] = 0x1b;
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
    std::string output;
    format_descriptor(std::back_inserter(output), *result.value(), buf.size());
    EXPECT_TRUE(output.find("SIGNAL_SELECTOR Descriptor") != std::string::npos);
}

TEST(aem_print, format_descriptor_convenience_parse_failure)
{
    std::array<uint8_t, 2> tiny{};
    std::string result;
    format_descriptor(std::back_inserter(result), std::span<uint8_t const>(tiny));
    EXPECT_TRUE(result.find("Failed to parse") != std::string::npos);
}

TEST(aem_print, format_aem_descriptor_helper)
{
    std::string result;
    format_aem_descriptor(std::back_inserter(result), DESCRIPTOR_ENTITY, 0);
    EXPECT_TRUE(result.find("Entity") != std::string::npos);
    EXPECT_TRUE(result.find("index=0") != std::string::npos);
}

//
// Smoke tests for the AEM payload format_to overloads.
// Each test constructs a default-initialized payload of a given type,
// writes it through format_to, and asserts the output is non-empty.
// Collectively these cover every AEM command/response payload's
// pretty-print path.
//

template <typename Payload>
void expect_format_to_non_empty()
{
    Payload payload{};
    std::string out;
    (void)aem::format_to(std::back_inserter(out), payload);
    EXPECT_TRUE(!out.empty());
}

TEST(aem_print_payloads, acquire_entity)
{
    expect_format_to_non_empty<aem::AemAcquireEntityPayload>();
}
TEST(aem_print_payloads, lock_entity)
{
    expect_format_to_non_empty<aem::AemLockEntityPayload>();
}
TEST(aem_print_payloads, read_descriptor_command)
{
    expect_format_to_non_empty<aem::AemReadDescriptorCommandPayload>();
}
TEST(aem_print_payloads, set_configuration)
{
    expect_format_to_non_empty<aem::AemSetConfigurationPayload>();
}
TEST(aem_print_payloads, stream_format)
{
    expect_format_to_non_empty<aem::AemStreamFormatPayload>();
}
TEST(aem_print_payloads, get_stream_format_command)
{
    expect_format_to_non_empty<aem::AemGetStreamFormatCommandPayload>();
}
TEST(aem_print_payloads, stream_info)
{
    expect_format_to_non_empty<aem::AemStreamInfoPayload>();
}
TEST(aem_print_payloads, get_stream_info_command)
{
    expect_format_to_non_empty<aem::AemGetStreamInfoCommandPayload>();
}
TEST(aem_print_payloads, name)
{
    expect_format_to_non_empty<aem::AemNamePayload>();
}
TEST(aem_print_payloads, name_command)
{
    expect_format_to_non_empty<aem::AemNameCommandPayload>();
}
TEST(aem_print_payloads, sampling_rate)
{
    expect_format_to_non_empty<aem::AemSamplingRatePayload>();
}
TEST(aem_print_payloads, get_sampling_rate_command)
{
    expect_format_to_non_empty<aem::AemGetSamplingRateCommandPayload>();
}
TEST(aem_print_payloads, clock_source)
{
    expect_format_to_non_empty<aem::AemClockSourcePayload>();
}
TEST(aem_print_payloads, get_clock_source_command)
{
    expect_format_to_non_empty<aem::AemGetClockSourceCommandPayload>();
}
TEST(aem_print_payloads, streaming)
{
    expect_format_to_non_empty<aem::AemStreamingPayload>();
}
TEST(aem_print_payloads, identify)
{
    expect_format_to_non_empty<aem::AemIdentifyPayload>();
}
TEST(aem_print_payloads, reboot)
{
    expect_format_to_non_empty<aem::AemRebootPayload>();
}
TEST(aem_print_payloads, avb_info)
{
    expect_format_to_non_empty<aem::AemAvbInfoPayload>();
}
TEST(aem_print_payloads, get_avb_info_command)
{
    expect_format_to_non_empty<aem::AemGetAvbInfoCommandPayload>();
}
TEST(aem_print_payloads, get_audio_map_command)
{
    expect_format_to_non_empty<aem::AemGetAudioMapCommandPayload>();
}
TEST(aem_print_payloads, signal_selector)
{
    expect_format_to_non_empty<aem::AemSignalSelectorPayload>();
}
TEST(aem_print_payloads, get_signal_selector_command)
{
    expect_format_to_non_empty<aem::AemGetSignalSelectorCommandPayload>();
}
TEST(aem_print_payloads, counters)
{
    expect_format_to_non_empty<aem::AemCountersPayload>();
}
TEST(aem_print_payloads, get_counters_command)
{
    expect_format_to_non_empty<aem::AemGetCountersCommandPayload>();
}
TEST(aem_print_payloads, operation_status)
{
    expect_format_to_non_empty<aem::AemOperationStatusPayload>();
}
TEST(aem_print_payloads, abort_operation)
{
    expect_format_to_non_empty<aem::AemAbortOperationPayload>();
}

//
// Wire-format decoder safety: format_atdecc() is invoked from packet-dump
// tools that may receive arbitrary bytes from the wire. It must produce a
// readable "truncated" message (and fall back to a hex dump) instead of
// running load_unchecked over a too-small buffer.
//

namespace {

template <typename Subtype>
auto format_pdu_to_string(std::span<uint8_t const> pdu) -> std::string
{
    std::string out;
    (void)format_atdecc(std::back_inserter(out), pdu);
    return out;
}

}  // namespace

TEST(atdecc_print_safety, empty_payload)
{
    std::string out;
    (void)format_atdecc(std::back_inserter(out), std::span<uint8_t const>{});
    EXPECT_TRUE(out.find("empty") != std::string::npos);
}

TEST(atdecc_print_safety, adp_truncated_below_full_pdu)
{
    // Subtype byte = ADP (0x7A), but only 20 bytes total — far short of the
    // 68-byte AdpDu. Must report "truncated" not crash.
    std::array<uint8_t, 20> pdu{};
    pdu[0] = static_cast<uint8_t>(avtp::AvtpSubtype::adp);
    auto out = format_pdu_to_string<AdpDu>(std::span<uint8_t const>{pdu});
    EXPECT_TRUE(out.find("ADP") != std::string::npos);
    EXPECT_TRUE(out.find("truncated") != std::string::npos);
}

TEST(atdecc_print_safety, acmp_truncated_below_full_pdu)
{
    std::array<uint8_t, 20> pdu{};
    pdu[0] = static_cast<uint8_t>(avtp::AvtpSubtype::acmp);
    auto out = format_pdu_to_string<AcmpDu>(std::span<uint8_t const>{pdu});
    EXPECT_TRUE(out.find("ACMP") != std::string::npos);
    EXPECT_TRUE(out.find("truncated") != std::string::npos);
}

TEST(atdecc_print_safety, aecp_truncated_below_common_header)
{
    // AECP common header is 12 bytes; supply only 6.
    std::array<uint8_t, 6> pdu{};
    pdu[0] = static_cast<uint8_t>(avtp::AvtpSubtype::aecp);
    auto out = format_pdu_to_string<AecpDuCommon>(std::span<uint8_t const>{pdu});
    EXPECT_TRUE(out.find("AECP") != std::string::npos);
    EXPECT_TRUE(out.find("truncated") != std::string::npos);
}

TEST(atdecc_print_safety, aecp_aem_truncated_between_common_and_extended)
{
    // AECP common header (22 bytes) is present and message_type signals AEM,
    // but the extended AEM header (24 bytes total) is not — caller must fall
    // back to the AEM-truncated path.
    std::array<uint8_t, 23> pdu{};
    pdu[0] = static_cast<uint8_t>(avtp::AvtpSubtype::aecp);
    pdu[1] = AECP_MESSAGE_TYPE_AEM_COMMAND;  // message_type in low nibble
    auto out = format_pdu_to_string<AemDu>(std::span<uint8_t const>{pdu});
    EXPECT_TRUE(out.find("AEM") != std::string::npos);
    EXPECT_TRUE(out.find("truncated") != std::string::npos);
}

//
// Smoke tests for the descriptor format_to overloads added to
// atdecc_aem_descriptor_format.hpp. Each test default-constructs the
// descriptor, runs format_to on a back_insert_iterator, and asserts the
// output is non-empty.
//

template <typename Descriptor>
void expect_descriptor_format_non_empty()
{
    Descriptor d{};
    std::string out;
    (void)aem::format_to(std::back_inserter(out), d);
    EXPECT_TRUE(!out.empty());
}

TEST(aem_descriptor_format, descriptor_ref_smoke)
{
    DescriptorRef d{};
    std::string out;
    aem::format_to(std::back_inserter(out), d);
    EXPECT_TRUE(!out.empty());
}
TEST(aem_descriptor_format, descriptor_count_entry_smoke)
{
    DescriptorCountEntry e{};
    std::string out;
    aem::format_to(std::back_inserter(out), e);
    EXPECT_TRUE(!out.empty());
}
TEST(aem_descriptor_format, stream_smoke)
{
    DescriptorStream d{};
    std::string out;
    aem::format_to(std::back_inserter(out), d);
    EXPECT_TRUE(!out.empty());
}
TEST(aem_descriptor_format, jack_smoke)
{
    DescriptorJack d{};
    std::string out;
    aem::format_to(std::back_inserter(out), d);
    EXPECT_TRUE(!out.empty());
}
TEST(aem_descriptor_format, stream_port_smoke)
{
    DescriptorStreamPort d{};
    std::string out;
    aem::format_to(std::back_inserter(out), d);
    EXPECT_TRUE(!out.empty());
}
TEST(aem_descriptor_format, video_unit_smoke)
{
    expect_descriptor_format_non_empty<DescriptorVideoUnit>();
}
TEST(aem_descriptor_format, sensor_unit_smoke)
{
    expect_descriptor_format_non_empty<DescriptorSensorUnit>();
}
TEST(aem_descriptor_format, external_port_smoke)
{
    expect_descriptor_format_non_empty<DescriptorExternalPort>();
}
TEST(aem_descriptor_format, internal_port_smoke)
{
    expect_descriptor_format_non_empty<DescriptorInternalPort>();
}
TEST(aem_descriptor_format, video_cluster_smoke)
{
    expect_descriptor_format_non_empty<DescriptorVideoCluster>();
}
TEST(aem_descriptor_format, sensor_cluster_smoke)
{
    expect_descriptor_format_non_empty<DescriptorSensorCluster>();
}
TEST(aem_descriptor_format, video_map_smoke)
{
    expect_descriptor_format_non_empty<DescriptorVideoMap>();
}
TEST(aem_descriptor_format, sensor_map_smoke)
{
    expect_descriptor_format_non_empty<DescriptorSensorMap>();
}
TEST(aem_descriptor_format, mixer_smoke)
{
    expect_descriptor_format_non_empty<DescriptorMixer>();
}
TEST(aem_descriptor_format, matrix_smoke)
{
    expect_descriptor_format_non_empty<DescriptorMatrix>();
}
TEST(aem_descriptor_format, matrix_signal_smoke)
{
    expect_descriptor_format_non_empty<DescriptorMatrixSignal>();
}
TEST(aem_descriptor_format, signal_splitter_smoke)
{
    expect_descriptor_format_non_empty<DescriptorSignalSplitter>();
}
TEST(aem_descriptor_format, signal_combiner_smoke)
{
    expect_descriptor_format_non_empty<DescriptorSignalCombiner>();
}
TEST(aem_descriptor_format, signal_demultiplexer_smoke)
{
    expect_descriptor_format_non_empty<DescriptorSignalDemultiplexer>();
}
TEST(aem_descriptor_format, signal_multiplexer_smoke)
{
    expect_descriptor_format_non_empty<DescriptorSignalMultiplexer>();
}
TEST(aem_descriptor_format, signal_transcoder_smoke)
{
    expect_descriptor_format_non_empty<DescriptorSignalTranscoder>();
}
TEST(aem_descriptor_format, control_block_smoke)
{
    expect_descriptor_format_non_empty<DescriptorControlBlock>();
}
TEST(aem_descriptor_format, timing_smoke)
{
    expect_descriptor_format_non_empty<DescriptorTiming>();
}
TEST(aem_descriptor_format, ptp_instance_smoke)
{
    expect_descriptor_format_non_empty<DescriptorPtpInstance>();
}
TEST(aem_descriptor_format, ptp_port_smoke)
{
    expect_descriptor_format_non_empty<DescriptorPtpPort>();
}

//
// Smoke tests for the new AEM command payload format_to overloads added to
// atdecc_aem_command_format.hpp. Each default-constructs the payload,
// runs format_to on a back_insert_iterator, and asserts non-empty output.
//

TEST(aem_command_format, audio_mapping_smoke)
{
    expect_format_to_non_empty<aem::AemAudioMapping>();
}
TEST(aem_command_format, control_payload_header_smoke)
{
    expect_format_to_non_empty<aem::AemControlPayloadHeader>();
}
TEST(aem_command_format, read_descriptor_response_smoke)
{
    expect_format_to_non_empty<aem::AemReadDescriptorResponsePayload>();
}
TEST(aem_command_format, ptp_port_info_command_smoke)
{
    expect_format_to_non_empty<aem::AemPtpPortInfoCommandPayload>();
}
TEST(aem_command_format, get_ptp_port_info_response_smoke)
{
    expect_format_to_non_empty<aem::AemGetPtpPortInfoResponsePayload>();
}
TEST(aem_command_format, start_operation_command_smoke)
{
    expect_format_to_non_empty<aem::AemStartOperationCommandPayload>();
}
TEST(aem_command_format, start_operation_response_smoke)
{
    expect_format_to_non_empty<aem::AemStartOperationResponsePayload>();
}

//
// Dispatcher coverage: verify format_descriptor renders every descriptor
// type through a dispatch path (parse_descriptor + format_descriptor),
// not the default hex-dump fallback.
//

template <uint16_t DescType>
void expect_dispatch_non_empty(size_t min_len)
{
    std::vector<uint8_t> buf(min_len, 0);
    statusbar::span_store(statusbar::make_span(buf, {.start = 0, .length = 2}), statusbar::ieee::doublet_t{DescType});
    auto parsed = aem::parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(parsed.has_value());
    std::string out;
    aem::format_descriptor(std::back_inserter(out), *parsed.value(), buf.size());
    EXPECT_TRUE(!out.empty());
}

TEST(aem_descriptor_dispatch, video_unit)
{
    expect_dispatch_non_empty<DESCRIPTOR_VIDEO_UNIT>(DescriptorVideoUnit::LENGTH);
}
TEST(aem_descriptor_dispatch, sensor_unit)
{
    expect_dispatch_non_empty<DESCRIPTOR_SENSOR_UNIT>(DescriptorSensorUnit::LENGTH);
}
TEST(aem_descriptor_dispatch, external_port_input)
{
    expect_dispatch_non_empty<DESCRIPTOR_EXTERNAL_PORT_INPUT>(DescriptorExternalPort::LENGTH);
}
TEST(aem_descriptor_dispatch, external_port_output)
{
    expect_dispatch_non_empty<DESCRIPTOR_EXTERNAL_PORT_OUTPUT>(DescriptorExternalPort::LENGTH);
}
TEST(aem_descriptor_dispatch, internal_port_input)
{
    expect_dispatch_non_empty<DESCRIPTOR_INTERNAL_PORT_INPUT>(DescriptorInternalPort::LENGTH);
}
TEST(aem_descriptor_dispatch, internal_port_output)
{
    expect_dispatch_non_empty<DESCRIPTOR_INTERNAL_PORT_OUTPUT>(DescriptorInternalPort::LENGTH);
}
TEST(aem_descriptor_dispatch, video_cluster)
{
    expect_dispatch_non_empty<DESCRIPTOR_VIDEO_CLUSTER>(DescriptorVideoCluster::LENGTH);
}
TEST(aem_descriptor_dispatch, sensor_cluster)
{
    expect_dispatch_non_empty<DESCRIPTOR_SENSOR_CLUSTER>(DescriptorSensorCluster::LENGTH);
}
TEST(aem_descriptor_dispatch, video_map)
{
    expect_dispatch_non_empty<DESCRIPTOR_VIDEO_MAP>(DescriptorVideoMap::LENGTH);
}
TEST(aem_descriptor_dispatch, sensor_map)
{
    expect_dispatch_non_empty<DESCRIPTOR_SENSOR_MAP>(DescriptorSensorMap::LENGTH);
}
TEST(aem_descriptor_dispatch, mixer)
{
    expect_dispatch_non_empty<DESCRIPTOR_MIXER>(DescriptorMixer::LENGTH);
}
TEST(aem_descriptor_dispatch, matrix)
{
    expect_dispatch_non_empty<DESCRIPTOR_MATRIX>(DescriptorMatrix::LENGTH);
}
TEST(aem_descriptor_dispatch, matrix_signal)
{
    expect_dispatch_non_empty<DESCRIPTOR_MATRIX_SIGNAL>(DescriptorMatrixSignal::LENGTH);
}
TEST(aem_descriptor_dispatch, signal_splitter)
{
    expect_dispatch_non_empty<DESCRIPTOR_SIGNAL_SPLITTER>(DescriptorSignalSplitter::LENGTH);
}
TEST(aem_descriptor_dispatch, signal_combiner)
{
    expect_dispatch_non_empty<DESCRIPTOR_SIGNAL_COMBINER>(DescriptorSignalCombiner::LENGTH);
}
TEST(aem_descriptor_dispatch, signal_demultiplexer)
{
    expect_dispatch_non_empty<DESCRIPTOR_SIGNAL_DEMULTIPLEXER>(DescriptorSignalDemultiplexer::LENGTH);
}
TEST(aem_descriptor_dispatch, signal_multiplexer)
{
    expect_dispatch_non_empty<DESCRIPTOR_SIGNAL_MULTIPLEXER>(DescriptorSignalMultiplexer::LENGTH);
}
TEST(aem_descriptor_dispatch, signal_transcoder)
{
    expect_dispatch_non_empty<DESCRIPTOR_SIGNAL_TRANSCODER>(DescriptorSignalTranscoder::MINIMUM_LENGTH);
}
TEST(aem_descriptor_dispatch, control_block)
{
    expect_dispatch_non_empty<DESCRIPTOR_CONTROL_BLOCK>(DescriptorControlBlock::MINIMUM_LENGTH);
}
TEST(aem_descriptor_dispatch, timing)
{
    expect_dispatch_non_empty<DESCRIPTOR_TIMING>(DescriptorTiming::LENGTH);
}
TEST(aem_descriptor_dispatch, ptp_instance)
{
    expect_dispatch_non_empty<DESCRIPTOR_PTP_INSTANCE>(DescriptorPtpInstance::LENGTH);
}
TEST(aem_descriptor_dispatch, ptp_port)
{
    expect_dispatch_non_empty<DESCRIPTOR_PTP_PORT>(DescriptorPtpPort::LENGTH);
}

//
// DescriptorClockDomain list helpers
//

TEST(aem_clock_domain_list, default_used_is_empty)
{
    DescriptorClockDomain d{};
    EXPECT_EQ(d.used_clock_sources().size(), 0U);
    EXPECT_EQ(d.wire_size(), DescriptorClockDomain::LENGTH);
}

TEST(aem_clock_domain_list, push_updates_count_and_view)
{
    DescriptorClockDomain d{};
    EXPECT_TRUE(d.push_clock_source(doublet_t{7}));
    EXPECT_TRUE(d.push_clock_source(doublet_t{9}));
    EXPECT_TRUE(d.push_clock_source(doublet_t{11}));

    EXPECT_EQ(d.clock_sources_count.get(), 3U);
    auto const view = d.used_clock_sources();
    EXPECT_EQ(view.size(), 3U);
    EXPECT_EQ(view[0].get(), 7U);
    EXPECT_EQ(view[1].get(), 9U);
    EXPECT_EQ(view[2].get(), 11U);
    EXPECT_EQ(d.wire_size(), DescriptorClockDomain::LENGTH + 3U * sizeof(doublet_t));
}

TEST(aem_clock_domain_list, push_returns_false_at_capacity)
{
    DescriptorClockDomain d{};
    for (size_t i = 0; i < DescriptorClockDomain::MAX_CLOCK_SOURCES; ++i) {
        EXPECT_TRUE(d.push_clock_source(doublet_t{static_cast<uint16_t>(i)}));
    }
    EXPECT_EQ(d.clock_sources_count.get(), DescriptorClockDomain::MAX_CLOCK_SOURCES);
    EXPECT_FALSE(d.push_clock_source(doublet_t{0xFFFF}));
    EXPECT_EQ(d.clock_sources_count.get(), DescriptorClockDomain::MAX_CLOCK_SOURCES);
    // Last successful push should be at index MAX-1 with value MAX-1.
    auto const view = d.used_clock_sources();
    EXPECT_EQ(view.size(), DescriptorClockDomain::MAX_CLOCK_SOURCES);
    EXPECT_EQ(view[DescriptorClockDomain::MAX_CLOCK_SOURCES - 1].get(), DescriptorClockDomain::MAX_CLOCK_SOURCES - 1);
}

TEST(aem_clock_domain_list, clear_resets_count_only)
{
    DescriptorClockDomain d{};
    (void)d.push_clock_source(doublet_t{42});
    (void)d.push_clock_source(doublet_t{99});
    EXPECT_EQ(d.used_clock_sources().size(), 2U);
    d.clear_clock_sources();
    EXPECT_EQ(d.used_clock_sources().size(), 0U);
    EXPECT_EQ(d.wire_size(), DescriptorClockDomain::LENGTH);
    // Backing storage is intentionally not zeroed; push_clock_source after
    // clear writes over the stale entry.
    EXPECT_TRUE(d.push_clock_source(doublet_t{7}));
    EXPECT_EQ(d.used_clock_sources()[0].get(), 7U);
}

TEST(aem_clock_domain_list, format_to_emits_each_entry)
{
    DescriptorClockDomain d{};
    (void)d.push_clock_source(doublet_t{0});
    (void)d.push_clock_source(doublet_t{1});
    (void)d.push_clock_source(doublet_t{3});
    std::string buf;
    format_to(std::back_inserter(buf), d);
    EXPECT_TRUE(buf.find("count=3") != std::string::npos);
    EXPECT_TRUE(buf.find("[0] = 0") != std::string::npos);
    EXPECT_TRUE(buf.find("[1] = 1") != std::string::npos);
    EXPECT_TRUE(buf.find("[2] = 3") != std::string::npos);
}

//
// DescriptorConfiguration descriptor_counts list helpers
//

TEST(aem_configuration_list, push_view_capacity_clear)
{
    DescriptorConfiguration d{};
    EXPECT_EQ(d.used_descriptor_counts().size(), 0U);

    DescriptorCountEntry e0{};
    e0.descriptor_type = static_cast<uint16_t>(DESCRIPTOR_AUDIO_UNIT);
    e0.count = 1;
    EXPECT_TRUE(d.push_descriptor_count(e0));
    DescriptorCountEntry e1{};
    e1.descriptor_type = static_cast<uint16_t>(DESCRIPTOR_STREAM_INPUT);
    e1.count = 2;
    EXPECT_TRUE(d.push_descriptor_count(e1));
    EXPECT_EQ(d.descriptor_counts_count.get(), 2U);
    auto const view = d.used_descriptor_counts();
    EXPECT_EQ(view.size(), 2U);
    EXPECT_EQ(view[1].count.get(), 2U);

    for (size_t i = d.descriptor_counts_count.get(); i < DescriptorConfiguration::MAX_DESCRIPTOR_COUNTS; ++i) {
        EXPECT_TRUE(d.push_descriptor_count(DescriptorCountEntry{}));
    }
    EXPECT_FALSE(d.push_descriptor_count(DescriptorCountEntry{}));

    d.clear_descriptor_counts();
    EXPECT_EQ(d.used_descriptor_counts().size(), 0U);
}

TEST(aem_configuration_list, format_to_emits_each_entry)
{
    DescriptorConfiguration d{};
    DescriptorCountEntry e{};
    e.descriptor_type = static_cast<uint16_t>(DESCRIPTOR_STREAM_OUTPUT);
    e.count = 4;
    (void)d.push_descriptor_count(e);
    std::string buf;
    format_to(std::back_inserter(buf), d);
    EXPECT_TRUE(buf.find("count=1") != std::string::npos);
    EXPECT_TRUE(buf.find("[0] = ") != std::string::npos);
    EXPECT_TRUE(buf.find(": 4") != std::string::npos);
}

//
// DescriptorAudioUnit sampling_rates list helpers
//

TEST(aem_audio_unit_list, push_view_capacity_clear)
{
    DescriptorAudioUnit d{};
    EXPECT_EQ(d.used_sampling_rates().size(), 0U);
    EXPECT_TRUE(d.push_sampling_rate(quadlet_t{48000}));
    EXPECT_TRUE(d.push_sampling_rate(quadlet_t{96000}));
    EXPECT_EQ(d.used_sampling_rates().size(), 2U);
    EXPECT_EQ(d.used_sampling_rates()[1].get(), 96000U);

    for (size_t i = d.sampling_rates_count.get(); i < DescriptorAudioUnit::MAX_SAMPLING_RATES; ++i) {
        EXPECT_TRUE(d.push_sampling_rate(quadlet_t{0}));
    }
    EXPECT_FALSE(d.push_sampling_rate(quadlet_t{0}));

    d.clear_sampling_rates();
    EXPECT_EQ(d.used_sampling_rates().size(), 0U);
}

TEST(aem_audio_unit_list, format_to_emits_each_entry)
{
    DescriptorAudioUnit d{};
    (void)d.push_sampling_rate(quadlet_t{48000});
    std::string buf;
    format_to(std::back_inserter(buf), d);
    EXPECT_TRUE(buf.find("sampling_rates: count=1") != std::string::npos);
    EXPECT_TRUE(buf.find("[0] = 0x0000bb80") != std::string::npos);
}

//
// DescriptorStream stream_formats list helpers
//

TEST(aem_stream_list, push_view_capacity_clear)
{
    DescriptorStream d{};
    EXPECT_EQ(d.used_stream_formats().size(), 0U);
    Eui64 const fmt{0x00, 0xA0, 0x02, 0x40, 0x40, 0x00, 0x00, 0x60};
    EXPECT_TRUE(d.push_stream_format(fmt));
    EXPECT_EQ(d.used_stream_formats().size(), 1U);
    EXPECT_EQ(d.used_stream_formats()[0], fmt);

    for (size_t i = d.number_of_formats.get(); i < DescriptorStream::MAX_STREAM_FORMATS; ++i) {
        EXPECT_TRUE(d.push_stream_format(Eui64{}));
    }
    EXPECT_FALSE(d.push_stream_format(Eui64{}));

    d.clear_stream_formats();
    EXPECT_EQ(d.used_stream_formats().size(), 0U);
}

TEST(aem_stream_list, format_to_emits_each_entry)
{
    DescriptorStream d{};
    d.descriptor_type = static_cast<uint16_t>(DESCRIPTOR_STREAM_INPUT);
    (void)d.push_stream_format(Eui64{0x00, 0xA0, 0x02, 0x40, 0x40, 0x00, 0x00, 0x60});
    std::string buf;
    format_stream_descriptor(std::back_inserter(buf), d, "STREAM_INPUT");
    EXPECT_TRUE(buf.find("formats: count=1") != std::string::npos);
    EXPECT_TRUE(buf.find("[0] = 00:a0:02:40:40:00:00:60") != std::string::npos);
}

//
// DescriptorAudioMap mappings list helpers
//

TEST(aem_audio_map_list, push_view_capacity_clear)
{
    DescriptorAudioMap d{};
    EXPECT_EQ(d.used_mappings().size(), 0U);
    AudioMapping m{};
    m.mapping_stream_index = 0;
    m.mapping_stream_channel = 1;
    m.mapping_cluster_offset = 2;
    m.mapping_cluster_channel = 3;
    EXPECT_TRUE(d.push_mapping(m));
    EXPECT_EQ(d.used_mappings().size(), 1U);
    EXPECT_EQ(d.used_mappings()[0].mapping_cluster_channel.get(), 3U);

    for (size_t i = d.number_of_mappings.get(); i < DescriptorAudioMap::MAX_MAPPINGS; ++i) {
        EXPECT_TRUE(d.push_mapping(AudioMapping{}));
    }
    EXPECT_FALSE(d.push_mapping(AudioMapping{}));

    d.clear_mappings();
    EXPECT_EQ(d.used_mappings().size(), 0U);
}

TEST(aem_audio_map_list, format_to_emits_each_entry)
{
    DescriptorAudioMap d{};
    AudioMapping m{};
    m.mapping_stream_index = 1;
    m.mapping_stream_channel = 2;
    m.mapping_cluster_offset = 3;
    m.mapping_cluster_channel = 4;
    (void)d.push_mapping(m);
    std::string buf;
    format_to(std::back_inserter(buf), d);
    EXPECT_TRUE(buf.find("mappings: count=1") != std::string::npos);
    EXPECT_TRUE(buf.find("stream[1]ch2 -> cluster[3]ch4") != std::string::npos);
}

//
// DescriptorVideoMap mappings list helpers
//

TEST(aem_video_map_list, push_view_capacity_clear)
{
    DescriptorVideoMap d{};
    EXPECT_EQ(d.used_mappings().size(), 0U);
    VideoMapping m{};
    m.mapping_stream_index = 7;
    m.mapping_program_stream = 5;
    m.mapping_elementary_stream = 3;
    m.mapping_cluster_offset = 2;
    EXPECT_TRUE(d.push_mapping(m));
    EXPECT_EQ(d.used_mappings().size(), 1U);
    EXPECT_EQ(d.used_mappings()[0].mapping_elementary_stream.get(), 3U);
    for (size_t i = d.number_of_mappings.get(); i < DescriptorVideoMap::MAX_MAPPINGS; ++i) {
        EXPECT_TRUE(d.push_mapping(VideoMapping{}));
    }
    EXPECT_FALSE(d.push_mapping(VideoMapping{}));
    d.clear_mappings();
    EXPECT_EQ(d.used_mappings().size(), 0U);
}

TEST(aem_video_map_list, format_to_emits_each_entry)
{
    DescriptorVideoMap d{};
    VideoMapping m{};
    m.mapping_stream_index = 7;
    m.mapping_program_stream = 5;
    m.mapping_elementary_stream = 3;
    m.mapping_cluster_offset = 2;
    (void)d.push_mapping(m);
    std::string buf;
    format_to(std::back_inserter(buf), d);
    EXPECT_TRUE(buf.find("mappings: count=1") != std::string::npos);
    EXPECT_TRUE(buf.find("stream[7] prog=5 es=3 -> cluster[2]") != std::string::npos);
}

//
// DescriptorSensorMap mappings list helpers
//

TEST(aem_sensor_map_list, push_view_capacity_clear)
{
    DescriptorSensorMap d{};
    EXPECT_EQ(d.used_mappings().size(), 0U);
    SensorMapping m{};
    m.mapping_stream_index = 4;
    m.mapping_stream_channel = 5;
    m.mapping_cluster_offset = 6;
    m.mapping_cluster_channel = 7;
    EXPECT_TRUE(d.push_mapping(m));
    EXPECT_EQ(d.used_mappings().size(), 1U);
    EXPECT_EQ(d.used_mappings()[0].mapping_cluster_channel.get(), 7U);
    for (size_t i = d.number_of_mappings.get(); i < DescriptorSensorMap::MAX_MAPPINGS; ++i) {
        EXPECT_TRUE(d.push_mapping(SensorMapping{}));
    }
    EXPECT_FALSE(d.push_mapping(SensorMapping{}));
    d.clear_mappings();
    EXPECT_EQ(d.used_mappings().size(), 0U);
}

TEST(aem_sensor_map_list, format_to_emits_each_entry)
{
    DescriptorSensorMap d{};
    SensorMapping m{};
    m.mapping_stream_index = 4;
    m.mapping_stream_channel = 5;
    m.mapping_cluster_offset = 6;
    m.mapping_cluster_channel = 7;
    (void)d.push_mapping(m);
    std::string buf;
    format_to(std::back_inserter(buf), d);
    EXPECT_TRUE(buf.find("mappings: count=1") != std::string::npos);
    EXPECT_TRUE(buf.find("stream[4]ch5 -> cluster[6]ch7") != std::string::npos);
}

// Security regression: a GET_CONFIGURATION *command* carries no configuration_index
// (min_length 0), yet parse_aem accepts a zero-length payload. Formatting the command
// direction must NOT read the 4-byte set_configuration struct off that short payload
// (OOB read on the print/analyzer path). The command now formats no fields.
TEST(aem_print, format_get_configuration_command_does_not_read_payload)
{
    std::array<uint8_t, 0> empty{};
    auto parsed = parse_aem(AEM_COMMAND_GET_CONFIGURATION, /*is_response=*/false, std::span<uint8_t const>(empty));
    EXPECT_TRUE(parsed.has_value());  // min_length 0 -> accepted
    if (parsed.has_value()) {
        std::string result;
        format_aem(
            std::back_inserter(result),
            AEM_COMMAND_GET_CONFIGURATION,
            /*is_response=*/false,
            **parsed,
            std::span<uint8_t const>(empty));
        EXPECT_TRUE(result.empty());  // no set_configuration fields read/printed
    }
}

// Same for OPERATION_STATUS: the command direction (min_length 0) must not read the
// 8-byte operation_status struct.
TEST(aem_print, format_operation_status_command_does_not_read_payload)
{
    std::array<uint8_t, 0> empty{};
    auto parsed = parse_aem(AEM_COMMAND_OPERATION_STATUS, /*is_response=*/false, std::span<uint8_t const>(empty));
    EXPECT_TRUE(parsed.has_value());
    if (parsed.has_value()) {
        std::string result;
        format_aem(
            std::back_inserter(result),
            AEM_COMMAND_OPERATION_STATUS,
            /*is_response=*/false,
            **parsed,
            std::span<uint8_t const>(empty));
        EXPECT_TRUE(result.empty());
    }
}

//
// Test Runner
//

TEST_MAIN(statusbar_atdecc, atdecc_aem_print_test)
