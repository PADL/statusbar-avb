#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// PortShaper / EntityShaper: the talker side's hardware traffic shaping (mqprio root, offloaded cbs per
/// SR-class queue, PTP event traffic on its own queue, an offloaded etf under the stream socket's for launch
/// time, SO_PRIORITY steering of the bypass stream socket). Reactor thread, Linux, CAP_NET_ADMIN. See docs/SHAPER.md.

#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/net/net_tc_launch_time.hpp"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/status.hpp"

#if defined(__linux__)
#    include "statusbar/net/net_message_reactor.hpp"
#    include "statusbar/net/net_tc_qdisc.hpp"
#endif

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace statusbar::avb_entity {

class AvbEntityKit;

/// Why the shaper refused an interface or a queue override.
enum class ShaperError
{
    not_avb_capable = 1,  ///< fewer than 3 TX queues (legacy + class B + class A)
    bad_queue_override,   ///< an override queue out of range, both classes on one queue, or a legacy range that is not contiguous
    bad_priority,         ///< an SR class PCP of 8 or more, a PTP priority of 16 or more, or two classes on one priority
    too_many_streams,     ///< more class A streams than MAX_PORT_SHAPER_STREAMS
};

class ShaperErrorCategory final : public std::error_category
{
  public:
    [[nodiscard]] auto name() const noexcept -> char const* override { return "statusbar.avb_entity.shaper"; }
    [[nodiscard]] auto message(int ev) const -> std::string override;
};

[[nodiscard]] auto shaper_error_category() noexcept -> std::error_category const&;
[[nodiscard]] auto make_error_code(ShaperError e) noexcept -> std::error_code;

}  // namespace statusbar::avb_entity

template <>
struct std::is_error_code_enum<statusbar::avb_entity::ShaperError> : std::true_type
{};

namespace statusbar::avb_entity {

// ---------------------------------------------------------------------------
// Queue layout (pure)
// ---------------------------------------------------------------------------

/// The fewest TX queues an AVB port needs: one legacy, one per SR class.
inline constexpr uint16_t MIN_AVB_TX_QUEUES = 3;

/// Explicit SR-class queues (0-based TX queue numbers), overriding the driver
/// policy. Either may be given alone; the other then follows the policy.
struct QueueOverride
{
    std::optional<uint16_t> class_a{};
    std::optional<uint16_t> class_b{};

    auto operator==(QueueOverride const&) const noexcept -> bool = default;
};

/// Which TX queue each SR class takes on one interface, where PTP event traffic goes and where the
/// legacy (best-effort) traffic class lies. Queues are 0-based, as the driver numbers them; tc's
/// per-queue classes are mqprio_major:(queue + 1).
struct QueueLayout
{
    uint16_t queue_count{0};
    uint16_t class_a_queue{0};
    uint16_t class_b_queue{0};
    uint16_t legacy_offset{0};            ///< first legacy queue
    uint16_t legacy_count{0};             ///< legacy queues: [legacy_offset, legacy_offset + legacy_count)
    std::optional<uint16_t> ptp_queue{};  ///< PTP event traffic's own queue (none: it is legacy traffic)

    auto operator==(QueueLayout const&) const noexcept -> bool = default;
};

/// Whether @p driver is one of Intel's Qav drivers (igb: i210; igc: i225/i226),
/// which shape only queues 0 and 1 and give the lowest queue the highest
/// priority, so the SR classes take the bottom queues.
[[nodiscard]] auto is_intel_qav_driver(std::string_view driver) noexcept -> bool;

/// The queue layout for @p driver with @p tx_queues TX queues (docs/SHAPER.md, "Queue order per
/// driver"); an explicit @p override wins. With two legacy queues or more, PTP takes the one next to
/// the SR classes. Errors: ShaperError::not_avb_capable below MIN_AVB_TX_QUEUES; bad_queue_override.
[[nodiscard]] auto make_queue_layout(std::string_view driver, uint16_t tx_queues, QueueOverride const& override = {})
    -> StatusValue<QueueLayout>;

/// The default SR class priorities (PCPs): class A 3, class B 2 (802.1Q, Milan).
inline constexpr uint8_t DEFAULT_CLASS_A_PCP = 3;
inline constexpr uint8_t DEFAULT_CLASS_B_PCP = 2;

/// The skb priority PTP event traffic is sent with (ptp4l's socket_priority).
inline constexpr uint8_t DEFAULT_PTP_PRIORITY = net::tc::PTP_PRIORITY;

/// mqprio traffic classes: TC0 legacy, TC1 class B, TC2 class A, TC3 PTP (with a PTP queue).
inline constexpr uint8_t LEGACY_TC = 0;
inline constexpr uint8_t CLASS_B_TC = 1;
inline constexpr uint8_t CLASS_A_TC = 2;
inline constexpr uint8_t PTP_TC = 3;

/// Whether @p class_a_pcp, @p class_b_pcp and @p ptp_priority can key an mqprio map: the PCPs below 8,
/// the PTP priority below 16, all three distinct. Errors: ShaperError::bad_priority.
[[nodiscard]] auto validate_priorities(
    uint8_t class_a_pcp, uint8_t class_b_pcp, uint8_t ptp_priority = DEFAULT_PTP_PRIORITY) noexcept -> Status;

#if defined(__linux__)
/// The mqprio root for @p layout with handle @p major:0, hw 0: @p class_a_pcp and @p class_b_pcp on their
/// SR classes' TCs, @p ptp_priority on PTP_TC when the layout has a PTP queue (num_tc 4, else 3, the PTP
/// priority unchecked), every other priority on TC0. Errors: ShaperError::bad_priority.
[[nodiscard]] auto mqprio_for(
    QueueLayout const& layout,
    uint16_t major,
    uint8_t class_a_pcp = DEFAULT_CLASS_A_PCP,
    uint8_t class_b_pcp = DEFAULT_CLASS_B_PCP,
    uint8_t ptp_priority = DEFAULT_PTP_PRIORITY) noexcept -> StatusValue<net::tc::MqprioQdisc>;
#endif

// ---------------------------------------------------------------------------
// Credit-based shaper parameters, IEEE 802.1Q Annex L (pure)
// ---------------------------------------------------------------------------

/// Per-frame media overhead on an 802.3 port (802.1Q 35.2.2.8.3: 42 octets).
inline constexpr uint32_t VLAN_OVERHEAD = 4;  ///< the 802.1Q tag
inline constexpr uint32_t L2_OVERHEAD = 18;   ///< Ethernet header + FCS
inline constexpr uint32_t L1_OVERHEAD = 20;   ///< preamble + SFD + inter-packet gap
/// Avnu ProAV Bridge 9.1: count at least a 68-octet frame (before L1).
inline constexpr uint32_t MIN_L2_FRAME = 68;
/// Class measurement intervals as frames per second (125 us, 250 us).
inline constexpr uint32_t CLASS_A_INTERVALS_PER_SEC = 8000;
inline constexpr uint32_t CLASS_B_INTERVALS_PER_SEC = 4000;

/// The octets a stream frame occupies on the wire (Milan v1.2 Table 4.4): F = MaxFrameSize + 22,
/// at least 68, plus 20. The clock-drift byte is already in MaxFrameSize (srp_max_frame_size).
[[nodiscard]] constexpr auto wire_frame_size(uint16_t const max_frame_size) noexcept -> uint32_t
{
    uint32_t frame = uint32_t{max_frame_size} + VLAN_OVERHEAD + L2_OVERHEAD;
    if (frame < MIN_L2_FRAME) {
        frame = MIN_L2_FRAME;
    }
    return frame + L1_OVERHEAD;
}

/// A stream's reserved bandwidth in kbit/s, rounded up:
/// wire_frame_size * 8 * max_interval_frames * @p intervals_per_sec / 1000
/// (Milan v1.2 Table 4.4's "Bandwidth (kbps)" column at class A).
[[nodiscard]] constexpr auto stream_bandwidth_kbps(TSpec const& tspec, uint32_t const intervals_per_sec) noexcept -> uint32_t
{
    uint64_t const bits_per_sec =
        uint64_t{wire_frame_size(tspec.max_frame_size)} * 8U * tspec.max_interval_frames * intervals_per_sec;
    return static_cast<uint32_t>((bits_per_sec + 999U) / 1000U);
}

/// One SR class's cbs settings (tc's units: kbit/s and octets). All zero
/// (empty()) when the class has no admitted stream: cbs_targets() then puts
/// the class's placeholder_cbs() in its place.
struct CbsParameters
{
    int32_t idleslope_kbps{0};
    int32_t sendslope_kbps{0};
    int32_t hicredit{0};
    int32_t locredit{0};
    uint32_t max_frame_size{0};  ///< the class's largest wire_frame_size (Annex L's maxFrameSize)
    bool placeholder{false};     ///< placeholder_cbs(): no admitted stream, the cbs only keeps the queue shaped

    [[nodiscard]] auto empty() const noexcept -> bool { return idleslope_kbps == 0; }
    auto operator==(CbsParameters const&) const noexcept -> bool = default;
};

/// Class A's shaper for @p streams on a @p link_kbps link with @p max_interfering_frame octets of
/// interference (IEEE 802.1Q-2022 Annex L, L-10 and L-12). Empty when @p streams is.
[[nodiscard]] auto class_a_cbs(std::span<TSpec const> streams, uint32_t link_kbps, uint32_t max_interfering_frame) noexcept
    -> CbsParameters;

/// Class B's shaper (IEEE 802.1Q-2022 Annex L, L-11 and L-12; class A's idleSlope and largest
/// frame count as interference). Empty when @p streams is.
[[nodiscard]] auto class_b_cbs(
    std::span<TSpec const> streams, CbsParameters const& class_a, uint32_t link_kbps, uint32_t max_interfering_frame) noexcept
    -> CbsParameters;

/// The share of the link the SR classes may reserve together (802.1Q 34.3.1's
/// default deltaBandwidth for class A, which class B shares).
inline constexpr uint32_t SR_BANDWIDTH_PERCENT = 75;
/// SR_BANDWIDTH_PERCENT of a @p link_kbps link, in kbit/s.
[[nodiscard]] constexpr auto sr_budget_kbps(uint32_t const link_kbps) noexcept -> int64_t
{
    return int64_t{link_kbps} * SR_BANDWIDTH_PERCENT / 100;
}

/// The cbs a class keeps while it has no admitted stream, at @p idleslope_kbps (at least 1% of the
/// link), so igb/igc never switch transmit mode while running. Empty for a zero link.
[[nodiscard]] auto placeholder_cbs(uint32_t link_kbps, uint32_t max_interfering_frame, int64_t idleslope_kbps) noexcept
    -> CbsParameters;

/// What both SR-class queues carry.
struct CbsTargets
{
    CbsParameters class_a{};
    CbsParameters class_b{};

    auto operator==(CbsTargets const&) const noexcept -> bool = default;
};

/// class_a_cbs and class_b_cbs for the admitted streams, each empty class replaced by a
/// placeholder_cbs sharing what the streams leave of the SR budget (docs/SHAPER.md). Neither class
/// is ever empty for a non-zero link.
[[nodiscard]] auto cbs_targets(
    std::span<TSpec const> class_a_streams,
    std::span<TSpec const> class_b_streams,
    uint32_t link_kbps,
    uint32_t max_interfering_frame) noexcept -> CbsTargets;

// ---------------------------------------------------------------------------
// Launch time: an offloaded etf under class A's cbs (tc-etf(8); net_tc_launch_time.hpp)
// ---------------------------------------------------------------------------

/// What the shaper does with etf under class A's cbs (net::tc::LaunchTimeMode), the mode from the
/// driver's one answer, and etf's default delta.
using net::tc::DEFAULT_ETF_DELTA_NS;
using net::tc::launch_time_mode;
using net::tc::LaunchTimeMode;

// ---------------------------------------------------------------------------
// ShaperQdiscs (the requests, Linux)
// ---------------------------------------------------------------------------

#if defined(__linux__)

/// The qdisc requests behind EntityShaper over a net::tc::TcControl (a recorder in tests). Every
/// SR-class queue keeps a cbs from the first program() on, changed only in place; only uninstall()
/// removes it (docs/SHAPER.md, "Adapter resets").
class ShaperQdiscs
{
  public:
    ShaperQdiscs(net::tc::TcControl& tc, logging::Logger log) noexcept;

    /// Lay the mqprio root down on @p ifindex for @p layout. An identical root is kept with its cbs;
    /// anything else is replaced, and EINVAL (our handle on another layout) deletes and retries
    /// once. Errors: the netlink errno.
    [[nodiscard]] auto install(int ifindex, std::string_view name, net::tc::MqprioQdisc const& mqprio, QueueLayout const& layout)
        -> Status;
    /// Put @p targets on the SR-class queues, class A's first, each only when changed; always a cbs
    /// replace. EOPNOTSUPP is logged once and stops further cbs requests. Errors: the first netlink
    /// failure (each is logged).
    auto program(CbsTargets const& targets, uint32_t link_mbps) -> Status;
    /// etf under class A's cbs as @p mode says, from the next program() on, settled once per install()
    /// and again after each change of mode (put or removed under the cbs); offloaded, CLOCK_TAI,
    /// @p delta_ns, skip_sock_check. Unsupported until set.
    void set_launch_time(LaunchTimeMode mode, int32_t delta_ns = DEFAULT_ETF_DELTA_NS) noexcept;
    /// Take everything away: pfifo_fast on class B's queue, then class A's, then delete the mqprio
    /// root (one adapter reset on igb/igc). A no-op success when not installed.
    [[nodiscard]] auto uninstall() -> Status;
    /// Forget the installation and leave the qdiscs as they are.
    void release() noexcept;

    [[nodiscard]] auto installed() const noexcept -> bool { return installed_; }
    /// Whether install() kept a root that was already in place.
    [[nodiscard]] auto reused() const noexcept -> bool { return reused_; }
    /// Whether the driver refused cbs offload (EOPNOTSUPP).
    [[nodiscard]] auto cbs_unsupported() const noexcept -> bool { return cbs_unsupported_; }
    /// The mode set_launch_time() gave.
    [[nodiscard]] auto launch_time_mode() const noexcept -> LaunchTimeMode { return launch_time_mode_; }
    /// Whether our etf is on class A's queue (LaunchTimeMode::on, settled).
    [[nodiscard]] auto launch_time() const noexcept -> bool { return etf_a_.placed; }
    /// The etf request that failed (logged; no further etf requests until the next install()).
    [[nodiscard]] auto etf_error() const noexcept -> std::error_code { return etf_error_; }
    /// What is programmed now (empty: no cbs of ours on that class's queue).
    [[nodiscard]] auto class_a() const noexcept -> CbsParameters const& { return class_a_; }
    [[nodiscard]] auto class_b() const noexcept -> CbsParameters const& { return class_b_; }

  private:
    /// One class queue's etf: settled for this install(), and ours on it.
    struct EtfState
    {
        bool settled{false};
        bool placed{false};
    };
    /// Where an etf goes on a class queue: our cbs's class, and what is on it now (nullopt: its default).
    struct EtfSlot
    {
        uint32_t parent{0};
        std::optional<net::tc::QdiscInfo> child{};
    };

    [[nodiscard]] auto program_one(uint16_t queue, CbsParameters const& params, CbsParameters& current, logging::StrLit label)
        -> Status;
    /// program_one() for class A, then (unless cbs offload was refused) class B: the first failure.
    [[nodiscard]] auto program_classes(CbsTargets const& targets) -> Status;
    /// Bring class @p label's queue @p queue to the launch-time mode once our cbs is on it (@p current).
    void settle_etf(uint16_t queue, CbsParameters const& current, EtfState& state, logging::StrLit label);
    /// The etf slot under our cbs on @p queue. Errors: the netlink errno (ENOENT: no cbs there).
    [[nodiscard]] auto etf_slot(uint16_t queue) -> StatusValue<EtfSlot>;
    /// The qdisc on class @p parent (nullopt: none reported, its default). Errors: the netlink errno.
    [[nodiscard]] auto child_of(uint32_t parent) -> StatusValue<std::optional<net::tc::QdiscInfo>>;
    /// Our etf on @p slot, kept when it is already there, else put in place of what is.
    [[nodiscard]] auto put_etf(EtfSlot const& slot, logging::StrLit label) -> Status;
    /// No etf on @p slot: an etf found there is deleted.
    [[nodiscard]] auto clear_etf(EtfSlot const& slot, logging::StrLit label) -> Status;
    [[nodiscard]] auto parent_of(uint16_t queue) const noexcept -> uint32_t;
    /// Lay @p mqprio as the root on @p ifindex, keeping one already laid so: whether it was kept.
    [[nodiscard]] auto put_root(int ifindex, std::string_view name, net::tc::MqprioQdisc const& mqprio) -> StatusValue<bool>;
    /// Delete @p current, the root, if it is another handle's mqprio or taprio (net::tc::remove_other_tc_root),
    /// which replaced in place would clear our TC map as it goes. Errors: the netlink errno.
    [[nodiscard]] auto take_over(int ifindex, std::string_view name, StatusValue<net::tc::QdiscInfo> const& current, uint16_t major)
        -> Status;
    /// Replace the root with @p mqprio, deleting our own handle @p root and retrying once on EINVAL.
    [[nodiscard]] auto replace_root(int ifindex, std::string_view name, net::tc::MqprioQdisc const& mqprio, uint32_t root)
        -> Status;
    /// Graft the default qdisc back on both class queues (logged) and forget their cbs.
    void remove_cbs();
    /// Delete the mqprio root (logged).
    [[nodiscard]] auto remove_root() -> Status;

    net::tc::TcControl& tc_;
    logging::Logger log_;
    int ifindex_{0};
    uint16_t major_{0};
    QueueLayout layout_{};
    uint32_t link_mbps_{0};
    bool installed_{false};
    bool reused_{false};
    bool cbs_unsupported_{false};
    CbsParameters class_a_{};
    CbsParameters class_b_{};
    LaunchTimeMode launch_time_mode_{LaunchTimeMode::unsupported};
    int32_t etf_delta_ns_{DEFAULT_ETF_DELTA_NS};
    EtfState etf_a_{};
    std::error_code etf_error_{};
};

#endif  // __linux__

// ---------------------------------------------------------------------------
// PortShaper and EntityShaper (reactor thread, Linux)
// ---------------------------------------------------------------------------

struct ShaperConfig
{
    std::string interface_name;
    QueueOverride queues{};
    /// The mqprio root's handle major (`tc qdisc show` prints it as "9000:").
    /// A root already holding this handle with our layout is kept; with
    /// another, it is taken over (deleted and replaced).
    uint16_t qdisc_major{0x9000};
    /// The link speed assumed while the kernel has never reported one.
    uint32_t fallback_link_mbps{1000};
    /// shutdown() removes the mqprio and its cbs (an adapter reset on igb/igc). false (the default)
    /// parks both cbs at their placeholders for the next install() to keep.
    bool restore_qdisc{false};
    /// etf under class A's cbs (launch_time_mode()): on only with the streams on an AF_XDP socket on
    /// class A's queue carrying launch times, as etf drops AF_PACKET frames; off removes a leftover one.
    LaunchTimeMode launch_time{LaunchTimeMode::unsupported};
    /// The etf's delta (ns): it bounds frames that pass through etf; AF_XDP frames bypass it.
    int32_t etf_delta_ns{DEFAULT_ETF_DELTA_NS};
    /// The skb priority mapped to the PTP queue (the layout's ptp_queue): ptp4l's socket_priority.
    uint8_t ptp_priority{DEFAULT_PTP_PRIORITY};
};

/// The most class A streams a PortShaper sizes its shaper for.
inline constexpr size_t MAX_PORT_SHAPER_STREAMS = 64;

/// What PortShaper::install() reads from the interface: its index, driver, TX queues and MTU.
struct ShaperPort
{
    int ifindex{0};
    std::string driver{};
    uint16_t tx_queues{0};
    uint32_t mtu{1500};
};

#if defined(__linux__)

/// One AVB port's hardware shaper, sized from the class A TSpecs its user programs: a tick-only
/// net::Pollable on the reactor (docs/SHAPER.md, "Threading"). After install() every failure is
/// logged on @p log, never fatal: the streams keep flowing, unshaped.
class PortShaper final : public net::Pollable
{
  public:
    /// Over its own rtnetlink client, class A's frames on PCP @p class_a_pcp.
    PortShaper(ShaperConfig config, logging::Logger log, uint8_t class_a_pcp = DEFAULT_CLASS_A_PCP);
    /// Over @p tc (a recorder in tests), which must outlive it.
    PortShaper(ShaperConfig config, logging::Logger log, net::tc::TcControl& tc, uint8_t class_a_pcp = DEFAULT_CLASS_A_PCP);

    PortShaper(PortShaper const&) = delete;
    auto operator=(PortShaper const&) -> PortShaper& = delete;
    PortShaper(PortShaper&&) = delete;
    auto operator=(PortShaper&&) -> PortShaper& = delete;
    ~PortShaper() override = default;

    /// Read the interface (index, driver, TX queues, MTU) and install(ShaperPort) on it. Errors: the
    /// interface's, then install(ShaperPort)'s.
    [[nodiscard]] auto install() -> Status;
    /// Lay the mqprio root down on @p port and program the TSpecs given so far. Errors:
    /// ShaperError::not_avb_capable, bad_queue_override or bad_priority, or the netlink errno
    /// (last_kernel_message() has the kernel's reason). Nothing of ours is left installed on failure.
    [[nodiscard]] auto install(ShaperPort const& port) -> Status;
    /// Size class A for @p class_a, every stream that may send on its queue, from the next tick()
    /// (before install(): from install()). Errors: ShaperError::too_many_streams; nothing changes.
    [[nodiscard]] auto program(std::span<TSpec const> class_a) noexcept -> Status;
    /// Another launch-time mode (launch time on or off at run time, as the system clock agrees with the
    /// PHC): settled by the next tick(), under cbs that stay (no adapter reset). A no-op before install().
    void set_launch_time(LaunchTimeMode mode) noexcept;
    /// Program the cbs for the TSpecs given and the current link speed now. A no-op success before
    /// install(). Errors: the first netlink failure (each is logged).
    auto reprogram() -> Status;
    /// Take both cbs away and delete the mqprio root. A no-op success when not installed.
    [[nodiscard]] auto uninstall() -> Status;
    /// uninstall() if config.restore_qdisc, else program the placeholders and leave the qdiscs in
    /// place (failures are logged). Idempotent.
    void shutdown();

    // net::Pollable: tick-only.
    [[nodiscard]] auto fd() const noexcept -> int override { return -1; }
    void on_ready(int64_t /*now_ns*/) override {}
    void tick(int64_t now_ns) override;
    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

    [[nodiscard]] auto installed() const noexcept -> bool { return qdiscs_.installed(); }
    [[nodiscard]] auto layout() const noexcept -> QueueLayout const& { return layout_; }
    [[nodiscard]] auto driver() const noexcept -> std::string const& { return driver_; }
    /// What is programmed now (empty: no cbs on that class's queue).
    [[nodiscard]] auto class_a() const noexcept -> CbsParameters const& { return qdiscs_.class_a(); }
    [[nodiscard]] auto class_b() const noexcept -> CbsParameters const& { return qdiscs_.class_b(); }
    /// The link speed the shaper sizes against (Mbit/s).
    [[nodiscard]] auto link_mbps() const noexcept -> uint32_t;
    [[nodiscard]] auto last_kernel_message() const noexcept -> std::string const& { return tc_->last_message(); }
    /// config.launch_time, as install() gave it to the qdiscs.
    [[nodiscard]] auto launch_time_mode() const noexcept -> LaunchTimeMode { return qdiscs_.launch_time_mode(); }
    /// Whether our etf is on class A's queue.
    [[nodiscard]] auto launch_time() const noexcept -> bool { return qdiscs_.launch_time(); }

    /// How often tick() re-reads the link speed.
    static constexpr int64_t LINK_CHECK_NS = 1'000'000'000;
    /// tick()'s retry of a failed reprogram(): after 1 s, doubling to 32 s.
    static constexpr int64_t RETRY_MIN_NS = 1'000'000'000;
    static constexpr int64_t RETRY_MAX_NS = 32'000'000'000;

  private:
    /// cbs_targets() for the TSpecs given (@p streams false: none) at the current link speed.
    [[nodiscard]] auto targets(bool streams) -> CbsTargets;
    /// Read the link speed; true if the speed the shaper sizes against changed.
    auto refresh_link_speed() -> bool;
    /// Park both cbs at the placeholders and leave the qdiscs for the next start (logged).
    void park();
    /// Open the tc client (our own) and install the mqprio root for @p layout on @p port.
    [[nodiscard]] auto install_layout(ShaperPort const& port, QueueLayout const& layout) -> Status;
    /// The root is down: settle launch time, keep @p driver and @p layout, program the TSpecs given.
    void settle(std::string driver, QueueLayout const& layout);
    /// Keep and log the installed @p driver and @p layout, warning about what will not shape.
    void adopt(std::string driver, QueueLayout const& layout);

    ShaperConfig config_;
    logging::Logger log_;
    uint8_t class_a_pcp_;
    net::tc::TcClient client_{};
    net::tc::TcControl* tc_;  ///< client_, or the one given
    ShaperQdiscs qdiscs_;
    std::string driver_{};
    QueueLayout layout_{};
    uint32_t mtu_{1500};
    uint32_t known_link_mbps_{0};   ///< last speed the kernel reported (0: never)
    std::error_code link_error_{};  ///< last non-"unknown" speed read error (logged on change)
    uint32_t programmed_link_mbps_{0};
    sg14::inplace_vector<TSpec, MAX_PORT_SHAPER_STREAMS> class_a_streams_{};
    bool dirty_{false};
    int64_t next_link_check_ns_{0};
    int64_t retry_at_ns_{0};
    int64_t retry_backoff_ns_{RETRY_MIN_NS};
};

/// The kit's hardware shaper: a PortShaper programmed from the kit's admitted talker streams, the
/// kit's stream PCP as class A's, and its link speed handed back for the class A budget. The kit
/// must outlive it.
class EntityShaper final : public net::Pollable
{
  public:
    EntityShaper(ShaperConfig config, AvbEntityKit& kit);
    /// Over @p tc (a recorder in tests), which must outlive it.
    EntityShaper(ShaperConfig config, AvbEntityKit& kit, net::tc::TcControl& tc);

    EntityShaper(EntityShaper const&) = delete;
    auto operator=(EntityShaper const&) -> EntityShaper& = delete;
    EntityShaper(EntityShaper&&) = delete;
    auto operator=(EntityShaper&&) -> EntityShaper& = delete;
    ~EntityShaper() override;

    /// PortShaper::install(), then follow the kit's admission changes. Errors: PortShaper::install()'s.
    [[nodiscard]] auto install() -> Status;
    /// The same on a given @p port (PortShaper::install(ShaperPort)).
    [[nodiscard]] auto install(ShaperPort const& port) -> Status;
    /// Record @p admitted (the kit's admission hook argument) for the next tick(), which also follows
    /// an admitted stream's format change. Cheap; safe inside the kit's ACMP/MSRP callbacks.
    void on_admission_changed(std::span<uint16_t const> admitted) noexcept;
    /// See PortShaper::set_launch_time.
    void set_launch_time(LaunchTimeMode mode) noexcept { port_.set_launch_time(mode); }
    /// Recompute and program the cbs for the recorded admitted set and the
    /// current link speed. A no-op success before install(). Errors: the
    /// first netlink failure (each is logged).
    auto reprogram() -> Status;
    /// Unregister the admission hook, take both cbs away and delete the
    /// mqprio root. A no-op success when not installed.
    [[nodiscard]] auto uninstall() -> Status;
    /// Unregister the admission hook, then PortShaper::shutdown(). Idempotent.
    void shutdown();

    // net::Pollable: tick-only.
    [[nodiscard]] auto fd() const noexcept -> int override { return -1; }
    void on_ready(int64_t /*now_ns*/) override {}
    void tick(int64_t now_ns) override;
    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

    [[nodiscard]] auto installed() const noexcept -> bool { return port_.installed(); }
    [[nodiscard]] auto layout() const noexcept -> QueueLayout const& { return port_.layout(); }
    [[nodiscard]] auto driver() const noexcept -> std::string const& { return port_.driver(); }
    [[nodiscard]] auto class_a() const noexcept -> CbsParameters const& { return port_.class_a(); }
    [[nodiscard]] auto class_b() const noexcept -> CbsParameters const& { return port_.class_b(); }
    [[nodiscard]] auto link_mbps() const noexcept -> uint32_t { return port_.link_mbps(); }
    [[nodiscard]] auto last_kernel_message() const noexcept -> std::string const& { return port_.last_kernel_message(); }
    [[nodiscard]] auto launch_time_mode() const noexcept -> LaunchTimeMode { return port_.launch_time_mode(); }
    [[nodiscard]] auto launch_time() const noexcept -> bool { return port_.launch_time(); }

    static constexpr int64_t LINK_CHECK_NS = PortShaper::LINK_CHECK_NS;
    static constexpr int64_t RETRY_MIN_NS = PortShaper::RETRY_MIN_NS;
    static constexpr int64_t RETRY_MAX_NS = PortShaper::RETRY_MAX_NS;

  private:
    /// Give the port the TSpecs of the recorded admitted streams when they differ from the last
    /// given. Every talker stream of the kit is class A (MSRP sr_class_id 6 only).
    void program_admitted() noexcept;
    /// The port's install returned @p status: on success, hand back the link speed and warn
    /// about the stream socket's priority; on failure, unhook.
    [[nodiscard]] auto installed_on(Status status) -> Status;
    /// Clear the kit's admission hook if this shaper set it.
    void unhook() noexcept;
    /// Hook the kit's admission changes and record the admitted set now.
    void follow_admission();

    AvbEntityKit& kit_;
    PortShaper port_;
    sg14::inplace_vector<uint16_t, MAX_ENTITY_STREAMS> admitted_{};
    sg14::inplace_vector<TSpec, MAX_ENTITY_STREAMS> given_{};  ///< the TSpecs last given to the port
    bool hooked_{false};
};

#endif  // __linux__

}  // namespace statusbar::avb_entity
