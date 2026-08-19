#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace statusbar::nanoavb::supervisor_sm {

using namespace statusbar::sm;

struct Context;

struct Callbacks
{
    statusbar::sg14::inplace_function<void(Context&, TimePoint), 64> init_iface{};
    statusbar::sg14::inplace_function<void(Context&, TimePoint), 64> start_protocols{};
    statusbar::sg14::inplace_function<void(Context&, TimePoint), 64> enter_ready{};
    statusbar::sg14::inplace_function<void(Context&, TimePoint), 64> degrade_stop_streams{};
    statusbar::sg14::inplace_function<void(Context&, TimePoint), 64> stop_all{};
    statusbar::sg14::inplace_function<void(Context&, TimePoint), 64> timeout_gptp{};  // Called when gPTP lock times out
};

struct Context
{
    Callbacks callbacks{};
    // facts
    bool link_up{false};
    bool gptp_locked{false};

    // diagnostics
};

// Lifecycle model: ADP/AECP/ACMP are independent and run from link-up
// (handled outside this SM). This SM gates the gPTP-dependent work — SRP
// (MVRP+MSRP) and streaming — purely on the gPTP lock state, with no VLAN
// registration gate:
//
//   Down --LinkUp--> Init (gPTP starting) --GptpLocked--> Ready (SRP+streaming)
//   Ready --GptpLost--> Degraded --GptpLocked--> Ready   (dynamic re-lock)
//   any --LinkDown--> Down;  Init --Timeout--> Down       (gPTP-lock watchdog)
struct Def
{
    using Context = supervisor_sm::Context;

    enum class State : uint8_t
    {
        Start = 0,
        Down,
        Init,
        Ready,
        Degraded,
        Count
    };
    enum class Event : uint8_t
    {
        UCT = 0,
        LinkUp,
        LinkDown,
        GptpLocked,
        GptpLost,
        Timeout,  // gPTP-lock watchdog expired in Init
        Count
    };
};

void init_iface(Context& ctx, TimePoint time);
void start_protocols(Context& ctx, TimePoint time);
void enter_ready(Context& ctx, TimePoint time);
void degrade_stop_streams(Context& ctx, TimePoint time);
void stop_all(Context& ctx, TimePoint time);
void timeout_gptp(Context& ctx, TimePoint time);

inline constexpr auto table = []() -> TransitionTable<Def> {
    using S = Def::State;
    using E = Def::Event;
    using T = Transitions<Def>;
    TransitionTable<Def> t{};

    t.at(S::Start, E::UCT) = T::action<init_iface>(S::Down);

    t.at(S::Down, E::LinkUp) = T::action<start_protocols>(S::Init);
    // LinkDown while already Down is ignored (no table row): with the Down
    // entry hook, an explicit self-loop would re-run stop_all and notify
    // the observer on every repeat, where today it is a silent no-op.

    // gPTP lock alone enables SRP + streaming (no VLAN-registration gate).
    t.at(S::Init, E::GptpLocked) = T::transition(S::Ready);
    t.at(S::Init, E::LinkDown) = T::transition(S::Down);
    t.at(S::Init, E::Timeout) = T::action<timeout_gptp>(S::Down);  // gPTP lock timeout

    t.at(S::Ready, E::GptpLost) = T::action<degrade_stop_streams>(S::Degraded);
    t.at(S::Ready, E::LinkDown) = T::transition(S::Down);

    // gPTP came back: restart SRP + streaming.
    t.at(S::Degraded, E::GptpLocked) = T::transition(S::Ready);
    t.at(S::Degraded, E::LinkDown) = T::transition(S::Down);

    // Entry hook: reaching Ready runs the same bring-up from Init or Degraded.
    t.on_entry(S::Ready) = T::hook<enter_ready>();

    // Entry hook: every arrival in Down stops all protocols. Also fires
    // after init_iface on the initial UCT edge (a no-op on a fresh
    // context) and after timeout_gptp (which no longer stops explicitly).
    t.on_entry(S::Down) = T::hook<stop_all>();

    return t;
}();

using Machine = StateMachine<Def, table>;

}  // namespace statusbar::nanoavb::supervisor_sm
