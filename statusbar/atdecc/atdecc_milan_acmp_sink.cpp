// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_milan_acmp_sink.hpp"

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace statusbar::atdecc {

void MilanSink::attach(MilanListenerEnv& env, uint16_t const index) noexcept
{
    ctx_.env = &env;
    ctx_.index = index;
}

void MilanSink::restore(MilanBinding const& binding, TimePoint const now)
{
    reset(now);
    ctx_.binding = binding;
    ctx_.binding->listener_unique_id = ctx_.index;
    handle(MilanSinkEvent::StartBound, now);
}

void MilanSink::restart(TimePoint const now)
{
    if (ctx_.binding.has_value()) {
        restore(MilanBinding{*ctx_.binding}, now);
    }
}

void MilanSink::on_command(AcmpCommandResponse const& cmd, bool const locked_out, TimePoint const now)
{
    ctx_.env->rcvd = cmd;
    if (auto const event = milan_sink_event_for_command(ctx_, cmd, locked_out)) {
        handle(*event, now);
    }
}

void MilanSink::on_probe_response(AcmpCommandResponse const& resp, TimePoint const now)
{
    ctx_.env->rcvd = resp;
    if (auto const event = milan_sink_event_for_probe_response(ctx_, resp)) {
        handle(*event, now);
    }
}

void MilanSink::on_adp(AdpDu const& adp, MilanGptpState const& own, TimePoint const now)
{
    if (!ctx_.binding.has_value() || adp.entity_id != ctx_.binding->talker_entity_id) {
        return;
    }
    if (auto const event = milan_discovery_event_for(discovery_ctx_, discovery_.current_state(), adp, own)) {
        discovery_ctx_.rcvd = adp;
        run_discovery(*event, now);
    }
}

void MilanSink::on_advertisement_heard_earlier(
    AdpDu const& adp, MilanGptpState const& own, TimePoint const valid_until, TimePoint const now)
{
    if (discovery_.current_state() != MilanDiscoveryState::TkNotDiscovered || valid_until <= now) {
        return;
    }
    on_adp(adp, own, now);
    if (discovery_.current_state() == MilanDiscoveryState::TkDiscovered) {
        discovery_ctx_.timer = valid_until;  // it has been ageing since it was heard
    }
}

void MilanSink::on_srp_changed(TimePoint const now)
{
    check_registration(now);
}

void MilanSink::tick(TimePoint const now)
{
    if (discovery_ctx_.timer.has_value() && now >= *discovery_ctx_.timer) {
        run_discovery(MilanDiscoveryEvent::TmrNoAdp, now);
    }
    if (ctx_.timer.has_value() && now >= *ctx_.timer) {
        expire_timer(now);
    }
}

auto MilanSink::next_deadline() const noexcept -> std::optional<TimePoint>
{
    if (ctx_.timer.has_value() && discovery_ctx_.timer.has_value()) {
        return std::min(*ctx_.timer, *discovery_ctx_.timer);
    }
    return ctx_.timer.has_value() ? ctx_.timer : discovery_ctx_.timer;
}

void MilanSink::handle(MilanSinkEvent const event, TimePoint const now)
{
    sm_.handle_event(ctx_, event, now);
    apply_discovery_requests(now);
    check_registration(now);
}

void MilanSink::apply_discovery_requests(TimePoint const now)
{
    if (ctx_.discovery_stop_requested) {
        discovery_.handle_event(discovery_ctx_, MilanDiscoveryEvent::Stop, now);
    }
    if (ctx_.discovery_start_requested) {
        discovery_.handle_event(discovery_ctx_, MilanDiscoveryEvent::Start, now);
    }
    ctx_.discovery_stop_requested = false;
    ctx_.discovery_start_requested = false;
}

void MilanSink::run_discovery(MilanDiscoveryEvent const event, TimePoint const now)
{
    discovery_ctx_.departed = false;
    discovery_ctx_.discovered = false;
    discovery_.handle_event(discovery_ctx_, event, now);
    // Read before delivering: 5.6.4.5.2 step 2 triggers departed, then discovered.
    bool const discovered = discovery_ctx_.discovered;
    if (discovery_ctx_.departed) {
        handle(MilanSinkEvent::TalkerDeparted, now);
    }
    if (discovered) {
        handle(MilanSinkEvent::TalkerDiscovered, now);
    }
}

void MilanSink::check_registration(TimePoint const now)
{
    auto const& ask = ctx_.env->talker_registration;
    bool const registered = ask && ask(ctx_.srp) != MilanTalkerRegistration::None;
    if (auto const event = milan_sink_registration_event(ctx_, sm_.current_state(), registered)) {
        sm_.handle_event(ctx_, *event, now);
    }
}

void MilanSink::expire_timer(TimePoint const now)
{
    ctx_.timer.reset();
    if (auto const event = milan_sink_timer_event(ctx_, sm_.current_state())) {
        handle(*event, now);
    }
}

void MilanSink::reset(TimePoint const now)
{
    if (ctx_.srp_active && ctx_.env->stop_srp) {
        ctx_.env->stop_srp(ctx_.index, now);
    }
    ctx_ = MilanSinkContext{.env = ctx_.env, .index = ctx_.index};
    discovery_ctx_ = MilanDiscoveryContext{};
    sm_.reset();
    discovery_.reset();
}

}  // namespace statusbar::atdecc
