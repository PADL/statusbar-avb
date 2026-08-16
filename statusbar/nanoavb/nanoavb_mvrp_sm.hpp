#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace statusbar::nanoavb::mvrp_sm {

using namespace statusbar::sm;

struct Context;

struct Callbacks
{
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> init;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> send_join;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> send_leave;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> mark_joined;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> mark_left;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> mark_error;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> reset;
};

struct Context
{
    Callbacks callbacks;
    uint16_t vid{0};
    uint32_t refcount{0};
    bool joined{false};
};

struct Def
{
    using Context = mvrp_sm::Context;
    enum class State : uint8_t
    {
        Start = 0,
        NotJoined,
        Joining,
        Joined,
        Leaving,
        Error,
        Count
    };
    enum class Event : uint8_t
    {
        UCT = 0,
        Acquire,
        ReleaseLast,
        JoinOk,
        JoinFail,
        LeaveOk,
        LeaveFail,
        Reset,
        Count
    };
};

void init(Context& ctx, TimePoint time);

inline void send_join(Context& ctx, TimePoint time)
{
    ctx.callbacks.send_join(ctx, time);
}

inline void send_leave(Context& ctx, TimePoint time)
{
    ctx.callbacks.send_leave(ctx, time);
}

void mark_joined(Context& ctx, TimePoint time);
void mark_left(Context& ctx, TimePoint time);
void mark_error(Context& ctx, TimePoint time);
void reset(Context& ctx, TimePoint time);

inline constexpr auto table = []() -> TransitionTable<Def> {
    using S = Def::State;
    using E = Def::Event;
    using T = Transitions<Def>;
    TransitionTable<Def> t{};

    t.at(S::Start, E::UCT) = T::action<init>(S::NotJoined);

    t.at(S::NotJoined, E::Acquire) = T::action<send_join>(S::Joining);

    t.at(S::Joining, E::JoinOk) = T::action<mark_joined>(S::Joined);
    t.at(S::Joining, E::JoinFail) = T::transition(S::Error);

    t.at(S::Joined, E::ReleaseLast) = T::action<send_leave>(S::Leaving);

    t.at(S::Leaving, E::LeaveOk) = T::action<mark_left>(S::NotJoined);
    t.at(S::Leaving, E::LeaveFail) = T::transition(S::Error);

    t.at(S::Error, E::Reset) = T::action<reset>(S::NotJoined);

    // Entry hook: any arrival in Error records the failure.
    t.on_entry(S::Error) = T::hook<mark_error>();

    return t;
}();

using Machine = StateMachine<Def, table>;

}  // namespace statusbar::nanoavb::mvrp_sm
