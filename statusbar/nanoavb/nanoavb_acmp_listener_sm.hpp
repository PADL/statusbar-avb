#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace statusbar::nanoavb::acmp_listener_sm {

using namespace statusbar::sm;

struct Context;

struct Callbacks
{
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> init;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> send_connect_tx;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> mark_connected;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> mark_failed;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> send_disconnect_tx;
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> mark_disconnected;
};

struct Context
{
    Callbacks callbacks{};
    bool connected{false};
};

struct Def
{
    using Context = acmp_listener_sm::Context;
    enum class State : uint8_t
    {
        Start = 0,
        Idle,
        Connecting,
        Connected,
        Disconnecting,
        Count
    };
    enum class Event : uint8_t
    {
        UCT = 0,
        ConnectReq,
        ConnectOk,
        ConnectFail,
        DisconnectReq,
        DisconnectOk,
        LinkDown,
        Count
    };
};

void init(Context& ctx, TimePoint time);

inline void send_connect_tx(Context& ctx, TimePoint time)
{
    ctx.callbacks.send_connect_tx(ctx, time);
}

void mark_connected(Context& ctx, TimePoint time);
void mark_failed(Context& ctx, TimePoint time);

inline void send_disconnect_tx(Context& ctx, TimePoint time)
{
    ctx.callbacks.send_disconnect_tx(ctx, time);
}

void mark_disconnected(Context& ctx, TimePoint time);

inline constexpr auto table = []() -> TransitionTable<Def> {
    using S = Def::State;
    using E = Def::Event;
    using T = Transitions<Def>;
    TransitionTable<Def> t{};

    t.at(S::Start, E::UCT) = T::action<init>(S::Idle);

    t.at(S::Idle, E::ConnectReq) = T::action<send_connect_tx>(S::Connecting);
    t.at(S::Connecting, E::ConnectOk) = T::action<mark_connected>(S::Connected);
    t.at(S::Connecting, E::ConnectFail) = T::action<mark_failed>(S::Idle);

    t.at(S::Connected, E::DisconnectReq) = T::action<send_disconnect_tx>(S::Disconnecting);
    t.at(S::Disconnecting, E::DisconnectOk) = T::transition(S::Idle);
    t.at(S::Disconnecting, E::LinkDown) = T::transition(S::Idle);

    t.at(S::Connected, E::LinkDown) = T::action<mark_disconnected>(S::Idle);

    // Exit hook: leaving Disconnecting always records the disconnect,
    // however the exchange ended.
    t.on_exit(S::Disconnecting) = T::hook<mark_disconnected>();

    return t;
}();

using Machine = StateMachine<Def, table>;

}  // namespace statusbar::nanoavb::acmp_listener_sm
