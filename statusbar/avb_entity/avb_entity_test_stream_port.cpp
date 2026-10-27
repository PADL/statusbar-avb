// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// TestStreamPort over net::TestPortContext.

#include "statusbar/avb_entity/avb_entity_test_stream_port.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <system_error>

namespace statusbar::avb_entity {

TestStreamPort::TestStreamPort(ieee::Eui48 const& mac, std::pmr::memory_resource* const memory_resource)
{
    (void)port_.open(net::TestPortConfig{.hardware_address = mac, .interface_index = 1, .frame_pool_size = 64}, memory_resource);
}

auto TestStreamPort::tx_begin(size_t const capacity) noexcept -> std::span<uint8_t>
{
    if (capacity > MAX_TX_PAYLOAD) {
        return {};
    }
    if (!pending_) {
        auto slot = port_.tx_start();
        if (!slot) {
            return {};
        }
        pending_ = *slot;
    }
    return pending_->buffer.subspan(TX_PAYLOAD_OFFSET, capacity);
}

auto TestStreamPort::tx_commit(
    std::span<uint8_t const> const payload, ieee::Eui48 const& dest, uint16_t const vid, uint8_t const pcp) noexcept -> Status
{
    if (!pending_ || payload.data() != pending_->buffer.data() + TX_PAYLOAD_OFFSET ||
        payload.size() > pending_->buffer.size() - TX_PAYLOAD_OFFSET) {
        count_tx(false);
        return failure(std::errc::invalid_argument);
    }
    auto const slot = *pending_;
    pending_.reset();
    if (refuse_commits_) {
        (void)port_.tx_cancel(slot.handle);
        count_tx(false);
        return failure(std::errc::no_buffer_space);
    }
    write_stream_header(slot.buffer, dest, port_.hardware_address(), vid, pcp);
    return commit_slot(slot, TX_PAYLOAD_OFFSET + payload.size());
}

auto TestStreamPort::commit_slot(net::EthernetTxSlot const& slot, size_t const length) noexcept -> Status
{
    auto const committed = port_.tx_commit(slot.handle, length);
    count_tx(committed.has_value());
    if (!committed) {
        return committed;
    }
    if (tap_) {
        tap_(std::span<uint8_t const>{slot.buffer.data(), length});
    }
    return success();
}

auto TestStreamPort::tx_flush() noexcept -> Status
{
    ++flushes_;
    return port_.tx_flush();
}

auto TestStreamPort::end_cycle() noexcept -> Status
{
    ++cycles_ended_;
    return port_.end_cycle();
}

auto TestStreamPort::rx_poll(RxFn const& fn, size_t const max_frames) noexcept -> size_t
{
    size_t delivered = 0;
    while (delivered < max_frames) {
        auto received = port_.rx_start();
        if (!received || !received->has_value()) {
            break;
        }
        auto const slot = **received;
        if (size_t const offset = stream_payload_offset(slot.buffer); offset != 0) {
            count_rx();
            fn(slot.buffer.subspan(offset), slot.buffer);
            ++delivered;
        }
        (void)port_.rx_release(slot.handle);
    }
    return delivered;
}

auto TestStreamPort::join_multicast(ieee::Eui48 const& group) noexcept -> Status
{
    if (std::ranges::find(joined_, group) == joined_.end() && joined_.try_push_back(group) == nullptr) {
        return failure(std::errc::no_buffer_space);
    }
    return success();
}

auto TestStreamPort::leave_multicast(ieee::Eui48 const& group) noexcept -> Status
{
    auto* const it = std::ranges::find(joined_, group);
    if (it == joined_.end()) {
        return failure(std::errc::invalid_argument);
    }
    joined_.erase(it);
    return success();
}

}  // namespace statusbar::avb_entity
