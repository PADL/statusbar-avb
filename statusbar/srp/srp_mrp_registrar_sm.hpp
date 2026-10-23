#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// IEEE 802.1Q-2014 Clause 10.7.8 — MRP Registrar state machine
// =============================================================
//
// Scope: local endpoint only (not bridge capable).
//
// Three states per attribute: In, Lv, Mt. Compared to the 12-state
// Applicant, the Registrar is purely reactive: it observes received
// messages from peers, the paired Applicant's tx!LA event, and the
// local LeaveTimer, and emits notifications (New / Join / Lv) when
// the registered state changes. See OpenAvnu mrpd mrp.c:932-1054 for
// the reference imperative implementation.
//
// Side effects — Context outputs:
//   notify          — set by actions; what to report to observers
//                      (None / New / Join / Lv)
//   lvtimer_request — set true by actions that require the per-port
//                      LeaveTimer to be (re)started; cleared by the
//                      participant driver after it schedules the timer
//
// The participant driver MUST reset both fields via
// Context::clear_outputs() immediately before dispatching any event
// and MUST read them after the event handler returns to forward
// notifications and reschedule the LeaveTimer.
//
// Note on IN-self-loops: mrp.c's registrar has asymmetric "self-loop"
// behaviour:
//   - rNew! on state In sets notify=New (registered freshness update)
//   - rJoinIn!/rJoinMt! on state In does NOT set notify (silent refresh)
// This asymmetry is preserved exactly: the rNew! self-loop uses
// a_notify_new; the rJoinIn!/rJoinMt! IN cell is simply absent from
// the table (no-op), matching the mrp.c default: break; fallthrough.
//

#include "statusbar/sm/sm.hpp"

#include <cstdint>

namespace statusbar::srp::mrp::registrar_sm {

using namespace statusbar::sm;

/// Notification that the Registrar has asked the participant to deliver
/// to subscribed observers for this attribute. See mrp.h:70-73
/// (MRP_NOTIFY_*).
enum class Notify : uint8_t
{
    None = 0,  ///< no notification this pass
    New,       ///< first-time registration observed (rNew! from MT or LV)
    Join,      ///< refresh of an existing registration (rJoinIn/rJoinMt from MT or LV)
    Leave,     ///< deregistration (LvTimer expiry, Flush, rLv! / rLA! / TxLA! / ReDeclare! from IN)
};

/// Per-attribute Registrar state machine Context.
///
/// Holds only transition side-effect outputs; the state itself lives
/// in the associated StateMachine<> instance.
struct Context
{
    // --- Transition side-effect outputs ---
    // Reset these with clear_outputs() before each handle_event() call,
    // then read them back afterward.
    Notify notify{Notify::None};

    /// Set true by transitions that require the per-port LeaveTimer
    /// to be (re)started. See mrp.c:956 (mrp_lvtimer_start).
    bool lvtimer_request{false};

    constexpr void clear_outputs() noexcept
    {
        notify = Notify::None;
        lvtimer_request = false;
    }
};

struct Def
{
    using Context = registrar_sm::Context;

    /// Registrar states — IEEE 802.1Q-2014 Clause 10.7.8, mrp.h:96-98.
    enum class State : uint8_t
    {
        Start = 0,  ///< pseudo-state before BEGIN! (UCT -> Mt)
        In,         ///< registration observed and current
        Lv,         ///< leaving; awaiting LeaveTimer expiry to fall back to Mt
        Mt,         ///< no registration (empty)
        Count,
    };

    /// Registrar events — IEEE 802.1Q-2014 Clause 10.7.5.
    /// Events not listed in Clause 10.7.8 Table 10-4 (rIn!, rMt!) are
    /// deliberately absent from the table and therefore no-ops.
    enum class Event : uint8_t
    {
        UCT = 0,  ///< unconditional — BEGIN! initialization from Start
        RNew,     ///< rNew!      (10.7.5.14) received New
        RJoinIn,  ///< rJoinIn!   (10.7.5.15) received JoinIn
        RJoinMt,  ///< rJoinMt!   (10.7.5.16) received JoinMt (JoinEmpty)
        RLeave,   ///< rLv!       (10.7.5.17) received Lv
        /// rLv! with the Milan v1.2 Section 4.2.7.2.2 registrar change
        /// (IN / rLv! -> Lv -> MT, no leavetimer). The MSRP participant
        /// dispatches this instead of RLeave; MVRP keeps RLeave.
        RLeaveImmediate,
        RLeaveAll,   ///< rLA!       (10.7.5.20) received LeaveAll
        TxLeaveAll,  ///< txLA!      (10.7.5.8)  paired Applicant emitted LeaveAll
        Redeclare,   ///< ReDeclare! (10.7.5.3)  port role change
        LvTimer,     ///< LeaveTimer expiry (10.7.4.1)
        Flush,       ///< Flush!     (10.7.5.2)  port role change — forget registrations
        Count,
    };
};

//
// Action functions — defined in mrp_registrar_sm.cpp.
//

void a_init(Context& ctx, TimePoint time) noexcept;
void a_notify_new(Context& ctx, TimePoint time) noexcept;
void a_notify_join(Context& ctx, TimePoint time) noexcept;
void a_notify_lv(Context& ctx, TimePoint time) noexcept;
void a_start_lvtimer(Context& ctx, TimePoint time) noexcept;
void a_notify_lv_and_start_lvtimer(Context& ctx, TimePoint time) noexcept;

//
// Transition table — IEEE 802.1Q-2014 Clause 10.7.8 Table 10-4.
// Reference: OpenAvnu mrpd mrp.c:932-1054.
//
inline constexpr auto table = []() -> TransitionTable<Def> {
    using S = Def::State;
    using E = Def::Event;
    using T = Transitions<Def>;
    TransitionTable<Def> t{};

    // BEGIN! — initialization from Start via UCT (mrp.c:940-942)
    t.at(S::Start, E::UCT) = T::action<a_init>(S::Mt);

    // -----------------------------------------------------------------
    // rNew! — mrp.c:962-979
    // notify=New is set unconditionally (before the state switch), so
    // EVERY cell uses a_notify_new, including the In self-loop.
    // -----------------------------------------------------------------
    t.at(S::Mt, E::RNew) = T::action<a_notify_new>(S::In);
    t.at(S::In, E::RNew) = T::action<a_notify_new>(S::In);  // self-loop with notify
    t.at(S::Lv, E::RNew) = T::action<a_notify_new>(S::In);

    // -----------------------------------------------------------------
    // rJoinIn! and rJoinMt! — mrp.c:980-1001
    // NOTE asymmetric with rNew!: the In case does NOT set notify. It
    // is omitted from the table entirely (no-op via early return in
    // process_one_event), matching mrp.c's default: break; fallthrough.
    // -----------------------------------------------------------------
    t.at(S::Mt, E::RJoinIn) = T::action<a_notify_join>(S::In);
    t.at(S::Lv, E::RJoinIn) = T::action<a_notify_join>(S::In);
    t.at(S::Mt, E::RJoinMt) = T::action<a_notify_join>(S::In);
    t.at(S::Lv, E::RJoinMt) = T::action<a_notify_join>(S::In);
    // In -> In silent refresh: no entry = no-op

    // -----------------------------------------------------------------
    // rLv! — mrp.c:943-961
    // notify=Lv set unconditionally; state transition only for In.
    // -----------------------------------------------------------------
    t.at(S::In, E::RLeave) = T::action<a_notify_lv_and_start_lvtimer>(S::Lv);
    t.at(S::Lv, E::RLeave) = T::action<a_notify_lv>(S::Lv);  // self-loop with notify only
    t.at(S::Mt, E::RLeave) = T::action<a_notify_lv>(S::Mt);  // self-loop with notify only

    // -----------------------------------------------------------------
    // rLv! for MSRP: IN / rLv! -> (Lv) -> MT (Milan v1.2 Section 4.2.7.2.2)
    // -----------------------------------------------------------------
    t.at(S::In, E::RLeaveImmediate) = T::action<a_notify_lv>(S::Mt);
    t.at(S::Lv, E::RLeaveImmediate) = T::action<a_notify_lv>(S::Lv);
    t.at(S::Mt, E::RLeaveImmediate) = T::action<a_notify_lv>(S::Mt);

    // -----------------------------------------------------------------
    // rLA! and txLA! and ReDeclare! — mrp.c:946-961 (shared, no notify)
    // State transition only for In; Lv and Mt are no-ops.
    // -----------------------------------------------------------------
    t.at(S::In, E::RLeaveAll) = T::action<a_start_lvtimer>(S::Lv);
    t.at(S::In, E::TxLeaveAll) = T::action<a_start_lvtimer>(S::Lv);
    t.at(S::In, E::Redeclare) = T::action<a_start_lvtimer>(S::Lv);
    // Lv, Mt for all three events: no entry = no-op

    // -----------------------------------------------------------------
    // LvTimer — mrp.c:1003-1016
    // Only Lv->Mt fires notify; Mt->Mt in mrp.c is an explicit self-loop
    // with no notify and no other side effect; we omit it (observationally
    // equivalent to no entry).
    // -----------------------------------------------------------------
    t.at(S::Lv, E::LvTimer) = T::action<a_notify_lv>(S::Mt);

    // -----------------------------------------------------------------
    // Flush — mrp.c:1017-1030
    // All three states collapse to Mt with notify=Lv set unconditionally.
    // -----------------------------------------------------------------
    t.at(S::In, E::Flush) = T::action<a_notify_lv>(S::Mt);
    t.at(S::Lv, E::Flush) = T::action<a_notify_lv>(S::Mt);
    t.at(S::Mt, E::Flush) = T::action<a_notify_lv>(S::Mt);  // self-loop with notify

    // -----------------------------------------------------------------
    // rIn! and rMt! — mrp.c:1031-1038 — intentionally ignored per
    // Clause 10.7.8 Table 10-4. No table entries.
    // -----------------------------------------------------------------

    return t;
}();

using Machine = StateMachine<Def, table>;

}  // namespace statusbar::srp::mrp::registrar_sm
