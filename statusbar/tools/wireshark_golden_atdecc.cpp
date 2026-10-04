// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ATDECC AVTPDUs for the Wireshark golden capture: discovery, connection
/// management, and enumeration/control messages built by the atdecc_*.hpp
/// wire structs, exercising every dissection path the dissector has (AEM
/// payload structs, READ_DESCRIPTOR responses with counted trailers, control
/// values carrying a JDKS log blob, Address Access TLVs, Vendor Unique).

#include "wireshark_golden_atdecc.hpp"

#include "statusbar/atdecc/atdecc_acmp_pdu.hpp"
#include "statusbar/atdecc/atdecc_acmp_types.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/atdecc/atdecc_aecp.hpp"
#include "statusbar/atdecc/atdecc_aecp_aa.hpp"
#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_jdks.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace statusbar::atdecc::golden {

using ieee::Eui48;
using ieee::Eui64;
using namespace aem;  // NOLINT(google-build-using-namespace) golden data names every AEM struct

namespace {

Eui64 const TARGET{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x03};
Eui64 const CONTROLLER{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x11};
Eui64 const MODEL{0x70, 0xB3, 0xD5, 0xED, 0xC1, 0x00, 0x00, 0x23};
Eui64 const TALKER{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0xC8, 0xF0};

template <typename T>
void append(std::vector<uint8_t>& out, T const& value)
{
    auto const bytes = make_const_span(value);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void append(std::vector<uint8_t>& out, std::span<uint8_t const> const bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// An AEM command or response AVTPDU: header + @p payload (+ @p trailer)
auto aem(
    uint16_t const command,
    bool const response,
    uint16_t const sequence,
    std::span<uint8_t const> const payload,
    std::span<uint8_t const> const trailer = {}) -> std::vector<uint8_t>
{
    AemDu du{};
    auto const data_length = static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + payload.size() + trailer.size());
    if (response) {
        du.init_response(command, AEM_STATUS_SUCCESS, data_length);
    } else {
        du.init_command(command, data_length);
    }
    du.target_entity_id = TARGET;
    du.controller_entity_id = CONTROLLER;
    du.sequence_id = sequence;
    std::vector<uint8_t> out;
    append(out, du);
    append(out, payload);
    append(out, trailer);
    return out;
}

template <typename Payload>
auto bytes_of(Payload const& payload) -> std::vector<uint8_t>
{
    std::vector<uint8_t> out;
    append(out, payload);
    return out;
}

/// A descriptor's on-wire image (its wire_size() octets)
template <typename Descriptor>
auto descriptor_bytes(Descriptor const& descriptor) -> std::vector<uint8_t>
{
    auto const bytes = make_const_span(descriptor).first(descriptor.wire_size());
    return {bytes.begin(), bytes.end()};
}

/// A descriptor's fixed part only (LENGTH octets), for descriptors whose
/// inline trailer storage is not what the golden frame appends
template <typename Descriptor>
auto descriptor_fixed_bytes(Descriptor const& descriptor) -> std::vector<uint8_t>
{
    auto const bytes = make_const_span(descriptor).first(Descriptor::LENGTH);
    return {bytes.begin(), bytes.end()};
}

auto read_descriptor_command(uint16_t const type, uint16_t const index, uint16_t const sequence) -> std::vector<uint8_t>
{
    AemReadDescriptorCommandPayload payload{};
    payload.configuration_index = 0;
    payload.descriptor_type = type;
    payload.descriptor_index = index;
    return aem(AEM_COMMAND_READ_DESCRIPTOR, false, sequence, std::span<uint8_t const>(bytes_of(payload)));
}

auto read_descriptor_response(std::vector<uint8_t> const& descriptor, uint16_t const sequence) -> std::vector<uint8_t>
{
    AemReadDescriptorResponsePayload payload{};
    payload.configuration_index = 0;
    return aem(
        AEM_COMMAND_READ_DESCRIPTOR,
        true,
        sequence,
        std::span<uint8_t const>(bytes_of(payload)),
        std::span<uint8_t const>(descriptor));
}

}  // namespace

auto atdecc_golden_frames() -> std::vector<AtdeccGoldenFrame>
{
    std::vector<AtdeccGoldenFrame> frames;

    // ---- ADP ----
    {
        AdpDu adp{};
        adp.init_entity_available(TARGET, 31);
        adp.entity_model_id = MODEL;
        adp.entity_capabilities = entity_capabilities::AEM_SUPPORTED | entity_capabilities::CLASS_A_SUPPORTED |
            entity_capabilities::GPTP_SUPPORTED | entity_capabilities::AEM_IDENTIFY_CONTROL_INDEX_VALID;
        adp.talker_stream_sources = 2;
        adp.talker_capabilities = talker_capabilities::IMPLEMENTED | talker_capabilities::AUDIO_SOURCE;
        adp.listener_stream_sinks = 4;
        adp.listener_capabilities = listener_capabilities::IMPLEMENTED | listener_capabilities::AUDIO_SINK;
        adp.available_index = 7;
        frames.push_back({"adp_entity_available", bytes_of(adp)});
        AdpDu departing{};
        departing.init_entity_departing(TARGET);
        frames.push_back({"adp_entity_departing", bytes_of(departing)});
        AdpDu discover{};
        discover.init_entity_discover();
        frames.push_back({"adp_entity_discover", bytes_of(discover)});
    }

    // ---- ACMP: a CONNECT_RX command and its response, a GET_TX_STATE command ----
    {
        AcmpDu command{};
        command.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
        command.controller_entity_id = CONTROLLER;
        command.talker_entity_id = TALKER;
        command.listener_entity_id = TARGET;
        command.talker_unique_id = 1;
        command.listener_unique_id = 2;
        command.sequence_id = 0x0101;
        command.flags = acmp_flags::STREAMING_WAIT;
        frames.push_back({"acmp_connect_rx_command", bytes_of(command)});
        AcmpDu response = command;
        response.init_response(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, ACMP_STATUS_SUCCESS);
        response.stream_id = Eui64{0x70, 0xB3, 0xD5, 0xED, 0xC8, 0xF0, 0x00, 0x01};
        response.stream_dest_mac = Eui48{0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x01};
        response.connection_count = 1;
        response.stream_vlan_id = 2;
        frames.push_back({"acmp_connect_rx_response", bytes_of(response)});
        AcmpDu state{};
        state.init_command(ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND);
        state.controller_entity_id = CONTROLLER;
        state.talker_entity_id = TALKER;
        state.sequence_id = 0x0102;
        frames.push_back({"acmp_get_tx_state_command", bytes_of(state)});
    }

    // ---- AEM: ACQUIRE_ENTITY command + response ----
    {
        AemAcquireEntityPayload acquire{};
        acquire.flags = 0x00000001U;  // PERSISTENT
        acquire.descriptor_type = DESCRIPTOR_ENTITY;
        frames.push_back(
            {"aem_acquire_entity_command", aem(AEM_COMMAND_ACQUIRE_ENTITY, false, 1, std::span<uint8_t const>(bytes_of(acquire)))});
        acquire.owner_entity_id = CONTROLLER;
        frames.push_back(
            {"aem_acquire_entity_response", aem(AEM_COMMAND_ACQUIRE_ENTITY, true, 1, std::span<uint8_t const>(bytes_of(acquire)))});
    }

    // ---- AEM: READ_DESCRIPTOR of several descriptor types with trailers ----
    frames.push_back({"aem_read_descriptor_entity_command", read_descriptor_command(DESCRIPTOR_ENTITY, 0, 2)});
    {
        DescriptorEntity entity{};
        entity.entity_id = TARGET;
        entity.entity_model_id = MODEL;
        entity.entity_capabilities = entity_capabilities::AEM_SUPPORTED;
        entity.talker_stream_sources = 2;
        entity.listener_stream_sinks = 4;
        entity.available_index = 7;
        entity.entity_name.assign("Golden Entity");
        entity.firmware_version.assign("1.2.3");
        entity.group_name.assign("FOH");
        entity.serial_number.assign("SN-4242");
        entity.configurations_count = 1;
        frames.push_back({"aem_read_descriptor_entity_response", read_descriptor_response(descriptor_bytes(entity), 2)});
    }
    {
        DescriptorConfiguration configuration{};
        configuration.object_name.assign("Default");
        configuration.descriptor_counts_count = 3;
        configuration.descriptor_counts[0].descriptor_type = DESCRIPTOR_AUDIO_UNIT;
        configuration.descriptor_counts[0].count = 1;
        configuration.descriptor_counts[1].descriptor_type = DESCRIPTOR_STREAM_INPUT;
        configuration.descriptor_counts[1].count = 4;
        configuration.descriptor_counts[2].descriptor_type = DESCRIPTOR_CONTROL;
        configuration.descriptor_counts[2].count = 10;
        frames.push_back(
            {"aem_read_descriptor_configuration_response", read_descriptor_response(descriptor_bytes(configuration), 3)});
    }
    {
        DescriptorStream stream{};
        stream.descriptor_type = DESCRIPTOR_STREAM_INPUT;
        stream.descriptor_index = 1;
        stream.object_name.assign("Input 1");
        stream.number_of_formats = 2;
        stream.stream_formats[0] = Eui64{0x02, 0x05, 0x02, 0x18, 0x00, 0x08, 0x02, 0x00};
        stream.stream_formats[1] = Eui64{0x02, 0x05, 0x02, 0x18, 0x00, 0x08, 0x06, 0x00};
        frames.push_back({"aem_read_descriptor_stream_input_response", read_descriptor_response(descriptor_bytes(stream), 4)});
    }
    {
        DescriptorAudioUnit unit{};
        unit.object_name.assign("Audio Unit");
        unit.sampling_rates_count = 2;
        unit.sampling_rates[0] = 48000U;
        unit.sampling_rates[1] = 96000U;
        frames.push_back({"aem_read_descriptor_audio_unit_response", read_descriptor_response(descriptor_bytes(unit), 5)});
    }
    {
        DescriptorClockDomain domain{};
        domain.object_name.assign("Clock Domain");
        domain.clock_sources_count = 2;
        domain.clock_sources[0] = 0;
        domain.clock_sources[1] = 1;
        frames.push_back({"aem_read_descriptor_clock_domain_response", read_descriptor_response(descriptor_bytes(domain), 6)});
    }
    {
        DescriptorAudioMap map{};
        map.number_of_mappings = 2;
        map.mappings[0].mapping_stream_index = 0;
        map.mappings[0].mapping_stream_channel = 0;
        map.mappings[0].mapping_cluster_offset = 0;
        map.mappings[0].mapping_cluster_channel = 0;
        map.mappings[1].mapping_stream_index = 0;
        map.mappings[1].mapping_stream_channel = 1;
        map.mappings[1].mapping_cluster_offset = 1;
        map.mappings[1].mapping_cluster_channel = 0;
        frames.push_back({"aem_read_descriptor_audio_map_response", read_descriptor_response(descriptor_bytes(map), 7)});
    }

    // ---- AEM: GET_STREAM_INFO command + response ----
    {
        AemGetStreamInfoCommandPayload command{};
        command.descriptor_type = DESCRIPTOR_STREAM_INPUT;
        command.descriptor_index = 1;
        frames.push_back(
            {"aem_get_stream_info_command",
             aem(AEM_COMMAND_GET_STREAM_INFO, false, 8, std::span<uint8_t const>(bytes_of(command)))});
        AemStreamInfoPayload info{};
        info.descriptor_type = DESCRIPTOR_STREAM_INPUT;
        info.descriptor_index = 1;
        info.flags = 0x00000081U;
        info.stream_format = {0x02, 0x05, 0x02, 0x18, 0x00, 0x08, 0x02, 0x00};
        info.stream_id = Eui64{0x70, 0xB3, 0xD5, 0xED, 0xC8, 0xF0, 0x00, 0x01};
        info.msrp_accumulated_latency = 2000000U;
        info.stream_dest_mac = Eui48{0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x01};
        info.stream_vlan_id = 2;
        frames.push_back(
            {"aem_get_stream_info_response", aem(AEM_COMMAND_GET_STREAM_INFO, true, 8, std::span<uint8_t const>(bytes_of(info)))});
    }

    // ---- AEM: SET_NAME, GET_COUNTERS response, SET_CONTROL with a JDKS log blob ----
    {
        AemNamePayload name{};
        name.descriptor_type = DESCRIPTOR_ENTITY;
        name.descriptor_index = 0;
        name.name_index = 0;
        name.configuration_index = 0;
        std::string_view const text{"Stage Left"};
        std::memcpy(name.name.data(), text.data(), text.size());
        frames.push_back({"aem_set_name_command", aem(AEM_COMMAND_SET_NAME, false, 9, std::span<uint8_t const>(bytes_of(name)))});
    }
    {
        AemCountersPayload counters{};
        counters.descriptor_type = DESCRIPTOR_AVB_INTERFACE;
        counters.descriptor_index = 0;
        counters.counters_valid = 0x00000007U;
        counters.counters[0] = 1U;
        counters.counters[1] = 2U;
        counters.counters[2] = 3U;
        counters.counters[31] = 0xDEADBEEFU;
        frames.push_back(
            {"aem_get_counters_response", aem(AEM_COMMAND_GET_COUNTERS, true, 10, std::span<uint8_t const>(bytes_of(counters)))});
    }
    {
        AemControlPayloadHeader control{};
        control.descriptor_type = DESCRIPTOR_CONTROL;
        control.descriptor_index = 3;
        jdks::LogBlobHeader blob{};
        blob.vendor_eui64 = jdks::CONTROL_LOG_TEXT;
        std::string_view const text{"hello from golden"};
        blob.blob_size = static_cast<uint32_t>(text.size() + 2);
        blob.log_detail = jdks::log_priority::INFO;
        std::vector<uint8_t> values;
        append(values, blob);
        values.insert(values.end(), text.begin(), text.end());
        frames.push_back(
            {"aem_set_control_jdks_log_command",
             aem(AEM_COMMAND_SET_CONTROL,
                 false,
                 11,
                 std::span<uint8_t const>(bytes_of(control)),
                 std::span<uint8_t const>(values))});
        std::array<uint8_t, 4> const raw{0x00, 0x00, 0x7F, 0xFF};
        frames.push_back(
            {"aem_set_control_raw_command",
             aem(AEM_COMMAND_SET_CONTROL, false, 12, std::span<uint8_t const>(bytes_of(control)), std::span<uint8_t const>(raw))});
    }

    // ---- AEM: GET_AUDIO_MAP response with mappings, ENTITY_AVAILABLE (no payload) ----
    {
        AemAudioMapResponseHeader header{};
        header.descriptor_type = DESCRIPTOR_STREAM_PORT_INPUT;
        header.descriptor_index = 0;
        header.map_index = 0;
        header.number_of_mappings = 2;
        header.number_of_maps = 1;
        std::vector<uint8_t> mappings;
        AemAudioMapping mapping{};
        mapping.stream_index = 0;
        mapping.stream_channel = 0;
        mapping.cluster_offset = 0;
        mapping.cluster_channel = 0;
        append(mappings, mapping);
        mapping.stream_channel = 1;
        mapping.cluster_offset = 1;
        append(mappings, mapping);
        frames.push_back(
            {"aem_get_audio_map_response",
             aem(AEM_COMMAND_GET_AUDIO_MAP,
                 true,
                 13,
                 std::span<uint8_t const>(bytes_of(header)),
                 std::span<uint8_t const>(mappings))});
        frames.push_back({"aem_entity_available_command", aem(AEM_COMMAND_ENTITY_AVAILABLE, false, 14, {})});
    }

    // ---- AEM: descriptors with counted tables and typed control values ----
    {
        // CONTROL: LINEAR_UINT16 gain, two values, dB x 10^-1, then SET_CONTROL
        // (typed from the descriptor) and a GET_CONTROL response
        DescriptorControl control{};
        control.descriptor_index = 3;
        control.object_name.assign("Gain");
        control.control_value_type = 0x0003;  // CONTROL_LINEAR_UINT16
        control.control_type = Eui64{0x90, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x01};
        control.values_offset = DescriptorControl::LENGTH;
        control.number_of_values = 2;
        auto bytes = descriptor_fixed_bytes(control);
        std::array<uint8_t, 28> const values{
            0x00, 0x00, 0x03, 0xE8, 0x00, 0x01, 0x01,
            0xF4, 0x02, 0x58, 0xFF, 0xB0, 0x00, 0x07,  // 0..1000 step 1 default 500 current 600, dB x10^-1, string 7
            0x00, 0x00, 0x03, 0xE8, 0x00, 0x01, 0x01,
            0xF4, 0x00, 0x64, 0xFF, 0xB0, 0x00, 0x08,
        };
        bytes.insert(bytes.end(), values.begin(), values.end());
        frames.push_back({"aem_read_descriptor_control_response", read_descriptor_response(bytes, 20)});
        AemControlPayloadHeader header{};
        header.descriptor_type = DESCRIPTOR_CONTROL;
        header.descriptor_index = 3;
        std::array<uint8_t, 4> const current{0x02, 0xBC, 0x00, 0x32};
        frames.push_back(
            {"aem_set_control_typed_command",
             aem(AEM_COMMAND_SET_CONTROL,
                 false,
                 21,
                 std::span<uint8_t const>(bytes_of(header)),
                 std::span<uint8_t const>(current))});
        frames.push_back(
            {"aem_get_control_typed_response",
             aem(AEM_COMMAND_GET_CONTROL,
                 true,
                 22,
                 std::span<uint8_t const>(bytes_of(header)),
                 std::span<uint8_t const>(current))});
    }
    {
        // CONTROL with a SELECTOR_STRING (3 options) and one with UTF8
        DescriptorControl selector{};
        selector.descriptor_index = 4;
        selector.object_name.assign("Mode");
        selector.control_value_type = 0x0014;  // CONTROL_SELECTOR_STRING
        selector.values_offset = DescriptorControl::LENGTH;
        selector.number_of_values = 3;
        auto bytes = descriptor_fixed_bytes(selector);
        std::array<uint8_t, 12> const values{0x00, 0x11, 0x00, 0x10, 0x00, 0x10, 0x00, 0x11, 0x00, 0x12, 0x00, 0x00};
        bytes.insert(bytes.end(), values.begin(), values.end());
        frames.push_back({"aem_read_descriptor_control_selector_response", read_descriptor_response(bytes, 23)});
        DescriptorControl utf8{};
        utf8.descriptor_index = 5;
        utf8.object_name.assign("Label");
        utf8.control_value_type = 0x001F;  // CONTROL_UTF8
        utf8.values_offset = DescriptorControl::LENGTH;
        utf8.number_of_values = 1;
        auto text_bytes = descriptor_fixed_bytes(utf8);
        std::string_view const text{"Stage left\0"};
        text_bytes.insert(text_bytes.end(), text.begin(), text.end());
        frames.push_back({"aem_read_descriptor_control_utf8_response", read_descriptor_response(text_bytes, 24)});
    }
    {
        // MIXER: two sources and one LINEAR_INT32 value; MATRIX: 2x2 LINEAR_INT16
        DescriptorMixer mixer{};
        mixer.object_name.assign("Mix");
        mixer.control_value_type = 0x0004;  // CONTROL_LINEAR_INT32
        mixer.sources_offset = DescriptorMixer::LENGTH;
        mixer.number_of_sources = 2;
        mixer.value_offset = DescriptorMixer::LENGTH + 8;
        auto bytes = descriptor_bytes(mixer);
        std::array<uint8_t, 8 + 24> const tail{
            0x00, 0x1D, 0x00, 0x00, 0x00, 0x1D, 0x00, 0x01,  // AUDIO_CLUSTER 0, 1
            0xFF, 0xFF, 0xFF, 0x9C, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFA, 0xFF, 0xB0, 0x00, 0x00,
        };
        bytes.insert(bytes.end(), tail.begin(), tail.end());
        frames.push_back({"aem_read_descriptor_mixer_response", read_descriptor_response(bytes, 25)});
        DescriptorMatrix matrix{};
        matrix.object_name.assign("Matrix");
        matrix.control_value_type = 0x0002;  // CONTROL_LINEAR_INT16
        matrix.width = 2;
        matrix.height = 2;
        matrix.values_offset = DescriptorMatrix::LENGTH;
        matrix.number_of_values = 4;
        matrix.number_of_sources = 2;
        auto mbytes = descriptor_bytes(matrix);
        for (uint8_t i = 0; i < 4; ++i) {
            std::array<uint8_t, 14> const entry{0x80, 0x00, 0x7F, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, i, 0xFF, 0xB0, 0x00, 0x00};
            mbytes.insert(mbytes.end(), entry.begin(), entry.end());
        }
        frames.push_back({"aem_read_descriptor_matrix_response", read_descriptor_response(mbytes, 26)});
        AemMatrixPayloadHeader set_matrix{};
        set_matrix.descriptor_type = DESCRIPTOR_MATRIX;
        set_matrix.descriptor_index = 0;
        set_matrix.region_width = 2;
        set_matrix.region_height = 1;
        set_matrix.rep_direction_value_count = 2;
        std::array<uint8_t, 4> const matrix_values{0x00, 0x05, 0xFF, 0xFB};
        frames.push_back(
            {"aem_set_matrix_typed_command",
             aem(AEM_COMMAND_SET_MATRIX,
                 false,
                 27,
                 std::span<uint8_t const>(bytes_of(set_matrix)),
                 std::span<uint8_t const>(matrix_values))});
    }
    {
        // SIGNAL_SELECTOR sources, TIMING ptp instances, SIGNAL_SPLITTER map, MATRIX_SIGNAL signals
        DescriptorSignalSelector selector{};
        selector.object_name.assign("Source select");
        selector.sources_offset = DescriptorSignalSelector::LENGTH;
        selector.number_of_sources = 2;
        auto bytes = descriptor_bytes(selector);
        std::array<uint8_t, 8> const sources{0x00, 0x1D, 0x00, 0x00, 0x00, 0x1D, 0x00, 0x05};
        bytes.insert(bytes.end(), sources.begin(), sources.end());
        frames.push_back({"aem_read_descriptor_signal_selector_response", read_descriptor_response(bytes, 28)});
        DescriptorTiming timing{};
        timing.object_name.assign("Timing");
        timing.ptp_instances_offset = DescriptorTiming::LENGTH;
        timing.number_of_ptp_instances = 2;
        auto tbytes = descriptor_bytes(timing);
        std::array<uint8_t, 4> const instances{0x00, 0x00, 0x00, 0x01};
        tbytes.insert(tbytes.end(), instances.begin(), instances.end());
        frames.push_back({"aem_read_descriptor_timing_response", read_descriptor_response(tbytes, 29)});
        DescriptorSignalSplitter splitter{};
        splitter.object_name.assign("Split");
        splitter.number_of_outputs = 2;
        splitter.splitter_map_count = 2;
        splitter.splitter_map_offset = DescriptorSignalSplitter::LENGTH;
        auto sbytes = descriptor_bytes(splitter);
        std::array<uint8_t, 8> const map{0x00, 0x1D, 0x00, 0x02, 0x00, 0x1D, 0x00, 0x03};
        sbytes.insert(sbytes.end(), map.begin(), map.end());
        frames.push_back({"aem_read_descriptor_signal_splitter_response", read_descriptor_response(sbytes, 30)});
        DescriptorMatrixSignal signals{};
        signals.descriptor_index = 1;
        signals.signals_offset = DescriptorMatrixSignal::LENGTH;
        signals.signals_count = 3;
        auto gbytes = descriptor_bytes(signals);
        std::array<uint8_t, 12> const sig{0x00, 0x1D, 0x00, 0x00, 0x00, 0x1D, 0x00, 0x01, 0x00, 0x1D, 0x00, 0x02};
        gbytes.insert(gbytes.end(), sig.begin(), sig.end());
        frames.push_back({"aem_read_descriptor_matrix_signal_response", read_descriptor_response(gbytes, 31)});
    }
    {
        // VIDEO_CLUSTER with all five tables, SENSOR_CLUSTER with two
        DescriptorVideoCluster video{};
        video.object_name.assign("Video in");
        video.supported_format_specifics_offset = DescriptorVideoCluster::LENGTH;
        video.supported_format_specifics_count = 1;
        video.supported_sampling_rates_offset = DescriptorVideoCluster::LENGTH + 4;
        video.supported_sampling_rates_count = 1;
        video.supported_aspect_ratios_offset = DescriptorVideoCluster::LENGTH + 8;
        video.supported_aspect_ratios_count = 2;
        video.supported_sizes_offset = DescriptorVideoCluster::LENGTH + 12;
        video.supported_sizes_count = 1;
        video.supported_color_spaces_offset = DescriptorVideoCluster::LENGTH + 16;
        video.supported_color_spaces_count = 1;
        auto bytes = descriptor_bytes(video);
        std::array<uint8_t, 18> const tables{
            0x00,
            0x00,
            0x00,
            0x42,  // format specific
            0x00,
            0x00,
            0x00,
            0x3C,  // 60 Hz
            0x10,
            0x09,
            0x04,
            0x03,  // 16:9, 4:3
            0x07,
            0x80,
            0x04,
            0x38,  // 1920x1080
            0x00,
            0x02,  // color space
        };
        bytes.insert(bytes.end(), tables.begin(), tables.end());
        frames.push_back({"aem_read_descriptor_video_cluster_response", read_descriptor_response(bytes, 32)});
        DescriptorSensorCluster sensor{};
        sensor.object_name.assign("Sensor");
        sensor.supported_formats_offset = DescriptorSensorCluster::LENGTH;
        sensor.supported_formats_count = 1;
        sensor.supported_sampling_rates_offset = DescriptorSensorCluster::LENGTH + 8;
        sensor.supported_sampling_rates_count = 2;
        auto sbytes = descriptor_bytes(sensor);
        std::array<uint8_t, 16> const stables{
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x03, 0xE8};
        sbytes.insert(sbytes.end(), stables.begin(), stables.end());
        frames.push_back({"aem_read_descriptor_sensor_cluster_response", read_descriptor_response(sbytes, 33)});
    }
    {
        // STREAM_OUTPUT with a decoded AAF current_format, formats and two redundant streams
        DescriptorStream stream{};
        stream.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
        stream.descriptor_index = 0;
        stream.object_name.assign("Output 1");
        stream.current_format = Eui64{0x02, 0x05, 0x02, 0x18, 0x00, 0x80, 0x60, 0x00};  // AAF 48k int32 24-bit 2ch 6 spf
        stream.number_of_formats = 2;
        stream.stream_formats[0] = Eui64{0x02, 0x05, 0x02, 0x18, 0x00, 0x80, 0x60, 0x00};
        stream.stream_formats[1] = Eui64{0x00, 0xA0, 0x02, 0x02, 0x40, 0x40, 0x00, 0x00};  // IEC 61883-6 AM824 48k 2ch
        stream.redundant_offset = static_cast<uint16_t>(stream.wire_size());
        stream.number_of_redundant_streams = 2;
        auto bytes = descriptor_bytes(stream);
        std::array<uint8_t, 4> const redundant{0x00, 0x01, 0x00, 0x02};
        bytes.insert(bytes.end(), redundant.begin(), redundant.end());
        frames.push_back({"aem_read_descriptor_stream_output_redundant_response", read_descriptor_response(bytes, 34)});
    }

    // ---- AEM: payloads of commands the C++ stack does not implement (from the standard) ----
    {
        std::array<uint8_t, 16> const video_format{
            0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x10, 0x09, 0x00, 0x02, 0x07, 0x80, 0x04, 0x38};
        frames.push_back({"aem_get_video_format_response", aem(0x000B, true, 40, std::span<uint8_t const>(video_format))});
        std::array<uint8_t, 12> const sensor_format{0x00, 0x14, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
        frames.push_back({"aem_set_sensor_format_command", aem(0x000C, false, 41, std::span<uint8_t const>(sensor_format))});
        std::array<uint8_t, 8> const association{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0xAA, 0x01};
        frames.push_back({"aem_set_association_id_command", aem(0x0012, false, 42, std::span<uint8_t const>(association))});
        std::array<uint8_t, 4 + 16> const as_path{0x00, 0x00, 0x00, 0x02, 0x70, 0xB3, 0xD5, 0xED, 0xC3, 0x00,
                                                  0x00, 0x01, 0x70, 0xB3, 0xD5, 0xED, 0xC3, 0x00, 0x00, 0x02};
        frames.push_back({"aem_get_as_path_response", aem(0x0028, true, 43, std::span<uint8_t const>(as_path))});
        std::array<uint8_t, 12 + 8> const video_map{0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01,
                                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00};
        frames.push_back({"aem_get_video_map_response", aem(0x002E, true, 44, std::span<uint8_t const>(video_map))});
        std::array<uint8_t, 8 + 12> const sensor_mappings{0x00, 0x11, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
                                                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01};
        frames.push_back({"aem_add_sensor_mappings_command", aem(0x0032, false, 45, std::span<uint8_t const>(sensor_mappings))});
        std::array<uint8_t, 12 + 6> const sensor_map{
            0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04};
        frames.push_back({"aem_get_sensor_map_response", aem(0x0031, true, 55, std::span<uint8_t const>(sensor_map))});
        std::array<uint8_t, 12> const encryption{0x00, 0x06, 0x00, 0x00, 0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x00, 0x07};
        frames.push_back({"aem_enable_stream_encryption_command", aem(0x0045, false, 46, std::span<uint8_t const>(encryption))});
        std::array<uint8_t, 12> const memory_length{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00};
        frames.push_back({"aem_set_memory_object_length_command", aem(0x0047, false, 47, std::span<uint8_t const>(memory_length))});
        std::array<uint8_t, 52> backup{0x00, 0x05, 0x00, 0x01};
        std::array<uint8_t, 8> const backup_talker{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x11};
        std::copy(backup_talker.begin(), backup_talker.end(), backup.begin() + 4);  // backup_talker_entity_id_0
        backup[13] = 0x02;                                                          // backup_talker_unique_id_0
        std::array<uint8_t, 8> const backedup_talker{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x22};
        std::copy(backedup_talker.begin(), backedup_talker.end(), backup.begin() + 40);  // backedup_talker_entity_id
        backup[49] = 0x03;                                                               // backedup_talker_unique_id
        frames.push_back({"aem_set_stream_backup_command", aem(0x0049, false, 48, std::span<uint8_t const>(backup))});
    }

    // ---- AEM: authentication, security, dynamic info, sampling rate range,
    // PTP instance and port, path latency (IEEE 1722.1-2021 7.4.56-7.4.104) ----
    {
        std::array<uint8_t, 12 + 4> const add_key{
            0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x00, 0x07, 0x02, 0x00, 0x04, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};
        frames.push_back({"aem_auth_add_key_command", aem(0x0037, false, 70, std::span<uint8_t const>(add_key))});
        std::array<uint8_t, 8 + 16> const keychain_list{0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x70, 0xB3, 0xD5, 0xED,
                                                        0xC4, 0x00, 0x00, 0x07, 0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x00, 0x08};
        frames.push_back({"aem_auth_get_keychain_list_response", aem(0x003D, true, 71, std::span<uint8_t const>(keychain_list))});
        std::array<uint8_t, 72> identity{};
        identity[7] = 0x09;
        identity[8] = 0xC1;
        identity[39] = 0xC2;
        identity[40] = 0xD1;
        identity[71] = 0xD2;
        frames.push_back({"aem_auth_get_identity_response", aem(0x003E, true, 72, std::span<uint8_t const>(identity))});
        std::array<uint8_t, 8 + 6> const authenticate{0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 's', 'e', 'c', 'r', 'e', 't'};
        frames.push_back({"aem_authenticate_command", aem(0x0041, false, 73, std::span<uint8_t const>(authenticate))});
        std::array<uint8_t, 8> const transport_key{0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x00, 0x09};
        frames.push_back(
            {"aem_enable_transport_security_command", aem(0x0043, false, 74, std::span<uint8_t const>(transport_key))});
        // GET_DYNAMIC_INFO: a GET_CONFIGURATION entry (no command payload) and a GET_SAMPLING_RATE entry
        std::array<uint8_t, 8 + 8 + 4> const dynamic_command{
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,                          // GET_CONFIGURATION
            0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x00, 0x01, 0x00, 0x00,  // GET_SAMPLING_RATE AUDIO_UNIT 0
        };
        frames.push_back({"aem_get_dynamic_info_command", aem(0x004B, false, 75, std::span<uint8_t const>(dynamic_command))});
        std::array<uint8_t, 8 + 4 + 8 + 8> const dynamic_response{
            0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00,  // GET_CONFIGURATION rsp
            0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x00, 0x01, 0x00, 0x00,
            0x00, 0x00, 0xBB, 0x80,  // GET_SAMPLING_RATE rsp 48 kHz
        };
        frames.push_back({"aem_get_dynamic_info_response", aem(0x004B, true, 75, std::span<uint8_t const>(dynamic_response))});
        std::array<uint8_t, 12> const rate_range{0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x3C};
        frames.push_back({"aem_set_sampling_rate_range_command", aem(0x004E, false, 76, std::span<uint8_t const>(rate_range))});
        std::array<uint8_t, 12> const set_instance{0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0xE0, 0xE0, 0xF8, 0xF8, 0x00, 0x05};
        frames.push_back({"aem_set_ptp_instance_info_command", aem(0x0050, false, 77, std::span<uint8_t const>(set_instance))});
        std::array<uint8_t, 36> instance_info{
            0x00, 0x22, 0x00, 0x00, 0xF8, 0xFE, 0x43, 0x6A, 0xF8, 0xF8, 0x00, 0x90, 0x00, 0x25, 0x01, 0x3D};
        std::array<uint8_t, 8> const gm{0x70, 0xB3, 0xD5, 0xED, 0xC3, 0x00, 0x00, 0x01};
        std::copy(gm.begin(), gm.end(), instance_info.begin() + 16);
        instance_info[24] = 0xF8;
        instance_info[25] = 0xFE;
        instance_info[26] = 0x43;
        instance_info[27] = 0x6A;
        instance_info[28] = 0xF8;
        instance_info[29] = 0xF8;
        instance_info[30] = 0x90;
        instance_info[31] = 0x3C;
        instance_info[33] = 0x25;
        frames.push_back({"aem_get_ptp_instance_info_response", aem(0x0051, true, 78, std::span<uint8_t const>(instance_info))});
        std::array<uint8_t, 100> extended{};
        std::copy(instance_info.begin(), instance_info.end(), extended.begin());
        std::copy(gm.begin(), gm.end(), extended.begin() + 36);  // parent clock identity
        extended[45] = 0x01;                                     // parent port 1
        extended[47] = 0x02;                                     // steps removed 2
        extended[52] = 0xFE;                                     // valid_flags: every optional field present
        extended[55] = 0x03;                                     // gm_timebase_indicator
        extended[67] = 0x2A;                                     // offset_from_master low octet
        extended[80] = 0x3F;                                     // last_gm_freq_change = 1.0f
        extended[81] = 0x80;
        extended[87] = 0x04;  // gm_change_count 4
        frames.push_back(
            {"aem_get_ptp_instance_extended_info_response", aem(0x0052, true, 79, std::span<uint8_t const>(extended))});
        std::array<uint8_t, 36> grandmaster{0x00, 0x22, 0x00, 0x00};
        std::copy(gm.begin(), gm.end(), grandmaster.begin() + 4);
        grandmaster[12] = 0xF8;
        grandmaster[13] = 0xFE;
        grandmaster[14] = 0x43;
        grandmaster[15] = 0x6A;
        grandmaster[16] = 0xF8;
        grandmaster[17] = 0xF8;
        grandmaster[18] = 0x90;
        grandmaster[19] = 0x3C;
        grandmaster[21] = 0x25;
        std::copy(gm.begin(), gm.end(), grandmaster.begin() + 24);
        grandmaster[33] = 0x01;
        grandmaster[35] = 0x02;
        frames.push_back(
            {"aem_get_ptp_instance_grandmaster_info_response", aem(0x0053, true, 80, std::span<uint8_t const>(grandmaster))});
        std::array<uint8_t, 8 + 16> path_trace{0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
        std::copy(gm.begin(), gm.end(), path_trace.begin() + 8);
        std::copy(gm.begin(), gm.end(), path_trace.begin() + 16);
        path_trace[23] = 0x02;
        frames.push_back({"aem_get_ptp_instance_path_trace_response", aem(0x0055, true, 81, std::span<uint8_t const>(path_trace))});
        std::array<uint8_t, 12> const pm_count{0x00, 0x22, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x61, 0x00, 0x10};
        frames.push_back(
            {"aem_get_ptp_instance_perf_mon_count_response", aem(0x0056, true, 82, std::span<uint8_t const>(pm_count))});
        std::array<uint8_t, 144> record{0x00, 0x22, 0x00, 0x00, 0x00, 0x03, 0xFC, 0x00};
        record[15] = 0x64;  // timestamp 100 ns
        record[23] = 0x10;  // average_master_slave_delay 16
        for (size_t i = 136; i < 144; ++i) {
            record[i] = 0xFF;  // std_dev_offset_from_master = -2
        }
        record[143] = 0xFE;
        frames.push_back(
            {"aem_get_ptp_instance_perf_mon_record_response", aem(0x0057, true, 83, std::span<uint8_t const>(record))});
        std::array<uint8_t, 12> const intervals{0x00, 0x23, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x00, 0x00, 0xFD, 0x00, 0x03};
        frames.push_back(
            {"aem_set_ptp_port_initial_intervals_command", aem(0x0058, false, 84, std::span<uint8_t const>(intervals))});
        std::array<uint8_t, 16> const overrides{
            0x00, 0x23, 0x00, 0x00, 0xFF, 0x00, 0xFE, 0xE0, 0x00, 0xFD, 0x00, 0x03, 0x09, 0x00, 0x00, 0x00};
        frames.push_back({"aem_set_ptp_port_overrides_command", aem(0x0060, false, 85, std::span<uint8_t const>(overrides))});
        std::array<uint8_t, 48> pdelay{0x00, 0x23, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00};
        pdelay[15] = 0x64;
        pdelay[23] = 0x7B;
        frames.push_back({"aem_get_ptp_port_pdelay_mon_record_response", aem(0x0063, true, 86, std::span<uint8_t const>(pdelay))});
        std::array<uint8_t, 84> port_record{0x00, 0x23, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00};
        port_record[15] = 0x64;
        port_record[19] = 0x08;  // announce_tx
        port_record[83] = 0x11;  // pdelay_resp_followup_rx
        frames.push_back(
            {"aem_get_ptp_port_perf_mon_record_response", aem(0x0065, true, 87, std::span<uint8_t const>(port_record))});
        std::array<uint8_t, 8> const latency{0x00, 0x14, 0x00, 0x00, 0x00, 0x1E, 0x84, 0x80};
        frames.push_back({"aem_get_path_latency_response", aem(0x0066, true, 88, std::span<uint8_t const>(latency))});
        std::array<uint8_t, 16> const nonces{
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18};
        frames.push_back({"aem_auth_get_nonce_response", aem(0x0067, true, 89, std::span<uint8_t const>(nonces))});
        std::array<uint8_t, 28 + 2> const key_nonce{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x11, 0x12,
                                                    0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x70, 0xB3, 0xD5, 0xED,
                                                    0xC4, 0x00, 0x00, 0x07, 0x02, 0x00, 0x02, 0x00, 0xBE, 0xEF};
        frames.push_back({"aem_auth_add_key_nonce_command", aem(0x0068, false, 90, std::span<uint8_t const>(key_nonce))});
        std::array<uint8_t, 4> const disable_encryption{0x00, 0x06, 0x00, 0x00};
        frames.push_back(
            {"aem_disable_stream_encryption_command", aem(0x0046, false, 91, std::span<uint8_t const>(disable_encryption))});
    }

    // ---- HDCP APM command: the last fragment of a 20-octet message ----
    {
        AecpDuCommon du{};
        du.init_command(AECP_MESSAGE_TYPE_HDCP_APM_COMMAND, static_cast<uint16_t>(AecpDuCommon::COMMON_DATA_LENGTH + 6 + 4));
        du.target_entity_id = TARGET;
        du.controller_entity_id = CONTROLLER;
        du.sequence_id = 51;
        std::vector<uint8_t> out;
        append(out, du);
        std::array<uint8_t, 10> const apm{0x00, 0x14, 0x00, 0x00, 0x00, 0x10, 0xA1, 0xA2, 0xA3, 0xA4};
        append(out, std::span<uint8_t const>(apm));
        frames.push_back({"hdcp_apm_command", out});
    }

    // ---- AVC command: an AV/C UNIT INFO status frame ----
    {
        AecpDuCommon du{};
        du.init_command(AECP_MESSAGE_TYPE_AVC_COMMAND, static_cast<uint16_t>(AecpDuCommon::COMMON_DATA_LENGTH + 2 + 8));
        du.target_entity_id = TARGET;
        du.controller_entity_id = CONTROLLER;
        du.sequence_id = 50;
        std::vector<uint8_t> out;
        append(out, du);
        std::array<uint8_t, 10> const avc{0x00, 0x08, 0x01, 0xFF, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        append(out, std::span<uint8_t const>(avc));
        frames.push_back({"avc_command", out});
    }

    // ---- EXTENDED command/response: reserved by IEEE 1722.1-2021 Table 9-1, so the data stays opaque ----
    {
        AecpDuCommon du{};
        du.init_command(AECP_MESSAGE_TYPE_EXTENDED_COMMAND, static_cast<uint16_t>(AecpDuCommon::COMMON_DATA_LENGTH + 6));
        du.target_entity_id = TARGET;
        du.controller_entity_id = CONTROLLER;
        du.sequence_id = 52;
        std::vector<uint8_t> out;
        append(out, du);
        std::array<uint8_t, 6> const data{0xE0, 0x01, 0x02, 0x03, 0x04, 0x05};
        append(out, std::span<uint8_t const>(data));
        frames.push_back({"extended_command", out});

        AecpDuCommon rsp{};
        rsp.init_response(AECP_MESSAGE_TYPE_EXTENDED_RESPONSE, AECP_STATUS_NOT_IMPLEMENTED, AecpDuCommon::COMMON_DATA_LENGTH);
        rsp.target_entity_id = TARGET;
        rsp.controller_entity_id = CONTROLLER;
        rsp.sequence_id = 52;
        std::vector<uint8_t> rout;
        append(rout, rsp);
        frames.push_back({"extended_response", rout});
    }

    // ---- Milan vendor unique: GET_MILAN_INFO, SET_SYSTEM_UNIQUE_ID, MCR info, BIND_STREAM, GET_STREAM_INPUT_INFO_EX ----
    {
        auto const mvu = [&](bool const response, uint16_t const sequence, std::span<uint8_t const> const payload) {
            AecpDuCommon du{};
            du.init_command(
                response ? AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE : AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND,
                static_cast<uint16_t>(AecpDuCommon::COMMON_DATA_LENGTH + 6 + payload.size()));
            du.target_entity_id = TARGET;
            du.controller_entity_id = CONTROLLER;
            du.sequence_id = sequence;
            std::vector<uint8_t> out;
            append(out, du);
            std::array<uint8_t, 6> const protocol_id{0x00, 0x1B, 0xC5, 0x0A, 0xC1, 0x00};
            append(out, std::span<uint8_t const>(protocol_id));
            append(out, payload);
            return out;
        };
        std::array<uint8_t, 4> const get_info{0x00, 0x00, 0x00, 0x00};
        frames.push_back({"mvu_get_milan_info_command", mvu(false, 60, std::span<uint8_t const>(get_info))});
        std::array<uint8_t, 20> const info{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
                                           0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x03, 0x00};
        frames.push_back({"mvu_get_milan_info_response", mvu(true, 60, std::span<uint8_t const>(info))});
        std::vector<uint8_t> unique_id{0x00, 0x01, 0x00, 0x00, 0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x51, 0x1D};
        std::array<uint8_t, 64> name{};
        std::string_view const text{"FOH system"};
        std::copy(text.begin(), text.end(), name.begin());
        unique_id.insert(unique_id.end(), name.begin(), name.end());
        frames.push_back({"mvu_set_system_unique_id_command", mvu(false, 61, std::span<uint8_t const>(unique_id))});
        std::vector<uint8_t> mcr{0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00};
        std::array<uint8_t, 64> domain{};
        std::string_view const dtext{"Main clock"};
        std::copy(dtext.begin(), dtext.end(), domain.begin());
        mcr.insert(mcr.end(), domain.begin(), domain.end());
        frames.push_back({"mvu_set_media_clock_reference_info_command", mvu(false, 62, std::span<uint8_t const>(mcr))});
        std::array<uint8_t, 20> const bind{0x00, 0x05, 0x00, 0x01, 0x00, 0x05, 0x00, 0x00, 0x70, 0xB3,
                                           0xD5, 0xED, 0xC0, 0x00, 0xC8, 0xF0, 0x00, 0x01, 0x00, 0x00};
        frames.push_back({"mvu_bind_stream_command", mvu(false, 63, std::span<uint8_t const>(bind))});
        std::array<uint8_t, 20> const info_ex{0x00, 0x07, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x70, 0xB3,
                                              0xD5, 0xED, 0xC0, 0x00, 0xC8, 0xF0, 0x00, 0x01, 0x02, 0x00};
        frames.push_back({"mvu_get_stream_input_info_ex_response", mvu(true, 64, std::span<uint8_t const>(info_ex))});
    }

    // ---- Address Access: a command with a read and a write TLV ----
    {
        AaTlvHeader read{};
        read.set_mode_length(AA_MODE_READ, 8);
        read.address = 0x0000000010000000ULL;
        AaTlvHeader write{};
        write.set_mode_length(AA_MODE_WRITE, 4);
        write.address = 0x0000000010000100ULL;
        std::array<uint8_t, 4> const data{0x11, 0x22, 0x33, 0x44};
        auto const tlv_octets = static_cast<uint16_t>(AaTlvHeader::LENGTH + 8 + AaTlvHeader::LENGTH + 4);
        AecpAaDu du{};
        du.init_command(TARGET, CONTROLLER, 15, 2, tlv_octets);
        std::vector<uint8_t> out;
        append(out, du);
        append(out, read);
        out.insert(out.end(), 8, 0);
        append(out, write);
        append(out, std::span<uint8_t const>(data));
        frames.push_back({"aa_command", out});
    }

    // ---- Vendor Unique: a command with a 6-octet protocol id and four octets ----
    {
        AecpDuCommon du{};
        du.init_command(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND, static_cast<uint16_t>(AecpDuCommon::COMMON_DATA_LENGTH + 6 + 4));
        du.target_entity_id = TARGET;
        du.controller_entity_id = CONTROLLER;
        du.sequence_id = 16;
        std::vector<uint8_t> out;
        append(out, du);
        std::array<uint8_t, 10> const tail{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0xCA, 0xFE, 0xF0, 0x0D};
        append(out, std::span<uint8_t const>(tail));
        frames.push_back({"vu_command", out});
    }
    return frames;
}

}  // namespace statusbar::atdecc::golden
