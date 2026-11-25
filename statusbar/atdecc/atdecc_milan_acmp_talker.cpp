// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_milan_acmp_talker.hpp"

#include "statusbar/atdecc/atdecc_acmp.hpp"

#include <cstdint>
#include <optional>

namespace statusbar::atdecc {

namespace {

/// @p cmd as its response: every field kept, the response message type, @p status.
[[nodiscard]] auto response_to(AcmpCommandResponse const& cmd, uint8_t const status) noexcept -> AcmpCommandResponse
{
    AcmpCommandResponse resp = cmd;
    resp.set_message_type(static_cast<uint8_t>(cmd.message_type() | 0x01U));
    resp.set_status(status);
    return resp;
}

void set_stream_fields(AcmpCommandResponse& resp, MilanTalkerStream const& stream) noexcept
{
    resp.stream_id = stream.stream_id;
    resp.stream_dest_mac = stream.stream_dest_mac;
    resp.stream_vlan_id = stream.stream_vlan_id;
}

/// Table 5.52: a talker has no listener to name and none to count.
void name_no_listener(AcmpCommandResponse& resp) noexcept
{
    resp.listener_entity_id = ieee::Eui64{};
    resp.listener_unique_id = 0;
    resp.connection_count = 0;
}

/// The status a PROBE_TX_COMMAND is answered with (5.5.4.1 steps 1, 3, 4).
[[nodiscard]] auto probe_status(MilanTalkerSource const& source) noexcept -> uint8_t
{
    if (source.stream == nullptr) {
        return ACMP_STATUS_TALKER_UNKNOWN_ID;
    }
    return source.dest_mac_valid ? ACMP_STATUS_SUCCESS : ACMP_STATUS_TALKER_DEST_MAC_FAIL;
}

}  // namespace

auto milan_probe_tx_response(AcmpCommandResponse const& cmd, MilanTalkerSource const& source) noexcept -> AcmpCommandResponse
{
    AcmpCommandResponse resp = response_to(cmd, probe_status(source));
    if (resp.status() != ACMP_STATUS_SUCCESS) {
        return resp;
    }
    resp.connection_count = 0;
    resp.flags = static_cast<uint16_t>(cmd.flags.get() & (acmp_flags::FAST_CONNECT | acmp_flags::STREAMING_WAIT));
    set_stream_fields(resp, *source.stream);
    return resp;
}

auto milan_disconnect_tx_response(AcmpCommandResponse const& cmd, MilanTalkerSource const& source) noexcept -> AcmpCommandResponse
{
    if (source.stream == nullptr) {
        return response_to(cmd, ACMP_STATUS_TALKER_UNKNOWN_ID);
    }
    AcmpCommandResponse resp = response_to(cmd, ACMP_STATUS_SUCCESS);
    resp.connection_count = 0;
    resp.flags = 0;
    set_stream_fields(resp, MilanTalkerStream{});
    return resp;
}

auto milan_get_tx_state_response(AcmpCommandResponse const& cmd, MilanTalkerSource const& source) noexcept -> AcmpCommandResponse
{
    if (source.stream == nullptr) {
        return response_to(cmd, ACMP_STATUS_TALKER_UNKNOWN_ID);
    }
    AcmpCommandResponse resp = response_to(cmd, ACMP_STATUS_SUCCESS);
    name_no_listener(resp);
    resp.flags = source.registering_failed ? acmp_flags::SRP_REGISTRATION_FAILED : uint16_t{0};
    set_stream_fields(resp, *source.stream);
    return resp;
}

auto milan_get_tx_connection_response(AcmpCommandResponse const& cmd) noexcept -> AcmpCommandResponse
{
    return response_to(cmd, ACMP_STATUS_NOT_SUPPORTED);
}

auto milan_talker_response(AcmpCommandResponse const& cmd, MilanTalkerSource const& source) noexcept
    -> std::optional<AcmpCommandResponse>
{
    switch (cmd.message_type()) {
        case ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND:
            return milan_probe_tx_response(cmd, source);
        case ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND:
            return milan_disconnect_tx_response(cmd, source);
        case ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND:
            return milan_get_tx_state_response(cmd, source);
        case ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND:
            return milan_get_tx_connection_response(cmd);
        default:
            return std::nullopt;
    }
}

}  // namespace statusbar::atdecc
