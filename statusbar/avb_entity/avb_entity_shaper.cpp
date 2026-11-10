// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_shaper.hpp"

#include "statusbar/avb_entity/avb_entity_kit.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/sg14/inplace_vector.h"

#if defined(__linux__)
#    include "statusbar/net/net_interface_info.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace statusbar::avb_entity {

auto ShaperErrorCategory::message(int const ev) const -> std::string
{
    switch (static_cast<ShaperError>(ev)) {
        case ShaperError::not_avb_capable:
            return "fewer than 3 TX queues: not AVB capable (do not reduce the AVB port's queues with ethtool -L)";
        case ShaperError::bad_queue_override:
            return "bad SR class queue override (out of range, shared, or leaving a non-contiguous legacy range)";
        case ShaperError::bad_priority:
            return "bad SR class or PTP priority (a PCP of 8 or more, a PTP priority of 16 or more, or two on one priority)";
        case ShaperError::too_many_streams:
            return "more class A streams than the shaper sizes for";
    }
    return "unknown shaper error";
}

auto shaper_error_category() noexcept -> std::error_category const&
{
    static ShaperErrorCategory const category;
    return category;
}

auto make_error_code(ShaperError const e) noexcept -> std::error_code
{
    return {static_cast<int>(e), shaper_error_category()};
}

//
// Queue layout
//

[[nodiscard]] auto is_intel_qav_driver(std::string_view const driver) noexcept -> bool
{
    return driver == "igb" || driver == "igc";
}

namespace {

/// The driver's SR class A and B queues of @p tx_queues: on igb/igc 0 and 1, or 1 and 0 with class B alone
/// (igc shapes queue 1 only after queue 0); else the top two, class A's highest.
[[nodiscard]] auto driver_class_queues(std::string_view const driver, uint16_t const tx_queues, SrClasses const classes) noexcept
    -> std::pair<uint16_t, uint16_t>
{
    if (!is_intel_qav_driver(driver)) {
        return {static_cast<uint16_t>(tx_queues - 1), static_cast<uint16_t>(tx_queues - 2)};
    }
    return classes == SrClasses::b ? std::pair<uint16_t, uint16_t>{1, 0} : std::pair<uint16_t, uint16_t>{0, 1};
}

/// The SR class A and B queues of @p tx_queues: the override's, else the driver's; in a layout without its
/// legacy range yet.
[[nodiscard]] auto class_queues(
    std::string_view const driver, uint16_t const tx_queues, QueueOverride const& override, SrClasses const classes)
    -> StatusValue<QueueLayout>
{
    auto const [driver_a, driver_b] = driver_class_queues(driver, tx_queues, classes);
    uint16_t const a = override.class_a.value_or(driver_a);
    uint16_t const b = override.class_b.value_or(driver_b);
    if (a >= tx_queues || b >= tx_queues || a == b) {
        return failure(ShaperError::bad_queue_override);
    }
    // igc shapes queue 1 only after queue 0, and queue 0 outranks it: class B below class A there, or alone.
    if (is_intel_qav_driver(driver) && (classes == SrClasses::b ? b != 0 : a == 1 && b == 0)) {
        return failure(ShaperError::bad_queue_override);
    }
    return QueueLayout{.queue_count = tx_queues, .class_a_queue = a, .class_b_queue = b};
}

/// The first queue from @p queue, moving by @p step, that is neither @p a nor @p b.
[[nodiscard]] constexpr auto skip_class_queues(uint16_t queue, int const step, uint16_t const a, uint16_t const b) noexcept
    -> uint16_t
{
    while (queue == a || queue == b) {
        queue = static_cast<uint16_t>(queue + step);
    }
    return queue;
}

/// @p layout with the legacy TC on every other queue, as one count@offset range.
[[nodiscard]] auto with_legacy_range(QueueLayout const& layout) -> StatusValue<QueueLayout>
{
    uint16_t const a = layout.class_a_queue;
    uint16_t const b = layout.class_b_queue;
    uint16_t const low = skip_class_queues(0, 1, a, b);
    uint16_t const high = skip_class_queues(static_cast<uint16_t>(layout.queue_count - 1), -1, a, b);
    if ((a > low && a < high) || (b > low && b < high)) {
        return failure(ShaperError::bad_queue_override);
    }
    return QueueLayout{
        .queue_count = layout.queue_count,
        .class_a_queue = a,
        .class_b_queue = b,
        .legacy_offset = low,
        .legacy_count = static_cast<uint16_t>(high - low + 1),
    };
}

/// @p layout with PTP on the legacy queue next to the SR classes, when that leaves a legacy queue.
[[nodiscard]] auto with_ptp_queue(QueueLayout layout) noexcept -> QueueLayout
{
    if (layout.legacy_count < 2) {
        return layout;
    }
    bool const above = layout.legacy_offset > std::min(layout.class_a_queue, layout.class_b_queue);
    layout.ptp_queue = above ? layout.legacy_offset : static_cast<uint16_t>(layout.legacy_offset + layout.legacy_count - 1);
    layout.legacy_offset = above ? static_cast<uint16_t>(layout.legacy_offset + 1) : layout.legacy_offset;
    layout.legacy_count = static_cast<uint16_t>(layout.legacy_count - 1);
    return layout;
}

}  // namespace

auto make_queue_layout(
    std::string_view const driver, uint16_t const tx_queues, QueueOverride const& override, SrClasses const classes)
    -> StatusValue<QueueLayout>
{
    if (tx_queues < MIN_AVB_TX_QUEUES) {
        return failure(ShaperError::not_avb_capable);
    }
    auto const layout = class_queues(driver, tx_queues, override, classes);
    if (!layout) {
        return forward_failure(layout);
    }
    return with_legacy_range(*layout).transform(with_ptp_queue);
}

namespace {

/// The SR class PCPs alone: both below 8 and distinct.
[[nodiscard]] auto validate_pcps(uint8_t const class_a_pcp, uint8_t const class_b_pcp) noexcept -> Status
{
    if (class_a_pcp >= 8 || class_b_pcp >= 8 || class_a_pcp == class_b_pcp) {
        return failure(ShaperError::bad_priority);
    }
    return success();
}

}  // namespace

auto validate_priorities(uint8_t const class_a_pcp, uint8_t const class_b_pcp, uint8_t const ptp_priority) noexcept -> Status
{
    if (auto status = validate_pcps(class_a_pcp, class_b_pcp); !status) {
        return status;
    }
    if (ptp_priority >= net::tc::PRIORITY_COUNT || ptp_priority == class_a_pcp || ptp_priority == class_b_pcp) {
        return failure(ShaperError::bad_priority);
    }
    return success();
}

#if defined(__linux__)
namespace {

/// PTP's traffic class on @p q, when @p layout has a PTP queue.
void add_ptp_class(net::tc::MqprioQdisc& q, QueueLayout const& layout, uint8_t const ptp_priority) noexcept
{
    if (!layout.ptp_queue) {
        return;
    }
    q.num_tc = PTP_TC + 1;
    q.prio_tc_map[ptp_priority] = PTP_TC;
    q.count[PTP_TC] = 1;
    q.offset[PTP_TC] = *layout.ptp_queue;
}

}  // namespace

auto mqprio_for(
    QueueLayout const& layout,
    uint16_t const major,
    uint8_t const class_a_pcp,
    uint8_t const class_b_pcp,
    uint8_t const ptp_priority,
    SrClasses const classes) noexcept -> StatusValue<net::tc::MqprioQdisc>
{  // NOLINT(statusbar-block-statements): one line per mqprio field
    auto const status =
        layout.ptp_queue ? validate_priorities(class_a_pcp, class_b_pcp, ptp_priority) : validate_pcps(class_a_pcp, class_b_pcp);
    if (!status) {
        return failure(status.error());
    }
    net::tc::MqprioQdisc q{};
    q.major = major;
    q.num_tc = 3;
    q.prio_tc_map.fill(LEGACY_TC);
    q.prio_tc_map[class_b_pcp] = runs_class_b(classes) ? CLASS_B_TC : LEGACY_TC;
    q.prio_tc_map[class_a_pcp] = runs_class_a(classes) ? CLASS_A_TC : LEGACY_TC;
    q.count[LEGACY_TC] = layout.legacy_count;
    q.offset[LEGACY_TC] = layout.legacy_offset;
    q.count[CLASS_B_TC] = 1;
    q.offset[CLASS_B_TC] = layout.class_b_queue;
    q.count[CLASS_A_TC] = 1;
    q.offset[CLASS_A_TC] = layout.class_a_queue;
    add_ptp_class(q, layout, ptp_priority);
    q.offload = false;
    return q;
}
#endif

//
// Credit-based shaper parameters - IEEE 802.1Q-2022 Annex L
//

namespace {

/// ceil(n / d) for d > 0 (C++ division truncates toward zero, which is the
/// ceiling for a negative quotient).
[[nodiscard]] constexpr auto ceil_div(int64_t const n, int64_t const d) noexcept -> int64_t
{
    return n >= 0 ? (n + d - 1) / d : n / d;
}

/// Sum of bandwidths and the largest wire frame of @p streams.
struct ClassLoad
{
    int64_t idleslope{0};
    uint32_t max_frame{0};
};

[[nodiscard]] auto class_load(std::span<TSpec const> const streams, uint32_t const intervals_per_sec) noexcept -> ClassLoad
{
    ClassLoad load{};
    for (auto const& tspec : streams) {
        load.idleslope += stream_bandwidth_kbps(tspec, intervals_per_sec);
        load.max_frame = std::max(load.max_frame, wire_frame_size(tspec.max_frame_size, tspec.clock_drift_bytes));
    }
    return load;
}

[[nodiscard]] auto clamp32(int64_t const v) noexcept -> int32_t
{
    return static_cast<int32_t>(std::clamp<int64_t>(v, INT32_MIN, INT32_MAX));
}

/// maxInterferenceSize (IEEE 802.1Q-2022 Annex L): an MTU frame's octets on the wire, counted as
/// a stream frame's are (wire_frame_size), so hiCredit and loCredit share one convention.
[[nodiscard]] auto interfering_wire_size(uint32_t const max_interfering_frame) noexcept -> int64_t
{
    return wire_frame_size(static_cast<uint16_t>(std::min<uint32_t>(max_interfering_frame, UINT16_MAX)));
}

}  // namespace

auto class_a_cbs(std::span<TSpec const> const streams, uint32_t const link_kbps, uint32_t const max_interfering_frame) noexcept
    -> CbsParameters
{
    if (streams.empty() || link_kbps == 0) {
        return {};
    }
    auto const load = class_load(streams, CLASS_A_INTERVALS_PER_SEC);
    int64_t const link = link_kbps;
    int64_t const sendslope = load.idleslope - link;
    return CbsParameters{
        .idleslope_kbps = clamp32(load.idleslope),
        .sendslope_kbps = clamp32(sendslope),
        .hicredit = clamp32(ceil_div(load.idleslope * interfering_wire_size(max_interfering_frame), link)),  // L-10
        .locredit = clamp32(ceil_div(sendslope * load.max_frame, link)),                                     // L-12
        .max_frame_size = load.max_frame,
    };
}

namespace {

/// Class B's hiCredit (IEEE 802.1Q-2022 Annex L, Equation L-11), in floating point.
[[nodiscard]] auto class_b_hicredit(
    int64_t const idleslope, CbsParameters const& class_a, uint32_t const link_kbps, uint32_t const max_interfering_frame) noexcept
    -> int64_t
{
    double const link = link_kbps;
    double const idle_a = class_a.idleslope_kbps;
    auto const interfering = static_cast<double>(interfering_wire_size(max_interfering_frame));
    return static_cast<int64_t>(std::ceil(
        static_cast<double>(idleslope) * ((interfering / (link - idle_a)) + (static_cast<double>(class_a.max_frame_size) / link))));
}

}  // namespace

auto class_b_cbs(
    std::span<TSpec const> const streams,
    CbsParameters const& class_a,
    uint32_t const link_kbps,
    uint32_t const max_interfering_frame) noexcept -> CbsParameters
{
    if (streams.empty() || link_kbps == 0 || class_a.idleslope_kbps >= static_cast<int64_t>(link_kbps)) {
        return {};
    }
    auto const load = class_load(streams, CLASS_B_INTERVALS_PER_SEC);
    int64_t const sendslope = load.idleslope - static_cast<int64_t>(link_kbps);
    return CbsParameters{
        .idleslope_kbps = clamp32(load.idleslope),
        .sendslope_kbps = clamp32(sendslope),
        .hicredit = clamp32(class_b_hicredit(load.idleslope, class_a, link_kbps, max_interfering_frame)),
        .locredit = clamp32(ceil_div(sendslope * load.max_frame, static_cast<int64_t>(link_kbps))),
        .max_frame_size = load.max_frame,
    };
}

auto placeholder_cbs(uint32_t const link_kbps, uint32_t const max_interfering_frame, int64_t const idleslope_kbps) noexcept
    -> CbsParameters
{
    if (link_kbps == 0) {
        return {};
    }
    int64_t const link = link_kbps;
    int64_t const idleslope = std::max<int64_t>({idleslope_kbps, link / 100, 1});
    int64_t const sendslope = idleslope - link;
    uint32_t const frame = wire_frame_size(static_cast<uint16_t>(std::min<uint32_t>(max_interfering_frame, UINT16_MAX)));
    return CbsParameters{
        .idleslope_kbps = clamp32(idleslope),
        .sendslope_kbps = clamp32(sendslope),
        .hicredit = clamp32(ceil_div(idleslope * interfering_wire_size(max_interfering_frame), link)),  // L-10
        .locredit = clamp32(ceil_div(sendslope * frame, link)),                                         // L-12
        .max_frame_size = frame,
        .placeholder = true,
    };
}

auto cbs_targets(
    std::span<TSpec const> const class_a_streams,
    std::span<TSpec const> const class_b_streams,
    uint32_t const link_kbps,
    uint32_t const max_interfering_frame) noexcept -> CbsTargets
{
    auto const a = class_a_cbs(class_a_streams, link_kbps, max_interfering_frame);
    auto const b = class_b_cbs(class_b_streams, a, link_kbps, max_interfering_frame);
    int64_t const budget = sr_budget_kbps(link_kbps) - a.idleslope_kbps - b.idleslope_kbps;
    int64_t const share = a.empty() && b.empty() ? budget / 2 : budget;
    return CbsTargets{
        .class_a = a.empty() ? placeholder_cbs(link_kbps, max_interfering_frame, share) : a,
        .class_b = b.empty() ? placeholder_cbs(link_kbps, max_interfering_frame, share) : b,
    };
}

auto class_a_targets(
    std::span<TSpec const> const class_a_streams, uint32_t const link_kbps, uint32_t const max_interfering_frame) noexcept
    -> CbsTargets
{
    auto const a = class_a_cbs(class_a_streams, link_kbps, max_interfering_frame);
    int64_t const placeholder = int64_t{link_kbps} * CLASS_A_PLACEHOLDER_PERCENT / 100;
    return CbsTargets{.class_a = a.empty() ? placeholder_cbs(link_kbps, max_interfering_frame, placeholder) : a};
}

auto class_b_targets(
    std::span<TSpec const> const class_b_streams, uint32_t const link_kbps, uint32_t const max_interfering_frame) noexcept
    -> CbsTargets
{
    auto const b = class_b_cbs(class_b_streams, CbsParameters{}, link_kbps, max_interfering_frame);
    int64_t const placeholder = int64_t{link_kbps} * CLASS_B_PLACEHOLDER_PERCENT / 100;
    return CbsTargets{.class_b = b.empty() ? placeholder_cbs(link_kbps, max_interfering_frame, placeholder) : b};
}

auto port_targets(
    SrClasses const classes,
    std::span<TSpec const> const class_a_streams,
    std::span<TSpec const> const class_b_streams,
    uint32_t const link_kbps,
    uint32_t const max_interfering_frame) noexcept -> CbsTargets
{
    switch (classes) {
        case SrClasses::a:
            return class_a_targets(class_a_streams, link_kbps, max_interfering_frame);
        case SrClasses::b:
            return class_b_targets(class_b_streams, link_kbps, max_interfering_frame);
        case SrClasses::both:
            break;
    }
    return cbs_targets(class_a_streams, class_b_streams, link_kbps, max_interfering_frame);
}

#if defined(__linux__)

//
// ShaperQdiscs
//

ShaperQdiscs::ShaperQdiscs(net::tc::TcControl& tc, logging::Logger const log) noexcept
    : tc_{tc}
    , log_{log}
{}

auto ShaperQdiscs::parent_of(uint16_t const queue) const noexcept -> uint32_t
{
    return net::tc::make_handle(major_, static_cast<uint16_t>(queue + 1));
}

auto ShaperQdiscs::install(
    int const ifindex, std::string_view const name, net::tc::MqprioQdisc const& mqprio, QueueLayout const& layout) -> Status
{  // NOLINT(statusbar-block-statements): the members a new root resets, one line each
    if (installed_) {
        return success();
    }
    auto const reused = put_root(ifindex, name, mqprio);
    if (!reused) {
        return forward_failure(reused);
    }
    ifindex_ = ifindex;
    major_ = mqprio.major;
    layout_ = layout;
    class_a_mapped_ = std::ranges::contains(mqprio.prio_tc_map, CLASS_A_TC);
    class_b_mapped_ = std::ranges::contains(mqprio.prio_tc_map, CLASS_B_TC);
    installed_ = true;
    reused_ = *reused;
    cbs_refused_a_ = cbs_refused_b_ = false;
    cbs_major_a_ = cbs_major_b_ = 0;
    class_a_ = {};
    class_b_ = {};
    etf_a_ = {};
    etf_error_ = {};
    return success();
}

auto ShaperQdiscs::put_root(int const ifindex, std::string_view const name, net::tc::MqprioQdisc const& mqprio) -> StatusValue<bool>
{
    uint32_t const root = net::tc::make_handle(mqprio.major, 0);
    // A root that already is ours, as we would lay it, stays: replacing it would destroy its cbs
    // children and reset igb/igc. The first program() changes its cbs in place.
    auto const current = tc_.get(ifindex, net::tc::ROOT);
    if (current && current->handle == root && current->has_mqprio_params && current->mqprio == mqprio) {
        log_.status("shaper: kept the mqprio root {:x}: already on {}", mqprio.major, logging::embed<16>(name));
        return true;
    }
    if (auto cleared = clear_foreign_root(ifindex, name, current, mqprio.major); !cleared) {
        return forward_failure(cleared);
    }
    if (auto replaced = replace_root(ifindex, name, mqprio, current); !replaced) {
        return forward_failure(replaced);
    }
    return false;
}

auto ShaperQdiscs::clear_foreign_root(
    int const ifindex, std::string_view const name, StatusValue<net::tc::QdiscInfo> const& current, uint16_t const major) -> Status
{
    if (current && current->kind_name() == "mqprio" && net::tc::handle_major(current->handle) != major) {
        clear_root_classes(ifindex, *current);
    }
    auto const removed = current ? net::tc::remove_other_tc_root(tc_, ifindex, *current, major) : StatusValue<bool>{false};
    if (removed && *removed) {
        log_.status(
            "shaper: removed the {} root {:x}: on {} before laying ours",
            logging::embed<16>(current->kind_name()),
            current->handle >> 16U,
            logging::embed<16>(name));
    }
    return removed.transform([](bool) {});
}

auto ShaperQdiscs::replace_root(
    int const ifindex,
    std::string_view const name,
    net::tc::MqprioQdisc const& mqprio,
    StatusValue<net::tc::QdiscInfo> const& current) -> Status
{
    auto replaced = tc_.replace_mqprio(ifindex, mqprio);
    if (replaced || replaced.error() != std::errc::invalid_argument) {
        return replaced;
    }
    // EINVAL: the root already holds an mqprio with our handle, which mqprio refuses to
    // change. Delete it, its cbs first, and retry once; a refused layout leaves our handle uninstalled.
    if (current) {
        clear_root_classes(ifindex, *current);
    }
    if (tc_.remove(ifindex, net::tc::make_handle(mqprio.major, 0), net::tc::ROOT)) {
        log_.status("shaper: replaced an existing mqprio root {:x}: on {}", mqprio.major, logging::embed<16>(name));
    }
    return tc_.replace_mqprio(ifindex, mqprio);
}

auto ShaperQdiscs::program(CbsTargets const& targets, uint32_t const link_mbps) -> Status
{
    if (!installed_) {
        return success();
    }
    link_mbps_ = link_mbps;
    auto status = program_classes(targets);
    // Class A's queue alone carries the AF_XDP socket's timed frames; class B's (AF_PACKET) gets none.
    // Told last, the etf settled: the listener may change the launch-time mode (set_launch_time()).
    if (auto const settled = settle_etf(layout_.class_a_queue, class_a_, etf_a_, logging::lit("A")); settled && on_etf_settled_) {
        on_etf_settled_(*settled);
    }
    return status;
}

auto ShaperQdiscs::program_classes(CbsTargets const& targets) -> Status
{
    // Class A's refusal ends both (no cbs offload); class B's waits for class A's cbs (igc enables queue 0's
    // first) and its own refusal leaves class A programmed.
    auto status_a = cbs_refused_a_
        ? success()
        : program_one(layout_.class_a_queue, targets.class_a, class_a_, logging::lit("A"), cbs_refused_a_);
    if (cbs_refused_a_ || !status_a) {
        return status_a;
    }
    return cbs_refused_b_ ? success()
                          : program_one(layout_.class_b_queue, targets.class_b, class_b_, logging::lit("B"), cbs_refused_b_);
}

void ShaperQdiscs::set_launch_time(LaunchTimeMode const mode, int32_t const delta_ns) noexcept
{
    if (mode != launch_time_mode_ || delta_ns != etf_delta_ns_) {
        etf_a_ = {};  // settled again under the new mode
        etf_error_ = {};
    }
    launch_time_mode_ = mode;
    etf_delta_ns_ = delta_ns;
}

auto ShaperQdiscs::settle_etf(uint16_t const queue, CbsParameters const& current, EtfState& state, logging::StrLit const label)
    -> std::optional<std::error_code>
{
    // Under our cbs once it is on the queue; with no cbs offload, on the queue's mqprio class.
    if (launch_time_mode_ == LaunchTimeMode::unsupported || state.settled || etf_error_ || (current.empty() && !cbs_refused_a_)) {
        return std::nullopt;
    }
    bool const on = launch_time_mode_ == LaunchTimeMode::on;
    auto const status = etf_slot(queue, !current.empty()).and_then([&](EtfSlot const& slot) {
        return on ? put_etf(slot, label) : clear_etf(slot, label);
    });
    if (!status) {
        // No etf offload (EOPNOTSUPP) or another refusal: frames on the queue go out at once; off, a
        // leftover etf stays (it drops AF_PACKET frames). Said once: no further etf until install().
        etf_error_ = status.error();
        log_.warning(
            "shaper: class {} etf on queue {} failed: errno {} {}: {}",
            label,
            queue,
            etf_error_.value(),
            logging::embed<32>(tc_.last_message()),
            on ? logging::lit("no launch time") : logging::lit("an earlier etf stays"));
        return on ? std::optional{etf_error_} : std::nullopt;
    }
    state = EtfState{.settled = true, .placed = on};
    return on ? std::optional{std::error_code{}} : std::nullopt;
}

auto ShaperQdiscs::etf_slot(uint16_t const queue, bool const under_cbs) -> StatusValue<EtfSlot>
{
    if (!under_cbs) {
        return child_of(parent_of(queue)).transform([&](std::optional<net::tc::QdiscInfo> child) {
            return EtfSlot{.parent = parent_of(queue), .child = child, .on_class = true};
        });
    }
    // cbs has one class, major:1 (sch_cbs.c), under the handle our cbs was sent with.
    uint32_t const parent = net::tc::make_handle(cbs_major(queue), 1);
    return child_of(parent).transform(
        [&](std::optional<net::tc::QdiscInfo> child) { return EtfSlot{.parent = parent, .child = child}; });
}

auto ShaperQdiscs::child_of(uint32_t const parent) -> StatusValue<std::optional<net::tc::QdiscInfo>>
{
    return net::tc::find_qdisc(tc_, ifindex_, parent);
}

auto ShaperQdiscs::put_etf(EtfSlot const& slot, logging::StrLit const label) -> Status
{
    net::tc::EtfQdisc const etf{.parent = slot.parent, .delta_ns = etf_delta_ns_, .offload = true, .skip_sock_check = true};
    if (slot.child && slot.child->has_etf_params && slot.child->etf == etf) {
        log_.status("shaper: class {} kept its etf (delta {} ns)", label, etf_delta_ns_);
        return success();
    }
    // etf has no change operation: another etf goes first; under a cbs neither resets igc.
    if (auto removed = slot.child && slot.child->kind_name() == "etf" ? drop_etf(slot, slot.child->handle) : Status{}; !removed) {
        return removed;
    }
    return tc_.replace_etf(ifindex_, etf).transform([&] {
        auto const where = slot.on_class ? logging::lit("on its queue (no cbs)") : logging::lit("under its cbs");
        log_.status("shaper: class {} etf {}: offloaded launch time, delta {} ns", label, where, etf_delta_ns_);
    });
}

auto ShaperQdiscs::clear_etf(EtfSlot const& slot, logging::StrLit const label) -> Status
{
    if (!slot.child || slot.child->kind_name() != "etf") {
        return success();
    }
    return drop_etf(slot, slot.child->handle).transform([&] {
        log_.status("shaper: class {} etf removed: launch time not wanted", label);
    });
}

auto ShaperQdiscs::drop_etf(EtfSlot const& slot, uint32_t const handle) -> Status
{
    // Deleting an mqprio class's child grafts noop_qdisc (mqprio_graft), which drops everything.
    return slot.on_class ? tc_.replace_kind(ifindex_, slot.parent, "pfifo_fast") : tc_.remove(ifindex_, handle, slot.parent);
}

auto ShaperQdiscs::program_one(
    uint16_t const queue, CbsParameters const& params, CbsParameters& current, logging::StrLit const label, bool& refused) -> Status
{
    if (params == current || params.empty()) {
        return success();
    }
    auto status = replace_class_cbs(queue, params);
    if (!status) {
        if (status.error() == std::errc::operation_not_supported || status.error() == std::errc::invalid_argument) {
            // No cbs offload on this queue (EOPNOTSUPP; igc answers EINVAL above queue 1). A software cbs
            // would never see the bypass frames, so this class stops asking: a state, not a failure.
            refused = true;
            log_.warning(
                "shaper: no cbs offload for class {} on queue {} ({}): its streams run unshaped",
                label,
                queue,
                logging::embed<32>(tc_.last_message()));
            return success();
        }
        log_.warning(
            "shaper: class {} cbs on queue {} ({:x}:{:x}) failed: errno {} {}",
            label,
            queue,
            major_,
            queue + 1,
            status.error().value(),
            logging::embed<32>(tc_.last_message()));
        return status;
    }
    current = params;
    log_.status(
        "shaper: class {} cbs on queue {}{}: idleslope {} sendslope {} hicredit {} locredit {} ({} Mbit/s)",
        label,
        queue,
        params.placeholder ? logging::lit(" (placeholder: none admitted)") : logging::lit(""),
        params.idleslope_kbps,
        params.sendslope_kbps,
        params.hicredit,
        params.locredit,
        link_mbps_);
    return success();
}

auto ShaperQdiscs::uninstall() -> Status
{
    if (!installed_) {
        return success();
    }
    installed_ = false;
    remove_cbs();
    return remove_root();
}

void ShaperQdiscs::clear_root_classes(int const ifindex, net::tc::QdiscInfo const& root)
{
    // The highest queue first (igc drops queue 1's cbs before queue 0's), best effort: a cbs still on
    // when the root goes would stay enabled in igc's hardware.
    uint16_t const major = net::tc::handle_major(root.handle);
    auto const queues = std::accumulate(root.mqprio.count.begin(), root.mqprio.count.begin() + root.mqprio.num_tc, 0U);
    for (auto queue = static_cast<uint16_t>(queues); queue > 0; --queue) {
        (void)tc_.replace_kind(ifindex, net::tc::make_handle(major, queue), "pfifo_fast");
    }
}

auto ShaperQdiscs::replace_class_cbs(uint16_t const queue, CbsParameters const& params) -> Status
{
    // Always a replace: a change in place on our cbs, a graft on the default qdisc (which resets
    // igb/igc only when it is the first cbs). See docs/SHAPER.md.
    return tc_.replace_cbs(
        ifindex_,
        net::tc::CbsQdisc{
            .parent = parent_of(queue),
            .offload = true,
            .hicredit = params.hicredit,
            .locredit = params.locredit,
            .idleslope_kbps = params.idleslope_kbps,
            .sendslope_kbps = params.sendslope_kbps,
            .handle_major = cbs_major(queue),
        });
}

auto ShaperQdiscs::cbs_major(uint16_t const queue) -> uint16_t
{
    auto& major = queue == layout_.class_a_queue ? cbs_major_a_ : cbs_major_b_;
    if (major == 0) {
        major = kept_cbs_major(queue).value_or(new_cbs_major(queue));
    }
    return major;
}

auto ShaperQdiscs::kept_cbs_major(uint16_t const queue) -> std::optional<uint16_t>
{
    // A kept root's cbs is changed in place by its own handle (no graft, no igc reset). A failed
    // lookup gives a new handle: a graft at worst, never a refusal.
    auto const child = reused_ ? child_of(parent_of(queue)) : StatusValue<std::optional<net::tc::QdiscInfo>>{};
    if (!child || !*child || (*child)->kind_name() != "cbs") {
        return std::nullopt;
    }
    return net::tc::handle_major((*child)->handle);
}

auto ShaperQdiscs::new_cbs_major(uint16_t const queue) const noexcept -> uint16_t
{
    // The root's major plus one plus the queue, below it instead where that would reach ffff:
    // (the ingress handle).
    unsigned const above = major_ + 1U + queue;
    return static_cast<uint16_t>(above < 0xFFFFU ? above : major_ - 1U - queue);
}

void ShaperQdiscs::remove_cbs()
{
    // Class B's queue first (an igc without the fix refuses to drop class A's cbs while B's is on), both queues
    // whatever was recorded, grafting the default qdisc (deleting an mqprio child leaves
    // noop_qdisc). See docs/SHAPER.md.
    for (uint16_t const queue : {layout_.class_b_queue, layout_.class_a_queue}) {
        if (queue == layout_.class_b_queue ? !class_b_mapped_ : !class_a_mapped_) {
            continue;  // nothing maps to it, so no cbs went there
        }
        if (auto status = tc_.replace_kind(ifindex_, parent_of(queue), "pfifo_fast"); !status) {
            log_.warning("shaper: removing the cbs on queue {} failed: errno {}", queue, status.error().value());
        }
    }
    class_a_ = {};
    class_b_ = {};
    cbs_major_a_ = cbs_major_b_ = 0;
    etf_a_ = {};
}

auto ShaperQdiscs::remove_root() -> Status
{
    auto status = tc_.remove(ifindex_, net::tc::make_handle(major_, 0), net::tc::ROOT);
    if (!status) {
        log_.warning("shaper: removing mqprio {:x} failed: errno {}", major_, status.error().value());
        return status;
    }
    log_.status("shaper: mqprio {:x} removed", major_);
    return success();
}

void ShaperQdiscs::release() noexcept
{
    installed_ = false;
    class_a_ = {};
    class_b_ = {};
}

//
// PortShaper
//

PortShaper::PortShaper(ShaperConfig config, logging::Logger const log, uint8_t const class_a_pcp)
    : config_{std::move(config)}
    , log_{log}
    , class_a_pcp_{class_a_pcp}
    , tc_{&client_}
    , qdiscs_{client_, log}
{}

PortShaper::PortShaper(ShaperConfig config, logging::Logger const log, net::tc::TcControl& tc, uint8_t const class_a_pcp)
    : config_{std::move(config)}
    , log_{log}
    , class_a_pcp_{class_a_pcp}
    , tc_{&tc}
    , qdiscs_{tc, log}
{}

void PortShaper::shutdown()
{
    if (!qdiscs_.installed()) {
        return;
    }
    dirty_ = false;
    if (config_.restore_qdisc) {
        (void)uninstall();  // logged
        return;
    }
    park();
}

void PortShaper::park()
{
    // Leave the root and the cbs for the next install(), parked at the
    // placeholders: no stream is ours any more, and taking the last cbs away
    // would reset an igb/igc adapter now and again at the next start.
    (void)qdiscs_.program(targets(false), link_mbps());  // logged
    qdiscs_.release();
    log_.status(
        "shaper: left mqprio {:x}: and its cbs on {} for the next start",
        config_.qdisc_major,
        logging::embed<16>(config_.interface_name));
}

namespace {

/// The interface @p name's index and driver; TX queues and MTU are left to size_port().
[[nodiscard]] auto identify_port(std::string_view const name) -> StatusValue<ShaperPort>
{
    auto const ifindex = net::interface_index(name);
    if (!ifindex) {
        return failure(ifindex.error());
    }
    auto driver = net::driver_name(name);
    if (!driver) {
        return failure(driver.error());
    }
    return ShaperPort{.ifindex = *ifindex, .driver = std::move(*driver)};
}

/// Fill @p port's TX queue count and MTU (1500 when unreadable) from the interface @p name.
[[nodiscard]] auto size_port(std::string_view const name, ShaperPort& port) -> Status
{
    auto const queues = net::tx_queue_count(name);
    if (!queues) {
        return failure(queues.error());
    }
    port.tx_queues = *queues;
    port.mtu = net::interface_mtu(name).value_or(1500U);
    return success();
}

}  // namespace

auto PortShaper::install() -> Status
{
    if (qdiscs_.installed()) {
        return success();
    }
    auto port = identify_port(config_.interface_name);
    if (!port) {
        return forward_failure(port);
    }
    if (auto status = size_port(config_.interface_name, *port); !status) {
        return status;
    }
    return install(*port);
}

auto PortShaper::install(ShaperPort const& port) -> Status
{
    if (qdiscs_.installed()) {
        return success();
    }
    auto const layout = make_queue_layout(port.driver, port.tx_queues, config_.queues, config_.classes);
    if (!layout) {
        return failure(layout.error());
    }
    if (auto status = install_layout(port, *layout); !status) {
        return status;
    }
    settle(port.driver, *layout);
    return success();
}

auto PortShaper::install_layout(ShaperPort const& port, QueueLayout const& layout) -> Status
{
    // A class the port does not run (Milan: class B) has its PCP on the legacy TC, so its queue carries nothing.
    auto const mqprio =
        mqprio_for(layout, config_.qdisc_major, class_a_pcp_, DEFAULT_CLASS_B_PCP, config_.ptp_priority, config_.classes);
    if (!mqprio) {
        return failure(mqprio.error());
    }
    mtu_ = port.mtu;
    if (tc_ == &client_) {
        if (auto status = client_.open(); !status) {
            return status;
        }
    }
    return qdiscs_.install(port.ifindex, config_.interface_name, *mqprio, layout);
}

void PortShaper::settle(std::string driver, QueueLayout const& layout)
{
    qdiscs_.set_launch_time(config_.launch_time, config_.etf_delta_ns);
    adopt(std::move(driver), layout);
    (void)refresh_link_speed();
    (void)reprogram();
}

void PortShaper::adopt(std::string driver, QueueLayout const& layout)
{
    driver_ = std::move(driver);
    layout_ = layout;
    log_.status(
        "shaper: mqprio {:x}: on {} ({}, {} TX queues): class A queue {}, class B queue {}, legacy {}@{}",
        config_.qdisc_major,
        logging::embed<16>(config_.interface_name),
        logging::embed<16>(driver_),
        layout_.queue_count,
        layout_.class_a_queue,
        layout_.class_b_queue,
        layout_.legacy_count,
        layout_.legacy_offset);
    if (layout_.ptp_queue) {
        log_.status("shaper: PTP event traffic (priority {}) on queue {}", config_.ptp_priority, *layout_.ptp_queue);
    }
    if (is_intel_qav_driver(driver_) && (layout_.class_a_queue > 1 || layout_.class_b_queue > 1)) {
        log_.warning(
            "shaper: {} offloads cbs only on queues 0 and 1; the override puts an SR class elsewhere", logging::embed<16>(driver_));
    }
}

namespace {

/// @p streams into @p out, those of one MaxFrameSize folded into one (MaxIntervalFrames summed); false when
/// they have more frame sizes than @p out holds.
[[nodiscard]] auto fold_tspecs(std::span<TSpec const> const streams, PortShaper::StreamTSpecs& out) noexcept -> bool
{
    for (auto const& tspec : streams) {
        auto* const same = std::ranges::find(out, tspec.max_frame_size, &TSpec::max_frame_size);
        if (same != out.end()) {
            same->max_interval_frames = static_cast<uint16_t>(same->max_interval_frames + tspec.max_interval_frames);
        } else if (out.try_push_back(tspec) == nullptr) {
            return false;
        }
    }
    return true;
}

}  // namespace

auto PortShaper::program(std::span<TSpec const> const class_a, std::span<TSpec const> const class_b) noexcept -> Status
{
    StreamTSpecs folded_a;
    StreamTSpecs folded_b;
    if (!fold_tspecs(class_a, folded_a) || !fold_tspecs(class_b, folded_b)) {
        return failure(make_error_code(ShaperError::too_many_streams));
    }
    assign_streams(folded_a, folded_b);
    return success();
}

void PortShaper::assign_streams(StreamTSpecs const& class_a, StreamTSpecs const& class_b) noexcept
{
    class_a_streams_ = class_a;
    class_b_streams_ = class_b;
    dirty_ = true;
    retry_at_ns_ = 0;  // a new set goes out at once
}

void PortShaper::set_launch_time(LaunchTimeMode const mode) noexcept
{
    config_.launch_time = mode;
    if (!qdiscs_.installed()) {
        return;
    }
    qdiscs_.set_launch_time(mode, config_.etf_delta_ns);
    dirty_ = true;  // tick() programs, the etf settled after the cbs
    retry_at_ns_ = 0;
}

void PortShaper::tick(int64_t const now_ns)
{
    if (!qdiscs_.installed()) {
        return;
    }
    if (now_ns >= next_link_check_ns_) {
        next_link_check_ns_ = now_ns + LINK_CHECK_NS;
        if (refresh_link_speed()) {
            dirty_ = true;
        }
    }
    if (dirty_ && now_ns >= retry_at_ns_) {
        if (reprogram()) {
            retry_backoff_ns_ = RETRY_MIN_NS;
        } else if (dirty_) {
            // A failed cbs request stays due: retry, backing off to
            // RETRY_MAX_NS (each attempt is logged).
            retry_at_ns_ = now_ns + retry_backoff_ns_;
            retry_backoff_ns_ = std::min(retry_backoff_ns_ * 2, RETRY_MAX_NS);
        }
    }
}

auto PortShaper::link_mbps() const noexcept -> uint32_t
{
    return known_link_mbps_ != 0 ? known_link_mbps_ : config_.fallback_link_mbps;
}

auto PortShaper::refresh_link_speed() -> bool
{
    auto const speed = net::link_speed_mbps(config_.interface_name);
    if (speed) {
        link_error_ = {};
        known_link_mbps_ = *speed;
    } else if (speed.error() != net::make_unknown_link_speed() && speed.error() != link_error_) {
        // Unknown speed (link down) keeps the last known one: a flap does not
        // churn the shaper. A real read error is logged once.
        link_error_ = speed.error();
        log_.warning("shaper: reading the link speed failed: errno {}", link_error_.value());
    }
    return link_mbps() != programmed_link_mbps_;
}

auto PortShaper::targets(bool const streams) -> CbsTargets
{
    // Each class the port runs: its streams' cbs, or with none its placeholder; a class it does not run, none.
    uint32_t const mbps = link_mbps();
    programmed_link_mbps_ = mbps;
    std::span<TSpec const> const class_a = streams ? std::span<TSpec const>{class_a_streams_} : std::span<TSpec const>{};
    std::span<TSpec const> const class_b = streams ? std::span<TSpec const>{class_b_streams_} : std::span<TSpec const>{};
    return port_targets(config_.classes, class_a, class_b, mbps * 1000U, mtu_);
}

auto PortShaper::reprogram() -> Status
{
    if (!qdiscs_.installed()) {
        return success();
    }
    dirty_ = false;
    auto status = qdiscs_.program(targets(true), link_mbps());
    if (!status && !qdiscs_.cbs_unsupported()) {
        dirty_ = true;  // tick() retries
    }
    return status;
}

auto PortShaper::uninstall() -> Status
{
    dirty_ = false;
    return qdiscs_.uninstall();
}

//
// EntityShaper
//

EntityShaper::EntityShaper(ShaperConfig config, AvbEntityKit& kit)
    : kit_{kit}
    , port_{std::move(config), kit.host().ctl_log(), kit.config().stream_pcp}
{}

EntityShaper::EntityShaper(ShaperConfig config, AvbEntityKit& kit, net::tc::TcControl& tc)
    : kit_{kit}
    , port_{std::move(config), kit.host().ctl_log(), tc, kit.config().stream_pcp}
{}

EntityShaper::~EntityShaper()
{
    unhook();
}

void EntityShaper::unhook() noexcept
{
    if (hooked_) {
        kit_.set_on_admission_changed({});
        hooked_ = false;
    }
}

void EntityShaper::shutdown()
{
    unhook();
    port_.shutdown();
}

auto EntityShaper::install() -> Status
{
    if (port_.installed()) {
        return success();
    }
    follow_admission();
    return installed_on(port_.install());
}

auto EntityShaper::install(ShaperPort const& port) -> Status
{
    if (port_.installed()) {
        return success();
    }
    follow_admission();
    return installed_on(port_.install(port));
}

auto EntityShaper::installed_on(Status status) -> Status
{
    if (!status) {
        unhook();
        return status;
    }
    kit_.set_link_mbps(port_.link_mbps());  // its class A budget
    return success();
}

void EntityShaper::follow_admission()
{
    // From here on the kit reports admission changes; tick() reprograms.
    kit_.set_on_admission_changed([this](std::span<uint16_t const> admitted) { on_admission_changed(admitted); });
    hooked_ = true;
    auto const admitted = kit_.admitted_talker_streams();
    admitted_.assign(admitted.begin(), admitted.end());
    program_admitted();
}

void EntityShaper::on_admission_changed(std::span<uint16_t const> const admitted) noexcept
{
    admitted_.clear();
    for (uint16_t const index : admitted) {
        (void)admitted_.try_push_back(index);
    }
    program_admitted();
}

void EntityShaper::program_admitted() noexcept
{
    sg14::inplace_vector<TSpec, MAX_ENTITY_STREAMS> class_a_streams{};
    for (uint16_t const index : admitted_) {
        for (auto const& spec : kit_.talker_specs()) {
            if (spec.index == index) {
                (void)class_a_streams.try_push_back(AvbEntityKit::talker_tspec(spec));
            }
        }
    }
    if (std::ranges::equal(class_a_streams, given_)) {
        return;  // a reprogram only for a change
    }
    given_ = class_a_streams;
    (void)port_.program(class_a_streams);  // MAX_ENTITY_STREAMS fits
}

void EntityShaper::tick(int64_t const now_ns)
{
    program_admitted();  // an admitted stream's format may have changed (gate off)
    port_.tick(now_ns);
    if (port_.installed()) {
        kit_.set_link_mbps(port_.link_mbps());  // its class A budget
    }
}

auto EntityShaper::reprogram() -> Status
{
    program_admitted();
    return port_.reprogram();
}

auto EntityShaper::uninstall() -> Status
{
    unhook();
    return port_.uninstall();
}

#endif  // __linux__

}  // namespace statusbar::avb_entity
