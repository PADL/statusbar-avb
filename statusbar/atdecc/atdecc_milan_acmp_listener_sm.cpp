// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Milan v1.3 listener sink state machine: PDUs, actions and event selection.

#include "statusbar/atdecc/atdecc_milan_acmp_listener_sm.hpp"

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/sm/sm_core.hpp"

#include <algorithm>
#include <chrono>
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

void set_stream_fields(AcmpCommandResponse& pdu, MilanSrpParameters const& srp) noexcept
{
    pdu.stream_id = srp.stream_id;
    pdu.stream_dest_mac = srp.stream_dest_mac;
    pdu.stream_vlan_id = srp.stream_vlan_id;
}

void set_talker_fields(AcmpCommandResponse& pdu, ieee::Eui64 const& talker_entity_id, uint16_t const talker_unique_id) noexcept
{
    pdu.talker_entity_id = talker_entity_id;
    pdu.talker_unique_id = talker_unique_id;
}

/// The flags of a bound sink's GET_RX_STATE_RESPONSE (Tables 5.42 to 5.44).
[[nodiscard]] auto bound_rx_state_flags(MilanBinding const& binding, bool const registering_failed) noexcept -> uint16_t
{
    uint16_t flags = acmp_flags::FAST_CONNECT;
    if (binding.streaming_wait) {
        flags |= acmp_flags::STREAMING_WAIT;
    }
    if (registering_failed) {
        flags |= acmp_flags::SRP_REGISTRATION_FAILED;
    }
    return flags;
}

/// The talker, connection_count and flags of a GET_RX_STATE_RESPONSE: the
/// binding's when bound (Tables 5.42 to 5.44), zero when not (Table 5.37).
void set_rx_state_binding(
    AcmpCommandResponse& resp, std::optional<MilanBinding> const& binding, bool const registering_failed) noexcept
{
    bool const bound = binding.has_value();
    set_talker_fields(resp, bound ? binding->talker_entity_id : ieee::Eui64{}, bound ? binding->talker_unique_id : uint16_t{0});
    resp.connection_count = bound ? uint16_t{1} : uint16_t{0};
    resp.flags = bound ? bound_rx_state_flags(*binding, registering_failed) : uint16_t{0};
}

void set_probe_identity(AcmpCommandResponse& cmd, MilanBinding const& binding, ieee::Eui64 const& my_id) noexcept
{
    cmd.controller_entity_id = binding.controller_entity_id;
    set_talker_fields(cmd, binding.talker_entity_id, binding.talker_unique_id);
    cmd.listener_entity_id = my_id;
    cmd.listener_unique_id = binding.listener_unique_id;
}

[[nodiscard]] auto names_bound_source(MilanSinkContext const& ctx, AcmpCommandResponse const& cmd) noexcept -> bool
{
    return ctx.binding.has_value() && ctx.binding->talker_entity_id == cmd.talker_entity_id &&
        ctx.binding->talker_unique_id == cmd.talker_unique_id.get();
}

[[nodiscard]] auto bind_event(MilanSinkContext const& ctx, AcmpCommandResponse const& cmd, bool const locked_out) noexcept
    -> MilanSinkEvent
{
    if (locked_out) {
        return MilanSinkEvent::BindRefused;
    }
    return names_bound_source(ctx, cmd) ? MilanSinkEvent::BindSameTalker : MilanSinkEvent::BindNewTalker;
}

/// 5.5.3.6.18 step 1: the fields a PROBE_TX_RESPONSE must share with the command sent.
[[nodiscard]] auto answers_probe(AcmpCommandResponse const& probe, AcmpCommandResponse const& resp) noexcept -> bool
{
    return resp.controller_entity_id == probe.controller_entity_id && resp.talker_entity_id == probe.talker_entity_id &&
        resp.talker_unique_id.get() == probe.talker_unique_id.get() && resp.sequence_id.get() == probe.sequence_id.get();
}

}  // namespace

auto milan_unknown_sink_response(AcmpCommandResponse const& cmd) noexcept -> AcmpCommandResponse
{
    return response_to(cmd, ACMP_STATUS_LISTENER_UNKNOWN_ID);
}

auto milan_not_authorized_response(AcmpCommandResponse const& cmd) noexcept -> AcmpCommandResponse
{
    return response_to(cmd, ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
}

auto milan_bind_response(AcmpCommandResponse const& cmd) noexcept -> AcmpCommandResponse
{
    AcmpCommandResponse resp = response_to(cmd, ACMP_STATUS_SUCCESS);
    resp.connection_count = 1;
    resp.flags = static_cast<uint16_t>(cmd.flags.get() & acmp_flags::STREAMING_WAIT);
    set_stream_fields(resp, MilanSrpParameters{});
    return resp;
}

auto milan_unbind_response(AcmpCommandResponse const& cmd) noexcept -> AcmpCommandResponse
{
    AcmpCommandResponse resp = response_to(cmd, ACMP_STATUS_SUCCESS);
    set_talker_fields(resp, ieee::Eui64{}, 0);
    resp.connection_count = 0;
    resp.flags = 0;
    set_stream_fields(resp, MilanSrpParameters{});
    return resp;
}

auto milan_probe_command(MilanBinding const& binding, ieee::Eui64 const& my_id, uint16_t const sequence_id) noexcept
    -> AcmpCommandResponse
{
    AcmpCommandResponse cmd{};
    cmd.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    set_probe_identity(cmd, binding, my_id);
    cmd.sequence_id = sequence_id;
    cmd.flags = acmp_flags::FAST_CONNECT;
    return cmd;
}

auto milan_rx_state_response(AcmpCommandResponse const& cmd, MilanSinkContext const& ctx, bool const registering_failed) noexcept
    -> AcmpCommandResponse
{
    AcmpCommandResponse resp = response_to(cmd, ACMP_STATUS_SUCCESS);
    set_rx_state_binding(resp, ctx.binding, registering_failed);
    set_stream_fields(resp, ctx.srp);
    return resp;
}

namespace milan_sink_actions {

namespace {

void set_status(MilanSinkContext& ctx, MilanProbingStatus const probing, uint8_t const acmp) noexcept
{
    ctx.probing_status = probing;
    ctx.acmp_status = acmp;
}

void send_response(MilanSinkContext& ctx, AcmpCommandResponse const& resp)
{
    if (ctx.env->tx_response) {
        (void)ctx.env->tx_response(resp);
    }
}

void save_binding(MilanSinkContext& ctx)
{
    if (ctx.env->binding_changed) {
        ctx.env->binding_changed(ctx.index);
    }
}

/// "Clear the SRP parameters and stop SRP", in the states that have any.
void drop_srp(MilanSinkContext& ctx, sm::TimePoint const now)
{
    ctx.srp = MilanSrpParameters{};
    if (!ctx.srp_active) {
        return;
    }
    ctx.srp_active = false;
    if (ctx.env->stop_srp) {
        ctx.env->stop_srp(ctx.index, now);
    }
}

/// What a bind to another source and an unbind both undo first: the
/// reservation, the discovery of the old talker, the state's timer.
void release_binding_state(MilanSinkContext& ctx, sm::TimePoint const now)
{
    drop_srp(ctx, now);
    ctx.discovery_stop_requested = true;
    ctx.timer.reset();
}

/// The binding parameters a BIND_RX_COMMAND carries, for this sink.
[[nodiscard]] auto binding_from(MilanSinkContext const& ctx, AcmpCommandResponse const& cmd) noexcept -> MilanBinding
{
    return MilanBinding{
        .listener_unique_id = ctx.index,
        .talker_entity_id = cmd.talker_entity_id,
        .talker_unique_id = cmd.talker_unique_id.get(),
        .controller_entity_id = cmd.controller_entity_id,
        .streaming_wait = cmd.is_streaming_wait()};
}

/// Update the binding from the command, save it, then answer the controller.
void take_binding(MilanSinkContext& ctx)
{
    ctx.binding = binding_from(ctx, ctx.env->rcvd);
    save_binding(ctx);
    send_response(ctx, milan_bind_response(ctx.env->rcvd));
}

/// Send the saved PROBE_TX_COMMAND and wait TMR_NO_RESP for its response.
void transmit_probe(MilanSinkContext& ctx, sm::TimePoint const now)
{
    if (ctx.env->tx_command) {
        (void)ctx.env->tx_command(ctx.probe);
    }
    ctx.timer = now + MILAN_TMR_NO_RESP;
}

void send_new_probe(MilanSinkContext& ctx, sm::TimePoint const now)
{
    ctx.probe = milan_probe_command(*ctx.binding, ctx.env->my_id, ctx.env->next_sequence_id++);
    transmit_probe(ctx, now);
}

/// Start TMR_DELAY: a random 0 to 1 second.
void arm_delay(MilanSinkContext& ctx, sm::TimePoint const now)
{
    auto const drawn = ctx.env->random_delay ? ctx.env->random_delay() : std::chrono::milliseconds{0};
    ctx.timer = now + std::clamp(drawn, std::chrono::milliseconds{0}, MILAN_TMR_DELAY_MAX);
}

}  // namespace

void start_bound(MilanSinkContext& ctx, sm::TimePoint /*now*/)
{
    // 5.5.3.6.2: the binding is the saved one, already in ctx.binding.
    ctx.discovery_start_requested = true;
    ctx.talker_discovered = false;
    set_status(ctx, MilanProbingStatus::Passive, ACMP_STATUS_SUCCESS);
}

void refuse_command(MilanSinkContext& ctx, sm::TimePoint /*now*/)
{
    // Step 1 of every bind and unbind treatment (Tables 5.32 and 5.38).
    send_response(ctx, milan_not_authorized_response(ctx.env->rcvd));
}

void update_binding(MilanSinkContext& ctx, sm::TimePoint /*now*/)
{
    // Step 2 of a bind in a bound state: the same source again. Only the
    // controller and STREAMING_WAIT are taken; the probing goes on as it was.
    ctx.binding->controller_entity_id = ctx.env->rcvd.controller_entity_id;
    ctx.binding->streaming_wait = ctx.env->rcvd.is_streaming_wait();
    save_binding(ctx);
    send_response(ctx, milan_bind_response(ctx.env->rcvd));
}

void bind_and_probe(MilanSinkContext& ctx, sm::TimePoint const now)
{
    // 5.5.3.6.3, and from step 3 of the bind treatment of every bound state.
    release_binding_state(ctx, now);
    take_binding(ctx);
    ctx.discovery_start_requested = true;
    ctx.talker_discovered = false;
    send_new_probe(ctx, now);
    set_status(ctx, MilanProbingStatus::Active, ACMP_STATUS_SUCCESS);
}

void answer_unbound(MilanSinkContext& ctx, sm::TimePoint /*now*/)
{
    // 5.5.3.6.5: nothing to undo.
    send_response(ctx, milan_unbind_response(ctx.env->rcvd));
}

void unbind(MilanSinkContext& ctx, sm::TimePoint const now)
{
    // 5.5.3.6.8, .13, .20, .27, .33, .39, .45
    release_binding_state(ctx, now);
    ctx.binding.reset();
    save_binding(ctx);
    set_status(ctx, MilanProbingStatus::Disabled, ACMP_STATUS_SUCCESS);
    send_response(ctx, milan_unbind_response(ctx.env->rcvd));
}

void answer_rx_state(MilanSinkContext& ctx, sm::TimePoint /*now*/)
{
    // Tables 5.37, 5.42 and 5.43: REGISTERING_FAILED is 0 in every state but SETTLED_RSV_OK.
    send_response(ctx, milan_rx_state_response(ctx.env->rcvd, ctx, false));
}

void answer_rx_state_reserved(MilanSinkContext& ctx, sm::TimePoint /*now*/)
{
    // Table 5.44: REGISTERING_FAILED when the matching attribute is a Talker Failed.
    bool const failed = ctx.env->talker_registration && ctx.env->talker_registration(ctx.srp) == MilanTalkerRegistration::Failed;
    send_response(ctx, milan_rx_state_response(ctx.env->rcvd, ctx, failed));
}

void start_delay(MilanSinkContext& ctx, sm::TimePoint const now)
{
    // 5.5.3.6.9
    ctx.talker_discovered = true;
    arm_delay(ctx, now);
    set_status(ctx, MilanProbingStatus::Active, ACMP_STATUS_SUCCESS);
}

void delay_again(MilanSinkContext& ctx, sm::TimePoint const now)
{
    // 5.5.3.6.30 step 2: the ACMP status keeps what the last probe ended with.
    arm_delay(ctx, now);
}

void send_probe(MilanSinkContext& ctx, sm::TimePoint const now)
{
    // 5.5.3.6.10
    send_new_probe(ctx, now);
}

void resend_probe(MilanSinkContext& ctx, sm::TimePoint const now)
{
    // 5.5.3.6.16: a duplicate of the command sent the first time.
    transmit_probe(ctx, now);
}

void note_probe_timeout(MilanSinkContext& ctx, sm::TimePoint const now)
{
    // 5.5.3.6.23
    ctx.timer = now + MILAN_TMR_RETRY;
    ctx.acmp_status = ACMP_STATUS_LISTENER_TALKER_TIMEOUT;
}

void note_probe_rejected(MilanSinkContext& ctx, sm::TimePoint const now)
{
    // 5.5.3.6.18 step 3
    ctx.timer = now + MILAN_TMR_RETRY;
    ctx.acmp_status = ctx.env->rcvd.status();
}

void settle(MilanSinkContext& ctx, sm::TimePoint const now)
{
    // 5.5.3.6.18 step 4
    auto const& resp = ctx.env->rcvd;
    ctx.srp = MilanSrpParameters{
        .stream_id = resp.stream_id, .stream_dest_mac = resp.stream_dest_mac, .stream_vlan_id = resp.stream_vlan_id.get()};
    ctx.srp_active = true;
    if (ctx.env->start_srp) {
        ctx.env->start_srp(ctx.index, ctx.srp, now);
    }
    ctx.timer = now + MILAN_TMR_NO_TK;
    set_status(ctx, MilanProbingStatus::Completed, ACMP_STATUS_SUCCESS);
}

void wait_for_talker(MilanSinkContext& ctx, sm::TimePoint /*now*/)
{
    // 5.5.3.6.15, .22, .29, .35, and .30 step 1
    ctx.talker_discovered = false;
    ctx.timer.reset();
    set_status(ctx, MilanProbingStatus::Passive, ACMP_STATUS_SUCCESS);
}

void note_discovered(MilanSinkContext& ctx, sm::TimePoint /*now*/)
{
    ctx.talker_discovered = true;
}

void note_departed(MilanSinkContext& ctx, sm::TimePoint /*now*/)
{
    // 5.5.3.6.41 and .47: a settled sink keeps its stream until SRP says otherwise.
    ctx.talker_discovered = false;
}

void note_registered(MilanSinkContext& ctx, sm::TimePoint /*now*/)
{
    // 5.5.3.6.42
    ctx.timer.reset();
}

void drop_srp_and_delay(MilanSinkContext& ctx, sm::TimePoint const now)
{
    // 5.5.3.6.36 and .48, the talker discovered
    drop_srp(ctx, now);
    set_status(ctx, MilanProbingStatus::Active, ACMP_STATUS_SUCCESS);
    arm_delay(ctx, now);
}

void drop_srp_and_wait(MilanSinkContext& ctx, sm::TimePoint const now)
{
    // 5.5.3.6.36 and .48, the talker not discovered
    drop_srp(ctx, now);
    ctx.timer.reset();
    set_status(ctx, MilanProbingStatus::Passive, ACMP_STATUS_SUCCESS);
}

}  // namespace milan_sink_actions

auto milan_sink_event_for_command(MilanSinkContext const& ctx, AcmpCommandResponse const& cmd, bool const locked_out) noexcept
    -> std::optional<MilanSinkEvent>
{
    switch (cmd.message_type()) {
        case ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND:
            return bind_event(ctx, cmd, locked_out);
        case ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND:
            return locked_out ? MilanSinkEvent::UnbindRefused : MilanSinkEvent::Unbind;
        case ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND:
            return MilanSinkEvent::GetRxState;
        default:
            return std::nullopt;
    }
}

auto milan_sink_event_for_probe_response(MilanSinkContext const& ctx, AcmpCommandResponse const& resp) noexcept
    -> std::optional<MilanSinkEvent>
{
    if (resp.message_type() != ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE || !answers_probe(ctx.probe, resp)) {
        return std::nullopt;
    }
    return resp.status() == ACMP_STATUS_SUCCESS ? MilanSinkEvent::ProbeAccepted : MilanSinkEvent::ProbeRejected;
}

auto milan_sink_timer_event(MilanSinkContext const& ctx, MilanSinkState const state) noexcept -> std::optional<MilanSinkEvent>
{
    switch (state) {
        case MilanSinkState::PrbWDelay:
            return MilanSinkEvent::TmrDelay;
        case MilanSinkState::PrbWResp:
        case MilanSinkState::PrbWResp2:
            return MilanSinkEvent::TmrNoResp;
        case MilanSinkState::PrbWRetry:
            return ctx.talker_discovered ? MilanSinkEvent::TmrRetry : MilanSinkEvent::TmrRetryNoTalker;
        case MilanSinkState::SettledNoRsv:
            return ctx.talker_discovered ? MilanSinkEvent::TmrNoTk : MilanSinkEvent::TmrNoTkNoTalker;
        default:
            return std::nullopt;
    }
}

auto milan_sink_registration_event(MilanSinkContext const& ctx, MilanSinkState const state, bool const registered) noexcept
    -> std::optional<MilanSinkEvent>
{
    if (state == MilanSinkState::SettledNoRsv && registered) {
        return MilanSinkEvent::TalkerRegistered;
    }
    if (state == MilanSinkState::SettledRsvOk && !registered) {
        return ctx.talker_discovered ? MilanSinkEvent::TalkerUnregistered : MilanSinkEvent::TalkerUnregisteredNoTalker;
    }
    return std::nullopt;
}

}  // namespace statusbar::atdecc
