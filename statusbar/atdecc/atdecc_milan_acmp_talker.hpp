#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Milan v1.3 talker behaviour, section 5.5.4.
///
/// A Milan talker keeps no state about listeners (5.5.2.7): a
/// PROBE_TX_COMMAND only asks for a stream's parameters, a
/// DISCONNECT_TX_COMMAND changes nothing, and GET_TX_CONNECTION is not
/// supported. So there is no state machine here, only the response each
/// command gets from what the talker knows of the source at that moment.
/// The IEEE 1722.1-2021 talker, with its list of connected listeners, is
/// atdecc_acmp_talker_sm.hpp.

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <cstdint>
#include <optional>

namespace statusbar::atdecc {

/// The parameters of one STREAM_OUTPUT's stream.
struct MilanTalkerStream
{
    ieee::Eui64 stream_id{};
    ieee::Eui48 stream_dest_mac{};
    uint16_t stream_vlan_id{0};
};

/// What the talker knows of the source a command names, when it answers.
struct MilanTalkerSource
{
    /// The stream, or nullptr when talker_unique_id names no STREAM_OUTPUT.
    MilanTalkerStream const* stream{nullptr};
    /// A valid destination MAC address is available (5.5.4.1 step 3).
    bool dest_mac_valid{true};
    /// The source declares a Talker attribute and registers a matching
    /// Listener Asking Failed (Table 5.23).
    bool registering_failed{false};
};

/// PROBE_TX_RESPONSE (5.5.4.1): Table 5.45 for an unknown source, 5.47
/// without a destination address, 5.48 otherwise.
[[nodiscard]] auto milan_probe_tx_response(AcmpCommandResponse const& cmd, MilanTalkerSource const& source) noexcept
    -> AcmpCommandResponse;

/// DISCONNECT_TX_RESPONSE (5.5.4.2): Table 5.49 or 5.50. Nothing changes.
[[nodiscard]] auto milan_disconnect_tx_response(AcmpCommandResponse const& cmd, MilanTalkerSource const& source) noexcept
    -> AcmpCommandResponse;

/// GET_TX_STATE_RESPONSE (5.5.4.3): Table 5.51 or 5.52.
[[nodiscard]] auto milan_get_tx_state_response(AcmpCommandResponse const& cmd, MilanTalkerSource const& source) noexcept
    -> AcmpCommandResponse;

/// GET_TX_CONNECTION_RESPONSE (5.5.4.4): NOT_SUPPORTED, Table 5.53.
[[nodiscard]] auto milan_get_tx_connection_response(AcmpCommandResponse const& cmd) noexcept -> AcmpCommandResponse;

/// The response to @p cmd, nullopt when it is not a command a talker answers.
[[nodiscard]] auto milan_talker_response(AcmpCommandResponse const& cmd, MilanTalkerSource const& source) noexcept
    -> std::optional<AcmpCommandResponse>;

}  // namespace statusbar::atdecc
