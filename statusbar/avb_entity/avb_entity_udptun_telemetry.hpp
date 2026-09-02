#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_udptun_telemetry.hpp
/// @brief Inter-site tunnel telemetry as a value handle whose copies alias
/// one counter block.

#include "statusbar/itc/itc_published.hpp"
#include "statusbar/itc/itc_telemetry_counter.hpp"

#include <cstdint>
#include <memory>

namespace statusbar::avb_entity {

/// Lock-free inter-thread telemetry for the inter-site tunnel. Producers run on
/// the RX / media-timer / punch-worker threads; readers are print_state and the
/// punch-service watchdog.
///
/// A regular-looking handle: copying it yields another handle onto the SAME
/// counters (the sharing is this type's business — the transport, ingest and
/// egress paths that bump the counters take a UdptunTelemetry by value and
/// are neither told nor concerned how it is owned). Const propagates: a
/// `UdptunTelemetry const` only reads. itc primitives (preferred over bare
/// std::atomic) give wait-free producer add()/publish() and acquire-ordered
/// reader load(); the counters live in one heap block because they are
/// atomics (non-movable) and must outlive every handle.
class UdptunTelemetry
{
  public:
    /// A fresh, zeroed counter block.
    UdptunTelemetry()
        : block_{std::make_shared<Block>()}
    {}

    /// primary tunnel datagrams sent
    [[nodiscard]] auto tx_packets() noexcept -> itc::TelemetryCounter<uint64_t>& { return block_->tx_packets; }
    [[nodiscard]] auto tx_packets() const noexcept -> itc::TelemetryCounter<uint64_t> const& { return block_->tx_packets; }
    /// decoded tunnel-audio datagrams received
    [[nodiscard]] auto rx_packets() noexcept -> itc::TelemetryCounter<uint64_t>& { return block_->rx_packets; }
    [[nodiscard]] auto rx_packets() const noexcept -> itc::TelemetryCounter<uint64_t> const& { return block_->rx_packets; }
    /// ALL datagrams received (audio + keepalive)
    [[nodiscard]] auto any_rx() noexcept -> itc::TelemetryCounter<uint64_t>& { return block_->any_rx; }
    [[nodiscard]] auto any_rx() const noexcept -> itc::TelemetryCounter<uint64_t> const& { return block_->any_rx; }
    /// egress anchor resets (watchdog)
    [[nodiscard]] auto egress_reset_count() noexcept -> itc::TelemetryCounter<uint64_t>& { return block_->egress_reset_count; }
    [[nodiscard]] auto egress_reset_count() const noexcept -> itc::TelemetryCounter<uint64_t> const&
    {
        return block_->egress_reset_count;
    }
    /// media-thread re-punches (escalation + stall)
    [[nodiscard]] auto egress_repunch_count() noexcept -> itc::TelemetryCounter<uint64_t>& { return block_->egress_repunch_count; }
    [[nodiscard]] auto egress_repunch_count() const noexcept -> itc::TelemetryCounter<uint64_t> const&
    {
        return block_->egress_repunch_count;
    }
    /// last real tunnel-audio ingest TAI (ns)
    [[nodiscard]] auto last_real_ingest_tai() noexcept -> itc::Published<int64_t>& { return block_->last_real_ingest_tai; }
    [[nodiscard]] auto last_real_ingest_tai() const noexcept -> itc::Published<int64_t> const&
    {
        return block_->last_real_ingest_tai;
    }

    /// Do two handles alias the same counters?
    [[nodiscard]] friend auto operator==(UdptunTelemetry const& a, UdptunTelemetry const& b) noexcept -> bool
    {
        return a.block_ == b.block_;
    }

  private:
    struct Block
    {
        itc::TelemetryCounter<uint64_t> tx_packets{};
        itc::TelemetryCounter<uint64_t> rx_packets{};
        itc::TelemetryCounter<uint64_t> any_rx{};
        itc::TelemetryCounter<uint64_t> egress_reset_count{};
        itc::TelemetryCounter<uint64_t> egress_repunch_count{};
        itc::Published<int64_t> last_real_ingest_tai{};
    };
    std::shared_ptr<Block> block_;
};

}  // namespace statusbar::avb_entity
