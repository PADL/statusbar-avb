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
Eui64 const TALKER{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0xC8, 0xF0};

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
        response.stream_id = Eui64{0x00, 0x1C, 0xAB, 0x00, 0xC8, 0xF0, 0x00, 0x01};
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
        info.stream_id = Eui64{0x00, 0x1C, 0xAB, 0x00, 0xC8, 0xF0, 0x00, 0x01};
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
