#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// StreamPorts: the ports an entity's streams go through, chosen by
/// StreamTransport. Under xdp (or auto, when it opens) the AF_XDP port carries
/// transmit and receive; else the AF_PACKET one does.

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
    /// AF_XDP frames carry their launch time: the host found the driver honours it (XdpStreamPortOptions).
    bool launch_time{false};
};

/// Which ports StreamPorts::open() opens. An AF_XDP socket takes the stream
/// frames from the kernel, so it carries both directions or neither.
enum class StreamPortPlan : uint8_t
{
    packet,           ///< AF_PACKET
    xdp,              ///< AF_XDP, or nothing
    xdp_then_packet,  ///< AF_XDP, else AF_PACKET
    refused,          ///< AF_XDP drained on its own thread
};

/// The plan for @p transport when receive is drained on its own thread
/// (@p rx_on_own_thread) or not (auto_ on its own thread: packet).
[[nodiscard]] constexpr auto stream_port_plan(StreamTransport const transport, bool const rx_on_own_thread) noexcept
    -> StreamPortPlan
{
    switch (transport) {
        case StreamTransport::xdp:
            return rx_on_own_thread ? StreamPortPlan::refused : StreamPortPlan::xdp;
        case StreamTransport::auto_:
            return rx_on_own_thread ? StreamPortPlan::packet : StreamPortPlan::xdp_then_packet;
        case StreamTransport::packet:
            break;
    }
    return StreamPortPlan::packet;
}

/// The entity's stream ports. open()/close() on the reactor thread while
/// nothing streams. The tick, every tx_* and rx_poll run on the media thread
/// only (an AF_XDP socket has one producer and one consumer).
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
    /// drained on its own thread (StreamPortPlan::refused); else success.
    [[nodiscard]] static auto check(StreamPortsOptions const& options) noexcept -> Status;

    /// Open the ports @p options choose. A port that does not open is recorded
    /// (tx_error(), rx_error(), xdp_error()) and stays closed. Errors: check()'s,
    /// before anything is opened.
    [[nodiscard]] auto open(StreamPortsOptions const& options) -> Status;
    /// Close every port.
    void close() noexcept;
    /// Stop receiving: the AF_PACKET receive socket closes; an AF_XDP port that
    /// also transmits stays open and the tick discards what it receives.
    void close_rx() noexcept;

    /// The transmit and receive ports (open or not: see the errors).
    [[nodiscard]] auto tx() noexcept -> StreamPort& { return *tx_; }
    [[nodiscard]] auto rx() noexcept -> StreamPort& { return *rx_; }
    /// The AF_PACKET and AF_XDP ports themselves.
    [[nodiscard]] auto packet() noexcept -> PacketStreamPort& { return packet_; }
    [[nodiscard]] auto packet() const noexcept -> PacketStreamPort const& { return packet_; }
    /// The AF_XDP port; mutable for its any-thread calls (reset_counters), the drain thread's otherwise.
    [[nodiscard]] auto xdp() noexcept -> XdpStreamPort& { return xdp_; }
    [[nodiscard]] auto xdp() const noexcept -> XdpStreamPort const& { return xdp_; }

    /// xdp when the streams go through the AF_XDP port, else packet.
    [[nodiscard]] auto transport_in_force() const noexcept -> StreamTransport
    {
        return tx_ == &xdp_ ? StreamTransport::xdp : StreamTransport::packet;
    }
    /// Why the transmit / receive port did not open (empty: it did, or receive
    /// was not asked for). Under xdp a failed AF_XDP open is both errors.
    [[nodiscard]] auto tx_error() const noexcept -> std::error_code { return tx_error_; }
    [[nodiscard]] auto rx_error() const noexcept -> std::error_code { return rx_error_; }
    /// Why the AF_XDP port did not open (empty: it did, or was not tried).
    [[nodiscard]] auto xdp_error() const noexcept -> std::error_code { return xdp_error_; }
    /// Whether the streams fell back to AF_PACKET after AF_XDP did not open (auto).
    [[nodiscard]] auto fell_back() const noexcept -> bool { return xdp_error_ && tx_ == &packet_ && !tx_error_; }

    /// Begin / end a media tick on both ports (StreamPort::begin_cycle /
    /// end_cycle). begin_tick drains an open AF_XDP port nobody receives from,
    /// so its queue keeps moving. Media thread. Errors: the first port's that failed.
    void begin_tick() noexcept;
    [[nodiscard]] auto end_tick() noexcept -> Status;

  private:
    /// Open the AF_XDP port when @p plan asks for it: true when that settles the ports
    /// (opened, or failed under xdp); false to go on with AF_PACKET.
    [[nodiscard]] auto open_xdp(StreamPortsOptions const& options, StreamPortPlan plan) -> bool;
    /// Open the AF_PACKET ports, recording tx_error() and rx_error().
    void open_packet(StreamPortsOptions const& options);

    PacketStreamPort packet_{};
    XdpStreamPort xdp_{};
    StreamPort* tx_{&packet_};
    StreamPort* rx_{&packet_};
    std::error_code tx_error_{};
    std::error_code rx_error_{};
    std::error_code xdp_error_{};
};

/// Warn on @p log, when AF_XDP did not open on @p interface_name, what the
/// streams do instead (AF_PACKET, or nothing). Reactor thread.
void log_xdp_error(logging::Logger log, StreamPorts const& ports, std::string_view interface_name) noexcept;

/// Say on @p log whether the streams' frames carry launch times, when @p options asked for them,
/// and why not when they do not. Reactor thread.
void log_launch_time(logging::Logger log, StreamPorts const& ports, StreamPortsOptions const& options) noexcept;

}  // namespace statusbar::avb_entity
