#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace statusbar::nanoavb::talker_engine_sm {

using namespace statusbar::sm;

struct Context;

struct Callbacks
{
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> init;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> start_audio_source;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> arm_stream;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> start_tx;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> stop_tx;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> mute_tx;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> unmute_tx;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> stop_all;
};

struct Context
{
    Callbacks callbacks{};
    bool audio_ready{false};
    bool send_allowed{false};  // computed by your supervisor / gate logic
};

struct Def
{
    using Context = talker_engine_sm::Context;
    enum class State : uint8_t
    {
        Start = 0,
        Off,
        Priming,
        Armed,
        Running,
        Muted,
        Count
    };
    enum class Event : uint8_t
    {
        UCT = 0,
        AudioReady,
        Primed,
        GateGo,
        GateStop,
        Underrun,
        Recovered,
        Fatal,
        Count
    };
};

inline void init(Context& ctx, TimePoint time)
{
    ctx.callbacks.init(ctx, time);
}

inline void start_audio_source(Context& ctx, TimePoint time)
{
    ctx.callbacks.start_audio_source(ctx, time);
}

inline void arm_stream(Context& ctx, TimePoint time)
{
    ctx.callbacks.arm_stream(ctx, time);
}

inline void start_tx(Context& ctx, TimePoint time)
{
    ctx.callbacks.start_tx(ctx, time);
}

inline void stop_tx(Context& ctx, TimePoint time)
{
    ctx.callbacks.stop_tx(ctx, time);
}

inline void mute_tx(Context& ctx, TimePoint time)
{
    ctx.callbacks.mute_tx(ctx, time);
}

inline void unmute_tx(Context& ctx, TimePoint time)
{
    ctx.callbacks.unmute_tx(ctx, time);
}

inline void stop_all(Context& ctx, TimePoint time)
{
    // Runs as the Off entry hook, including on the initial UCT edge before a
    // driver may have wired callbacks - nothing wired means nothing to stop.
    if (ctx.callbacks.stop_all) {
        ctx.callbacks.stop_all(ctx, time);
    }
}

inline constexpr auto table = []() -> TransitionTable<Def> {
    using S = Def::State;
    using E = Def::Event;
    using T = Transitions<Def>;
    TransitionTable<Def> t{};

    t.at(S::Start, E::UCT) = T::action<init>(S::Off);

    t.at(S::Off, E::AudioReady) = T::action<start_audio_source>(S::Priming);
    t.at(S::Priming, E::Primed) = T::action<arm_stream>(S::Armed);

    t.at(S::Armed, E::GateGo) = T::action<start_tx>(S::Running);

    t.at(S::Running, E::GateStop) = T::action<stop_tx>(S::Armed);
    t.at(S::Running, E::Underrun) = T::action<mute_tx>(S::Muted);

    t.at(S::Muted, E::Recovered) = T::action<unmute_tx>(S::Running);
    t.at(S::Muted, E::GateStop) = T::action<stop_tx>(S::Armed);

    // Fatal from any operational state: keep explicit ones (your table doesn't support wildcards)
    t.at(S::Off, E::Fatal) = T::transition(S::Off);
    t.at(S::Priming, E::Fatal) = T::transition(S::Off);
    t.at(S::Armed, E::Fatal) = T::transition(S::Off);
    t.at(S::Running, E::Fatal) = T::transition(S::Off);
    t.at(S::Muted, E::Fatal) = T::transition(S::Off);

    // Entry hook: every arrival in Off (Fatal from anywhere, including the
    // Off self-loop) stops everything. Also fires after init on the initial
    // UCT edge (a no-op on a fresh context).
    t.on_entry(S::Off) = T::hook<stop_all>();

    return t;
}();

using Machine = StateMachine<Def, table>;

}  // namespace statusbar::nanoavb::talker_engine_sm
