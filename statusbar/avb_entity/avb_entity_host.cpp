// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AvbEntityHost — the reusable AVB entity control plane. Owns the nanoavb state
// machines + NanoAvbComponents + net handlers and the lifecycle that drives them.
// The generic SM-callback wiring lives here once (it was duplicated verbatim in
// every entity); the stream-specific bits are reached through the three typed hooks
// (advertise / withdraw / on-listener-ready) and through components() (ACMP/MSRP).

#include "statusbar/avb_entity/avb_entity_host.hpp"

#include "statusbar/atdecc/atdecc_aem_control_types.hpp"
#include "statusbar/avb_entity/avb_entity_listener_bindings.hpp"
#include "statusbar/srp/srp_msrp.hpp"

#include <chrono>
#include <cstdint>
#include <print>
#include <utility>

namespace statusbar::avb_entity {

AvbEntityHost::AvbEntityHost(
    nanoavb::EntityModel model,
    nanoavb::AdpAdvertiserConfig adp_config,
    size_t const talker_max_streams,
    size_t const talker_max_listeners,
    size_t const listener_max_streams)
    : components_{std::move(model), adp_config, talker_max_streams, talker_max_listeners, listener_max_streams}
{}

AvbEntityHost::AvbEntityHost(
    nanoavb::AemEntityHandler& handler,
    nanoavb::AdpAdvertiserConfig adp_config,
    size_t const talker_max_streams,
    size_t const talker_max_listeners,
    size_t const listener_max_streams)
    : handler_{&handler}
    , components_{handler, adp_config, talker_max_streams, talker_max_listeners, listener_max_streams}
{
    wire_identify_control();
}

void AvbEntityHost::wire_identify_control()
{
    auto const* storage = descriptor_storage();
    if (storage == nullptr) {
        return;
    }
    for (uint16_t index = 0;; ++index) {
        auto const desc = storage->get_descriptor(0, atdecc::aem::DESCRIPTOR_CONTROL, index);
        if (!desc) {
            return;
        }
        if (desc.value().size() < atdecc::aem::DescriptorControl::LENGTH) {
            continue;
        }
        // control_type is the EUI-64 at offset 82 of the CONTROL descriptor.
        ieee::Eui64 control_type{};
        span_load(control_type, desc.value().subspan(82));
        if (control_type == atdecc::aem::CONTROL_TYPE_IDENTIFY) {
            components_.aem_handler.set_identify_control_index(index);
            components_.adp_advertiser.set_identify_control_index(index);
            // Default identify indicator: these entities have no LED, so log to
            // the journal. Entities with a real indicator can override via
            // components().aem_handler.set_identify_changed().
            components_.aem_handler.set_identify_changed([log = ctl_log()](bool const active) mutable {
                log.status(
                    "identify {}", active ? logging::lit("ON — a controller is identifying this entity") : logging::lit("off"));
            });
            return;
        }
    }
}

//
// Lifecycle
//

auto AvbEntityHost::start_control_plane(net::MessageReactor& reactor, std::string_view const interface_name) -> Status
{
    if (running_) {
        return failure(std::make_error_code(std::errc::already_connected));
    }
    interface_name_ = std::string{interface_name};

    net_handlers_ = std::make_unique<nanoavb::NanoAvbNetHandlers>(interface_name_, components_);
    net_handlers_->add_to_reactor(reactor);
    net_handlers_->print_warnings(interface_name_);
    // Reactor-thread logging for the ATDECC handler's once-per-second status
    // and the MSRP participant's Debug-level MRP diagnostics.
    net_handlers_->atdecc_handler().set_logger(ctl_log());
    components_.msrp_handler.set_logger(ctl_log());

    nanoavb::setup_nanoavb_callbacks(components_, *net_handlers_);
    wire_callbacks();

    running_ = true;
    return success();
}

auto AvbEntityHost::stop_control_plane(TimePoint const now) -> Status
{
    if (!running_) {
        return failure(std::make_error_code(std::errc::not_connected));
    }
    (void)components_.adp_advertiser.stop(now);
    net_handlers_.reset();
    running_ = false;
    return success();
}

//
// Event handlers (drive the SM stack)
//

void AvbEntityHost::on_link_up(TimePoint const time)
{
    supervisor_ctx_.link_up = true;
    supervisor_.handle_event(supervisor_ctx_, nanoavb::supervisor_sm::Def::Event::LinkUp, time);
}

void AvbEntityHost::on_link_down(TimePoint const time)
{
    supervisor_ctx_.link_up = false;
    supervisor_.handle_event(supervisor_ctx_, nanoavb::supervisor_sm::Def::Event::LinkDown, time);
}

void AvbEntityHost::on_gptp_announce(TimePoint const time, bool const has_grandmaster)
{
    if (has_grandmaster && !gptp_ctx_.time_locked) {
        gptp_.handle_event(gptp_ctx_, nanoavb::gptp_sm::Def::Event::LockedStable, time);
    }
}

void AvbEntityHost::on_timeout(TimePoint const time)
{
    using nanoavb::supervisor_sm::Def;
    if (supervisor_.current_state() == Def::State::Init) {
        supervisor_.handle_event(supervisor_ctx_, Def::Event::Timeout, time);
    }
}

//
// State queries
//

auto AvbEntityHost::is_ready() const noexcept -> bool
{
    return supervisor_.current_state() == nanoavb::supervisor_sm::Def::State::Ready;
}

auto AvbEntityHost::state_string() const -> std::string_view
{
    using nanoavb::supervisor_sm::Def;
    switch (supervisor_.current_state()) {
        case Def::State::Start:
            return "Start";
        case Def::State::Down:
            return "Down";
        case Def::State::Init:
            return "Init";
        case Def::State::Ready:
            return "Ready";
        case Def::State::Degraded:
            return "Degraded";
        default:
            return "Unknown";
    }
}

//
// Generic SM-callback wiring (no stream specifics)
//

void AvbEntityHost::wire_callbacks()
{
    wire_supervisor_callbacks();
    wire_gptp_callbacks();
    wire_mvrp_callbacks();
    wire_srp_callbacks();
    wire_engine_callbacks();
}

void AvbEntityHost::wire_supervisor_callbacks()
{
    supervisor_ctx_.callbacks.init_iface = [this](auto& /*ctx*/, TimePoint /*time*/) -> void {
        gptp_.reset();
        mvrp_.reset();
    };

    supervisor_ctx_.callbacks.start_protocols = [this](auto& /*ctx*/, TimePoint time) -> void {
        gptp_.handle_event(gptp_ctx_, nanoavb::gptp_sm::Def::Event::AsCapableUp, time);
        auto result = components_.adp_advertiser.start(time);
        if (!result) {
            ctl_log().error("ADP start failed: errno {}", result.error().value());
        }
    };

    supervisor_ctx_.callbacks.enter_ready = [this](auto& /*ctx*/, TimePoint time) -> void {
        mvrp_.handle_event(mvrp_ctx_, nanoavb::mvrp_sm::Def::Event::Acquire, time);
        msrp_talker_.handle_event(msrp_talker_ctx_, nanoavb::msrp_talker_sm::Def::Event::StartAdvertise, time);
        talker_engine_ctx_.send_allowed = true;
        listener_engine_ctx_.play_allowed = true;
    };

    supervisor_ctx_.callbacks.degrade_stop_streams = [this](auto& /*ctx*/, TimePoint time) -> void {
        talker_engine_.handle_event(talker_engine_ctx_, nanoavb::talker_engine_sm::Def::Event::GateStop, time);
        listener_engine_.handle_event(listener_engine_ctx_, nanoavb::listener_engine_sm::Def::Event::GateStop, time);
        talker_engine_ctx_.send_allowed = false;
        listener_engine_ctx_.play_allowed = false;
        msrp_talker_.handle_event(msrp_talker_ctx_, nanoavb::msrp_talker_sm::Def::Event::StopAdvertise, time);
        mvrp_.reset();
        msrp_talker_.reset();
        msrp_listener_.reset();
    };

    supervisor_ctx_.callbacks.stop_all = [this](auto& /*ctx*/, TimePoint time) -> void {
        (void)components_.adp_advertiser.stop(time);
        talker_engine_.handle_event(talker_engine_ctx_, nanoavb::talker_engine_sm::Def::Event::Fatal, time);
        listener_engine_.handle_event(listener_engine_ctx_, nanoavb::listener_engine_sm::Def::Event::GateStop, time);
        gptp_.reset();
        mvrp_.reset();
        msrp_talker_.reset();
        msrp_listener_.reset();
        talker_engine_ctx_.send_allowed = false;
        listener_engine_ctx_.play_allowed = false;
    };

    supervisor_ctx_.callbacks.timeout_gptp = [log = ctl_log()](auto& /*ctx*/, TimePoint /*time*/) mutable -> void {
        log.warning("supervisor: gPTP lock timeout");
    };
}

void AvbEntityHost::wire_gptp_callbacks()
{
    gptp_ctx_.callbacks.init = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    gptp_ctx_.callbacks.start_servo = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    gptp_ctx_.callbacks.report_locked = [this](auto& /*ctx*/, TimePoint time) -> void {
        supervisor_.handle_event(supervisor_ctx_, nanoavb::supervisor_sm::Def::Event::GptpLocked, time);
    };
    gptp_ctx_.callbacks.report_unlocked = [this](auto& /*ctx*/, TimePoint time) -> void {
        supervisor_.handle_event(supervisor_ctx_, nanoavb::supervisor_sm::Def::Event::GptpLost, time);
    };
}

void AvbEntityHost::wire_mvrp_callbacks()
{
    mvrp_ctx_.callbacks.init = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    mvrp_ctx_.callbacks.send_join = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    mvrp_ctx_.callbacks.mark_joined = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    mvrp_ctx_.callbacks.mark_error = [log = ctl_log()](auto& /*ctx*/, TimePoint /*time*/) mutable -> void {
        log.error("mvrp: VLAN registration failed");
    };
    mvrp_ctx_.callbacks.send_leave = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    mvrp_ctx_.callbacks.mark_left = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    mvrp_ctx_.callbacks.reset = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
}

void AvbEntityHost::wire_srp_callbacks()
{
    msrp_talker_ctx_.callbacks.init = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    msrp_talker_ctx_.callbacks.msrp_talker_advertise = [this](auto& /*ctx*/, TimePoint time) -> void {
        // Declare our SR class domain(s) before advertising streams so the bridge
        // includes this port in the SRP domain; otherwise inbound Talker Advertise
        // declarations are converted to Talker Failed (failure_code 8). This is
        // generic; the per-stream reservations are the entity's job (the hook).
        (void)components_.msrp_handler.declare_domain(time);
        if (advertise_streams_) {
            advertise_streams_(time);
        }
    };
    msrp_talker_ctx_.callbacks.mark_ready = [this](auto& /*ctx*/, TimePoint time) -> void {
        if (supervisor_.current_state() == nanoavb::supervisor_sm::Def::State::Ready) {
            talker_engine_.handle_event(talker_engine_ctx_, nanoavb::talker_engine_sm::Def::Event::AudioReady, time);
        }
    };
    msrp_talker_ctx_.callbacks.mark_failed = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    msrp_talker_ctx_.callbacks.msrp_talker_withdraw = [this](auto& /*ctx*/, TimePoint time) -> void {
        if (withdraw_streams_) {
            withdraw_streams_(time);
        }
    };
    msrp_talker_ctx_.callbacks.mark_idle = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};

    components_.msrp_handler.set_on_talker_listener([this](nanoavb::StreamId const& stream_id, bool ready) {
        auto const now = sm::Clock::now();
        msrp_talker_.handle_event(
            msrp_talker_ctx_, ready ? nanoavb::msrp_talker_sm::Def::Event::Ready : nanoavb::msrp_talker_sm::Def::Event::Lost, now);

        // Diagnostic: log the EXACT reason behind every gate decision (fires on each
        // listener register/leave for one of our talker streams), so a listener-ready
        // flap is explained -- which of the four conditions (record present,
        // operation==Register, registrar In, Ready substate) is flipping. The
        // enabled() gate skips the permit-debug computation when Debug is off.
        if (ctl_log().enabled(logging::LogLevel::Debug)) {
            auto const dbg = components_.msrp_handler.listener_permit_debug(stream_id);
            ctl_log().debug(
                "srp-gate sid={:016x} permits={} | record={} op={} registrar_in={} substate={}",
                stream_id.to_uint64(),
                dbg.permits,
                dbg.has_record,
                dbg.operation == statusbar::srp::msrp::Operation::Register ? logging::lit("Register") : logging::lit("Declare"),
                dbg.registrar_in,
                logging::static_str(statusbar::srp::msrp::listener_declaration_name(dbg.substate)));
        }

        // The entity's transmit gate tracks readiness (TalkerGate::note_listener_ready).
        if (on_listener_ready_) {
            on_listener_ready_(stream_id, ready);
        }
    });

    msrp_listener_ctx_.callbacks.init = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    msrp_listener_ctx_.callbacks.msrp_listener_ready = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    msrp_listener_ctx_.callbacks.mark_ready = [this](auto& /*ctx*/, TimePoint time) -> void {
        if (supervisor_.current_state() == nanoavb::supervisor_sm::Def::State::Ready) {
            listener_engine_.handle_event(listener_engine_ctx_, nanoavb::listener_engine_sm::Def::Event::GateListen, time);
        }
    };
    msrp_listener_ctx_.callbacks.mark_failed = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    msrp_listener_ctx_.callbacks.msrp_listener_leave = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    msrp_listener_ctx_.callbacks.mark_idle = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
}

void AvbEntityHost::wire_engine_callbacks()
{
    talker_engine_ctx_.callbacks.init = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    talker_engine_ctx_.callbacks.start_audio_source = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    talker_engine_ctx_.callbacks.arm_stream = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    talker_engine_ctx_.callbacks.start_tx = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    talker_engine_ctx_.callbacks.stop_tx = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    talker_engine_ctx_.callbacks.mute_tx = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    talker_engine_ctx_.callbacks.unmute_tx = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    talker_engine_ctx_.callbacks.stop_all = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};

    listener_engine_ctx_.callbacks.init = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    listener_engine_ctx_.callbacks.enable_rx_filter = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    listener_engine_ctx_.callbacks.start_sync = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    listener_engine_ctx_.callbacks.start_audio_sink = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    listener_engine_ctx_.callbacks.stop_all = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    listener_engine_ctx_.callbacks.resync = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    listener_engine_ctx_.callbacks.mute_out = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
    listener_engine_ctx_.callbacks.unmute_out = [](auto& /*ctx*/, TimePoint /*time*/) -> void {};
}

void AvbEntityHost::enable_listener_binding_persistence(std::string path)
{
    if (path.empty()) {
        return;
    }
    listener_bindings_path_ = std::move(path);
    auto& listener = components_.acmp_listener;

    // Reload yesterday's bindings as fast-connect goals: the listener's
    // reactor tick re-connects each remembered talker until it answers,
    // healing the half-open state an entity or talker restart leaves.
    if (auto loaded = load_listener_bindings(listener_bindings_path_)) {
        for (auto const& binding : *loaded) {
            listener.set_fast_connect_goal(binding.listener_unique_id, binding.talker_entity_id, binding.talker_unique_id);
        }
        if (!loaded->empty()) {
            ctl_log().status("acmp: {} persisted listener binding(s) loaded; fast-connect armed", loaded->size());
        }
    } else {
        ctl_log().status("acmp: listener bindings file unreadable; starting with no fast-connect goals");
    }

    // Sticky from here on: every successful connect is remembered, every
    // goal change (connect, retarget, controller DISCONNECT) mirrors to disk.
    listener.enable_sticky_bindings();
    listener.set_on_goals_changed([this]() {
        auto const goals = components_.acmp_listener.fast_connect_goals();
        if (auto const st = save_listener_bindings(listener_bindings_path_, goals); !st) {
            ctl_log().status("acmp: could not persist {} listener binding(s)", goals.size());
        }
    });
}

}  // namespace statusbar::avb_entity
