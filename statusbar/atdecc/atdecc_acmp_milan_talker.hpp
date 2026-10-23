#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// The Milan talker's connection management (Milan v1.2 Section 5.5.4, Tables 5.40-5.48): a
/// stateless responder answering each command from the stream output's current state.
/// See docs/MILAN_TALKER.md.

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <cstdint>
#include <optional>

namespace statusbar::atdecc {

/// Milan v1.2 Section 5.5.2.2 renames CONNECT_TX to PROBE_TX (same values).
constexpr uint8_t ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND = ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND;
constexpr uint8_t ACMP_MESSAGE_TYPE_PROBE_TX_RESPONSE = ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE;

namespace acmp_flags {
/// Milan v1.2 Table 5.23: TALKER_FAILED renamed. In a GET_TX_STATE_RESPONSE,
/// the source is declaring a Talker attribute and registering a matching
/// Listener Asking Failed.
constexpr uint16_t REGISTERING_FAILED = TALKER_FAILED;
}  // namespace acmp_flags

/// What a Milan talker answers from, for one STREAM_OUTPUT.
struct MilanTalkerSource
{
    /// The stream's identity, as declared (or to be declared) in its Talker
    /// Advertise.
    ieee::Eui64 stream_id{};
    ieee::Eui48 stream_dest_mac{};
    uint16_t stream_vlan_id{0};
    /// The STREAM_OUTPUT's avb_interface_index: a probe arriving on another
    /// AVB interface is refused (Section 5.5.4.1 step 2).
    uint16_t avb_interface_index{0};
    /// MAAP has allocated the stream's destination address with no conflict (Milan v1.2 Section
    /// 4.3.3.1 condition 1); without it a probe is answered TALKER_DEST_MAC_FAIL, never ignored.
    bool dest_mac_valid{false};
    /// Declaring a Talker attribute and registering a matching Listener
    /// Asking Failed (Table 5.23), for GET_TX_STATE_RESPONSE.
    bool registering_failed{false};
};

/// The talker's answer to one command, and whether it is a probe that opens the 15 s declaration
/// window (Milan v1.2 Section 4.3.3.1): a PROBE_TX_COMMAND for a valid stream output on its own
/// AVB interface, answered SUCCESS or TALKER_DEST_MAC_FAIL.
struct MilanTalkerReply
{
    AcmpCommandResponse response{};
    bool probe_counts{false};
};

/// The response to @p cmd (addressed to this talker) arriving on @p ingress_interface; @p source is
/// the addressed STREAM_OUTPUT or nullptr. Responses per Milan v1.2 Tables 5.40-5.48; nullopt when
/// @p cmd is not a talker command.
[[nodiscard]] auto milan_talker_respond(
    AcmpCommandResponse const& cmd, MilanTalkerSource const* source, uint16_t ingress_interface) noexcept
    -> std::optional<MilanTalkerReply>;

}  // namespace statusbar::atdecc
