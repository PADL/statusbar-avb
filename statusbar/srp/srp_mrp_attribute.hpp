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

    /// Is the peer's declaration registered: Registrar In, or Leaving (a
    /// LeaveAll or a Leave not yet timed out)?
    [[nodiscard]] auto registrar_is_registered() const noexcept -> bool
    {
        auto const s = registrar_sm.current_state();
        return s == registrar_sm::Def::State::In || s == registrar_sm::Def::State::Lv;
    }

    /// Convenience: has this attribute fully decayed?
    [[nodiscard]] auto is_dead() const noexcept -> bool
    {
        return applicant_sm.current_state() == applicant_sm::Def::State::Vo &&
            registrar_sm.current_state() == registrar_sm::Def::State::Mt;
    }
};

}  // namespace statusbar::srp::mrp
