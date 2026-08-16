#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace statusbar::nanoavb::gptp_sm {

using namespace statusbar::sm;

struct Context;

struct Callbacks
{
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> init{};
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> start_servo{};
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> report_locked{};
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> report_unlocked{};
};

struct Context
{
    Callbacks callbacks{};
    bool as_capable{false};
    bool time_locked{false};
};

struct Def
{
    using Context = gptp_sm::Context;
    enum class State : uint8_t
    {
        Start = 0,
        Unlocked,
        Acquiring,
        Locked,
        Count
    };
    enum class Event : uint8_t
    {
        UCT = 0,
        AsCapableUp,
        AsCapableDown,
        LockedStable,
        LockLost,
        Count
    };
};

inline void init(Context& ctx, TimePoint time)
{
    ctx.callbacks.init(ctx, time);
}

void start_servo(Context& ctx, TimePoint time);
void report_locked(Context& ctx, TimePoint time);
void report_unlocked(Context& ctx, TimePoint time);

inline constexpr auto table = []() -> TransitionTable<Def> {
    using S = Def::State;
    using E = Def::Event;
    using T = Transitions<Def>;
    TransitionTable<Def> t{};

    t.at(S::Start, E::UCT) = T::action<init>(S::Unlocked);

    t.at(S::Unlocked, E::AsCapableUp) = T::transition(S::Acquiring);
    t.at(S::Acquiring, E::LockedStable) = T::action<report_locked>(S::Locked);
    t.at(S::Acquiring, E::AsCapableDown) = T::action<report_unlocked>(S::Unlocked);

    t.at(S::Locked, E::AsCapableDown) = T::action<report_unlocked>(S::Unlocked);
    t.at(S::Locked, E::LockLost) = T::transition(S::Acquiring);

    // Entry hook: entering Acquiring always (re)starts the servo.
    t.on_entry(S::Acquiring) = T::hook<start_servo>();

    return t;
}();

using Machine = StateMachine<Def, table>;

}  // namespace statusbar::nanoavb::gptp_sm
