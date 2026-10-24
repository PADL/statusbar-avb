#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// XdpStreamPort: the StreamPort over one AF_XDP socket on the SR class A NIC queue (queue 0 on
/// igb/igc); see docs/STREAM_PORTS.md. Without libxdp, or off Linux, open() fails with
/// not_supported_on_platform.

#include "statusbar/avb_entity/avb_entity_stream_port.hpp"
#include "statusbar/avb_entity/avb_entity_stream_transport.hpp"
#include "statusbar/net/net_linux_xdp.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace statusbar::avb_entity {

/// What XdpStreamPort::open() takes past the interface.
struct XdpStreamPortOptions
{
    /// The NIC queue the socket binds (the SR class A queue).
    uint32_t queue_id{0};
    net::XdpBindMode bind_mode{net::XdpBindMode::auto_};
    /// How a fresh attach attaches the program (net::XdpConfig::attach_mode).
    net::XdpAttachMode attach_mode{net::XdpAttachMode::native};
    /// Where the program is pinned and adopted from; empty: not pinned.
    std::string pin_path{};
};

/// The options for @p mode on @p interface_name's queue @p queue_id: auto_ and
/// zerocopy attach natively, copy generically; pinned under
/// net::XdpProgram::pin_path_for(@p interface_name), so a pinned program is adopted.
[[nodiscard]] auto xdp_stream_port_options(StreamXdpMode mode, std::string_view interface_name, uint32_t queue_id)
    -> XdpStreamPortOptions;

class XdpStreamPort final : public StreamPort
{
  public:
    /// The fill ring's entries.
    static constexpr size_t FILL_RING = 1024;
    /// The RX and TX rings' entries.
    static constexpr size_t RX_RING = 256;
    static constexpr size_t TX_RING = 256;
    /// UMEM frames held back from the fill ring for transmit.
    static constexpr size_t TX_POOL = 128;
    /// UMEM frames: the fill ring's worth plus as many again for the RX ring,
    /// the TX pool and the frames in flight.
    static constexpr size_t UMEM_FRAMES = 2 * FILL_RING;
    static_assert(UMEM_FRAMES >= FILL_RING + RX_RING + TX_POOL + TX_RING);

    /// UMEM frames held back for transmit (internal; public for tests).
    class TxPool
    {
      public:
        [[nodiscard]] auto take() noexcept -> std::optional<net::EthernetTxSlot>
        {
            if (count_ == 0) {
                return std::nullopt;
            }
            return slots_[--count_];
        }
        /// False when full.
        auto put(net::EthernetTxSlot const& slot) noexcept -> bool
        {
            if (count_ == TX_POOL) {
                return false;
            }
            slots_[count_++] = slot;
            return true;
        }
        [[nodiscard]] auto size() const noexcept -> size_t { return count_; }
        /// Forget every frame (their UMEM is gone).
        void clear() noexcept { count_ = 0; }

      private:
        std::array<net::EthernetTxSlot, TX_POOL> slots_{};
        size_t count_{0};
    };

    /// The XdpConfig open() uses on @p interface_name: UMEM_FRAMES frames, a
    /// FILL_RING fill ring, @p rules, no per-frame clock, no drain budget (rx_poll
    /// bounds it) and @p options, both by view: they must outlive the config.
    [[nodiscard]] static auto config_for(
        std::string_view interface_name,
        XdpStreamPortOptions const& options,
        std::span<net::CompiledRule<32> const> rules) noexcept -> net::XdpConfig;

    XdpStreamPort() = default;
    ~XdpStreamPort() override = default;
    XdpStreamPort(XdpStreamPort const&) = delete;
    auto operator=(XdpStreamPort const&) -> XdpStreamPort& = delete;
    XdpStreamPort(XdpStreamPort&&) = delete;
    auto operator=(XdpStreamPort&&) -> XdpStreamPort& = delete;

    /// Open AF_XDP on @p interface_name with the AVTP stream rules, adopting or
    /// attaching (and pinning) the program as @p options say. Reactor thread.
    /// Errors: net::XdpError (no NIC support, libxdp or privilege; zero-copy refused).
    [[nodiscard]] auto open(std::string_view interface_name, XdpStreamPortOptions const& options) -> Status;
    /// Close the socket; a pinned native program stays attached. Reactor
    /// thread, while nothing receives or transmits.
    void close() noexcept;

    /// Whether the socket is open.
    [[nodiscard]] auto is_open() const noexcept -> bool { return context_.valid(); }
    /// The queue the socket is bound to (open()'s).
    [[nodiscard]] auto queue_id() const noexcept -> uint32_t { return queue_id_; }
    /// The mode the program runs in, and whether open() adopted a pinned one.
    [[nodiscard]] auto attach_mode() const noexcept -> net::XdpAttachMode { return context_.attach_mode(); }
    [[nodiscard]] auto adopted() const noexcept -> bool { return context_.adopted(); }
    /// The bind mode open() asked for.
    [[nodiscard]] auto bind_mode() const noexcept -> net::XdpBindMode { return context_.bind_mode(); }
    /// The socket's counters. On the thread that drains the port only.
    [[nodiscard]] auto socket_stats() const noexcept -> net::XdpSocketStats const& { return context_.stats(); }
    /// The frames held back for transmit (internal; public for tests).
    [[nodiscard]] auto tx_pool_size() const noexcept -> size_t { return tx_pool_.size(); }

    /// Transmit is not implemented yet: an empty window.
    [[nodiscard]] auto tx_begin(size_t capacity) noexcept -> std::span<uint8_t> override;
    /// Transmit is not implemented yet: not_supported.
    [[nodiscard]] auto tx_commit(std::span<uint8_t const> payload, ieee::Eui48 const& dest, uint16_t vid, uint8_t pcp) noexcept
        -> Status override;
    [[nodiscard]] auto tx_flush() noexcept -> Status override { return success(); }
    /// Reap completions and top up the TX pool, before rx_poll offers the
    /// kernel the rest of the UMEM.
    void begin_cycle() noexcept override;
    [[nodiscard]] auto end_cycle() noexcept -> Status override;
    /// Takes up to @p max_frames from the RX ring (a frame that is not AVTP is
    /// dropped but counts against @p max_frames), then replenishes the fill ring.
    auto rx_poll(RxFn const& fn, size_t max_frames) noexcept -> size_t override;
    /// None: the media tick drains the port.
    [[nodiscard]] auto rx_fd() const noexcept -> int override { return -1; }
    /// The interface's multicast filter (SIOCADDMULTI / SIOCDELMULTI).
    [[nodiscard]] auto join_multicast(ieee::Eui48 const& group) noexcept -> Status override;
    [[nodiscard]] auto leave_multicast(ieee::Eui48 const& group) noexcept -> Status override;
    void set_tx_tap(TxTap tap) noexcept override { tx_tap_ = std::move(tap); }
    [[nodiscard]] auto hardware_address() const noexcept -> ieee::Eui48 override { return context_.hardware_address(); }
    /// True only under a native program with a zero-copy bind.
    [[nodiscard]] auto zero_copy() const noexcept -> bool override { return context_.zero_copy(); }

  private:
    void refill_tx_pool() noexcept;

    net::XdpContext context_{};
    TxPool tx_pool_{};
    TxTap tx_tap_{};
    uint32_t queue_id_{0};
};

}  // namespace statusbar::avb_entity
