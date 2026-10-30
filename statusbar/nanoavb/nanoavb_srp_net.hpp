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
// SRP Network Handlers
//

/// The MVRP wire: its ethertype and group address (IEEE 802.1Q-2022 Clause 11).
struct MvrpWire
{
    static constexpr uint16_t ETHERTYPE = ETHERTYPE_MVRP;
    static constexpr ieee::Eui48 MULTICAST_MAC = MVRP_MULTICAST_MAC;
};

/// The MSRP wire: its ethertype and group address (IEEE 802.1Q-2022 Clause 35).
struct MsrpWire
{
    static constexpr uint16_t ETHERTYPE = ETHERTYPE_MSRP;
    static constexpr ieee::Eui48 MULTICAST_MAC = MSRP_MULTICAST_MAC;
};

/// One MRP application's socket on an interface as a reactor Pollable: each PDU to
/// @p Handler (receive_packet(span, TimePoint)), and its 10 ms tick (tick(TimePoint)).
/// Reactor thread only; never blocks or throws.
template <class Handler, class Wire>
class SrpNetHandlerT : public net::Pollable
{
  public:
    static constexpr int PROTOCOL_TICK_PERIOD_MS = 10;

    /// Opens the socket on @p interface_name (valid() says whether it did); @p handler outlives this.
    SrpNetHandlerT(std::string_view interface_name, Handler& handler)
        : handler_{handler}
    {
        (void)context_.open(interface_name, Wire::ETHERTYPE, &Wire::MULTICAST_MAC);
    }

    /// Whether the socket opened.
    [[nodiscard]] auto valid() const noexcept -> bool { return context_.valid(); }

    /// Send a frame to @p dest_mac.
    [[nodiscard]] auto send(ieee::Eui48 const& dest_mac, std::span<uint8_t const> payload) -> StatusValue<ssize_t>
    {
        return context_.send(&dest_mac, payload);
    }

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
            handler_.receive_packet(
                {payload_buf_.data(), static_cast<size_t>(*result)}, sm::TimePoint{std::chrono::nanoseconds{now_ns}});
        }
    }

    void tick(int64_t now_ns) override { handler_.tick(sm::TimePoint{std::chrono::nanoseconds{now_ns}}); }

    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    net::RawnetContext context_{};
    Handler& handler_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::array<uint8_t, 2048> payload_buf_{};
};

template <class Handler = MvrpHandler>
using MvrpNetHandlerT = SrpNetHandlerT<Handler, MvrpWire>;
using MvrpNetHandler = MvrpNetHandlerT<>;

template <class Handler = MsrpHandler<>>
using MsrpNetHandlerT = SrpNetHandlerT<Handler, MsrpWire>;
using MsrpNetHandler = MsrpNetHandlerT<>;

}  // namespace statusbar::nanoavb
