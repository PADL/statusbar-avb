#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_stream_port.hpp
/// @brief StreamPort: the one Ethernet port an entity's AVTP streams go
/// through, with the backend chosen at runtime.
///
/// The entity's talker sends from the media-timer thread and its listener
/// receives on the reactor or an RT RX timer: two threads on one port.
/// AnyEthernetPort says whether the backend it opened can take that
/// (rawnet: two sockets, yes) or not (the ring backends share state
/// between the two sides). This class carries that answer as a lock that
/// is taken only when it is needed, so the rawnet path - the default, and
/// what the entities have always run on - pays nothing for the ring
/// backends' constraint, and the ring backends are correct. The lock is a
/// plain spin: it assumes the two sides run on different cores, as the
/// media timer and the RX drain do on the rigs this is for. A holder that
/// shares a core with a SCHED_FIFO waiter would be spun against until the
/// kernel's RT throttling let it run.
///
/// Guard holds that lock for the shortest stretch that is still right: a
/// tx_start/tx_commit pair, or an rx_start/copy/rx_release triple. The
/// listener copies each frame out under the guard and decodes it outside,
/// so the media timer never waits on a decode.
///
/// The XDP backend is opened with the AVTP stream rules from
/// net_xdp_avtp_rules.hpp: only the three stream subtypes are steered into
/// the socket, so ADP, AECP and ACMP - the same ethertype - keep reaching
/// the kernel and the control plane's own raw socket.

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_spin_lock.hpp"
#include "statusbar/net/net_any_ethernet_port.hpp"
#include "statusbar/net/net_error.hpp"
#include "statusbar/net/net_xdp_avtp_rules.hpp"
#include "statusbar/status/status.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace statusbar::avb_entity {

struct StreamPortConfig
{
    /// One of net::ETHERNET_BACKEND_CHOICES. Not a backend name: refused.
    std::string_view backend{"rawnet"};
    std::string_view interface_name;
    /// skb->priority for the stream frames, where the backend takes one.
    uint8_t stream_pcp{3};
    /// The NIC queue the xdp backend binds to.
    uint32_t xdp_queue{0};
};

/// The 802.1Q-tagged AVTP header of a stream frame.
[[nodiscard]] inline auto stream_frame_header(
    ieee::Eui48 const& dest_mac, ieee::Eui48 const& src_mac, uint16_t vlan_id, uint8_t pcp) noexcept -> ieee::EthernetFrame
{
    ieee::EthernetFrame header{};
    header.dest_mac = dest_mac;
    header.src_mac = src_mac;
    header.vlan_tag = ieee::VlanTag{vlan_id, false, pcp};
    header.ethertype = avtp::AVTP_ETHERTYPE;
    return header;
}

/// Write an 802.1Q-tagged AVTP frame header and @p payload into @p buffer:
/// the frame length, or 0 when the buffer cannot hold them.
[[nodiscard]] inline auto build_stream_frame(
    std::span<uint8_t> buffer,
    ieee::Eui48 const& dest_mac,
    ieee::Eui48 const& src_mac,
    uint16_t vlan_id,
    uint8_t pcp,
    std::span<uint8_t const> payload) noexcept -> size_t
{
    auto const header = stream_frame_header(dest_mac, src_mac, vlan_id, pcp);
    size_t const header_length = ieee::wire_size(header);
    if (buffer.size() < header_length + payload.size()) {
        return 0;
    }
    (void)ieee::store_unchecked(buffer, header);
    span_copy(buffer.subspan(header_length, payload.size()), payload);
    return header_length + payload.size();
}

class StreamPort
{
  public:
    StreamPort() noexcept = default;
    StreamPort(StreamPort const&) = delete;
    auto operator=(StreamPort const&) -> StreamPort& = delete;

    /// Serialises TX against RX when the backend needs it; free otherwise.
    class Guard
    {
      public:
        explicit Guard(StreamPort& port) noexcept
            : lock_{port.serialize_ ? &port.lock_ : nullptr}
        {
            if (lock_ != nullptr) {
                lock_->lock();
            }
        }
        ~Guard()
        {
            if (lock_ != nullptr) {
                lock_->unlock();
            }
        }
        Guard(Guard const&) = delete;
        auto operator=(Guard const&) -> Guard& = delete;

      private:
        itc::SpinLock* lock_;
    };

    [[nodiscard]] auto open(StreamPortConfig const& config) -> Status
    {
        auto const backend = net::parse_ethernet_backend(config.backend);
        if (!backend) {
            return failure(net::NetError::invalid_argument);
        }
        auto status = port_.open(
            *backend,
            net::AnyEthernetPortConfig{
                .interface_name = config.interface_name,
                .ethertype = avtp::AVTP_ETHERTYPE,
                .priority = config.stream_pcp,
                .xdp_queue_id = config.xdp_queue,
                .xdp_filter_rules = rules_,
            });
        serialize_ = status.has_value() && !port_.tx_rx_independent();
        if (status) {
            ++generation_;
        }
        return status;
    }
    void close() noexcept
    {
        port_.close();
        serialize_ = false;
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return port_.valid(); }

    /// Which open this is: 0 before the first, one more on each open that
    /// succeeded. A holder that attached to the port remembers the value and
    /// knows, when it differs, that the port it attached to was closed and
    /// another opened in its place - a close followed by a reopen between
    /// two of its looks would otherwise pass for the port it knew.
    [[nodiscard]] auto generation() const noexcept -> uint32_t { return generation_; }
    /// The fd a reactor polls for receive readiness.
    [[nodiscard]] auto fd() const noexcept -> int { return port_.fd(); }
    [[nodiscard]] auto describe() const -> std::string { return port_.describe(); }
    /// Whether TX and RX go through Guard's lock.
    [[nodiscard]] auto serialized() const noexcept -> bool { return serialize_; }

    /// The port. Take a Guard around any use from a thread that is not the
    /// only one using it.
    [[nodiscard]] auto port() noexcept -> net::AnyEthernetPort& { return port_; }

  private:
    net::AnyEthernetPort port_{};
    net::AvtpStreamRules rules_{net::avtp_stream_rules()};
    itc::SpinLock lock_{};
    bool serialize_{false};
    uint32_t generation_{0};
};

}  // namespace statusbar::avb_entity
