#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// XdpStreamPort: the StreamPort over one AF_XDP socket on the SR class A NIC queue (queue 0 on
/// igb/igc); see docs/STREAM_PORTS.md. Without libxdp, or off Linux, open() fails with
/// not_supported_on_platform. The libbpf-facing net::XdpContext stays in the .cpp.

#include "statusbar/avb_entity/avb_entity_stream_port.hpp"
#include "statusbar/avb_entity/avb_entity_stream_transport.hpp"
#include "statusbar/net/net_compiled_rule.hpp"
#include "statusbar/net/net_ethernet_port.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace statusbar::net {
class XdpContext;
struct XdpConfig;
struct XdpSocketStats;
enum class XdpBindMode : uint8_t;
enum class XdpAttachMode : uint8_t;
}  // namespace statusbar::net

namespace statusbar::avb_entity {

/// What XdpStreamPort::open() takes past the interface.
struct XdpStreamPortOptions
{
    /// The NIC queue the socket binds (the SR class A queue).
    uint32_t queue_id{0};
    /// net::XdpBindMode::auto_ (the zero value).
    net::XdpBindMode bind_mode{};
    /// How a fresh attach attaches the program (net::XdpConfig::attach_mode);
    /// net::XdpAttachMode::native (the zero value).
    net::XdpAttachMode attach_mode{};
    /// Where the program is pinned and adopted from; empty: not pinned.
    std::string pin_path{};
    /// Frames carry their launch time (net::XdpConfig::tx_launch_time). The caller has found the driver
    /// honours it (launch_time_support()); open() still goes on without it when this build lacks it.
    bool launch_time{false};
};

/// The options for @p mode on @p interface_name's queue @p queue_id: auto_ and
/// zerocopy attach natively, copy generically; pinned under
/// net::XdpProgram::pin_path_for(@p interface_name), so a pinned program is adopted.
[[nodiscard]] auto xdp_stream_port_options(StreamXdpMode mode, std::string_view interface_name, uint32_t queue_id)
    -> XdpStreamPortOptions;

/// Whether AF_XDP frames can carry launch times, given whether the driver honours them (@p driver:
/// net::netdev::launch_time_supported(), asked once). Errors: operation_not_supported (the driver),
/// the query's, or net::XdpError::tx_metadata_not_supported (this build: libxdp before 1.5).
[[nodiscard]] auto launch_time_support(StatusValue<bool> const& driver) noexcept -> Status;

/// Register a small UMEM with TX metadata and let it go, before anything is attached to an interface:
/// what a kernel before 6.11 refuses (tx_metadata_not_supported); the build's refusal first.
[[nodiscard]] auto probe_tx_metadata() -> Status;

/// @p options with launch_time off when @p probe (-> Status, e.g. probe_tx_metadata) refuses, the
/// reason into @p launch_time_error: the port then opens once, attaching once, as the kernel allows.
template <typename Probe>
[[nodiscard]] auto launch_time_as_probed(XdpStreamPortOptions options, Probe const& probe, std::error_code& launch_time_error)
    -> XdpStreamPortOptions
{
    if (!options.launch_time) {
        return options;
    }
    if (auto const probed = probe(); !probed) {
        launch_time_error = probed.error();
        options.launch_time = false;
    }
    return options;
}

/// One AF_XDP socket, both ways. tx_begin/tx_commit/tx_flush, rx_poll and the
/// cycle run on one thread only (the media thread): the rings have one producer
/// and one consumer.
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
    /// Consecutive refused TX kicks that put the port in error (tx_error()).
    static constexpr uint32_t TX_FAILING_AFTER = 8;

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

    /// Refused TX kicks, the last errno, and whether transmit is failing
    /// (internal; public for tests). record() on the transmitting thread only.
    class TxHealth
    {
      public:
        /// One kick's result: refused with @p error (an errno) unless @p ok.
        void record(bool const ok, int const error) noexcept
        {
            if (ok) {
                in_a_row_ = 0;
                if (failing_.load(std::memory_order_relaxed)) {
                    failing_.store(false, std::memory_order_relaxed);
                }
                return;
            }
            errno_.store(error, std::memory_order_relaxed);
            failures_.store(failures_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            if (++in_a_row_ >= TX_FAILING_AFTER && !failing_.load(std::memory_order_relaxed)) {
                failing_.store(true, std::memory_order_relaxed);
            }
        }
        /// Forget every kick (open()). Not while a kick is recorded.
        void reset() noexcept
        {
            in_a_row_ = 0;
            failures_.store(0, std::memory_order_relaxed);
            errno_.store(0, std::memory_order_relaxed);
            failing_.store(false, std::memory_order_relaxed);
        }
        /// Kicks refused since the last reset. Any thread.
        [[nodiscard]] auto failures() const noexcept -> uint64_t { return failures_.load(std::memory_order_relaxed); }
        /// The last refusal's errno as an error code (none before the first). Any thread.
        [[nodiscard]] auto error() const noexcept -> std::error_code
        {
            int const value = errno_.load(std::memory_order_relaxed);
            return value != 0 ? std::error_code{value, std::system_category()} : std::error_code{};
        }
        /// TX_FAILING_AFTER refusals in a row, and none accepted since. Any thread.
        [[nodiscard]] auto failing() const noexcept -> bool { return failing_.load(std::memory_order_relaxed); }

      private:
        uint32_t in_a_row_{0};
        std::atomic<uint64_t> failures_{0};
        std::atomic<int> errno_{0};
        std::atomic<bool> failing_{false};
    };

    /// The launch times one socket's frames carry, in commit order (internal; public for tests). igc's
    /// launch-time queue is a FIFO, so they never decrease; an untimed frame (0) goes at now(). next() on
    /// the transmitting thread; the counters from any thread.
    class LaunchOrder
    {
      public:
        /// A wanted time this far behind the last is a clock step, not cross-slot rounding (nor a launch
        /// lead, at most half this): the floor goes.
        static constexpr uint64_t STEP_NS = 1'000'000;
        /// A frame held back by more than this (a class A packet) is counted: by design, two slots on one
        /// socket differ by under a packet.
        static constexpr uint64_t CLAMP_TOLERANCE_NS = 125'000;

        /// The time for a frame asked for at @p requested_ns (0: untimed, @p now() in gPTP ns instead):
        /// at least the last one given, unless it is more than STEP_NS behind (a backward step).
        template <typename Now>
        [[nodiscard]] auto next(uint64_t const requested_ns, Now const& now) noexcept -> uint64_t
        {
            uint64_t const wanted = requested_ns != 0 ? requested_ns : now();
            if (wanted >= last_ns_ || last_ns_ - wanted > STEP_NS) {
                count_step(wanted < last_ns_);
                last_ns_ = wanted;
                return wanted;
            }
            count_clamp(last_ns_ - wanted);
            return last_ns_;
        }
        /// Forget the last time (open()); the counters stay.
        void reset() noexcept { last_ns_ = 0; }
        /// Frames held back to the last time by more than CLAMP_TOLERANCE_NS, the most one was held (ns),
        /// and floors dropped for a step (STEP_NS): each means trouble, not the design's rounding.
        [[nodiscard]] auto clamped() const noexcept -> uint64_t { return clamped_.load(std::memory_order_relaxed); }
        [[nodiscard]] auto max_clamp_ns() const noexcept -> uint64_t { return max_clamp_ns_.load(std::memory_order_relaxed); }
        [[nodiscard]] auto steps() const noexcept -> uint64_t { return steps_.load(std::memory_order_relaxed); }

      private:
        void count_clamp(uint64_t const ns) noexcept
        {
            if (ns <= CLAMP_TOLERANCE_NS) {
                return;
            }
            clamped_.store(clamped_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            if (ns > max_clamp_ns_.load(std::memory_order_relaxed)) {
                max_clamp_ns_.store(ns, std::memory_order_relaxed);
            }
        }
        void count_step(bool const stepped) noexcept
        {
            if (stepped) {
                steps_.store(steps_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            }
        }

        uint64_t last_ns_{0};
        std::atomic<uint64_t> clamped_{0};
        std::atomic<uint64_t> max_clamp_ns_{0};
        std::atomic<uint64_t> steps_{0};
    };

    /// The XdpConfig open() uses on @p interface_name: UMEM_FRAMES frames, a
    /// FILL_RING fill ring, a ring's worth of completions a cycle, @p rules, no per-frame
    /// clock, no drain budget (rx_poll bounds it) and @p options, both by view.
    [[nodiscard]] static auto config_for(
        std::string_view interface_name, XdpStreamPortOptions const& options, std::span<net::CompiledRule<32> const> rules) noexcept
        -> net::XdpConfig;

    XdpStreamPort();
    ~XdpStreamPort() override;
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
    [[nodiscard]] auto is_open() const noexcept -> bool;
    /// The queue the socket is bound to (open()'s).
    [[nodiscard]] auto queue_id() const noexcept -> uint32_t { return queue_id_; }
    /// The mode the program runs in, and whether open() adopted a pinned one.
    [[nodiscard]] auto attach_mode() const noexcept -> net::XdpAttachMode;
    [[nodiscard]] auto adopted() const noexcept -> bool;
    /// Whether the program is pinned (by open() or adopted from the pins); false
    /// while closed, without a pin path, or when pinning failed (no bpffs).
    [[nodiscard]] auto pinned() const noexcept -> bool;
    /// The bind mode open() asked for.
    [[nodiscard]] auto bind_mode() const noexcept -> net::XdpBindMode;
    /// Whether committed frames carry their launch time (opened with options.launch_time, supported).
    [[nodiscard]] auto launch_time() const noexcept -> bool;
    /// Why the last open() asking for launch time went on without it (empty: it did not).
    [[nodiscard]] auto launch_time_error() const noexcept -> std::error_code { return launch_time_error_; }
    /// The socket's counters. On the thread that drains the port only.
    [[nodiscard]] auto socket_stats() const noexcept -> net::XdpSocketStats const&;
    /// The frames held back for transmit, the uncommitted tx_begin frame not
    /// counted (internal; public for tests).
    [[nodiscard]] auto tx_pool_size() const noexcept -> size_t { return tx_pool_.size(); }
    /// tx_begin calls that found no pooled frame. Any thread.
    [[nodiscard]] auto tx_pool_empty_count() const noexcept -> uint64_t { return tx_pool_empty_.load(std::memory_order_relaxed); }
    /// While TX_FAILING_AFTER kicks in a row have been refused (until one is
    /// not): the last refusal's errno; else none. Any thread.
    [[nodiscard]] auto tx_error() const noexcept -> std::error_code
    {
        return tx_health_.failing() ? tx_health_.error() : std::error_code{};
    }
    /// The kick history behind tx_error(). Any thread.
    [[nodiscard]] auto tx_health() const noexcept -> TxHealth const& { return tx_health_; }
    /// The launch-time ordering's counters (frames clamped, the largest clamp, steps). Any thread.
    [[nodiscard]] auto launch_order() const noexcept -> LaunchOrder const& { return launch_order_; }

    /// The window at TX_PAYLOAD_OFFSET of a pooled UMEM frame; empty when the
    /// port is closed, the pool is empty or @p capacity exceeds MAX_TX_PAYLOAD.
    [[nodiscard]] auto tx_begin(size_t capacity) noexcept -> std::span<uint8_t> override;
    /// Write the tagged header in front of @p payload, tap the frame and put it on the TX ring; with
    /// launch_time(), due at LaunchOrder::next(@p launch_time_ns). A full ring fails (tx_ring_full), the
    /// frame back in the pool; a span not in the window: invalid_argument.
    [[nodiscard]] auto tx_commit(
        std::span<uint8_t const> payload, ieee::Eui48 const& dest, uint16_t vid, uint8_t pcp, uint64_t launch_time_ns) noexcept
        -> Status override;
    /// One kick when the kernel asks for it (need_wakeup); a transient refusal
    /// (net::detail::is_transient_kick_error) is not one. Errors: tx_flush_failed.
    [[nodiscard]] auto tx_flush() noexcept -> Status override;
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
    [[nodiscard]] auto hardware_address() const noexcept -> ieee::Eui48 override;
    /// True only under a native program with a zero-copy bind.
    [[nodiscard]] auto zero_copy() const noexcept -> bool override;

  private:
    void refill_tx_pool() noexcept;
    /// Open the socket, once, with the AVTP stream rules as @p options say, launch time only where the
    /// build and the kernel allow it (with_launch_time).
    [[nodiscard]] auto open_socket(std::string_view interface_name, XdpStreamPortOptions const& options) -> Status;
    /// @p options with launch_time kept only where the build and the kernel take TX metadata
    /// (probe_tx_metadata; else recorded in launch_time_error()); a fresh LaunchOrder.
    [[nodiscard]] auto with_launch_time(XdpStreamPortOptions const& options) -> XdpStreamPortOptions;
    /// Commit @p slot's first @p length octets (a written frame) due at @p launch_time_ns, counting it and tapping it.
    [[nodiscard]] auto commit_slot(net::EthernetTxSlot const& slot, size_t length, uint64_t launch_time_ns) noexcept -> Status;

    std::unique_ptr<net::XdpContext> context_;  ///< never null
    TxPool tx_pool_{};
    std::optional<net::EthernetTxSlot> tx_pending_{};  ///< the tx_begin frame not yet committed
    size_t tx_window_{0};                              ///< the last tx_begin capacity
    TxTap tx_tap_{};
    TxHealth tx_health_{};
    std::atomic<uint64_t> tx_pool_empty_{0};
    uint32_t queue_id_{0};
    std::error_code launch_time_error_{};
    LaunchOrder launch_order_{};
};

}  // namespace statusbar::avb_entity
