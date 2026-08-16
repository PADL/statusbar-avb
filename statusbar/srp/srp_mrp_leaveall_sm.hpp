#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// IEEE 802.1Q-2014 Clause 10.7.5.22 — MRP LeaveAll state machine
// ===============================================================
//
// Scope: local endpoint only. One instance per MRP participant (per
// port, per protocol) — NOT per attribute.
//
// The LeaveAll FSM periodically triggers a broadcast LeaveAll on the
// outbound MRPDU so that peers re-assert their registrations. It has
// only two substantive states — Passive and Active — plus a pseudo
// Start state.
//
// Reference: OpenAvnu mrpd mrp.c::mrp_lvatimer_fsm at mrp.c:446-504.
//
// Side effects — Context outputs:
//   tx_leaveall_pending — set true when a tx! transition demands that
//                          the next outgoing MRPDU carry the LeaveAll
//                          flag in its vector header(s).
//   timer_restart       — set true when the driver must (re)arm the
//                          per-port LeaveAll timer with a fresh
//                          randomized LeaveAllTime interval
//                          (Clause 10.7.4.2: 0.5 × LeaveAllTime to
//                          1.5 × LeaveAllTime).
//
// The participant driver MUST reset both fields via
// Context::clear_outputs() immediately before dispatching any event
// and MUST read them after the event handler returns.
//

#include "statusbar/sm/sm.hpp"

#include <cstdint>

namespace statusbar::srp::mrp::leaveall_sm {

using namespace statusbar::sm;

struct Context
{
    // --- Transition side-effect outputs ---
    bool tx_leaveall_pending{false};
    bool timer_restart{false};

    constexpr void clear_outputs() noexcept
    {
        tx_leaveall_pending = false;
        timer_restart = false;
    }
};

struct Def
{
    using Context = leaveall_sm::Context;

    /// LeaveAll FSM states — mrp.h:155-156.
    enum class State : uint8_t
    {
        Start = 0,  ///< pseudo-state before BEGIN! (UCT -> Passive)
        Passive,    ///< quiescent; timer running toward next fire
        Active,     ///< timer expired; next tx! emits LeaveAll
        Count,
    };

    /// LeaveAll FSM events.
    enum class Event : uint8_t
    {
        UCT = 0,    ///< unconditional — BEGIN! initialization from Start
        Tx,         ///< tx! — paired with an attribute-level TX event
        RLeaveAll,  ///< rLA! — received a LeaveAll
        LvaTimer,   ///< LeaveAllTime expired
        Count,
    };
};

//
// Action functions — defined in mrp_leaveall_sm.cpp.
//

void a_init(Context& ctx, TimePoint time) noexcept;
void a_tx_leaveall(Context& ctx, TimePoint time) noexcept;
void a_restart_timer(Context& ctx, TimePoint time) noexcept;

//
// Transition table — IEEE 802.1Q-2014 Clause 10.7.5.22.
// Reference: OpenAvnu mrpd mrp.c:446-504.
//
inline constexpr auto table = []() -> TransitionTable<Def> {
    using S = Def::State;
    using E = Def::Event;
    using T = Transitions<Def>;
    TransitionTable<Def> t{};

    // BEGIN! — mrp.c:458-461 — initialize Passive and start timer.
    t.at(S::Start, E::UCT) = T::action<a_init>(S::Passive);

    // tx! — mrp.c:462-468 — only the Active state actually transmits.
    // Passive + Tx: no entry (no-op) — matches the implicit fall-through.
    t.at(S::Active, E::Tx) = T::action<a_tx_leaveall>(S::Passive);

    // rLA! — mrp.c:469-473 — restart timer from any state.
    t.at(S::Passive, E::RLeaveAll) = T::action<a_restart_timer>(S::Passive);  // self
    t.at(S::Active, E::RLeaveAll) = T::action<a_restart_timer>(S::Passive);

    // LvaTimer — mrp.c:474-478 — timer fired; enter Active and rearm.
    t.at(S::Passive, E::LvaTimer) = T::transition(S::Active);
    t.at(S::Active, E::LvaTimer) = T::transition(S::Active);  // self

    // Entry hook: arriving in Active (fresh or via the self-loop) restarts
    // the LeaveAll timer. The RLeaveAll edges keep their explicit restart
    // actions: Active->Passive is covered by neither hook, and a Passive
    // exit hook would double-fire on the LvaTimer edge.
    t.on_entry(S::Active) = T::hook<a_restart_timer>();

    return t;
}();

using Machine = StateMachine<Def, table>;

}  // namespace statusbar::srp::mrp::leaveall_sm
