#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace statusbar::nanoavb::msrp_listener_sm {

using namespace statusbar::sm;

struct Context;

struct Callbacks
{
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> init;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> msrp_listener_ready;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> mark_ready;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> mark_failed;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> msrp_listener_leave;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> mark_idle;
};

struct Context
{
    Callbacks callbacks;
    bool reserved{false};
    bool failed{false};
};

struct Def
{
    using Context = msrp_listener_sm::Context;
    enum class State : uint8_t
    {
        Start = 0,
        Idle,
        Joining,
        Ready,
        Failed,
        Leaving,
        Count
    };
    enum class Event : uint8_t
    {
        UCT = 0,
        StartJoin,
        StopJoin,
        Ready,
        Failed,
        Lost,
        Left,
        Count
    };
};

void init(Context& ctx, TimePoint time);

inline void msrp_listener_ready(Context& ctx, TimePoint time)
{
    ctx.callbacks.msrp_listener_ready(ctx, time);
}

void mark_ready(Context& ctx, TimePoint time);
void mark_failed(Context& ctx, TimePoint time);
void msrp_listener_leave(Context& ctx, TimePoint time);
void mark_idle(Context& ctx, TimePoint time);

inline constexpr auto table = []() -> TransitionTable<Def> {
    using S = Def::State;
    using E = Def::Event;
    using T = Transitions<Def>;
    TransitionTable<Def> t{};

    t.at(S::Start, E::UCT) = T::action<init>(S::Idle);

    t.at(S::Idle, E::StartJoin) = T::transition(S::Joining);

    t.at(S::Joining, E::Ready) = T::action<mark_ready>(S::Ready);
    t.at(S::Joining, E::Failed) = T::action<mark_failed>(S::Failed);

    t.at(S::Ready, E::Lost) = T::transition(S::Joining);

    t.at(S::Ready, E::StopJoin) = T::transition(S::Leaving);
    t.at(S::Failed, E::StopJoin) = T::transition(S::Leaving);

    t.at(S::Leaving, E::Left) = T::action<mark_idle>(S::Idle);

    // Entry hooks: Joining/Leaving always issue their MSRP declaration.
    t.on_entry(S::Joining) = T::hook<msrp_listener_ready>();
    t.on_entry(S::Leaving) = T::hook<msrp_listener_leave>();

    return t;
}();

using Machine = StateMachine<Def, table>;

}  // namespace statusbar::nanoavb::msrp_listener_sm
