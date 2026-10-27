// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_acmp_milan_listener.hpp"

#include "statusbar/atdecc/atdecc_acmp_milan_talker.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace statusbar::atdecc {

namespace probing_status = aem::probing_status;

auto milan_sink_state_name(MilanSinkState const state) noexcept -> std::string_view
{
    switch (state) {
        case MilanSinkState::unbound:
            return "UNBOUND";
        case MilanSinkState::prb_w_avail:
            return "PRB_W_AVAIL";
        case MilanSinkState::prb_w_delay:
            return "PRB_W_DELAY";
        case MilanSinkState::prb_w_resp:
            return "PRB_W_RESP";
        case MilanSinkState::prb_w_resp2:
            return "PRB_W_RESP2";
        case MilanSinkState::prb_w_retry:
            return "PRB_W_RETRY";
        case MilanSinkState::settled_no_rsv:
            return "SETTLED_NO_RSV";
        case MilanSinkState::settled_rsv_ok:
            return "SETTLED_RSV_OK";
    }
    return "?";
}

MilanListener::MilanListener(ieee::Eui64 const entity_id, size_t const sinks)
    : entity_id_{entity_id}
    , rng_{rng_seed(entity_id)}
{
    for (size_t i = 0; i < sinks && i < MAX_SINKS; ++i) {
        sinks_.push_back(MilanSink{});
        notified_.push_back(Status{});
    }
}

namespace {

/// The response to @p cmd: every field echoed, the message type the
/// command's response, the fields of Table 5.25 cleared.
auto echo(AcmpCommandResponse const& cmd, uint8_t const status) noexcept -> AcmpCommandResponse
{
    AcmpCommandResponse resp = cmd;
    resp.set_message_type(static_cast<uint8_t>(cmd.message_type() + 1));
    resp.set_status(status);
    return resp;
}

void clear_stream(AcmpCommandResponse& resp) noexcept
{
    resp.stream_id = ieee::Eui64{};
    resp.stream_dest_mac = ieee::Eui48{};
    resp.stream_vlan_id = uint16_t{0};
}

/// The talker, connection count and flags of a response.
void set_talker(AcmpCommandResponse& resp, MilanSinkBinding const& binding, uint16_t const count, uint16_t const flags) noexcept
{
    resp.talker_entity_id = binding.talker_entity_id;
    resp.talker_unique_id = binding.talker_unique_id;
    resp.connection_count = count;
    resp.flags = flags;
}

/// Table 5.32: the response to a BIND_RX_COMMAND.
auto bind_response(AcmpCommandResponse const& cmd) noexcept -> AcmpCommandResponse
{
    auto resp = echo(cmd, ACMP_STATUS_SUCCESS);
    resp.connection_count = uint16_t{1};
    resp.flags = static_cast<uint16_t>(cmd.is_streaming_wait() ? acmp_flags::STREAMING_WAIT : 0U);
    clear_stream(resp);
    return resp;
}

/// Tables 5.34 and 5.36: the response to GET_RX_STATE or UNBIND_RX for an unbound sink.
auto unbound_response(AcmpCommandResponse const& cmd) noexcept -> AcmpCommandResponse
{
    auto resp = echo(cmd, ACMP_STATUS_SUCCESS);
    set_talker(resp, MilanSinkBinding{}, 0, 0);
    clear_stream(resp);
    return resp;
}

/// The flags GET_RX_STATE reports for bound sink @p s.
auto rx_state_flags(MilanSink const& s) noexcept -> uint16_t
{
    uint16_t flags = acmp_flags::FAST_CONNECT;
    if (!s.binding.started) {
        flags |= acmp_flags::STREAMING_WAIT;
    }
    if (s.state == MilanSinkState::settled_rsv_ok && s.registration == MilanTalkerRegistration::failed) {
        flags |= acmp_flags::REGISTERING_FAILED;
    }
    return flags;
}

/// Tables 5.37 (probing), 5.38 (SETTLED_NO_RSV), 5.39 (SETTLED_RSV_OK): GET_RX_STATE for bound sink @p s.
auto bound_response(AcmpCommandResponse const& cmd, MilanSink const& s) noexcept -> AcmpCommandResponse
{
    auto resp = echo(cmd, ACMP_STATUS_SUCCESS);
    set_talker(resp, s.binding, 1, rx_state_flags(s));
    clear_stream(resp);
    if (s.settled()) {
        resp.stream_id = s.stream.stream_id;
        resp.stream_dest_mac = s.stream.dest_mac;
        resp.stream_vlan_id = s.stream.vlan_id;
    }
    return resp;
}

/// Sections 5.5.3.5.18 and 5.5.3.5.25 step 1: @p resp answers the probe @p s
/// sent and is waiting for (Table 5.30: ignored in every other state).
auto answers_probe(MilanSink const& s, AcmpCommandResponse const& resp) noexcept -> bool
{
    return (s.state == MilanSinkState::prb_w_resp || s.state == MilanSinkState::prb_w_resp2) &&
        resp.controller_entity_id == s.probe.controller_entity_id && resp.talker_entity_id == s.probe.talker_entity_id &&
        resp.talker_unique_id.get() == s.probe.talker_unique_id.get() && resp.sequence_id.get() == s.probe.sequence_id.get();
}

/// Sink @p s is bound to the talker and source @p cmd names.
auto bound_to(MilanSink const& s, AcmpCommandResponse const& cmd) noexcept -> bool
{
    return s.bound() && s.binding.talker_entity_id == cmd.talker_entity_id &&
        s.binding.talker_unique_id == cmd.talker_unique_id.get();
}

/// Enter @p state with probing status @p probing and no ACMP status.
void enter(MilanSink& s, uint8_t const probing, MilanSinkState const state) noexcept
{
    s.probing_status = probing;
    s.acmp_status = 0;
    s.state = state;
}

}  // namespace

auto MilanListener::receive(AcmpCommandResponse const& pdu, TimePoint const now) -> bool
{
    if (pdu.listener_entity_id != entity_id_) {
        return false;
    }
    uint8_t const type = pdu.message_type();
    uint16_t const index = pdu.listener_unique_id.get();
    if (type == ACMP_MESSAGE_TYPE_PROBE_TX_RESPONSE) {
        // Section 5.5.3.1: a response for no sink is ignored.
        if (index >= sinks_.size()) {
            return false;
        }
        probe_response(index, pdu, now);
        return true;
    }
    return receive_command(type, index, pdu, now);
}

auto MilanListener::receive_command(uint8_t const type, uint16_t const index, AcmpCommandResponse const& pdu, TimePoint const now)
    -> bool
{
    if (type != ACMP_MESSAGE_TYPE_BIND_RX_COMMAND && type != ACMP_MESSAGE_TYPE_UNBIND_RX_COMMAND &&
        type != ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND) {
        return false;  // "The listener shall ignore all other ACMP messages."
    }
    if (index >= sinks_.size()) {
        respond(pdu, ACMP_STATUS_LISTENER_UNKNOWN_ID);  // Table 5.27
        return true;
    }
    switch (type) {
        case ACMP_MESSAGE_TYPE_BIND_RX_COMMAND:
            bind(index, pdu, now);
            break;
        case ACMP_MESSAGE_TYPE_UNBIND_RX_COMMAND:
            unbind(index, pdu);
            break;
        default:
            get_rx_state(index, pdu);
            break;
    }
    return true;
}

void MilanListener::respond(AcmpCommandResponse const& cmd, uint8_t const status)
{
    send(echo(cmd, status));
}

auto MilanListener::restore(uint16_t const index, MilanSinkBinding const& binding) -> bool
{
    if (index >= sinks_.size() || sinks_[index].bound()) {
        return false;
    }
    // The ADP discovery state machine starts: not discovered.
    sinks_[index] = MilanSink{.state = MilanSinkState::prb_w_avail, .binding = binding, .probing_status = probing_status::PASSIVE};
    notify(index);
    return true;
}

void MilanListener::bind(uint16_t const index, AcmpCommandResponse const& cmd, TimePoint const now)
{
    // Step 1 of each RCV_BIND_RX_CMD row (Table 5.31).
    if (!authorized(cmd.controller_entity_id)) {
        respond(cmd, ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
        return;
    }
    if (bound_to(sinks_[index], cmd)) {
        update_binding(index, cmd);
        send(bind_response(cmd));
        notify(index);
        return;
    }
    rebind(index, cmd);
    send(bind_response(cmd));
    send_probe(index, now);
    notify(index);
}

void MilanListener::update_binding(uint16_t const index, AcmpCommandResponse const& cmd)
{
    auto& s = sinks_[index];
    bool const started = !cmd.is_streaming_wait();
    bool const changed = s.binding.controller_entity_id != cmd.controller_entity_id || s.binding.started != started;
    s.binding.controller_entity_id = cmd.controller_entity_id;
    s.binding.started = started;
    if (changed) {
        binding_changed(index);
    }
}

void MilanListener::rebind(uint16_t const index, AcmpCommandResponse const& cmd)
{
    stop(index);
    auto& s = sinks_[index];
    s.binding = MilanSinkBinding{
        .talker_entity_id = cmd.talker_entity_id,
        .talker_unique_id = cmd.talker_unique_id.get(),
        .controller_entity_id = cmd.controller_entity_id,
        .started = !cmd.is_streaming_wait()};
    // Restart discovery and probe at once: an immediate consequence of a bind skips
    // waiting for ADP (Section 5.5.2.6). Bound before the hook runs: it sees the new state.
    enter(s, probing_status::ACTIVE, MilanSinkState::prb_w_resp);
    binding_changed(index);
}

void MilanListener::stop(uint16_t const index)
{
    auto& s = sinks_[index];
    s.timer_running = false;
    if (s.settled()) {
        unsettle(index);
    }
    s.discovered = false;
}

void MilanListener::unbind(uint16_t const index, AcmpCommandResponse const& cmd)
{
    if (!authorized(cmd.controller_entity_id)) {
        respond(cmd, ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);  // Table 5.35
        return;
    }
    if (sinks_[index].bound()) {
        stop(index);
        sinks_[index] = MilanSink{.probing_status = probing_status::DISABLED};  // binding cleared, discovery stopped
        binding_changed(index);
    }
    // Table 5.36. No DISCONNECT_TX: the talker learns from MSRP.
    send(unbound_response(cmd));
    notify(index);
}

void MilanListener::get_rx_state(uint16_t const index, AcmpCommandResponse const& cmd)
{
    auto const& s = sinks_[index];
    send(s.bound() ? bound_response(cmd, s) : unbound_response(cmd));
}

auto MilanListener::probe_command(uint16_t const index) -> AcmpCommandResponse
{  // NOLINT(statusbar-block-statements): one assignment per field of Table 5.33
    auto const& binding = sinks_[index].binding;
    AcmpCommandResponse cmd{};
    cmd.init_command(ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND);
    cmd.controller_entity_id = binding.controller_entity_id;
    cmd.talker_entity_id = binding.talker_entity_id;
    cmd.listener_entity_id = entity_id_;
    cmd.talker_unique_id = binding.talker_unique_id;
    cmd.listener_unique_id = index;
    cmd.sequence_id = sequence_id_++;
    cmd.connection_count = uint16_t{0};
    cmd.flags = acmp_flags::FAST_CONNECT;
    return cmd;
}

void MilanListener::send_probe(uint16_t const index, TimePoint const now)
{
    auto& s = sinks_[index];
    // A new sequence_id for each new probe; the duplicate sent on TMR_NO_RESP
    // is this message unchanged.
    auto const cmd = probe_command(index);
    s.probe = cmd;
    // The timer first: a response delivered within send_ settles the sink.
    start_timer(s, now, TMR_NO_RESP);
    send(cmd);
}

void MilanListener::probe_response(uint16_t const index, AcmpCommandResponse const& resp, TimePoint const now)
{
    auto& s = sinks_[index];
    if (!answers_probe(s, resp)) {
        return;
    }
    s.timer_running = false;
    if (resp.status() != ACMP_STATUS_SUCCESS) {
        // Step 3: a refusal (TALKER_DEST_MAC_FAIL before the talker's MAAP,
        // TALKER_UNKNOWN_ID, ...) never settles; probe again after TMR_RETRY.
        start_timer(s, now, TMR_RETRY);
        s.acmp_status = resp.status();
        s.state = MilanSinkState::prb_w_retry;
        notify(index);
        return;
    }
    // Step 4.
    s.stream = MilanSinkStream{.stream_id = resp.stream_id, .dest_mac = resp.stream_dest_mac, .vlan_id = resp.stream_vlan_id.get()};
    settle(index, now);
}

void MilanListener::settle(uint16_t const index, TimePoint const now)
{
    auto& s = sinks_[index];
    s.registration = MilanTalkerRegistration::none;
    enter(s, probing_status::COMPLETED, MilanSinkState::settled_no_rsv);
    start_timer(s, now, TMR_NO_TK);
    notify(index);
    if (on_settled_) {
        on_settled_(index, s.stream);
    }
}

void MilanListener::unsettle(uint16_t const index)
{
    auto& s = sinks_[index];
    MilanSinkStream const was = s.stream;
    s.stream = MilanSinkStream{};
    s.registration = MilanTalkerRegistration::none;
    // No longer settled before the hook runs (the caller sets the state that
    // follows): a registration the hook reports back is ignored.
    s.state = MilanSinkState::prb_w_avail;
    if (on_unsettled_) {
        on_unsettled_(index, was);
    }
}

void MilanListener::reprobe(uint16_t const index, TimePoint const now)
{
    auto& s = sinks_[index];
    if (!s.discovered) {
        s.timer_running = false;
        enter(s, probing_status::PASSIVE, MilanSinkState::prb_w_avail);
        return;
    }
    s.probing_status = probing_status::ACTIVE;
    start_delay(s, now);
}

void MilanListener::start_delay(MilanSink& s, TimePoint const now)
{
    Duration delay{};
    if (delay_) {
        delay = delay_();
    } else {
        std::uniform_int_distribution<int64_t> pick{TMR_DELAY_MIN.count(), TMR_DELAY_MAX.count()};
        delay = Duration{pick(rng_)};
    }
    start_timer(s, now, delay);
    s.state = MilanSinkState::prb_w_delay;
}

void MilanListener::start_timer(MilanSink& s, TimePoint const now, Duration const duration) noexcept
{
    s.timer_running = true;
    s.deadline = now + std::chrono::duration_cast<TimePoint::duration>(duration);
}

void MilanListener::set_registration(uint16_t const index, MilanTalkerRegistration const registration, TimePoint const now)
{
    if (index >= sinks_.size() || !sinks_[index].settled()) {
        return;  // Section 5.3.8.8: not registering unless settled
    }
    auto& s = sinks_[index];
    bool const was = s.registering();
    s.registration = registration;
    if (s.state == MilanSinkState::settled_no_rsv && !was && s.registering()) {
        // Section 5.5.3.5.42: EVT_TK_REGISTERED.
        s.timer_running = false;
        s.state = MilanSinkState::settled_rsv_ok;
    } else if (s.state == MilanSinkState::settled_rsv_ok && was && !s.registering()) {
        // Section 5.5.3.5.48: EVT_TK_UNREGISTERED.
        s.timer_running = false;
        unsettle(index);
        s.acmp_status = 0;
        reprobe(index, now);
    }
    notify(index);
}

auto MilanListener::set_started(uint16_t const index, bool const started) -> bool
{
    if (index >= sinks_.size() || !sinks_[index].bound() || sinks_[index].binding.started == started) {
        return false;
    }
    sinks_[index].binding.started = started;
    binding_changed(index);
    notify(index);
    return true;
}

void MilanListener::discovered(uint16_t const index, TimePoint const now)
{
    auto& s = sinks_[index];
    s.discovered = true;
    // Section 5.5.3.5.9; in every other bound state the talker is only noted
    // as discovered.
    if (s.state == MilanSinkState::prb_w_avail) {
        s.probing_status = probing_status::ACTIVE;
        s.acmp_status = 0;
        start_delay(s, now);
        notify(index);
    }
}

void MilanListener::departed(uint16_t const index)
{
    auto& s = sinks_[index];
    s.discovered = false;
    // Sections 5.5.3.5.15, .22, .29, .35; the settled states only note it.
    switch (s.state) {
        case MilanSinkState::prb_w_delay:
        case MilanSinkState::prb_w_resp:
        case MilanSinkState::prb_w_resp2:
        case MilanSinkState::prb_w_retry:
            s.timer_running = false;
            enter(s, probing_status::PASSIVE, MilanSinkState::prb_w_avail);
            notify(index);
            break;
        default:
            break;
    }
}

void MilanListener::receive_adp(AdpDu const& adp, tsn::ClockIdentity const& grandmaster, uint8_t const domain, TimePoint const now)
{
    if (!adp.is_entity_available() && !adp.is_entity_departing()) {
        return;
    }
    bool const same_gptp = adp.gptp_grandmaster_id == grandmaster && adp.gptp_domain_number.get() == domain;
    // TMR_NO_ADP: valid_time in 2 s units, at least one (a valid_time of 0).
    TimePoint const valid_until = now + std::chrono::seconds{2 * std::max<int64_t>(adp.valid_time(), 1)};
    for (size_t i = 0; i < sinks_.size(); ++i) {
        auto const& s = sinks_[i];
        if (!s.bound() || s.binding.talker_entity_id != adp.entity_id) {
            continue;
        }
        if (s.discovered) {
            adp_discovered(static_cast<uint16_t>(i), adp, same_gptp, valid_until, now);
        } else {
            adp_not_discovered(static_cast<uint16_t>(i), adp, same_gptp, valid_until, now);
        }
    }
}

void MilanListener::adp_not_discovered(
    uint16_t const index, AdpDu const& adp, bool const same_gptp, TimePoint const valid_until, TimePoint const now)
{
    // Section 5.6.4.5.1 (TK_NOT_DISCOVERED ignores RCV_ADP_DEPARTING).
    if (!adp.is_entity_available() || !same_gptp) {
        return;
    }
    auto& s = sinks_[index];
    s.interface_index = adp.interface_index.get();
    s.available_index = adp.available_index.get();
    s.adp_deadline = valid_until;
    discovered(index, now);
}

void MilanListener::adp_discovered(
    uint16_t const index, AdpDu const& adp, bool const same_gptp, TimePoint const valid_until, TimePoint const now)
{
    auto& s = sinks_[index];
    if (adp.interface_index.get() != s.interface_index) {
        return;  // Sections 5.6.4.5.2-3 step 1
    }
    if (!adp.is_entity_available()) {
        departed(index);  // Section 5.6.4.5.3
        return;
    }
    // Section 5.6.4.5.2: an available_index that did not move forward
    // (wrap-safe) is a talker that restarted.
    if (static_cast<int32_t>(adp.available_index.get() - s.available_index) <= 0) {
        departed(index);
        if (!same_gptp) {
            return;  // TK_NOT_DISCOVERED, the timer stopped
        }
        discovered(index, now);
    }
    s.available_index = adp.available_index.get();
    s.adp_deadline = valid_until;
}

void MilanListener::tick(TimePoint const now)
{
    for (size_t i = 0; i < sinks_.size(); ++i) {
        auto const& s = sinks_[i];
        auto const index = static_cast<uint16_t>(i);
        if (!s.bound()) {
            continue;
        }
        // Section 5.6.4.5.4: TMR_NO_ADP.
        if (s.discovered && now >= s.adp_deadline) {
            departed(index);
        }
        if (s.timer_running && now >= s.deadline) {
            expire(index, now);
        }
    }
}

void MilanListener::expire(uint16_t const index, TimePoint const now)
{
    auto& s = sinks_[index];
    s.timer_running = false;
    switch (s.state) {
        case MilanSinkState::prb_w_delay:
            // Section 5.5.3.5.10.
            s.state = MilanSinkState::prb_w_resp;
            send_probe(index, now);
            break;
        case MilanSinkState::prb_w_resp:
            // Section 5.5.3.5.16: the duplicate, same sequence_id.
            start_timer(s, now, TMR_NO_RESP);
            s.state = MilanSinkState::prb_w_resp2;
            send(s.probe);
            break;
        case MilanSinkState::prb_w_resp2:
            // Section 5.5.3.5.23.
            start_timer(s, now, TMR_RETRY);
            s.acmp_status = ACMP_STATUS_LISTENER_TALKER_TIMEOUT;
            s.state = MilanSinkState::prb_w_retry;
            break;
        case MilanSinkState::prb_w_retry:
            // Section 5.5.3.5.30 (the ACMP status of the last probe
            // sequence is kept while probing again).
            reprobe(index, now);
            break;
        case MilanSinkState::settled_no_rsv:
            // Section 5.5.3.5.36: TMR_NO_TK.
            unsettle(index);
            s.acmp_status = 0;
            reprobe(index, now);
            break;
        default:
            break;
    }
    notify(index);
}

void MilanListener::notify(uint16_t const index)
{
    auto const& s = sinks_[index];
    Status const now{
        .bound = s.bound(),
        .talker_entity_id = s.binding.talker_entity_id,
        .talker_unique_id = s.binding.talker_unique_id,
        .started = s.binding.started,
        .probing_status = s.probing_status,
        .acmp_status = s.acmp_status,
        .stream = s.stream,
        .registration = s.registration};
    if (now == notified_[index]) {
        return;
    }
    notified_[index] = now;
    if (on_status_changed_) {
        on_status_changed_(index);
    }
}

}  // namespace statusbar::atdecc
