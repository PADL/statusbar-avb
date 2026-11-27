// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_milan_mvu_stream_input.hpp"

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_milan_acmp_listener_sm.hpp"

#include <cstdint>

namespace statusbar::atdecc {

namespace {

/// Who sends @p cmd and which sink it is for: the controller and the target
/// of @p header, and @p sink (Table 5.22).
void set_parties(AcmpCommandResponse& cmd, AecpMvuDu const& header, uint16_t const sink) noexcept
{
    cmd.controller_entity_id = header.common.controller_entity_id;
    cmd.listener_entity_id = header.common.target_entity_id;
    cmd.listener_unique_id = sink;
}

/// The ACMP command of @p message_type from the controller of @p header to
/// its target's sink @p sink: what both binding commands share.
[[nodiscard]] auto sink_command(AecpMvuDu const& header, uint8_t const message_type, uint16_t const sink) noexcept
    -> AcmpCommandResponse
{
    AcmpCommandResponse cmd{};
    cmd.init_command(message_type);
    set_parties(cmd, header, sink);
    cmd.sequence_id = header.common.sequence_id.get();
    return cmd;
}

}  // namespace

auto milan_bind_command_from_mvu(AecpMvuDu const& header, MvuBindStream const& bind) noexcept -> AcmpCommandResponse
{
    AcmpCommandResponse cmd = sink_command(header, ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, bind.descriptor_index.get());
    cmd.talker_entity_id = bind.talker_entity_id;
    cmd.talker_unique_id = bind.talker_stream_index.get();
    cmd.flags = bind.streaming_wait() ? acmp_flags::STREAMING_WAIT : uint16_t{0};
    return cmd;
}

auto milan_unbind_command_from_mvu(AecpMvuDu const& header, MvuStreamInput const& unbind) noexcept -> AcmpCommandResponse
{
    return sink_command(header, ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, unbind.descriptor_index.get());
}

auto mvu_status_for_binding_response(uint8_t const acmp_status) noexcept -> uint8_t
{
    switch (acmp_status) {
        case ACMP_STATUS_SUCCESS:
            return MVU_STATUS_SUCCESS;
        case ACMP_STATUS_LISTENER_UNKNOWN_ID:
            return MVU_STATUS_NO_SUCH_DESCRIPTOR;
        case ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED:
            return MVU_STATUS_ENTITY_LOCKED;
        default:
            return MVU_STATUS_ENTITY_MISBEHAVING;
    }
}

auto milan_stream_input_info_ex(MilanSinkContext const& ctx) noexcept -> MvuGetStreamInputInfoExResponse
{
    MvuGetStreamInputInfoExResponse info{};
    info.descriptor_type = aem::DESCRIPTOR_STREAM_INPUT;
    info.descriptor_index = ctx.index;
    if (ctx.binding.has_value()) {
        info.talker_entity_id = ctx.binding->talker_entity_id;
        info.talker_unique_id = ctx.binding->talker_unique_id;
    }
    info.set_status(static_cast<uint8_t>(ctx.probing_status), ctx.acmp_status);
    return info;
}

auto milan_stream_input_notified(MilanSinkContext const& ctx) -> MilanStreamInputNotified
{
    bool const asks = ctx.srp_active && ctx.env != nullptr && ctx.env->talker_registration;
    return MilanStreamInputNotified{
        .info = milan_stream_input_info_ex(ctx),
        .talker_registration = asks ? ctx.env->talker_registration(ctx.srp) : MilanTalkerRegistration::None};
}

}  // namespace statusbar::atdecc
