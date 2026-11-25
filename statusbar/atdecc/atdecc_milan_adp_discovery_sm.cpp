// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Milan v1.3 listener discovery state machine: actions and event selection.

#include "statusbar/atdecc/atdecc_milan_adp_discovery_sm.hpp"

#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/sm/sm_core.hpp"

#include <optional>

namespace statusbar::atdecc {

namespace {

[[nodiscard]] auto on_our_gptp(AdpDu const& adp, MilanGptpState const& own) noexcept -> bool
{
    return adp.gptp_grandmaster_id == own.grandmaster_id && adp.gptp_domain_number.get() == own.domain_number;
}

/// The advertisement's current_configuration_index, when its capabilities
/// mark the field valid (IEEE 1722.1-2021 6.2.1.10).
[[nodiscard]] auto configuration_of(AdpDu const& adp) noexcept -> std::optional<uint16_t>
{
    if (!adp.has_entity_capability(entity_capabilities::AEM_CONFIGURATION_INDEX_VALID)) {
        return std::nullopt;
    }
    return adp.current_configuration_index.get();
}

/// 5.6.4.5.2 step 2: the talker went away and came back between two
/// advertisements, or changed configuration. The configuration is compared
/// "if present": when this advertisement and the last both mark it valid.
[[nodiscard]] auto shows_restart(MilanDiscoveryContext const& ctx, AdpDu const& adp) noexcept -> bool
{
    auto const configuration = configuration_of(adp);
    bool const reconfigured = configuration.has_value() && ctx.current_configuration_index.has_value() &&
        *configuration != *ctx.current_configuration_index;
    return adp.available_index.get() <= ctx.available_index || reconfigured;
}

[[nodiscard]] auto discovered_talker_event(MilanDiscoveryContext const& ctx, AdpDu const& adp, MilanGptpState const& own) noexcept
    -> std::optional<MilanDiscoveryEvent>
{
    if (adp.interface_index.get() != ctx.interface_index) {
        return std::nullopt;
    }
    if (adp.is_entity_departing()) {
        return MilanDiscoveryEvent::Departing;
    }
    if (!shows_restart(ctx, adp)) {
        return MilanDiscoveryEvent::Refreshed;
    }
    return on_our_gptp(adp, own) ? MilanDiscoveryEvent::Restarted : MilanDiscoveryEvent::RestartedElsewhere;
}

}  // namespace

namespace milan_discovery_actions {

namespace {

/// Note what the advertisement says and run TMR_NO_ADP from it.
void note_advertisement(MilanDiscoveryContext& ctx, sm::TimePoint const now)
{
    ctx.interface_index = ctx.rcvd.interface_index.get();
    ctx.available_index = ctx.rcvd.available_index.get();
    ctx.current_configuration_index = configuration_of(ctx.rcvd);
    ctx.timer = now + milan_adp_valid_for(ctx.rcvd);
}

}  // namespace

void start(MilanDiscoveryContext& ctx, sm::TimePoint /*now*/)
{
    // "at this point, talker has not been discovered": nothing is told to the sink.
    ctx = MilanDiscoveryContext{};
}

void stop(MilanDiscoveryContext& ctx, sm::TimePoint /*now*/)
{
    ctx = MilanDiscoveryContext{};
}

void discover(MilanDiscoveryContext& ctx, sm::TimePoint const now)
{
    // 5.6.4.5.1 steps 2 to 4
    note_advertisement(ctx, now);
    ctx.discovered = true;
}

void refresh(MilanDiscoveryContext& ctx, sm::TimePoint const now)
{
    // 5.6.4.5.2 step 3
    note_advertisement(ctx, now);
}

void rediscover(MilanDiscoveryContext& ctx, sm::TimePoint const now)
{
    // 5.6.4.5.2 steps 2a, 2c and 3
    note_advertisement(ctx, now);
    ctx.departed = true;
    ctx.discovered = true;
}

void depart(MilanDiscoveryContext& ctx, sm::TimePoint /*now*/)
{
    // 5.6.4.5.2 step 2b, 5.6.4.5.3 and 5.6.4.5.4
    ctx.timer.reset();
    ctx.departed = true;
}

}  // namespace milan_discovery_actions

auto milan_discovery_event_for(
    MilanDiscoveryContext const& ctx, MilanDiscoveryState const state, AdpDu const& adp, MilanGptpState const& own) noexcept
    -> std::optional<MilanDiscoveryEvent>
{
    bool const available = adp.is_entity_available();
    if (state == MilanDiscoveryState::TkNotDiscovered && available && on_our_gptp(adp, own)) {
        return MilanDiscoveryEvent::Available;
    }
    if (state == MilanDiscoveryState::TkDiscovered && (available || adp.is_entity_departing())) {
        return discovered_talker_event(ctx, adp, own);
    }
    return std::nullopt;
}

}  // namespace statusbar::atdecc
