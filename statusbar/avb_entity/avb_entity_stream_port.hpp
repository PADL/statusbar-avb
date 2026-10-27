#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// StreamPort: the link layer under the AVB stream data plane, shared by
/// TalkerStreams (transmit) and ListenerStreams (receive), and
/// PacketStreamPort, its AF_PACKET (macOS: BPF) implementation.

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/status/status.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>

namespace statusbar::avb_entity {

/// What a StreamPort has done since it was constructed. Any thread.
struct StreamPortStats
{
    uint64_t tx_frames{0};  ///< frames tx_commit handed to the link
    uint64_t tx_errors{0};  ///< frames tx_commit refused or the link rejected
    uint64_t rx_frames{0};  ///< AVTP frames rx_poll delivered
};

/// The link under the stream data plane. Transmit and receive run on one thread
/// at a time; multicast membership and a concrete port's open/close run on the
/// reactor, the open/close while nothing transmits.
class StreamPort
{
  public:
    /// One received AVTP frame: @p payload is the AVTPDU, @p frame the whole
    /// L2 frame it came in. Both are valid during the call only.
    using RxFn = sg14::inplace_function<void(std::span<uint8_t const> payload, std::span<uint8_t const> frame), 32>;
    /// Sees every transmitted frame, L2 header and VLAN tag included, on the
    /// transmitting thread: it must not block or allocate.
    using TxTap = net::RawnetContext::TxTap;

    /// Where a received AVTPDU starts: after the 14-byte Ethernet header, or
    /// 18 under one 802.1Q tag. Every transmitted frame is tagged, so its
    /// AVTPDU starts at TX_PAYLOAD_OFFSET (tx_begin hands out that window).
    static constexpr size_t UNTAGGED_PAYLOAD_OFFSET = net::ETHERNET_HEADER_SIZE;
    static constexpr size_t TX_PAYLOAD_OFFSET = net::ETHERNET_HEADER_SIZE + net::VLAN_TAG_SIZE;
    /// The largest AVTPDU tx_begin hands out (the 1500-octet MTU).
    static constexpr size_t MAX_TX_PAYLOAD = net::MAX_ETHERNET_PAYLOAD;

    StreamPort() = default;
    StreamPort(StreamPort const&) = delete;
    auto operator=(StreamPort const&) -> StreamPort& = delete;
    StreamPort(StreamPort&&) = delete;
    auto operator=(StreamPort&&) -> StreamPort& = delete;
    virtual ~StreamPort() = default;

    /// A writable window of exactly @p capacity octets for the next AVTPDU, at TX_PAYLOAD_OFFSET;
    /// empty when the port cannot transmit or @p capacity exceeds MAX_TX_PAYLOAD. An uncommitted
    /// window is reused by the next tx_begin.
    [[nodiscard]] virtual auto tx_begin(size_t capacity) noexcept -> std::span<uint8_t> = 0;
    /// Send @p payload, a prefix of the last tx_begin window, to @p dest
    /// tagged with @p vid and @p pcp. Errors: the link's, or invalid_argument
    /// for a span that is not such a prefix.
    [[nodiscard]] virtual auto tx_commit(
        std::span<uint8_t const> payload, ieee::Eui48 const& dest, uint16_t vid, uint8_t pcp) noexcept -> Status = 0;
    /// Hand the frames committed since the last flush to the link: once per
    /// media tick, after the last commit. Errors: the link's.
    [[nodiscard]] virtual auto tx_flush() noexcept -> Status = 0;

    /// Bracket one media tick's receive and transmit (ring housekeeping);
    /// begin_cycle before the tick's rx_poll, end_cycle after its tx_flush.
    virtual void begin_cycle() noexcept = 0;
    [[nodiscard]] virtual auto end_cycle() noexcept -> Status = 0;

    /// Deliver up to @p max_frames received AVTP frames to @p fn, without
    /// blocking; returns how many were delivered.
    virtual auto rx_poll(RxFn const& fn, size_t max_frames) noexcept -> size_t = 0;
    /// The descriptor that becomes readable when a frame arrives, for a
    /// reactor-drained receive; -1 when there is none to poll.
    [[nodiscard]] virtual auto rx_fd() const noexcept -> int = 0;

    /// Receive frames sent to multicast group @p group / stop. Reactor thread.
    [[nodiscard]] virtual auto join_multicast(ieee::Eui48 const& group) noexcept -> Status = 0;
    [[nodiscard]] virtual auto leave_multicast(ieee::Eui48 const& group) noexcept -> Status = 0;

    /// Install @p tap on every transmitted frame (an empty tap removes it).
    virtual void set_tx_tap(TxTap tap) noexcept = 0;

    /// The source address of transmitted frames (zero before opened).
    [[nodiscard]] virtual auto hardware_address() const noexcept -> ieee::Eui48 = 0;
    /// Whether frames go to the link from the port's buffers without a copy.
    [[nodiscard]] virtual auto zero_copy() const noexcept -> bool = 0;

    /// The port's counters. Any thread.
    [[nodiscard]] auto stats() const noexcept -> StreamPortStats
    {
        return StreamPortStats{
            .tx_frames = tx_frames_.load(std::memory_order_relaxed),
            .tx_errors = tx_errors_.load(std::memory_order_relaxed),
            .rx_frames = rx_frames_.load(std::memory_order_relaxed)};
    }

  protected:
    /// Count one tx_commit (@p sent: the link took it). Transmitting thread.
    void count_tx(bool const sent) noexcept { bump(sent ? tx_frames_ : tx_errors_); }
    /// Count one delivered frame. Receiving thread.
    void count_rx() noexcept { bump(rx_frames_); }

  private:
    static void bump(std::atomic<uint64_t>& n) noexcept
    {
        n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    std::atomic<uint64_t> tx_frames_{0};
    std::atomic<uint64_t> tx_errors_{0};
    std::atomic<uint64_t> rx_frames_{0};
};

/// Write the 802.1Q-tagged Ethernet header of an AVTP frame from @p src to
/// @p dest into @p header (at least TX_PAYLOAD_OFFSET octets), as
/// net::RawnetContext::send_vlan builds it: TPID 0x8100, TCI pcp<<13 | vid.
void write_stream_header(
    std::span<uint8_t> header, ieee::Eui48 const& dest, ieee::Eui48 const& src, uint16_t vid, uint8_t pcp) noexcept;

/// Where @p frame's AVTPDU starts (UNTAGGED_PAYLOAD_OFFSET, or TX_PAYLOAD_OFFSET
/// under one 802.1Q tag), or 0 when it is not an AVTP frame.
[[nodiscard]] auto stream_payload_offset(std::span<uint8_t const> frame) noexcept -> size_t;

/// The StreamPort over AF_PACKET (macOS: BPF): a transmit-only qdisc-bypass
/// socket whose SO_PRIORITY picks the TX queue, and a receive socket that
/// takes only the stream subtypes. Each side opens on its own.
class PacketStreamPort final : public StreamPort
{
  public:
    PacketStreamPort() = default;
    ~PacketStreamPort() override = default;
    PacketStreamPort(PacketStreamPort const&) = delete;
    auto operator=(PacketStreamPort const&) -> PacketStreamPort& = delete;
    PacketStreamPort(PacketStreamPort&&) = delete;
    auto operator=(PacketStreamPort&&) -> PacketStreamPort& = delete;

    /// Open the transmit socket on @p interface_name and set its priority to
    /// @p priority, best effort (tx_priority_error). Reactor thread. Errors:
    /// the socket's (net::RawnetContext::open).
    [[nodiscard]] auto open_tx(std::string_view interface_name, uint8_t priority) -> Status;
    /// Open the receive socket on @p interface_name, filtered to the AM824,
    /// AAF and CRF subtypes; no group is joined. Reactor thread. Errors: the
    /// socket's.
    [[nodiscard]] auto open_rx(std::string_view interface_name) -> Status;
    /// Close both sockets / the receive socket. Reactor thread, while nothing
    /// transmits or receives.
    void close() noexcept;
    void close_rx() noexcept { rx_.close(); }

    /// Set the transmit socket's SO_PRIORITY: under PACKET_QDISC_BYPASS it
    /// picks the hardware TX queue (net::RawnetContext::set_priority).
    /// Errors: the setsockopt errno; bad_file_descriptor before open_tx.
    [[nodiscard]] auto set_tx_priority(int const priority) noexcept -> Status { return tx_.set_priority(priority); }
    /// Why open_tx could not set the priority it was given (empty: it did).
    [[nodiscard]] auto tx_priority_error() const noexcept -> std::error_code { return tx_priority_error_; }
    /// Whether the transmit / receive socket is open.
    [[nodiscard]] auto tx_open() const noexcept -> bool { return tx_.valid(); }
    [[nodiscard]] auto rx_open() const noexcept -> bool { return rx_.valid(); }

    [[nodiscard]] auto tx_begin(size_t capacity) noexcept -> std::span<uint8_t> override;
    [[nodiscard]] auto tx_commit(std::span<uint8_t const> payload, ieee::Eui48 const& dest, uint16_t vid, uint8_t pcp) noexcept
        -> Status override;
    /// Nothing to flush: tx_commit sends.
    [[nodiscard]] auto tx_flush() noexcept -> Status override { return success(); }
    void begin_cycle() noexcept override {}
    [[nodiscard]] auto end_cycle() noexcept -> Status override { return success(); }
    /// The received frame is passed with an untagged header rebuilt from the
    /// addresses the socket reports.
    auto rx_poll(RxFn const& fn, size_t max_frames) noexcept -> size_t override;
    [[nodiscard]] auto rx_fd() const noexcept -> int override { return rx_.fd(); }
    [[nodiscard]] auto join_multicast(ieee::Eui48 const& group) noexcept -> Status override { return rx_.join_multicast(group); }
    [[nodiscard]] auto leave_multicast(ieee::Eui48 const& group) noexcept -> Status override { return rx_.leave_multicast(group); }
    void set_tx_tap(TxTap tap) noexcept override { tx_.set_tx_tap(std::move(tap)); }
    [[nodiscard]] auto hardware_address() const noexcept -> ieee::Eui48 override { return tx_.my_mac(); }
    [[nodiscard]] auto zero_copy() const noexcept -> bool override { return false; }

  private:
    /// Receive one frame into @p frame behind a rebuilt untagged header: its payload
    /// length, 0 when none is waiting.
    [[nodiscard]] auto receive_into(std::span<uint8_t> frame) noexcept -> size_t;

    net::RawnetContext tx_{};
    net::RawnetContext rx_{};
    std::error_code tx_priority_error_{};
    /// The tx_begin window (send_vlan copies it behind the header).
    std::array<uint8_t, MAX_TX_PAYLOAD> tx_payload_{};
    /// A received frame: the rebuilt header, then the payload.
    std::array<uint8_t, net::MAX_ETHERNET_FRAME_SIZE_VLAN> rx_frame_{};
};

}  // namespace statusbar::avb_entity
