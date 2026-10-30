// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// XdpStreamPort: transmit from the TX pool, the receive side and the multicast filter.

#include "statusbar/avb_entity/avb_entity_xdp_stream_port.hpp"

#include "statusbar/net/net_linux_xdp.hpp"
#include "statusbar/net/net_xdp_avtp_rules.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>

namespace statusbar::avb_entity {

static_assert(net::XdpBindMode{} == net::XdpBindMode::auto_);
static_assert(net::XdpAttachMode{} == net::XdpAttachMode::native);

auto xdp_stream_port_options(StreamXdpMode const mode, std::string_view const interface_name, uint32_t const queue_id)
    -> XdpStreamPortOptions
{
    XdpStreamPortOptions options{.queue_id = queue_id, .pin_path = net::XdpProgram::pin_path_for(interface_name)};
    switch (mode) {
        case StreamXdpMode::zerocopy:
            options.bind_mode = net::XdpBindMode::zero_copy;
            options.attach_mode = net::XdpAttachMode::native;
            break;
        case StreamXdpMode::copy:
            options.bind_mode = net::XdpBindMode::copy;
            options.attach_mode = net::XdpAttachMode::generic;
            break;
        case StreamXdpMode::auto_:
            options.bind_mode = net::XdpBindMode::auto_;
            options.attach_mode = net::XdpAttachMode::native;
            break;
    }
    return options;
}

auto launch_time_support(StatusValue<bool> const& driver) noexcept -> Status
{
    return net::launch_time_support(driver);
}

auto probe_tx_metadata() -> Status
{
    return net::probe_tx_metadata();
}

auto XdpStreamPort::config_for(
    std::string_view const interface_name,
    XdpStreamPortOptions const& options,
    std::span<net::CompiledRule<32> const> const rules) noexcept -> net::XdpConfig
{
    return net::XdpConfig{
        .interface_name = interface_name,
        .filter_rules = rules,
        .default_action = net::XdpAction::pass_kernel,
        .queue_id = options.queue_id,
        .rx_ring_size = RX_RING,
        .tx_ring_size = TX_RING,
        .fill_ring_size = FILL_RING,
        .umem_frame_count = UMEM_FRAMES,
        .rx_drain_max = 0,
        .completion_drain_max = TX_RING,
        .rx_clock_id = -1,
        .bind_mode = options.bind_mode,
        .attach_mode = options.attach_mode,
        .pin_path = options.pin_path,
        .tx_launch_time = options.launch_time,
    };
}

XdpStreamPort::XdpStreamPort()
    : context_{std::make_unique<net::XdpContext>()}
{}

XdpStreamPort::~XdpStreamPort() = default;

auto XdpStreamPort::is_open() const noexcept -> bool
{
    return context_->valid();
}

auto XdpStreamPort::attach_mode() const noexcept -> net::XdpAttachMode
{
    return context_->attach_mode();
}

auto XdpStreamPort::adopted() const noexcept -> bool
{
    return context_->adopted();
}

auto XdpStreamPort::pinned() const noexcept -> bool
{
    return context_->program().pinned();
}

auto XdpStreamPort::bind_mode() const noexcept -> net::XdpBindMode
{
    return context_->bind_mode();
}

auto XdpStreamPort::socket_stats() const noexcept -> net::XdpSocketStats const&
{
    return context_->stats();
}

auto XdpStreamPort::hardware_address() const noexcept -> ieee::Eui48
{
    return context_->hardware_address();
}

auto XdpStreamPort::zero_copy() const noexcept -> bool
{
    return context_->zero_copy();
}

auto XdpStreamPort::launch_time() const noexcept -> bool
{
    return context_->tx_launch_time();
}

auto XdpStreamPort::with_launch_time(XdpStreamPortOptions const& options) -> XdpStreamPortOptions
{
    launch_time_error_ = {};
    launch_order_.reset();
    return launch_time_as_probed(options, probe_tx_metadata, launch_time_error_);
}

auto XdpStreamPort::open_socket(std::string_view const interface_name, XdpStreamPortOptions const& options) -> Status
{
    auto const rules = net::avtp_stream_rules();
    return context_->open(config_for(interface_name, with_launch_time(options), rules));
}

auto XdpStreamPort::open(std::string_view const interface_name, XdpStreamPortOptions const& options) -> Status
{
    close();
    if (auto opened = open_socket(interface_name, options); !opened) {
        return opened;
    }
    queue_id_ = options.queue_id;
    tx_health_.reset();
    return success();
}

void XdpStreamPort::close() noexcept
{
    tx_pending_.reset();
    tx_window_ = 0;
    tx_pool_.clear();
    context_->close();
}

auto XdpStreamPort::tx_begin(size_t const capacity) noexcept -> std::span<uint8_t>
{
    if (!context_->valid() || capacity > MAX_TX_PAYLOAD) {
        return {};
    }
    if (!tx_pending_) {
        tx_pending_ = tx_pool_.take();
        if (!tx_pending_) {
            tx_pool_empty_.store(tx_pool_empty_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            return {};
        }
    }
    tx_window_ = capacity;
    return tx_pending_->buffer.subspan(TX_PAYLOAD_OFFSET, capacity);
}

auto XdpStreamPort::commit_slot(net::EthernetTxSlot const& slot, size_t const length, uint64_t const launch_time_ns) noexcept
    -> Status
{
    // A pooled frame was taken untimed (tx_start): its launch time goes in now, every frame timed.
    if (context_->tx_launch_time()) {
        context_->tx_set_launch_time(slot.handle, static_cast<int64_t>(launch_order_.next(launch_time_ns, net::tai_now_ns)));
    }
    if (auto committed = context_->tx_commit(slot.handle, length); !committed) {
        // The socket freed the frame: take one back so the pool stays whole.
        if (auto again = context_->tx_start()) {
            (void)tx_pool_.put(*again);
        }
        count_tx(false);
        return committed;
    }
    count_tx(true);
    if (tx_tap_) {
        tx_tap_(std::span<uint8_t const>{slot.buffer.data(), length});
    }
    return success();
}

auto XdpStreamPort::tx_commit(
    std::span<uint8_t const> const payload,
    ieee::Eui48 const& dest,
    uint16_t const vid,
    uint8_t const pcp,
    uint64_t const launch_time_ns) noexcept -> Status
{
    if (!tx_pending_ || payload.data() != tx_pending_->buffer.data() + TX_PAYLOAD_OFFSET || payload.size() > tx_window_) {
        count_tx(false);
        return failure(std::errc::invalid_argument);
    }
    auto const slot = *tx_pending_;
    tx_pending_.reset();
    write_stream_header(slot.buffer, dest, context_->hardware_address(), vid, pcp);
    return commit_slot(slot, TX_PAYLOAD_OFFSET + payload.size(), launch_time_ns);
}

auto XdpStreamPort::tx_flush() noexcept -> Status
{
    if (!context_->valid()) {
        return success();
    }
    auto flushed = context_->tx_flush();
    tx_health_.record(flushed.has_value(), flushed ? 0 : context_->stats().tx_flush_errno);
    return flushed;
}

void XdpStreamPort::refill_tx_pool() noexcept
{
    while (tx_pool_.size() < TX_POOL) {
        auto slot = context_->tx_start();
        if (!slot) {
            break;
        }
        (void)tx_pool_.put(*slot);
    }
}

void XdpStreamPort::begin_cycle() noexcept
{
    if (!context_->valid()) {
        return;
    }
    context_->begin_cycle();
    (void)context_->tx_complete();
    refill_tx_pool();
}

auto XdpStreamPort::end_cycle() noexcept -> Status
{
    return success();
}

auto XdpStreamPort::rx_poll(RxFn const& fn, size_t const max_frames) noexcept -> size_t
{
    if (!context_->valid()) {
        return 0;
    }
    size_t delivered = 0;
    for (size_t taken = 0; taken < max_frames; ++taken) {
        auto slot = context_->rx_start();
        if (!slot || !*slot) {
            break;
        }
        auto const& rx = **slot;
        if (auto const offset = net::avtp_payload_offset(rx.buffer)) {
            count_rx();
            fn(rx.buffer.subspan(*offset), rx.buffer);
            ++delivered;
        }
        (void)context_->rx_release(rx.handle);
    }
    (void)context_->fill_replenish();
    return delivered;
}

auto XdpStreamPort::join_multicast(ieee::Eui48 const& group) noexcept -> Status
{
    return context_->join_multicast(group);
}

auto XdpStreamPort::leave_multicast(ieee::Eui48 const& group) noexcept -> Status
{
    return context_->leave_multicast(group);
}

}  // namespace statusbar::avb_entity
