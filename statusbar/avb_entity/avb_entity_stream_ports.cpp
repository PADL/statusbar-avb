// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// StreamPorts: opening the transport's ports, and the tick over them.

#include "statusbar/avb_entity/avb_entity_stream_ports.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>

namespace statusbar::avb_entity {

namespace {

/// The AF_XDP port's options for @p options: its mode, queue and launch time.
[[nodiscard]] auto xdp_options_for(StreamPortsOptions const& options) -> XdpStreamPortOptions
{
    auto xdp = xdp_stream_port_options(options.xdp_mode, options.interface_name, options.xdp_queue);
    xdp.launch_time = options.launch_time;
    return xdp;
}

}  // namespace

auto StreamPorts::check(StreamPortsOptions const& options) noexcept -> Status
{
    if (options.receive && stream_port_plan(options.transport, options.rx_on_own_thread) == StreamPortPlan::refused) {
        return failure(std::errc::invalid_argument);
    }
    return success();
}

auto StreamPorts::open(StreamPortsOptions const& options) -> Status
{
    if (auto checked = check(options); !checked) {
        return checked;
    }
    close();
    xdp_error_ = {};
    // Without a receiver the drain thread does not matter: the tick discards.
    auto const plan = stream_port_plan(options.transport, options.receive && options.rx_on_own_thread);
    if (!open_xdp(options, plan)) {
        open_packet(options);
    }
    return success();
}

auto StreamPorts::open_xdp(StreamPortsOptions const& options, StreamPortPlan const plan) -> bool
{
    if (plan != StreamPortPlan::xdp && plan != StreamPortPlan::xdp_then_packet) {
        return false;
    }
    auto const opened = xdp_.open(options.interface_name, xdp_options_for(options));
    if (opened) {
        tx_ = &xdp_;
        rx_ = options.receive ? static_cast<StreamPort*>(&xdp_) : &packet_;
        tx_error_ = {};
        rx_error_ = {};
        return true;
    }
    xdp_error_ = opened.error();
    if (plan == StreamPortPlan::xdp) {
        tx_error_ = xdp_error_;
        rx_error_ = options.receive ? xdp_error_ : std::error_code{};
    }
    return plan == StreamPortPlan::xdp;
}

void StreamPorts::open_packet(StreamPortsOptions const& options)
{
    auto const tx = packet_.open_tx(options.interface_name, options.tx_priority);
    tx_error_ = tx ? std::error_code{} : tx.error();
    rx_error_ = {};
    if (!options.receive) {
        return;
    }
    auto const rx = packet_.open_rx(options.interface_name);
    rx_error_ = rx ? std::error_code{} : rx.error();
}

void StreamPorts::close() noexcept
{
    packet_.close();
    xdp_.close();
    tx_ = &packet_;
    rx_ = &packet_;
}

void StreamPorts::close_rx() noexcept
{
    packet_.close_rx();
    if (tx_ != &xdp_) {
        xdp_.close();
    }
    rx_ = &packet_;
}

void StreamPorts::begin_tick() noexcept
{
    packet_.begin_cycle();
    if (!xdp_.is_open()) {
        return;
    }
    xdp_.begin_cycle();
    if (rx_ != &xdp_) {  // open for transmit, nobody receiving
        static constexpr size_t DISCARD_MAX = XdpStreamPort::RX_RING;
        (void)xdp_.rx_poll([](std::span<uint8_t const>, std::span<uint8_t const>) {}, DISCARD_MAX);
    }
}

auto StreamPorts::end_tick() noexcept -> Status
{
    auto packet_ended = packet_.end_cycle();
    if (!xdp_.is_open()) {
        return packet_ended;
    }
    auto xdp_ended = xdp_.end_cycle();
    return packet_ended ? xdp_ended : packet_ended;
}

void log_xdp_error(logging::Logger log, StreamPorts const& ports, std::string_view const interface_name) noexcept
{
    auto const error = ports.xdp_error();
    if (!error) {
        return;
    }
    if (ports.fell_back()) {
        log.warning(
            "streams over AF_PACKET: AF_XDP on {}: {}", logging::embed<16>(interface_name), logging::embed<40>(error.message()));
    } else {
        log.warning("no streams: AF_XDP on {}: {}", logging::embed<16>(interface_name), logging::embed<40>(error.message()));
    }
}

void log_launch_time(logging::Logger log, StreamPorts const& ports, StreamPortsOptions const& options) noexcept
{
    if (!options.launch_time) {
        return;
    }
    auto const name = logging::embed<16>(options.interface_name);
    if (ports.transport_in_force() == StreamTransport::xdp && ports.xdp().launch_time()) {
        log.status("streams: AF_XDP frames on {} carry their launch time", name);
        return;
    }
    if (auto const error = ports.xdp().launch_time_error(); error) {
        log.warning("streams: no launch time on {}: {}", name, logging::embed<40>(error.message()));
        return;
    }
    log.warning("streams: no launch time on {}: the streams are not on AF_XDP", name);
}

}  // namespace statusbar::avb_entity
