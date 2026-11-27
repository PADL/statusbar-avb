#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Milan v1.3's stream input commands over AECP MVU, in the sink's terms.
///
/// BIND_STREAM and UNBIND_STREAM (5.4.4.6, 5.4.4.7) are a second way for a
/// controller to say what ACMP's BIND_RX and UNBIND_RX commands say. The
/// sink state machine has one event for both (RCV_BIND_RX_CMD and
/// RCV_UNBIND_RX_CMD, Table 5.30), and 5.5.3.2 sends an MVU command through
/// it exactly as 5.5.3.1 sends the ACMP one. So nothing here is a second
/// treatment: an MVU command is turned into the ACMP command it equals
/// (Table 5.22), the sink treats that, and the status of the ACMP response
/// it would have sent becomes the status of the MVU response.
///
/// GET_STREAM_INPUT_INFO_EX (5.4.4.8) reads a sink's variables, and Table
/// 5.21 has the same response sent as a notification when they change.

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/atdecc/atdecc_milan_acmp_listener_sm.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <cstdint>

namespace statusbar::atdecc {

/// The BIND_RX_COMMAND that the BIND_STREAM command @p header with the data
/// @p bind equals, by Table 5.22: the target is the listener, the
/// descriptor_index its listener_unique_id, the talker_stream_index the
/// talker_unique_id, and STREAMING_WAIT is the one flag both have.
[[nodiscard]] auto milan_bind_command_from_mvu(AecpMvuDu const& header, MvuBindStream const& bind) noexcept -> AcmpCommandResponse;

/// The UNBIND_RX_COMMAND that the UNBIND_STREAM command @p header with the
/// data @p unbind equals.
[[nodiscard]] auto milan_unbind_command_from_mvu(AecpMvuDu const& header, MvuStreamInput const& unbind) noexcept
    -> AcmpCommandResponse;

/// The status of the BIND_STREAM or UNBIND_STREAM response that stands for
/// the sink's BIND_RX or UNBIND_RX response of @p acmp_status: SUCCESS
/// (Tables 5.35, 5.41), NO_SUCH_DESCRIPTOR for a sink that does not exist
/// (Table 5.28) and ENTITY_LOCKED for a controller the lock shuts out
/// (Tables 5.33, 5.39). The sink gives no other answer; one would be
/// ENTITY_MISBEHAVING.
[[nodiscard]] auto mvu_status_for_binding_response(uint8_t acmp_status) noexcept -> uint8_t;

/// The data of the GET_STREAM_INPUT_INFO_EX response for the sink @p ctx:
/// the talker it is bound to, its Probing status and its ACMP status
/// (5.3.8.6). descriptor_type is STREAM_INPUT and descriptor_index the
/// sink's own.
[[nodiscard]] auto milan_stream_input_info_ex(MilanSinkContext const& ctx) noexcept -> MvuGetStreamInputInfoExResponse;

/// What Table 5.21 names of a stream input: a GET_STREAM_INPUT_INFO_EX
/// notification is due when any of it changes. The talker, the Probing
/// status and the ACMP status are the response's own fields. The
/// registration of the Talker attribute is named with them and is no field
/// of the response, so a notification for it alone repeats the last one.
struct MilanStreamInputNotified
{
    MvuGetStreamInputInfoExResponse info{};
    MilanTalkerRegistration talker_registration{MilanTalkerRegistration::None};

    auto operator==(MilanStreamInputNotified const&) const noexcept -> bool = default;
};

/// That of the sink with the variables @p ctx, now. The registration is
/// asked of the listener the sink belongs to, for a settled sink's SRP
/// parameters; a sink that is not settled has none to match and registers
/// nothing.
[[nodiscard]] auto milan_stream_input_notified(MilanSinkContext const& ctx) -> MilanStreamInputNotified;

}  // namespace statusbar::atdecc
