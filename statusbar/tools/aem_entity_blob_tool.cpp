// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// aem-entity-blob — generate an AEM descriptor-storage blob for the AVB
// audio-bridge entity, consumed by AvbEntityAm824IO::create (and any
// AvbEntity*). Replaces the externally-produced testdata/simple2.bin with an
// in-tree, interop-grade model.
//
// The model is a single-configuration 8-channel audio bridge: one
// STREAM_INPUT (listener) + one STREAM_OUTPUT (talker), each carrying an
// 8-channel IEC 61883-6 AM824 stream at 96 kHz, with the STREAM_PORT /
// AUDIO_CLUSTER / AUDIO_MAP / CLOCK_SOURCE / CLOCK_DOMAIN / LOCALE / STRINGS
// descriptors a controller and real endpoints (third-party devices) expect.
//
// Stream format codes (see tests/avb/device-enumerations/README.md):
//   AM824 8-ch 96 kHz = 0x00A0040860000800  (byte2 SFC=4=96k, byte3=8ch)
//
// Usage: aem-entity-blob [--out FILE] [--channels N] [--name NAME]

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/tools/aem_entity_blob.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace statusbar;
using namespace statusbar::atdecc::aem;

// Entity / talker / listener capability bits (IEEE 1722.1 Clause 6.2.1).
constexpr uint32_t CAP_AEM = 0x00000008;
constexpr uint32_t CAP_CLASS_A = 0x00000100;
constexpr uint32_t CAP_GPTP = 0x00000400;
constexpr uint16_t TALKER_IMPLEMENTED = 0x0001;
constexpr uint16_t TALKER_MEDIA_CLOCK_SOURCE = 0x0800;  // talker_capabilities::MEDIA_CLOCK_SOURCE
constexpr uint16_t TALKER_AUDIO_SOURCE = 0x4000;
constexpr uint16_t LISTENER_IMPLEMENTED = 0x0001;
constexpr uint16_t LISTENER_AUDIO_SINK = 0x4000;

// No localized description (use the inline object_name instead).
constexpr uint16_t NO_LOCALIZED = 0xFFFF;

// AM824 8-channel 96 kHz stream format, wire order (MSB first).
[[nodiscard]] auto am824_8ch_96k() -> ieee::Eui64
{
    return ieee::Eui64{0x00, 0xA0, 0x04, 0x08, 0x60, 0x00, 0x08, 0x00};
}

// AAF 8-channel 96 kHz INT_32 stream format (IEEE 1722-2016 Clause 7.3.4).
// Verified byte-for-byte against a reference STREAM_INPUT descriptor:
//   byte0 subtype=0x02 (AAF)
//   byte1 = nsr in the LOW nibble: 0x07 = 96 kHz   (NOT the high nibble!)
//   byte2 = format = 0x02 (INT_32)                 (sample format, not channels)
//   byte3 = bit_depth = 0x20 (32)
//   bytes4-7 (32-bit BE) = channels_per_frame(8)<<22 | samples_per_frame(12)<<12
//                        = (8<<22)|(12<<12) = 0x0200C000 -> 02 00 c0 00
// The earlier code (02 70 08 20 00 00 00 00) put nsr in the high nibble and the
// channel count where the format byte goes, so compliant controllers
// decoded it as "AAF 0 kHz, 0 channels" and refused the format. The int-vs-float
// distinction also lives in the AAF PDU format field at run time (int_32bit).
[[nodiscard]] auto aaf_8ch_96k_32bit() -> ieee::Eui64
{
    return ieee::Eui64{0x02, 0x07, 0x02, 0x20, 0x02, 0x00, 0xC0, 0x00};
}

// CRF (Clock Reference Format) Milan 48 kHz audio-sample media-clock stream format
// (IEEE 1722-2016 §10; bit layout per la_avdecc StreamFormatInfo). Milan mandates a
// 48 kHz CRF media clock; both 48 kHz and 96 kHz clients lock to it (our 96 kHz
// audio rides as 2x this base). This is BYTE-FOR-BYTE the CRF stream-input format
// the DSP processor/the DSP processor advertises (0x041060010000bb80) -- a base match alone is not
// enough; the listener flags a mismatched interval/ts-per-PDU as UNSUPPORTED_FORMAT:
//   byte0 0x04        subtype=CRF, v=0
//   byte1 0x10        crf_type=1 (AudioSample), timestamp_interval[11:8]=0
//   byte2 0x60        timestamp_interval=96 (one timestamp per 96 events of the base)
//   byte3 0x01        timestamps_per_pdu=1  (=> 48000/96 = 500 AVTPDU/s, one per 2 ms)
//   byte4 0x00        pull=0 (x1.0), base_frequency[28:24]=0
//   byte5-7 00 bb 80  base_frequency=48000
[[nodiscard]] auto crf_audio_48k() -> ieee::Eui64
{
    return ieee::Eui64{0x04, 0x10, 0x60, 0x01, 0x00, 0x00, 0xBB, 0x80};
}

// AemEntityBlob — the DescriptorStorage (AEM1) blob writer — and its
// descriptor_wire_bytes helper now live in statusbar/tools/aem_entity_blob.hpp
// so they can be unit tested and reused.
using statusbar::tools::AemEntityBlob;

// --------------------------------------------------------------------------
// Model construction
// --------------------------------------------------------------------------

[[nodiscard]] auto build_bridge_model(std::string_view name, uint16_t channels, uint32_t sample_rate) -> std::vector<uint8_t>
{
    AemEntityBlob b;
    bool ok = true;  // accumulates every descriptor push; a false means inline overflow
    constexpr uint16_t CFG = 0;
    ieee::Eui64 const stream_format = am824_8ch_96k();

    // ENTITY — entity_id / model_id / name / firmware are overridden at runtime
    // by AvbEntityAm824IO from its config; capabilities + counts are honored.
    {
        DescriptorEntity d{};
        d.entity_name = AtdeccString{std::string{name}.c_str()};
        d.entity_capabilities = CAP_AEM | CAP_CLASS_A | CAP_GPTP;
        d.talker_stream_sources = 1;
        d.talker_capabilities = TALKER_IMPLEMENTED | TALKER_AUDIO_SOURCE;
        d.listener_stream_sinks = 1;
        d.listener_capabilities = LISTENER_IMPLEMENTED | LISTENER_AUDIO_SINK;
        d.configurations_count = 1;
        d.current_configuration = 0;
        b.add(CFG, d);
    }

    // CONFIGURATION — declares every descriptor type/count in this config.
    {
        DescriptorConfiguration d{};
        d.object_name = AtdeccString{"Bridge"};
        d.localized_description = NO_LOCALIZED;
        // In ascending descriptor-type order.
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_UNIT, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STREAM_INPUT, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STREAM_OUTPUT, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AVB_INTERFACE, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_CLOCK_SOURCE, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_LOCALE, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STRINGS, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STREAM_PORT_INPUT, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STREAM_PORT_OUTPUT, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_CLUSTER, .count = 2})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_MAP, .count = 2})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_CLOCK_DOMAIN, .count = 1})) && ok;
        b.add(CFG, d);
    }

    // AUDIO_UNIT — one stream-input port + one stream-output port; 96 kHz.
    {
        DescriptorAudioUnit d{};
        d.object_name = AtdeccString{"AudioUnit"};
        d.localized_description = NO_LOCALIZED;
        d.clock_domain_index = 0;
        d.number_of_stream_input_ports = 1;
        d.base_stream_input_port = 0;
        d.number_of_stream_output_ports = 1;
        d.base_stream_output_port = 0;
        d.current_sampling_rate = sample_rate;
        ok = (d.push_sampling_rate(sample_rate)) && ok;
        b.add(CFG, d);
    }

    // STREAM_INPUT (listener) + STREAM_OUTPUT (talker), 8-ch AM824 @ 96 kHz.
    {
        DescriptorStream d{};
        d.descriptor_type = DESCRIPTOR_STREAM_INPUT;
        d.object_name = AtdeccString{"StreamInput"};
        d.localized_description = NO_LOCALIZED;
        d.clock_domain_index = 0;
        d.stream_flags = STREAM_FLAG_CLASS_A;
        d.current_format = stream_format;
        ok = (d.push_stream_format(stream_format)) && ok;
        d.avb_interface_index = 0;
        b.add(CFG, d);
    }
    {
        DescriptorStream d{};
        d.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
        d.object_name = AtdeccString{"StreamOutput"};
        d.localized_description = NO_LOCALIZED;
        d.clock_domain_index = 0;
        d.stream_flags = STREAM_FLAG_CLASS_A;
        d.current_format = stream_format;
        ok = (d.push_stream_format(stream_format)) && ok;
        d.avb_interface_index = 0;
        b.add(CFG, d);
    }

    // STREAM_PORT_INPUT → cluster 0 / map 0; STREAM_PORT_OUTPUT → cluster 1 / map 1.
    {
        DescriptorStreamPort d{};
        d.descriptor_type = DESCRIPTOR_STREAM_PORT_INPUT;
        d.clock_domain_index = 0;
        d.number_of_clusters = 1;
        d.base_cluster = 0;
        d.number_of_maps = 1;
        d.base_map = 0;
        b.add(CFG, d);
    }
    {
        DescriptorStreamPort d{};
        d.descriptor_type = DESCRIPTOR_STREAM_PORT_OUTPUT;
        d.clock_domain_index = 0;
        d.number_of_clusters = 1;
        d.base_cluster = 1;
        d.number_of_maps = 1;
        d.base_map = 1;
        b.add(CFG, d);
    }

    // AUDIO_CLUSTER 0 (input) + 1 (output): one N-channel MBLA cluster each.
    // AvbEntityAm824IO derives its channel count from the first AUDIO_CLUSTER.
    for (uint16_t idx : {uint16_t{0}, uint16_t{1}}) {
        DescriptorAudioCluster d{};
        d.descriptor_index = idx;
        d.object_name = AtdeccString{idx == 0 ? "InputCluster" : "OutputCluster"};
        d.localized_description = NO_LOCALIZED;
        d.channel_count = channels;
        d.format = AUDIO_CLUSTER_FORMAT_MBLA;
        b.add(CFG, d);
    }

    // AUDIO_MAP 0 (input) + 1 (output): identity map stream ch i → cluster ch i.
    for (uint16_t idx : {uint16_t{0}, uint16_t{1}}) {
        DescriptorAudioMap d{};
        d.descriptor_index = idx;
        for (uint16_t ch = 0; ch < channels; ++ch) {
            AudioMapping m{};
            m.mapping_stream_index = 0;
            m.mapping_stream_channel = ch;
            m.mapping_cluster_offset = 0;
            m.mapping_cluster_channel = ch;
            ok = (d.push_mapping(m)) && ok;
        }
        b.add(CFG, d);
    }

    // AVB_INTERFACE — mac/clock_identity filled at runtime from the NIC + gPTP.
    {
        DescriptorAvbInterface d{};
        d.object_name = AtdeccString{"eth0"};
        d.localized_description = NO_LOCALIZED;
        d.interface_flags = AVB_INTERFACE_FLAG_GPTP_SUPPORTED;
        b.add(CFG, d);
    }

    // CLOCK_SOURCE — INTERNAL (gPTP-derived): the bridge is the media-clock
    // leader; downstream the audio interface slaves to our stream via its INPUT_STREAM source.
    {
        DescriptorClockSource d{};
        d.object_name = AtdeccString{"Internal"};
        d.localized_description = NO_LOCALIZED;
        d.clock_source_type = CLOCK_SOURCE_TYPE_INTERNAL;
        b.add(CFG, d);
    }

    // CLOCK_DOMAIN — one source (index 0), selected.
    {
        DescriptorClockDomain d{};
        d.object_name = AtdeccString{"ClockDomain"};
        d.localized_description = NO_LOCALIZED;
        d.clock_source_index = 0;
        ok = (d.push_clock_source(0)) && ok;
        b.add(CFG, d);
    }

    // LOCALE + STRINGS.
    {
        DescriptorLocale d{};
        d.locale_identifier = AtdeccString{"en"};
        d.number_of_strings = 1;
        d.base_strings = 0;
        b.add(CFG, d);
    }
    {
        DescriptorStrings d{};
        d.string_0 = AtdeccString{std::string{name}.c_str()};
        b.add(CFG, d);
    }

    if (!ok) {
        return {};  // a descriptor push overflowed its inline capacity → malformed blob
    }
    return b.build();
}

// --------------------------------------------------------------------------
// Dual-format model: two stream inputs + two stream outputs.
//   stream 0 = AM824 (IEC 61883-6), stream 1 = AAF (32-bit PCM), both N-ch.
// Consumed by AvbEntityAudioIO (one AM824 pair + one AAF pair).
// --------------------------------------------------------------------------
[[nodiscard]] auto build_audio_model(std::string_view name, uint16_t channels, uint32_t sample_rate) -> std::vector<uint8_t>
{
    AemEntityBlob b;
    bool ok = true;  // accumulates every descriptor push; a false means inline overflow
    constexpr uint16_t CFG = 0;
    ieee::Eui64 const am824_format = am824_8ch_96k();
    ieee::Eui64 const aaf_format = aaf_8ch_96k_32bit();

    // Per-stream descriptor wiring: index, type-specific format, and the
    // STREAM_PORT → cluster/map base indices (inputs use cluster/map 0,1;
    // outputs use 2,3).
    struct StreamSpec
    {
        uint16_t index;
        ieee::Eui64 format;
    };
    std::array<StreamSpec, 2> const streams{{{.index = 0, .format = am824_format}, {.index = 1, .format = aaf_format}}};

    // ENTITY — two talker sources + two listener sinks.
    {
        DescriptorEntity d{};
        d.entity_name = AtdeccString{std::string{name}.c_str()};
        d.entity_capabilities = CAP_AEM | CAP_CLASS_A | CAP_GPTP;
        d.talker_stream_sources = 3;  // AM824, AAF, CRF (media clock)
        d.talker_capabilities = TALKER_IMPLEMENTED | TALKER_AUDIO_SOURCE | TALKER_MEDIA_CLOCK_SOURCE;
        d.listener_stream_sinks = 2;
        d.listener_capabilities = LISTENER_IMPLEMENTED | LISTENER_AUDIO_SINK;
        d.configurations_count = 1;
        d.current_configuration = 0;
        b.add(CFG, d);
    }

    // CONFIGURATION — counts for the dual-stream model.
    {
        DescriptorConfiguration d{};
        d.object_name = AtdeccString{"AudioIO"};
        d.localized_description = NO_LOCALIZED;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_UNIT, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STREAM_INPUT, .count = 2})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STREAM_OUTPUT, .count = 3})) && ok;  // +CRF
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_JACK_INPUT, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_JACK_OUTPUT, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AVB_INTERFACE, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_CLOCK_SOURCE, .count = 2})) && ok;  // INTERNAL + INPUT_STREAM
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_LOCALE, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STRINGS, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STREAM_PORT_INPUT, .count = 2})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STREAM_PORT_OUTPUT, .count = 2})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_CLUSTER, .count = 4})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_MAP, .count = 4})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_CLOCK_DOMAIN, .count = 1})) && ok;
        b.add(CFG, d);
    }

    // AUDIO_UNIT — two stream-input ports + two stream-output ports.
    {
        DescriptorAudioUnit d{};
        d.object_name = AtdeccString{"AudioUnit"};
        d.localized_description = NO_LOCALIZED;
        d.clock_domain_index = 0;
        d.number_of_stream_input_ports = 2;
        d.base_stream_input_port = 0;
        d.number_of_stream_output_ports = 2;
        d.base_stream_output_port = 0;
        d.current_sampling_rate = sample_rate;
        ok = (d.push_sampling_rate(sample_rate)) && ok;
        b.add(CFG, d);
    }

    // STREAM_INPUT 0/1 + STREAM_OUTPUT 0/1 (index 0 = AM824, 1 = AAF).
    for (auto const& s : streams) {
        DescriptorStream d{};
        d.descriptor_type = DESCRIPTOR_STREAM_INPUT;
        d.descriptor_index = s.index;
        d.object_name = AtdeccString{s.index == 0 ? "StreamInputAM824" : "StreamInputAAF"};
        d.localized_description = NO_LOCALIZED;
        d.clock_domain_index = 0;
        d.stream_flags = STREAM_FLAG_CLASS_A;
        d.current_format = s.format;
        ok = (d.push_stream_format(s.format)) && ok;
        d.avb_interface_index = 0;
        b.add(CFG, d);
    }
    for (auto const& s : streams) {
        DescriptorStream d{};
        d.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
        d.descriptor_index = s.index;
        d.object_name = AtdeccString{s.index == 0 ? "StreamOutputAM824" : "StreamOutputAAF"};
        d.localized_description = NO_LOCALIZED;
        d.clock_domain_index = 0;
        d.stream_flags = STREAM_FLAG_CLASS_A;
        d.current_format = s.format;
        ok = (d.push_stream_format(s.format)) && ok;
        d.avb_interface_index = 0;
        b.add(CFG, d);
    }

    // STREAM_OUTPUT 2 — CRF (Clock Reference Format) media-clock stream. A clock
    // stream carries no audio, so it has no STREAM_PORT / AUDIO_CLUSTER / MAP; it
    // just sources the clock domain. Milan listeners can lock their media clock
    // to it.
    {
        DescriptorStream d{};
        d.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
        d.descriptor_index = 2;
        d.object_name = AtdeccString{"StreamOutputCRF"};
        d.localized_description = NO_LOCALIZED;
        d.clock_domain_index = 0;
        d.stream_flags = STREAM_FLAG_CLASS_A;
        d.current_format = crf_audio_48k();
        ok = (d.push_stream_format(crf_audio_48k())) && ok;
        d.avb_interface_index = 0;
        b.add(CFG, d);
    }

    // STREAM_PORT_INPUT 0→cluster0/map0, 1→cluster1/map1;
    // STREAM_PORT_OUTPUT 0→cluster2/map2, 1→cluster3/map3.
    for (uint16_t idx : {uint16_t{0}, uint16_t{1}}) {
        DescriptorStreamPort d{};
        d.descriptor_type = DESCRIPTOR_STREAM_PORT_INPUT;
        d.descriptor_index = idx;
        d.clock_domain_index = 0;
        d.number_of_clusters = 1;
        d.base_cluster = idx;
        d.number_of_maps = 1;
        d.base_map = idx;
        b.add(CFG, d);
    }
    for (uint16_t idx : {uint16_t{0}, uint16_t{1}}) {
        DescriptorStreamPort d{};
        d.descriptor_type = DESCRIPTOR_STREAM_PORT_OUTPUT;
        d.descriptor_index = idx;
        d.clock_domain_index = 0;
        d.number_of_clusters = 1;
        d.base_cluster = static_cast<uint16_t>(2 + idx);
        d.number_of_maps = 1;
        d.base_map = static_cast<uint16_t>(2 + idx);
        b.add(CFG, d);
    }

    // AUDIO_CLUSTER 0..3 (one N-channel MBLA cluster per stream port).
    // AvbEntityAudioIO derives its channel count from AUDIO_CLUSTER 0.
    for (uint16_t idx = 0; idx < 4; ++idx) {
        DescriptorAudioCluster d{};
        d.descriptor_index = idx;
        d.object_name = AtdeccString{idx < 2 ? "InputCluster" : "OutputCluster"};
        d.localized_description = NO_LOCALIZED;
        d.channel_count = channels;
        d.format = AUDIO_CLUSTER_FORMAT_MBLA;
        b.add(CFG, d);
    }

    // AUDIO_MAP 0..3 — identity map stream ch i → cluster ch i.
    for (uint16_t idx = 0; idx < 4; ++idx) {
        DescriptorAudioMap d{};
        d.descriptor_index = idx;
        for (uint16_t ch = 0; ch < channels; ++ch) {
            AudioMapping m{};
            m.mapping_stream_index = 0;
            m.mapping_stream_channel = ch;
            m.mapping_cluster_offset = 0;
            m.mapping_cluster_channel = ch;
            ok = (d.push_mapping(m)) && ok;
        }
        b.add(CFG, d);
    }

    // AVB_INTERFACE.
    {
        DescriptorAvbInterface d{};
        d.object_name = AtdeccString{"eth0"};
        d.localized_description = NO_LOCALIZED;
        d.interface_flags = AVB_INTERFACE_FLAG_GPTP_SUPPORTED;
        b.add(CFG, d);
    }

    // JACK_INPUT 0 + JACK_OUTPUT 0 — a single N-channel digital jack each side.
    // Optional in 1722.1, but some controllers expect physical jacks to render
    // the routing UI; their absence reads as an incomplete model.
    {
        DescriptorJack d{};
        d.descriptor_type = DESCRIPTOR_JACK_INPUT;
        d.descriptor_index = 0;
        d.object_name = AtdeccString{"JackInput"};
        d.localized_description = NO_LOCALIZED;
        d.jack_type = JACK_TYPE_DIGITAL;
        b.add(CFG, d);
    }
    {
        DescriptorJack d{};
        d.descriptor_type = DESCRIPTOR_JACK_OUTPUT;
        d.descriptor_index = 0;
        d.object_name = AtdeccString{"JackOutput"};
        d.localized_description = NO_LOCALIZED;
        d.jack_type = JACK_TYPE_DIGITAL;
        b.add(CFG, d);
    }

    // CLOCK_SOURCE 0 (INTERNAL, located on the AUDIO_UNIT) + 1 (INPUT_STREAM,
    // located on STREAM_INPUT 0 so a controller can select 'lock to the incoming
    // stream'), then a CLOCK_DOMAIN listing both with INTERNAL current.
    {
        DescriptorClockSource d{};
        d.descriptor_index = 0;
        d.object_name = AtdeccString{"Internal"};
        d.localized_description = NO_LOCALIZED;
        d.clock_source_type = CLOCK_SOURCE_TYPE_INTERNAL;
        d.clock_source_location_type = DESCRIPTOR_AUDIO_UNIT;
        d.clock_source_location_index = 0;
        b.add(CFG, d);
    }
    {
        DescriptorClockSource d{};
        d.descriptor_index = 1;
        d.object_name = AtdeccString{"InputStream"};
        d.localized_description = NO_LOCALIZED;
        d.clock_source_type = CLOCK_SOURCE_TYPE_INPUT_STREAM;
        d.clock_source_location_type = DESCRIPTOR_STREAM_INPUT;
        d.clock_source_location_index = 0;
        b.add(CFG, d);
    }
    {
        DescriptorClockDomain d{};
        d.object_name = AtdeccString{"ClockDomain"};
        d.localized_description = NO_LOCALIZED;
        d.clock_source_index = 0;  // current = INTERNAL
        ok = (d.push_clock_source(0)) && ok;
        ok = (d.push_clock_source(1)) && ok;
        b.add(CFG, d);
    }

    // LOCALE + STRINGS.
    {
        DescriptorLocale d{};
        d.locale_identifier = AtdeccString{"en"};
        d.number_of_strings = 1;
        d.base_strings = 0;
        b.add(CFG, d);
    }
    {
        DescriptorStrings d{};
        d.string_0 = AtdeccString{std::string{name}.c_str()};
        b.add(CFG, d);
    }

    if (!ok) {
        return {};  // a descriptor push overflowed its inline capacity → malformed blob
    }
    return b.build();
}

// --------------------------------------------------------------------------
// Tone-generator model: talker-only, three stream OUTPUTS, no inputs.
//   stream 0 = AM824 (IEC 61883-6), stream 1 = AAF (32-bit PCM), both N-ch;
//   stream 2 = CRF (Milan 48 kHz media clock). No STREAM_INPUT / listener.
// Consumed by AvbEntityToneGenerator. The entity derives its channel count
// from AUDIO_CLUSTER 0 (here the first OUTPUT cluster).
// --------------------------------------------------------------------------
[[nodiscard]] auto build_tone_generator_model(std::string_view name, uint16_t channels, uint32_t sample_rate)
    -> std::vector<uint8_t>
{
    AemEntityBlob b;
    bool ok = true;  // accumulates every descriptor push; a false means inline overflow
    constexpr uint16_t CFG = 0;

    struct StreamSpec
    {
        uint16_t index;
        ieee::Eui64 format;
        char const* name;
    };
    std::array<StreamSpec, 2> const audio_streams{
        {{.index = 0, .format = am824_8ch_96k(), .name = "StreamOutputAM824"},
         {.index = 1, .format = aaf_8ch_96k_32bit(), .name = "StreamOutputAAF"}}};

    // ENTITY — three talker sources (AM824, AAF, CRF), no listener sinks.
    {
        DescriptorEntity d{};
        d.entity_name = AtdeccString{std::string{name}.c_str()};
        d.entity_capabilities = CAP_AEM | CAP_CLASS_A | CAP_GPTP;
        d.talker_stream_sources = 3;  // AM824, AAF, CRF (media clock)
        d.talker_capabilities = TALKER_IMPLEMENTED | TALKER_AUDIO_SOURCE | TALKER_MEDIA_CLOCK_SOURCE;
        d.listener_stream_sinks = 0;
        d.listener_capabilities = 0;
        d.configurations_count = 1;
        d.current_configuration = 0;
        b.add(CFG, d);
    }

    // CONFIGURATION — counts for the talker-only tone-generator model.
    {
        DescriptorConfiguration d{};
        d.object_name = AtdeccString{"ToneGen"};
        d.localized_description = NO_LOCALIZED;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_UNIT, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STREAM_OUTPUT, .count = 3})) && ok;  // AM824 + AAF + CRF
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AVB_INTERFACE, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_CLOCK_SOURCE, .count = 1})) && ok;  // INTERNAL only
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_LOCALE, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STRINGS, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STREAM_PORT_OUTPUT, .count = 2})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_CLUSTER, .count = 2})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_MAP, .count = 2})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_CLOCK_DOMAIN, .count = 1})) && ok;
        b.add(CFG, d);
    }

    // AUDIO_UNIT — no stream-input ports; two stream-output ports (AM824, AAF).
    {
        DescriptorAudioUnit d{};
        d.object_name = AtdeccString{"AudioUnit"};
        d.localized_description = NO_LOCALIZED;
        d.clock_domain_index = 0;
        d.number_of_stream_input_ports = 0;
        d.base_stream_input_port = 0;
        d.number_of_stream_output_ports = 2;
        d.base_stream_output_port = 0;
        d.current_sampling_rate = sample_rate;
        ok = (d.push_sampling_rate(sample_rate)) && ok;
        b.add(CFG, d);
    }

    // STREAM_OUTPUT 0 = AM824, 1 = AAF (carry the 8 tone channels).
    for (auto const& s : audio_streams) {
        DescriptorStream d{};
        d.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
        d.descriptor_index = s.index;
        d.object_name = AtdeccString{s.name};
        d.localized_description = NO_LOCALIZED;
        d.clock_domain_index = 0;
        d.stream_flags = STREAM_FLAG_CLASS_A;
        d.current_format = s.format;
        ok = (d.push_stream_format(s.format)) && ok;
        d.avb_interface_index = 0;
        b.add(CFG, d);
    }

    // STREAM_OUTPUT 2 — CRF media-clock stream (no audio → no port/cluster/map).
    {
        DescriptorStream d{};
        d.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
        d.descriptor_index = 2;
        d.object_name = AtdeccString{"StreamOutputCRF"};
        d.localized_description = NO_LOCALIZED;
        d.clock_domain_index = 0;
        d.stream_flags = STREAM_FLAG_CLASS_A;
        d.current_format = crf_audio_48k();
        ok = (d.push_stream_format(crf_audio_48k())) && ok;
        d.avb_interface_index = 0;
        b.add(CFG, d);
    }

    // STREAM_PORT_OUTPUT 0→cluster0/map0 (AM824), 1→cluster1/map1 (AAF).
    for (uint16_t idx : {uint16_t{0}, uint16_t{1}}) {
        DescriptorStreamPort d{};
        d.descriptor_type = DESCRIPTOR_STREAM_PORT_OUTPUT;
        d.descriptor_index = idx;
        d.clock_domain_index = 0;
        d.number_of_clusters = 1;
        d.base_cluster = idx;
        d.number_of_maps = 1;
        d.base_map = idx;
        b.add(CFG, d);
    }

    // AUDIO_CLUSTER 0/1 — one N-channel MBLA cluster per output port.
    // AvbEntityToneGenerator derives its channel count from AUDIO_CLUSTER 0.
    for (uint16_t idx : {uint16_t{0}, uint16_t{1}}) {
        DescriptorAudioCluster d{};
        d.descriptor_index = idx;
        d.object_name = AtdeccString{"OutputCluster"};
        d.localized_description = NO_LOCALIZED;
        d.channel_count = channels;
        d.format = AUDIO_CLUSTER_FORMAT_MBLA;
        b.add(CFG, d);
    }

    // AUDIO_MAP 0/1 — identity map stream ch i → cluster ch i.
    for (uint16_t idx : {uint16_t{0}, uint16_t{1}}) {
        DescriptorAudioMap d{};
        d.descriptor_index = idx;
        for (uint16_t ch = 0; ch < channels; ++ch) {
            AudioMapping m{};
            m.mapping_stream_index = 0;
            m.mapping_stream_channel = ch;
            m.mapping_cluster_offset = 0;
            m.mapping_cluster_channel = ch;
            ok = (d.push_mapping(m)) && ok;
        }
        b.add(CFG, d);
    }

    // AVB_INTERFACE.
    {
        DescriptorAvbInterface d{};
        d.object_name = AtdeccString{"eth0"};
        d.localized_description = NO_LOCALIZED;
        d.interface_flags = AVB_INTERFACE_FLAG_GPTP_SUPPORTED;
        b.add(CFG, d);
    }

    // CLOCK_SOURCE 0 — INTERNAL (gPTP-derived): the tone generator is the
    // media-clock leader; there is no input stream to lock to.
    {
        DescriptorClockSource d{};
        d.descriptor_index = 0;
        d.object_name = AtdeccString{"Internal"};
        d.localized_description = NO_LOCALIZED;
        d.clock_source_type = CLOCK_SOURCE_TYPE_INTERNAL;
        d.clock_source_location_type = DESCRIPTOR_AUDIO_UNIT;
        d.clock_source_location_index = 0;
        b.add(CFG, d);
    }

    // CLOCK_DOMAIN — one source (INTERNAL), selected.
    {
        DescriptorClockDomain d{};
        d.object_name = AtdeccString{"ClockDomain"};
        d.localized_description = NO_LOCALIZED;
        d.clock_source_index = 0;
        ok = (d.push_clock_source(0)) && ok;
        b.add(CFG, d);
    }

    // LOCALE + STRINGS.
    {
        DescriptorLocale d{};
        d.locale_identifier = AtdeccString{"en"};
        d.number_of_strings = 1;
        d.base_strings = 0;
        b.add(CFG, d);
    }
    {
        DescriptorStrings d{};
        d.string_0 = AtdeccString{std::string{name}.c_str()};
        b.add(CFG, d);
    }

    if (!ok) {
        return {};  // a descriptor push overflowed its inline capacity → malformed blob
    }
    return b.build();
}

// --------------------------------------------------------------------------
// AAF-only tone-generator model: talker-only, a SINGLE AAF stream output.
//   stream 0 = AAF (32-bit PCM), N-ch. No AM824, no CRF, no inputs.
// A clean N-channel AAF device for a listener that stalls on the mixed
// AM824+AAF aggregate (e.g. macOS). Consumed by AvbEntityToneGenerator in
// AafOnly mode. The entity derives its channel count from AUDIO_CLUSTER 0.
// --------------------------------------------------------------------------
[[nodiscard]] auto build_tone_aaf_model(std::string_view name, uint16_t channels, uint32_t sample_rate) -> std::vector<uint8_t>
{
    AemEntityBlob b;
    bool ok = true;
    constexpr uint16_t CFG = 0;

    // ENTITY — one talker source (AAF), no listener sinks.
    {
        DescriptorEntity d{};
        d.entity_name = AtdeccString{std::string{name}.c_str()};
        d.entity_capabilities = CAP_AEM | CAP_CLASS_A | CAP_GPTP;
        d.talker_stream_sources = 1;  // AAF only
        d.talker_capabilities = TALKER_IMPLEMENTED | TALKER_AUDIO_SOURCE;
        d.listener_stream_sinks = 0;
        d.listener_capabilities = 0;
        d.configurations_count = 1;
        d.current_configuration = 0;
        b.add(CFG, d);
    }

    // CONFIGURATION.
    {
        DescriptorConfiguration d{};
        d.object_name = AtdeccString{"ToneAAF"};
        d.localized_description = NO_LOCALIZED;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_UNIT, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STREAM_OUTPUT, .count = 1})) && ok;  // AAF only
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AVB_INTERFACE, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_CLOCK_SOURCE, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_LOCALE, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STRINGS, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STREAM_PORT_OUTPUT, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_CLUSTER, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_MAP, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_CLOCK_DOMAIN, .count = 1})) && ok;
        b.add(CFG, d);
    }

    // AUDIO_UNIT — no input ports; one output port (AAF).
    {
        DescriptorAudioUnit d{};
        d.object_name = AtdeccString{"AudioUnit"};
        d.localized_description = NO_LOCALIZED;
        d.clock_domain_index = 0;
        d.number_of_stream_input_ports = 0;
        d.base_stream_input_port = 0;
        d.number_of_stream_output_ports = 1;
        d.base_stream_output_port = 0;
        d.current_sampling_rate = sample_rate;
        ok = (d.push_sampling_rate(sample_rate)) && ok;
        b.add(CFG, d);
    }

    // STREAM_OUTPUT 0 = AAF.
    {
        DescriptorStream d{};
        d.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
        d.descriptor_index = 0;
        d.object_name = AtdeccString{"StreamOutputAAF"};
        d.localized_description = NO_LOCALIZED;
        d.clock_domain_index = 0;
        d.stream_flags = STREAM_FLAG_CLASS_A;
        d.current_format = aaf_8ch_96k_32bit();
        ok = (d.push_stream_format(aaf_8ch_96k_32bit())) && ok;
        d.avb_interface_index = 0;
        b.add(CFG, d);
    }

    // STREAM_PORT_OUTPUT 0 → cluster 0 / map 0.
    {
        DescriptorStreamPort d{};
        d.descriptor_type = DESCRIPTOR_STREAM_PORT_OUTPUT;
        d.descriptor_index = 0;
        d.clock_domain_index = 0;
        d.number_of_clusters = 1;
        d.base_cluster = 0;
        d.number_of_maps = 1;
        d.base_map = 0;
        b.add(CFG, d);
    }

    // AUDIO_CLUSTER 0 — N-channel MBLA. Channel count source for the entity.
    {
        DescriptorAudioCluster d{};
        d.descriptor_index = 0;
        d.object_name = AtdeccString{"OutputCluster"};
        d.localized_description = NO_LOCALIZED;
        d.channel_count = channels;
        d.format = AUDIO_CLUSTER_FORMAT_MBLA;
        b.add(CFG, d);
    }

    // AUDIO_MAP 0 — identity map.
    {
        DescriptorAudioMap d{};
        d.descriptor_index = 0;
        for (uint16_t ch = 0; ch < channels; ++ch) {
            AudioMapping m{};
            m.mapping_stream_index = 0;
            m.mapping_stream_channel = ch;
            m.mapping_cluster_offset = 0;
            m.mapping_cluster_channel = ch;
            ok = (d.push_mapping(m)) && ok;
        }
        b.add(CFG, d);
    }

    // AVB_INTERFACE.
    {
        DescriptorAvbInterface d{};
        d.object_name = AtdeccString{"eth0"};
        d.localized_description = NO_LOCALIZED;
        d.interface_flags = AVB_INTERFACE_FLAG_GPTP_SUPPORTED;
        b.add(CFG, d);
    }

    // CLOCK_SOURCE 0 — INTERNAL (gPTP-derived).
    {
        DescriptorClockSource d{};
        d.descriptor_index = 0;
        d.object_name = AtdeccString{"Internal"};
        d.localized_description = NO_LOCALIZED;
        d.clock_source_type = CLOCK_SOURCE_TYPE_INTERNAL;
        d.clock_source_location_type = DESCRIPTOR_AUDIO_UNIT;
        d.clock_source_location_index = 0;
        b.add(CFG, d);
    }

    // CLOCK_DOMAIN.
    {
        DescriptorClockDomain d{};
        d.object_name = AtdeccString{"ClockDomain"};
        d.localized_description = NO_LOCALIZED;
        d.clock_source_index = 0;
        ok = (d.push_clock_source(0)) && ok;
        b.add(CFG, d);
    }

    // LOCALE + STRINGS.
    {
        DescriptorLocale d{};
        d.locale_identifier = AtdeccString{"en"};
        d.number_of_strings = 1;
        d.base_strings = 0;
        b.add(CFG, d);
    }
    {
        DescriptorStrings d{};
        d.string_0 = AtdeccString{std::string{name}.c_str()};
        b.add(CFG, d);
    }

    if (!ok) {
        return {};
    }
    return b.build();
}

// --------------------------------------------------------------------------
// AAF + CRF tone-generator model: talker-only, AAF audio (idx 0) + CRF media
// clock (idx 1). No AM824 -> a clean N-channel AAF device, but WITH a CRF
// media-clock stream a listener (e.g. macOS) can recover its clock from.
// Consumed by AvbEntityToneGenerator in AafCrf mode.
// --------------------------------------------------------------------------
[[nodiscard]] auto build_tone_aaf_crf_model(std::string_view name, uint16_t channels, uint32_t sample_rate) -> std::vector<uint8_t>
{
    AemEntityBlob b;
    bool ok = true;
    constexpr uint16_t CFG = 0;

    // ENTITY — two talker sources (AAF + CRF media clock), no listener sinks.
    {
        DescriptorEntity d{};
        d.entity_name = AtdeccString{std::string{name}.c_str()};
        d.entity_capabilities = CAP_AEM | CAP_CLASS_A | CAP_GPTP;
        d.talker_stream_sources = 2;  // AAF + CRF
        d.talker_capabilities = TALKER_IMPLEMENTED | TALKER_AUDIO_SOURCE | TALKER_MEDIA_CLOCK_SOURCE;
        d.listener_stream_sinks = 0;
        d.listener_capabilities = 0;
        d.configurations_count = 1;
        d.current_configuration = 0;
        b.add(CFG, d);
    }

    // CONFIGURATION.
    {
        DescriptorConfiguration d{};
        d.object_name = AtdeccString{"ToneAAFCRF"};
        d.localized_description = NO_LOCALIZED;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_UNIT, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STREAM_OUTPUT, .count = 2})) && ok;  // AAF + CRF
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AVB_INTERFACE, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_CLOCK_SOURCE, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_LOCALE, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STRINGS, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_STREAM_PORT_OUTPUT, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_CLUSTER, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_MAP, .count = 1})) && ok;
        ok = (d.push_descriptor_count({.descriptor_type = DESCRIPTOR_CLOCK_DOMAIN, .count = 1})) && ok;
        b.add(CFG, d);
    }

    // AUDIO_UNIT — one output port (AAF). Localized strings (STRINGS desc 0)
    // name the clock/stream objects so a controller (macOS Audio MIDI Setup)
    // can label + select them; string indices: 1=Internal 2=ClockDomain
    // 3=AAF Audio 4=CRF Media Clock 5=Audio Unit 6=Output.
    {
        DescriptorAudioUnit d{};
        d.object_name = AtdeccString{"AudioUnit"};
        d.localized_description = 5;
        d.clock_domain_index = 0;
        d.number_of_stream_input_ports = 0;
        d.base_stream_input_port = 0;
        d.number_of_stream_output_ports = 1;
        d.base_stream_output_port = 0;
        d.current_sampling_rate = sample_rate;
        ok = (d.push_sampling_rate(sample_rate)) && ok;
        b.add(CFG, d);
    }

    // STREAM_OUTPUT 0 = AAF (audio).
    {
        DescriptorStream d{};
        d.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
        d.descriptor_index = 0;
        d.object_name = AtdeccString{"StreamOutputAAF"};
        d.localized_description = 3;
        d.clock_domain_index = 0;
        d.stream_flags = STREAM_FLAG_CLASS_A;
        d.current_format = aaf_8ch_96k_32bit();
        ok = (d.push_stream_format(aaf_8ch_96k_32bit())) && ok;
        d.avb_interface_index = 0;
        b.add(CFG, d);
    }

    // STREAM_OUTPUT 1 = CRF media clock (no audio -> no port/cluster/map).
    {
        DescriptorStream d{};
        d.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
        d.descriptor_index = 1;
        d.object_name = AtdeccString{"StreamOutputCRF"};
        d.localized_description = 4;
        d.clock_domain_index = 0;
        d.stream_flags = STREAM_FLAG_CLASS_A;
        d.current_format = crf_audio_48k();
        ok = (d.push_stream_format(crf_audio_48k())) && ok;
        d.avb_interface_index = 0;
        b.add(CFG, d);
    }

    // STREAM_PORT_OUTPUT 0 → cluster 0 / map 0 (AAF audio).
    {
        DescriptorStreamPort d{};
        d.descriptor_type = DESCRIPTOR_STREAM_PORT_OUTPUT;
        d.descriptor_index = 0;
        d.clock_domain_index = 0;
        d.number_of_clusters = 1;
        d.base_cluster = 0;
        d.number_of_maps = 1;
        d.base_map = 0;
        b.add(CFG, d);
    }

    // AUDIO_CLUSTER 0 — N-channel MBLA. Channel-count source for the entity.
    {
        DescriptorAudioCluster d{};
        d.descriptor_index = 0;
        d.object_name = AtdeccString{"OutputCluster"};
        d.localized_description = 6;
        d.channel_count = channels;
        d.format = AUDIO_CLUSTER_FORMAT_MBLA;
        b.add(CFG, d);
    }

    // AUDIO_MAP 0 — identity map.
    {
        DescriptorAudioMap d{};
        d.descriptor_index = 0;
        for (uint16_t ch = 0; ch < channels; ++ch) {
            AudioMapping m{};
            m.mapping_stream_index = 0;
            m.mapping_stream_channel = ch;
            m.mapping_cluster_offset = 0;
            m.mapping_cluster_channel = ch;
            ok = (d.push_mapping(m)) && ok;
        }
        b.add(CFG, d);
    }

    // AVB_INTERFACE.
    {
        DescriptorAvbInterface d{};
        d.object_name = AtdeccString{"eth0"};
        d.localized_description = NO_LOCALIZED;
        d.interface_flags = AVB_INTERFACE_FLAG_GPTP_SUPPORTED;
        b.add(CFG, d);
    }

    // CLOCK_SOURCE 0 — INTERNAL (gPTP-derived); distributed via the CRF stream.
    // localized name (string 1) so a controller labels it "Internal" -- distinct
    // from the CRF stream a listener may pick as its recovered media clock.
    {
        DescriptorClockSource d{};
        d.descriptor_index = 0;
        d.object_name = AtdeccString{"Internal"};
        d.localized_description = 1;
        d.clock_source_type = CLOCK_SOURCE_TYPE_INTERNAL;
        d.clock_source_location_type = DESCRIPTOR_AUDIO_UNIT;
        d.clock_source_location_index = 0;
        b.add(CFG, d);
    }

    // CLOCK_DOMAIN.
    {
        DescriptorClockDomain d{};
        d.object_name = AtdeccString{"ClockDomain"};
        d.localized_description = 2;
        d.clock_source_index = 0;
        ok = (d.push_clock_source(0)) && ok;
        b.add(CFG, d);
    }

    // LOCALE + STRINGS (one STRINGS descriptor holding 7 names: 0=entity,
    // 1=Internal, 2=ClockDomain, 3=AAF Audio, 4=CRF Media Clock, 5=Audio Unit,
    // 6=Output). number_of_strings is the count of STRINGS DESCRIPTORS (1), not
    // the number of strings -- setting it to 7 makes a controller read STRINGS
    // descriptors 1..6 (which don't exist) and abort enumeration.
    {
        DescriptorLocale d{};
        d.locale_identifier = AtdeccString{"en"};
        d.number_of_strings = 1;
        d.base_strings = 0;
        b.add(CFG, d);
    }
    {
        DescriptorStrings d{};
        d.string_0 = AtdeccString{std::string{name}.c_str()};
        d.string_1 = AtdeccString{"Internal"};
        d.string_2 = AtdeccString{"Clock Domain"};
        d.string_3 = AtdeccString{"AAF Audio"};
        d.string_4 = AtdeccString{"CRF Media Clock"};
        d.string_5 = AtdeccString{"Audio Unit"};
        d.string_6 = AtdeccString{"Output"};
        b.add(CFG, d);
    }

    if (!ok) {
        return {};
    }
    return b.build();
}

struct Config
{
    std::string out = "entity.bin";
    std::string name = "Statusbar AVB Bridge";
    uint16_t channels = 8;
    uint32_t sample_rate = 96000;
    bool dual = false;
    bool tone = false;
    bool tone_aaf = false;
    bool tone_aaf_crf = false;
};

auto build_arg_specs(Config& c) -> args::ArgumentSpecs
{
    args::ArgumentSpecs specs;
    specs.add_file("out", "Output blob path", c.out, [&](auto v) { c.out = std::string{v}; });
    specs.add<std::string>("name", "Entity name", c.name, [&](auto v) { c.name = v; });
    specs.add<uint16_t>("channels", "Audio channels per cluster", c.channels, [&](auto v) { c.channels = v; });
    specs.add<uint32_t>("sample-rate", "Sample rate in Hz", c.sample_rate, [&](auto v) { c.sample_rate = v; });
    specs.add_flag(
        "dual", "Emit a dual-format model: 2 stream inputs + 3 stream outputs (stream 0 AM824, 1 AAF, 2 CRF)", [&](auto v) {
            c.dual = v;
        });
    specs.add_flag(
        "tone",
        "Emit a talker-only tone-generator model: 0 stream inputs + 3 stream outputs (stream 0 AM824, 1 AAF, 2 CRF)",
        [&](auto v) { c.tone = v; });
    specs.add_flag(
        "tone-aaf",
        "Emit a talker-only AAF-ONLY tone-generator model: 0 stream inputs + 1 stream output (stream 0 AAF int32)",
        [&](auto v) { c.tone_aaf = v; });
    specs.add_flag(
        "tone-aaf-crf",
        "Emit a talker-only AAF+CRF tone-generator model: 0 stream inputs + 2 stream outputs (stream 0 AAF int32, 1 CRF)",
        [&](auto v) { c.tone_aaf_crf = v; });
    return specs;
}

}  // namespace

int main(int argc, char** argv)
{
    Config cfg;
    auto specs = build_arg_specs(cfg);
    // No custom usage needed — the generic renderer covers it.
    auto const cli_result = config::parse_cli_args(argc, argv, specs, config::default_print_usage, "statusbar-aem-entity-blob");
    if (!cli_result) {
        return config::handled_builtin_command(cli_result) ? 0 : 1;
    }

    auto const out = cfg.out;
    auto const dual = cfg.dual;
    auto const tone = cfg.tone;
    auto const tone_aaf = cfg.tone_aaf;
    auto const tone_aaf_crf = cfg.tone_aaf_crf;
    auto const channels = cfg.channels;
    auto const sample_rate = cfg.sample_rate;
    if (static_cast<int>(dual) + static_cast<int>(tone) + static_cast<int>(tone_aaf) + static_cast<int>(tone_aaf_crf) > 1) {
        std::println(stderr, "error: --dual, --tone, --tone-aaf and --tone-aaf-crf are mutually exclusive");
        return 1;
    }
    auto const blob = tone_aaf_crf ? build_tone_aaf_crf_model(cfg.name, channels, sample_rate)
        : tone_aaf                 ? build_tone_aaf_model(cfg.name, channels, sample_rate)
        : tone                     ? build_tone_generator_model(cfg.name, channels, sample_rate)
        : dual                     ? build_audio_model(cfg.name, channels, sample_rate)
                                   : build_bridge_model(cfg.name, channels, sample_rate);

    if (blob.empty()) {
        std::println(stderr, "error: descriptor model overflowed an inline buffer (a push failed); blob not generated");
        return 1;
    }

    // Round-trip validate before writing — fail loudly if the blob is malformed.
    auto storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{blob});
    if (!storage) {
        std::println(stderr, "error: generated blob failed DescriptorStorage::create: {}", storage.error().message());
        return 1;
    }

    std::ofstream f{out, std::ios::binary};
    if (!f) {
        std::println(stderr, "error: cannot open {} for writing", out);
        return 1;
    }
    f.write(reinterpret_cast<char const*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    if (tone_aaf_crf) {
        std::println(
            "wrote {} ({} bytes): {}-ch AAF+CRF tone-generator @ {} Hz — 0 listeners + 2 talkers (stream 0 AAF int32, 1 CRF)",
            out,
            blob.size(),
            channels,
            sample_rate);
    } else if (tone_aaf) {
        std::println(
            "wrote {} ({} bytes): {}-ch AAF-only tone-generator @ {} Hz — 0 listeners + 1 talker (stream 0 AAF int32)",
            out,
            blob.size(),
            channels,
            sample_rate);
    } else if (tone) {
        std::println(
            "wrote {} ({} bytes): {}-ch tone-generator @ {} Hz — 0 listeners + 3 talkers "
            "(stream 0 AM824, stream 1 AAF int32, stream 2 CRF media clock)",
            out,
            blob.size(),
            channels,
            sample_rate);
    } else if (dual) {
        std::println(
            "wrote {} ({} bytes): {}-ch dual-format @ {} Hz — 2 listeners + 3 talkers "
            "(stream 0 AM824, stream 1 AAF int32, stream 2 CRF media clock)",
            out,
            blob.size(),
            channels,
            sample_rate);
    } else {
        std::println("wrote {} ({} bytes): {}-ch AM824 @ {} Hz, 1 listener + 1 talker", out, blob.size(), channels, sample_rate);
    }
    return 0;
}
