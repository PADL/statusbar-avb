// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/srp/srp_mrp_participant.hpp"

namespace statusbar::srp::mrp {

PortState::PortState(uint64_t rng_seed, TimerConfig const& timers)
    : timers_{rng_seed, timers}
{}

void PortState::start(TimePoint now)
{
    // The first handle_event() call processes the UCT chain from
    // Start, which fires a_init on both FSMs. That sets
    // timer_restart=true, which we forward to the TimerScheduler.
    dispatch_leaveall(leaveall_sm::Def::Event::UCT, now);
    dispatch_periodic(periodic_sm::Def::Event::UCT, now);
}

void PortState::stop() noexcept
{
    timers_.stop_join();
    timers_.stop_leave();
    timers_.stop_leaveall();
    timers_.stop_periodic();
}

auto PortState::dispatch_leaveall(leaveall_sm::Def::Event event, TimePoint now) -> leaveall_sm::Context const&
{
    lva_ctx_.clear_outputs();
    lva_sm_.handle_event(lva_ctx_, event, now);
    if (lva_ctx_.timer_restart) {
        timers_.arm_leaveall(now);
    }
    return lva_ctx_;
}

auto PortState::dispatch_periodic(periodic_sm::Def::Event event, TimePoint now) -> periodic_sm::Context const&
{
    periodic_ctx_.clear_outputs();
    periodic_sm_.handle_event(periodic_ctx_, event, now);
    if (periodic_ctx_.timer_restart) {
        timers_.arm_periodic(now);
    }
    return periodic_ctx_;
}

}  // namespace statusbar::srp::mrp
