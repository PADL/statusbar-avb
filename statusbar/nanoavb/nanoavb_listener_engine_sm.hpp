#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace statusbar::nanoavb::listener_engine_sm {

using namespace statusbar::sm;

struct Context;

struct Callbacks
{
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> init;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> enable_rx_filter;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> start_sync;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> start_audio_sink;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> stop_all;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> resync;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> mute_out;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> unmute_out;
};

struct Context
{
    Callbacks callbacks{};
    bool play_allowed{false};  // computed gate
};

struct Def
{
    using Context = listener_engine_sm::Context;
    enum class State : uint8_t
    {
        Start = 0,
        Off,
        Listening,
        Syncing,
        Playing,
        Muted,
        Count
    };
    enum class Event : uint8_t
    {
        UCT = 0,
        GateListen,
        FirstPacket,
        Synced,
        GateStop,
        PacketGap,
        Underrun,
        Recovered,
        Count
    };
};

inline void init(Context& ctx, TimePoint time)
{
    ctx.callbacks.init(ctx, time);
}

inline void enable_rx_filter(Context& ctx, TimePoint time)
{
    ctx.callbacks.enable_rx_filter(ctx, time);
}

inline void start_sync(Context& ctx, TimePoint time)
{
    ctx.callbacks.start_sync(ctx, time);
}

inline void start_audio_sink(Context& ctx, TimePoint time)
{
    ctx.callbacks.start_audio_sink(ctx, time);
}

inline void stop_all(Context& ctx, TimePoint time)
{
    // Runs as the Off entry hook, including on the initial UCT edge before a
    // driver may have wired callbacks - nothing wired means nothing to stop.
    if (ctx.callbacks.stop_all) {
        ctx.callbacks.stop_all(ctx, time);
    }
}

inline void resync(Context& ctx, TimePoint time)
{
    ctx.callbacks.resync(ctx, time);
}

inline void mute_out(Context& ctx, TimePoint time)
{
    ctx.callbacks.mute_out(ctx, time);
}

inline void unmute_out(Context& ctx, TimePoint time)
{
    ctx.callbacks.unmute_out(ctx, time);
}

inline constexpr auto table = []() -> TransitionTable<Def> {
    using S = Def::State;
    using E = Def::Event;
    using T = Transitions<Def>;
    TransitionTable<Def> t{};

    t.at(S::Start, E::UCT) = T::action<init>(S::Off);

    t.at(S::Off, E::GateListen) = T::action<enable_rx_filter>(S::Listening);

    t.at(S::Listening, E::FirstPacket) = T::action<start_sync>(S::Syncing);
    t.at(S::Listening, E::GateStop) = T::transition(S::Off);

    t.at(S::Syncing, E::Synced) = T::action<start_audio_sink>(S::Playing);
    t.at(S::Syncing, E::GateStop) = T::transition(S::Off);           // Allow stop from Syncing
    t.at(S::Syncing, E::PacketGap) = T::action<resync>(S::Syncing);  // Handle gap during sync

    t.at(S::Playing, E::GateStop) = T::transition(S::Off);
    t.at(S::Playing, E::PacketGap) = T::action<resync>(S::Syncing);
    t.at(S::Playing, E::Underrun) = T::action<mute_out>(S::Muted);

    t.at(S::Muted, E::Recovered) = T::action<unmute_out>(S::Playing);
    t.at(S::Muted, E::GateStop) = T::transition(S::Off);
    t.at(S::Muted, E::PacketGap) = T::action<resync>(S::Syncing);  // Handle gap while muted

    // Entry hook: every arrival in Off (GateStop from any active state)
    // stops everything. Also fires after init on the initial UCT edge
    // (a no-op on a fresh context).
    t.on_entry(S::Off) = T::hook<stop_all>();

    return t;
}();

using Machine = StateMachine<Def, table>;

}  // namespace statusbar::nanoavb::listener_engine_sm
