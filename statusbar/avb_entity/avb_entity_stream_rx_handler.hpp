#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_stream_rx_handler.hpp
/// @brief StreamRxHandler — the shared reactor adapter that watches the entity's
/// stream port for receive readiness, joins it to a stream's multicast group(s), and
/// delegates each batch drain to a caller-supplied callback.
///
/// Replaces the three near-identical per-entity copies (AvbEntityAudioIO / AvbEntityAm824IO
/// / AvbEntityStereoIO). Each differed only in how many groups it joined (AudioIO joins
/// AM824 + AAF; the others one) and which drain it called (ListenerStreams::drain_rx vs the
/// entity's own reactor drain) — both are now parameters: the groups span and a DrainFn.
///
/// The reactor's monotonic now is intentionally ignored: the RX tally + the AM824/AAF
/// deserialize need a gPTP "now", which the drain callback sources itself (the media-timer
/// wake for the reactor path, or the RX timer's wake in RT-timer mode). The port it
/// drains belongs to the entity, which joins and leaves multicast groups on it directly.

#include "statusbar/avb_entity/avb_entity_stream_port.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sg14/inplace_function.h"

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

namespace statusbar::avb_entity {

class StreamRxHandler : public net::Pollable
{
  public:
    /// Called on each reactor wake to drain the socket; captures the owner + the gPTP
    /// source. One pointer fits the small inplace capacity (RT-safe, no heap).
    using DrainFn = statusbar::sg14::inplace_function<void(), 16>;

    /// Join @p port to each of @p groups and drain it via @p drain on readiness.
    StreamRxHandler(StreamPort& port, std::span<ieee::Eui48 const> groups, DrainFn drain)
        : port_{port}
        , drain_{std::move(drain)}
    {
        StreamPort::Guard const guard{port_};
        for (auto const& group : groups) {
            if (group.is_set()) {
                (void)port_.port().join_multicast(group);
            }
        }
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return port_.valid(); }

    [[nodiscard]] auto fd() const noexcept -> int override { return port_.fd(); }
    void on_ready(int64_t /*reactor_now_ns*/) override { drain_(); }
    void tick(int64_t /*now_ns*/) override {}
    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    StreamPort& port_;
    DrainFn drain_;
};

}  // namespace statusbar::avb_entity
