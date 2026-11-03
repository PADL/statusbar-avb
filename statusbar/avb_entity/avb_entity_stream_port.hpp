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
/// tx_start/copy/tx_commit triple, or an rx_start/copy/rx_release one. The
/// talker builds each frame outside it and the listener decodes each frame
/// outside it, so neither side ever waits on the other's render or decode. A
/// host that drives both sides from one thread says so (set_single_threaded()):
/// no lock is used then, the talker renders into the slot and the listener
/// decodes in place.
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
#include "statusbar/net/net_launch_order.hpp"
#include "statusbar/net/net_linux_xdp.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_xdp_avtp_rules.hpp"
#include "statusbar/status/status.hpp"

#include <atomic>
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
    /// The xdp backend's bind and attach modes (net::XdpConfig).
    net::XdpBindMode xdp_bind_mode{net::XdpBindMode::auto_};
    net::XdpAttachMode xdp_attach_mode{net::XdpAttachMode::native};
    /// Where the xdp backend pins its program, to be adopted by the next open; empty for none.
    std::string_view xdp_pin_path{};
    /// The xdp backend's frames carry launch times (AF_XDP TX metadata; the
    /// driver must honour them, net::netdev::launch_time_supported()).
    bool tx_launch_time{false};
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
                .xdp_attach_mode = config.xdp_attach_mode,
                .xdp_bind_mode = config.xdp_bind_mode,
                .xdp_tx_launch_time = config.tx_launch_time,
                .xdp_pin_path = config.xdp_pin_path,
            });
        if (status) {
            ++generation_;
        }
        test_launch_time_ = false;
        settle_mode(status.has_value());
        return status;
    }
    /// An in-process test port (net::AnyEthernetPort::open_test), its frames
    /// timed as an xdp socket with TX metadata would be when @p launch_time
    /// (test_launch_time_: the one thing a test port cannot report itself).
    [[nodiscard]] auto open_test(net::TestPortConfig const& config, bool const launch_time = false) -> Status
    {
        auto status = port_.open_test(config);
        if (status) {
            ++generation_;
        }
        test_launch_time_ = launch_time;
        settle_mode(status.has_value());
        return status;
    }
    /// The test port's context, nullptr unless open_test() opened one.
    [[nodiscard]] auto test_port() noexcept -> net::TestPortContext* { return port_.as<net::TestPortContext>(); }

    void close() noexcept
    {
        port_.close();
        test_launch_time_ = false;
        settle_mode(false);
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
    /// The host will drive TX and RX from one thread, never both at once: Guard uses no lock, the talker
    /// renders into the slot, the listener decodes in place and RX is never the reactor's. Adopted by the
    /// next open; refused (device_or_resource_busy) while the port is open.
    [[nodiscard]] auto set_single_threaded(bool const single) noexcept -> Status
    {
        if (valid()) {
            return failure(std::errc::device_or_resource_busy);
        }
        want_single_threaded_ = single;
        return success();
    }
    /// Whether the port in force was opened single threaded.
    [[nodiscard]] auto single_threaded() const noexcept -> bool { return single_threaded_; }

    /// The port. Take a Guard around any use from a thread that is not the
    /// only one using it.
    [[nodiscard]] auto port() noexcept -> net::AnyEthernetPort& { return port_; }

    /// The xdp backend's socket, for what only it offers (busy poll, counters,
    /// pinning); nullptr on another backend.
    [[nodiscard]] auto xdp() noexcept -> net::XdpContext* { return port_.as<net::XdpContext>(); }
    [[nodiscard]] auto xdp() const noexcept -> net::XdpContext const* { return port_.as<net::XdpContext>(); }

    /// The sending NIC's PHC, which an AF_XDP launch time is in, read once a frame (a PHC
    /// read is a syscall: a host caches one per tick). Set on the control thread before the
    /// media thread runs; without it frames are untimed.
    void set_phc_clock(net::ClockFn clock) noexcept { phc_ = std::move(clock); }

    /// Whether this port's frames carry launch times (an xdp socket opened
    /// with TX metadata).
    [[nodiscard]] auto tx_launch_time() const noexcept -> bool
    {
        auto const* const xdp_socket = xdp();
        return xdp_socket != nullptr ? xdp_socket->tx_launch_time() : test_launch_time_;
    }

    /// The launch time for a frame wanted at @p requested_ns (0: now), in
    /// commit order (net::LaunchOrder); 0, no launch time, on a port without
    /// or with no PHC set. Media thread.
    [[nodiscard]] auto launch_time(uint64_t const requested_ns) noexcept -> int64_t
    {
        if (!phc_ || !timed_) {  // timed_: tx_launch_time() as the port opened
            return 0;
        }
        return static_cast<int64_t>(launch_order_.next(requested_ns, [this] { return net::phc_now_ns(phc_); }));
    }

    /// Whether a talker writes its frames straight into the port's slots: no other thread drives the
    /// port (rawnet, or opened single threaded). A timed frame is then timed after its render.
    [[nodiscard]] auto writes_in_place() const noexcept -> bool { return !serialize_; }

    /// Give frame @p handle, started untimed, the launch time for @p requested_ns (launch_time()), fixed
    /// now, after its render: a render refused holds no place in the order. Media thread.
    void stamp_launch_time(uint64_t const handle, uint64_t const requested_ns) noexcept
    {
        if (int64_t const at = launch_time(requested_ns); at != 0) {
            (void)port_.tx_set_launch_time(handle, at);
        }
    }

    /// The launch-time ordering's counters (clamps, steps, late frames). Any thread.
    [[nodiscard]] auto launch_order() const noexcept -> net::LaunchOrder const& { return launch_order_; }

    /// Zero the socket's and the ordering's counters at the next end_cycle(),
    /// on the thread that drives the port. Any thread; not with an external
    /// kick, whose thread writes the flush counters (TalkerStreams::set_external_kick).
    void reset_counters() noexcept { reset_pending_.store(true, std::memory_order_relaxed); }

    /// End the port's cycle (net::AnyEthernetPort::end_cycle), and apply a
    /// counter reset asked for since the last. Under the guard.
    [[nodiscard]] auto end_cycle() -> Status
    {
        if (reset_pending_.exchange(false, std::memory_order_relaxed)) {
            reset_now();
        }
        return port_.end_cycle();
    }

  private:
    /// The mode of the port open() opened (or none, when @p ok is false): its threading and timing.
    void settle_mode(bool const ok) noexcept
    {
        single_threaded_ = ok && want_single_threaded_;
        serialize_ = ok && !port_.tx_rx_independent() && !single_threaded_;
        timed_ = ok && tx_launch_time();
    }

    void reset_now() noexcept
    {
        if (auto* const xdp_socket = xdp(); xdp_socket != nullptr) {
            xdp_socket->reset_stats();
        }
        launch_order_.reset();
    }

    net::AnyEthernetPort port_{};
    net::AvtpStreamRules rules_{net::avtp_stream_rules()};
    itc::SpinLock lock_{};
    bool serialize_{false};
    uint32_t generation_{0};
    bool want_single_threaded_{false};  ///< set_single_threaded()'s, for the next open
    bool single_threaded_{false};       ///< the port in force's: open() adopted it
    bool timed_{false};                 ///< the port in force's frames carry launch times
    bool test_launch_time_{false};
    net::ClockFn phc_{};
    net::LaunchOrder launch_order_{};
    std::atomic<bool> reset_pending_{false};
};

}  // namespace statusbar::avb_entity
