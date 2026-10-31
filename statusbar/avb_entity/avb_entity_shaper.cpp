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
            return "bad SR class priority (a PCP of 8 or more, or class A and class B on one PCP)";
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

// ---------------------------------------------------------------------------
// Queue layout
// ---------------------------------------------------------------------------

auto is_intel_qav_driver(std::string_view const driver) noexcept -> bool
{
    return driver == "igb" || driver == "igc";
}

namespace {

/// The SR class A and B queues of @p tx_queues: the override's, else the driver's;
/// in a layout without its legacy range yet.
auto class_queues(std::string_view const driver, uint16_t const tx_queues, QueueOverride const& override)
    -> StatusValue<QueueLayout>
{
    bool const intel = is_intel_qav_driver(driver);
    uint16_t const a = override.class_a.value_or(intel ? uint16_t{0} : static_cast<uint16_t>(tx_queues - 1));
    uint16_t const b = override.class_b.value_or(intel ? uint16_t{1} : static_cast<uint16_t>(tx_queues - 2));
    if (a >= tx_queues || b >= tx_queues || a == b) {
        return failure(ShaperError::bad_queue_override);
    }
    // On igb/igc queue 0 outranks queue 1, and igc takes queue 1's cbs only
    // after queue 0's: class A on 1 with class B on 0 cannot be shaped.
    if (intel && a == 1 && b == 0) {
        return failure(ShaperError::bad_queue_override);
    }
    return QueueLayout{.queue_count = tx_queues, .class_a_queue = a, .class_b_queue = b};
}

/// The first queue from @p queue, moving by @p step, that is neither @p a nor @p b.
constexpr auto skip_class_queues(uint16_t queue, int const step, uint16_t const a, uint16_t const b) noexcept -> uint16_t
{
    while (queue == a || queue == b) {
        queue = static_cast<uint16_t>(queue + step);
    }
    return queue;
}

/// @p layout with the legacy TC on every other queue, as one count@offset range.
auto with_legacy_range(QueueLayout const& layout) -> StatusValue<QueueLayout>
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

}  // namespace

auto make_queue_layout(std::string_view const driver, uint16_t const tx_queues, QueueOverride const& override)
    -> StatusValue<QueueLayout>
{
    if (tx_queues < MIN_AVB_TX_QUEUES) {
        return failure(ShaperError::not_avb_capable);
    }
    auto const layout = class_queues(driver, tx_queues, override);
    if (!layout) {
        return forward_failure(layout);
    }
    return with_legacy_range(*layout);
}

auto validate_priorities(uint8_t const class_a_pcp, uint8_t const class_b_pcp) noexcept -> Status
{
    if (class_a_pcp >= 8 || class_b_pcp >= 8 || class_a_pcp == class_b_pcp) {
        return failure(ShaperError::bad_priority);
    }
    return success();
}

#if defined(__linux__)
auto mqprio_for(QueueLayout const& layout, uint16_t const major, uint8_t const class_a_pcp, uint8_t const class_b_pcp) noexcept
    -> StatusValue<net::tc::MqprioQdisc>
{  // NOLINT(statusbar-block-statements): one line per mqprio field
    if (auto status = validate_priorities(class_a_pcp, class_b_pcp); !status) {
        return failure(status.error());
    }
    net::tc::MqprioQdisc q{};
    q.major = major;
    q.num_tc = 3;
    q.prio_tc_map.fill(LEGACY_TC);
    q.prio_tc_map[class_b_pcp] = CLASS_B_TC;
    q.prio_tc_map[class_a_pcp] = CLASS_A_TC;
    q.count[LEGACY_TC] = layout.legacy_count;
    q.offset[LEGACY_TC] = layout.legacy_offset;
    q.count[CLASS_B_TC] = 1;
    q.offset[CLASS_B_TC] = layout.class_b_queue;
    q.count[CLASS_A_TC] = 1;
    q.offset[CLASS_A_TC] = layout.class_a_queue;
    q.hw = false;
    return q;
}
#endif

// ---------------------------------------------------------------------------
// Annex L
// ---------------------------------------------------------------------------

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
        load.max_frame = std::max(load.max_frame, wire_frame_size(tspec.max_frame_size));
    }
    return load;
}

[[nodiscard]] auto clamp32(int64_t const v) noexcept -> int32_t
{
    return static_cast<int32_t>(std::clamp<int64_t>(v, INT32_MIN, INT32_MAX));
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
        .hicredit = clamp32(ceil_div(load.idleslope * max_interfering_frame, link)),  // L-10
        .locredit = clamp32(ceil_div(sendslope * load.max_frame, link)),              // L-12
        .max_frame_size = load.max_frame,
    };
}

namespace {

/// Class B's hiCredit (Annex L, L-11), in floating point as SwiftMRP's calcClassBCredits.
[[nodiscard]] auto class_b_hicredit(
    int64_t const idleslope, CbsParameters const& class_a, uint32_t const link_kbps, uint32_t const max_interfering_frame) noexcept
    -> int64_t
{
    double const link = link_kbps;
    double const idle_a = class_a.idleslope_kbps;
    return static_cast<int64_t>(std::ceil(
        static_cast<double>(idleslope) *
        ((static_cast<double>(max_interfering_frame) / (link - idle_a)) + (static_cast<double>(class_a.max_frame_size) / link))));
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
        .hicredit = clamp32(ceil_div(idleslope * max_interfering_frame, link)),  // L-10
        .locredit = clamp32(ceil_div(sendslope * frame, link)),                  // L-12
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

#if defined(__linux__)

// ---------------------------------------------------------------------------
// ShaperQdiscs
// ---------------------------------------------------------------------------

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
    installed_ = true;
    reused_ = *reused;
    cbs_unsupported_ = false;
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
    if (current && current->handle == root && current->mqprio.has_value() && *current->mqprio == mqprio) {
        log_.status("shaper: kept the mqprio root {:x}: already on {}", mqprio.major, logging::embed<16>(name));
        return true;
    }
    if (auto replaced = replace_root(ifindex, name, mqprio, root); !replaced) {
        return forward_failure(replaced);
    }
    return false;
}

auto ShaperQdiscs::replace_root(
    int const ifindex, std::string_view const name, net::tc::MqprioQdisc const& mqprio, uint32_t const root) -> Status
{
    auto replaced = tc_.replace_mqprio(ifindex, mqprio);
    if (replaced || replaced.error() != std::errc::invalid_argument) {
        return replaced;
    }
    // EINVAL: the root already holds an mqprio with our handle, which mqprio refuses to
    // change. Delete it and retry once; a refused layout leaves our handle uninstalled.
    if (tc_.remove(ifindex, root, net::tc::ROOT)) {
        log_.status("shaper: replaced an existing mqprio root {:x}: on {}", mqprio.major, logging::embed<16>(name));
    }
    return tc_.replace_mqprio(ifindex, mqprio);
}

auto ShaperQdiscs::program(CbsTargets const& targets, uint32_t const link_mbps) -> Status
{
    if (!installed_ || cbs_unsupported_) {
        return success();
    }
    link_mbps_ = link_mbps;
    auto status = program_classes(targets);
    // Class A's queue alone carries the AF_XDP socket's timed frames; class B's (AF_PACKET) gets none.
    settle_etf(layout_.class_a_queue, class_a_, etf_a_, logging::lit("A"));
    return status;
}

auto ShaperQdiscs::program_classes(CbsTargets const& targets) -> Status
{
    auto status_a = program_one(layout_.class_a_queue, targets.class_a, class_a_, logging::lit("A"));
    if (cbs_unsupported_) {
        return status_a;
    }
    auto status_b = program_one(layout_.class_b_queue, targets.class_b, class_b_, logging::lit("B"));
    return !status_a ? status_a : status_b;
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

void ShaperQdiscs::settle_etf(uint16_t const queue, CbsParameters const& current, EtfState& state, logging::StrLit const label)
{
    if (launch_time_mode_ == LaunchTimeMode::unsupported || state.settled || etf_error_ || current.empty()) {
        return;
    }
    bool const on = launch_time_mode_ == LaunchTimeMode::on;
    auto const status =
        etf_slot(queue).and_then([&](EtfSlot const& slot) { return on ? put_etf(slot, label) : clear_etf(slot, label); });
    if (!status) {
        // No etf offload (EOPNOTSUPP) or another refusal: frames on the queue go out at once. Quiet
        // when off: nothing was asked for, the leftover cleanup is best effort.
        etf_error_ = status.error();
        if (!on) {
            return;
        }
        log_.warning(
            "shaper: class {} etf on queue {} failed: errno {} {}: no launch time",
            label,
            queue,
            etf_error_.value(),
            logging::embed<32>(tc_.last_message()));
        return;
    }
    state = EtfState{.settled = true, .placed = on};
}

auto ShaperQdiscs::etf_slot(uint16_t const queue) -> StatusValue<EtfSlot>
{
    auto const cbs = tc_.get(ifindex_, parent_of(queue));
    if (!cbs) {
        return failure(cbs.error());
    }
    // cbs has one class, major:1 (sch_cbs.c); the kernel numbered the cbs itself.
    uint32_t const parent = net::tc::make_handle(static_cast<uint16_t>(cbs->handle >> 16U), 1);
    return child_of(parent).transform(
        [&](std::optional<net::tc::QdiscInfo> child) { return EtfSlot{.parent = parent, .child = std::move(child)}; });
}

auto ShaperQdiscs::child_of(uint32_t const parent) -> StatusValue<std::optional<net::tc::QdiscInfo>>
{
    auto child = tc_.get(ifindex_, parent);
    if (child) {
        return std::optional{std::move(*child)};
    }
    if (child.error() == std::errc::no_such_file_or_directory) {
        return std::optional<net::tc::QdiscInfo>{};
    }
    return failure(child.error());
}

auto ShaperQdiscs::put_etf(EtfSlot const& slot, logging::StrLit const label) -> Status
{
    net::tc::EtfQdisc const etf{.parent = slot.parent, .delta_ns = etf_delta_ns_, .offload = true, .skip_sock_check = true};
    if (slot.child && slot.child->etf == etf) {
        log_.status("shaper: class {} kept its etf (delta {} ns)", label, etf_delta_ns_);
        return success();
    }
    // etf has no change operation: another etf goes first. Neither resets igc while the cbs stays.
    if (auto removed = slot.child && slot.child->kind == "etf" ? tc_.remove(ifindex_, slot.child->handle, slot.parent) : Status{};
        !removed) {
        return removed;
    }
    return tc_.replace_etf(ifindex_, etf).transform([&] {
        log_.status("shaper: class {} etf under its cbs: offloaded launch time, delta {} ns", label, etf_delta_ns_);
    });
}

auto ShaperQdiscs::clear_etf(EtfSlot const& slot, logging::StrLit const label) -> Status
{
    if (!slot.child || slot.child->kind != "etf") {
        return success();
    }
    return tc_.remove(ifindex_, slot.child->handle, slot.parent).transform([&] {
        log_.status("shaper: class {} etf removed: launch time not wanted", label);
    });
}

auto ShaperQdiscs::program_one(
    uint16_t const queue, CbsParameters const& params, CbsParameters& current, logging::StrLit const label) -> Status
{
    if (params == current || params.empty()) {
        return success();
    }
    // Always a replace: a change in place on a cbs, a graft on the default qdisc (which resets
    // igb/igc only when it is the first cbs). See docs/SHAPER.md.
    auto status = tc_.replace_cbs(
        ifindex_,
        net::tc::CbsQdisc{
            .parent = parent_of(queue),
            .offload = true,
            .hicredit = params.hicredit,
            .locredit = params.locredit,
            .idleslope_kbps = params.idleslope_kbps,
            .sendslope_kbps = params.sendslope_kbps,
        });
    if (!status) {
        if (status.error() == std::errc::operation_not_supported) {
            // No cbs offload (not an i210/i225, a switch port without it):
            // a software cbs would never see the bypass frames, so stop asking.
            cbs_unsupported_ = true;
            log_.warning(
                "shaper: no cbs offload on queue {} ({}): talker streams run unshaped on the class queues",
                queue,
                logging::embed<32>(tc_.last_message()));
            return status;
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

void ShaperQdiscs::remove_cbs()
{
    // Class B's queue first (igc refuses to drop class A's cbs while B's is on), both queues
    // whatever was recorded, grafting the default qdisc (deleting an mqprio child leaves
    // noop_qdisc). See docs/SHAPER.md.
    for (uint16_t const queue : {layout_.class_b_queue, layout_.class_a_queue}) {
        if (auto status = tc_.replace_kind(ifindex_, parent_of(queue), "pfifo_fast"); !status) {
            log_.warning("shaper: removing the cbs on queue {} failed: errno {}", queue, status.error().value());
        }
    }
    class_a_ = {};
    class_b_ = {};
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

// ---------------------------------------------------------------------------
// PortShaper
// ---------------------------------------------------------------------------

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
    // Leave the root and both cbs for the next install(), parked at the
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
    auto const layout = make_queue_layout(port.driver, port.tx_queues, config_.queues);
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
    auto const mqprio = mqprio_for(layout, config_.qdisc_major, class_a_pcp_, DEFAULT_CLASS_B_PCP);
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
    if (is_intel_qav_driver(driver_) && (layout_.class_a_queue > 1 || layout_.class_b_queue > 1)) {
        log_.warning(
            "shaper: {} offloads cbs only on queues 0 and 1; the override puts an SR class elsewhere", logging::embed<16>(driver_));
    }
}

auto PortShaper::program(std::span<TSpec const> const class_a) noexcept -> Status
{
    if (class_a.size() > MAX_PORT_SHAPER_STREAMS) {
        return failure(make_error_code(ShaperError::too_many_streams));
    }
    class_a_streams_.assign(class_a.begin(), class_a.end());
    dirty_ = true;
    retry_at_ns_ = 0;  // a new set goes out at once
    return success();
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
    // Class B stays empty (its placeholder) and is laid out (queue, TC) regardless.
    uint32_t const mbps = link_mbps();
    programmed_link_mbps_ = mbps;
    std::span<TSpec const> const class_a = streams ? std::span<TSpec const>{class_a_streams_} : std::span<TSpec const>{};
    return cbs_targets(class_a, {}, mbps * 1000U, mtu_);
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

// ---------------------------------------------------------------------------
// EntityShaper
// ---------------------------------------------------------------------------

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
    if (auto const priority = kit_.stream_tx_priority_error(); priority) {
        kit_.host().ctl_log().warning(
            "shaper: the stream socket has no SO_PRIORITY (errno {}): its frames miss the shaped queue", priority.value());
    }
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
