// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_acmp_milan_talker.hpp"

#include <cstdint>
#include <optional>

namespace statusbar::atdecc {

namespace {

/// The response to @p cmd with @p status: every field echoed, the message
/// type the command's response.
auto echo(AcmpCommandResponse const& cmd, uint8_t const status) noexcept -> AcmpCommandResponse
{
    AcmpCommandResponse resp = cmd;
    resp.set_message_type(static_cast<uint8_t>(cmd.message_type() + 1));
    resp.set_status(status);
    return resp;
}

/// The stream's identity, as Tables 5.43 and 5.47 report it.
void set_stream(AcmpCommandResponse& resp, MilanTalkerSource const& source) noexcept
{
    resp.stream_id = source.stream_id;
    resp.stream_dest_mac = source.stream_dest_mac;
    resp.stream_vlan_id = source.stream_vlan_id;
}

}  // namespace

auto milan_talker_respond(
    AcmpCommandResponse const& cmd, MilanTalkerSource const* const source, uint16_t const ingress_interface) noexcept
    -> std::optional<MilanTalkerReply>
{
    uint8_t const type = cmd.message_type();
    if (type != ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND && type != ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND &&
        type != ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND && type != ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND) {
        return std::nullopt;
    }

    // Table 5.48: GET_TX_CONNECTION is not implemented, for any unique id.
    if (type == ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND) {
        return MilanTalkerReply{.response = echo(cmd, ACMP_STATUS_NOT_SUPPORTED)};
    }
    // Tables 5.40, 5.44, 5.46: talker_unique_id names no STREAM_OUTPUT.
    if (source == nullptr) {
        return MilanTalkerReply{.response = echo(cmd, ACMP_STATUS_TALKER_UNKNOWN_ID)};
    }

    switch (type) {
        case ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND: {
            // Section 5.5.4.1 step 2 (Table 5.41): answered rather than ignored.
            if (ingress_interface != source->avb_interface_index) {
                return MilanTalkerReply{.response = echo(cmd, ACMP_STATUS_INCOMPATIBLE_REQUEST)};
            }
            // Step 3 (Table 5.42). The probe still counts: the window opens
            // now and the talker declares once MAAP has an address.
            if (!source->dest_mac_valid) {
                return MilanTalkerReply{.response = echo(cmd, ACMP_STATUS_TALKER_DEST_MAC_FAIL), .probe_counts = true};
            }
            // Table 5.43. STREAMING_WAIT is echoed and otherwise ignored: a
            // Milan talker always streams while bandwidth is reserved.
            auto resp = echo(cmd, ACMP_STATUS_SUCCESS);
            resp.connection_count = uint16_t{0};
            resp.flags = static_cast<uint16_t>(cmd.flags.get() & (acmp_flags::FAST_CONNECT | acmp_flags::STREAMING_WAIT));
            set_stream(resp, *source);
            return MilanTalkerReply{.response = resp, .probe_counts = true};
        }
        case ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND: {
            // Table 5.45: SUCCESS, the talker unchanged (it has no
            // connections), every stream field and flag zero.
            auto resp = echo(cmd, ACMP_STATUS_SUCCESS);
            resp.connection_count = uint16_t{0};
            resp.flags = uint16_t{0};
            resp.stream_id = ieee::Eui64{};
            resp.stream_dest_mac = ieee::Eui48{};
            resp.stream_vlan_id = uint16_t{0};
            return MilanTalkerReply{.response = resp};
        }
        case ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND:
        default: {
            // Table 5.47. The stream fields are only defined while a Talker
            // attribute is declared; they are reported whatever.
            auto resp = echo(cmd, ACMP_STATUS_SUCCESS);
            resp.listener_entity_id = ieee::Eui64{};
            resp.listener_unique_id = uint16_t{0};
            resp.connection_count = uint16_t{0};
            resp.flags = source->registering_failed ? acmp_flags::REGISTERING_FAILED : uint16_t{0};
            set_stream(resp, *source);
            return MilanTalkerReply{.response = resp};
        }
    }
}

}  // namespace statusbar::atdecc
