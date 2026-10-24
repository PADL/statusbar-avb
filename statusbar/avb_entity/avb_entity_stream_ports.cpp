// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// StreamPorts: opening the transport's ports, and the tick over both.

#include "statusbar/avb_entity/avb_entity_stream_ports.hpp"

#include <system_error>

namespace statusbar::avb_entity {

auto StreamPorts::check(StreamPortsOptions const& options) noexcept -> Status
{
    if (options.receive && stream_rx_plan(options.transport, options.rx_on_own_thread) == StreamRxPlan::refused) {
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
    tx_error_ = {};
    rx_error_ = {};
    xdp_error_ = {};
    rx_ = &packet_;
    if (auto tx = packet_.open_tx(options.interface_name, options.tx_priority); !tx) {
        tx_error_ = tx.error();
    }
    if (!options.receive) {
        return success();
    }
    auto const plan = stream_rx_plan(options.transport, options.rx_on_own_thread);
    if (plan == StreamRxPlan::xdp || plan == StreamRxPlan::xdp_then_packet) {
        auto const xdp_options = xdp_stream_port_options(options.xdp_mode, options.interface_name, options.xdp_queue);
        auto const opened = xdp_.open(options.interface_name, xdp_options);
        if (opened) {
            rx_ = &xdp_;
            return success();
        }
        xdp_error_ = opened.error();
        if (plan == StreamRxPlan::xdp) {
            rx_error_ = xdp_error_;
            return success();
        }
    }
    if (auto rx = packet_.open_rx(options.interface_name); !rx) {
        rx_error_ = rx.error();
    }
    return success();
}

void StreamPorts::close() noexcept
{
    packet_.close();
    xdp_.close();
    rx_ = &packet_;
}

void StreamPorts::close_rx() noexcept
{
    packet_.close_rx();
    xdp_.close();
    rx_ = &packet_;
}

void StreamPorts::begin_tick() noexcept
{
    packet_.begin_cycle();
    if (rx_ != &packet_) {
        rx_->begin_cycle();
    }
}

auto StreamPorts::end_tick() noexcept -> Status
{
    auto tx_ended = packet_.end_cycle();
    if (rx_ == &packet_) {
        return tx_ended;
    }
    auto rx_ended = rx_->end_cycle();
    return tx_ended ? rx_ended : tx_ended;
}

void log_xdp_error(logging::Logger log, StreamPorts const& ports, std::string_view const interface_name) noexcept
{
    auto const error = ports.xdp_error();
    if (!error) {
        return;
    }
    if (ports.rx_fell_back()) {
        log.warning(
            "stream RX over AF_PACKET: AF_XDP on {}: {}", logging::embed<16>(interface_name), logging::embed<40>(error.message()));
    } else {
        log.warning("no stream RX: AF_XDP on {}: {}", logging::embed<16>(interface_name), logging::embed<40>(error.message()));
    }
}

}  // namespace statusbar::avb_entity
