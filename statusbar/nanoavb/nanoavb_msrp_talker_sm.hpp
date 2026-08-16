#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace statusbar::nanoavb::msrp_talker_sm {

using namespace statusbar::sm;

struct Context;

struct Callbacks
{
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> init;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> msrp_talker_advertise;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> mark_ready;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> mark_failed;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> msrp_talker_withdraw;
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
    using Context = msrp_talker_sm::Context;
    enum class State : uint8_t
    {
        Start = 0,
        Idle,
        Advertising,
        Ready,
        Failed,
        Withdrawing,
        Count
    };
    enum class Event : uint8_t
    {
        UCT = 0,
        StartAdvertise,
        StopAdvertise,
        Ready,
        Failed,
        Lost,
        Withdrawn,
        Count
    };
};

void init(Context& ctx, TimePoint time);

inline void msrp_talker_advertise(Context& ctx, TimePoint time)
{
    ctx.callbacks.msrp_talker_advertise(ctx, time);
}

void mark_ready(Context& ctx, TimePoint time);
void mark_failed(Context& ctx, TimePoint time);
void msrp_talker_withdraw(Context& ctx, TimePoint time);
void mark_idle(Context& ctx, TimePoint time);

inline constexpr auto table = []() -> TransitionTable<Def> {
    using S = Def::State;
    using E = Def::Event;
    using T = Transitions<Def>;
    TransitionTable<Def> t{};

    t.at(S::Start, E::UCT) = T::action<init>(S::Idle);

    t.at(S::Idle, E::StartAdvertise) = T::transition(S::Advertising);

    t.at(S::Advertising, E::Ready) = T::action<mark_ready>(S::Ready);
    t.at(S::Advertising, E::Failed) = T::action<mark_failed>(S::Failed);

    t.at(S::Ready, E::Lost) = T::transition(S::Advertising);

    t.at(S::Ready, E::StopAdvertise) = T::transition(S::Withdrawing);
    t.at(S::Failed, E::StopAdvertise) = T::transition(S::Withdrawing);

    t.at(S::Withdrawing, E::Withdrawn) = T::action<mark_idle>(S::Idle);

    // Entry hooks: Advertising/Withdrawing always issue their MSRP declaration.
    t.on_entry(S::Advertising) = T::hook<msrp_talker_advertise>();
    t.on_entry(S::Withdrawing) = T::hook<msrp_talker_withdraw>();

    return t;
}();

using Machine = StateMachine<Def, table>;

}  // namespace statusbar::nanoavb::msrp_talker_sm
