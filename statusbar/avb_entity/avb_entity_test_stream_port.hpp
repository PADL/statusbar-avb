#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// TestStreamPort: a StreamPort over net::TestPortContext, in process, so the
/// whole stream transmit path and the receive routing run in unit tests.

#include "statusbar/avb_entity/avb_entity_stream_port.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/net/net_test_port.hpp"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/status.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>

namespace statusbar::avb_entity {

/// A StreamPort whose link is a net::TestPortContext: committed frames are
/// captured whole (pop_tx), injected frames are received (inject_rx), and
/// flushes, cycles and multicast membership are recorded. Single-threaded.
class TestStreamPort final : public StreamPort
{
  public:
    /// The most multicast groups joined at once.
    static constexpr size_t MAX_GROUPS = 32;

    /// Open the underlying test port with source address @p mac. Allocates
    /// its frame pool from @p memory_resource (nullptr: the default).
    explicit TestStreamPort(ieee::Eui48 const& mac, std::pmr::memory_resource* memory_resource = nullptr);
    ~TestStreamPort() override = default;
    TestStreamPort(TestStreamPort const&) = delete;
    auto operator=(TestStreamPort const&) -> TestStreamPort& = delete;
    TestStreamPort(TestStreamPort&&) = delete;
    auto operator=(TestStreamPort&&) -> TestStreamPort& = delete;

    [[nodiscard]] auto tx_begin(size_t capacity) noexcept -> std::span<uint8_t> override;
    [[nodiscard]] auto tx_commit(std::span<uint8_t const> payload, ieee::Eui48 const& dest, uint16_t vid, uint8_t pcp) noexcept
        -> Status override;
    [[nodiscard]] auto tx_flush() noexcept -> Status override;
    void begin_cycle() noexcept override { ++cycles_begun_; }
    [[nodiscard]] auto end_cycle() noexcept -> Status override;
    /// Delivers the queued AVTP frames, untagged or under one 802.1Q tag;
    /// other ethertypes are consumed and not delivered.
    auto rx_poll(RxFn const& fn, size_t max_frames) noexcept -> size_t override;
    [[nodiscard]] auto rx_fd() const noexcept -> int override { return -1; }
    [[nodiscard]] auto join_multicast(ieee::Eui48 const& group) noexcept -> Status override;
    [[nodiscard]] auto leave_multicast(ieee::Eui48 const& group) noexcept -> Status override;
    void set_tx_tap(TxTap tap) noexcept override { tap_ = std::move(tap); }
    [[nodiscard]] auto hardware_address() const noexcept -> ieee::Eui48 override { return port_.hardware_address(); }
    [[nodiscard]] auto zero_copy() const noexcept -> bool override { return false; }

    /// Queue @p frame (a whole L2 frame) for rx_poll. Errors: the pool is full.
    [[nodiscard]] auto inject_rx(std::span<uint8_t const> frame) -> Status { return port_.inject_rx(frame); }
    /// The oldest committed frame not yet popped, L2 header included.
    [[nodiscard]] auto pop_tx() -> std::optional<net::TestPortContext::TxCapture> { return port_.pop_tx(); }
    /// Committed frames not yet popped.
    [[nodiscard]] auto tx_pending() const noexcept -> size_t { return port_.tx_pending(); }
    /// How many times tx_flush / begin_cycle / end_cycle ran.
    [[nodiscard]] auto flushes() const noexcept -> size_t { return flushes_; }
    [[nodiscard]] auto cycles_begun() const noexcept -> size_t { return cycles_begun_; }
    [[nodiscard]] auto cycles_ended() const noexcept -> size_t { return cycles_ended_; }
    /// Refuse every tx_commit from now on (@p refuse) as a full ring does: the
    /// frame is not sent and goes back to the pool (errc::no_buffer_space).
    void refuse_commits(bool const refuse) noexcept { refuse_commits_ = refuse; }
    /// The groups joined and not left, in join order.
    [[nodiscard]] auto joined() const noexcept -> std::span<ieee::Eui48 const> { return {joined_.data(), joined_.size()}; }

  private:
    net::TestPortContext port_{};
    std::optional<net::EthernetTxSlot> pending_{};  ///< the tx_begin frame not yet committed
    TxTap tap_{};
    bool refuse_commits_{false};
    size_t flushes_{0};
    size_t cycles_begun_{0};
    size_t cycles_ended_{0};
    sg14::inplace_vector<ieee::Eui48, MAX_GROUPS> joined_{};
};

}  // namespace statusbar::avb_entity
