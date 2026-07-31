// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_acmp_pdu.hpp"

#include <string_view>

namespace statusbar::atdecc {

auto acmp_message_type_name(uint8_t const type) noexcept -> std::string_view
{
    switch (type) {
        case ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND:
            return "Connect TX Command";
        case ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE:
            return "Connect TX Response";
        case ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND:
            return "Disconnect TX Command";
        case ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE:
            return "Disconnect TX Response";
        case ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND:
            return "Get TX State Command";
        case ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE:
            return "Get TX State Response";
        case ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND:
            return "Connect RX Command";
        case ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE:
            return "Connect RX Response";
        case ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND:
            return "Disconnect RX Command";
        case ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE:
            return "Disconnect RX Response";
        case ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND:
            return "Get RX State Command";
        case ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE:
            return "Get RX State Response";
        case ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND:
            return "Get TX Connection Command";
        case ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE:
            return "Get TX Connection Response";
        default:
            return "Unknown";
    }
}

auto acmp_status_name(uint8_t const status) noexcept -> std::string_view
{
    switch (status) {
        case ACMP_STATUS_SUCCESS:
            return "Success";
        case ACMP_STATUS_LISTENER_UNKNOWN_ID:
            return "Listener Unknown ID";
        case ACMP_STATUS_TALKER_UNKNOWN_ID:
            return "Talker Unknown ID";
        case ACMP_STATUS_TALKER_DEST_MAC_FAIL:
            return "Talker Dest MAC Fail";
        case ACMP_STATUS_TALKER_NO_STREAM_INDEX:
            return "Talker No Stream Index";
        case ACMP_STATUS_TALKER_NO_BANDWIDTH:
            return "Talker No Bandwidth";
        case ACMP_STATUS_TALKER_EXCLUSIVE:
            return "Talker Exclusive";
        case ACMP_STATUS_LISTENER_TALKER_TIMEOUT:
            return "Listener Talker Timeout";
        case ACMP_STATUS_LISTENER_EXCLUSIVE:
            return "Listener Exclusive";
        case ACMP_STATUS_STATE_UNAVAILABLE:
            return "State Unavailable";
        case ACMP_STATUS_NOT_CONNECTED:
            return "Not Connected";
        case ACMP_STATUS_NO_SUCH_CONNECTION:
            return "No Such Connection";
        case ACMP_STATUS_COULD_NOT_SEND_MESSAGE:
            return "Could Not Send Message";
        case ACMP_STATUS_TALKER_MISBEHAVING:
            return "Talker Misbehaving";
        case ACMP_STATUS_LISTENER_MISBEHAVING:
            return "Listener Misbehaving";
        case ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED:
            return "Controller Not Authorized";
        case ACMP_STATUS_INCOMPATIBLE_REQUEST:
            return "Incompatible Request";
        case ACMP_STATUS_LISTENER_INVALID_CONNECTION:
            return "Listener Invalid Connection";
        case ACMP_STATUS_LISTENER_CAN_ONLY_LISTEN_ONCE:
            return "Listener Can Only Listen Once";
        case ACMP_STATUS_NOT_SUPPORTED:
            return "Not Supported";
        default:
            return "Unknown";
    }
}

auto acmp_timeout_for_message_type(uint8_t message_type) noexcept -> std::chrono::milliseconds
{
    switch (message_type) {
        case ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND:
            return std::chrono::milliseconds{ACMP_TIMEOUT_CONNECT_TX_COMMAND_MS};
        case ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND:
            return std::chrono::milliseconds{ACMP_TIMEOUT_DISCONNECT_TX_COMMAND_MS};
        case ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND:
            return std::chrono::milliseconds{ACMP_TIMEOUT_GET_TX_STATE_COMMAND_MS};
        case ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND:
            return std::chrono::milliseconds{ACMP_TIMEOUT_CONNECT_RX_COMMAND_MS};
        case ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND:
            return std::chrono::milliseconds{ACMP_TIMEOUT_DISCONNECT_RX_COMMAND_MS};
        case ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND:
            return std::chrono::milliseconds{ACMP_TIMEOUT_GET_RX_STATE_COMMAND_MS};
        case ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND:
            return std::chrono::milliseconds{ACMP_TIMEOUT_GET_TX_CONNECTION_COMMAND_MS};
        default:
            return std::chrono::milliseconds{200};  // Default timeout
    }
}

auto acmp_command_response_from_pdu(AcmpDu const& pdu) noexcept -> AcmpCommandResponse
{
    AcmpCommandResponse resp{};
    resp.subtype = pdu.subtype;
    resp.sv_version_msgtype = pdu.sv_version_msgtype;
    resp.status_cdl_h = pdu.status_cdl_h;
    resp.control_data_length_l = pdu.control_data_length_l;
    resp.stream_id = pdu.stream_id;
    resp.controller_entity_id = pdu.controller_entity_id;
    resp.talker_entity_id = pdu.talker_entity_id;
    resp.listener_entity_id = pdu.listener_entity_id;
    resp.talker_unique_id = pdu.talker_unique_id;
    resp.listener_unique_id = pdu.listener_unique_id;
    resp.stream_dest_mac = pdu.stream_dest_mac;
    resp.connection_count = pdu.connection_count;
    resp.sequence_id = pdu.sequence_id;
    resp.flags = pdu.flags;
    resp.stream_vlan_id = pdu.stream_vlan_id;
    resp.connected_listeners_entries = pdu.connected_listeners_entries;
    // Extended fields (ip_flags, reserved, ports, IPs) remain zero-initialized
    return resp;
}

auto acmp_command_response_to_pdu(AcmpCommandResponse const& resp, AcmpDu& pdu) noexcept -> void
{
    pdu.subtype = resp.subtype;
    pdu.sv_version_msgtype = resp.sv_version_msgtype;
    pdu.status_cdl_h = resp.status_cdl_h;
    pdu.control_data_length_l = resp.control_data_length_l;
    pdu.stream_id = resp.stream_id;
    pdu.controller_entity_id = resp.controller_entity_id;
    pdu.talker_entity_id = resp.talker_entity_id;
    pdu.listener_entity_id = resp.listener_entity_id;
    pdu.talker_unique_id = resp.talker_unique_id;
    pdu.listener_unique_id = resp.listener_unique_id;
    pdu.stream_dest_mac = resp.stream_dest_mac;
    pdu.connection_count = resp.connection_count;
    pdu.sequence_id = resp.sequence_id;
    pdu.flags = resp.flags;
    pdu.stream_vlan_id = resp.stream_vlan_id;
    pdu.connected_listeners_entries = resp.connected_listeners_entries;
}

auto acmp_serialize_2016(AcmpCommandResponse const& resp, std::span<uint8_t> buffer) noexcept -> std::span<uint8_t const>
{
    // UDP-encapsulated ACMP needs the extended IP fields; serialize the full PDU.
    if (resp.is_udp()) {
        size_t const n = protocol::store_unchecked(buffer, resp);
        return buffer.subspan(0, n);
    }
    // L2 ACMP: emit the pre-2021 56-byte (control_data_length=44) form for max
    // device interop. acmp_command_response_to_pdu copies the cdl header bytes
    // verbatim (still 84), so reset it to the L2 length after the field copy.
    AcmpDu pdu{};
    acmp_command_response_to_pdu(resp, pdu);
    pdu.set_control_data_length(AcmpDu::DATA_LENGTH);
    size_t const n = protocol::store_unchecked(buffer, pdu);
    return buffer.subspan(0, n);
}

}  // namespace statusbar::atdecc
