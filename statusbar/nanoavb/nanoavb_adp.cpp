// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_adp.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace statusbar::nanoavb {

auto NanoAvbAdpAdvertiser::start(TimePoint now) -> Status
{
    if (state_ == AdpAdvertiserState::Advertising) {
        return success();  // Already advertising, idempotent
    }

    state_ = AdpAdvertiserState::Advertising;
    if (config_.milan_advertise) {
        start_milan(now);
        return success();
    }

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

    // Under Milan the change asks for an ENTITY_AVAILABLE through TMR_DELAY
    // (5.6.3.5.7, GM_CHANGE). There is no time here to start a timer from:
    // the next tick(), which is told it, does.
    if (config_.milan_advertise) {
        milan_change_pending_ = true;
        return;
    }
    // If advertising, send updated Entity Available immediately
    // (send_entity_available will increment available_index per IEEE 1722.1)
    if (state_ == AdpAdvertiserState::Advertising) {
        send_entity_available();
    }
}

auto NanoAvbAdpAdvertiser::draw_delay(std::chrono::milliseconds const longest) noexcept -> std::chrono::milliseconds
{
    if (random_delay_) {
        return std::clamp(random_delay_(longest), std::chrono::milliseconds{0}, longest);
    }
    // xorshift32: spread is all TMR_DELAY asks of it.
    random_state_ ^= random_state_ << 13U;
    random_state_ ^= random_state_ >> 17U;
    random_state_ ^= random_state_ << 5U;
    return std::chrono::milliseconds{random_state_ % (static_cast<uint32_t>(longest.count()) + 1U)};
}

void NanoAvbAdpAdvertiser::begin_milan_delay(TimePoint const now, std::chrono::milliseconds const longest) noexcept
{
    milan_state_ = MilanAdvertise::Delay;
    milan_deadline_ = now + draw_delay(longest);
}

void NanoAvbAdpAdvertiser::start_milan(TimePoint const now) noexcept
{
    // 5.6.3.5.2: at start-up with the link up TMR_DELAY is 0 to 2 s.
    // 5.6.3.5.3: at a later LINK_UP it is 0 to 4 s.
    bool const first = !std::exchange(milan_started_before_, true);
    milan_change_pending_ = false;  // what is sent after the delay is current
    begin_milan_delay(now, first ? MILAN_ADP_STARTUP_DELAY_MAX : MILAN_ADP_TMR_DELAY_MAX);
}

void NanoAvbAdpAdvertiser::tick_milan(TimePoint const now)
{
    bool const changed = std::exchange(milan_change_pending_, false);
    // 5.6.3.5.7, GM_CHANGE in WAITING, and 5.6.3.5.5, TMR_ADVERTISE: both
    // start TMR_DELAY. In DELAY a change is ignored (Table 5.56): the
    // ENTITY_AVAILABLE that is owed will carry it.
    if (milan_state_ == MilanAdvertise::Waiting && (changed || now >= milan_deadline_)) {
        begin_milan_delay(now, MILAN_ADP_TMR_DELAY_MAX);
    }
    // A delay of nothing ends in the tick that started it.
    if (milan_state_ == MilanAdvertise::Waiting || now < milan_deadline_) {
        return;
    }
    // 5.6.3.5.9, TMR_DELAY: send, start TMR_ADVERTISE, go to WAITING.
    send_entity_available();
    milan_state_ = MilanAdvertise::Waiting;
    milan_deadline_ = now + MILAN_TMR_ADVERTISE;
}

void NanoAvbAdpAdvertiser::answer_discover(TimePoint const now)
{
    if (!config_.milan_advertise) {
        send_entity_available();
        return;
    }
    // 5.6.3.5.4: in WAITING, stop TMR_ADVERTISE and start TMR_DELAY. In
    // DELAY the message is ignored (Table 5.56): one is owed already.
    if (milan_state_ == MilanAdvertise::Waiting) {
        begin_milan_delay(now, MILAN_ADP_TMR_DELAY_MAX);
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

auto NanoAvbAdpAdvertiser::receive_adpdu(AdpDu const& adpdu, TimePoint now) -> void
{
    // Check if this is a Discover message targeting us or all entities
    if (adpdu.is_entity_discover()) {
        // Entity Discover with entity_id = 0 is a broadcast discover
        // Entity Discover with our entity_id is targeted at us
        bool const is_broadcast = (adpdu.entity_id == Eui64{});
        bool const is_targeted = (adpdu.entity_id == entity_.entity_id);

        if ((is_broadcast || is_targeted) && state_ == AdpAdvertiserState::Advertising) {
            // Respond with Entity Available: at once, or after Milan's TMR_DELAY
            answer_discover(now);
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
    if (config_.milan_advertise) {
        tick_milan(now);
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

    bool const send_ok = callbacks_.send_adpdu(departing_adpdu());

    if (send_ok) {
        consecutive_send_failures_ = 0;
    } else {
        ++consecutive_send_failures_;
    }
    restart_available_index();

    return send_ok;
}

auto NanoAvbAdpAdvertiser::departing_adpdu() const -> AdpDu
{
    AdpDu departing = adpdu_;
    departing.init_entity_departing(entity_.entity_id);
    departing.available_index = quadlet_t{0};
    return departing;
}

void NanoAvbAdpAdvertiser::restart_available_index()
{
    // IEEE 1722.1-2021 6.2.2.15: "reset to zero (0) when transmitting an
    // ENTITY_DEPARTING or after a power cycle".
    available_index_ = 0;
    adpdu_.available_index = available_index_;
    if (callbacks_.on_available_index_change) {
        callbacks_.on_available_index_change(available_index_);
    }
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
            callbacks_.on_entity_departing(id, last_event_time_);
        }
    };
}

void NanoAvbAdpDiscovery::receive_adpdu(AdpDu const& adp, Eui48 const& src_mac, TimePoint now)
{
    last_event_time_ = now;
    ctx_.rcvd_adpdu = adp;
    ctx_.rcvd_src_mac = src_mac;

    if (adp.is_entity_available()) {
        sm_.handle_event(ctx_, atdecc::DiscoveryEvent::RcvdAvailable, now);
    } else if (adp.is_entity_departing()) {
        sm_.handle_event(ctx_, atdecc::DiscoveryEvent::RcvdDeparting, now);
    }
}

void NanoAvbAdpDiscovery::discover_all(TimePoint const now)
{
    ctx_.discover_target = {};
    sm_.handle_event(ctx_, atdecc::DiscoveryEvent::DoDiscover, now);
}

void NanoAvbAdpDiscovery::discover(Eui64 const& target, TimePoint const now)
{
    ctx_.discover_target = target;
    sm_.handle_event(ctx_, atdecc::DiscoveryEvent::DoDiscover, now);
}

void NanoAvbAdpDiscovery::tick(TimePoint now)
{
    last_event_time_ = now;
    ctx_.entities.expire_stale(now, [this, now](atdecc::DiscoveredEntity const& e) {
        if (callbacks_.on_entity_departing) {
            callbacks_.on_entity_departing(e.adpdu.entity_id, now);
        }
    });
}

}  // namespace statusbar::nanoavb
