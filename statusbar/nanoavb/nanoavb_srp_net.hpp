#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB SRP network handlers: the MVRP and MSRP sockets of one interface as reactor Pollables,
/// feeding an MvrpHandler and an MsrpHandler (IEEE 802.1Q-2022 Clauses 11 and 35).

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>

namespace statusbar::nanoavb {

using ieee::protocols::ETHERTYPE_MSRP;
using ieee::protocols::ETHERTYPE_MVRP;
using ieee::protocols::MSRP_MULTICAST_MAC;
using ieee::protocols::MVRP_MULTICAST_MAC;

//
// MVRP Network Handler
//

/// MVRP Network Handler - receives/sends MVRP packets via Pollable interface.
/// Handles periodic ticks for MVRP state machine.
///
/// Threading: All methods (on_ready, tick) are called from
/// the reactor's poll thread. Must not block — long operations should
/// be deferred. Must not throw — errors should be logged and dropped.
/// @tparam Handler receives each PDU (receive_packet(span, TimePoint)) and the 10 ms tick (tick(TimePoint)).
template <class Handler = MvrpHandler>
class MvrpNetHandlerT : public net::Pollable
{
  public:
    static constexpr int PROTOCOL_TICK_PERIOD_MS = 10;

    /// @param interface_name The network interface name
    /// @param handler Reference to the MVRP protocol handler
    MvrpNetHandlerT(std::string_view interface_name, Handler& handler)
        : handler_{handler}
    {
        (void)context_.open(interface_name, ETHERTYPE_MVRP, &MVRP_MULTICAST_MAC);
    }

    /// Check if the handler is valid (socket opened successfully)
    [[nodiscard]] auto valid() const noexcept -> bool { return context_.valid(); }

    /// Send a frame directly
    [[nodiscard]] auto send(ieee::Eui48 const& dest_mac, std::span<uint8_t const> payload) -> StatusValue<ssize_t>
    {
        return context_.send(&dest_mac, payload);
    }

    // -- Pollable interface --

    [[nodiscard]] auto fd() const noexcept -> int override { return context_.fd(); }

    void on_ready(int64_t now_ns) override
    {
        ieee::Eui48 src_mac{};
        ieee::Eui48 dest_mac{};
        while (true) {
            auto result = context_.recv(&src_mac, &dest_mac, payload_buf_);
            if (!result || *result <= 0) {
                break;
            }
            auto const len = static_cast<size_t>(*result);
            auto const sm_now = sm::TimePoint{std::chrono::nanoseconds{now_ns}};
            handler_.receive_packet({payload_buf_.data(), len}, sm_now);
        }
    }

    void tick(int64_t now_ns) override
    {
        auto const sm_now = sm::TimePoint{std::chrono::nanoseconds{now_ns}};
        handler_.tick(sm_now);
    }

    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    net::RawnetContext context_{};
    Handler& handler_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::array<uint8_t, 2048> payload_buf_{};
};

using MvrpNetHandler = MvrpNetHandlerT<>;

//
// MSRP Network Handler
//

/// MSRP Network Handler - receives/sends MSRP packets via Pollable interface.
/// Handles periodic ticks for MSRP state machine.
///
/// Threading: All methods (on_ready, tick) are called from
/// the reactor's poll thread. Must not block — long operations should
/// be deferred. Must not throw — errors should be logged and dropped.
/// @tparam Handler receives each PDU (receive_packet(span, TimePoint)) and the 10 ms tick (tick(TimePoint)).
template <class Handler = MsrpHandler<>>
class MsrpNetHandlerT : public net::Pollable
{
  public:
    static constexpr int PROTOCOL_TICK_PERIOD_MS = 10;

    /// @param interface_name The network interface name
    /// @param handler Reference to the MSRP protocol handler
    MsrpNetHandlerT(std::string_view interface_name, Handler& handler)
        : handler_{handler}
    {
        (void)context_.open(interface_name, ETHERTYPE_MSRP, &MSRP_MULTICAST_MAC);
    }

    /// Check if the handler is valid (socket opened successfully)
    [[nodiscard]] auto valid() const noexcept -> bool { return context_.valid(); }

    /// Send a frame directly
    [[nodiscard]] auto send(ieee::Eui48 const& dest_mac, std::span<uint8_t const> payload) -> StatusValue<ssize_t>
    {
        return context_.send(&dest_mac, payload);
    }

    // -- Pollable interface --

    [[nodiscard]] auto fd() const noexcept -> int override { return context_.fd(); }

    void on_ready(int64_t now_ns) override
    {
        ieee::Eui48 src_mac{};
        ieee::Eui48 dest_mac{};
        while (true) {
            auto result = context_.recv(&src_mac, &dest_mac, payload_buf_);
            if (!result || *result <= 0) {
                break;
            }
            auto const len = static_cast<size_t>(*result);
            auto const sm_now = sm::TimePoint{std::chrono::nanoseconds{now_ns}};
            handler_.receive_packet({payload_buf_.data(), len}, sm_now);
        }
    }

    void tick(int64_t now_ns) override
    {
        auto const sm_now = sm::TimePoint{std::chrono::nanoseconds{now_ns}};
        handler_.tick(sm_now);
    }

    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    net::RawnetContext context_{};
    Handler& handler_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::array<uint8_t, 2048> payload_buf_{};
};

using MsrpNetHandler = MsrpNetHandlerT<>;

}  // namespace statusbar::nanoavb
