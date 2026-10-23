#pragma once
// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Enumerator — the headless full descriptor-tree walk behind
/// `statusbar-atdecc-monitor --enumerate`.
///
/// Walks the whole AEM model of each discovered entity (optionally one
/// --target): ENTITY -> CONFIGURATION -> every (type, index) listed in the
/// configuration's descriptor_counts, handing each descriptor read for the
/// first time to a hook, then reports completion so the tool can exit.
///
/// Network-free: reads go out through a hook and responses and timeouts are
/// told to it, so its in-flight accounting is testable without a controller.

#include "statusbar/atdecc/atdecc_adp_discovery.hpp"
#include "statusbar/atdecc/atdecc_adp_format.hpp"
#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/sg14/inplace_function.h"

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <tuple>
#include <utility>

namespace statusbar::atdecc_tools {

// Defensive caps against a hostile/corrupt CONFIGURATION descriptor: real
// entities have at most tens of most descriptor types and far fewer than 108
// types, so these bound enumeration hard (a raw 65535 x 65535 would otherwise
// be ~4 billion enqueues).
constexpr uint16_t ENUMERATOR_MAX_DESCRIPTORS_PER_TYPE = 60000;  // a 55k-CONTROL processor proxy is a real model
constexpr uint16_t ENUMERATOR_MAX_DESCRIPTOR_TYPES = 108;        // total AVDECC descriptor types

/// The big-endian doublet at `off` in `b`, or 0 when `b` is too short.
[[nodiscard]] inline auto be16_at(std::span<uint8_t const> b, size_t off) -> uint16_t
{
    if (off + 1 >= b.size()) {
        return 0;
    }
    ieee::doublet_t v{};
    statusbar::span_load(v, b.subspan(off, sizeof(v)));
    return v.get();
}

/// What the enumerator needs from its host.
struct EnumeratorHooks
{
    /// Issue READ_DESCRIPTOR(type, index) to target. False when the controller
    /// cannot send it now (the read is kept and tried again on the next pump).
    statusbar::sg14::inplace_function<bool(ieee::Eui64 const& target, uint16_t type, uint16_t index), 64> read_descriptor;
    /// A descriptor read for the first time: its body (type and index first).
    statusbar::sg14::inplace_function<void(ieee::Eui64 const& target, std::span<uint8_t const> body), 64> on_descriptor;
    /// Trace the walk on stderr.
    bool debug{false};
};

class Enumerator
{
  public:
    /// Reads in flight at once: under the AEM controller's in-flight cap.
    static constexpr int MAX_INFLIGHT = 4;

    /// @param target  walk only this entity (and finish as soon as its tree is
    ///                walked); nullopt walks every discovered entity and waits
    ///                out `quiet_ns` without a new one before finishing.
    Enumerator(std::optional<ieee::Eui64> target, int64_t quiet_ns, EnumeratorHooks hooks)
        : target_filter_{target}
        , quiet_ns_{quiet_ns}
        , hooks_{std::move(hooks)}
    {}

    /// An entity is available or updated: register it and queue its ENTITY
    /// read (once; ADP re-announces every couple of seconds). The enumerator
    /// issues every READ_DESCRIPTOR itself, the ENTITY one included, so that
    /// each read it counts out is one it counted in.
    void on_entity(atdecc::DiscoveredEntity const& e, int64_t now_ns)
    {
        ieee::Eui64 const id = e.adpdu.entity_id;
        if (target_filter_ && !eui_eq(id, *target_filter_)) {
            return;
        }
        last_discovery_ns_ = now_ns;
        if (targets_.emplace(id, TargetState{}).second) {
            std::println(
                "\n=== entity {}  caps=[{}] ===",
                ieee::to_string(id).view(),
                atdecc::entity_capabilities_to_string(e.adpdu.entity_capabilities.get()));
        }
        enqueue(id, atdecc::aem::DESCRIPTOR_ENTITY, 0);
    }

    /// A final AEM response. Every READ_DESCRIPTOR on this controller is one
    /// of ours, and the AEM controller ends each exactly once (a final response
    /// or a timeout), so every READ_DESCRIPTOR response releases one slot
    /// whatever it carries; nothing else touches the count.
    void on_response(ieee::Eui64 const& target, uint16_t cmd, uint8_t status, std::span<uint8_t const> data)
    {
        if (cmd != atdecc::AEM_COMMAND_READ_DESCRIPTOR) {
            return;
        }
        release_one();
        if (!targets_.contains(target) || data.size() < atdecc::aem::AemReadDescriptorResponsePayload::LENGTH) {
            return;
        }
        auto const body = data.subspan(atdecc::aem::AemReadDescriptorResponsePayload::LENGTH);
        if (status != atdecc::AEM_STATUS_SUCCESS || body.size() < 4) {
            trace("[enum] error resp: status={} data_len={}", status, data.size());
            return;
        }
        expand(target, body);
    }

    /// An AEM command's retry timed out too: the other way a read ends.
    void on_timeout(ieee::Eui64 const& /*target*/, uint16_t cmd)
    {
        if (cmd == atdecc::AEM_COMMAND_READ_DESCRIPTOR) {
            release_one();
        }
    }

    /// Issue queued reads up to MAX_INFLIGHT. Called from the main loop, never
    /// from inside a response: issuing a read re-entrantly would mutate the
    /// controller's in-flight list mid-dispatch.
    void pump()
    {
        while (outstanding_ < MAX_INFLIGHT && !queue_.empty()) {
            auto const [t, type, index] = queue_.front();
            queue_.pop_front();
            bool const ok = hooks_.read_descriptor && hooks_.read_descriptor(t, type, index);
            trace("[enum] read type={:#06x} idx={} ok={}", type, index, ok);
            if (!ok) {
                queue_.push_front({t, type, index});  // controller busy; retry next pump
                return;
            }
            ++outstanding_;
        }
    }

    [[nodiscard]] auto finished(int64_t now_ns) const -> bool
    {
        if (targets_.empty() || !queue_.empty() || outstanding_ > 0) {
            return false;
        }
        for (auto const& [id, st] : targets_) {
            if (!st.seen_entity || !st.seen_config) {
                return false;
            }
        }
        // Explicit single target: done as soon as its tree is walked. Otherwise
        // wait out a quiet window so slow-to-advertise entities aren't missed.
        return target_filter_.has_value() || (now_ns - last_discovery_ns_) > quiet_ns_;
    }

    [[nodiscard]] auto entity_count() const noexcept -> size_t { return targets_.size(); }

    /// Reads issued and not yet answered or timed out.
    [[nodiscard]] auto outstanding() const noexcept -> int { return outstanding_; }

  private:
    struct TargetState
    {
        bool seen_entity{false};
        bool seen_config{false};
    };
    using ReadKey = std::tuple<ieee::Eui64, uint16_t, uint16_t>;

    static auto eui_eq(ieee::Eui64 const& a, ieee::Eui64 const& b) -> bool { return !(a < b) && !(b < a); }

    template <class... Args>
    void trace(std::format_string<Args...> fmt, Args&&... args) const
    {
        if (hooks_.debug) {
            std::println(stderr, fmt, std::forward<Args>(args)...);
        }
    }

    void release_one() noexcept
    {
        if (outstanding_ > 0) {
            --outstanding_;
        }
    }

    void enqueue(ieee::Eui64 const& t, uint16_t type, uint16_t index)
    {
        if (queued_.insert(ReadKey{t, type, index}).second) {
            queue_.push_back({t, type, index});
        }
    }

    /// A descriptor arrived: hand it on the first time, and walk ENTITY and
    /// CONFIGURATION into the reads beneath them, once each.
    void expand(ieee::Eui64 const& target, std::span<uint8_t const> body)
    {
        uint16_t const dtype = be16_at(body, 0);
        uint16_t const dindex = be16_at(body, 2);
        trace("[enum] resp type={:#06x} idx={} blen={} q={} out={}", dtype, dindex, body.size(), queue_.size(), outstanding_);
        ReadKey const key{target, dtype, dindex};
        if (printed_.insert(key).second && hooks_.on_descriptor) {
            hooks_.on_descriptor(target, body);
        }
        if (expanded_.insert(key).second) {
            expand_tree(targets_[target], target, dtype, body);
        }
    }

    void expand_tree(TargetState& state, ieee::Eui64 const& target, uint16_t dtype, std::span<uint8_t const> body)
    {
        if (dtype == atdecc::aem::DESCRIPTOR_ENTITY) {
            expand_entity(state, target, body);
        } else if (dtype == atdecc::aem::DESCRIPTOR_CONFIGURATION) {
            expand_configuration(state, target, body);
        } else {
            expand_children(target, dtype, body);
        }
    }

    /// descriptor_counts lists only top-level descriptors (IEEE 1722.1-2021
    /// 7.2.2): the rest are reached through their owners' base/count pairs.
    void expand_children(ieee::Eui64 const& target, uint16_t type, std::span<uint8_t const> body)
    {  // NOLINT(statusbar-block-statements): one case per owning descriptor type
        using namespace atdecc::aem;
        switch (type) {
            case DESCRIPTOR_AUDIO_UNIT:
                expand_unit<DescriptorAudioUnit>(target, body, DESCRIPTOR_AUDIO_CLUSTER, DESCRIPTOR_AUDIO_MAP);
                break;
            case DESCRIPTOR_VIDEO_UNIT:
                expand_unit<DescriptorVideoUnit>(target, body, DESCRIPTOR_VIDEO_CLUSTER, DESCRIPTOR_VIDEO_MAP);
                break;
            case DESCRIPTOR_SENSOR_UNIT:
                expand_unit<DescriptorSensorUnit>(target, body, DESCRIPTOR_SENSOR_CLUSTER, DESCRIPTOR_SENSOR_MAP);
                break;
            case DESCRIPTOR_STREAM_PORT_INPUT:
            case DESCRIPTOR_STREAM_PORT_OUTPUT:
                expand_port(target, type, body);
                break;
            case DESCRIPTOR_LOCALE: {
                DescriptorLocale locale{};
                span_load_padded(locale, body);
                range(target, DESCRIPTOR_STRINGS, locale.base_strings.get(), locale.number_of_strings.get());
                break;
            }
            case DESCRIPTOR_PTP_INSTANCE: {
                DescriptorPtpInstance ptp{};
                span_load_padded(ptp, body);
                range(target, DESCRIPTOR_PTP_PORT, ptp.base_ptp_port.get(), ptp.number_of_ptp_ports.get());
                range(target, DESCRIPTOR_CONTROL, ptp.base_control.get(), ptp.number_of_controls.get());
                break;
            }
            case DESCRIPTOR_AVB_INTERFACE:
                expand_controls<DescriptorAvbInterface>(target, body);
                break;
            case DESCRIPTOR_JACK_INPUT:
            case DESCRIPTOR_JACK_OUTPUT:
                expand_controls<DescriptorJack>(target, body);
                break;
            case DESCRIPTOR_CONTROL_BLOCK:
                expand_controls<DescriptorControlBlock>(target, body);
                break;
            default:
                break;
        }
    }

    /// The cluster and map descriptor types of the kind of unit that owns a stream port.
    auto port_children(ieee::Eui64 const& target, uint16_t type, uint16_t index) const -> std::pair<uint16_t, uint16_t>
    {
        auto const owner = port_owners_.find(ReadKey{target, type, index});
        return owner != port_owners_.end() ? owner->second
                                           : std::pair{atdecc::aem::DESCRIPTOR_AUDIO_CLUSTER, atdecc::aem::DESCRIPTOR_AUDIO_MAP};
    }

    void expand_port(ieee::Eui64 const& target, uint16_t type, std::span<uint8_t const> body)
    {
        atdecc::aem::DescriptorStreamPort port{};
        span_load_padded(port, body);
        auto const [cluster, map] = port_children(target, type, be16_at(body, 2));
        range(target, cluster, port.base_cluster.get(), port.number_of_clusters.get());
        range(target, map, port.base_map.get(), port.number_of_maps.get());
        range(target, atdecc::aem::DESCRIPTOR_CONTROL, port.base_control.get(), port.number_of_controls.get());
    }

    template <typename Unit>
    void expand_unit(ieee::Eui64 const& target, std::span<uint8_t const> body, uint16_t cluster, uint16_t map)
    {  // NOLINT(statusbar-block-statements): one range per kind of child descriptor
        using namespace atdecc::aem;
        Unit unit{};
        span_load_padded(unit, body);
        auto ports = [&](uint16_t port_type, uint16_t base, uint16_t count) {
            for (uint32_t i = 0; i < std::min(count, ENUMERATOR_MAX_DESCRIPTORS_PER_TYPE) && base + i <= 0xFFFF; ++i) {
                port_owners_[ReadKey{target, port_type, static_cast<uint16_t>(base + i)}] = {cluster, map};
            }
            range(target, port_type, base, count);
        };
        ports(DESCRIPTOR_STREAM_PORT_INPUT, unit.base_stream_input_port.get(), unit.number_of_stream_input_ports.get());
        ports(DESCRIPTOR_STREAM_PORT_OUTPUT, unit.base_stream_output_port.get(), unit.number_of_stream_output_ports.get());
        range(
            target, DESCRIPTOR_EXTERNAL_PORT_INPUT, unit.base_external_input_port.get(), unit.number_of_external_input_ports.get());
        range(
            target,
            DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
            unit.base_external_output_port.get(),
            unit.number_of_external_output_ports.get());
        range(
            target, DESCRIPTOR_INTERNAL_PORT_INPUT, unit.base_internal_input_port.get(), unit.number_of_internal_input_ports.get());
        range(
            target,
            DESCRIPTOR_INTERNAL_PORT_OUTPUT,
            unit.base_internal_output_port.get(),
            unit.number_of_internal_output_ports.get());
        range(target, DESCRIPTOR_CONTROL, unit.base_control.get(), unit.number_of_controls.get());
    }

    template <typename Owner>
    void expand_controls(ieee::Eui64 const& target, std::span<uint8_t const> body)
    {
        Owner owner{};
        span_load_padded(owner, body);
        range(target, atdecc::aem::DESCRIPTOR_CONTROL, owner.base_control.get(), owner.number_of_controls.get());
    }

    /// Enqueue @p count descriptors of @p type from @p base (bounded).
    void range(ieee::Eui64 const& target, uint16_t type, uint16_t base, uint16_t count)
    {
        count = std::min(count, ENUMERATOR_MAX_DESCRIPTORS_PER_TYPE);
        for (uint32_t i = 0; i < count && base + i <= 0xFFFF; ++i) {
            enqueue(target, type, static_cast<uint16_t>(base + i));
        }
    }

    void expand_entity(TargetState& state, ieee::Eui64 const& target, std::span<uint8_t const> body)
    {
        state.seen_entity = true;
        // configurations_count at offset 308 of the ENTITY descriptor.
        uint16_t const n_configs = be16_at(body, 308);
        trace("[enum] ENTITY n_configs={}", n_configs);
        for (uint16_t c = 0; c < n_configs; ++c) {
            enqueue(target, atdecc::aem::DESCRIPTOR_CONFIGURATION, c);
        }
    }

    void expand_configuration(TargetState& state, ieee::Eui64 const& target, std::span<uint8_t const> body)
    {
        state.seen_config = true;
        // descriptor_counts: count at offset 70, table offset at 72. Both
        // counts are untrusted uint16 -- bound them so a hostile CONFIGURATION
        // can't drive ~4 billion enqueues (M2 DoS).
        uint16_t const counts = std::min<uint16_t>(be16_at(body, 70), ENUMERATOR_MAX_DESCRIPTOR_TYPES);
        uint16_t const off = std::max<uint16_t>(be16_at(body, 72), 74);
        for (uint16_t i = 0; i < counts; ++i) {
            enqueue_counted(target, body, static_cast<size_t>(off) + (static_cast<size_t>(i) * 4));
        }
    }

    /// Queue the reads for the descriptor_counts entry at `base`.
    void enqueue_counted(ieee::Eui64 const& target, std::span<uint8_t const> body, size_t base)
    {
        uint16_t const t = be16_at(body, base);
        uint16_t const cnt = std::min<uint16_t>(be16_at(body, base + 2), ENUMERATOR_MAX_DESCRIPTORS_PER_TYPE);
        if (t == atdecc::aem::DESCRIPTOR_ENTITY || t == atdecc::aem::DESCRIPTOR_CONFIGURATION) {
            return;  // already covered; avoid recursion
        }
        for (uint16_t k = 0; k < cnt; ++k) {
            enqueue(target, t, k);
        }
    }

    std::optional<ieee::Eui64> target_filter_;
    int64_t quiet_ns_;
    EnumeratorHooks hooks_;
    int64_t last_discovery_ns_{0};
    std::map<ieee::Eui64, TargetState> targets_;
    std::deque<ReadKey> queue_;
    std::set<ReadKey> queued_;
    std::set<ReadKey> printed_;
    std::set<ReadKey> expanded_;
    /// A stream port's owning unit kind: its (cluster, map) descriptor types.
    std::map<ReadKey, std::pair<uint16_t, uint16_t>> port_owners_;
    int outstanding_{0};
};

}  // namespace statusbar::atdecc_tools
