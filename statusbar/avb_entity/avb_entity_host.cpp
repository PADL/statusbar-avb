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
#include "statusbar/net/net_posix_util.hpp"
#include "statusbar/srp/srp_msrp.hpp"

#include <algorithm>
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
    wire_avb_info();
}

AvbEntityHost::~AvbEntityHost()
{
    // The reactor owns the ptp4l client; make sure it stops calling back into
    // a host that is going away.
    if (ptp4l_ != nullptr) {
        ptp4l_->set_on_changed({});
    }
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
    reactor_ = &reactor;
    // The AVB_INTERFACE descriptor reports the NIC we are actually on from
    // the start; ptp4l (when enabled) refines the gPTP fields as it answers.
    apply_interface_runtime();
    attach_ptp4l(reactor);
    return success();
}

auto AvbEntityHost::stop_control_plane(TimePoint const now) -> Status
{
    if (!running_) {
        return failure(std::make_error_code(std::errc::not_connected));
    }
    (void)components_.adp_advertiser.stop(now);
    net_handlers_.reset();
    if (ptp4l_ != nullptr) {
        ptp4l_->set_on_changed({});
        ptp4l_ = nullptr;
    }
    reactor_ = nullptr;
    running_ = false;
    return success();
}

//
// Live gPTP state: AVB_INTERFACE and GET_AVB_INFO
//

void AvbEntityHost::wire_avb_info()
{
    components_.aem_handler.set_get_avb_info([this](uint16_t const index, nanoavb::AvbInfo& out) -> bool {
        // Index 0 is this host's NIC; any further AVB_INTERFACE the model
        // authors is answered with the same state (one port, one clock).
        if (index != 0) {
            auto const* storage = descriptor_storage();
            if (storage == nullptr || !storage->get_descriptor(0, atdecc::aem::DESCRIPTOR_AVB_INTERFACE, index)) {
                return false;
            }
        }
        out = avb_info(index);
        return true;
    });
}

void AvbEntityHost::enable_ptp4l_status(gptp::Ptp4lClient::Config config)
{
    ptp4l_config_ = std::move(config);
    if (reactor_ != nullptr && ptp4l_ == nullptr) {
        attach_ptp4l(*reactor_);
    }
}

void AvbEntityHost::attach_ptp4l(net::MessageReactor& reactor)
{
    if (!ptp4l_config_.has_value() || ptp4l_ != nullptr) {
        return;
    }
    auto client = std::make_unique<gptp::Ptp4lClient>(*ptp4l_config_);
    if (auto st = client->open(); !st) {
        ctl_log().warning("ptp4l status: cannot open client socket (errno {})", st.error().value());
        return;
    }
    client->set_on_changed([this](gptp::Ptp4lStatus const& status) { on_ptp4l_changed(status); });
    ptp4l_ = client.get();
    reactor.add(std::move(client));
    ctl_log().status("ptp4l status: reading {}", logging::embed<64>(ptp4l_config_->uds_path));
}

auto AvbEntityHost::ptp4l_status() const noexcept -> gptp::Ptp4lStatus const*
{
    return ptp4l_status_.valid() ? &ptp4l_status_ : nullptr;
}

void AvbEntityHost::set_propagation_delay_ns(uint32_t const ns)
{
    propagation_delay_override_ = true;
    propagation_delay_override_ns_ = ns;
}

void AvbEntityHost::on_ptp4l_changed(gptp::Ptp4lStatus const& status)
{
    ptp4l_status_ = status;
    if (status.valid()) {
        ctl_log().status(
            "ptp4l: clock {:016x} priority1={} priority2={} domain={} port_state={} as_capable={} gm={:016x} peer_delay={}ns",
            status.clock_identity.to_uint64(),
            status.priority1,
            status.priority2,
            status.domain_number,
            status.port_state,
            status.as_capable,
            status.grandmaster_identity.to_uint64(),
            status.peer_mean_path_delay_ns);
    } else {
        ctl_log().warning("ptp4l: no answer — reporting observed gPTP state");
    }
    apply_interface_runtime();

    // ADP carries the grandmaster too; keep it in step with what we report.
    auto const info = avb_info(0);
    components_.adp_advertiser.set_gptp_info(tsn::ClockIdentity{info.gptp_grandmaster_id}, info.gptp_domain_number);
    components_.adp_advertiser.notify_entity_changed();

    // IEEE 1722.1 7.5.2: gPTP changes are unsolicited GET_AVB_INFO notifications.
    if (info.gptp_grandmaster_id != last_avb_info_.gptp_grandmaster_id || info.flags != last_avb_info_.flags ||
        info.gptp_domain_number != last_avb_info_.gptp_domain_number ||
        info.propagation_delay != last_avb_info_.propagation_delay) {
        last_avb_info_ = info;
        (void)components_.aem_handler.notify_avb_info_changed(0);
    }
}

void AvbEntityHost::apply_interface_runtime()
{
    if (handler_ == nullptr) {
        return;
    }
    nanoavb::AvbInterfaceRuntime rt{};
    if (auto const mac = net::read_interface_mac(interface_name_)) {
        rt.identity_valid = true;
        rt.mac_address = *mac;
        // ptp4l derives its clockIdentity from the NIC the same way, so the
        // fallback agrees with it.
        rt.clock_identity = mac->to_modified_eui64();
        rt.port_number = 1;
    }
    if (ptp4l_status_.valid()) {
        rt.identity_valid = true;
        rt.clock_identity = ptp4l_status_.clock_identity.to_eui64();
        if (ptp4l_status_.have_port) {
            rt.port_number = ptp4l_status_.port_number;
        }
        rt.gptp_valid = true;
        rt.priority1 = ptp4l_status_.priority1;
        rt.clock_class = ptp4l_status_.clock_class;
        rt.offset_scaled_log_variance = ptp4l_status_.offset_scaled_log_variance;
        rt.clock_accuracy = ptp4l_status_.clock_accuracy;
        rt.priority2 = ptp4l_status_.priority2;
        rt.domain_number = ptp4l_status_.domain_number;
        rt.log_sync_interval = ptp4l_status_.log_sync_interval;
        rt.log_announce_interval = ptp4l_status_.log_announce_interval;
        rt.log_pdelay_interval = ptp4l_status_.log_min_pdelay_req_interval;
    }
    if (!rt.identity_valid && !rt.gptp_valid) {
        return;
    }
    // Every AVB_INTERFACE the model authors describes this one NIC.
    auto const* storage = descriptor_storage();
    for (uint16_t index = 0;; ++index) {
        if (index > 0 && (storage == nullptr || !storage->get_descriptor(0, atdecc::aem::DESCRIPTOR_AVB_INTERFACE, index))) {
            break;
        }
        handler_->set_avb_interface_runtime(index, rt);
    }
}

auto AvbEntityHost::avb_info(uint16_t const index) const -> nanoavb::AvbInfo
{
    (void)index;
    using namespace atdecc::aem::avb_info_flags;
    nanoavb::AvbInfo info{};
    info.flags = GPTP_ENABLED | AVTP_DOWN_VALID;
    if (!supervisor_ctx_.link_up) {
        info.flags |= AVTP_DOWN;
    }
    if (net_handlers_ != nullptr && net_handlers_->msrp_handler().valid()) {
        info.flags |= SRP_ENABLED;
        // One mapping per declared SR class: MSRP SRclassID 6 is class A
        // (traffic class 0), 5 is class B (traffic class 1).
        auto const& domain = components_.msrp_handler.domain();
        info.msrp_mappings.push_back(
            {.traffic_class = static_cast<uint8_t>(domain.sr_class_id == 6 ? 0 : 1),
             .priority = domain.sr_class_priority,
             .vlan_id = domain.sr_class_vid});
    }
    if (ptp4l_status_.valid()) {
        if (ptp4l_status_.gm_present || ptp4l_status_.have_parent) {
            info.gptp_grandmaster_id = ptp4l_status_.grandmaster_identity.to_eui64();
        }
        info.gptp_domain_number = ptp4l_status_.domain_number;
        if (ptp4l_status_.as_capable) {
            info.flags |= AS_CAPABLE;
        }
        if (ptp4l_status_.have_port) {
            info.propagation_delay = static_cast<uint32_t>(std::max<int64_t>(0, ptp4l_status_.peer_mean_path_delay_ns));
        }
    } else {
        if (net_handlers_ != nullptr) {
            auto const& announce = net_handlers_->gptp_handler();
            if (announce.has_grandmaster()) {
                info.gptp_grandmaster_id = announce.grandmaster_identity().to_eui64();
            }
            if (auto const* last = announce.last_announce()) {
                info.gptp_domain_number = last->header.domain_number.get();
            }
        }
        if (gptp_ctx_.as_capable || gptp_ctx_.time_locked) {
            info.flags |= AS_CAPABLE;
        }
    }
    if (propagation_delay_override_ && info.propagation_delay == 0) {
        info.propagation_delay = propagation_delay_override_ns_;
    }
    return info;
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
