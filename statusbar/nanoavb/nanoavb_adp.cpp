// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_adp.hpp"

namespace statusbar::nanoavb {

auto NanoAvbAdpAdvertiser::start(TimePoint now) -> Status
{
    if (state_ == AdpAdvertiserState::Advertising) {
        return success();  // Already advertising, idempotent
    }

    state_ = AdpAdvertiserState::Advertising;

    // Send initial Entity Available
    send_entity_available();

    // Record the time we sent so tick() waits for the reannounce interval
    last_announce_time_ = now;

    return success();
}

auto NanoAvbAdpAdvertiser::stop(TimePoint /*now*/) -> Status
{
    if (state_ == AdpAdvertiserState::Stopped) {
        return success();  // Already stopped, idempotent
    }

    // Send Entity Departing
    send_entity_departing();

    state_ = AdpAdvertiserState::Stopped;
    return success();
}

auto NanoAvbAdpAdvertiser::notify_entity_changed() -> void
{
    // Update ADPDU from current model state
    update_adpdu_from_model();

    // If advertising, send updated Entity Available immediately
    // (send_entity_available will increment available_index per IEEE 1722.1)
    if (state_ == AdpAdvertiserState::Advertising) {
        send_entity_available();
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

    // gPTP fields would typically come from gPTP stack
    // For now, leave at defaults (0)
}

auto NanoAvbAdpAdvertiser::receive_adpdu(AdpDu const& adpdu, TimePoint /*now*/) -> void
{
    // Check if this is a Discover message targeting us or all entities
    if (adpdu.is_entity_discover()) {
        // Entity Discover with entity_id = 0 is a broadcast discover
        // Entity Discover with our entity_id is targeted at us
        bool const is_broadcast = (adpdu.entity_id == Eui64{});
        bool const is_targeted = (adpdu.entity_id == entity_.entity_id);

        if ((is_broadcast || is_targeted) && state_ == AdpAdvertiserState::Advertising) {
            // Respond with Entity Available
            send_entity_available();
        }
    }

    // In a full implementation, this would also:
    // 1. Track other entities that we've discovered
    // 2. Maintain a database of known entities
    // 3. Handle entity departing messages
}

auto NanoAvbAdpAdvertiser::tick(TimePoint now) -> void
{
    if (state_ != AdpAdvertiserState::Advertising) {
        return;
    }

    // Check if it's time to reannounce
    auto const elapsed = now - last_announce_time_;
    if (elapsed >= config_.reannounce_interval) {
        send_entity_available();
        last_announce_time_ = now;
    }
}

auto NanoAvbAdpAdvertiser::send_entity_available() -> bool
{
    if (!callbacks_.send_adpdu) {
        return false;
    }

    // Ensure we're sending Entity Available
    adpdu_.set_message_type(ADP_MESSAGE_TYPE_ENTITY_AVAILABLE);
    bool const send_ok = callbacks_.send_adpdu(adpdu_);

    if (send_ok) {
        // Per IEEE 1722.1, increment available_index after each successful transmission
        ++available_index_;
        adpdu_.available_index = available_index_;
        consecutive_send_failures_ = 0;

        if (callbacks_.on_available_index_change) {
            callbacks_.on_available_index_change(available_index_);
        }
    } else {
        ++consecutive_send_failures_;
    }

    return send_ok;
}

auto NanoAvbAdpAdvertiser::send_entity_departing() -> bool
{
    if (!callbacks_.send_adpdu) {
        return false;
    }

    // Create a departing message
    AdpDu departing = adpdu_;
    departing.init_entity_departing(entity_.entity_id);
    departing.available_index = quadlet_t{0};
    bool const send_ok = callbacks_.send_adpdu(departing);

    if (send_ok) {
        consecutive_send_failures_ = 0;
    } else {
        ++consecutive_send_failures_;
    }

    return send_ok;
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
