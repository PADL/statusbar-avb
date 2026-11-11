#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// Per-attribute state record for the MRP protocol engine.
//
// Each attribute tracked by a participant carries its own Applicant
// and Registrar state machines plus the protocol-specific FirstValue
// payload. Protocol specializations (MSRP, MVRP) choose the FirstValue
// type parameter.
//

#include "statusbar/srp/srp_mrp_applicant_sm.hpp"
#include "statusbar/srp/srp_mrp_registrar_sm.hpp"

#include <cstdint>
#include <optional>

namespace statusbar::srp::mrp {

/// How an attribute came to exist in the participant's database.
/// See mrp.c:148-149 (MSRP_OPERATION_REGISTER / MSRP_OPERATION_DECLARE).
enum class Operation : uint8_t
{
    Register = 0,  ///< learned from a peer PDU
    Declare = 1,   ///< declared by a local client
};

/// Combined per-attribute state: Applicant FSM instance + Registrar
/// FSM instance + the protocol-specific FirstValue payload.
///
/// The FirstValue template parameter is the wire-format fixed-size
/// struct from statusbar/srp/srp_msrp.hpp or mvrp.hpp (e.g.
/// TalkerAdvertiseFirstValue, ListenerFirstValue, DomainFirstValue,
/// VlanIdentifierFirstValue).
template <typename FirstValue>
struct AttributeRecord
{
    FirstValue first_value{};
    Operation operation{Operation::Register};

    // Per-attribute state machine instances. The Contexts hold only
    // transition side-effect outputs; the state_ itself lives inside
    // the Machine.
    applicant_sm::Context applicant_ctx{};
    applicant_sm::Machine applicant_sm{};
    registrar_sm::Context registrar_ctx{};
    registrar_sm::Machine registrar_sm{};

    /// This Registrar's leavetimer (IEEE 802.1Q-2022 10.7.4.2: one per
    /// Registrar): the instant it expires while the Registrar is in Lv,
    /// nothing otherwise. Kept by note_leave_timer(); the port's
    /// TimerScheduler holds only the earliest of them.
    std::optional<sm::TimePoint> leave_deadline{};

    /// A replacement FirstValue waiting for the Lv of the current one to
    /// reach the wire. IEEE 802.1Q-2022 Clause 10.7 makes a changed value a
    /// different attribute, so a participant that changes a declared value
    /// leaves the old one first and declares the new one New afterwards (MSRP
    /// Talker Advertise: 35.2.2.8, where only AccumulatedLatency moves in
    /// place). Set by declare_*, consumed by the participant once the Lv has
    /// been encoded; nullopt the rest of the time.
    std::optional<FirstValue> pending_first_value{};

    /// Set by the participant when the attribute has fully decayed
    /// (Applicant in Vo and Registrar in Mt) and can be removed from
    /// the database on the next reclaim sweep. See mrp.c:3220-3242
    /// (msrp_reclaim / msrp_conditional_reclaim).
    bool should_reclaim{false};

    /// Convenience: is the Registrar currently In? Used by the
    /// transmit driver to pick between TxRegistrarIn and TxRegistrarMt
    /// variants of the tx! event.
    [[nodiscard]] auto registrar_is_in() const noexcept -> bool
    {
        return registrar_sm.current_state() == registrar_sm::Def::State::In;
    }

    /// Convenience: has this attribute fully decayed?
    [[nodiscard]] auto is_dead() const noexcept -> bool
    {
        return applicant_sm.current_state() == applicant_sm::Def::State::Vo &&
            registrar_sm.current_state() == registrar_sm::Def::State::Mt;
    }
};

}  // namespace statusbar::srp::mrp
