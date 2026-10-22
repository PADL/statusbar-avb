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

auto make_queue_layout(std::string_view const driver, uint16_t const tx_queues, QueueOverride const& override)
    -> StatusValue<QueueLayout>
{
    if (tx_queues < MIN_AVB_TX_QUEUES) {
        return failure(ShaperError::not_avb_capable);
    }
    bool const intel = is_intel_qav_driver(driver);
    uint16_t const a = override.class_a.value_or(intel ? uint16_t{0} : static_cast<uint16_t>(tx_queues - 1));
    uint16_t const b = override.class_b.value_or(intel ? uint16_t{1} : static_cast<uint16_t>(tx_queues - 2));
    if (a >= tx_queues || b >= tx_queues || a == b) {
        return failure(ShaperError::bad_queue_override);
    }
    // The legacy TC takes every other queue, as one count@offset range.
    uint16_t low = 0;
    while (low == a || low == b) {
        ++low;
    }
    uint16_t high = tx_queues - 1;
    while (high == a || high == b) {
        --high;
    }
    if ((a > low && a < high) || (b > low && b < high)) {
        return failure(ShaperError::bad_queue_override);
    }
    return QueueLayout{
        .queue_count = tx_queues,
        .class_a_queue = a,
        .class_b_queue = b,
        .legacy_offset = low,
        .legacy_count = static_cast<uint16_t>(high - low + 1),
    };
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
{
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
    double const link = link_kbps;
    double const idle_a = class_a.idleslope_kbps;
    int64_t const sendslope = load.idleslope - static_cast<int64_t>(link_kbps);
    // L-11, in floating point as SwiftMRP's calcClassBCredits.
    double const hicredit = std::ceil(
        static_cast<double>(load.idleslope) *
        ((static_cast<double>(max_interfering_frame) / (link - idle_a)) + (static_cast<double>(class_a.max_frame_size) / link)));
    return CbsParameters{
        .idleslope_kbps = clamp32(load.idleslope),
        .sendslope_kbps = clamp32(sendslope),
        .hicredit = clamp32(static_cast<int64_t>(hicredit)),
        .locredit = clamp32(ceil_div(sendslope * load.max_frame, static_cast<int64_t>(link_kbps))),
        .max_frame_size = load.max_frame,
    };
}

// ---------------------------------------------------------------------------
// EntityShaper
// ---------------------------------------------------------------------------

#if defined(__linux__)

EntityShaper::EntityShaper(ShaperConfig config, AvbEntityKit& kit)
    : config_{std::move(config)}
    , kit_{kit}
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
    if (config_.restore_qdisc) {
        (void)uninstall();  // logged
    }
}

auto EntityShaper::install() -> Status
{
    if (installed_) {
        return success();
    }
    auto log = kit_.host().ctl_log();
    auto const& name = config_.interface_name;
    auto const ifindex = net::interface_index(name);
    if (!ifindex) {
        return failure(ifindex.error());
    }
    auto driver = net::driver_name(name);
    if (!driver) {
        return failure(driver.error());
    }
    auto const queues = net::tx_queue_count(name);
    if (!queues) {
        return failure(queues.error());
    }
    auto const layout = make_queue_layout(*driver, *queues, config_.queues);
    if (!layout) {
        return failure(layout.error());
    }
    // Class A is the kit's stream PCP (open_tx made it the stream socket's
    // SO_PRIORITY); class B the 802.1Q default.
    auto const mqprio = mqprio_for(*layout, config_.qdisc_major, kit_.config().stream_pcp, DEFAULT_CLASS_B_PCP);
    if (!mqprio) {
        return failure(mqprio.error());
    }
    if (auto const mtu = net::interface_mtu(name)) {
        mtu_ = *mtu;
    }
    if (auto status = client_.open(); !status) {
        return status;
    }
    ifindex_ = *ifindex;
    driver_ = std::move(*driver);
    layout_ = *layout;

    uint32_t const root = net::tc::make_handle(config_.qdisc_major, 0);
    auto replaced = client_.replace_mqprio(ifindex_, *mqprio);
    if (!replaced && replaced.error() == std::errc::invalid_argument) {
        // EINVAL: the root already holds an mqprio with our handle (ours from a run that did not
        // uninstall, or a foreign one we take over), which mqprio will not change. Delete it and try
        // once more; a refused layout fails again and leaves our handle uninstalled.
        if (client_.remove(ifindex_, root, net::tc::ROOT)) {
            log.status("shaper: replaced an existing mqprio root {:x}: on {}", config_.qdisc_major, logging::embed<16>(name));
        }
        replaced = client_.replace_mqprio(ifindex_, *mqprio);
    }
    if (!replaced) {
        return replaced;
    }
    installed_ = true;
    log.status(
        "shaper: mqprio {:x}: on {} ({}, {} TX queues): class A queue {}, class B queue {}, legacy {}@{}",
        config_.qdisc_major,
        logging::embed<16>(name),
        logging::embed<16>(driver_),
        layout_.queue_count,
        layout_.class_a_queue,
        layout_.class_b_queue,
        layout_.legacy_count,
        layout_.legacy_offset);
    if (is_intel_qav_driver(driver_) && (layout_.class_a_queue > 1 || layout_.class_b_queue > 1)) {
        log.warning(
            "shaper: {} offloads cbs only on queues 0 and 1; the override puts an SR class elsewhere", logging::embed<16>(driver_));
    }
    if (auto const priority = kit_.talker().tx_priority_error(); priority) {
        log.warning("shaper: the stream socket has no SO_PRIORITY (errno {}): its frames miss the shaped queue", priority.value());
    }
    // From here on the kit reports admission changes; tick() reprograms.
    kit_.set_on_admission_changed([this](std::span<uint16_t const> admitted) { on_admission_changed(admitted); });
    hooked_ = true;
    auto const admitted = kit_.admitted_talker_streams();
    admitted_.assign(admitted.begin(), admitted.end());
    (void)refresh_link_speed();
    (void)reprogram();
    return success();
}

void EntityShaper::on_admission_changed(std::span<uint16_t const> const admitted) noexcept
{
    admitted_.clear();
    for (uint16_t const index : admitted) {
        (void)admitted_.try_push_back(index);
    }
    dirty_ = true;
}

void EntityShaper::tick(int64_t const now_ns)
{
    if (!installed_) {
        return;
    }
    if (now_ns >= next_link_check_ns_) {
        next_link_check_ns_ = now_ns + LINK_CHECK_NS;
        if (refresh_link_speed()) {
            dirty_ = true;
        }
    }
    if (dirty_) {
        (void)reprogram();
    }
}

auto EntityShaper::link_mbps() const noexcept -> uint32_t
{
    return known_link_mbps_ != 0 ? known_link_mbps_ : config_.fallback_link_mbps;
}

auto EntityShaper::refresh_link_speed() -> bool
{
    auto const speed = net::link_speed_mbps(config_.interface_name);
    if (speed) {
        link_error_ = {};
        known_link_mbps_ = *speed;
    } else if (speed.error() != net::make_unknown_link_speed() && speed.error() != link_error_) {
        // Unknown speed (link down) keeps the last known one: a flap does not
        // churn the shaper. A real read error is logged once.
        link_error_ = speed.error();
        kit_.host().ctl_log().warning("shaper: reading the link speed failed: errno {}", link_error_.value());
    }
    return link_mbps() != programmed_link_mbps_;
}

auto EntityShaper::reprogram() -> Status
{
    if (!installed_) {
        return success();
    }
    dirty_ = false;
    // Every talker stream of the kit is class A: it declares only the class A
    // SR domain (MSRP sr_class_id 6). Class B stays empty until it talks class
    // B, and is laid out (queue, TC) regardless.
    sg14::inplace_vector<TSpec, MAX_ENTITY_STREAMS> class_a_streams{};
    for (uint16_t const index : admitted_) {
        for (auto const& spec : kit_.talker_specs()) {
            if (spec.index == index) {
                (void)class_a_streams.try_push_back(AvbEntityKit::talker_tspec(spec));
            }
        }
    }
    uint32_t const mbps = link_mbps();
    programmed_link_mbps_ = mbps;
    uint32_t const link = mbps * 1000U;
    auto const a = class_a_cbs(class_a_streams, link, mtu_);
    auto const b = class_b_cbs({}, a, link, mtu_);
    auto status_a = program(layout_.class_a_queue, a, class_a_, logging::lit("A"));
    auto status_b = program(layout_.class_b_queue, b, class_b_, logging::lit("B"));
    return !status_a ? status_a : status_b;
}

auto EntityShaper::program(uint16_t const queue, CbsParameters const& params, CbsParameters& current, logging::StrLit const label)
    -> Status
{
    if (params == current) {
        return success();
    }
    auto log = kit_.host().ctl_log();
    uint32_t const parent = net::tc::make_handle(config_.qdisc_major, static_cast<uint16_t>(queue + 1));
    Status status = success();
    if (params.empty()) {
        // Removing the cbs: graft the default qdisc back rather than delete
        // (deleting an mqprio child leaves noop_qdisc on the queue). Either
        // way the cbs is destroyed and the NIC's shaper on the queue disabled.
        status = client_.replace_kind(ifindex_, parent, "pfifo_fast");
    } else {
        status = client_.replace_cbs(
            ifindex_,
            net::tc::CbsQdisc{
                .parent = parent,
                .offload = true,
                .hicredit = params.hicredit,
                .locredit = params.locredit,
                .idleslope_kbps = params.idleslope_kbps,
                .sendslope_kbps = params.sendslope_kbps,
            });
    }
    if (!status) {
        log.warning(
            "shaper: class {} cbs on queue {} ({:x}:{:x}) failed: errno {} {}",
            label,
            queue,
            config_.qdisc_major,
            queue + 1,
            status.error().value(),
            logging::embed<32>(client_.last_message()));
        return status;
    }
    current = params;
    if (params.empty()) {
        log.status("shaper: class {} cbs removed from queue {}: no admitted stream", label, queue);
    } else {
        log.status(
            "shaper: class {} cbs on queue {}: idleslope {} sendslope {} hicredit {} locredit {} ({} Mbit/s)",
            label,
            queue,
            params.idleslope_kbps,
            params.sendslope_kbps,
            params.hicredit,
            params.locredit,
            programmed_link_mbps_);
    }
    return success();
}

auto EntityShaper::uninstall() -> Status
{
    unhook();
    if (!installed_) {
        return success();
    }
    installed_ = false;
    dirty_ = false;
    class_a_ = {};
    class_b_ = {};
    auto log = kit_.host().ctl_log();
    auto status = client_.remove(ifindex_, net::tc::make_handle(config_.qdisc_major, 0), net::tc::ROOT);
    if (!status) {
        log.warning("shaper: removing mqprio {:x} failed: errno {}", config_.qdisc_major, status.error().value());
        return status;
    }
    log.status("shaper: mqprio {:x} removed", config_.qdisc_major);
    return success();
}

#endif  // __linux__

}  // namespace statusbar::avb_entity
