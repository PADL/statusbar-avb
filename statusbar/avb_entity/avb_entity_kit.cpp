// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AvbEntityKit methods — the Entity Construction Kit facade (refactor phase D;
// the wiring bodies come verbatim from AvbEntityToneGenerator/AvbEntityAudioIO,
// which each carried a copy).

#include "statusbar/avb_entity/avb_entity_kit.hpp"

#include "statusbar/avb_entity/avb_entity_identity.hpp"
#include "statusbar/avb_entity/avb_entity_shaper.hpp"
#include "statusbar/avb_entity/avb_entity_stream_info.hpp"
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
auto load_descriptor(AvbEntityHost const& host, uint16_t const type, uint16_t const index) -> std::optional<T>
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
auto audio_cluster(AvbEntityHost const& host, atdecc::aem::DescriptorStreamPort const& port, uint16_t const offset)
    -> std::optional<atdecc::aem::DescriptorAudioCluster>
{
    return load_descriptor<atdecc::aem::DescriptorAudioCluster>(
        host, atdecc::aem::DESCRIPTOR_AUDIO_CLUSTER, static_cast<uint16_t>(port.base_cluster.get() + offset));
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
    std::atomic<uint64_t>& last_gptp_ns,
    StreamRxAudioSink* audio_sink,
    std::pmr::memory_resource* memory_resource)
    : config_{config}
    , talker_specs_{talker_specs}
    , listener_specs_{listener_specs}
    , sample_rate_{sample_rate}
    , storage_handler_{handler}
    , host_{handler, default_adp_advertiser_config(), talker_specs_.size(), 4, listener_specs_.size(), config.milan}
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
    }

    // ATDECC descriptor wire version. Default "2016": emit descriptors at their
    // 1722.1-2013/2016 lengths so controllers that reject 2021-length descriptors
    // can enumerate the model. "2021" emits the full 2021 forms.
    host_.components().aem_handler.set_legacy_2016(config_.atdecc_version != "2021");

    configure_talker_streams();
    configure_class_a_domain();
    wire_controller_hooks();
}

void AvbEntityKit::configure_talker_streams()
{
    // Base the talker stream_ids on the NIC MAC (globally unique per box), and
    // seed the static dest MACs by kind (MAAP mode reassigns after acquisition).
    ieee::Eui48 const stream_base_mac = stream_base_mac_for(config_.interface_name, config_.entity_id);
    for (auto const& spec : talker_specs_) {
        (void)host_.components().acmp_talker.configure_stream(
            spec.index, stream_id_for(stream_base_mac, spec.index), talker_dest_mac(config_, spec.format.kind), config_.vlan_id);
        note_talker_interface(spec.index);
        // Static destination addresses are valid from the start; MAAP's once
        // its block is defended (start() and the acquirer's callbacks).
        declarations_.note_address_valid(spec.index, config_.stream_address_mode != "maap");
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
}

void AvbEntityKit::note_talker_interface(uint16_t const index)
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
    (void)host_.components().mvrp_handler.register_vlan(config_.vlan_id, sm::Clock::now());
    host_.components().msrp_handler.set_domain(
        DomainInfo{.sr_class_id = 6, .sr_class_priority = 3, .sr_class_vid = config_.vlan_id});
    // The Class A VID in use (adopted MSRP Domain): tag stream frames with
    // it, and keep it on the NIC's VLAN filter for the listener streams (a
    // no-op until bind_listener_vlan()).
    host_.set_on_domain_vid_changed([this](uint16_t const vlan_id, TimePoint const time) {
        talker_->set_vlan_id(vlan_id);
#if defined(__linux__)
        listener_vlan_.follow(vlan_id);
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

void AvbEntityKit::wire_controller_hooks()
{
    wire_storage_hooks();
    wire_dynamic_mappings();
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
    storage_handler_.set_on_streaming_changed([this](uint16_t type, uint16_t index, bool streaming) -> uint8_t {
        if (type == DESCRIPTOR_STREAM_OUTPUT) {
            talker_->set_stream_stopped(index, !streaming);
            note_admission(steady_now_ns());  // STREAM_START / STREAM_STOP now
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
    // none is saved). SET_SAMPLING_RATE: the unit's current rate only.
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
    auto const& specs = port_type == atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT ? talker_specs_ : listener_specs_;
    auto const* spec = std::ranges::find(specs, port_index, &StreamSpec::index);
    auto const port = load_descriptor<atdecc::aem::DescriptorStreamPort>(host_, port_type, port_index);
    // Milan v1.2 Section 5.4.2.27: none for a stream at another rate than the unit's.
    if (spec == specs.end() || !port || spec->format.sample_rate_hz != sample_rate_) {
        return {};
    }
    return cluster_identity_mappings(port_index, *port, spec->format.channels);
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
    return static_map_channels(atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT, stream_index);
}

auto AvbEntityKit::static_map_channels(uint16_t const port_type, uint16_t const stream_index) const -> uint16_t
{
    // Every such port's static AUDIO_MAPs (the model's): the channels of
    // stream @p stream_index of the port's direction they name.
    uint16_t needed = 0;
    for (uint16_t p = 0; p < UINT16_MAX; ++p) {
        auto const port = load_descriptor<atdecc::aem::DescriptorStreamPort>(host_, port_type, p);
        if (!port) {
            break;
        }
        for (uint16_t i = 0; i < port->number_of_maps.get(); ++i) {
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
    if (static_map_channels(
            output ? atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT : atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, index) != 0) {
        return true;
    }
    bool named = false;
    if (!output) {
        host_.components().aem_handler.dynamic_audio_maps().for_each(
            [&named, index](uint16_t const port_type, uint16_t /*port_index*/, nanoavb::DynamicAudioMapping const& m) {
                named = named || (port_type == atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT && m.stream_index == index);
            });
    }
    return named;
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

auto AvbEntityKit::output_streaming(uint16_t const idx, int64_t const now_ns) const noexcept -> bool
{
    return talker_should_transmit(idx, now_ns) || (declarations_.declared(idx) && gate_.listener_admitted(idx, now_ns));
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
    if (!is_audio_kind(updated.format.kind)) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;
    }
    // Milan v1.2 Section 5.4.2.7.
    if (output ? output_streaming(spec.index, steady_now_ns()) : input_running(spec.index)) {
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
    // Milan v1.2 Sections 5.4.2.7, 5.4.2.27: no port maps a stream at another rate than the
    // unit's, so it takes one only while no map names it (the kit's own output identity goes).
    if (updated.format.sample_rate_hz != sample_rate_ && stream_mapped(output, updated.index)) {
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
        refresh_output_mappings();
    }
    host_.ctl_log().status(
        "aem: {} stream {} format {:016x} -> {:016x}",
        output ? logging::lit("talker") : logging::lit("listener"),
        updated.index,
        before,
        updated.format_word);
    if (output && declarations_.declared(updated.index)) {
        update_talker_declarations(steady_now_ns(), true);  // its TSpec
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
    StreamPort* const port = listener_ != nullptr ? listener_->port_ : nullptr;
    if (port == nullptr) {
        return;
    }
    auto const before = rx_static_groups_;
    for (size_t i = 0; i < rx_static_groups_.size() && i < listener_specs_.size(); ++i) {
        rx_static_groups_[i] = static_rx_group(listener_specs_[i]);
    }
    // A port's memberships may not be counted (AF_XDP): a group goes once nothing holds it, statically
    // or by an ACMP connection, and a new one comes once.
    for (auto const& group : before) {
        if (group != ieee::Eui48{} && std::ranges::find(rx_static_groups_, group) == rx_static_groups_.end() &&
            !group_connected(group)) {
            (void)port->leave_multicast(group);
        }
    }
    for (auto const& group : rx_static_groups_) {
        if (group != ieee::Eui48{} && std::ranges::find(before, group) == before.end()) {
            (void)port->join_multicast(group);
        }
    }
}

auto AvbEntityKit::group_connected(ieee::Eui48 const& group) const -> bool
{
    return std::ranges::any_of(listener_specs_, [this, &group](StreamSpec const& spec) {
        auto const* const stream = host_.components().acmp_listener.get_stream(spec.index);
        return stream != nullptr && stream->connected && stream->stream_dest_mac == group;
    });
}

auto AvbEntityKit::talkers_fit(uint16_t const index, uint64_t const word) const noexcept -> bool
{
    auto const kbps = [](StreamSpec const& s) {
        return uint64_t{stream_bandwidth_kbps(talker_tspec(s), CLASS_A_INTERVALS_PER_SEC)};
    };
    uint64_t before = 0;
    uint64_t after = 0;
    for (auto const& spec : talker_specs_) {
        before += kbps(spec);
        after += spec.index == index ? kbps(with_format(spec, word)) : kbps(spec);
    }
    return after <= before || after <= static_cast<uint64_t>(sr_budget_kbps(link_mbps_ * 1000U));
}

auto AvbEntityKit::change_sampling_rate(uint16_t const audio_unit, uint32_t const rate) -> uint8_t
{
    // Milan v1.2 Section 5.4.2.13: the unit's current rate is a no-op; the kit re-times no
    // unit, so any other rate it lists is NOT_SUPPORTED (a stream's rate is its format's).
    auto const in_force = storage_handler_.current_sampling_rate(
        {.configuration_index = 0, .descriptor_type = atdecc::aem::DESCRIPTOR_AUDIO_UNIT, .descriptor_index = audio_unit});
    return in_force == rate ? atdecc::AEM_STATUS_SUCCESS : atdecc::AEM_STATUS_NOT_SUPPORTED;
}

auto AvbEntityKit::request_entity_name(std::string_view const name) -> uint8_t
{
    auto& aem = host_.components().aem_handler;
    if (aem.is_locked() || aem.is_acquired()) {
        return atdecc::AEM_STATUS_ENTITY_LOCKED;
    }
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

auto AvbEntityKit::admitted_talker_streams(int64_t const now_ns) const noexcept -> AdmittedStreams
{
    AdmittedStreams admitted{};
    for (auto const& spec : talker_specs_) {
        if (gate_.admitted(spec.index, now_ns)) {
            (void)admitted.try_push_back(spec.index);
        }
    }
    return admitted;
}

void AvbEntityKit::note_admission(int64_t const now_ns)
{
    update_output_streaming(now_ns);
    auto admitted = admitted_talker_streams(now_ns);
    if (std::ranges::equal(admitted, admitted_)) {
        return;
    }
    admitted_ = admitted;
    if (on_admission_changed_) {
        on_admission_changed_(std::span<uint16_t const>{admitted_.data(), admitted_.size()});
    }
}

void AvbEntityKit::update_output_streaming(int64_t const now_ns)
{
    if (talker_ == nullptr) {
        return;
    }
    for (auto const& spec : talker_specs_) {
        bool const on = talker_should_transmit(spec.index, now_ns);
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

void AvbEntityKit::note_listener_ready(nanoavb::StreamId const& stream_id, bool const ready, int64_t const now_ns)
{
    auto const idx = talker_index_of(stream_id);
    // Only a declared stream streams (Milan v1.2 Section 5.3.7.3): a Listener
    // registered for a withdrawn stream opens nothing; re-declaring re-learns it.
    if (!idx || declarations_.declared(*idx)) {
        gate_.note_listener_ready(stream_id, ready, now_ns);
    }
    // Any Listener registration keeps the stream declared (Section 4.3.3.1),
    // and GET_STREAM_INFO reports REGISTERING / REGISTERING_FAILED.
    if (idx && refresh_talker_listener(*idx, now_ns)) {
        bool const was_declared = declarations_.declared(*idx);
        update_talker_declarations(now_ns);
        if (declarations_.declared(*idx) == was_declared) {
            notify_talker_stream_info(*idx);  // else the (un)declaration notified
        }
    }
    note_admission(now_ns);
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

auto AvbEntityKit::refresh_talker_listener(uint16_t const idx, int64_t const now_ns) -> bool
{
    auto const listener = registered_listener_of(idx);
    if (!listener) {
        return false;
    }
    bool const changed = *listener != talker_listener_[idx];
    talker_listener_[idx] = *listener;
    declarations_.note_listener_registered(idx, *listener != srp::msrp::ListenerDeclaration::Ignore, now_ns);
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

void AvbEntityKit::note_talker_probe(uint16_t const idx, int64_t const now_ns)
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
    declarations_.note_probe(idx, now_ns);
    update_talker_declarations(now_ns);
}

void AvbEntityKit::note_talker_addresses(bool const valid, int64_t const now_ns)
{
    std::array<bool, MAX_ENTITY_STREAMS> was_declared{};
    for (auto const& spec : talker_specs_) {
        declarations_.note_address_valid(spec.index, valid);
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

void AvbEntityKit::update_talker_declarations(int64_t const now_ns, bool const refresh)
{
    // MSRP calls back on the reactor; a nested evaluation would act on the
    // table the outer one is walking: it is noted, and the outer one runs
    // again once its pass is done.
    if (updating_declarations_) {
        declarations_pending_ = true;
        declarations_pending_refresh_ = declarations_pending_refresh_ || refresh;
        return;
    }
    run_declaration_passes(now_ns, refresh);
    declaration_deadline_ns_ = declarations_.next_deadline(now_ns);
    note_admission(now_ns);
}

void AvbEntityKit::run_declaration_passes(int64_t const now_ns, bool const refresh)
{
    updating_declarations_ = true;
    run_talker_declarations(now_ns, refresh);
    for (int pass = 0; declarations_pending_ && pass < 4; ++pass) {
        bool const again_refresh = declarations_pending_refresh_;
        declarations_pending_ = false;
        declarations_pending_refresh_ = false;
        run_talker_declarations(now_ns, again_refresh);
    }
    declarations_pending_ = false;
    declarations_pending_refresh_ = false;
    updating_declarations_ = false;
}

void AvbEntityKit::run_talker_declarations(int64_t const now_ns, bool const refresh)
{
    using Action = TalkerDeclarations::Action;
    auto& msrp = host_.components().msrp_handler;
    TimePoint const time = sm::Clock::now();
    for (auto const& spec : talker_specs_) {
        auto const info = make_talker_srp_info(spec);
        TalkerDeclarations::Params params{.dest_mac = info.dest_address, .vlan_id = info.vlan_id};
        if (auto const* s = host_.components().acmp_talker.get_stream(spec.index); s != nullptr) {
            params.stream_id = s->stream_id;
        }
        bool const was_declared = declarations_.declared(spec.index);
        switch (declarations_.evaluate(spec.index, now_ns, params, refresh)) {
            case Action::declare: {
                if (auto result = msrp.talker_advertise(info, time); !result) {
                    host_.ctl_log().warning(
                        "msrp: talker_advertise (stream {}) failed: errno {}", spec.index, result.error().value());
                    break;
                }
                declarations_.note_declared(spec.index, params);
                // Re-learn the gate from the live registration: a withdraw forgot it,
                // and a registration that stayed raises no fresh MSRP event.
                gate_.note_listener_ready(info.stream_id, msrp.listener_permit_debug(info.stream_id).permits, now_ns);
                // A Listener registered or gone changes REGISTERING (Milan v1.2 Table 5.12).
                bool const listener_changed = refresh_talker_listener(spec.index, now_ns);
                if (!was_declared) {
                    host_.ctl_log().status(
                        "msrp: talker stream {} ({}) Talker Advertise declared",
                        spec.index,
                        logging::static_str(stream_kind_name(spec.format.kind)));
                }
                if (!was_declared || listener_changed) {
                    notify_talker_stream_info(spec.index);
                }
                break;
            }
            case Action::withdraw:
                (void)msrp.talker_withdraw(info.stream_id, time);
                declarations_.note_withdrawn(spec.index, now_ns);
                // No Talker Advertise, no streaming (Milan v1.2 5.3.7.3).
                gate_.note_talker_withdrawn(spec.index);
                host_.ctl_log().status(
                    "msrp: talker stream {} ({}) Talker Advertise withdrawn",
                    spec.index,
                    logging::static_str(stream_kind_name(spec.format.kind)));
                notify_talker_stream_info(spec.index);
                break;
            case Action::none:
            default:
                break;
        }
    }
}

void AvbEntityKit::withdraw_talker_streams(TimePoint const time)
{
    for (auto const& spec : talker_specs_) {
        (void)host_.components().msrp_handler.talker_withdraw(make_talker_srp_info(spec).stream_id, time);
    }
    int64_t const now_ns = steady_now_ns();
    bool const any =
        std::ranges::any_of(talker_specs_, [this](StreamSpec const& spec) { return declarations_.declared(spec.index); });
    note_all_talkers_withdrawn(now_ns);
    if (any) {
        for (auto const& spec : talker_specs_) {
            notify_talker_stream_info(spec.index);
        }
    }
}

void AvbEntityKit::note_all_talkers_withdrawn(int64_t const now_ns)
{
    declarations_.set_active(false);
    declarations_.note_all_withdrawn(now_ns);
    declaration_deadline_ns_ = declarations_.next_deadline(now_ns);
    // No Talker Advertise, no streaming (Milan 1.2 5.3.7.3): close every
    // stream now, without the grace window.
    gate_.note_talker_withdrawn();
    note_admission(now_ns);
}

void AvbEntityKit::wire_stream_callbacks()
{
    // MSRP reservations for the talker streams. The host declares the SR class
    // domain before calling these, then advertises/withdraws on the MSRP cycle.
    host_.set_advertise_streams([this](TimePoint time) { advertise_talker_streams(time); });
    host_.set_withdraw_streams([this](TimePoint time) { withdraw_talker_streams(time); });
    // The transmit gate tracks the MSRP listener declarations against our talker
    // streams; that alone admits a stream (docs/AVB_ENTITY_KIT.md).
    host_.set_on_listener_ready(
        [this](nanoavb::StreamId const& stream_id, bool ready) { note_listener_ready(stream_id, ready, steady_now_ns()); });

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
            [this](uint16_t stream_index, ieee::Eui64 const& stream_id, ieee::Eui48 dest_mac) {
                listener_->on_listener_connected(stream_index, stream_id, dest_mac);
                if (host_.components().profile.milan) {
                    if (auto const* s = host_.components().acmp_listener.get_stream(stream_index); s != nullptr) {
                        join_sink_vlan(s->stream_vlan_id);
                    }
                    refresh_sink_registration(stream_index);
                }
            },
            [this](uint16_t stream_index) {
                if (auto const* s = host_.components().acmp_listener.get_stream(stream_index);
                    host_.components().profile.milan && s != nullptr && s->connected) {
                    leave_sink_vlan(s->stream_vlan_id);
                }
                // A classic disconnect is the controller's: no interruption.
                // (A Milan input unsettles for other reasons too; its unbind is
                // the sink's binding change.)
                listener_->on_listener_disconnected(stream_index);
                if (!host_.components().profile.milan) {
                    listener_->note_input_unbound(stream_index);
                }
            });
    }
    wire_aecp_queries();
}

void AvbEntityKit::wire_aecp_queries()
{
    // AECP GET_COUNTERS (STREAM_OUTPUT talker rate + STREAM_INPUT listener
    // health; ENTITY, AVB_INTERFACE and CLOCK_DOMAIN from the host) and GET_STREAM_INFO (talker stream identity, queried by a Milan
    // listener to verify the stream before sustaining a connection; listener
    // sink identity so a controller can read what an input is connected to).
    host_.components().aem_handler.set_get_counters(
        [this](uint16_t descriptor_type, uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& counters) -> bool {
            bool const milan = host_.components().profile.milan;
            if (descriptor_type == DESCRIPTOR_STREAM_OUTPUT) {
                return is_talker_index(descriptor_index) &&
                    fill_talker_stream_counters(
                           *talker_, descriptor_index, output_counters_[descriptor_index], milan, valid, counters);
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
                    .accumulated_latency_ns = TALKER_ACCUMULATED_LATENCY_NS};
                filled = fill_talker_stream_info(host_, descriptor_index, status, out, ext);
            } else if (descriptor_type == DESCRIPTOR_STREAM_INPUT && listener_ != nullptr) {
                filled = milan ? fill_milan_listener_stream_info(host_, descriptor_index, out, ext)
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
        [this](uint16_t const idx, TimePoint) { note_talker_probe(idx, steady_now_ns()); });

    // The presentation time offset (Milan v1.2 Sections 5.4.2.9-10): refused
    // while the output is streaming and when the entity cannot apply it.
    components.aem_handler.set_presentation_offset_hooks(
        [this](uint16_t const idx) -> std::optional<uint32_t> {
            return is_talker_index(idx) ? std::optional<uint32_t>{presentation_offset_ns(idx)} : std::nullopt;
        },
        [this](uint16_t const idx, uint32_t const offset_ns) -> uint8_t {
            if (!on_presentation_offset_changed_) {
                return atdecc::AEM_STATUS_NOT_SUPPORTED;
            }
            // Streaming as Milan v1.2 Section 5.3.7.3 has it: declaring and a
            // Listener Ready / Ready Failed registered (the grace window
            // included), even with the transmit gate disabled.
            if (declarations_.declared(idx) && gate_.listener_admitted(idx, steady_now_ns())) {
                return atdecc::AEM_STATUS_STREAM_IS_RUNNING;
            }
            uint32_t const before = presentation_offset_ns(idx);
            if (offset_ns == before) {
                return atdecc::AEM_STATUS_SUCCESS;
            }
            if (auto const status = on_presentation_offset_changed_(idx, offset_ns); status != atdecc::AEM_STATUS_SUCCESS) {
                return status;
            }
            presentation_offset_ns_[idx].store(offset_ns, std::memory_order_relaxed);
            host_.ctl_log().status("aem: talker stream {} presentation offset {} -> {} ns", idx, before, offset_ns);
            return atdecc::AEM_STATUS_SUCCESS;
        });
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
                    listener_->note_input_unbound(index);
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
    components.msrp_handler.set_on_remote_talker([this](nanoavb::StreamId const& sid) {
        auto const& sinks = host_.components().acmp_listener.milan_sinks();
        for (size_t i = 0; i < sinks.sink_count(); ++i) {
            auto const index = static_cast<uint16_t>(i);
            auto const* sink = sinks.sink(index);
            if (sink->settled() && sink->stream.stream_id.to_uint64() == sid.to_uint64()) {
                refresh_sink_registration(index);
            }
        }
    });
}

void AvbEntityKit::refresh_sink_registration(uint16_t const index)
{
    auto& sinks = host_.components().acmp_listener.milan_sinks();
    auto const* sink = sinks.sink(index);
    if (sink == nullptr || !sink->settled()) {
        return;
    }
    sinks.set_registration(index, talker_registration(*sink), sm::Clock::now());
}

auto AvbEntityKit::talker_registration(atdecc::MilanSink const& sink) const -> atdecc::MilanTalkerRegistration
{
    // Milan v1.2 Section 5.5.3.3: a Talker attribute matching the stream ID,
    // destination address and VLAN ID the probe returned.
    statusbar::tsn::StreamId sid{};
    (void)statusbar::tsn::load_unchecked(sink.stream.stream_id.span(), &sid);
    auto const matches = [&sink](srp::msrp::TalkerAdvertiseFirstValue const& fv) {
        return fv.destination_address == sink.stream.dest_mac && fv.vlan_identifier.get() == sink.stream.vlan_id;
    };
    if (auto const* failed = host_.components().msrp_handler.peer_talker_failed(sid);
        failed != nullptr && matches(failed->advertise)) {
        return atdecc::MilanTalkerRegistration::failed;
    }
    auto const* advertise = host_.components().msrp_handler.peer_talker_advertise(sid);
    return advertise != nullptr && matches(*advertise) ? atdecc::MilanTalkerRegistration::advertise
                                                       : atdecc::MilanTalkerRegistration::none;
}

void AvbEntityKit::join_sink_vlan(uint16_t const vid)
{
    if (vid == 0 || vid > 4094) {
        return;
    }
    for (auto& [joined, count] : sink_vlans_) {
        if (joined == vid) {
            ++count;
            return;
        }
    }
    (void)sink_vlans_.try_push_back({vid, uint16_t{1}});
    // The Class A VID in use is the host's (always joined).
    if (vid != host_.components().msrp_handler.domain().sr_class_vid) {
        (void)host_.components().mvrp_handler.register_vlan(vid, sm::Clock::now());
#if defined(__linux__)
        host_.ctl_log().warning(
            "listener: a stream on VLAN {}, not the Class A VLAN {}: joined with MVRP, but not on the NIC's VLAN filter",
            vid,
            host_.components().msrp_handler.domain().sr_class_vid);
#endif
    }
}

void AvbEntityKit::leave_sink_vlan(uint16_t const vid)
{
    for (auto* it = sink_vlans_.begin(); it != sink_vlans_.end(); ++it) {
        if (it->first != vid) {
            continue;
        }
        if (--it->second == 0) {
            sink_vlans_.erase(it);
            if (vid != host_.components().msrp_handler.domain().sr_class_vid) {
                (void)host_.components().mvrp_handler.withdraw_vlan(vid, sm::Clock::now());
            }
        }
        return;
    }
}

auto AvbEntityKit::acquire_maap_addresses(net::MessageReactor& reactor) -> Status
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
        [this]() { note_talker_addresses(true, steady_now_ns()); },
        [this]() { note_talker_addresses(false, steady_now_ns()); });
}

auto AvbEntityKit::start(net::MessageReactor& reactor) -> Status
{  // NOLINT(statusbar-block-statements): the bring-up phases in order, then what they leave behind
    auto const ports = stream_ports_options();
    if (auto status = check_stream_rx(ports); !status) {
        return status;
    }
    if (auto status = start_control(reactor); !status) {
        return status;
    }
    if (auto status = start_data_plane(reactor, ports); !status) {
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

auto AvbEntityKit::stream_ports_options() const -> StreamPortsOptions
{
    return StreamPortsOptions{
        .interface_name = config_.interface_name,
        .tx_priority = config_.stream_pcp,
        .receive = listener_ != nullptr,
        .transport = config_.stream_transport,
        .xdp_mode = config_.stream_xdp_mode,
        .xdp_queue = config_.stream_xdp_queue,
        .rx_on_own_thread = config_.stream_rx_rt_timer && !config_.stream_rx_on_media_thread,
        .launch_time = config_.stream_launch_time,
    };
}

auto AvbEntityKit::check_stream_rx(StreamPortsOptions const& ports) -> Status
{
    // An AF_XDP socket has one consumer: the media thread that ticks it.
    bool const refused = bound_port_ != nullptr ? ports.receive && ports.rx_on_own_thread && bound_port_->rx_fd() < 0
                                                : !StreamPorts::check(ports).has_value();
    if (refused) {
        host_.ctl_log().error("kit: an AF_XDP (or bound, unpollable) stream RX port is drained on the media thread only: "
                              "stream_rx_rt_timer needs stream_rx_on_media_thread");
        return failure(std::errc::invalid_argument);
    }
    return success();
}

auto AvbEntityKit::start_control(net::MessageReactor& reactor) -> Status
{
    prepare_control_plane();
    if (auto status = host_.start_control_plane(reactor, config_.interface_name); !status) {
        return status;
    }
    wire_stream_callbacks();
    // A restarted kit whose control plane stayed Ready gets no advertise hook
    // again (stop() withdrew everything): advertise now.
    if (host_.is_ready()) {
        advertise_talker_streams(sm::Clock::now());
    }
    return start_declarations(reactor);
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
    // The saved settings, before the entity shows itself to the network.
    load_persistent_state();
}

auto AvbEntityKit::start_declarations(net::MessageReactor& reactor) -> Status
{
    // In "maap" mode, claim the talker stream destination addresses via MAAP and
    // overwrite the static defaults BEFORE the data plane reads them below.
    // Non-fatal inside: on socket failure the entity keeps the static MACs.
    if (config_.stream_address_mode == "maap") {
        if (auto status = acquire_maap_addresses(reactor); !status) {
            return status;
        }
    }
    // Static addresses are valid at once, MAAP's when the block is defended. Until
    // then the configured default must not reach a listener (TALKER_DEST_MAC_FAIL).
    for (auto const& spec : talker_specs_) {
        declarations_.note_address_valid(spec.index, maap_.ready());
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
        void tick(int64_t /*now_ns*/) override
        {
            if (auto const kit = kit_.lock(); kit != nullptr) {
                (*kit)->on_declaration_timer(steady_now_ns());
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

auto AvbEntityKit::start_data_plane(net::MessageReactor& reactor, StreamPortsOptions const& ports) -> Status
{
    if (auto status = open_talker_streams(); !status) {
        return status;
    }
    auto const rx_port = open_stream_ports(ports);
    if (!rx_port) {
        return forward_failure(rx_port);
    }
    if (listener_ != nullptr) {
        if (auto status = start_listener_streams(reactor, *rx_port); !status) {
            return status;
        }
    }
    // TX on the port + optional pcap capture (TalkerStreams assembles its own I/O).
    if (!stream_tx_error_) {
        if (auto tx = talker_->open_tx(
                *tx_port_,
                TalkerStreams::TxPcapConfig{
                    .path = config_.tx_pcap_path, .max_bytes = config_.tx_pcap_max_bytes, .seconds = config_.tx_pcap_seconds});
            !tx) {
            stream_tx_error_ = tx.error();
        }
    }
    return success();
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

auto AvbEntityKit::open_stream_ports(StreamPortsOptions const& ports) -> StatusValue<StreamPort*>
{
    // The stream ports: the kit's own (config.stream_transport) unless a port
    // is bound. Tolerated here (the entity stays a control-plane-only node);
    // recorded for a host that needs the data plane (stream_tx_error()).
    stream_tx_error_ = {};
    stream_rx_error_ = {};
    rx_on_tick_ = false;
    if (bound_port_ != nullptr) {
        tx_port_ = bound_port_;
        return bound_port_;
    }
    return open_own_stream_ports(ports);
}

auto AvbEntityKit::open_own_stream_ports(StreamPortsOptions const& ports) -> StatusValue<StreamPort*>
{  // NOLINT(statusbar-block-statements): the ports opened, then their errors recorded and logged
    if (auto opened = ports_.open(ports); !opened) {
        return forward_failure(opened);  // check()ed above
    }
    tx_port_ = &ports_.tx();
    stream_tx_error_ = ports_.tx_error();
    stream_rx_error_ = ports_.rx_error();
    if (auto const priority = ports_.packet().tx_priority_error(); !stream_tx_error_ && priority) {
        host_.ctl_log().warning(
            "talker: SO_PRIORITY {} on the stream TX socket failed: errno {}; streams use the default TX queue",
            config_.stream_pcp,
            priority.value());
    }
    log_xdp_error(host_.ctl_log(), ports_, config_.interface_name);
    log_launch_time(host_.ctl_log(), ports_, ports);
    return &ports_.rx();
}

auto AvbEntityKit::start_listener_streams(net::MessageReactor& reactor, StreamPort* const rx_port) -> Status
{
    if (auto status = open_listener_streams(); !status) {
        return status;
    }
    // The handler is kept for the tool's SCHED_FIFO RX timer
    // (drain_stream_rx() with its wake gPTP time), or for the media tick
    // when the port has no descriptor (tick_stream_rx()).
    if (!stream_rx_error_) {
        bool const pollable = rx_port->rx_fd() >= 0;
        if (auto rx = listener_->attach_rx(*rx_port, rx_static_groups_, reactor, config_.stream_rx_rt_timer || !pollable)) {
            rt_rx_handler_ = std::move(*rx);
            rx_on_tick_ = !pollable && !config_.stream_rx_rt_timer;
        } else {
            stream_rx_error_ = rx.error();  // tolerated here; a host may refuse (stream_rx_error())
            ports_.close_rx();              // nobody reads it
        }
    }
#if defined(__linux__)
    // The VID's tagged stream frames must get past the NIC's VLAN filter.
    // Never fatal: without CAP_NET_ADMIN this only warns.
    // (A control bound before start() -- a test's -- is used as it is.)
    if (!listener_vlan_.bound()) {
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
    // Static RX groups: an audio input pre-joins the same-kind talker
    // output's group (the loopback topology listens on its own output
    // group); everything else joins dynamically on ACMP connect (a zero
    // MAC just opens the socket).
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
    listener_vlan_.bind(control, config_.interface_name, ifindex);
    listener_vlan_.follow(host_.components().msrp_handler.domain().sr_class_vid);
}
#endif

auto AvbEntityKit::stop() -> Status
{
    close_data_plane();
    if (!host_.is_running()) {
        return failure(std::make_error_code(std::errc::not_connected));
    }
    auto const now = TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    // Stopping the control plane runs no withdraw hook: withdraw here, so a
    // restart starts undeclared, inactive and with every gate closed.
    withdraw_talker_streams(now);
    return host_.stop_control_plane(now);
}

void AvbEntityKit::close_data_plane()
{
#if defined(__linux__)
    listener_vlan_.release();
#endif
    rx_on_tick_ = false;
    rx_static_groups_.clear();
    ports_.close();
    declaration_timer_token_.reset();  // the timer finishes
    if (persist_pending_) {
        persist();  // a listener goal change of this pass
    }
}

void AvbEntityKit::on_declaration_timer(int64_t const now_ns)
{
    // A listener goal change during this pass is saved now, after its ACMP
    // response.
    if (persist_pending_) {
        persist();
    }
    if (declaration_deadline_ns_ && host_.is_running() && now_ns >= *declaration_deadline_ns_) {
        update_talker_declarations(now_ns);
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
    if (!note_persistent_state_outcome(loaded)) {
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

auto AvbEntityKit::note_persistent_state_outcome(LoadedPersistentState const& loaded) -> bool
{
    auto log = host_.ctl_log();
    switch (loaded.outcome) {
        case PersistentStateOutcome::fresh:
            log.status("kit: no persistent state yet; starting from the model's defaults");
            break;
        case PersistentStateOutcome::loaded:
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
    // Milan's restore order (lib_avb 70c93a1) less the rate the kit never changes, each
    // through the handler's validation: formats (the mappings, which name their channels,
    // off meanwhile, then apply_saved_audio_maps), clock source, then the rest.
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
    mvu.set_system_unique_id(saved.system_unique_id);
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
    nanoavb::MvuCommandHandler::McrInfo const defaults{.user_mcr_prio = mvu.default_mcr_priority()};
    for (size_t index = 0; index < mvu.clock_domain_count(); ++index) {
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
    if (!persistence_active_) {
        return;
    }
    if (persistence_blocked_) {
        return;  // the file did not read at start: it is never overwritten this run
    }
    auto const state = collect_persistent_state();
    auto text = serialize_persistent_state(state);
    if (text == persisted_text_) {
        return;
    }
    if (auto const saved = save_persistent_state(config_.persistent_state_path, state); !saved) {
        host_.ctl_log().warning("kit: could not save the persistent state: error {}", saved.error().value());
        return;
    }
    persisted_text_ = std::move(text);
}

auto AvbEntityKit::make_talker_srp_info(StreamSpec const& spec) const -> nanoavb::TalkerStreamSrpInfo
{
    nanoavb::TalkerStreamSrpInfo info{.accumulated_latency = TALKER_ACCUMULATED_LATENCY_NS};
    if (auto const* stream = host_.components().acmp_talker.get_stream(spec.index); stream != nullptr) {
        (void)statusbar::tsn::load_unchecked(stream->stream_id.span(), &info.stream_id);
        info.dest_address = stream->stream_dest_mac;
        info.vlan_id = stream->stream_vlan_id;
    } else {
        info.vlan_id = config_.vlan_id;
    }
    // TSpec from the format word (kit phase 1) — no per-entity byte math to
    // keep in step with the model; the hardware shaper sizes by the same one.
    auto const tspec = talker_tspec(spec);
    info.max_interval_frames = tspec.max_interval_frames;
    info.max_frame_size = tspec.max_frame_size;
    return info;
}

void AvbEntityKit::advertise_talker_streams(TimePoint const /*time*/)
{
    // The control plane advertises: each stream declares when TalkerDeclarations
    // says so, after its MAAP address and, under Milan, interest.
    declarations_.set_active(true);
    update_talker_declarations(steady_now_ns(), true);
}

}  // namespace statusbar::avb_entity
