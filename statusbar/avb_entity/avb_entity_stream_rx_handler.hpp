#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_stream_rx_handler.hpp
/// @brief StreamRxHandler — the shared reactor adapter that owns one RX socket joined to
/// a stream's multicast group(s) and delegates each batch drain to a caller-supplied
/// callback.
///
/// Replaces the three near-identical per-entity copies (AvbEntityAudioIO / AvbEntityAm824IO
/// / AvbEntityStereoIO). Each differed only in how many groups it joined (AudioIO joins
/// AM824 + AAF; the others one) and which drain it called (ListenerStreams::drain_rx vs the
/// entity's own reactor drain) — both are now parameters: the groups span and a DrainFn.
///
/// The reactor's monotonic now is intentionally ignored: the RX tally + the AM824/AAF
/// deserialize need a gPTP "now", which the drain callback sources itself (the media-timer
/// wake for the reactor path, or the RX timer's wake in RT-timer mode). The socket is
/// borrowable via socket() for dynamic multicast join/leave on ACMP connect/disconnect;
/// the pointer stays valid across the move into the reactor (heap-allocated Pollable).

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"
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

    /// Open a non-promiscuous raw AVTP socket joined to @p groups[0] (then join any
    /// further groups), draining via @p drain. @p groups must be non-empty.
    StreamRxHandler(std::string_view interface_name, std::span<ieee::Eui48 const> groups, DrainFn drain)
        : drain_{std::move(drain)}
    {
        (void)sock_.open(interface_name, avtp::AVTP_ETHERTYPE, groups.empty() ? nullptr : &groups[0], /*qdisc_bypass=*/false);
        for (size_t i = 1; i < groups.size(); ++i) {
            (void)sock_.join_multicast(groups[i]);
        }
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return sock_.fd() >= 0; }

    /// Non-owning access to the RX socket, for dynamic multicast join/leave on ACMP
    /// connect/disconnect. Stable across the move into the reactor.
    [[nodiscard]] auto socket() noexcept -> net::RawnetContext* { return &sock_; }

    [[nodiscard]] auto fd() const noexcept -> int override { return sock_.fd(); }
    void on_ready(int64_t /*reactor_now_ns*/) override { drain_(); }
    void tick(int64_t /*now_ns*/) override {}
    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    net::RawnetContext sock_{};
    DrainFn drain_;
};

}  // namespace statusbar::avb_entity
