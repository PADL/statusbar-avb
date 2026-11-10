#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// Generic MRP participant building blocks — shared by MSRP, MVRP.
//
// Scope: local endpoint only (not bridge capable). Protocol-specific
// participants (MsrpParticipant, MvrpParticipant) compose a PortState
// instance and manage their own protocol-specific attribute database.
//
// PortState owns the per-port shared state: the timer scheduler, the
// LeaveAll FSM, and the Periodic FSM. It provides event dispatch
// helpers that consume each FSM's Context outputs and wire them back
// into the timer scheduler.
//
// The template dispatch helpers in this header handle the per-attribute
// Applicant / Registrar FSM dispatch, including the TxRegistrarIn /
// TxRegistrarMt variant selection required by Clause 10.7.7 Note 8.
//

#include "statusbar/srp/srp_mrp_attribute.hpp"
#include "statusbar/srp/srp_mrp_leaveall_sm.hpp"
#include "statusbar/srp/srp_mrp_periodic_sm.hpp"
#include "statusbar/srp/srp_mrp_timers.hpp"

#include <concepts>
#include <cstdint>

namespace statusbar::srp::mrp {

//
// Per-port shared state: TimerScheduler + LeaveAll FSM + Periodic FSM.
//
// Not thread-safe; single participant event loop thread only.
//
class PortState
{
  public:
    /// Construct with @p timers' durations and an optional deterministic RNG seed for tests.
    explicit PortState(uint64_t rng_seed = 0, TimerConfig const& timers = {});

    /// Run BEGIN! on the LeaveAll and Periodic FSMs and arm the
    /// initial LeaveAll + Periodic timers. Call once during startup.
    void start(TimePoint now);

    /// Stop all timers. The FSMs retain their current state.
    void stop() noexcept;

    [[nodiscard]] auto timers() noexcept -> TimerScheduler& { return timers_; }
    [[nodiscard]] auto timers() const noexcept -> TimerScheduler const& { return timers_; }

    [[nodiscard]] auto leaveall_state() const noexcept -> leaveall_sm::Def::State { return lva_sm_.current_state(); }

    [[nodiscard]] auto periodic_state() const noexcept -> periodic_sm::Def::State { return periodic_sm_.current_state(); }

    /// Dispatch an event to the LeaveAll FSM. Consumes timer_restart
    /// and arms the LVA timer if requested. Returns the Context so
    /// the caller can observe tx_leaveall_pending.
    auto dispatch_leaveall(leaveall_sm::Def::Event event, TimePoint now) -> leaveall_sm::Context const&;

    /// Dispatch an event to the Periodic FSM. Consumes timer_restart
    /// and arms the periodic timer if requested.
    auto dispatch_periodic(periodic_sm::Def::Event event, TimePoint now) -> periodic_sm::Context const&;

  private:
    TimerScheduler timers_;
    leaveall_sm::Machine lva_sm_{};
    leaveall_sm::Context lva_ctx_{};
    periodic_sm::Machine periodic_sm_{};
    periodic_sm::Context periodic_ctx_{};
};

//
// Per-attribute dispatch helpers.
//
// These are templated on the record type (not just FirstValue) so
// they work uniformly for AttributeRecord<T> and for protocol-
// specific record types that carry extra per-attribute fields
// (e.g. MSRP's ListenerRecord with its 4-packed substate).
//
// The Record type must expose:
//   .applicant_ctx          — applicant_sm::Context
//   .applicant_sm           — applicant_sm::Machine
//   .registrar_ctx          — registrar_sm::Context
//   .registrar_sm           — registrar_sm::Machine
//   .registrar_is_in()      — bool
//

template <typename Record>
concept MrpAttributeLike = requires(Record r) {
    r.applicant_ctx.clear_outputs();
    r.applicant_sm.handle_event(r.applicant_ctx, applicant_sm::Def::Event::UCT, TimePoint{});
    r.registrar_ctx.clear_outputs();
    r.registrar_sm.handle_event(r.registrar_ctx, registrar_sm::Def::Event::UCT, TimePoint{});
    { r.registrar_is_in() } -> std::convertible_to<bool>;
};

/// Dispatch an event to the Applicant FSM. Clears tx outputs before
/// dispatch. Caller reads rec.applicant_ctx after return to learn
/// whether a transmit is pending.
template <MrpAttributeLike Record>
void dispatch_applicant(Record& rec, applicant_sm::Def::Event event, TimePoint now)
{
    rec.applicant_ctx.clear_outputs();
    rec.applicant_sm.handle_event(rec.applicant_ctx, event, now);
}

/// Start @p rec's own leavetimer at @p now (IEEE 802.1Q-2022 Clause 10.7.4.2) and run the per-port
/// leave timer to the earliest Registrar deadline.
template <MrpAttributeLike Record>
void start_registrar_leave(Record& rec, TimePoint now, TimerScheduler& timers) noexcept
{
    rec.registrar_ctx.leave_at = now + timers.config().leave_time;
    timers.start_leave_at(rec.registrar_ctx.leave_at);
}

/// Dispatch an event to the Registrar FSM. Clears notify outputs
/// before dispatch. If the transition sets lvtimer_request, the
/// Registrar's own leave deadline is set and the per-port leave timer
/// runs to the earliest one.
template <MrpAttributeLike Record>
void dispatch_registrar(Record& rec, registrar_sm::Def::Event event, TimePoint now, TimerScheduler& timers)
{
    rec.registrar_ctx.clear_outputs();
    rec.registrar_sm.handle_event(rec.registrar_ctx, event, now);
    if (rec.registrar_ctx.lvtimer_request) {
        start_registrar_leave(rec, now, timers);
    }
}

/// At the port's leave timer expiry: true when @p rec is in LV and its own leavetimer has run out (the
/// caller dispatches LvTimer); an LV Registrar not yet due runs the port's timer on to its deadline.
template <MrpAttributeLike Record>
[[nodiscard]] auto registrar_leave_due(Record const& rec, TimePoint now, TimerScheduler& timers) noexcept -> bool
{
    if (rec.registrar_sm.current_state() != registrar_sm::Def::State::Lv) {
        return false;
    }
    if (rec.registrar_ctx.leave_at > now) {
        timers.start_leave_at(rec.registrar_ctx.leave_at);
        return false;
    }
    return true;
}

/// Dispatch the tx! event to an attribute's Applicant. Selects the
/// TxRegistrarIn or TxRegistrarMt variant automatically based on the
/// paired Registrar's current state, as required by Clause 10.7.7
/// Note 8. After return, rec.applicant_ctx carries the tx outputs
/// (tx_pending, send_msg, encode) for the caller to build the PDU.
template <MrpAttributeLike Record>
void dispatch_applicant_tx(Record& rec, TimePoint now)
{
    auto const event = rec.registrar_is_in() ? applicant_sm::Def::Event::TxRegistrarIn : applicant_sm::Def::Event::TxRegistrarMt;
    dispatch_applicant(rec, event, now);
}

}  // namespace statusbar::srp::mrp
