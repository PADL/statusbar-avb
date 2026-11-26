// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#pragma once

/// The clock of the reactor that drives an entity.
///
/// An entity's control plane is told its time: the reactor reads its clock
/// once per cycle and hands that reading to every handler. The talker's
/// transmit gate is on another clock, CLOCK_MONOTONIC_RAW, the one a media
/// wake is told. So that nothing under the loop has to read that clock, the
/// loop reads the two together, once per cycle, and tells the entity the pair
/// (tell_cycle_time) before any handler of the cycle runs.

#include "statusbar/net/net_ethernet_port.hpp"
#include "statusbar/net/net_message_reactor.hpp"

#include <cstdint>

namespace statusbar::avb_entity {

/// The clock to give the net::MessageReactor that drives @p entity: the
/// reactor's own clock (net::monotonic_ns), with an entity that asks for it
/// (tell_cycle_time(now_ns, raw_now)) told that reading and
/// CLOCK_MONOTONIC_RAW read at the same moment. @p entity outlives the reactor.
template <typename Entity>
[[nodiscard]] auto entity_reactor_clock(Entity& entity) -> net::ClockFn
{
    return [&entity]() -> int64_t {
        int64_t const now_ns = net::monotonic_ns();
        if constexpr (requires { entity.tell_cycle_time(now_ns, net::rx_clock_now()); }) {
            entity.tell_cycle_time(now_ns, net::rx_clock_now());
        }
        return now_ns;
    };
}

}  // namespace statusbar::avb_entity
