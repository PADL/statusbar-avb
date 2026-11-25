#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Milan v1.3 listener discovery state machine, section 5.6.4.
///
/// One machine per bound STREAM_INPUT, watching the ADP messages of the one
/// talker the sink is bound to. It differs from the discovery of IEEE
/// 1722.1-2021 6.2.6: a talker counts as discovered only on this listener's
/// gPTP domain and grandmaster, and a talker that restarted (available_index
/// not above the last, or another configuration) is reported as departed and
/// discovered again so the sink probes it afresh.
///
/// The machine does not call the sink's connection machine itself. An action
/// leaves what 5.6.4.5 says to trigger in the context (departed, discovered),
/// and whoever runs the two machines of a sink delivers it, in that order.
///
/// Table 5.57's two states are joined by Stopped: the sink's machine starts
/// and stops this one ("Start the ADP Discovery state machine", "Stop ...").

#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/sm/sm.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace statusbar::atdecc {

/// This listener's gPTP state on the port: what a talker's ENTITY_AVAILABLE
/// must name to be discovered (5.6.4.5.1 step 1).
struct MilanGptpState
{
    ClockIdentity grandmaster_id{};
    uint8_t domain_number{0};

    auto operator==(MilanGptpState const&) const noexcept -> bool = default;
};

enum class MilanDiscoveryState : uint8_t
{
    Stopped,          ///< not running: the sink is not bound
    TkNotDiscovered,  ///< TK_NOT_DISCOVERED
    TkDiscovered,     ///< TK_DISCOVERED
    Count
};

/// Table 5.58's events, RCV_ADP_AVAILABLE split by the checks of 5.6.4.5.
enum class MilanDiscoveryEvent : uint8_t
{
    Start,               ///< the sink's machine starts discovery
    Stop,                ///< the sink's machine stops discovery
    Available,           ///< RCV_ADP_AVAILABLE on our gPTP domain and grandmaster, talker not discovered
    Refreshed,           ///< RCV_ADP_AVAILABLE from the discovered talker, nothing restarted
    Restarted,           ///< RCV_ADP_AVAILABLE showing a restart, still on our gPTP domain
    RestartedElsewhere,  ///< RCV_ADP_AVAILABLE showing a restart, on another gPTP domain or grandmaster
    Departing,           ///< RCV_ADP_DEPARTING
    TmrNoAdp,            ///< TMR_NO_ADP
    Count
};

struct MilanDiscoveryContext
{
    /// The ADP message the event being handled arrived in.
    AdpDu rcvd{};

    /// What the talker's last accepted ENTITY_AVAILABLE said. The
    /// configuration index is noted only when the advertisement marks the
    /// field valid (AEM_CONFIGURATION_INDEX_VALID, IEEE 1722.1-2021 6.2.1.10):
    /// 5.6.4.5.2 compares it "if present".
    uint16_t interface_index{0};
    uint32_t available_index{0};
    std::optional<uint16_t> current_configuration_index{};

    /// TMR_NO_ADP: when the talker's advertisement lapses.
    std::optional<sm::TimePoint> timer{};

    /// What the last event tells the sink's connection machine, delivered
    /// departed first: EVT_TK_DEPARTED, EVT_TK_DISCOVERED.
    bool departed{false};
    bool discovered{false};
};

/// The time an ENTITY_AVAILABLE stays good for: valid_time counts 2 second
/// units. A valid_time of 0, which no ENTITY_AVAILABLE should carry (IEEE
/// 1722.1-2021 6.2.1.6), is taken as one unit, so that such a talker is
/// discovered for two seconds and not for none.
[[nodiscard]] constexpr auto milan_adp_valid_for(AdpDu const& adp) noexcept -> std::chrono::seconds
{
    int const units = adp.valid_time() == 0 ? 1 : static_cast<int>(adp.valid_time());
    return std::chrono::seconds{2 * units};
}

/// An ENTITY_AVAILABLE heard earlier and still good: what an entity's ADP
/// discovery holds of a talker when a sink is bound to it.
struct MilanHeardAdvertisement
{
    AdpDu adp{};
    sm::TimePoint valid_until{};  ///< when TMR_NO_ADP, run from its arrival, falls due
};

namespace milan_discovery_actions {

void start(MilanDiscoveryContext& ctx, sm::TimePoint now);
void stop(MilanDiscoveryContext& ctx, sm::TimePoint now);
void discover(MilanDiscoveryContext& ctx, sm::TimePoint now);
void refresh(MilanDiscoveryContext& ctx, sm::TimePoint now);
void rediscover(MilanDiscoveryContext& ctx, sm::TimePoint now);
void depart(MilanDiscoveryContext& ctx, sm::TimePoint now);

}  // namespace milan_discovery_actions

struct MilanDiscoveryDef
{
    using Context = MilanDiscoveryContext;
    using State = MilanDiscoveryState;
    using Event = MilanDiscoveryEvent;
};

namespace milan_discovery_table_detail {

using T = sm::Transitions<MilanDiscoveryDef>;
using Table = sm::TransitionTable<MilanDiscoveryDef>;
using State = MilanDiscoveryState;
using Event = MilanDiscoveryEvent;
namespace a = milan_discovery_actions;

/// The sink's machine may start or stop discovery whatever it is doing.
consteval void add_start_and_stop(Table& t)
{
    for (auto const s : {State::Stopped, State::TkNotDiscovered, State::TkDiscovered}) {
        t.at(s, Event::Start) = T::action<a::start>(State::TkNotDiscovered);
        t.at(s, Event::Stop) = T::action<a::stop>(State::Stopped);
    }
}

/// TK_DISCOVERED: 5.6.4.5.2 (steps 3, 2 and 2b), 5.6.4.5.3, 5.6.4.5.4
consteval void add_discovered_rows(Table& t)
{
    t.at(State::TkDiscovered, Event::Refreshed) = T::action<a::refresh>(State::TkDiscovered);
    t.at(State::TkDiscovered, Event::Restarted) = T::action<a::rediscover>(State::TkDiscovered);
    t.at(State::TkDiscovered, Event::RestartedElsewhere) = T::action<a::depart>(State::TkNotDiscovered);
    t.at(State::TkDiscovered, Event::Departing) = T::action<a::depart>(State::TkNotDiscovered);
    t.at(State::TkDiscovered, Event::TmrNoAdp) = T::action<a::depart>(State::TkNotDiscovered);
}

}  // namespace milan_discovery_table_detail

/// Table 5.59, with Start and Stop.
consteval auto make_milan_discovery_table()
{
    sm::TransitionTable<MilanDiscoveryDef> t{};
    milan_discovery_table_detail::add_start_and_stop(t);
    // TK_NOT_DISCOVERED: 5.6.4.5.1
    t.at(MilanDiscoveryState::TkNotDiscovered, MilanDiscoveryEvent::Available) =
        sm::Transitions<MilanDiscoveryDef>::action<milan_discovery_actions::discover>(MilanDiscoveryState::TkDiscovered);
    milan_discovery_table_detail::add_discovered_rows(t);
    return t;
}

inline constexpr auto milan_discovery_table = make_milan_discovery_table();

/// The Milan listener discovery state machine.
template <typename Observer = sm::NullObserver>
using MilanDiscoveryStateMachine = sm::StateMachine<MilanDiscoveryDef, milan_discovery_table, Observer>;

/// The event an ADP message from the bound talker is in @p state, nullopt
/// when 5.6.4.5 says to ignore it: an ENTITY_AVAILABLE on another gPTP domain
/// or grandmaster while not discovered, any message on another interface_index
/// while discovered, an ENTITY_DISCOVER.
[[nodiscard]] auto milan_discovery_event_for(
    MilanDiscoveryContext const& ctx, MilanDiscoveryState state, AdpDu const& adp, MilanGptpState const& own) noexcept
    -> std::optional<MilanDiscoveryEvent>;

}  // namespace statusbar::atdecc
