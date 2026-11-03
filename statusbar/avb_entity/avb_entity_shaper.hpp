#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// EntityShaper: the talker side's hardware traffic shaping (mqprio root, offloaded cbs on the
/// SR-class queue, SO_PRIORITY steering of the bypass stream socket). Reactor thread, Linux,
/// CAP_NET_ADMIN. See docs/SHAPER.md.

#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/logging/logging.hpp"
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
    bad_priority,         ///< an SR class PCP of 8 or more, or class A and class B on one PCP
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

/// Which TX queue each SR class takes on one interface and where the legacy
/// (best-effort) traffic class lies. Queues are 0-based, as the driver numbers
/// them; tc's per-queue classes are mqprio_major:(queue + 1).
struct QueueLayout
{
    uint16_t queue_count{0};
    uint16_t class_a_queue{0};
    uint16_t class_b_queue{0};
    uint16_t legacy_offset{0};  ///< first legacy queue
    uint16_t legacy_count{0};   ///< legacy queues: [legacy_offset, legacy_offset + legacy_count)

    auto operator==(QueueLayout const&) const noexcept -> bool = default;
};

/// Whether @p driver is one of Intel's Qav drivers (igb: i210; igc: i225/i226),
/// which shape only queues 0 and 1 and give the lowest queue the highest
/// priority, so the SR classes take the bottom queues.
[[nodiscard]] auto is_intel_qav_driver(std::string_view driver) noexcept -> bool;

/// The queue layout for @p driver with @p tx_queues TX queues (docs/SHAPER.md, "Queue order per
/// driver"); an explicit @p override wins. Errors: ShaperError::not_avb_capable below
/// MIN_AVB_TX_QUEUES; ShaperError::bad_queue_override.
[[nodiscard]] auto make_queue_layout(std::string_view driver, uint16_t tx_queues, QueueOverride const& override = {})
    -> StatusValue<QueueLayout>;

/// The default SR class priorities (PCPs): class A 3, class B 2 (802.1Q, Milan).
inline constexpr uint8_t DEFAULT_CLASS_A_PCP = 3;
inline constexpr uint8_t DEFAULT_CLASS_B_PCP = 2;

/// mqprio traffic classes: TC0 legacy, TC1 class B, TC2 class A.
inline constexpr uint8_t LEGACY_TC = 0;
inline constexpr uint8_t CLASS_B_TC = 1;
inline constexpr uint8_t CLASS_A_TC = 2;

/// Whether @p class_a_pcp and @p class_b_pcp can key an mqprio map: both
/// below 8 and distinct. Errors: ShaperError::bad_priority.
[[nodiscard]] auto validate_priorities(uint8_t class_a_pcp, uint8_t class_b_pcp) noexcept -> Status;

#if defined(__linux__)
/// The mqprio root for @p layout with handle @p major:0: num_tc 3, @p class_a_pcp and
/// @p class_b_pcp on their SR classes' TCs, every other priority on TC0, hw 0.
/// Errors: ShaperError::bad_priority (validate_priorities).
[[nodiscard]] auto mqprio_for(
    QueueLayout const& layout,
    uint16_t major,
    uint8_t class_a_pcp = DEFAULT_CLASS_A_PCP,
    uint8_t class_b_pcp = DEFAULT_CLASS_B_PCP) noexcept -> StatusValue<net::tc::MqprioQdisc>;
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

/// The octets a stream frame occupies on the wire for bandwidth purposes:
/// max(MaxFrameSize + VLAN + L2, 68) + L1 + 1 (one octet for clock drift),
/// as SwiftMRP's calcFrameSize / calculateBandwidthUsed (non-nominal) count it.
[[nodiscard]] constexpr auto wire_frame_size(uint16_t const max_frame_size) noexcept -> uint32_t
{
    uint32_t frame = uint32_t{max_frame_size} + VLAN_OVERHEAD + L2_OVERHEAD;
    if (frame < MIN_L2_FRAME) {
        frame = MIN_L2_FRAME;
    }
    return frame + L1_OVERHEAD + 1;
}

/// A stream's reserved bandwidth in kbit/s, rounded up:
/// wire_frame_size * 8 * max_interval_frames * @p intervals_per_sec / 1000.
[[nodiscard]] constexpr auto stream_bandwidth_kbps(TSpec const& tspec, uint32_t const intervals_per_sec) noexcept -> uint32_t
{
    uint64_t const bits_per_sec =
        uint64_t{wire_frame_size(tspec.max_frame_size)} * 8U * tspec.max_interval_frames * intervals_per_sec;
    return static_cast<uint32_t>((bits_per_sec + 999U) / 1000U);
}

/// One SR class's cbs settings (tc's units: kbit/s and octets). All zero
/// (empty()) when the class has no admitted stream: remove its shaper.
struct CbsParameters
{
    int32_t idleslope_kbps{0};
    int32_t sendslope_kbps{0};
    int32_t hicredit{0};
    int32_t locredit{0};
    uint32_t max_frame_size{0};  ///< the class's largest wire_frame_size (Annex L's maxFrameSize)

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

// ---------------------------------------------------------------------------
// EntityShaper (reactor thread, Linux)
// ---------------------------------------------------------------------------

struct ShaperConfig
{
    std::string interface_name;
    QueueOverride queues{};
    /// The mqprio root's handle major (`tc qdisc show` prints it as "9000:").
    /// A root already holding this handle is taken over (deleted and replaced).
    uint16_t qdisc_major{0x9000};
    /// The link speed assumed while the kernel has never reported one.
    uint32_t fallback_link_mbps{1000};
    /// shutdown() removes the mqprio (the kernel restores the default root);
    /// false leaves it, and its cbs, in place.
    bool restore_qdisc{true};
};

#if defined(__linux__)

/// The kit's hardware shaper: a tick-only net::Pollable on the reactor (docs/SHAPER.md,
/// "Threading"). The kit must outlive it. After install() every failure is logged on the kit's ctl
/// log, never fatal: the streams keep flowing, unshaped.
class EntityShaper final : public net::Pollable
{
  public:
    EntityShaper(ShaperConfig config, AvbEntityKit& kit);

    EntityShaper(EntityShaper const&) = delete;
    auto operator=(EntityShaper const&) -> EntityShaper& = delete;
    EntityShaper(EntityShaper&&) = delete;
    auto operator=(EntityShaper&&) -> EntityShaper& = delete;
    ~EntityShaper() override;

    /// Errors: the interface's, ShaperError::not_avb_capable, bad_queue_override or bad_priority,
    /// or the netlink errno (last_kernel_message() has the kernel's reason). Nothing of ours is left
    /// installed on failure.
    [[nodiscard]] auto install() -> Status;
    /// Record @p admitted (talker stream indices: the kit's admission hook
    /// argument) and mark the shaper dirty for the next tick(). Cheap; safe
    /// inside the kit's ACMP/MSRP callbacks.
    void on_admission_changed(std::span<uint16_t const> admitted) noexcept;
    /// Recompute and program the cbs for the recorded admitted set and the
    /// current link speed. A no-op success before install(). Errors: the
    /// first netlink failure (each is logged).
    auto reprogram() -> Status;
    /// Unregister the admission hook and delete the mqprio root. A no-op
    /// success when not installed.
    [[nodiscard]] auto uninstall() -> Status;
    /// Unregister the admission hook, then uninstall() if config.restore_qdisc
    /// (failures are logged). Idempotent.
    void shutdown();

    // net::Pollable: tick-only.
    [[nodiscard]] auto fd() const noexcept -> int override { return -1; }
    void on_ready(int64_t /*now_ns*/) override {}
    void tick(int64_t now_ns) override;
    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

    [[nodiscard]] auto installed() const noexcept -> bool { return installed_; }
    [[nodiscard]] auto layout() const noexcept -> QueueLayout const& { return layout_; }
    [[nodiscard]] auto driver() const noexcept -> std::string const& { return driver_; }
    /// What is programmed now (empty: no cbs on that class's queue).
    [[nodiscard]] auto class_a() const noexcept -> CbsParameters const& { return class_a_; }
    [[nodiscard]] auto class_b() const noexcept -> CbsParameters const& { return class_b_; }
    /// The link speed the shaper sizes against (Mbit/s).
    [[nodiscard]] auto link_mbps() const noexcept -> uint32_t;
    [[nodiscard]] auto last_kernel_message() const noexcept -> std::string const& { return client_.last_message(); }

    /// How often tick() re-reads the link speed.
    static constexpr int64_t LINK_CHECK_NS = 1'000'000'000;

  private:
    /// Put @p params on @p queue (replace), or the default qdisc back when empty.
    [[nodiscard]] auto program(uint16_t queue, CbsParameters const& params, CbsParameters& current, logging::StrLit label)
        -> Status;
    /// Read the link speed; true if the speed the shaper sizes against changed.
    auto refresh_link_speed() -> bool;
    /// Clear the kit's admission hook if this shaper set it.
    void unhook() noexcept;

    ShaperConfig config_;
    AvbEntityKit& kit_;
    net::tc::TcClient client_{};
    int ifindex_{0};
    std::string driver_{};
    QueueLayout layout_{};
    uint32_t mtu_{1500};
    uint32_t known_link_mbps_{0};   ///< last speed the kernel reported (0: never)
    std::error_code link_error_{};  ///< last non-"unknown" speed read error (logged on change)
    uint32_t programmed_link_mbps_{0};
    sg14::inplace_vector<uint16_t, MAX_ENTITY_STREAMS> admitted_{};
    bool dirty_{false};
    int64_t next_link_check_ns_{0};
    bool installed_{false};
    bool hooked_{false};
    CbsParameters class_a_{};
    CbsParameters class_b_{};
};

#endif  // __linux__

}  // namespace statusbar::avb_entity
