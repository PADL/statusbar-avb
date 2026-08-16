#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Client-side transaction state machine. The Pollable layer drives this
/// FSM with concrete events; the actions update timing parameters and
/// flag what side-effect the Pollable should perform on the next tick.

#include "statusbar/sm/sm_core.hpp"
#include "statusbar/stun/stun_constants.hpp"

#include <cstdint>

namespace statusbar::stun {

struct ClientContext
{
    int64_t now_ns{0};

    /// Wall-clock time (in ns) at which the current outstanding request
    /// was sent. Used by the Pollable to compute RTO.
    int64_t request_sent_ns{0};

    /// Number of retransmissions of the current request so far.
    int retransmits{0};

    /// Refresh interval the server told us to use (ms).
    uint32_t refresh_interval_ms{default_refresh_interval_ms};

    /// One-shot flags read by the Pollable each tick. The action
    /// functions set them; the Pollable acts and clears them.
    bool emit_send{false};
    bool emit_failure{false};
    bool reached_pairing{false};
};

struct ClientDef
{
    using Context = ClientContext;

    enum class State : uint8_t
    {
        Idle = 0,
        Registering,  ///< Sent a request, awaiting any response or RTO
        Waiting,      ///< Got "waiting" response, holding for refresh tick
        Paired,       ///< Got "paired" response, terminal-success (Pollable may keep refreshing)
        Failed,       ///< Permanent failure, give up
        Count,
    };

    enum class Event : uint8_t
    {
        Start = 0,
        ResponseWaiting,
        ResponsePaired,
        ResponseError,
        ResponseSessionExpired,
        Rto,
        RetryBudgetExhausted,
        RefreshTick,
        Count,
    };
};

namespace client_sm_actions {

inline void mark_send(ClientDef::Context& ctx, statusbar::sm::TimePoint /*tp*/)
{
    ctx.emit_send = true;
    ctx.retransmits = 0;
}

inline void mark_retransmit(ClientDef::Context& ctx, statusbar::sm::TimePoint /*tp*/)
{
    ctx.emit_send = true;
    ctx.retransmits += 1;
}

inline void enter_waiting(ClientDef::Context& ctx, statusbar::sm::TimePoint /*tp*/)
{
    ctx.retransmits = 0;
}

inline void enter_paired(ClientDef::Context& ctx, statusbar::sm::TimePoint /*tp*/)
{
    ctx.retransmits = 0;
    ctx.reached_pairing = true;
}

inline void enter_failed(ClientDef::Context& ctx, statusbar::sm::TimePoint /*tp*/)
{
    ctx.emit_failure = true;
}

}  // namespace client_sm_actions

inline constexpr auto client_table = [] {
    using State = ClientDef::State;
    using Event = ClientDef::Event;
    using T = statusbar::sm::Transitions<ClientDef>;
    using namespace client_sm_actions;

    statusbar::sm::TransitionTable<ClientDef> t{};

    // Idle: Start sends initial request and moves to Registering.
    t.at(State::Idle, Event::Start) = T::action<mark_send>(State::Registering);

    // Registering: any response moves us forward.
    t.at(State::Registering, Event::ResponseWaiting) = T::action<enter_waiting>(State::Waiting);
    t.at(State::Registering, Event::ResponsePaired) = T::transition(State::Paired);
    t.at(State::Registering, Event::ResponseError) = T::transition(State::Failed);
    t.at(State::Registering, Event::ResponseSessionExpired) = T::action<mark_send>(State::Registering);
    t.at(State::Registering, Event::Rto) = T::action<mark_retransmit>(State::Registering);
    t.at(State::Registering, Event::RetryBudgetExhausted) = T::transition(State::Failed);

    // Waiting: a refresh tick causes us to re-send.
    t.at(State::Waiting, Event::RefreshTick) = T::action<mark_send>(State::Registering);
    // (We can also receive an unsolicited paired notification if the
    // server happens to push one out as a response to a refresh that
    // crossed in flight; treated as paired arrival.)
    t.at(State::Waiting, Event::ResponsePaired) = T::transition(State::Paired);
    t.at(State::Waiting, Event::ResponseError) = T::transition(State::Failed);

    // Paired: the Pollable may keep refreshing as a NAT keepalive. A
    // refresh tick re-sends; a "paired" response is idempotent.
    t.at(State::Paired, Event::RefreshTick) = T::action<mark_send>(State::Registering);
    t.at(State::Paired, Event::ResponsePaired) = T::transition(State::Paired);

    // Entry hooks: arriving in Paired or Failed always runs the same
    // bookkeeping, wherever the transition came from (self-loops included).
    t.on_entry(State::Paired) = T::hook<enter_paired>();
    t.on_entry(State::Failed) = T::hook<enter_failed>();

    return t;
}();

using ClientStateMachine = statusbar::sm::StateMachine<ClientDef, client_table>;

}  // namespace statusbar::stun
