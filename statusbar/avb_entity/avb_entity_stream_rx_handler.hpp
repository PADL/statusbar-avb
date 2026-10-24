#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// StreamRxHandler: the reactor adapter that polls a StreamPort's receive
/// descriptor and drains it through a caller-supplied callback.

#include "statusbar/avb_entity/avb_entity_stream_port.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sg14/inplace_function.h"

#include <cstdint>
#include <utility>

namespace statusbar::avb_entity {

/// Polls @p port's rx_fd() and calls the drain on every wake. The reactor's
/// monotonic now is ignored: the drain sources its own gPTP "now".
class StreamRxHandler : public net::Pollable
{
  public:
    /// Drains the port; one pointer fits (RT-safe, no heap).
    using DrainFn = statusbar::sg14::inplace_function<void(), 16>;

    /// Poll @p port, which must outlive the handler, draining via @p drain.
    StreamRxHandler(StreamPort& port, DrainFn drain) noexcept
        : port_{port}
        , drain_{std::move(drain)}
    {}

    [[nodiscard]] auto fd() const noexcept -> int override { return port_.rx_fd(); }
    void on_ready(int64_t /*reactor_now_ns*/) override { drain_(); }
    void tick(int64_t /*now_ns*/) override {}
    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    StreamPort& port_;
    DrainFn drain_;
};

}  // namespace statusbar::avb_entity
