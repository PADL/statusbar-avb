#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// IEEE 802.1Q-2014 Clause 10.7.7 — MRP Applicant state machine
// =============================================================
//
// Scope: local endpoint only (not bridge capable).
//
// operPointToPointMAC is hardcoded to TRUE, which collapses the
// shared-media branches in rJoinIn! and rIn! transitions (Clause
// 10.7.7 Table 10-3 notes 4-7) into unconditional transitions. For
// the reference imperative implementation see OpenAvnu mrpd
// mrp.c::mrp_applicant_fsm() at lines 551-904.
//
// Cross-FSM coupling — IEEE 802.1Q-2014 Clause 10.7.7 Note 8: on the
// tx! event from state An, the next state depends on whether the
// paired Registrar is in the In state. With operPointToPointMAC=TRUE
// this is the ONLY runtime-dependent transition in the entire
// Applicant FSM. We resolve it by splitting the single tx! event
// into two variants, TxRegistrarIn and TxRegistrarMt, which the
// participant driver selects based on the current Registrar state
// before dispatching. All transition cells in the two columns are
// identical EXCEPT the An cell, which splits Qa vs Aa.
//
// Side effects — tx outputs in Context: every transmit-producing
// transition records its intent (what to send, and whether the
// encoding is required or optional) into Context::tx_pending,
// Context::send_msg, and Context::encode. The participant driver
// MUST reset these via Context::clear_outputs() immediately before
// dispatching any event and MUST read them after the event handler
// returns to build the outgoing PDU. The send_msg is an internal
// intent (New/Join/In/Leave) that the transmit path translates to a
// wire AttributeEvent (New/JoinIn/JoinMt/In/Mt/Lv) by consulting the
// paired Registrar's current state.
//

#include "statusbar/sm/sm.hpp"

#include <array>
#include <cstdint>

namespace statusbar::srp::mrp::applicant_sm {

using namespace statusbar::sm;

/// Intent to transmit for this attribute. This is the internal sndmsg
/// from mrp.c (mrp.h:126-132), NOT a wire AttributeEvent. The
/// participant's transmit path translates this to a wire AttributeEvent
/// by looking at the paired Registrar state:
///
///     Join  + registrar In    -> AttributeEvent::JoinIn
///     Join  + registrar Mt/Lv -> AttributeEvent::JoinMt
///     In    + registrar In    -> AttributeEvent::In
///     In    + registrar Mt/Lv -> AttributeEvent::Mt
///     New   (always)          -> AttributeEvent::New
///     Leave (always)          -> AttributeEvent::Lv
enum class SendMessage : uint8_t
{
    None = 0,  ///< no transmit requested
    New,       ///< sndmsg = NEW
    Join,      ///< sndmsg = JOIN — becomes JoinIn or JoinMt at encode time
    In,        ///< sndmsg = IN   — becomes In or Mt at encode time (NULL attribute)
    Leave,     ///< sndmsg = LV
};

/// Transmit urgency for the outgoing vector attribute. Optional means
/// the attribute MAY be omitted if doing so produces a smaller PDU.
/// See mrp.c:48-49 (MRP_ENCODE_YES / MRP_ENCODE_OPTIONAL).
enum class Encoding : uint8_t
{
    None = 0,  ///< no transmit requested this pass
    Required,  ///< must send (MRP_ENCODE_YES)
    Optional,  ///< send if it improves encoding (MRP_ENCODE_OPTIONAL)
};

/// Per-attribute Applicant state machine Context.
///
/// Holds only the transition side-effect outputs; the state itself lives
/// in the associated StateMachine<> instance.
struct Context
{
    // --- Transition side-effect outputs ---
    // Reset these with clear_outputs() before each handle_event() call,
    // then read them back afterward to decide what to transmit.
    bool tx_pending{false};
    SendMessage send_msg{SendMessage::None};
    Encoding encode{Encoding::None};

    constexpr void clear_outputs() noexcept
    {
        tx_pending = false;
        send_msg = SendMessage::None;
        encode = Encoding::None;
    }
};

struct Def
{
    using Context = applicant_sm::Context;

    /// Applicant states — IEEE 802.1Q-2014 Clause 10.7.7, mrp.h:82-93.
    /// State naming: first letter is the Applicant's urgency
    /// (V=VeryAnxious, A=Anxious, Q=Quiet, L=Leaving), second letter
    /// hints at the paired Registrar (N=New, A=Active, P=Passive,
    /// O=Observer). The paired Registrar state is authoritative; these
    /// letters are a mnemonic only.
    enum class State : uint8_t
    {
        Start = 0,  ///< pseudo-state before BEGIN! (UCT -> Vo)
        Vo,         ///< Very Anxious Observer
        Vp,         ///< Very Anxious Passive
        Vn,         ///< Very Anxious New
        An,         ///< Anxious New
        Aa,         ///< Anxious Active
        Qa,         ///< Quiet Active
        La,         ///< Leaving Active
        Ao,         ///< Anxious Observer
        Qo,         ///< Quiet Observer
        Ap,         ///< Anxious Passive
        Qp,         ///< Quiet Passive
        Lo,         ///< Leaving Observer
        Count,
    };

    /// IEEE 802.1Q Table 10-3: an Applicant asks for a transmit opportunity on
    /// entry to VN, AN, AA, LA, VP, AP and LO, the states that have a message
    /// to send. The quiet and the pure observer states (QA, QP, VO, AO, QO)
    /// do not.
    /// The states of an Applicant that declares nothing: VO, AO and QO. What
    /// Table 10-3 lets them send on tx! is the optional [s], an In or Mt that
    /// only helps pack a vector.
    [[nodiscard]] static constexpr auto is_observer(State const s) noexcept -> bool
    {
        return s == State::Vo || s == State::Ao || s == State::Qo;
    }

    [[nodiscard]] static constexpr auto wants_transmit(State const s) noexcept -> bool
    {
        return s == State::Vn || s == State::An || s == State::Aa || s == State::La || s == State::Vp || s == State::Ap ||
            s == State::Lo;
    }

    /// Applicant events — IEEE 802.1Q-2014 Clause 10.7.5.
    enum class Event : uint8_t
    {
        UCT = 0,         ///< unconditional — BEGIN! initialization from Start
        New,             ///< New!       (10.7.5.4)  local declare new participant
        Join,            ///< Join!      (10.7.5.5)  local declare
        Leave,           ///< Lv!        (10.7.5.6)  local withdraw
        TxRegistrarIn,   ///< tx! variant when paired Registrar is In   (Note 8)
        TxRegistrarMt,   ///< tx! variant when paired Registrar is Mt/Lv (Note 8)
        TxLeaveAll,      ///< txLA!      (10.7.5.8)  transmit with LeaveAll flag
        TxLeaveAllFull,  ///< txLAF!     (10.7.5.9)  transmit with LVA, no room
        RNew,            ///< rNew!      (10.7.5.14) received New
        RJoinIn,         ///< rJoinIn!   (10.7.5.15) received JoinIn
        RIn,             ///< rIn!       (10.7.5.18) received In
        RJoinMt,         ///< rJoinMt!   (10.7.5.16) received JoinMt (JoinEmpty)
        RMt,             ///< rMt!       (10.7.5.19) received Mt (Empty)
        RLeave,          ///< rLv!       (10.7.5.17) received Lv
        RLeaveAll,       ///< rLA!       (10.7.5.20) received LeaveAll
        Redeclare,       ///< ReDeclare! (10.7.5.3)  port role change
        Periodic,        ///< periodic!             periodic timer tick
        Count,
    };
};

//
// Action functions — defined in mrp_applicant_sm.cpp.
// Each action records the transition's transmit intent into the Context.
//

void a_init(Context& ctx, TimePoint time) noexcept;
void a_tx_new(Context& ctx, TimePoint time) noexcept;
void a_tx_join_required(Context& ctx, TimePoint time) noexcept;
void a_tx_join_optional(Context& ctx, TimePoint time) noexcept;
void a_tx_in_required(Context& ctx, TimePoint time) noexcept;
void a_tx_in_optional(Context& ctx, TimePoint time) noexcept;
void a_tx_leave(Context& ctx, TimePoint time) noexcept;

//
// Transition table — IEEE 802.1Q-2014 Clause 10.7.7 Table 10-3.
// Reference: OpenAvnu mrpd mrp.c:551-904.
//
// Line-number references in comments point to the corresponding branch
// in mrp.c for auditability.
//
inline constexpr auto table = []() -> TransitionTable<Def> {
    using S = Def::State;
    using E = Def::Event;
    using T = Transitions<Def>;
    TransitionTable<Def> t{};

    // BEGIN! — initialization from Start via UCT (mrp.c:562-564)
    t.at(S::Start, E::UCT) = T::action<a_init>(S::Vo);

    // -----------------------------------------------------------------
    // New! — mrp.c:565-579
    // Vn, An: stay. All others: -> Vn.
    // -----------------------------------------------------------------
    t.at(S::Vo, E::New) = T::transition(S::Vn);
    t.at(S::Vp, E::New) = T::transition(S::Vn);
    t.at(S::Aa, E::New) = T::transition(S::Vn);
    t.at(S::Qa, E::New) = T::transition(S::Vn);
    t.at(S::La, E::New) = T::transition(S::Vn);
    t.at(S::Ao, E::New) = T::transition(S::Vn);
    t.at(S::Qo, E::New) = T::transition(S::Vn);
    t.at(S::Ap, E::New) = T::transition(S::Vn);
    t.at(S::Qp, E::New) = T::transition(S::Vn);
    t.at(S::Lo, E::New) = T::transition(S::Vn);

    // -----------------------------------------------------------------
    // Join! — mrp.c:580-602
    // -----------------------------------------------------------------
    t.at(S::Lo, E::Join) = T::transition(S::Vp);
    t.at(S::Vo, E::Join) = T::transition(S::Vp);
    t.at(S::La, E::Join) = T::transition(S::Aa);
    t.at(S::Ao, E::Join) = T::transition(S::Ap);
    t.at(S::Qo, E::Join) = T::transition(S::Qp);
    // Vn, An, Aa, Qa, Vp, Ap, Qp: no transition = no-op (default: break)

    // -----------------------------------------------------------------
    // Lv! — mrp.c:603-625
    // -----------------------------------------------------------------
    t.at(S::Vn, E::Leave) = T::transition(S::La);
    t.at(S::An, E::Leave) = T::transition(S::La);
    t.at(S::Aa, E::Leave) = T::transition(S::La);
    t.at(S::Qa, E::Leave) = T::transition(S::La);
    t.at(S::Vp, E::Leave) = T::transition(S::Vo);
    t.at(S::Ap, E::Leave) = T::transition(S::Ao);
    t.at(S::Qp, E::Leave) = T::transition(S::Qo);
    // La, Vo, Ao, Qo, Lo: no change

    // -----------------------------------------------------------------
    // tx! — mrp.c:719-791 — split into TxRegistrarIn / TxRegistrarMt.
    // The two variants are byte-identical EXCEPT at state An.
    // -----------------------------------------------------------------
    constexpr std::array<E, 2> tx_events{E::TxRegistrarIn, E::TxRegistrarMt};
    for (auto const ev : tx_events) {
        t.at(S::Vo, ev) = T::action<a_tx_in_optional>(S::Vo);    // NULL if encoding improves
        t.at(S::Vp, ev) = T::action<a_tx_join_required>(S::Aa);  // send Join
        t.at(S::Vn, ev) = T::action<a_tx_new>(S::An);            // send New
        t.at(S::Aa, ev) = T::action<a_tx_join_required>(S::Qa);  // send Join
        t.at(S::Ap, ev) = T::action<a_tx_join_required>(S::Qa);  // send Join
        t.at(S::Qa, ev) = T::action<a_tx_join_optional>(S::Qa);  // Join if encoding improves
        t.at(S::La, ev) = T::action<a_tx_leave>(S::Vo);          // send Lv
        t.at(S::Ao, ev) = T::action<a_tx_in_optional>(S::Ao);    // NULL if encoding improves
        t.at(S::Qo, ev) = T::action<a_tx_in_optional>(S::Qo);    // NULL if encoding improves
        t.at(S::Qp, ev) = T::action<a_tx_in_optional>(S::Qp);    // NULL if encoding improves
        t.at(S::Lo, ev) = T::action<a_tx_in_required>(S::Vo);    // send NULL
    }
    // The ONE cell where the two variants differ (mrp.c:743-754 Note 8):
    t.at(S::An, E::TxRegistrarIn) = T::action<a_tx_new>(S::Qa);  // registrar In -> Qa
    t.at(S::An, E::TxRegistrarMt) = T::action<a_tx_new>(S::Aa);  // registrar Mt -> Aa

    // -----------------------------------------------------------------
    // txLA! — mrp.c:627-687
    // -----------------------------------------------------------------
    t.at(S::Vo, E::TxLeaveAll) = T::action<a_tx_in_optional>(S::Lo);
    t.at(S::Vp, E::TxLeaveAll) = T::action<a_tx_in_required>(S::Aa);
    t.at(S::Vn, E::TxLeaveAll) = T::action<a_tx_new>(S::An);
    t.at(S::An, E::TxLeaveAll) = T::action<a_tx_new>(S::Qa);
    t.at(S::Qp, E::TxLeaveAll) = T::action<a_tx_join_required>(S::Qa);
    t.at(S::Ap, E::TxLeaveAll) = T::action<a_tx_join_required>(S::Qa);
    t.at(S::Aa, E::TxLeaveAll) = T::action<a_tx_join_required>(S::Qa);
    t.at(S::Qa, E::TxLeaveAll) = T::action<a_tx_join_required>(S::Qa);  // self-loop
    t.at(S::La, E::TxLeaveAll) = T::action<a_tx_in_optional>(S::Lo);
    t.at(S::Ao, E::TxLeaveAll) = T::action<a_tx_in_optional>(S::Lo);
    t.at(S::Qo, E::TxLeaveAll) = T::action<a_tx_in_optional>(S::Lo);
    t.at(S::Lo, E::TxLeaveAll) = T::action<a_tx_in_optional>(S::Lo);  // self-loop

    // -----------------------------------------------------------------
    // txLAF! — mrp.c:688-718 — no tx output, state change only
    // -----------------------------------------------------------------
    t.at(S::Vo, E::TxLeaveAllFull) = T::transition(S::Lo);
    t.at(S::An, E::TxLeaveAllFull) = T::transition(S::Vn);
    t.at(S::Qp, E::TxLeaveAllFull) = T::transition(S::Vp);
    t.at(S::Ap, E::TxLeaveAllFull) = T::transition(S::Vp);
    t.at(S::Aa, E::TxLeaveAllFull) = T::transition(S::Vp);
    t.at(S::Qa, E::TxLeaveAllFull) = T::transition(S::Vp);
    t.at(S::Qo, E::TxLeaveAllFull) = T::transition(S::Lo);
    t.at(S::Ao, E::TxLeaveAllFull) = T::transition(S::Lo);
    t.at(S::La, E::TxLeaveAllFull) = T::transition(S::Lo);
    // Lo, Vp, Vn: stay

    // -----------------------------------------------------------------
    // rNew! — mrp.c:792-794 — Applicant does nothing (Registrar notifies).
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // rJoinIn! — mrp.c:796-819
    // With operPointToPointMAC=TRUE, the Vo->Ao and Vp->Ap branches are
    // not taken and are omitted from the table.
    // -----------------------------------------------------------------
    t.at(S::Aa, E::RJoinIn) = T::transition(S::Qa);
    t.at(S::Ao, E::RJoinIn) = T::transition(S::Qo);
    t.at(S::Ap, E::RJoinIn) = T::transition(S::Qp);

    // -----------------------------------------------------------------
    // rIn! — mrp.c:820-829
    // With operPointToPointMAC=TRUE, the Aa->Qa branch is unconditional.
    // -----------------------------------------------------------------
    t.at(S::Aa, E::RIn) = T::transition(S::Qa);

    // -----------------------------------------------------------------
    // rJoinMt! and rMt! — mrp.c:830-848 (shared case)
    // -----------------------------------------------------------------
    constexpr std::array<E, 2> rjm_events{E::RJoinMt, E::RMt};
    for (auto const ev : rjm_events) {
        t.at(S::Qa, ev) = T::transition(S::Aa);
        t.at(S::Qo, ev) = T::transition(S::Ao);
        t.at(S::Qp, ev) = T::transition(S::Ap);
        t.at(S::Lo, ev) = T::transition(S::Vo);
    }

    // -----------------------------------------------------------------
    // rLv! and rLA! and ReDeclare! — mrp.c:850-875 (shared case)
    // -----------------------------------------------------------------
    constexpr std::array<E, 3> rlv_events{E::RLeave, E::RLeaveAll, E::Redeclare};
    for (auto const ev : rlv_events) {
        t.at(S::Vo, ev) = T::transition(S::Lo);
        t.at(S::An, ev) = T::transition(S::Vn);
        t.at(S::Qa, ev) = T::transition(S::Vp);
        t.at(S::Aa, ev) = T::transition(S::Vp);
        t.at(S::Ao, ev) = T::transition(S::Lo);
        t.at(S::Qo, ev) = T::transition(S::Lo);
        t.at(S::Ap, ev) = T::transition(S::Vp);
        t.at(S::Qp, ev) = T::transition(S::Vp);
    }

    // -----------------------------------------------------------------
    // periodic! — mrp.c:876-887
    // -----------------------------------------------------------------
    t.at(S::Qa, E::Periodic) = T::transition(S::Aa);
    t.at(S::Qp, E::Periodic) = T::transition(S::Ap);

    return t;
}();

using Machine = StateMachine<Def, table>;

}  // namespace statusbar::srp::mrp::applicant_sm
