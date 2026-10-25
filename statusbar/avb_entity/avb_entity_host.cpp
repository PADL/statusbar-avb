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
    size_t const listener_max_streams,
    std::optional<bool> const milan)
    : components_{std::move(model), adp_config, talker_max_streams, talker_max_listeners, listener_max_streams, milan}
{
    wire_domain_callbacks();
}

AvbEntityHost::AvbEntityHost(
    nanoavb::AemEntityHandler& handler,
    nanoavb::AdpAdvertiserConfig adp_config,
    size_t const talker_max_streams,
    size_t const talker_max_listeners,
    size_t const listener_max_streams,
    std::optional<bool> const milan)
    : handler_{&handler}
    , components_{handler, adp_config, talker_max_streams, talker_max_listeners, listener_max_streams, milan}
{
    wire_identify_control();
    wire_avb_info();
    wire_domain_callbacks();
}

AvbEntityHost::~AvbEntityHost()
{
    // The reactor owns the mirror port; make sure it stops calling back into
    // a host that is going away.
    if (mirror_ != nullptr) {
        mirror_->mirror().set_on_update({});
    }
}

void AvbEntityHost::wire_domain_callbacks()
{
    // The Class A VID changed: join the new VLAN (Milan v1.2 Section 4.3.2), report
    // it in ACMP, let the entity re-tag, and re-advertise the talker streams.
    components_.msrp_handler.set_on_domain_vid_changed(
        [this](uint16_t const old_vid, uint16_t const new_vid, TimePoint const time) {
            ctl_log().status("msrp: Class A VID {} -> {} (Domain)", old_vid, new_vid);
            (void)components_.mvrp_handler.withdraw_vlan(old_vid, time);
            (void)components_.mvrp_handler.register_vlan(new_vid, time);
            for (size_t i = 0; i < components_.acmp_talker.max_streams(); ++i) {
                (void)components_.acmp_talker.set_stream_vlan_id(static_cast<uint16_t>(i), new_vid);
            }
            if (on_domain_vid_changed_) {
                on_domain_vid_changed_(new_vid, time);
            }
            refresh_avb_info();
            if (supervisor_.current_state() == nanoavb::supervisor_sm::Def::State::Ready && advertise_streams_) {
                advertise_streams_(time);
            }
        });
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
    net_handlers_->set_local_port_config(local_atdecc_socket_);
    net_handlers_->add_to_reactor(reactor);
    net_handlers_->print_warnings(interface_name_);
    if (auto* local = net_handlers_->local_port(); local != nullptr) {
        local->set_logger(ctl_log());
    }
    // Reactor-thread logging for the ATDECC handler's once-per-second status
    // and the MSRP participant's Debug-level MRP diagnostics.
    net_handlers_->atdecc_handler().set_logger(ctl_log());
    components_.msrp_handler.set_logger(ctl_log());

    nanoavb::setup_nanoavb_callbacks(components_, *net_handlers_);
    wire_callbacks();

    running_ = true;
    reactor_ = &reactor;
    // The AVB_INTERFACE descriptor reports the NIC we are actually on from
    // the start; a mirrored device's descriptors refine it as they arrive.
    apply_interface_runtime();
    attach_mirror(reactor);
    return success();
}

auto AvbEntityHost::stop_control_plane(TimePoint const now) -> Status
{
    if (!running_) {
        return failure(std::make_error_code(std::errc::not_connected));
    }
    (void)components_.adp_advertiser.stop(now);
    net_handlers_->close_local_port();
    net_handlers_.reset();
    if (mirror_ != nullptr) {
        mirror_->mirror().set_on_update({});
        mirror_ = nullptr;
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
    components_.aem_handler.set_get_as_path(
        [this](uint16_t const index, nanoavb::AsPath& path) -> bool { return as_path(index, path); });
}

auto AvbEntityHost::as_path(uint16_t const index, nanoavb::AsPath& path) const -> bool
{
    if (index != 0) {
        auto const* storage = descriptor_storage();
        if (storage == nullptr || !storage->get_descriptor(0, atdecc::aem::DESCRIPTOR_AVB_INTERFACE, index)) {
            return false;
        }
    }
    path = net_handlers_ != nullptr ? compose_as_path(net_handlers_->gptp_handler().path_trace(), own_clock_identity_)
                                    : nanoavb::AsPath{};
    return true;
}

auto compose_as_path(nanoavb::AsPath const& path_trace, std::optional<ieee::Eui64> const& own_clock_identity) -> nanoavb::AsPath
{
    nanoavb::AsPath path = path_trace;
    if (!path.empty() && own_clock_identity.has_value() && path.back() != *own_clock_identity && path.size() < path.capacity()) {
        path.push_back(*own_clock_identity);
    }
    return path;
}

void AvbEntityHost::note_as_path_changed()
{
    (void)components_.aem_handler.notify_as_path_changed(0);
}

void AvbEntityHost::refresh_avb_info()
{
    if (mirror_config_.has_value()) {
        return;
    }
    using atdecc::aem::avb_info_flags::AS_CAPABLE;
    auto const info = avb_info(0);
    auto const class_a = [](nanoavb::AvbInfo const& i) -> std::pair<uint8_t, uint16_t> {
        auto const* const m = std::ranges::find_if(i.msrp_mappings, [](auto const& x) { return x.traffic_class == 0; });
        return m != i.msrp_mappings.end() ? std::pair{m->priority, m->vlan_id} : std::pair<uint8_t, uint16_t>{};
    };
    bool const changed = info.gptp_grandmaster_id != last_avb_info_.gptp_grandmaster_id ||
        info.propagation_delay != last_avb_info_.propagation_delay ||
        info.gptp_domain_number != last_avb_info_.gptp_domain_number ||
        (info.flags & AS_CAPABLE) != (last_avb_info_.flags & AS_CAPABLE) || class_a(info) != class_a(last_avb_info_);
    last_avb_info_ = info;
    if (changed) {
        (void)components_.aem_handler.notify_avb_info_changed(0);
    }
}

void AvbEntityHost::mirror_avb_info_from(
    ieee::Eui64 const target_entity_id, ieee::Eui64 const controller_entity_id, int64_t const poll_interval_ns)
{
    AvbInfoMirror::Config cfg{};
    cfg.target_entity_id = target_entity_id;
    cfg.controller_entity_id = controller_entity_id;
    cfg.poll_interval_ns = poll_interval_ns;
    mirror_config_ = cfg;
    if (reactor_ != nullptr && mirror_ == nullptr) {
        attach_mirror(*reactor_);
    }
}

auto AvbEntityHost::mirror_target() const noexcept -> std::optional<ieee::Eui64>
{
    if (!mirror_config_.has_value()) {
        return std::nullopt;
    }
    return mirror_config_->target_entity_id;
}

void AvbEntityHost::attach_mirror(net::MessageReactor& reactor)
{
    if (!mirror_config_.has_value() || mirror_ != nullptr) {
        return;
    }
    auto port = std::make_unique<AvbInfoMirrorPort>(interface_name_, *mirror_config_);
    if (!port->valid()) {
        ctl_log().warning("avb info mirror: cannot open the ATDECC socket");
        return;
    }
    port->mirror().set_on_update(
        [this](uint16_t const index, atdecc::aem::DescriptorAvbInterface const* desc, nanoavb::AvbInfo const* info) {
            on_mirror_update(index, desc, info);
        });
    mirror_ = port.get();
    reactor.add(std::move(port));
    ctl_log().status("avb info mirror: following entity {:016x}", mirror_config_->target_entity_id.to_uint64());
}

void AvbEntityHost::set_propagation_delay_ns(uint32_t const ns)
{
    propagation_delay_override_ = true;
    propagation_delay_override_ns_ = ns;
    refresh_avb_info();  // a change notifies GET_AVB_INFO's subscribers
}

void AvbEntityHost::on_mirror_update(
    uint16_t const index, atdecc::aem::DescriptorAvbInterface const* desc, nanoavb::AvbInfo const* info)
{
    auto it = std::find_if(mirrored_.begin(), mirrored_.end(), [index](MirroredInterface const& m) { return m.index == index; });
    if (it == mirrored_.end()) {
        mirrored_.push_back({.index = index});
        it = std::prev(mirrored_.end());
    }
    if (desc != nullptr) {
        it->descriptor = *desc;
        ctl_log().status(
            "avb info mirror: interface {} clock {:016x} priority1={} priority2={} domain={}",
            index,
            desc->clock_identity.to_uint64(),
            static_cast<uint8_t>(desc->priority1),
            static_cast<uint8_t>(desc->priority2),
            static_cast<uint8_t>(desc->domain_number));
        apply_interface_runtime();
    }
    if (info != nullptr) {
        it->info = *info;
        ctl_log().status(
            "avb info mirror: interface {} gm {:016x} domain={} flags={:#04x} propagation_delay={}ns",
            index,
            info->gptp_grandmaster_id.to_uint64(),
            info->gptp_domain_number,
            info->flags,
            info->propagation_delay);
        if (index == 0) {
            // ADP carries the grandmaster too; keep it in step with what we report.
            components_.adp_advertiser.set_gptp_info(tsn::ClockIdentity{info->gptp_grandmaster_id}, info->gptp_domain_number);
            components_.adp_advertiser.notify_entity_changed();
        }
        // IEEE 1722.1 7.5.2: gPTP changes are unsolicited GET_AVB_INFO notifications.
        (void)components_.aem_handler.notify_avb_info_changed(index);
    }
}

void AvbEntityHost::apply_interface_runtime()
{
    if (handler_ == nullptr) {
        return;
    }
    auto const mac = net::read_interface_mac(interface_name_);
    auto const* storage = descriptor_storage();
    // Every AVB_INTERFACE the model authors is this one NIC. A mirrored
    // device's descriptor supplies the gPTP port fields; otherwise only the
    // NIC identity is known (ptp4l derives its clockIdentity from the MAC the
    // same way, so the fallback agrees with a local gPTP daemon).
    for (uint16_t index = 0;; ++index) {
        if (index > 0 && (storage == nullptr || !storage->get_descriptor(0, atdecc::aem::DESCRIPTOR_AVB_INTERFACE, index))) {
            break;
        }
        nanoavb::AvbInterfaceRuntime rt{};
        if (mac.has_value()) {
            rt.identity_valid = true;
            rt.mac_address = *mac;
            rt.clock_identity = mac->to_modified_eui64();
            rt.port_number = 1;
        }
        auto const m = std::find_if(mirrored_.begin(), mirrored_.end(), [index](MirroredInterface const& x) {
            return x.index == index && x.descriptor.has_value();
        });
        if (m != mirrored_.end()) {
            auto const& d = *m->descriptor;
            rt.identity_valid = true;
            if (!mac.has_value()) {
                rt.mac_address = d.mac_address;
            }
            rt.clock_identity = d.clock_identity;
            rt.port_number = d.port_number.get();
            rt.gptp_valid = true;
            rt.priority1 = static_cast<uint8_t>(d.priority1);
            rt.clock_class = static_cast<uint8_t>(d.clock_class);
            rt.offset_scaled_log_variance = d.offset_scaled_log_variance.get();
            rt.clock_accuracy = static_cast<uint8_t>(d.clock_accuracy);
            rt.priority2 = static_cast<uint8_t>(d.priority2);
            rt.domain_number = static_cast<uint8_t>(d.domain_number);
            rt.log_sync_interval = static_cast<int8_t>(static_cast<uint8_t>(d.log_sync_interval));
            rt.log_announce_interval = static_cast<int8_t>(static_cast<uint8_t>(d.log_announce_interval));
            rt.log_pdelay_interval = static_cast<int8_t>(static_cast<uint8_t>(d.log_pdelay_interval));
        }
        if (!rt.identity_valid && !rt.gptp_valid) {
            continue;
        }
        if (index == 0) {
            own_clock_identity_ = rt.clock_identity;
        }
        handler_->set_avb_interface_runtime(index, rt);
    }
}

auto AvbEntityHost::avb_info(uint16_t const index) const -> nanoavb::AvbInfo
{
    using namespace atdecc::aem::avb_info_flags;
    // A mirrored device's answer is reported as-is: it IS the state.
    auto const m = std::find_if(
        mirrored_.begin(), mirrored_.end(), [index](MirroredInterface const& x) { return x.index == index && x.info.has_value(); });
    if (m != mirrored_.end()) {
        return *m->info;
    }
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
    if (propagation_delay_override_) {
        info.propagation_delay = propagation_delay_override_ns_;
    }
    return info;
}

//
// Event handlers (drive the SM stack)
//

void AvbEntityHost::on_link_up(TimePoint const time)
{
    // Milan v1.2 Section 4.2.7.2.1: after a Link Up, return to the configured
    // Class A domain (forget an adopted VID) and declare it at once -- not
    // gated on gPTP, unlike the stream advertisements.
    (void)components_.msrp_handler.reset_domain(time);
    if (!supervisor_ctx_.link_up) {
        ++link_up_count_;
        (void)components_.aem_handler.notify_counters_changed(atdecc::aem::DESCRIPTOR_AVB_INTERFACE, 0);
    }
    supervisor_ctx_.link_up = true;
    supervisor_.handle_event(supervisor_ctx_, nanoavb::supervisor_sm::Def::Event::LinkUp, time);
    refresh_avb_info();
}

void AvbEntityHost::on_link_down(TimePoint const time)
{
    if (supervisor_ctx_.link_up) {
        ++link_down_count_;
        (void)components_.aem_handler.notify_counters_changed(atdecc::aem::DESCRIPTOR_AVB_INTERFACE, 0);
    }
    supervisor_ctx_.link_up = false;
    supervisor_.handle_event(supervisor_ctx_, nanoavb::supervisor_sm::Def::Event::LinkDown, time);
    refresh_avb_info();
}

void AvbEntityHost::on_gptp_announce(TimePoint const time, bool const has_grandmaster)
{
    if (has_grandmaster && !gptp_ctx_.time_locked) {
        gptp_.handle_event(gptp_ctx_, nanoavb::gptp_sm::Def::Event::LockedStable, time);
    }
}

void AvbEntityHost::note_grandmaster_changed()
{
    refresh_avb_info();
    // The first grandmaster found is not a change.
    if (!grandmaster_seen_) {
        grandmaster_seen_ = true;
        return;
    }
    ++gm_changed_count_;
    (void)components_.aem_handler.notify_counters_changed(atdecc::aem::DESCRIPTOR_AVB_INTERFACE, 0);
}

void AvbEntityHost::note_grandmaster_lost()
{
    refresh_avb_info();
}

void AvbEntityHost::note_media_clock_locked(bool const locked)
{
    if (!clock_domain_locks_[0].entity_driven) {
        count_clock_domain_lock(0, locked);
    }
    refresh_avb_info();  // asCapable follows the gPTP lock
}

void AvbEntityHost::count_clock_domain_lock(uint16_t const index, bool const locked)
{
    auto& domain = clock_domain_locks_[index];
    if (locked == domain.locked) {
        return;
    }
    domain.locked = locked;
    ++(locked ? domain.locked_count : domain.unlocked_count);
    (void)components_.aem_handler.notify_counters_changed(atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN, index);
}

auto AvbEntityHost::has_clock_domain(uint16_t const index) const noexcept -> bool
{
    auto const* store = descriptor_storage();
    bool const exists =
        store != nullptr ? store->get_descriptor(0, atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN, index).has_value() : index == 0;
    return exists && index < MAX_CLOCK_DOMAINS;
}

auto AvbEntityHost::note_clock_domain_locked(uint16_t const index, bool const locked) -> bool
{
    if (!has_clock_domain(index)) {
        return false;
    }
    clock_domain_locks_[index].entity_driven = true;
    count_clock_domain_lock(index, locked);
    return true;
}

auto AvbEntityHost::release_clock_domain_lock(uint16_t const index) -> bool
{
    if (!has_clock_domain(index)) {
        return false;
    }
    auto& domain = clock_domain_locks_[index];
    if (!domain.entity_driven) {
        return true;
    }
    if (index == 0) {
        domain.entity_driven = false;
        count_clock_domain_lock(0, gptp_locked());
        return true;
    }
    bool const was_locked = domain.locked;
    count_clock_domain_lock(index, false);  // a released domain is unlocked: its next lock counts
    domain.entity_driven = false;
    if (!was_locked) {
        (void)components_.aem_handler.notify_counters_changed(atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN, index);
    }
    return true;
}

auto AvbEntityHost::fill_counters(
    uint16_t const descriptor_type, uint16_t const descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& counters) const
    -> bool
{
    using namespace atdecc::aem;
    // On the legacy (parsed-model) path there is no blob to check against:
    // the one ENTITY, AVB_INTERFACE and CLOCK_DOMAIN every model has.
    auto const exists = [this, descriptor_type, descriptor_index] {
        auto const* store = descriptor_storage();
        return store != nullptr ? store->get_descriptor(0, descriptor_type, descriptor_index).has_value() : descriptor_index == 0;
    };
    // Bit value (1 << i) flags counters[i] (bit 31 - i in IEEE 1722.1's numbering).
    switch (descriptor_type) {
        case DESCRIPTOR_ENTITY:
            return descriptor_index == 0;  // only ENTITY_SPECIFIC_1..8 are defined: none valid
        case DESCRIPTOR_AVB_INTERFACE:
            if (!exists()) {
                return false;
            }
            if (descriptor_index != 0) {
                return true;  // the host accounts for interface 0, the one it runs on
            }
            valid |= 0x00000001U | 0x00000002U | 0x00000020U;  // LINK_UP, LINK_DOWN, GPTP_GM_CHANGED
            counters[0] = link_up_count_;
            counters[1] = link_down_count_;
            counters[5] = gm_changed_count_;
            return true;
        case DESCRIPTOR_CLOCK_DOMAIN:
            if (!exists()) {
                return false;
            }
            if (descriptor_index >= MAX_CLOCK_DOMAINS ||
                (descriptor_index != 0 && !clock_domain_locks_[descriptor_index].entity_driven)) {
                return true;  // domain 0 follows gPTP; another is accounted for once the entity drives it
            }
            valid |= 0x00000001U | 0x00000002U;  // LOCKED, UNLOCKED
            counters[0] = clock_domain_locks_[descriptor_index].locked_count;
            counters[1] = clock_domain_locks_[descriptor_index].unlocked_count;
            return true;
        default:
            return false;
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
        note_media_clock_locked(false);  // the gPTP reset below drops the lock
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
        note_media_clock_locked(true);
        supervisor_.handle_event(supervisor_ctx_, nanoavb::supervisor_sm::Def::Event::GptpLocked, time);
    };
    gptp_ctx_.callbacks.report_unlocked = [this](auto& /*ctx*/, TimePoint time) -> void {
        note_media_clock_locked(false);
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
