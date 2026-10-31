// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_adp.hpp"

#include <chrono>
#include <cstdint>
#include <utility>

namespace statusbar::nanoavb {

NanoAvbAdpAdvertiser::NanoAvbAdpAdvertiser(DescriptorEntity entity, AdpAdvertiserCallbacks callbacks, AdpAdvertiserConfig config)
    : entity_{entity}
    , callbacks_{std::move(callbacks)}
    , config_{config}
{
    update_adpdu_from_model();
    apply_timer_config();
    ieee::Eui48 id{};  // the entity ID's low 48 bits until seed_random() gives the MAC
    ctx_.random.seed(id.from_uint64(entity_.entity_id.to_uint64()), adp_adv_sm::realtime_ns());
}

auto NanoAvbAdpAdvertiser::apply_timer_config() noexcept -> void
{
    ctx_.advertise_interval = config_.reannounce_interval;
    ctx_.delay_max = config_.delay_max();
    ctx_.startup_delay_max = config_.startup_delay_max;
}

auto NanoAvbAdpAdvertiser::handle(adp_adv_sm::Def::Event const event, TimePoint const now) -> void
{
    machine_.handle_event(ctx_, event, now);
    if (std::exchange(ctx_.do_advertise, false)) {
        send_entity_available();
    }
    if (std::exchange(ctx_.do_terminate, false)) {
        send_entity_departing();
    }
}

auto NanoAvbAdpAdvertiser::link_up(TimePoint const now) -> void
{
    handle(adp_adv_sm::Def::Event::LinkUp, now);
}

auto NanoAvbAdpAdvertiser::link_down(TimePoint const now) -> void
{
    handle(adp_adv_sm::Def::Event::LinkDown, now);
}

auto NanoAvbAdpAdvertiser::shutdown(TimePoint const now) -> bool
{
    machine_.handle_event(ctx_, adp_adv_sm::Def::Event::Shutdown, now);
    return std::exchange(ctx_.do_terminate, false) && send_entity_departing();
}

auto NanoAvbAdpAdvertiser::notify_entity_changed(TimePoint const now) -> void
{
    update_adpdu_from_model();
    handle(adp_adv_sm::Def::Event::EntityChange, now);
}

auto NanoAvbAdpAdvertiser::set_gptp_info(ClockIdentity const& grandmaster_id, uint8_t const domain_number, TimePoint const now)
    -> void
{
    bool const changed = adpdu_.gptp_grandmaster_id != grandmaster_id;
    adpdu_.gptp_grandmaster_id = grandmaster_id;
    adpdu_.gptp_domain_number = domain_number;
    if (changed) {
        handle(adp_adv_sm::Def::Event::GmChange, now);
    }
}

auto NanoAvbAdpAdvertiser::update_adpdu_from_model() -> void
{
    auto const& entity = entity_;

    // Initialize as Entity Available with our entity ID
    adpdu_.init_entity_available(entity.entity_id, config_.valid_time);

    // Copy fields from entity descriptor that map to ADPDU
    adpdu_.entity_model_id = entity.entity_model_id;
    adpdu_.entity_capabilities = entity.entity_capabilities;
    adpdu_.talker_stream_sources = entity.talker_stream_sources;
    adpdu_.talker_capabilities = entity.talker_capabilities;
    adpdu_.listener_stream_sinks = entity.listener_stream_sinks;
    adpdu_.listener_capabilities = entity.listener_capabilities;
    adpdu_.controller_capabilities = entity.controller_capabilities;
    adpdu_.available_index = available_index_;
    adpdu_.association_id = entity.association_id;

    // Re-apply the identify-control advertisement: it is discovered from the
    // descriptor storage (see AvbEntityHost::wire_identify_control), not part
    // of the ENTITY descriptor this rebuild copies from.
    if (identify_control_index_valid_) {
        adpdu_.identify_control_index = identify_control_index_;
        adpdu_.entity_capabilities.set_flag(atdecc::entity_capabilities::AEM_IDENTIFY_CONTROL_INDEX_VALID);
    }
    // The interface the ADPDU goes out on is always known: Milan v1.2 Section
    // 5.6.2 requires AEM_INTERFACE_INDEX_VALID with interface_index populated
    // (IEEE 1722.1-2021 6.2.2.20).
    adpdu_.interface_index = interface_index_;
    adpdu_.entity_capabilities.set_flag(atdecc::entity_capabilities::AEM_INTERFACE_INDEX_VALID);

    // gPTP fields would typically come from gPTP stack
    // For now, leave at defaults (0)
}

auto NanoAvbAdpAdvertiser::is_discover_for_us(AdpDu const& adpdu) const noexcept -> bool
{
    return adpdu.is_entity_discover() && (adpdu.entity_id == Eui64{} || adpdu.entity_id == entity_.entity_id);
}

auto NanoAvbAdpAdvertiser::receive_adpdu(AdpDu const& adpdu, TimePoint const now) -> void
{
    if (is_discover_for_us(adpdu)) {
        handle(adp_adv_sm::Def::Event::RcvDiscover, now);
    }
}

auto NanoAvbAdpAdvertiser::tick(TimePoint const now) -> void
{
    if (ctx_.advertise_at && now >= *ctx_.advertise_at) {
        handle(adp_adv_sm::Def::Event::TmrAdvertise, now);
    }
    if (ctx_.delay_until && now >= *ctx_.delay_until) {
        handle(adp_adv_sm::Def::Event::TmrDelay, now);
    }
}

auto NanoAvbAdpAdvertiser::send_entity_available() -> bool
{
    return callbacks_.send_adpdu && note_send(note_available(callbacks_.send_adpdu(available_adpdu())));
}

auto NanoAvbAdpAdvertiser::available_adpdu() noexcept -> AdpDu const&
{
    adpdu_.set_message_type(ADP_MESSAGE_TYPE_ENTITY_AVAILABLE);
    return adpdu_;
}

auto NanoAvbAdpAdvertiser::note_available(bool const sent) -> bool
{
    if (sent) {
        // IEEE 1722.1-2021 Clause 6.2.2.15: incremented after transmitting ENTITY_AVAILABLE.
        adpdu_.available_index = ++available_index_;
        if (callbacks_.on_available_index_change) {
            callbacks_.on_available_index_change(available_index_);
        }
    }
    return sent;
}

auto NanoAvbAdpAdvertiser::send_entity_departing() -> bool
{
    // IEEE 1722.1-2021 Clause 6.2.2.15: reset to zero when transmitting ENTITY_DEPARTING.
    available_index_ = 0;
    adpdu_.available_index = available_index_;
    AdpDu departing = adpdu_;
    departing.init_entity_departing(entity_.entity_id);
    return callbacks_.send_adpdu && note_send(callbacks_.send_adpdu(departing));
}

auto NanoAvbAdpAdvertiser::note_send(bool const sent) noexcept -> bool
{
    consecutive_send_failures_ = sent ? 0 : consecutive_send_failures_ + 1;
    return sent;
}

//
// NanoAvbAdpDiscovery
//

NanoAvbAdpDiscovery::NanoAvbAdpDiscovery(AdpDiscoveryCallbacks callbacks)
    : callbacks_{std::move(callbacks)}
{
    wire_callbacks();
    sm_.handle_event(ctx_, atdecc::DiscoveryEvent::UCT, TimePoint{});
}

void NanoAvbAdpDiscovery::set_callbacks(AdpDiscoveryCallbacks callbacks)
{
    callbacks_ = std::move(callbacks);
    wire_callbacks();
}

void NanoAvbAdpDiscovery::wire_callbacks()
{
    ctx_.tx_discover = [this](AdpDu const& adp) -> bool {
        if (callbacks_.send_adpdu) {
            return callbacks_.send_adpdu(adp);
        }
        return false;
    };
    ctx_.on_entity_available = [this](atdecc::DiscoveredEntity const& e) {
        if (callbacks_.on_entity_available) {
            callbacks_.on_entity_available(e);
        }
    };
    ctx_.on_entity_updated = [this](atdecc::DiscoveredEntity const& e) {
        if (callbacks_.on_entity_updated) {
            callbacks_.on_entity_updated(e);
        }
    };
    ctx_.on_entity_departing = [this](Eui64 id) {
        if (callbacks_.on_entity_departing) {
            callbacks_.on_entity_departing(id);
        }
    };
}

void NanoAvbAdpDiscovery::receive_adpdu(AdpDu const& adp, Eui48 const& src_mac, TimePoint now)
{
    ctx_.rcvd_adpdu = adp;
    ctx_.rcvd_src_mac = src_mac;

    if (adp.is_entity_available()) {
        sm_.handle_event(ctx_, atdecc::DiscoveryEvent::RcvdAvailable, now);
    } else if (adp.is_entity_departing()) {
        sm_.handle_event(ctx_, atdecc::DiscoveryEvent::RcvdDeparting, now);
    }
}

void NanoAvbAdpDiscovery::discover_all()
{
    ctx_.discover_target = {};
    sm_.handle_event(ctx_, atdecc::DiscoveryEvent::DoDiscover, TimePoint::clock::now());
}

void NanoAvbAdpDiscovery::discover(Eui64 const& target)
{
    ctx_.discover_target = target;
    sm_.handle_event(ctx_, atdecc::DiscoveryEvent::DoDiscover, TimePoint::clock::now());
}

void NanoAvbAdpDiscovery::tick(TimePoint now)
{
    ctx_.entities.expire_stale(now, [this](atdecc::DiscoveredEntity const& e) {
        if (callbacks_.on_entity_departing) {
            callbacks_.on_entity_departing(e.adpdu.entity_id);
        }
    });
}

}  // namespace statusbar::nanoavb
