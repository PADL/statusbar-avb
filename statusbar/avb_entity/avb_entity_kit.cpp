// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AvbEntityKit methods — the Entity Construction Kit facade (refactor phase D;
// the wiring bodies come verbatim from AvbEntityToneGenerator/AvbEntityAudioIO,
// which each carried a copy).

#include "statusbar/avb_entity/avb_entity_kit.hpp"

#include "statusbar/avb_entity/avb_entity_identity.hpp"
#include "statusbar/avb_entity/avb_entity_shaper.hpp"
#include "statusbar/avb_entity/avb_entity_stream_info.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/net/net_interface_info.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

namespace statusbar::avb_entity {

using statusbar::atdecc::aem::DESCRIPTOR_STREAM_INPUT;
using statusbar::atdecc::aem::DESCRIPTOR_STREAM_OUTPUT;
using statusbar::nanoavb::DomainInfo;

namespace {

/// The static destination MAC @p config gives a talker stream of @p kind (none for "other").
auto talker_dest_mac(AvbEntityAudioIOConfig const& config, StreamKind const kind) noexcept -> ieee::Eui48
{
    switch (kind) {
        case StreamKind::am824:
            return config.am824_talker_dest_mac;
        case StreamKind::aaf:
            return config.aaf_talker_dest_mac;
        case StreamKind::crf:
            return config.crf_talker_dest_mac;
        case StreamKind::other:
        default:
            return ieee::Eui48{};
    }
}

/// Descriptor (@p type, @p index) of @p host's model as a @p T; nullopt when there is none.
template <typename T>
[[nodiscard]] auto load_descriptor(AvbEntityHost const& host, uint16_t const type, uint16_t const index) -> std::optional<T>
{
    auto const blob = host.get_descriptor(type, index);
    if (!blob.has_value()) {
        return std::nullopt;
    }
    T desc{};
    span_load_padded(desc, *blob);
    return desc;
}

/// The AUDIO_CLUSTER at @p offset from @p port's base cluster; nullopt when there is none.
[[nodiscard]] auto audio_cluster(AvbEntityHost const& host, atdecc::aem::DescriptorStreamPort const& port, uint16_t const offset)
    -> std::optional<atdecc::aem::DescriptorAudioCluster>
{
    return load_descriptor<atdecc::aem::DescriptorAudioCluster>(
        host, atdecc::aem::DESCRIPTOR_AUDIO_CLUSTER, static_cast<uint16_t>(port.base_cluster.get() + offset));
}

/// STREAM_PORT port_flags ASYNC_ and SYNC_SAMPLE_RATE_CONV (IEEE 1722.1-2021 Clause 7.2.13.1).
constexpr uint16_t PORT_FLAGS_SAMPLE_RATE_CONV = 0x0002 | 0x0004;

/// Whether @p port converts sample rates between its audio unit and its streams (Milan v1.3 Section 5.3.3.7).
[[nodiscard]] auto converts_rates(atdecc::aem::DescriptorStreamPort const& port) noexcept -> bool
{
    return (port.port_flags.get() & PORT_FLAGS_SAMPLE_RATE_CONV) != 0;
}

/// SET_CLOCK_SOURCE's body (IEEE 1722.1-2021 Clause 7.4.23.1): CLOCK_DOMAIN, its index, the source, reserved.
auto clock_source_command(uint16_t const clock_domain_index, uint16_t const clock_source_index) -> std::array<uint8_t, 8>
{
    std::array<uint8_t, 8> body{};
    span_store(std::span{body}.subspan(0, 2), atdecc::doublet_t{atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN});
    span_store(std::span{body}.subspan(2, 2), atdecc::doublet_t{clock_domain_index});
    span_store(std::span{body}.subspan(4, 2), atdecc::doublet_t{clock_source_index});
    return body;
}

/// Descriptor (@p type, @p index) of configuration 0.
auto descriptor_id(uint16_t const type, uint16_t const index) -> nanoavb::DescriptorId
{
    return nanoavb::DescriptorId{
        .ref = {.configuration_index = 0, .descriptor_type = type, .descriptor_index = index}, .symbol = 0};
}

/// A saved value the model refused is dropped (the file is rewritten without it).
void log_refused(logging::Logger log, std::string_view const what, uint16_t const type, uint16_t const index, uint8_t const status)
{
    log.warning("kit: saved {} of descriptor {}:{} refused (status {}): dropped", logging::embed<24>(what), type, index, status);
}

/// Whether another AUDIO_UNIT is in @p unit_zero's clock domain (then the domain's streams belong to neither alone).
auto shares_clock_domain(AvbEntityHost const& host, std::optional<atdecc::aem::DescriptorAudioUnit> const& unit_zero) -> bool
{
    for (uint16_t k = 1; unit_zero && k < UINT16_MAX; ++k) {
        auto const unit = load_descriptor<atdecc::aem::DescriptorAudioUnit>(host, atdecc::aem::DESCRIPTOR_AUDIO_UNIT, k);
        if (!unit) {
            return false;
        }
        if (unit->clock_domain_index == unit_zero->clock_domain_index) {
            return true;
        }
    }
    return false;
}

}  // namespace

AvbEntityKit::AvbEntityKit(
    AvbEntityAudioIOConfig const& config,
    nanoavb::DescriptorStorageHandler& handler,
    StreamSpecs talker_specs,
    StreamSpecs listener_specs,
    uint32_t const sample_rate,
    uint16_t const initial_clock_source,
    std::optional<uint16_t> const crf_clock_source_index,
    ptpclient::MediaClockGenerator& media_clock,
    std::atomic<int64_t>& last_gptp_ns,
    StreamRxAudioSink* audio_sink,
    std::pmr::memory_resource* memory_resource)
    : config_{config}
    , talker_specs_{talker_specs}
    , listener_specs_{listener_specs}
    , sample_rate_{sample_rate}
    , initial_rate_{sample_rate}
    , storage_handler_{handler}
    , host_{handler, default_adp_advertiser_config(), talker_specs_.size(), 4, listener_specs_.size()}
    , unit_zero_{load_descriptor<atdecc::aem::DescriptorAudioUnit>(host_, atdecc::aem::DESCRIPTOR_AUDIO_UNIT, 0)}
    , unit_zero_shares_domain_{shares_clock_domain(host_, unit_zero_)}
    , gate_{config.gate_talker_on_listener, host_.components()}
    , declarations_{host_.components().profile.milan}
    , talker_{std::make_unique<TalkerStreams>(
          TalkerStreamsConfig{.sample_rate = sample_rate, .vlan_id = config.vlan_id, .stream_pcp = config.stream_pcp},
          media_clock,
          last_gptp_ns,
          memory_resource)}
    , active_clock_source_{initial_clock_source}
    , crf_clock_source_index_{crf_clock_source_index}
{
    if (!listener_specs_.empty()) {
        listener_ = std::make_unique<ListenerStreams>(config_.lock_tolerance_ns, host_.components(), last_gptp_ns, audio_sink);
        listener_->set_presentation_offset_ns(config_.presentation_offset_ns);
    }

    configure_milan();
    // ATDECC descriptor wire version. Default "2016": emit descriptors at their
    // 1722.1-2013/2016 lengths so controllers that reject 2021-length descriptors
    // can enumerate the model. "2021" emits the full 2021 forms.
    host_.components().aem_handler.set_legacy_2016(config_.atdecc_version != "2021");

    configure_talker_streams();
    configure_class_a_domain();
    wire_controller_hooks();
}

void AvbEntityKit::configure_milan()
{
    // GET_MILAN_INFO's features, fixed before the entity is advertised (Milan v1.3 Section 5.4.4.1).
    host_.components().mvu_handler.set_extra_features(
        config_.signal_presence ? atdecc::mvu::MVU_FEATURES_TALKER_SIGNAL_PRESENCE : 0U);
    // Advertised, so reported from the start: no signal until the application says otherwise.
    for (auto const& spec : talker_specs_) {
        output_counters_[spec.index].presence = reports_signal_presence() ? std::optional<uint64_t>{0} : std::nullopt;
    }
}

void AvbEntityKit::configure_talker_streams()
{
    // Base the talker stream_ids on the NIC MAC (globally unique per box), and
    // seed the static dest MACs by kind (MAAP mode reassigns after acquisition).
    ieee::Eui48 const stream_base_mac = stream_base_mac_for(config_.interface_name, config_.entity_id);
    for (auto const& spec : talker_specs_) {
        (void)host_.components().acmp_talker.configure_stream(
            spec.index, stream_id_for(stream_base_mac, spec.index), talker_dest_mac(config_, spec.format.kind), config_.vlan_id);
        notify_talker_interface(spec.index);
        // Static destination addresses are valid from the start; MAAP's once
        // its block is defended (start() and the acquirer's callbacks).
        declarations_.notify_address_valid(spec.index, config_.stream_address_mode != "maap");
    }
    // Milan v1.2 Section 5.3.7.6: every stream output starts at the
    // configured presentation offset (2 ms by default), within the range a
    // controller may set (0..0x7FFFFFFF ns).
    for (auto& offset : presentation_offset_ns_) {
        offset.store(default_presentation_offset_ns(), std::memory_order_relaxed);
    }
    // The name the entity was given (a saved one replaces it only while the
    // configuration still says this).
    configured_entity_name_ = std::string{storage_handler_.entity_name().as_string_view()};
    host_.set_on_grandmaster_transition([this](GrandmasterTransition const transition) { follow_grandmaster(transition); });
}

void AvbEntityKit::follow_grandmaster(GrandmasterTransition const transition) noexcept
{
    switch (transition) {  // NOLINT(statusbar-block-statements) one case per transition
        case GrandmasterTransition::changed:
            talker_->notify_grandmaster_changed(net::monotonic_ns());  // Milan v1.3 Annex B.1.1's window
            return;
        case GrandmasterTransition::lost:
            talker_->notify_grandmaster_lost();
            return;
        case GrandmasterTransition::own:
            talker_->notify_own_grandmaster();
            return;
        case GrandmasterTransition::unchanged:
            return;
    }
}

void AvbEntityKit::lose_announced_grandmaster()
{
    if (host_.own_grandmaster_in_force()) {
        talker_->notify_own_grandmaster();  // a running window cut short
        return;
    }
    talker_->notify_grandmaster_lost();
}

void AvbEntityKit::notify_talker_interface(uint16_t const index)
{
    auto const stream = load_descriptor<atdecc::aem::DescriptorStream>(host_, DESCRIPTOR_STREAM_OUTPUT, index);
    if (index >= talker_interface_.size() || !stream) {
        return;
    }
    talker_interface_[index] = stream->avb_interface_index.get();
    if (talker_interface_[index] != 0) {
        // The entity runs on one AVB interface (0): probes for this
        // output will be refused INCOMPATIBLE_REQUEST.
        host_.ctl_log().warning(
            "kit: STREAM_OUTPUT {} names AVB_INTERFACE {}; the entity has only interface 0", index, talker_interface_[index]);
    }
}

void AvbEntityKit::configure_class_a_domain()
{
    // The VID itself is registered at start() (stop() withdraws it).
    host_.components().msrp_handler.set_domain(
        DomainInfo{.sr_class_id = 6, .sr_class_priority = 3, .sr_class_vid = config_.vlan_id});
    // The Class A VID in use (adopted MSRP Domain): tag stream frames with
    // it, and keep it on the NIC's VLAN filter for the listener streams (a
    // no-op until bind_listener_vlan()).
    host_.set_on_domain_vid_changed([this](uint16_t const vlan_id, TimePoint const time) {
        talker_->set_tag(stream_tag(vlan_id));
#if defined(__linux__)
        // The new VID held before the old is dropped: a link an input also holds stays as it is.
        vlan_filters_.hold(vlan_id);
        vlan_filters_.drop(std::exchange(class_a_filtered_, vlan_id));
#endif
        // The host withdrew the old Class A VID: keep the ones settled
        // listener streams still use.
        for (auto const& [vid, count] : sink_vlans_) {
            (void)host_.components().mvrp_handler.register_vlan(vid, time);
        }
    });
    host_.components().msrp_handler.set_redeclare_registered_listeners(config_.redeclare_registered_listeners);
    host_.components().msrp_handler.set_suppress_leaveall(config_.suppress_leaveall);
}

auto AvbEntityKit::stream_tag(uint16_t const vlan_id) const noexcept -> StreamTag
{
    // The SR class priority in use when adopted from a Domain, else the configured PCP.
    auto const& msrp = host_.components().msrp_handler;
    bool const adopted = msrp.domain().sr_class_priority != msrp.configured_domain().sr_class_priority;
    return StreamTag{.vlan_id = vlan_id, .pcp = adopted ? msrp.domain().sr_class_priority : config_.stream_pcp};
}

void AvbEntityKit::wire_controller_hooks()
{
    wire_storage_hooks();
    wire_dynamic_mappings();
    // Every local change through the AEM handler, except a clock source the host has applied (request_clock_source).
    host_.components().aem_handler.set_local_change_refusal(
        [this] { return applying_local_clock_source_ ? std::nullopt : local_change_refusal(); });
    // The Milan talker's and listener's hooks need no network.
    wire_milan_talker();
    wire_milan_listener();
}

void AvbEntityKit::wire_storage_hooks()
{
    // Storage-handler built-in hooks (all reactor thread):
    // Per-control dispatch (kit phase 4): the generic CONTROL built-in calls
    // back with each accepted SET_CONTROL; bound symbols get their handler's
    // verdict, everything else is accepted (store/serve only).
    storage_handler_.set_on_control_changed([this](uint16_t control_index, std::span<uint8_t const> value) -> uint8_t {
        for (auto& binding : control_bindings_) {
            if (binding.resolved && binding.control_index == control_index && binding.fn) {
                return binding.fn(value);
            }
        }
        return atdecc::AEM_STATUS_SUCCESS;
    });
    // Kit phase 5: a controller's STOP_STREAMING gates the talker slot
    // (the media thread treats it as a closed SRP gate); START reopens.
    // The change shows in GET_STREAM_INFO as STREAMING_WAIT (IEEE 1722.1-2021
    // Table 7-145), so every subscribed controller is told by an unsolicited
    // GET_STREAM_INFO response (7.5.2), beside the command's own echo.
    storage_handler_.set_on_streaming_changed([this](uint16_t type, uint16_t index, bool streaming) -> uint8_t {
        if (type == DESCRIPTOR_STREAM_OUTPUT) {
            talker_->set_stream_stopped(index, !streaming);
            notify_admission(net::rx_clock_now());  // STREAM_START / STREAM_STOP now
            (void)host_.components().aem_handler.notify_stream_info_changed(type, index);
        }
        // Milan v1.2 Sections 5.4.2.19-20: a bound input becomes started or
        // stopped (no effect on an unbound one); the AEM lock was checked.
        if (type == DESCRIPTOR_STREAM_INPUT && host_.components().profile.milan) {
            (void)host_.components().acmp_listener.milan_sinks().set_started(index, streaming);
        }
        return atdecc::AEM_STATUS_SUCCESS;
    });
    // SET_STREAM_FORMAT: the kit's checks, then the entity's data plane (see the
    // header), so no descriptor reports a format the streams are not using (and
    // none is saved). SET_SAMPLING_RATE re-times AUDIO_UNIT 0's streams through it.
    storage_handler_.set_on_stream_format_changed(
        [this](uint16_t const type, uint16_t const index, uint64_t const format) -> uint8_t {
            return change_stream_format(type, index, format);
        });
    storage_handler_.set_on_sampling_rate_changed(
        [this](uint16_t const unit, uint32_t const rate) -> uint8_t { return change_sampling_rate(unit, rate); });
    // SET_CLOCK_SOURCE (IEEE 1722.1-2021 Clause 7.4.23) on any CLOCK_DOMAIN: the entity's hook
    // decides; domain 0's active source is what media_rate() reads per tick.
    storage_handler_.set_on_clock_source_changed([this](uint16_t const domain, uint16_t const source) -> uint8_t {
        if (on_clock_source_changed_ && !applying_local_clock_source_) {
            if (auto const status = on_clock_source_changed_(domain, source); status != atdecc::AEM_STATUS_SUCCESS) {
                return status;
            }
        }
        if (domain == 0) {
            active_clock_source_.store(source, std::memory_order_release);
        }
        return atdecc::AEM_STATUS_SUCCESS;
    });
}

void AvbEntityKit::wire_dynamic_mappings()
{
    // Dynamic input mappings (Milan v1.2 Section 5.3.10): each port starts at its default
    // (the identity the data plane routes); a controller's change goes
    // through the entity's hook.
    auto& aem = host_.components().aem_handler;
    for (uint16_t port = 0; port < UINT16_MAX && host_.get_descriptor(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port).has_value();
         ++port) {
        if (aem.has_dynamic_input_map(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port)) {
            dynamic_input_ports_.push_back(port);
        }
    }
    reset_input_mappings();
    // Dynamic output ports carry the kit's identity map while their stream is at the unit's rate.
    for (uint16_t port = 0; port < UINT16_MAX && host_.get_descriptor(atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT, port).has_value();
         ++port) {
        if (aem.has_dynamic_output_map(port)) {
            dynamic_output_ports_.push_back(port);
        }
    }
    refresh_output_mappings();
    aem.set_on_audio_mappings_changed(
        [this](uint16_t const port_type, uint16_t const port_index, std::span<nanoavb::DynamicAudioMapping const> const mappings)
            -> uint8_t {
            if (port_type != atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT || !on_input_mappings_changed_) {
                return atdecc::AEM_STATUS_NOT_SUPPORTED;
            }
            auto const status = on_input_mappings_changed_(port_index, mappings);
            if (status == atdecc::AEM_STATUS_SUCCESS) {
                host_.ctl_log().status("aem: STREAM_PORT_INPUT {}: {} dynamic mapping(s)", port_index, mappings.size());
            }
            return status;
        });
}

auto AvbEntityKit::input_mappings(uint16_t const port_index) const -> nanoavb::DynamicAudioMaps::Mappings
{
    return host_.components().aem_handler.dynamic_audio_maps().port(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port_index);
}

auto AvbEntityKit::output_mappings(uint16_t const port_index) const -> nanoavb::DynamicAudioMaps::Mappings
{
    return host_.components().aem_handler.output_audio_maps().port(atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT, port_index);
}

auto AvbEntityKit::default_input_mappings(uint16_t const port_index) const -> nanoavb::DynamicAudioMaps::Mappings
{
    return identity_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port_index);
}

auto AvbEntityKit::default_output_mappings(uint16_t const port_index) const -> nanoavb::DynamicAudioMaps::Mappings
{
    return identity_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT, port_index);
}

auto AvbEntityKit::identity_mappings(uint16_t const port_type, uint16_t const port_index) const
    -> nanoavb::DynamicAudioMaps::Mappings
{
    auto const* const spec = identity_spec(port_type, port_index);
    auto const port = load_descriptor<atdecc::aem::DescriptorStreamPort>(host_, port_type, port_index);
    if (spec == nullptr || !port) {
        return {};
    }
    return cluster_identity_mappings(port_index, *port, spec->format.channels);
}

auto AvbEntityKit::identity_spec(uint16_t const port_type, uint16_t const port_index) const -> StreamSpec const*
{
    bool const output = port_type == atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT;
    auto const& specs = output ? talker_specs_ : listener_specs_;
    auto const* const spec = std::ranges::find(specs, port_index, &StreamSpec::index);
    // Milan v1.2 Section 5.4.2.27: none at another rate than the unit's, unless the port converts rates (Milan v1.3
    // Section 5.3.3.7) or outputs opted out.
    bool const at_rate = spec != specs.end() &&
        (spec->format.sample_rate_hz == unit_rate_of(output, port_index) || port_converts(port_type, port_index) ||
         (output && output_mappings_across_rates_));
    return at_rate ? &*spec : nullptr;
}

auto AvbEntityKit::cluster_identity_mappings(
    uint16_t const port_index, atdecc::aem::DescriptorStreamPort const& port, uint16_t const channels) const
    -> nanoavb::DynamicAudioMaps::Mappings
{
    nanoavb::DynamicAudioMaps::Mappings out{};
    uint16_t stream_channel = 0;
    for (uint16_t offset = 0; offset < port.number_of_clusters.get() && stream_channel < channels; ++offset) {
        auto const cluster = audio_cluster(host_, port, offset);
        if (!cluster) {
            break;
        }
        for (uint16_t ch = 0; ch < cluster->channel_count.get() && stream_channel < channels &&
             out.size() < nanoavb::DynamicAudioMaps::Mappings::capacity();
             ++ch) {
            out.push_back(
                {.stream_index = port_index, .stream_channel = stream_channel++, .cluster_offset = offset, .cluster_channel = ch});
        }
    }
    return out;
}

auto AvbEntityKit::input_cluster_channel(uint16_t const port_index, nanoavb::DynamicAudioMapping const& mapping) const
    -> std::optional<uint16_t>
{
    auto const port =
        load_descriptor<atdecc::aem::DescriptorStreamPort>(host_, atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port_index);
    if (!port || mapping.cluster_offset >= port->number_of_clusters.get()) {
        return std::nullopt;
    }
    uint32_t ordinal = 0;
    for (uint16_t offset = 0; offset <= mapping.cluster_offset; ++offset) {
        auto const cluster = audio_cluster(host_, *port, offset);
        if (!cluster) {
            return std::nullopt;
        }
        uint16_t const channels = cluster->channel_count.get();
        if (offset == mapping.cluster_offset) {
            if (mapping.cluster_channel >= channels) {
                return std::nullopt;
            }
            ordinal += mapping.cluster_channel;
        } else {
            ordinal += channels;
        }
    }
    return ordinal <= UINT16_MAX ? std::optional{static_cast<uint16_t>(ordinal)} : std::nullopt;
}

auto AvbEntityKit::fall_back_to_default_mappings(uint16_t const port_index) -> nanoavb::DynamicAudioMaps::Mappings
{
    auto& aem = host_.components().aem_handler;
    auto mappings = default_input_mappings(port_index);
    if (aem.set_audio_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port_index, mappings, false) !=
        atdecc::AEM_STATUS_SUCCESS) {
        mappings.clear();
        (void)aem.set_audio_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port_index, {}, false);
    }
    host_.ctl_log().warning(
        "kit: STREAM_PORT_INPUT {}: the entity cannot route its mappings: {} default mapping(s) instead",
        port_index,
        mappings.size());
    return mappings;
}

void AvbEntityKit::reset_input_mappings()
{
    auto& aem = host_.components().aem_handler;
    for (auto const port : dynamic_input_ports_) {
        auto const mappings = default_input_mappings(port);
        if (auto const status = aem.set_audio_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port, mappings, /*apply=*/false);
            status != atdecc::AEM_STATUS_SUCCESS) {
            host_.ctl_log().warning("kit: STREAM_PORT_INPUT {}: its default mappings were refused (status {})", port, status);
            (void)aem.set_audio_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port, {}, false);
        }
    }
}

void AvbEntityKit::refresh_output_mappings()
{
    auto& aem = host_.components().aem_handler;
    for (auto const port : dynamic_output_ports_) {
        if (auto const status = aem.set_output_mappings(port, default_output_mappings(port));
            status != atdecc::AEM_STATUS_SUCCESS) {
            host_.ctl_log().warning(
                "kit: STREAM_PORT_OUTPUT {}: its identity mappings were refused (status {}): unmapped", port, status);
            (void)aem.set_output_mappings(port, {});
        }
    }
}

void AvbEntityKit::apply_saved_audio_maps(PersistentState const& saved)
{
    auto& aem = host_.components().aem_handler;
    using atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT;
    // Through the entity's hook when there is one (a restarted kit's data
    // plane may route another map); without it validated and kept only: the
    // entity routes kit.input_mappings() when its data plane comes up.
    bool const apply = static_cast<bool>(on_input_mappings_changed_);
    for (auto it = saved.audio_maps.begin(); it != saved.audio_maps.end(); ++it) {
        auto const& map = *it;
        bool const repeated = std::any_of(saved.audio_maps.begin(), it, [&map](PersistedAudioMap const& m) {
            return m.descriptor_type == map.descriptor_type && m.descriptor_index == map.descriptor_index;
        });
        if (map.descriptor_type != DESCRIPTOR_STREAM_PORT_INPUT || !has_dynamic_input_map(map.descriptor_index) || repeated) {
            host_.ctl_log().warning(
                "kit: saved audio mappings of descriptor {}:{}: {}: dropped",
                map.descriptor_type,
                map.descriptor_index,
                logging::static_str(repeated ? "listed again" : "no such dynamic input port"));
        }
    }
    for (auto const port : dynamic_input_ports_) {
        // Each port once: its saved set, else (none, or refused) its default
        // for the formats now in force, else (refused too) unmapped.
        auto const saved_map = std::ranges::find_if(saved.audio_maps, [port](PersistedAudioMap const& m) {
            return m.descriptor_type == DESCRIPTOR_STREAM_PORT_INPUT && m.descriptor_index == port;
        });
        if (saved_map != saved.audio_maps.end()) {
            nanoavb::DynamicAudioMaps::Mappings mappings{};
            for (auto const& m : saved_map->mappings) {
                if (mappings.size() < nanoavb::DynamicAudioMaps::Mappings::capacity()) {
                    mappings.push_back(
                        {.stream_index = m.stream_index,
                         .stream_channel = m.stream_channel,
                         .cluster_offset = m.cluster_offset,
                         .cluster_channel = m.cluster_channel});
                }
            }
            auto const status = aem.set_audio_mappings(DESCRIPTOR_STREAM_PORT_INPUT, port, mappings, apply);
            if (status == atdecc::AEM_STATUS_SUCCESS) {
                continue;
            }
            host_.ctl_log().warning(
                "kit: saved audio mappings of STREAM_PORT_INPUT {} refused (status {}): the default", port, status);
        }
        if (auto const status = aem.set_audio_mappings(DESCRIPTOR_STREAM_PORT_INPUT, port, default_input_mappings(port), apply);
            status != atdecc::AEM_STATUS_SUCCESS) {
            // Unmapped (silent), and the data plane told so: never a map
            // the entity reports but does not route.
            host_.ctl_log().warning(
                "kit: STREAM_PORT_INPUT {}: its default mappings were refused (status {}): unmapped", port, status);
            (void)aem.set_audio_mappings(DESCRIPTOR_STREAM_PORT_INPUT, port, {}, apply);
        }
    }
}

auto AvbEntityKit::output_static_map_channels(uint16_t const stream_index) const -> uint16_t
{
    return static_map_channels(atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT, stream_index, /*unconverted_only=*/false);
}

auto AvbEntityKit::static_map_channels(uint16_t const port_type, uint16_t const stream_index, bool const unconverted_only) const
    -> uint16_t
{
    // Every such port's static AUDIO_MAPs (the model's), or only those of ports that convert no
    // rates: the channels of stream @p stream_index of the port's direction they name.
    uint16_t needed = 0;
    for (uint16_t p = 0; p < UINT16_MAX; ++p) {
        auto const port = load_descriptor<atdecc::aem::DescriptorStreamPort>(host_, port_type, p);
        if (!port) {
            break;
        }
        uint16_t const maps = unconverted_only && converts_rates(*port) ? uint16_t{0} : port->number_of_maps.get();
        for (uint16_t i = 0; i < maps; ++i) {
            auto map = load_descriptor<atdecc::aem::DescriptorAudioMap>(
                host_, atdecc::aem::DESCRIPTOR_AUDIO_MAP, static_cast<uint16_t>(port->base_map.get() + i));
            if (!map) {
                continue;
            }
            map->clamp_trailer_count();
            for (auto const& m : map->used_mappings()) {
                if (m.mapping_stream_index.get() == stream_index) {
                    needed = std::max<uint16_t>(needed, static_cast<uint16_t>(m.mapping_stream_channel.get() + 1));
                }
            }
        }
    }
    return needed;
}

auto AvbEntityKit::stream_mapped(bool const output, uint16_t const index) const -> bool
{
    uint16_t const port_type = output ? atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT : atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT;
    if (static_map_channels(port_type, index, /*unconverted_only=*/true) != 0) {
        return true;
    }
    bool named = false;
    if (!output) {
        host_.components().aem_handler.dynamic_audio_maps().for_each(
            [this, &named, index](uint16_t const type, uint16_t const port_index, nanoavb::DynamicAudioMapping const& m) {
                named = named ||
                    (type == atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT && m.stream_index == index &&
                     !port_converts(type, port_index));
            });
    }
    return named;
}

auto AvbEntityKit::identity_installed(uint16_t const index) const -> bool
{
    bool named = false;
    host_.components().aem_handler.output_audio_maps().for_each(
        [this, &named, index](uint16_t const type, uint16_t const port_index, nanoavb::DynamicAudioMapping const& m) {
            named = named || (m.stream_index == index && !port_converts(type, port_index));
        });
    return named;
}

auto AvbEntityKit::port_converts(uint16_t const port_type, uint16_t const port_index) const -> bool
{
    auto const port = load_descriptor<atdecc::aem::DescriptorStreamPort>(host_, port_type, port_index);
    return port.has_value() && converts_rates(*port);
}

auto AvbEntityKit::held_at_unit_rate(bool const output, uint16_t const index) const -> bool
{
    // Milan v1.2 Sections 5.4.2.7, 5.4.2.27: a stream mapped through a port that converts no rates keeps the unit's
    // rate. That rule is a should, not part of Section 5.3.3.4's validity, so an entity may opt its outputs out of it.
    return !(output && output_mappings_across_rates_) && stream_mapped(output, index);
}

auto AvbEntityKit::input_running(uint16_t const idx) const noexcept -> bool
{
    auto const& components = host_.components();
    if (components.profile.milan) {
        auto const* sink = components.acmp_listener.milan_sinks().sink(idx);
        return sink != nullptr && sink->bound();
    }
    auto const* stream = components.acmp_listener.get_stream(idx);
    return stream != nullptr && stream->connected;
}

auto AvbEntityKit::output_streaming(uint16_t const idx, GateTimePoint const now) const noexcept -> bool
{
    return talker_should_transmit(idx, now) || (declarations_.declared(idx) && gate_.listener_admitted(idx, now));
}

auto AvbEntityKit::reshape_stream(bool const output, StreamSpec const& spec) -> Status
{
    if (output) {
        return talker_->reformat_stream(spec);
    }
    return listener_ != nullptr ? listener_->reformat_stream(spec) : Status{};
}

auto AvbEntityKit::change_stream_format(uint16_t const type, uint16_t const index, uint64_t const format) -> uint8_t
{
    auto& specs = type == DESCRIPTOR_STREAM_OUTPUT ? talker_specs_ : listener_specs_;
    auto* const spec = std::ranges::find(specs, index, &StreamSpec::index);
    if (spec == specs.end()) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;  // a stream the kit does not run
    }
    if (spec->format_word == format) {
        return atdecc::AEM_STATUS_SUCCESS;
    }
    return reformat_stream(type, *spec, with_format(*spec, format));
}

auto AvbEntityKit::reformat_stream(uint16_t const type, StreamSpec& spec, StreamSpec const& updated) -> uint8_t
{
    bool const output = type == DESCRIPTOR_STREAM_OUTPUT;
    if (auto const status = stream_format_refusal(output, spec, updated); status != atdecc::AEM_STATUS_SUCCESS) {
        return status;
    }
    uint64_t const before = spec.format_word;
    if (auto const status = install_stream_format(type, output, spec, updated); status != atdecc::AEM_STATUS_SUCCESS) {
        return status;
    }
    announce_stream_format(type, before, updated);
    return atdecc::AEM_STATUS_SUCCESS;
}

auto AvbEntityKit::stream_format_refusal(bool const output, StreamSpec const& spec, StreamSpec const& updated) const -> uint8_t
{
    // An audio stream's format changes at any rate and between AAF and AM824; a CRF's never.
    if (!kind_change_allowed(spec.format.kind, updated.format.kind)) {
        return atdecc::AEM_STATUS_BAD_ARGUMENTS;  // a listed format this stream cannot carry (IEEE 1722.1-2021 Clause 7.4.9)
    }
    // The talker sends AAF in class A packets and sizes its TSpec so (IEEE 1722-2016 Clause 7.7, Table 17).
    if (!is_audio_kind(updated.format.kind) || (output && !class_a_framed(updated.format))) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;
    }
    // Milan v1.2 Section 5.4.2.7.
    if (output ? output_streaming(spec.index, net::rx_clock_now()) : input_running(spec.index)) {
        return atdecc::AEM_STATUS_STREAM_IS_RUNNING;
    }
    if (!mappings_allow_format(output, updated)) {
        return atdecc::AEM_STATUS_BAD_ARGUMENTS;
    }
    if (!on_stream_format_changed_) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;
    }
    // IEEE 802.1Q-2022 Clause 34.3.1: class A's share of the link. Milan v1.2 Section
    // 5.4.2.7 names no status for it; NO_RESOURCES says what is short.
    return output && !talkers_fit(spec.index, updated.format_word) ? atdecc::AEM_STATUS_NO_RESOURCES : atdecc::AEM_STATUS_SUCCESS;
}

auto AvbEntityKit::mappings_allow_format(bool const output, StreamSpec const& updated) const -> bool
{
    if (updated.format.sample_rate_hz != unit_rate_of(output, updated.index) && held_at_unit_rate(output, updated.index)) {
        return false;
    }
    // ... and no mapped channel of the stream may go: an output's static
    // map is the model's (the generator lists no narrower format; this is
    // the backstop), an input's dynamic ones the controller removes first.
    if (output) {
        return updated.format.channels >= output_static_map_channels(updated.index);
    }
    bool lost = false;
    host_.components().aem_handler.dynamic_audio_maps().for_each(
        [&](uint16_t const port_type, uint16_t /*port_index*/, nanoavb::DynamicAudioMapping const& m) {
            lost = lost ||
                (port_type == atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT && m.stream_index == updated.index &&
                 m.stream_channel >= updated.format.channels);
        });
    return !lost;
}

auto AvbEntityKit::install_stream_format(uint16_t const type, bool const output, StreamSpec& spec, StreamSpec const& updated)
    -> uint8_t
{
    // The stream path first (it may refuse the format; no slot before
    // start(), which opens it with the spec), then the entity's data plane;
    // either refusing leaves both as they were.
    auto const original = spec;
    if (auto const reshaped = reshape_stream(output, updated); !reshaped && reshaped.error() != std::errc::no_such_device) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;
    }
    if (auto const status = on_stream_format_changed_(type, spec.index, updated.format); status != atdecc::AEM_STATUS_SUCCESS) {
        (void)reshape_stream(output, original);
        return status;
    }
    spec = updated;
    if (updated.format.kind != original.format.kind) {
        rejoin_static_rx_groups();
    }
    return atdecc::AEM_STATUS_SUCCESS;
}

void AvbEntityKit::announce_stream_format(uint16_t const type, uint64_t const before, StreamSpec const& updated)
{
    bool const output = type == DESCRIPTOR_STREAM_OUTPUT;
    if (output) {
        // The handler records the format after this hook; the identity it maps is checked against it.
        (void)storage_handler_.set_runtime_stream_format(
            {.configuration_index = 0, .descriptor_type = type, .descriptor_index = updated.index},
            ieee::Eui64{}.from_uint64(updated.format_word));
        if (!retiming_) {
            refresh_output_mappings();  // a re-time refreshes them once, at its end
        }
    }
    host_.ctl_log().status(
        "aem: {} stream {} format {:016x} -> {:016x}",
        output ? logging::lit("talker") : logging::lit("listener"),
        updated.index,
        before,
        updated.format_word);
    if (output && declarations_.declared(updated.index)) {
        refresh_pending_.set(updated.index);  // its TSpec, re-declared at the next pass (this hook is told no time)
    }
    if (output && reports_signal_presence()) {
        output_counters_[updated.index].narrow_presence(updated.format.channels);  // its presence, at the next fold
    }
}

auto AvbEntityKit::static_rx_group(StreamSpec const& spec) const noexcept -> ieee::Eui48
{
    auto const* const slot = is_audio_kind(spec.format.kind) ? std::as_const(*talker_).slot_of(spec.format.kind) : nullptr;
    return slot != nullptr ? slot->dest_mac : ieee::Eui48{};
}

auto AvbEntityKit::crf_recovery_input() const -> std::optional<uint16_t>
{
    auto const is_crf_input = [this](uint16_t const index) {
        return std::ranges::any_of(listener_specs_, [index](StreamSpec const& spec) {
            return spec.index == index && spec.format.kind == StreamKind::crf;
        });
    };
    if (crf_clock_source_index_) {
        auto const source = load_descriptor<atdecc::aem::DescriptorClockSource>(
            host_, atdecc::aem::DESCRIPTOR_CLOCK_SOURCE, *crf_clock_source_index_);
        if (source && source->clock_source_location_type == atdecc::aem::DESCRIPTOR_STREAM_INPUT &&
            is_crf_input(source->clock_source_location_index.get())) {
            return source->clock_source_location_index.get();
        }
    }
    auto const* const first =
        std::ranges::find(listener_specs_, StreamKind::crf, [](StreamSpec const& spec) { return spec.format.kind; });
    return first == listener_specs_.end() ? std::nullopt : std::optional{first->index};
}

void AvbEntityKit::rejoin_static_rx_groups()
{
    if (listener_ == nullptr || !stream_port_.valid()) {
        return;
    }
    // No guard: a join or leave is a socket call and touches no ring, so neither the media thread nor
    // the drain need wait on it. The port counts memberships: one per input, a changed group swapped.
    for (size_t i = 0; i < rx_static_groups_.size() && i < listener_specs_.size(); ++i) {
        auto const group = static_rx_group(listener_specs_[i]);
        if (group != rx_static_groups_[i]) {
            swap_static_rx_group(rx_static_groups_[i], group);
        }
    }
}

void AvbEntityKit::swap_static_rx_group(ieee::Eui48& held, ieee::Eui48 const& group)
{
    if (held.is_set()) {
        (void)stream_port_.port().leave_multicast(held);
    }
    if (group.is_set()) {
        (void)stream_port_.port().join_multicast(group);
    }
    held = group;
}

auto AvbEntityKit::talkers_fit(uint16_t const index, uint64_t const word) const noexcept -> bool
{
    std::array const changed{RetimedFormat{.type = DESCRIPTOR_STREAM_OUTPUT, .index = index, .to = word}};
    return talkers_fit(changed);
}

auto AvbEntityKit::talkers_fit(std::span<RetimedFormat const> const changed) const noexcept -> bool
{
    auto const kbps = [this, changed](StreamSpec const& s) {
        auto const it = std::ranges::find_if(
            changed, [&s](RetimedFormat const& r) { return r.type == DESCRIPTOR_STREAM_OUTPUT && r.index == s.index; });
        return uint64_t{
            stream_bandwidth_kbps(talker_tspec(it == changed.end() ? s : with_format(s, it->to)), CLASS_A_INTERVALS_PER_SEC)};
    };
    uint64_t before = 0;
    uint64_t after = 0;
    for (auto const& spec : talker_specs_) {
        before += uint64_t{stream_bandwidth_kbps(talker_tspec(spec), CLASS_A_INTERVALS_PER_SEC)};
        after += kbps(spec);
    }
    return after <= before || after <= static_cast<uint64_t>(sr_budget_kbps(link_mbps_ * 1000U));
}

auto AvbEntityKit::change_sampling_rate(uint16_t const audio_unit, uint32_t const rate) -> uint8_t
{
    // IEEE 1722.1-2021 Clause 7.4.21: the rate in force is a no-op; the kit re-times AUDIO_UNIT 0 alone.
    auto const in_force = storage_handler_.current_sampling_rate(
        {.configuration_index = 0, .descriptor_type = atdecc::aem::DESCRIPTOR_AUDIO_UNIT, .descriptor_index = audio_unit});
    if (in_force == rate) {
        return atdecc::AEM_STATUS_SUCCESS;
    }
    if (audio_unit != 0 || !in_force || !on_unit_rate_changed_ || unit_zero_shares_domain_) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;
    }
    auto const retimed = retimed_formats(*in_force, rate);
    return retimed ? retime_streams(rate, *retimed) : retimed.error();
}

auto AvbEntityKit::retimed_formats(uint32_t const from, uint32_t const rate) const -> std::expected<RetimedFormats, uint8_t>
{
    RetimedFormats out{};
    for (bool const output : {true, false}) {
        for (auto const& spec : output ? talker_specs_ : listener_specs_) {
            if (auto const status = add_retimed(output, spec, from, rate, out); status != atdecc::AEM_STATUS_SUCCESS) {
                return std::unexpected(status);
            }
        }
    }
    // IEEE 802.1Q-2022 Clause 34.3.1, checked before the data plane moves (as each SET_STREAM_FORMAT would be).
    return talkers_fit(out) ? std::expected<RetimedFormats, uint8_t>{out} : std::unexpected(atdecc::AEM_STATUS_NO_RESOURCES);
}

auto AvbEntityKit::add_retimed(
    bool const output, StreamSpec const& spec, uint32_t const from, uint32_t const rate, RetimedFormats& out) const -> uint8_t
{
    if (!is_audio_kind(spec.format.kind) || spec.format.sample_rate_hz != from || !in_unit_zero(output, spec.index)) {
        return atdecc::AEM_STATUS_SUCCESS;  // a CRF's format stays, as does one at another rate or of another unit
    }
    // Milan v1.3 Section 5.4.2.13: one that cannot follow (running, or listing no such format) stays, unless a
    // port that converts no rates maps it, the identity the kit installed on an output port included.
    auto const word = following_format(output, spec, rate);
    if (!word) {
        bool const mapped = stream_mapped(output, spec.index) || (output && identity_installed(spec.index));
        return mapped ? atdecc::AEM_STATUS_NOT_SUPPORTED : atdecc::AEM_STATUS_SUCCESS;
    }
    (void)out.try_push_back(
        {.type = output ? DESCRIPTOR_STREAM_OUTPUT : DESCRIPTOR_STREAM_INPUT,
         .index = spec.index,
         .from = spec.format_word,
         .to = *word});
    return atdecc::AEM_STATUS_SUCCESS;
}

auto AvbEntityKit::following_format(bool const output, StreamSpec const& spec, uint32_t const rate) const -> std::optional<uint64_t>
{
    // Milan v1.2 Section 5.4.2.7: a bound input or a streaming output keeps its format.
    if (output ? output_streaming(spec.index, net::rx_clock_now()) : input_running(spec.index)) {
        return std::nullopt;
    }
    uint16_t const type = output ? DESCRIPTOR_STREAM_OUTPUT : DESCRIPTOR_STREAM_INPUT;
    auto const word = aaf_format_at_rate(spec.format_word, rate);
    bool const listed = word.has_value() &&
        storage_handler_.stream_format_listed(
            {.configuration_index = 0, .descriptor_type = type, .descriptor_index = spec.index}, ieee::Eui64{}.from_uint64(*word));
    return listed ? word : std::nullopt;
}

auto AvbEntityKit::in_unit_zero(bool const output, uint16_t const index) const noexcept -> bool
{
    // IEEE 1722.1-2021 Clauses 7.2.3, 7.2.6: a stream runs at the rate of the unit in its clock domain.
    auto const& specs = output ? talker_specs_ : listener_specs_;
    auto const* const spec = std::ranges::find(specs, index, &StreamSpec::index);
    return unit_zero_ && spec != specs.end() && spec->clock_domain_index == unit_zero_->clock_domain_index.get();
}

auto AvbEntityKit::unit_rate_of(bool const output, uint16_t const index) const noexcept -> uint32_t
{
    return in_unit_zero(output, index) ? sample_rate_ : initial_rate_;  // only AUDIO_UNIT 0's rate changes
}

auto AvbEntityKit::retime_streams(uint32_t const rate, RetimedFormats const& retimed) -> uint8_t
{
    // The data plane first, all or none; then each stream through its SET_STREAM_FORMAT path (each logged)
    // with the unit at its new rate (the mapping rules read it); then the output mappings once.
    if (auto const status = on_unit_rate_changed_(rate, retimed); status != atdecc::AEM_STATUS_SUCCESS) {
        return status;
    }
    uint32_t const before = std::exchange(sample_rate_, rate);
    auto const status = retime_all(before, retimed);
    refresh_output_mappings();
    return status;
}

auto AvbEntityKit::retime_all(uint32_t const before, RetimedFormats const& retimed) -> uint8_t
{
    retiming_ = true;  // the output mappings are refreshed once, after
    for (size_t k = 0; k < retimed.size(); ++k) {
        if (auto const status = set_stream_word(retimed[k].type, retimed[k].index, retimed[k].to);
            status != atdecc::AEM_STATUS_SUCCESS) {
            retiming_ = false;
            sample_rate_ = before;  // before the undo: the streams go back to the unit's rate
            undo_retimed(before, retimed, k);
            return status;
        }
    }
    retiming_ = false;
    for (auto const& r : retimed) {
        (void)format_notifications_.try_push_back({r.type, r.index});
    }
    return atdecc::AEM_STATUS_SUCCESS;
}

void AvbEntityKit::undo_retimed(uint32_t const before, RetimedFormats const& retimed, size_t const changed)
{
    for (auto const& r : std::span{retimed}.first(changed)) {
        if (auto const status = set_stream_word(r.type, r.index, r.from); status != atdecc::AEM_STATUS_SUCCESS) {
            host_.ctl_log().warning("aem: stream {}:{} not restored to {:016x} (status {})", r.type, r.index, r.from, status);
        }
    }
    RetimedFormats back{};
    for (auto const& r : retimed) {
        (void)back.try_push_back({.type = r.type, .index = r.index, .from = r.to, .to = r.from});
    }
    if (auto const status = on_unit_rate_changed_(before, back); status != atdecc::AEM_STATUS_SUCCESS) {
        host_.ctl_log().warning("aem: the data plane not restored to {} Hz (status {})", before, status);
    }
}

auto AvbEntityKit::set_stream_word(uint16_t const type, uint16_t const index, uint64_t const word) -> uint8_t
{
    // As a SET_STREAM_FORMAT's: listed, the kit's hook (and the entity's), then the runtime format.
    ieee::Eui64 const value = ieee::Eui64{}.from_uint64(word);
    return storage_handler_.on_set_descriptor_value(
        atdecc::AEM_COMMAND_SET_STREAM_FORMAT, descriptor_id(type, index), make_const_span(value));
}

void AvbEntityKit::flush_format_notifications()
{
    // After the SET_SAMPLING_RATE's response, as if each stream's SET_STREAM_FORMAT had been sent
    // (Milan v1.2 Section 5.4.5.2): the descriptor type and index, then the format.
    for (auto const& [type, index] : std::exchange(format_notifications_, {})) {
        auto const format =
            storage_handler_.current_stream_format({.configuration_index = 0, .descriptor_type = type, .descriptor_index = index});
        std::array<uint8_t, atdecc::aem::AemStreamFormatPayload::LENGTH> body{};
        span_store(std::span{body}, atdecc::aem::AemStreamFormatPayload{.descriptor_type = type, .descriptor_index = index});
        span_store(
            std::span{body}.subspan(offsetof(atdecc::aem::AemStreamFormatPayload, stream_format)), format.value_or(ieee::Eui64{}));
        host_.components().aem_handler.emit_unsolicited(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, body);
    }
}

auto AvbEntityKit::local_change_refusal() const noexcept -> std::optional<uint8_t>
{
    if (!lock_refuses_local_changes_) {
        return std::nullopt;
    }
    return host_.components().aem_handler.exclusive_access_status(ieee::Eui64{});  // as no controller's command
}

auto AvbEntityKit::request_entity_name(std::string_view const name) -> uint8_t
{
    auto& aem = host_.components().aem_handler;
    if (name.size() > atdecc::aem::AtdeccString::LENGTH) {
        return atdecc::AEM_STATUS_BAD_ARGUMENTS;
    }
    // SET_NAME of the ENTITY's entity_name: ENTITY 0, name_index 0, configuration 0, the name.
    std::array<uint8_t, 8 + atdecc::aem::AtdeccString::LENGTH> body{};
    std::ranges::copy(name, body.begin() + 8);
    return aem.apply_local_name(body);
}

auto AvbEntityKit::request_clock_source(uint16_t const clock_domain_index, uint16_t const clock_source_index) -> uint8_t
{
    auto& aem = host_.components().aem_handler;
    auto const body = clock_source_command(clock_domain_index, clock_source_index);
    applying_local_clock_source_ = true;
    auto const status = aem.apply_local_descriptor_value(atdecc::AEM_COMMAND_SET_CLOCK_SOURCE, body);
    applying_local_clock_source_ = false;
    return status;
}

auto AvbEntityKit::request_audio_mappings(
    uint16_t const port_index,
    std::span<nanoavb::DynamicAudioMapping const> const add,
    std::span<nanoavb::DynamicAudioMapping const> const remove) -> uint8_t
{
    return host_.components().aem_handler.apply_local_audio_mappings(port_index, add, remove);
}

void AvbEntityKit::on_control_symbol(uint32_t const symbol_code, ControlChangedFn fn)
{
    ControlBinding binding{};
    binding.symbol = symbol_code;
    binding.fn = std::move(fn);
    if (auto const entry = host_.descriptor_for_symbol(symbol_code);
        entry.has_value() && entry->descriptor_type == atdecc::aem::DESCRIPTOR_CONTROL) {
        binding.control_index = entry->descriptor_index;
        binding.resolved = true;
    }
    for (auto& existing : control_bindings_) {
        if (existing.symbol == symbol_code) {
            existing = std::move(binding);
            return;
        }
    }
    if (control_bindings_.size() < MAX_CONTROL_BINDINGS) {
        control_bindings_.push_back(std::move(binding));
    }
}

auto AvbEntityKit::talker_kind_of(uint16_t const index) const noexcept -> StreamKind
{
    for (auto const& spec : talker_specs_) {
        if (spec.index == index) {
            return spec.format.kind;
        }
    }
    return StreamKind::other;
}

auto AvbEntityKit::admitted_talker_streams(GateTimePoint const now) const noexcept -> AdmittedStreams
{
    AdmittedStreams admitted{};
    for (auto const& spec : talker_specs_) {
        if (gate_.admitted(spec.index, now)) {
            (void)admitted.try_push_back(spec.index);
        }
    }
    return admitted;
}

void AvbEntityKit::notify_admission(GateTimePoint const now)
{
    update_output_streaming(now);
    arm_admission_deadline(now);
    auto admitted = admitted_talker_streams(now);
    if (std::ranges::equal(admitted, admitted_)) {
        return;
    }
    admitted_ = admitted;
    if (on_admission_changed_) {
        on_admission_changed_(std::span<uint16_t const>{admitted_.data(), admitted_.size()});
    }
}

void AvbEntityKit::arm_admission_deadline(GateTimePoint const now) noexcept
{
    admission_deadline_.reset();
    for (auto const& spec : talker_specs_) {
        // A grace running out: one not yet past, its listener no longer registered (admitted at its end).
        if (auto const ends = gate_.grace_ends(spec.index); ends && now < *ends && !gate_.listener_admitted(spec.index, *ends)) {
            admission_deadline_ = std::min(admission_deadline_.value_or(*ends), *ends);
        }
    }
}

void AvbEntityKit::on_admission_timer(GateTimePoint const now)
{
    if (admission_deadline_ && now >= *admission_deadline_) {
        notify_admission(now);
    }
}

void AvbEntityKit::update_output_streaming(GateTimePoint const now)
{
    if (talker_ == nullptr) {
        return;
    }
    for (auto const& spec : talker_specs_) {
        bool const on = talker_should_transmit(spec.index, now);
        if (output_counters_[spec.index].set_streaming(on, talker_stream_totals(*talker_, spec.index))) {
            (void)host_.components().aem_handler.notify_counters_changed(DESCRIPTOR_STREAM_OUTPUT, spec.index);
        }
    }
}

void AvbEntityKit::poll_counters(int64_t const now_ns)
{
    if (next_counters_fold_ns_.has_value() && now_ns < *next_counters_fold_ns_) {
        return;
    }
    bool const first = !next_counters_fold_ns_.has_value();
    next_counters_fold_ns_ = now_ns + COUNTERS_INTERVAL_NS;
    if (!first) {  // else the first interval starts now
        fold_stream_counters();
    }
}

void AvbEntityKit::set_output_signal_presence(uint16_t const idx, uint64_t const channels) noexcept
{
    if (reports_signal_presence() && is_talker_index(idx)) {
        output_counters_[idx].set_presence(presence_of(channels, talker_channels(idx)));
    }
}

auto AvbEntityKit::talker_channels(uint16_t const idx) const noexcept -> uint16_t
{
    auto const* const spec = std::ranges::find(talker_specs_, idx, &StreamSpec::index);
    return spec == talker_specs_.end() ? uint16_t{0} : spec->format.channels;
}

void AvbEntityKit::fold_stream_counters()
{
    auto& aem = host_.components().aem_handler;
    if (talker_ != nullptr) {
        for (auto const& spec : talker_specs_) {
            if (output_counters_[spec.index].fold(talker_stream_totals(*talker_, spec.index))) {
                (void)aem.notify_counters_changed(DESCRIPTOR_STREAM_OUTPUT, spec.index);
            }
        }
    }
    if (listener_ != nullptr) {
        for (auto const& spec : listener_specs_) {
            if (listener_->fold_input_counters(spec.index)) {
                (void)aem.notify_counters_changed(DESCRIPTOR_STREAM_INPUT, spec.index);
            }
        }
    }
}

void AvbEntityKit::notify_listener_ready(
    nanoavb::StreamId const& stream_id, bool const ready, int64_t const now_ns, GateTimePoint const gate_now)
{
    auto const idx = talker_index_of(stream_id);
    // Only a declared stream streams (Milan v1.2 Section 5.3.7.3): a Listener
    // registered for a withdrawn stream opens nothing; re-declaring re-learns it.
    if (!idx || declarations_.declared(*idx)) {
        gate_.note_listener_ready(stream_id, ready, gate_now);
    }
    // Any Listener registration keeps the stream declared (Section 4.3.3.1),
    // and GET_STREAM_INFO reports REGISTERING / REGISTERING_FAILED.
    if (!idx || !refresh_talker_listener(*idx)) {
        notify_admission(gate_now);
        return;
    }
    bool const was_declared = declarations_.declared(*idx);
    update_talker_declarations(now_ns, gate_now, RefreshSet{});  // ends with the admission
    if (declarations_.declared(*idx) == was_declared) {
        notify_talker_stream_info(*idx);  // else the (un)declaration notified
    }
}

auto AvbEntityKit::is_talker_index(uint16_t const idx) const noexcept -> bool
{
    return std::ranges::any_of(talker_specs_, [idx](StreamSpec const& spec) { return spec.index == idx; });
}

auto AvbEntityKit::talker_index_of(nanoavb::StreamId const& stream_id) const noexcept -> std::optional<uint16_t>
{
    uint64_t const sid = stream_id.to_uint64();
    for (auto const& spec : talker_specs_) {
        if (auto const* s = host_.components().acmp_talker.get_stream(spec.index);
            s != nullptr && s->stream_id.to_uint64() == sid) {
            return spec.index;
        }
    }
    return std::nullopt;
}

auto AvbEntityKit::refresh_talker_listener(uint16_t const idx) -> bool
{
    auto const listener = registered_listener_of(idx);
    if (!listener) {
        return false;
    }
    bool const changed = *listener != talker_listener_[idx];
    talker_listener_[idx] = *listener;
    return changed;
}

auto AvbEntityKit::registered_listener_of(uint16_t const idx) const -> std::optional<srp::msrp::ListenerDeclaration>
{
    auto const* s = idx < talker_listener_.size() ? host_.components().acmp_talker.get_stream(idx) : nullptr;
    if (s == nullptr) {
        return std::nullopt;
    }
    statusbar::tsn::StreamId sid{};
    (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &sid);
    return host_.components().msrp_handler.registered_listener(sid);
}

void AvbEntityKit::notify_talker_stream_info(uint16_t const idx)
{
    (void)host_.components().aem_handler.notify_stream_info_changed(DESCRIPTOR_STREAM_OUTPUT, idx);
}

void AvbEntityKit::notify_talker_probe(uint16_t const idx, int64_t const now_ns)
{
    if (!is_talker_index(idx)) {
        return;
    }
    if (!declarations_.probe_window_open(idx, now_ns)) {
        host_.ctl_log().status(
            "acmp: talker stream {} ({}) probed (PROBE_TX): Talker Advertise wanted for 15 s",
            idx,
            logging::static_str(stream_kind_name(talker_kind_of(idx))));
    }
    declarations_.notify_probe(idx, now_ns);
    update_talker_declarations(now_ns);
}

void AvbEntityKit::notify_talker_addresses(bool const valid, int64_t const now_ns)
{
    std::array<bool, MAX_ENTITY_STREAMS> was_declared{};
    for (auto const& spec : talker_specs_) {
        declarations_.notify_address_valid(spec.index, valid);
        was_declared[spec.index] = declarations_.declared(spec.index);
    }
    update_talker_declarations(now_ns);
    // The destination address changed: notify the streams whose declaration
    // did not (that notified already).
    for (auto const& spec : talker_specs_) {
        if (declarations_.declared(spec.index) == was_declared[spec.index]) {
            notify_talker_stream_info(spec.index);
        }
    }
}

void AvbEntityKit::update_talker_declarations(int64_t const now_ns, GateTimePoint const gate_now, RefreshSet const refresh)
{
    // MSRP calls back on the reactor; a nested evaluation would act on the
    // table the outer one is walking: it is noted, and the outer one runs
    // again once its pass is done, at the outer one's times.
    if (updating_declarations_) {
        declarations_pending_ = true;
        declarations_pending_refresh_ |= refresh;
        return;
    }
    declarations_gate_now_ = gate_now;  // stashed for the gate stamps the passes make
    run_declaration_passes(now_ns, refresh);
    declaration_deadline_ns_ = declarations_.next_deadline(now_ns);
    notify_admission(gate_now);
}

void AvbEntityKit::run_declaration_passes(int64_t const now_ns, RefreshSet const refresh)
{
    updating_declarations_ = true;
    run_talker_declarations(now_ns, refresh);
    for (int pass = 0; declarations_pending_ && pass < 4; ++pass) {
        RefreshSet const again_refresh = declarations_pending_refresh_;
        declarations_pending_ = false;
        declarations_pending_refresh_.reset();
        run_talker_declarations(now_ns, again_refresh);
    }
    declarations_pending_ = false;
    declarations_pending_refresh_.reset();
    updating_declarations_ = false;
}

void AvbEntityKit::run_talker_declarations(int64_t const now_ns, RefreshSet const refresh)
{
    for (auto const& spec : talker_specs_) {
        // Milan v1.3 Section 4.3.3.1 condition 2 reads the registration as it is now: a pass can run
        // on an MSRP event before the kit's Listener hook has been told of it.
        auto const listener = registered_listener_of(spec.index).value_or(srp::msrp::ListenerDeclaration::Ignore);
        declarations_.notify_listener_registered(spec.index, listener != srp::msrp::ListenerDeclaration::Ignore);
        run_talker_declaration(spec, now_ns, refresh.test(spec.index));
    }
}

void AvbEntityKit::run_talker_declaration(StreamSpec const& spec, int64_t const now_ns, bool const refresh)
{
    using Action = TalkerDeclarations::Action;
    auto const info = make_talker_srp_info(spec);
    TalkerDeclarations::Params params{.dest_mac = info.dest_address, .vlan_id = info.vlan_id};
    if (auto const* s = host_.components().acmp_talker.get_stream(spec.index); s != nullptr) {
        params.stream_id = s->stream_id;
    }
    bool const was_declared = declarations_.declared(spec.index);
    switch (declarations_.evaluate(spec.index, now_ns, params, refresh)) {
        case Action::declare:
            declare_talker(spec, info, params, was_declared, now_ns);
            break;
        case Action::withdraw:
            withdraw_talker(spec, info, now_ns);
            break;
        case Action::none:
        default:
            break;
    }
}

void AvbEntityKit::declare_talker(
    StreamSpec const& spec,
    nanoavb::TalkerStreamSrpInfo const& info,
    TalkerDeclarations::Params const& params,
    bool const was_declared,
    int64_t const now_ns)
{
    auto& msrp = host_.components().msrp_handler;
    if (auto result = msrp.talker_advertise(info, TimePoint{std::chrono::nanoseconds{now_ns}}); !result) {
        host_.ctl_log().warning("msrp: talker_advertise (stream {}) failed: errno {}", spec.index, result.error().value());
        return;
    }
    declarations_.notify_declared(spec.index, params);
    // Re-learn the gate from the live registration: a withdraw forgot it,
    // and a registration that stayed raises no fresh MSRP event.
    gate_.note_listener_ready(info.stream_id, msrp.listener_permit_debug(info.stream_id).permits, declarations_gate_now_);
    // A Listener registered or gone changes REGISTERING (Milan v1.2 Table 5.12).
    announce_talker_declared(spec, was_declared, refresh_talker_listener(spec.index));
}

void AvbEntityKit::announce_talker_declared(StreamSpec const& spec, bool const was_declared, bool const listener_changed)
{
    if (!was_declared) {
        host_.ctl_log().status(
            "msrp: talker stream {} ({}) Talker Advertise declared",
            spec.index,
            logging::static_str(stream_kind_name(spec.format.kind)));
    }
    if (!was_declared || listener_changed) {
        notify_talker_stream_info(spec.index);
    }
}

void AvbEntityKit::withdraw_talker(StreamSpec const& spec, nanoavb::TalkerStreamSrpInfo const& info, int64_t const now_ns)
{
    (void)host_.components().msrp_handler.talker_withdraw(info.stream_id, TimePoint{std::chrono::nanoseconds{now_ns}});
    declarations_.notify_withdrawn(spec.index, now_ns);
    // No Talker Advertise, no streaming (Milan v1.2 5.3.7.3).
    gate_.notify_talker_withdrawn(spec.index);
    host_.ctl_log().status(
        "msrp: talker stream {} ({}) Talker Advertise withdrawn",
        spec.index,
        logging::static_str(stream_kind_name(spec.format.kind)));
    notify_talker_stream_info(spec.index);
}

void AvbEntityKit::withdraw_talker_streams(TimePoint const time)
{
    for (auto const& spec : talker_specs_) {
        (void)host_.components().msrp_handler.talker_withdraw(make_talker_srp_info(spec).stream_id, time);
    }
    int64_t const now_ns = steady_ns_of(time);
    bool const any =
        std::ranges::any_of(talker_specs_, [this](StreamSpec const& spec) { return declarations_.declared(spec.index); });
    notify_all_talkers_withdrawn(now_ns);
    if (any) {
        for (auto const& spec : talker_specs_) {
            notify_talker_stream_info(spec.index);
        }
    }
}

void AvbEntityKit::notify_all_talkers_withdrawn(int64_t const now_ns)
{
    declarations_.set_active(false);
    declarations_.notify_all_withdrawn(now_ns);
    declaration_deadline_ns_ = declarations_.next_deadline(now_ns);
    // No Talker Advertise, no streaming (Milan 1.2 5.3.7.3): close every
    // stream now, without the grace window.
    gate_.notify_talker_withdrawn();
    notify_admission(net::rx_clock_now());
}

void AvbEntityKit::wire_stream_callbacks()
{  // NOLINT(statusbar-block-statements) the stream callback wiring, one install per hook; pre-dates the gate
    // MSRP reservations for the talker streams. The host declares the SR class
    // domain before calling these, then advertises/withdraws on the MSRP cycle.
    host_.set_advertise_streams([this](TimePoint time) { advertise_talker_streams(time); });
    host_.set_withdraw_streams([this](TimePoint time) { withdraw_talker_streams(time); });
    // The transmit gate tracks the MSRP listener declarations against our talker
    // streams; that alone admits a stream (docs/AVB_ENTITY_KIT.md).
    host_.set_on_listener_ready([this](nanoavb::StreamId const& stream_id, bool const ready, TimePoint const time) {
        notify_listener_ready(stream_id, ready, steady_ns_of(time), net::rx_clock_now());
    });

    // ACMP: log talker connections (classic only: they no longer gate
    // anything); drive the listener (MSRP attach + mcast join) on listener
    // connect/disconnect.
    host_.components().acmp_talker.set_connection_callbacks(
        [this](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            host_.ctl_log().status(
                "acmp: talker stream {} ({}) CONNECTED by listener {:012x} unique_id {}",
                stream_index,
                logging::static_str(stream_kind_name(talker_kind_of(stream_index))),
                listener_entity_id.to_uint64(),
                listener_unique_id);
        },
        [this](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            host_.ctl_log().status(
                "acmp: talker stream {} ({}) DISCONNECTED by listener {:012x} unique_id {}",
                stream_index,
                logging::static_str(stream_kind_name(talker_kind_of(stream_index))),
                listener_entity_id.to_uint64(),
                listener_unique_id);
        });
    if (listener_ != nullptr) {
        // A connected (classic) or settled (Milan) input: the MSRP Listener
        // declaration, the multicast join and, for Milan, the stream's VID with
        // MVRP (Milan v1.2 Section 4.4.1).
        host_.components().acmp_listener.set_connection_callbacks(
            [this](uint16_t stream_index, ieee::Eui64 const& stream_id, ieee::Eui48 dest_mac, TimePoint now) {
                listener_->on_listener_connected(stream_index, stream_id, dest_mac, now);
                if (host_.components().profile.milan) {
                    refresh_sink_registration(stream_index, now);  // the input's VID joined there
                }
            },
            [this](uint16_t stream_index, TimePoint now) {
                if (stream_index < input_vid_.size()) {
                    leave_sink_vlan(std::exchange(input_vid_[stream_index], uint16_t{0}), now);
                }
                // A classic disconnect is the controller's: no interruption.
                // (A Milan input unsettles for other reasons too; its unbind is
                // the sink's binding change.)
                listener_->on_listener_disconnected(stream_index, now);
                if (!host_.components().profile.milan) {
                    listener_->notify_input_unbound(stream_index);
                }
            });
    }
    wire_aecp_queries();
}

void AvbEntityKit::wire_aecp_queries()
{
    // GET_COUNTERS: the streams' (ENTITY, AVB_INTERFACE, CLOCK_DOMAIN from the host); GET_STREAM_INFO:
    // a talker stream's identity (a Milan listener checks it), a listener sink's binding.
    host_.components().aem_handler.set_get_counters(
        [this](uint16_t descriptor_type, uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& counters) -> bool {
            bool const milan = host_.components().profile.milan;
            if (descriptor_type == DESCRIPTOR_STREAM_OUTPUT) {
                return is_talker_index(descriptor_index) &&
                    fill_talker_stream_counters(
                           *talker_,
                           descriptor_index,
                           output_counters_[descriptor_index],
                           host_.components().profile,
                           valid,
                           counters);
            }
            if (descriptor_type == DESCRIPTOR_STREAM_INPUT && listener_ != nullptr) {
                // Classic keeps IEEE 1722.1's running totals.
                return milan ? listener_->fill_interval_stream_input_counters(descriptor_index, valid, counters)
                             : listener_->fill_stream_input_counters(descriptor_index, valid, counters);
            }
            return host_.fill_counters(descriptor_type, descriptor_index, valid, counters);
        });
    host_.components().aem_handler.set_get_stream_info(
        [this](
            uint16_t descriptor_type,
            uint16_t descriptor_index,
            atdecc::aem::AemStreamInfoPayload& out,
            atdecc::aem::AemStreamInfoMilanExtension& ext) -> bool {
            bool const milan = host_.components().profile.milan;
            bool filled = false;
            if (descriptor_type == DESCRIPTOR_STREAM_OUTPUT) {
                if (!is_talker_index(descriptor_index)) {
                    return false;
                }
                TalkerStreamStatus const status{
                    .milan = milan,
                    .layout = host_.components().aem_handler.stream_info_layout(),
                    .declaring = declarations_.declared(descriptor_index),
                    .listener = talker_listener_[descriptor_index],
                    .presentation_offset_ns = presentation_offset_ns(descriptor_index),
                    .accumulated_latency_ns = talker_accumulated_latency_ns(descriptor_index),
                    .stopped = talker_->stream_stopped(descriptor_index)};
                filled = fill_talker_stream_info(host_, descriptor_index, status, out, ext);
            } else if (descriptor_type == DESCRIPTOR_STREAM_INPUT && listener_ != nullptr) {
                auto const layout = host_.components().aem_handler.stream_info_layout();
                filled = milan ? fill_milan_listener_stream_info(host_, descriptor_index, layout, out, ext)
                               : fill_listener_stream_info(host_, descriptor_index, out);
            }
            // The fills read the blob's authored format: the stream's current
            // one is the runtime format (a SET_STREAM_FORMAT, a rate change).
            if (filled) {
                if (auto const format = storage_handler_.current_stream_format(
                        {.configuration_index = 0, .descriptor_type = descriptor_type, .descriptor_index = descriptor_index})) {
                    span_copy(make_span(out.stream_format), format->span());
                }
            }
            return filled;
        });
}

void AvbEntityKit::wire_milan_talker()
{
    auto& components = host_.components();
    // Milan v1.2 Section 5.5.4: the ACMP talker answers from what only the kit
    // knows (MAAP's address, the registered Listener, the interface), and a
    // probe opens the stream's declaration window.
    components.acmp_talker.set_milan_callbacks(
        [this](uint16_t const idx) -> nanoavb::MilanTalkerStreamState {
            auto const* s = host_.components().acmp_talker.get_stream(idx);
            return nanoavb::MilanTalkerStreamState{
                .dest_mac_valid = declarations_.address_valid(idx) && s != nullptr && s->stream_dest_mac != ieee::Eui48{},
                .registering_failed = idx < talker_listener_.size() && declarations_.declared(idx) &&
                    talker_listener_[idx] == srp::msrp::ListenerDeclaration::AskingFailed,
                .avb_interface_index = idx < talker_interface_.size() ? talker_interface_[idx] : uint16_t{0}};
        },
        [this](uint16_t const idx, TimePoint const time) { notify_talker_probe(idx, steady_ns_of(time)); });

    // The presentation time offset (Milan v1.2 Sections 5.4.2.9-10): refused
    // while the output is streaming and when the entity cannot apply it.
    components.aem_handler.set_presentation_offset_hooks(
        [this](uint16_t const idx) -> std::optional<uint32_t> {
            return is_talker_index(idx) ? std::optional<uint32_t>{presentation_offset_ns(idx)} : std::nullopt;
        },
        [this](uint16_t const idx, uint32_t const offset_ns) -> uint8_t { return change_presentation_offset(idx, offset_ns); });
}

auto AvbEntityKit::change_presentation_offset(uint16_t const idx, uint32_t const offset_ns) -> uint8_t
{
    if (!on_presentation_offset_changed_) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;
    }
    // Streaming as Milan v1.2 Section 5.3.7.3 has it: declaring and a Listener Ready / Ready
    // Failed registered (the grace window included), even with the transmit gate disabled.
    if (declarations_.declared(idx) && gate_.listener_admitted(idx, net::rx_clock_now())) {
        return atdecc::AEM_STATUS_STREAM_IS_RUNNING;
    }
    uint32_t const before = presentation_offset_ns(idx);
    return offset_ns == before ? atdecc::AEM_STATUS_SUCCESS : commit_presentation_offset(idx, before, offset_ns);
}

auto AvbEntityKit::commit_presentation_offset(uint16_t const idx, uint32_t const before, uint32_t const offset_ns) -> uint8_t
{
    if (auto const status = on_presentation_offset_changed_(idx, offset_ns); status != atdecc::AEM_STATUS_SUCCESS) {
        return status;
    }
    presentation_offset_ns_[idx].store(offset_ns, std::memory_order_relaxed);
    host_.ctl_log().status("aem: talker stream {} presentation offset {} -> {} ns", idx, before, offset_ns);
    return atdecc::AEM_STATUS_SUCCESS;
}

void AvbEntityKit::wire_milan_listener()
{
    auto& components = host_.components();
    auto& sinks = components.acmp_listener.milan_sinks();
    // A binding change is saved after the reactor pass that made it (after the
    // ACMP response); a stopped input discards its stream (Milan v1.2 5.3.8.7).
    sinks.set_on_binding_changed([this](uint16_t const index) {
        persist_pending_ = true;
        if (listener_ != nullptr) {
            auto const* sink = host_.components().acmp_listener.milan_sinks().sink(index);
            // An unbound sink's started is the default, true.
            listener_->set_stream_stopped(index, sink != nullptr && !sink->binding.started);
            // Milan v1.2 Section 5.3.8.10: bound from unbound zeroes the
            // input's counters; an unbind is no interruption.
            bool const bound = sink != nullptr && sink->bound();
            if (index < input_bound_.size() && bound != input_bound_[index]) {
                input_bound_[index] = bound;
                if (bound) {
                    listener_->reset_input_counters(index);
                } else {
                    listener_->notify_input_unbound(index);
                }
                (void)host_.components().aem_handler.notify_counters_changed(DESCRIPTOR_STREAM_INPUT, index);
            }
        }
    });
    sinks.set_on_status_changed([this](uint16_t const index) {
        auto const* sink = host_.components().acmp_listener.milan_sinks().sink(index);
        if (sink == nullptr) {
            return;
        }
        if (!sink->bound()) {
            host_.ctl_log().status("acmp: listener stream {} unbound", index);
        } else {
            host_.ctl_log().status(
                "acmp: listener stream {} bound to {:016x}:{} {}{} (acmp status {}){}",
                index,
                sink->binding.talker_entity_id.to_uint64(),
                sink->binding.talker_unique_id,
                logging::static_str(atdecc::milan_sink_state_name(sink->state)),
                sink->registration == atdecc::MilanTalkerRegistration::failed ? logging::lit(" talker failed") : logging::lit(""),
                sink->acmp_status,
                sink->binding.started ? logging::lit("") : logging::lit(", stopped"));
        }
        (void)host_.components().aem_handler.notify_stream_info_changed(DESCRIPTOR_STREAM_INPUT, index);
    });
    // EVT_TK_REGISTERED / EVT_TK_UNREGISTERED: every change of a talker's
    // registration is matched against the settled sinks.
    components.msrp_handler.set_on_remote_talker([this](nanoavb::StreamId const& sid, TimePoint const now) {
        auto const& sinks = host_.components().acmp_listener.milan_sinks();
        for (size_t i = 0; i < sinks.sink_count(); ++i) {
            auto const index = static_cast<uint16_t>(i);
            auto const* sink = sinks.sink(index);
            if (sink->settled() && sink->stream.stream_id.to_uint64() == sid.to_uint64()) {
                refresh_sink_registration(index, now);
            }
        }
    });
}

void AvbEntityKit::refresh_sink_registration(uint16_t const index, TimePoint const now)
{
    auto& sinks = host_.components().acmp_listener.milan_sinks();
    auto const* sink = sinks.sink(index);
    if (sink == nullptr || !sink->settled()) {
        return;
    }
    join_input_vlan(index, *sink, now);
    sinks.set_registration(index, talker_registration(*sink), now);
}

void AvbEntityKit::join_input_vlan(uint16_t const index, atdecc::MilanSink const& sink, TimePoint const now)
{
    // Once per settle; a probe answer naming no VID (0) uses the matched Talker attribute's, when registered.
    uint16_t const vid = sink.stream.vlan_id != 0 ? sink.stream.vlan_id : matched_talker_vid(sink);
    if (index < input_vid_.size() && input_vid_[index] == 0 && vid != 0) {
        input_vid_[index] = vid;
        join_sink_vlan(vid, now);
    }
}

auto AvbEntityKit::matched_talker_vid(atdecc::MilanSink const& sink) const -> uint16_t
{
    statusbar::tsn::StreamId sid{};
    (void)statusbar::tsn::load_unchecked(sink.stream.stream_id.span(), &sid);
    auto const& msrp = host_.components().msrp_handler;
    auto const* const failed = msrp.peer_talker_failed(sid);
    auto const* const fv = failed != nullptr ? &failed->advertise : msrp.peer_talker_advertise(sid);
    return fv != nullptr && fv->destination_address == sink.stream.dest_mac ? fv->vlan_identifier.get() : uint16_t{0};
}

auto AvbEntityKit::talker_registration(atdecc::MilanSink const& sink) const -> atdecc::MilanTalkerRegistration
{
    // Milan v1.2 Section 5.5.3.3: a Talker attribute matching the stream ID,
    // destination address and VLAN ID the probe returned, as the Listener declaration follows.
    statusbar::tsn::StreamId sid{};
    (void)statusbar::tsn::load_unchecked(sink.stream.stream_id.span(), &sid);
    auto const& msrp = host_.components().msrp_handler;
    nanoavb::TalkerMatch const match{.destination = sink.stream.dest_mac, .vid = sink.stream.vlan_id};
    if (msrp.talker_failed_registered(sid, match)) {
        return atdecc::MilanTalkerRegistration::failed;
    }
    return msrp.talker_advertise_registered(sid, match) ? atdecc::MilanTalkerRegistration::advertise
                                                        : atdecc::MilanTalkerRegistration::none;
}

void AvbEntityKit::join_sink_vlan(uint16_t const vid, TimePoint const now)
{
    if (vid == 0 || vid > 4094) {
        return;
    }
#if defined(__linux__)
    vlan_filters_.hold(vid);  // one hold per input, beside the Class A VID's own
#endif
    for (auto& [joined, count] : sink_vlans_) {
        if (joined == vid) {
            ++count;
            return;
        }
    }
    (void)sink_vlans_.try_push_back({vid, uint16_t{1}});
    // The Class A VID in use is the host's (always joined).
    if (vid != host_.components().msrp_handler.domain().sr_class_vid) {
        (void)host_.components().mvrp_handler.register_vlan(vid, now);
    }
}

void AvbEntityKit::leave_sink_vlan(uint16_t const vid, TimePoint const now)
{
    for (auto* it = sink_vlans_.begin(); it != sink_vlans_.end(); ++it) {
        if (it->first != vid) {
            continue;
        }
#if defined(__linux__)
        vlan_filters_.drop(vid);
#endif
        if (--it->second == 0) {
            sink_vlans_.erase(it);
            if (vid != host_.components().msrp_handler.domain().sr_class_vid) {
                (void)host_.components().mvrp_handler.withdraw_vlan(vid, now);
            }
        }
        return;
    }
}

auto AvbEntityKit::acquire_maap_addresses(net::MessageReactor& reactor, TimePoint const now) -> Status
{
    sg14::inplace_vector<uint16_t, MAX_ENTITY_STREAMS> indices{};
    for (auto const& spec : talker_specs_) {
        (void)indices.try_push_back(spec.index);
    }
    // On (re)defend the streams declare against the addresses; a conflict
    // withdraws them and a new block waits out 2 x LeaveAll (Milan v1.2 Table 5.3).
    return maap_.acquire(
        reactor,
        config_.interface_name,
        stream_base_mac_for(config_.interface_name, config_.entity_id),
        host_.components(),
        *talker_,
        indices,
        host_.ctl_log(),
        [this](TimePoint const time) { notify_talker_addresses(true, steady_ns_of(time)); },
        [this](TimePoint const time) { notify_talker_addresses(false, steady_ns_of(time)); },
        now);
}

auto AvbEntityKit::start(net::MessageReactor& reactor, TimePoint const now) -> Status
{
    (void)read_port();
    if (auto status = start_control(reactor, now); !status) {
        return status;
    }
    if (auto status = start_data_plane(reactor); !status) {
        return status;
    }
    // Write what start() ended up with: an imported, fresh or pruned state
    // (nothing when the file already says it).
    persist();
    // Menu/selection diagnostics: registrations the model left inert. Status
    // level -- typo-finding, never an error.
    for (auto const& binding : control_bindings_) {
        if (!binding.resolved) {
            host_.ctl_log().status("control: registered symbol 0x{:08x} not a CONTROL in this model (inert)", binding.symbol);
        }
    }
    return success();
}

auto AvbEntityKit::read_port() -> bool
{
#if defined(__linux__)
    auto const before = std::pair{link_mbps_, mtu_};
    link_mbps_ = net::link_speed_mbps(config_.interface_name).value_or(link_mbps_);
    mtu_ = net::interface_mtu(config_.interface_name).value_or(mtu_);
    return std::pair{link_mbps_, mtu_} != before;
#else
    return false;
#endif
}

void AvbEntityKit::on_link_up(TimePoint const time)
{
    bool const changed = !class_a_cbs_ && read_port();  // a shaper keeps its own
    host_.on_link_up(time);
    if (!changed) {
        return;
    }
    update_talker_declarations(steady_ns_of(time), true);
    for (auto const& spec : talker_specs_) {
        notify_talker_stream_info(spec.index);  // GET_STREAM_INFO's msrp_accumulated_latency moved
    }
}

auto AvbEntityKit::stream_port_config(std::string_view const backend) const -> StreamPortConfig
{
    return StreamPortConfig{
        .backend = backend,
        .interface_name = config_.interface_name,
        .stream_pcp = config_.stream_pcp,
        .xdp_queue = config_.net_xdp_queue,
        .xdp_bind_mode = config_.net_xdp_bind_mode,
        .xdp_attach_mode = config_.net_xdp_attach_mode,
        .xdp_pin_path = config_.net_xdp_pin_path,
        .tx_launch_time = config_.stream_launch_time,
    };
}

auto AvbEntityKit::start_control(net::MessageReactor& reactor, TimePoint const now) -> Status
{
    prepare_control_plane();
    if (auto status = host_.start_control_plane(reactor, config_.interface_name); !status) {
        return status;
    }
    wire_stream_callbacks();
    declare_vid_and_domain(now);
    // A restarted kit whose control plane stayed Ready gets no advertise hook
    // again (stop() withdrew everything): advertise now.
    if (host_.is_ready()) {
        advertise_talker_streams(now);
    }
    return start_declarations(reactor, now);
}

void AvbEntityKit::declare_vid_and_domain(TimePoint const now)
{
    // The VID, registered on every start: stop() withdraws it with the rest.
    (void)host_.components().mvrp_handler.register_vlan(config_.vlan_id, now);
    // After a stop, the run resumes on the configured Domain, which a link already
    // up reports no link up to restore (Milan v1.2 Section 4.2.7.2.1).
    if (host_.components().msrp_handler.withdrawn()) {
        (void)host_.components().msrp_handler.reset_domain(now);
    }
}

void AvbEntityKit::prepare_control_plane()
{
    // Bring up the shared control plane (net handlers + generic SM wiring),
    // then attach the stream-specific callbacks (hooks + ACMP + AEM handlers).
    gate_.set_logger(host_.ctl_log());
    // A restarted kit reports its first admitted set afresh.
    admitted_.clear();
    if (listener_ != nullptr) {
        listener_->set_logger(host_.ctl_log());
    }
    host_.set_local_atdecc_socket(config_.atdecc_socket_path, config_.atdecc_socket_mode);
    // The vendor's defaults, then the saved settings, before the entity shows itself to the network.
    apply_default_mcr_priorities();
    load_persistent_state();
}

void AvbEntityKit::apply_default_mcr_priorities()
{
    auto const& priorities = config_.default_mcr_priorities;
    auto& mvu = host_.components().mvu_handler;
    if (mvu.clock_domain_count() != 0 && priorities.size() > mvu.clock_domain_count()) {
        host_.ctl_log().warning(
            "kit: {} default media clock reference priorities for {} clock domain(s): the rest ignored",
            priorities.size(),
            mvu.clock_domain_count());
    }
    for (size_t index = 0; !priorities.empty() && index < mvu.clock_domain_count(); ++index) {
        (void)mvu.set_default_mcr_priority(index, priorities[std::min(index, priorities.size() - 1)]);
    }
}

auto AvbEntityKit::start_declarations(net::MessageReactor& reactor, TimePoint const now) -> Status
{
    // In "maap" mode, claim the talker stream destination addresses via MAAP and
    // overwrite the static defaults BEFORE the data plane reads them below.
    // Non-fatal inside: on socket failure the entity keeps the static MACs.
    if (config_.stream_address_mode == "maap") {
        if (auto status = acquire_maap_addresses(reactor, now); !status) {
            return status;
        }
    }
    // Static addresses are valid at once, MAAP's when the block is defended. Until
    // then the configured default must not reach a listener (TALKER_DEST_MAC_FAIL).
    for (auto const& spec : talker_specs_) {
        declarations_.notify_address_valid(spec.index, maap_.ready());
        if (!maap_.ready()) {
            if (auto const* s = host_.components().acmp_talker.get_stream(spec.index); s != nullptr) {
                (void)host_.components().acmp_talker.configure_stream(spec.index, s->stream_id, ieee::Eui48{}, s->stream_vlan_id);
            }
        }
    }
    // The declarations' timer: a probe window closing, a hold ending. It
    // reaches the kit through a token stop() (or the kit's end) drops, and
    // then finishes: the reactor discards it and a restart adds a new one.
    class DeclarationTimer final : public net::Pollable
    {
      public:
        explicit DeclarationTimer(std::weak_ptr<AvbEntityKit* const> kit) noexcept
            : kit_{std::move(kit)}
        {}
        [[nodiscard]] auto fd() const noexcept -> int override { return -1; }
        void on_ready(int64_t /*now_ns*/) override {}
        void tick(int64_t const now_ns) override
        {
            if (auto const kit = kit_.lock(); kit != nullptr) {
                (*kit)->on_declaration_timer(now_ns);
            }
        }
        [[nodiscard]] auto finished() const noexcept -> bool override { return kit_.expired(); }

      private:
        std::weak_ptr<AvbEntityKit* const> kit_;
    };
    declaration_timer_token_ = std::make_shared<AvbEntityKit* const>(this);
    reactor.add(std::make_unique<DeclarationTimer>(declaration_timer_token_));
    return success();
}

auto AvbEntityKit::start_data_plane(net::MessageReactor& reactor) -> Status
{
    if (auto status = open_talker_streams(); !status) {
        return status;
    }
    open_stream_port();
    if (listener_ != nullptr) {
        if (auto status = start_listener_streams(reactor); !status) {
            return status;
        }
    }
    open_stream_tx();
    return success();
}

void AvbEntityKit::open_stream_tx()
{
    // TX on the port + optional pcap capture. Tolerated here (the entity stays
    // a control-plane-only node); recorded for a host that needs the data plane.
    if (!stream_tx_error_) {
        auto const pcap = TalkerStreams::TxPcapConfig{
            .path = config_.tx_pcap_path, .max_bytes = config_.tx_pcap_max_bytes, .seconds = config_.tx_pcap_seconds};
        if (auto tx = talker_->open_tx(stream_port_, pcap); !tx) {
            stream_tx_error_ = tx.error();
        }
    }
    // The TX kick off the media thread (the tool's kicker timer, or its RX
    // timer's tick). The tool must then run that timer, or call
    // kick_from_media_tick() / rx_to_reactor() when it cannot.
    bool const kick_elsewhere = config_.stream_tx_kicker || (config_.stream_tx_kick_in_rx_timer && config_.stream_rx_rt_timer);
    if (!talker_->set_external_kick(kick_elsewhere)) {
        host_.ctl_log().warning("stream_tx.kicker: no stream port to kick; kicking in the media tick");
    }
}

auto AvbEntityKit::open_talker_streams() -> Status
{
    // One TX slot per blob-declared stream, shaped by its format word, with
    // the stream identity (id + dest MAC) the ACMP model carries.
    for (auto const& spec : talker_specs_) {
        statusbar::tsn::StreamId sid{};
        ieee::Eui48 dest{};
        if (auto const* s = host_.components().acmp_talker.get_stream(spec.index); s != nullptr) {
            (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &sid);
            dest = s->stream_dest_mac;
        }
        if (auto status = talker_->open_stream(spec, sid, dest); !status) {
            return status;
        }
    }
    return success();
}

void AvbEntityKit::open_stream_port()
{
    stream_tx_error_ = stream_rx_error_ = stream_xdp_error_ = stream_launch_time_error_ = std::error_code{};
    if (stream_port_.valid()) {
        log_stream_port();  // the host opened it before start() (a test port): kept
        return;
    }
    auto const opened = open_configured_stream_port();
    if (!opened) {
        stream_tx_error_ = opened.error();
        stream_rx_error_ = opened.error();
    }
    log_stream_port();
}

auto AvbEntityKit::open_configured_stream_port() -> Status
{
    auto opened = stream_port_.open_resolved(stream_port_config(config_.net_backend));
    stream_xdp_error_ = stream_port_.xdp_error();
    stream_launch_time_error_ = stream_port_.launch_time_error();
    return opened;
}

void AvbEntityKit::log_stream_port()
{
    if (stream_xdp_error_) {
        host_.ctl_log().warning(
            "stream port: xdp did not open ({}); rawnet instead", logging::embed<40>(stream_xdp_error_.message()));
    }
    if (stream_launch_time_error_) {
        host_.ctl_log().warning(
            "stream port: no launch time ({}); frames untimed", logging::embed<40>(stream_launch_time_error_.message()));
    }
    if (stream_tx_error_) {
        host_.ctl_log().error(
            "stream port: {} did not open: {}",
            logging::embed<8>(config_.net_backend),
            logging::embed<40>(stream_tx_error_.message()));
        return;
    }
    host_.ctl_log().status(
        "stream port: {}{}{}",
        logging::embed<48>(stream_port_.describe()),
        stream_port_.serialized() ? logging::lit(", TX and RX serialised") : logging::lit(""),
        stream_port_.tx_launch_time() ? logging::lit(", TX metadata for launch times") : logging::lit(""));
}

auto AvbEntityKit::start_listener_streams(net::MessageReactor& reactor) -> Status
{
    if (auto status = open_listener_streams(); !status) {
        return status;
    }
    // In RT-timer mode the handler is kept here for the tool's SCHED_FIFO RX timer (drain_stream_rx(),
    // its wake's gPTP time), so the control plane cannot starve RX; rx_to_reactor() is the way back
    // should that timer not start.
    if (!stream_rx_error_) {
        if (auto rx = listener_->attach_rx(stream_port_, rx_static_groups_, reactor, config_.stream_rx_rt_timer)) {
            rt_rx_handler_ = std::move(*rx);
        } else {
            stream_rx_error_ = rx.error();  // tolerated here; a host may refuse (stream_rx_error())
        }
    }
#if defined(__linux__)
    // The VID's tagged stream frames must get past the NIC's VLAN filter.
    // Never fatal: without CAP_NET_ADMIN this only warns.
    // (A control bound before start() -- a test's -- is used as it is.)
    if (!vlan_filters_.bound()) {
        open_listener_vlan_filter();
    }
#endif
    // Persisted fast-connect bindings (kit phase 5b): a restarted entity
    // re-connects its inputs without a controller (see AvbEntityHost),
    // kept in the persistent state when there is one.
    if (persistence_active_) {
        apply_saved_listener_bindings();
    } else if (!host_.components().profile.milan) {
        host_.enable_listener_binding_persistence(config_.listener_bindings_path);
    } else if (!config_.listener_bindings_path.empty()) {
        host_.ctl_log().warning("kit: the Milan listener keeps its bindings in the persistent state file only: none is configured, "
                                "the listener bindings file is not used");
    }
    return success();
}

auto AvbEntityKit::open_listener_streams() -> Status
{
    // RX slots per blob-declared stream input. One CRF input feeds the media-clock
    // recovery unless the host bound it (set_crf before start); the ACMP listener
    // callbacks (wired above) drive the MSRP attach + the multicast join per connection.
    auto const recovery_input = crf_recovery_input();
    // Static RX groups: an audio input pre-joins the same-kind talker output's group (the loopback
    // topology listens on its own); everything else joins on ACMP connect (a zero MAC opens the socket).
    rx_static_groups_.clear();
    for (auto const& spec : listener_specs_) {
        if (auto status = listener_->open_stream(spec); !status) {
            return status;
        }
        if (auto const* slot = listener_->slot_for(spec.index); spec.index == recovery_input && slot != nullptr && !slot->on_crf) {
            listener_->set_crf(spec.index, crf_recovery_.make_consumer());
        }
        (void)rx_static_groups_.try_push_back(static_rx_group(spec));
    }
    return success();
}

#if defined(__linux__)
void AvbEntityKit::open_listener_vlan_filter()
{
    auto const ifindex = net::interface_index(config_.interface_name);
    if (!ifindex) {
        host_.ctl_log().warning(
            "listener: no VLAN filter registration: no index for the interface: errno {}", ifindex.error().value());
        return;
    }
    if (vlan_client_ == nullptr || !vlan_client_->is_open()) {
        auto client = std::make_unique<net::vlan::VlanLinkClient>();
        if (auto const opened = client->open(); !opened) {
            host_.ctl_log().warning("listener: no VLAN filter registration: rtnetlink socket: errno {}", opened.error().value());
            return;
        }
        vlan_client_ = std::move(client);
    }
    bind_listener_vlan(*vlan_client_, *ifindex);
}
#endif

#if defined(__linux__)
void AvbEntityKit::bind_listener_vlan(net::vlan::VlanControl& control, int const ifindex)
{
    if (listener_ == nullptr) {
        return;
    }
    vlan_filters_.bind(control, config_.interface_name, ifindex);
    class_a_filtered_ = host_.components().msrp_handler.domain().sr_class_vid;
    vlan_filters_.hold(class_a_filtered_);
    for (auto const vid : input_vid_) {
        vlan_filters_.hold(vid);  // the inputs settled before (0: none)
    }
}
#endif

auto AvbEntityKit::stop(TimePoint const now) -> Status
{
    close_data_plane();
    if (!host_.is_running()) {
        return failure(std::make_error_code(std::errc::not_connected));
    }
    // Stopping the control plane runs no withdraw hook: withdraw here, so a
    // restart starts undeclared, inactive, unconnected and with every gate closed.
    withdraw_talker_streams(now);
    drop_listener_connections(now);
    return host_.stop_control_plane(now);
}

void AvbEntityKit::drop_listener_connections(TimePoint const now)
{
    for (auto const& spec : listener_specs_) {
        drop_listener_connection(spec.index, now);
    }
}

void AvbEntityKit::drop_listener_connection(uint16_t const index, TimePoint const now)
{
    auto& acmp = host_.components().acmp_listener;
    if (auto const* sink = acmp.milan_sinks().sink(index); sink != nullptr && sink->bound()) {
        // Locally: no controller's authority, no response, no notification; unbound until restored.
        (void)acmp.milan_sinks().release(index, now);
        if (index < input_bound_.size()) {
            input_bound_[index] = false;
        }
        return;
    }
    // Every input of the talker; stop() drops them all. Its fast-connect goal stays.
    if (auto const* stream = acmp.get_stream(index); stream != nullptr && stream->connected) {
        (void)acmp.on_talker_departed(stream->talker_entity_id, now);
    }
}

void AvbEntityKit::close_streams() noexcept
{
    rt_rx_handler_.reset();
    talker_->close_tx();
    if (listener_ != nullptr) {
        listener_->close_rx();
    }
    stream_port_.close();
}

void AvbEntityKit::rx_to_reactor(net::MessageReactor& reactor)
{
    if (config_.stream_tx_kick_in_rx_timer) {
        kick_from_media_tick();
    }
    if (stream_port_.single_threaded()) {
        // Its host drains RX on the transmitting thread (drain_stream_rx()), which runs whatever timer
        // failed: RX stays there. On the reactor it would race that thread's unlocked TX.
        host_.ctl_log().warning("stream RX: a single-threaded stream port is drained by its host, not the reactor");
        return;
    }
    if (rt_rx_handler_ != nullptr) {
        reactor.add(std::move(rt_rx_handler_));
    }
}

void AvbEntityKit::close_data_plane()
{
#if defined(__linux__)
    vlan_filters_.release();
#endif
    close_streams();                   // the port's memberships close with it
    rx_static_groups_.clear();         // held on that port only: the next start joins them afresh
    refresh_pending_.reset();          // a re-declaration asked for in this run: the next run declares afresh
    declaration_timer_token_.reset();  // the timer finishes
    if (persist_pending_) {
        persist();  // a listener goal change of this pass
    }
}

void AvbEntityKit::on_declaration_timer(int64_t const now_ns)
{
    if (!format_notifications_.empty()) {
        flush_format_notifications();  // a rate change's, after its response
    }
    // A listener goal change during this pass is saved now, after its ACMP
    // response.
    if (persist_pending_) {
        persist();
    }
    if (admission_deadline_) {
        on_admission_timer(net::rx_clock_now());  // the gate's clock, read while a grace runs
    }
    if (!host_.is_running()) {
        return;
    }
    // One pass, told its time, for a refresh an AEM hook asked for (told no time) or a deadline reached.
    bool const due = declaration_deadline_ns_ && now_ns >= *declaration_deadline_ns_;
    if (due || refresh_pending_.any()) {
        update_talker_declarations(now_ns, net::rx_clock_now(), std::exchange(refresh_pending_, RefreshSet{}));
    }
}

auto AvbEntityKit::default_presentation_offset_ns() const noexcept -> uint32_t
{
    return static_cast<uint32_t>(std::min<uint64_t>(config_.presentation_offset_ns, atdecc::aem::MILAN_PRESENTATION_OFFSET_MAX_NS));
}

auto AvbEntityKit::served_entity_model_id() const -> uint64_t
{
    atdecc::aem::DescriptorEntity entity{};
    nanoavb::DescriptorId const id{
        .ref = {.configuration_index = 0, .descriptor_type = atdecc::aem::DESCRIPTOR_ENTITY, .descriptor_index = 0}, .symbol = 0};
    if (!storage_handler_.on_get_entity(id, entity)) {
        return config_.entity_model_id.to_uint64();
    }
    return entity.entity_model_id.to_uint64();
}

void AvbEntityKit::load_persistent_state()
{
    persistence_active_ = false;
    persistence_blocked_ = false;
    persist_pending_ = false;
    saved_bindings_.clear();
    if (!config_.persistent_state_path.empty()) {
        restore_persistent_state();
    }
}

void AvbEntityKit::restore_persistent_state()
{
    auto loaded = avb_entity::load_persistent_state(
        config_.persistent_state_path,
        served_entity_model_id(),
        listener_ != nullptr ? config_.listener_bindings_path : std::string{});
    save_settings_changes();
    if (!notify_persistent_state_outcome(loaded)) {
        return;
    }
    apply_saved_settings(loaded.state);
    saved_bindings_ = std::move(loaded.state.listener_bindings);
}

void AvbEntityKit::save_settings_changes()
{
    persistence_active_ = true;
    // Every later change is saved: AEM settings, the MVU values (the
    // listener bindings once the listener is up).
    host_.components().aem_handler.set_on_settings_changed([this](uint16_t) { persist(); });
    host_.components().mvu_handler.set_on_settings_changed([this] { persist(); });
}

auto AvbEntityKit::notify_persistent_state_outcome(LoadedPersistentState const& loaded) -> bool
{
    auto log = host_.ctl_log();
    switch (loaded.outcome) {
        case PersistentStateOutcome::fresh:
            log.status("kit: no persistent state yet; starting from the model's defaults");
            if (loaded.error) {
                log.warning("kit: the listener bindings file was not imported: {}", logging::embed<40>(loaded.error.message()));
            }
            break;
        case PersistentStateOutcome::loaded:
            if (loaded.state.system_unique_id_ignored) {
                log.warning("kit: the persistent state's system_unique_id is not a hex string: left out, the rest loaded");
            }
            // Unchanged by what follows, it needs no rewrite.
            persisted_text_ = serialize_persistent_state(loaded.state);
            break;
        case PersistentStateOutcome::imported:
            log.status(
                "kit: no persistent state yet; {} listener binding(s) imported from the listener bindings file, which is no "
                "longer written",
                loaded.state.listener_bindings.size());
            break;
        case PersistentStateOutcome::corrupt:
            log.warning(
                "kit: the persistent state file is corrupt (error {}); starting fresh, the old file kept as {}",
                loaded.error.value(),
                logging::embed<48>(loaded.bad_copy.empty() ? std::string_view{"(not kept)"} : std::string_view{loaded.bad_copy}));
            break;
        case PersistentStateOutcome::unreadable:
        case PersistentStateOutcome::newer_version:
            // Never overwrite what could not be read: run on the model's
            // defaults and write nothing this run.
            log.warning(
                "kit: the persistent state file {} (error {}); running on the model's defaults, saving nothing this run",
                logging::static_str(
                    loaded.outcome == PersistentStateOutcome::unreadable ? "cannot be read" : "is of a later version"),
                loaded.error.value());
            persistence_blocked_ = true;
            return false;
        case PersistentStateOutcome::model_changed:
            log.warning(
                "kit: the persistent state belongs to entity model {:016x}, not this model {:016x}: discarded",
                loaded.discarded_model_id,
                loaded.state.entity_model_id);
            break;
    }
    return true;
}

void AvbEntityKit::apply_saved_settings(PersistentState const& saved)
{
    // Each value through the handler's validation: the rate (it re-times the streams), then the formats (the
    // mappings, which name their channels, off meanwhile, then apply_saved_audio_maps), clock source, then the rest.
    apply_saved_descriptor_values(saved);
    apply_saved_audio_maps(saved);
    apply_saved_presentation_offsets(saved);
    apply_saved_names(saved);
    apply_saved_mvu_settings(saved);
}

void AvbEntityKit::apply_saved_descriptor_values(PersistentState const& saved)
{
    for (auto const port : dynamic_input_ports_) {
        (void)host_.components().aem_handler.set_audio_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port, {}, false);
    }
    apply_saved_sampling_rates(saved);
    for (auto const& format : saved.stream_formats) {
        ieee::Eui64 value{};
        value.from_uint64(format.format);
        if (auto const status = storage_handler_.on_set_descriptor_value(
                atdecc::AEM_COMMAND_SET_STREAM_FORMAT,
                descriptor_id(format.descriptor_type, format.descriptor_index),
                make_const_span(value));
            status != atdecc::AEM_STATUS_SUCCESS) {
            log_refused(host_.ctl_log(), "stream format", format.descriptor_type, format.descriptor_index, status);
        }
    }
    for (auto const& source : saved.clock_sources) {
        // clock_source_index and the reserved doublet (AemClockSourcePayload's tail)
        std::array<uint8_t, 4> const value{
            static_cast<uint8_t>(source.clock_source >> 8U), static_cast<uint8_t>(source.clock_source & 0xFFU), 0, 0};
        if (auto const status = storage_handler_.on_set_descriptor_value(
                atdecc::AEM_COMMAND_SET_CLOCK_SOURCE,
                descriptor_id(atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN, source.clock_domain),
                value);
            status != atdecc::AEM_STATUS_SUCCESS) {
            log_refused(host_.ctl_log(), "clock source", atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN, source.clock_domain, status);
        }
    }
}

void AvbEntityKit::apply_saved_sampling_rates(PersistentState const& saved)
{
    for (auto const& rate : saved.sampling_rates) {
        ieee::quadlet_t const value{rate.rate};
        if (auto const status = storage_handler_.on_set_descriptor_value(
                atdecc::AEM_COMMAND_SET_SAMPLING_RATE,
                descriptor_id(atdecc::aem::DESCRIPTOR_AUDIO_UNIT, rate.audio_unit),
                make_const_span(value));
            status != atdecc::AEM_STATUS_SUCCESS) {
            log_refused(host_.ctl_log(), "sampling rate", atdecc::aem::DESCRIPTOR_AUDIO_UNIT, rate.audio_unit, status);
        }
    }
    format_notifications_.clear();  // before any controller registers
}

void AvbEntityKit::apply_saved_presentation_offsets(PersistentState const& saved)
{
    if (saved.configured_presentation_offset_ns != default_presentation_offset_ns()) {
        if (!saved.presentation_offsets.empty()) {
            host_.ctl_log().status("kit: the configured presentation offset changed: the saved ones are dropped");
        }
        return;
    }
    for (auto const& offset : saved.presentation_offsets) {
        uint8_t status = atdecc::AEM_STATUS_NOT_SUPPORTED;
        if (is_talker_index(offset.stream_output) && offset.ns <= atdecc::aem::MILAN_PRESENTATION_OFFSET_MAX_NS &&
            on_presentation_offset_changed_) {
            status = on_presentation_offset_changed_(offset.stream_output, offset.ns);
        }
        if (status != atdecc::AEM_STATUS_SUCCESS) {
            log_refused(host_.ctl_log(), "presentation offset", DESCRIPTOR_STREAM_OUTPUT, offset.stream_output, status);
            continue;
        }
        presentation_offset_ns_[offset.stream_output].store(offset.ns, std::memory_order_relaxed);
    }
}

void AvbEntityKit::apply_saved_names(PersistentState const& saved)
{
    for (auto const& name : saved.names) {
        // A saved entity name stands only while the configuration still
        // names the entity as it did when it was saved.
        if (name.descriptor_type == atdecc::aem::DESCRIPTOR_ENTITY && name.descriptor_index == 0 && name.name_index == 0 &&
            saved.configured_entity_name != configured_entity_name_) {
            host_.ctl_log().status("kit: the configured entity name changed: the saved one is dropped");
            continue;
        }
        if (auto const status = storage_handler_.on_set_name(
                descriptor_id(name.descriptor_type, name.descriptor_index), name.name_index, atdecc::aem::AtdeccString{name.value});
            status != atdecc::AEM_STATUS_SUCCESS) {
            log_refused(host_.ctl_log(), "name", name.descriptor_type, name.descriptor_index, status);
        }
    }
}

void AvbEntityKit::apply_saved_mvu_settings(PersistentState const& saved)
{
    auto& mvu = host_.components().mvu_handler;
    mvu.set_system_unique_id(saved.system_unique_id, nanoavb::mcr_domain_name(saved.system_unique_name));
    for (auto const& mcr : saved.media_clock_references) {
        nanoavb::MvuCommandHandler::McrInfo const info{
            .user_mcr_prio = mcr.user_priority, .domain_name = nanoavb::mcr_domain_name(mcr.domain_name)};
        if (!mvu.set_mcr_info(mcr.clock_domain, info)) {
            host_.ctl_log().warning(
                "kit: saved media clock reference of CLOCK_DOMAIN {}: no such clock domain: dropped", mcr.clock_domain);
        }
    }
}

void AvbEntityKit::apply_saved_listener_bindings()
{
    bool const milan = host_.components().profile.milan;
    auto const armed = restore_saved_bindings(milan);
    if (armed != 0 && milan) {
        host_.ctl_log().status("acmp: {} saved listener binding(s) restored; probing their talkers once discovered", armed);
    } else if (armed != 0) {
        host_.ctl_log().status("acmp: {} saved listener binding(s) loaded; fast-connect armed", armed);
    }
    if (milan) {
        return;  // the sinks report their binding changes (wire_milan_listener)
    }
    // Sticky from here on: every successful connect is remembered, every goal
    // change (connect, retarget, controller DISCONNECT) saved once the
    // reactor pass that made it is done (after the ACMP response).
    host_.components().acmp_listener.enable_sticky_bindings();
    host_.components().acmp_listener.set_on_goals_changed([this] { persist_pending_ = true; });
}

auto AvbEntityKit::restore_saved_bindings(bool const milan) -> size_t
{
    size_t armed = 0;
    for (auto const& binding : saved_bindings_) {
        if (restore_listener_binding(binding, milan)) {
            ++armed;
        }
    }
    saved_bindings_.clear();
    return armed;
}

auto AvbEntityKit::restore_listener_binding(PersistedListenerBinding const& binding, bool const milan) -> bool
{
    if (!saved_binding_applies(binding)) {
        return false;
    }
    if (milan) {
        return restore_milan_binding(binding);
    }
    host_.components().acmp_listener.set_fast_connect_goal(
        binding.stream_input, ieee::Eui64{}.from_uint64(binding.talker_entity_id), binding.talker_unique_id);
    return true;
}

auto AvbEntityKit::saved_binding_applies(PersistedListenerBinding const& binding) -> bool
{
    bool const known =
        std::ranges::any_of(listener_specs_, [&binding](StreamSpec const& spec) { return spec.index == binding.stream_input; });
    if (!known) {
        host_.ctl_log().warning("kit: saved listener binding of STREAM_INPUT {}: no such input: dropped", binding.stream_input);
        return false;
    }
    // An input whose format changed is not the input the binding was
    // made for.
    if (auto const format = storage_handler_.current_stream_format(
            {.configuration_index = 0, .descriptor_type = DESCRIPTOR_STREAM_INPUT, .descriptor_index = binding.stream_input});
        binding.format != 0 && (!format || format->to_uint64() != binding.format)) {
        host_.ctl_log().warning(
            "kit: saved listener binding of STREAM_INPUT {}: the input's format changed: dropped", binding.stream_input);
        return false;
    }
    return true;
}

auto AvbEntityKit::restore_milan_binding(PersistedListenerBinding const& binding) -> bool
{
    // Milan v1.2 Section 5.5.3.5.2: bound, waiting in PRB_W_AVAIL to
    // discover the talker, then probing it again.
    if (!host_.components().acmp_listener.milan_sinks().restore(
            binding.stream_input,
            atdecc::MilanSinkBinding{
                .talker_entity_id = ieee::Eui64{}.from_uint64(binding.talker_entity_id),
                .talker_unique_id = binding.talker_unique_id,
                .controller_entity_id = ieee::Eui64{}.from_uint64(binding.controller_entity_id),
                .started = binding.started})) {
        return false;
    }
    listener_->set_stream_stopped(binding.stream_input, !binding.started);
    // Bound from here on: an UNBIND_RX is a change of binding.
    input_bound_[binding.stream_input] = true;
    listener_->reset_input_counters(binding.stream_input);
    return true;
}

auto AvbEntityKit::collect_persistent_state() const -> PersistentState
{
    PersistentState state{};
    collect_entity_settings(state);
    collect_stream_settings(state);
    collect_listener_bindings(state);
    collect_mvu_settings(state);
    return state;
}

void AvbEntityKit::collect_entity_settings(PersistentState& state) const
{
    state.entity_model_id = served_entity_model_id();
    state.configured_entity_name = configured_entity_name_;
    state.configured_presentation_offset_ns = default_presentation_offset_ns();
    if (auto const name = storage_handler_.entity_name().as_string_view(); name != configured_entity_name_) {
        state.names.push_back(
            {.descriptor_type = atdecc::aem::DESCRIPTOR_ENTITY,
             .descriptor_index = 0,
             .name_index = 0,
             .value = std::string{name}});
    }
    if (auto const* group = storage_handler_.group_name_override()) {
        state.names.push_back(
            {.descriptor_type = atdecc::aem::DESCRIPTOR_ENTITY,
             .descriptor_index = 0,
             .name_index = 1,
             .value = std::string{group->as_string_view()}});
    }
    storage_handler_.for_each_object_name_override(
        [&state](uint16_t const type, uint16_t const index, atdecc::aem::AtdeccString const& name) {
            state.names.push_back(
                {.descriptor_type = type, .descriptor_index = index, .name_index = 0, .value = std::string{name.as_string_view()}});
        });
}

void AvbEntityKit::collect_stream_settings(PersistentState& state) const
{
    storage_handler_.for_each_runtime_stream_format([this,
                                                     &state](uint16_t const type, uint16_t const index, ieee::Eui64 const& format) {
        // A stream's own format is the model's: nothing to keep.
        auto const authored =
            storage_handler_.authored_stream_format({.configuration_index = 0, .descriptor_type = type, .descriptor_index = index});
        if (!authored || *authored != format) {
            state.stream_formats.push_back({.descriptor_type = type, .descriptor_index = index, .format = format.to_uint64()});
        }
    });
    collect_unit_formats(state);
    storage_handler_.for_each_runtime_sampling_rate(
        [&state](uint16_t const unit, uint32_t const rate) { state.sampling_rates.push_back({.audio_unit = unit, .rate = rate}); });
    storage_handler_.for_each_runtime_clock_source([&state](uint16_t const domain, uint16_t const source) {
        state.clock_sources.push_back({.clock_domain = domain, .clock_source = source});
    });
    for (auto const& spec : talker_specs_) {
        if (auto const ns = presentation_offset_ns(spec.index); ns != default_presentation_offset_ns()) {
            state.presentation_offsets.push_back({.stream_output = spec.index, .ns = ns});
        }
    }
    for (auto const port : dynamic_input_ports_) {
        auto const mappings = input_mappings(port);
        if (std::ranges::equal(mappings, default_input_mappings(port))) {
            continue;  // the default: nothing to keep
        }
        PersistedAudioMap map{
            .descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, .descriptor_index = port, .mappings = {}};
        for (auto const& m : mappings) {
            map.mappings.push_back(
                {.stream_index = m.stream_index,
                 .stream_channel = m.stream_channel,
                 .cluster_offset = m.cluster_offset,
                 .cluster_channel = m.cluster_channel});
        }
        state.audio_maps.push_back(std::move(map));
    }
}

void AvbEntityKit::collect_unit_formats(PersistentState& state) const
{
    // The rate is restored first and its streams follow it: while it is not the authored one, every stream of
    // the unit keeps its own format too, so one that stayed (running, on a converting port) stays.
    if (sample_rate_ == initial_rate_) {
        return;
    }
    for (bool const output : {true, false}) {
        for (auto const& spec : output ? talker_specs_ : listener_specs_) {
            uint16_t const type = output ? DESCRIPTOR_STREAM_OUTPUT : DESCRIPTOR_STREAM_INPUT;
            bool const kept = std::ranges::any_of(state.stream_formats, [&](PersistedStreamFormat const& f) {
                return f.descriptor_type == type && f.descriptor_index == spec.index;
            });
            if (!kept && is_audio_kind(spec.format.kind) && in_unit_zero(output, spec.index)) {
                state.stream_formats.push_back(
                    {.descriptor_type = type, .descriptor_index = spec.index, .format = spec.format_word});
            }
        }
    }
}

void AvbEntityKit::collect_listener_bindings(PersistentState& state) const
{
    if (listener_ == nullptr) {
        return;
    }
    // Before the listener is up (start() arms them then), the saved ones.
    if (!saved_bindings_.empty()) {
        state.listener_bindings = saved_bindings_;
    } else if (host_.components().profile.milan) {
        // Milan v1.2 Sections 5.3.8.2-3, 5.3.8.7: every bound input.
        auto const& sinks = host_.components().acmp_listener.milan_sinks();
        for (size_t i = 0; i < sinks.sink_count(); ++i) {
            auto const index = static_cast<uint16_t>(i);
            auto const* sink = sinks.sink(index);
            if (!sink->bound()) {
                continue;
            }
            auto const format = storage_handler_.current_stream_format(
                {.configuration_index = 0, .descriptor_type = DESCRIPTOR_STREAM_INPUT, .descriptor_index = index});
            state.listener_bindings.push_back(
                {.stream_input = index,
                 .talker_entity_id = sink->binding.talker_entity_id.to_uint64(),
                 .talker_unique_id = sink->binding.talker_unique_id,
                 .format = format ? format->to_uint64() : 0,
                 .controller_entity_id = sink->binding.controller_entity_id.to_uint64(),
                 .started = sink->binding.started});
        }
    } else {
        for (auto const& goal : host_.components().acmp_listener.fast_connect_goals()) {
            auto const format = storage_handler_.current_stream_format(
                {.configuration_index = 0,
                 .descriptor_type = DESCRIPTOR_STREAM_INPUT,
                 .descriptor_index = goal.listener_unique_id});
            state.listener_bindings.push_back(
                {.stream_input = goal.listener_unique_id,
                 .talker_entity_id = goal.talker_entity_id.to_uint64(),
                 .talker_unique_id = goal.talker_unique_id,
                 .format = format ? format->to_uint64() : 0});
        }
    }
}

void AvbEntityKit::collect_mvu_settings(PersistentState& state) const
{
    auto const& mvu = host_.components().mvu_handler;
    state.system_unique_id = mvu.system_unique_id();
    auto const& name = mvu.system_unique_name();
    state.system_unique_name.assign(name.data(), static_cast<size_t>(std::ranges::find(name, '\0') - name.begin()));
    for (size_t index = 0; index < mvu.clock_domain_count(); ++index) {
        nanoavb::MvuCommandHandler::McrInfo const defaults{.user_mcr_prio = mvu.default_mcr_priority(index)};
        if (auto const* info = mvu.mcr_info(index); info != nullptr && !(*info == defaults)) {
            auto const& field = info->domain_name;
            auto const length = static_cast<size_t>(std::ranges::find(field, '\0') - field.begin());
            state.media_clock_references.push_back(
                {.clock_domain = static_cast<uint16_t>(index),
                 .user_priority = info->user_mcr_prio,
                 .domain_name = std::string{field.data(), length}});
        }
    }
}

void AvbEntityKit::persist()
{
    persist_pending_ = false;
    if (!persistence_active_ || persistence_blocked_) {
        return;  // off, or the file did not read at start: never overwritten this run
    }
    auto text = serialize_persistent_state(collect_persistent_state());
    if (text != persisted_text_) {
        save_persisted(std::move(text));
    }
}

void AvbEntityKit::save_persisted(std::string text)
{
    if (auto const saved = save_persistent_state(config_.persistent_state_path, text); !saved) {
        host_.ctl_log().warning("kit: could not save the persistent state: error {}", saved.error().value());
        return;
    }
    persisted_text_ = std::move(text);
}

auto AvbEntityKit::make_talker_srp_info(StreamSpec const& spec) const -> nanoavb::TalkerStreamSrpInfo
{
    // TSpec from the format word — no per-entity byte math to keep in step
    // with the model; the hardware shaper sizes by the same one.
    auto const tspec = talker_tspec(spec);
    nanoavb::TalkerStreamSrpInfo info{
        .max_frame_size = tspec.max_frame_size,
        .max_interval_frames = tspec.max_interval_frames,
        .accumulated_latency = talker_port_latency(spec)};
    if (auto const* stream = host_.components().acmp_talker.get_stream(spec.index); stream != nullptr) {
        (void)statusbar::tsn::load_unchecked(stream->stream_id.span(), &info.stream_id);
        info.dest_address = stream->stream_dest_mac;
        info.vlan_id = stream->stream_vlan_id;
    } else {
        info.vlan_id = config_.vlan_id;
    }
    return info;
}

auto AvbEntityKit::talker_port_latency(StreamSpec const& spec) const noexcept -> uint32_t
{
    uint64_t octets = 0;
    for (auto const& talker : talker_specs_) {
        octets += interval_wire_octets(talker_tspec(talker));
    }
    TalkerPort const port{.link_mbps = link_mbps_, .mtu = mtu_, .cbs = class_a_cbs_};
    return talker_port_latency_ns(port, octets, interval_wire_octets(talker_tspec(spec)));
}

auto AvbEntityKit::declared_talker_latency_ns(uint16_t const idx) const noexcept -> std::optional<uint32_t>
{
    auto const* const stream = host_.components().acmp_talker.get_stream(idx);
    if (stream == nullptr || !declarations_.declared(idx)) {
        return std::nullopt;
    }
    nanoavb::StreamId stream_id{};
    (void)statusbar::tsn::load_unchecked(stream->stream_id.span(), &stream_id);
    auto const* const declared = host_.components().msrp_handler.get_talker_stream(stream_id);
    return declared != nullptr ? std::optional{declared->accumulated_latency} : std::nullopt;
}

auto AvbEntityKit::talker_accumulated_latency_ns(uint16_t const idx) const noexcept -> uint32_t
{
    if (auto const declared = declared_talker_latency_ns(idx)) {
        return *declared;
    }
    auto const* const spec = std::ranges::find(talker_specs_, idx, &StreamSpec::index);
    return spec == talker_specs_.end() ? 0U : talker_port_latency(*spec);
}

void AvbEntityKit::advertise_talker_streams(TimePoint const time)
{
    // The control plane advertises: each stream declares when TalkerDeclarations
    // says so, after its MAAP address and, under Milan, interest.
    declarations_.set_active(true);
    update_talker_declarations(steady_ns_of(time), true);
}

}  // namespace statusbar::avb_entity
