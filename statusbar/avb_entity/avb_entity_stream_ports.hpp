#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// StreamPorts: the ports an entity's streams go through, chosen by
/// StreamTransport. Transmit uses the AF_PACKET port; receive uses the AF_XDP
/// port under xdp (or auto, when it opens), else the AF_PACKET one.

#include "statusbar/avb_entity/avb_entity_stream_port.hpp"
#include "statusbar/avb_entity/avb_entity_stream_transport.hpp"
#include "statusbar/avb_entity/avb_entity_xdp_stream_port.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/status/status.hpp"

#include <cstdint>
#include <string_view>
#include <system_error>

namespace statusbar::avb_entity {

/// What StreamPorts::open() opens.
struct StreamPortsOptions
{
    std::string_view interface_name;
    /// SO_PRIORITY of the AF_PACKET transmit socket (the stream PCP).
    uint8_t tx_priority{0};
    /// Open a receive side (the entity has stream inputs).
    bool receive{true};
    StreamTransport transport{StreamTransport::packet};
    StreamXdpMode xdp_mode{StreamXdpMode::auto_};
    /// The NIC queue the AF_XDP socket binds.
    uint32_t xdp_queue{0};
    /// Receive is drained on a thread of its own, not the media thread that
    /// transmits; an AF_XDP socket has one consumer, so xdp is refused then.
    bool rx_on_own_thread{false};
};

/// Which port StreamPorts::open() opens for receive.
enum class StreamRxPlan : uint8_t
{
    packet,           ///< AF_PACKET
    xdp,              ///< AF_XDP, or nothing
    xdp_then_packet,  ///< AF_XDP, else AF_PACKET
    refused,          ///< AF_XDP drained on its own thread
};

/// The receive plan for @p transport (auto_ with @p rx_on_own_thread: packet).
[[nodiscard]] constexpr auto stream_rx_plan(StreamTransport const transport, bool const rx_on_own_thread) noexcept -> StreamRxPlan
{
    switch (transport) {
        case StreamTransport::xdp:
            return rx_on_own_thread ? StreamRxPlan::refused : StreamRxPlan::xdp;
        case StreamTransport::auto_:
            return rx_on_own_thread ? StreamRxPlan::packet : StreamRxPlan::xdp_then_packet;
        case StreamTransport::packet:
            break;
    }
    return StreamRxPlan::packet;
}

/// The entity's stream ports. open()/close() on the reactor thread while
/// nothing streams; the tick calls on the media thread.
class StreamPorts
{
  public:
    StreamPorts() = default;
    ~StreamPorts() = default;
    StreamPorts(StreamPorts const&) = delete;
    auto operator=(StreamPorts const&) -> StreamPorts& = delete;
    StreamPorts(StreamPorts&&) = delete;
    auto operator=(StreamPorts&&) -> StreamPorts& = delete;

    /// invalid_argument for a receive @p options would put on an AF_XDP socket
    /// drained on its own thread (StreamRxPlan::refused); else success.
    [[nodiscard]] static auto check(StreamPortsOptions const& options) noexcept -> Status;

    /// Open the ports @p options choose. A port that does not open is recorded
    /// (tx_error(), rx_error(), xdp_error()) and stays closed. Errors: check()'s,
    /// before anything is opened.
    [[nodiscard]] auto open(StreamPortsOptions const& options) -> Status;
    /// Close every port / the receive port.
    void close() noexcept;
    void close_rx() noexcept;

    /// The transmit and receive ports (open or not: see the errors).
    [[nodiscard]] auto tx() noexcept -> StreamPort& { return packet_; }
    [[nodiscard]] auto rx() noexcept -> StreamPort& { return *rx_; }
    /// The AF_PACKET and AF_XDP ports themselves.
    [[nodiscard]] auto packet() noexcept -> PacketStreamPort& { return packet_; }
    [[nodiscard]] auto packet() const noexcept -> PacketStreamPort const& { return packet_; }
    [[nodiscard]] auto xdp() const noexcept -> XdpStreamPort const& { return xdp_; }

    /// xdp when receive is on the AF_XDP port, else packet.
    [[nodiscard]] auto transport_in_force() const noexcept -> StreamTransport
    {
        return rx_ == &xdp_ ? StreamTransport::xdp : StreamTransport::packet;
    }
    /// Why the transmit / receive port did not open (empty: it did, or receive
    /// was not asked for). Under xdp a failed AF_XDP open is the receive error.
    [[nodiscard]] auto tx_error() const noexcept -> std::error_code { return tx_error_; }
    [[nodiscard]] auto rx_error() const noexcept -> std::error_code { return rx_error_; }
    /// Why the AF_XDP port did not open (empty: it did, or was not tried).
    [[nodiscard]] auto xdp_error() const noexcept -> std::error_code { return xdp_error_; }
    /// Whether receive fell back to AF_PACKET after AF_XDP did not open (auto).
    [[nodiscard]] auto rx_fell_back() const noexcept -> bool { return xdp_error_ && rx_ == &packet_ && !rx_error_; }

    /// Begin / end a media tick on both ports (StreamPort::begin_cycle /
    /// end_cycle). Media thread. Errors: the first port's that failed.
    void begin_tick() noexcept;
    [[nodiscard]] auto end_tick() noexcept -> Status;

  private:
    PacketStreamPort packet_{};
    XdpStreamPort xdp_{};
    StreamPort* rx_{&packet_};
    std::error_code tx_error_{};
    std::error_code rx_error_{};
    std::error_code xdp_error_{};
};

/// Warn on @p log, when AF_XDP did not open on @p interface_name, what
/// receive does instead (AF_PACKET, or nothing). Reactor thread.
void log_xdp_error(logging::Logger log, StreamPorts const& ports, std::string_view interface_name) noexcept;

}  // namespace statusbar::avb_entity
