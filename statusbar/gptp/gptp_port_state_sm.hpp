#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// PortStateSM — high-level slave-port lifecycle state machine.
//
// Drives the port through power-up, link acquisition, asCapable
// acquisition (if not pre-enabled), and finally entering Slave state
// where media transmission over the disciplined clock is authorized.
//
// Scope: slave-only. There is no Master, PreMaster, or BMCA path.
//
// Profile differences:
//   - Standard profile: asCapable requires a successful Pdelay
//     exchange before Uncalibrated->Slave is allowed. The SM waits
//     in Uncalibrated until LinkDelayMeasured AND AsCapableAcquired
//     are both satisfied.
//   - AVnu Automotive Profile with as_capable_initial=true: asCapable
//     is pre-granted on LinkUp. The SM jumps Initializing->Uncalibrated
//     immediately and then Uncalibrated->Slave on the first successful
//     sync.
//
// Outputs are side-effect flags on the Context, consumed by the
// containing GptpSlavePort:
//   - as_capable      — current asCapable flag for observer reporting
//   - synced          — true once FirstSyncLocked was observed (cleared on SyncLost)
//

#include "statusbar/sm/sm.hpp"

#include <cstdint>

namespace statusbar::gptp::port_state_sm {

using namespace statusbar::sm;

struct Context
{
    // Configuration snapshot (set once at participant construction).
    bool as_capable_initial{false};  // from GptpConfig::as_capable_initial

    // Outputs (read by the participant after each event dispatch).
    bool as_capable{false};
    bool synced{false};

    constexpr void clear_transient_outputs() noexcept
    {
        // as_capable and synced are cumulative state, not per-event
        // outputs — leave them alone across dispatches.
    }
};

struct Def
{
    using Context = port_state_sm::Context;

    /// Port lifecycle states — slave only. No Master/PreMaster/BMCA.
    enum class State : uint8_t
    {
        Start = 0,     ///< pseudo-state before BEGIN (UCT -> Disabled)
        Disabled,      ///< port administratively or physically down
        Initializing,  ///< powered on, waiting for link
        Listening,     ///< link up, waiting for asCapable (Std profile only)
        Uncalibrated,  ///< asCapable acquired, waiting for first sync
        Slave,         ///< synchronized; media transmission authorized
        Count,
    };

    enum class Event : uint8_t
    {
        UCT = 0,                ///< unconditional — initialization from Start
        LinkUp,                 ///< physical link came up
        LinkDown,               ///< physical link went down
        AsCapableAcquired,      ///< Pdelay succeeded (Std) or as_capable_initial (Auto)
        AsCapableLost,          ///< Pdelay threshold exceeded or too many lost responses
        FirstSyncLocked,        ///< servo applied first sync (Uncalibrated -> Slave)
        SyncLost,               ///< sync receipt timeout fired
        AdministrativeDisable,  ///< software told the port to go down
        Count,
    };
};

//
// Action functions — defined in gptp_port_state_sm.cpp.
//

void a_init(Context& ctx, TimePoint time) noexcept;
void a_enter_initializing(Context& ctx, TimePoint time) noexcept;
void a_enter_listening(Context& ctx, TimePoint time) noexcept;
void a_enter_uncalibrated(Context& ctx, TimePoint time) noexcept;
void a_enter_uncalibrated_with_pre_asc(Context& ctx, TimePoint time) noexcept;
void a_enter_slave(Context& ctx, TimePoint time) noexcept;
void a_enter_disabled(Context& ctx, TimePoint time) noexcept;
void a_drop_as_capable(Context& ctx, TimePoint time) noexcept;
void a_lose_sync(Context& ctx, TimePoint time) noexcept;

//
// Transition table. Slave-only lifecycle; no BMCA paths.
//
inline constexpr auto table = []() -> TransitionTable<Def> {
    using S = Def::State;
    using E = Def::Event;
    using T = Transitions<Def>;
    TransitionTable<Def> t{};

    // BEGIN — Start -> Disabled (no link assumed).
    t.at(S::Start, E::UCT) = T::action<a_init>(S::Disabled);

    // Disabled -> Initializing on LinkUp.
    t.at(S::Disabled, E::LinkUp) = T::action<a_enter_initializing>(S::Initializing);
    // Disabled ignores LinkDown (already down) and sync events.

    // Initializing: we just powered up and link is up. What happens
    // next depends on the profile, which is carried on the Context.
    // In Standard profile we move to Listening to wait for asCapable;
    // in Automotive with as_capable_initial=true we jump straight to
    // Uncalibrated (the a_enter_uncalibrated_with_pre_asc action
    // reads ctx.as_capable_initial and sets ctx.as_capable = true).
    //
    // The sm framework doesn't support guards, so we express both
    // transitions in the table and let the driver pick which event
    // to fire. We use two events: LinkUp fires Initializing ->
    // Listening (then the driver inspects as_capable_initial and, if
    // true, immediately dispatches AsCapableAcquired which takes us
    // Listening -> Uncalibrated).
    t.at(S::Initializing, E::AsCapableAcquired) = T::action<a_enter_listening>(S::Listening);
    t.at(S::Initializing, E::LinkDown) = T::transition(S::Disabled);

    // Listening: waiting for asCapable (dynamic in Standard profile).
    t.at(S::Listening, E::AsCapableAcquired) = T::action<a_enter_uncalibrated>(S::Uncalibrated);
    t.at(S::Listening, E::LinkDown) = T::transition(S::Disabled);

    // Uncalibrated: asCapable, waiting for first sync lock.
    t.at(S::Uncalibrated, E::FirstSyncLocked) = T::action<a_enter_slave>(S::Slave);
    t.at(S::Uncalibrated, E::AsCapableLost) = T::action<a_drop_as_capable>(S::Listening);
    t.at(S::Uncalibrated, E::LinkDown) = T::transition(S::Disabled);

    // Slave: synchronized. Sync loss drops us back to Uncalibrated;
    // asCapable loss takes us all the way back to Listening.
    t.at(S::Slave, E::SyncLost) = T::action<a_lose_sync>(S::Uncalibrated);
    t.at(S::Slave, E::AsCapableLost) = T::action<a_drop_as_capable>(S::Listening);
    t.at(S::Slave, E::LinkDown) = T::transition(S::Disabled);

    // AdministrativeDisable applies from any state. Express it for
    // the states that are worth supporting.
    t.at(S::Initializing, E::AdministrativeDisable) = T::transition(S::Disabled);
    t.at(S::Listening, E::AdministrativeDisable) = T::transition(S::Disabled);
    t.at(S::Uncalibrated, E::AdministrativeDisable) = T::transition(S::Disabled);
    t.at(S::Slave, E::AdministrativeDisable) = T::transition(S::Disabled);

    // Entry hook: every arrival in Disabled quiesces the port. Also fires
    // after a_init on the initial UCT edge (a no-op on a fresh context).
    t.on_entry(S::Disabled) = T::hook<a_enter_disabled>();

    return t;
}();

using Machine = StateMachine<Def, table>;

}  // namespace statusbar::gptp::port_state_sm
