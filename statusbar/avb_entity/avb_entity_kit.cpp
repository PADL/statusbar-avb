// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AvbEntityKit methods — the Entity Construction Kit facade (refactor phase D;
// the wiring bodies come verbatim from AvbEntityToneGenerator/AvbEntityAudioIO,
// which each carried a copy).

#include "statusbar/avb_entity/avb_entity_kit.hpp"

#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
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

/// Whether a Milan sink's binding, when it has one, is in the stopped state (Milan v1.3 Section 5.3.8.7).
[[nodiscard]] auto binding_waits(std::optional<atdecc::MilanBinding> const& binding) noexcept -> bool
{
    return binding.has_value() && binding->streaming_wait;
}

/// An ACMP stream_id as the StreamID SRP knows the stream by.
[[nodiscard]] auto srp_stream_id(ieee::Eui64 const& acmp_stream_id) noexcept -> nanoavb::StreamId
{
    nanoavb::StreamId stream_id{};
    (void)statusbar::tsn::load_unchecked(acmp_stream_id.span(), &stream_id);
    return stream_id;
}

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
    , talker_{std::make_unique<TalkerStreams>(
          TalkerStreamsConfig{.sample_rate = sample_rate, .vlan_id = config.vlan_id, .stream_pcp = config.stream_pcp},
          media_clock,
          last_gptp_ns,
          memory_resource)}
    , active_clock_source_{initial_clock_source}
    , crf_clock_source_index_{crf_clock_source_index}
    , clock_source_kinds_{clock_source_kinds(handler.storage())}
{  // NOLINT(statusbar-block-statements) the kit's construction phases, in order
    if (!listener_specs_.empty()) {
        listener_ = std::make_unique<ListenerStreams>(config_.lock_tolerance_ns, host_.components(), last_gptp_ns, audio_sink);
        listener_->set_presentation_offset_ns(effective_presentation_offset_ns(config_));
    }

    admitted_ = admitted_talker_streams(RawTimePoint{});  // a new gate has no stamp: any instant will do
    configure_milan();
    // ATDECC descriptor wire version: "2016" emits descriptors at their 1722.1-2013/2016 lengths
    // for controllers that reject the 2021 ones; "2021" the full forms; empty, the profile's
    // (effective_atdecc_version).
    host_.components().aem_handler.set_legacy_2016(effective_atdecc_version(config_) != "2021");

    configure_profile();
    configure_talker_streams();
    configure_class_a_domain();
    wire_controller_hooks();
}

void AvbEntityKit::configure_milan()
{
    // Advertised, so reported from the start: no signal until the application says otherwise.
    for (auto const& spec : reports_signal_presence() ? std::span{talker_specs_} : std::span<StreamSpec const>{}) {
        (void)talker_->set_signal_presence(spec.index, 0);
    }
}

void AvbEntityKit::configure_talker_streams()
{
    // Base the talker stream_ids on the NIC MAC (globally unique per box), and
    // seed the static dest MACs by kind (MAAP mode reassigns after acquisition).
    ieee::Eui48 const stream_base_mac = stream_base_mac_for(config_.interface_mac, config_.interface_name, config_.entity_id);
    for (auto const& spec : talker_specs_) {
        (void)host_.components().configure_talker_stream(
            spec.index, stream_id_for(stream_base_mac, spec.index), talker_dest_mac(config_, spec.format.kind), config_.vlan_id);
        check_talker_interface(spec.index);
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
    host_.set_on_grandmaster_transition(
        [this](GrandmasterTransition const transition) { notify_talker_of_transition(transition); });
}

void AvbEntityKit::notify_talker_of_transition(GrandmasterTransition const transition) noexcept
{
    switch (transition) {  // NOLINT(statusbar-block-statements) one case per transition
        case GrandmasterTransition::changed:
            talker_->notify_grandmaster_changed(gate_.told_time());  // the reactor cycle's instant, as the loop told it
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

void AvbEntityKit::follow_grandmaster()
{
    // AVTP tu (IEEE 1722-2016 Clause 4.4.4.7, Milan v1.3 Section 4.3.5.2) from the Announce
    // handler's own observer, which an integrator's GptpAnnounceCallbacks do not replace.
    auto& announces = host_.net_handlers()->gptp_handler();
    announces.set_on_grandmaster_changed(
        [this, &announces](int64_t) { notify_talker_of_grandmaster(announces.has_grandmaster()); });
    notify_talker_of_grandmaster(announces.has_grandmaster());
}

void AvbEntityKit::notify_talker_of_grandmaster(bool const in_force) noexcept
{
    if (host_.follows_parent_grandmaster()) {
        return;  // the gPTP stack's transitions drive tu (notify_talker_of_transition)
    }
    if (in_force) {
        talker_->notify_grandmaster_changed(gate_.told_time());  // the reactor cycle's instant, as the loop told it
        return;
    }
    lose_announced_grandmaster();
}

void AvbEntityKit::lose_announced_grandmaster()
{
    if (host_.own_grandmaster_in_force()) {
        talker_->notify_own_grandmaster();  // a running window cut short
        return;
    }
    talker_->notify_grandmaster_lost();
}

void AvbEntityKit::check_talker_interface(uint16_t const index)
{
    // The entity runs on one AVB interface (0); its Milan talker does not check a probe's interface.
    auto const stream = load_descriptor<atdecc::aem::DescriptorStream>(host_, DESCRIPTOR_STREAM_OUTPUT, index);
    if (stream && stream->avb_interface_index.get() != 0) {
        host_.ctl_log().warning(
            "kit: STREAM_OUTPUT {} names AVB_INTERFACE {}; the entity has only interface 0",
            index,
            stream->avb_interface_index.get());
    }
}

void AvbEntityKit::configure_profile()
{  // NOLINT(statusbar-block-statements) one setting per difference between the profiles
    // IDENTIFY honours acquire and lock unless the configuration says otherwise.
    host_.components().aem_handler.set_identify_bypasses_exclusive_access(config_.identify_bypasses_lock);
    // Where 1722.1 and Milan differ, the profile decides: the AEM handler's answers and, for SRP, the MRP
    // timers (Table 4.3), Domain adoption (4.2.7.2.1) and instant leave (4.2.7.2.2). A Milan model also
    // answers GET_MILAN_INFO, whichever profile it is held to.
    bool const milan = milan_profile();
    host_.components().aem_handler.set_profile(
        milan ? nanoavb::AemCommandHandler::Profile::milan13 : nanoavb::AemCommandHandler::Profile::classic);
    install_milan_info();
    apply_acmp_profile(milan);
    host_.components().msrp_handler.set_adopt_domain(milan);
    host_.components().msrp_handler.set_instant_leave(milan);
    // Milan v1.3 4.3.3.1: a Talker attribute is declared only while a listener wants the stream.
    host_.components().msrp_handler.set_talker_waits_for_listener(milan);
    apply_mrp_timers(milan);
    // An acquisition shuts the other controllers out unless the configuration says it is shared.
    host_.components().aem_handler.set_acquire_mode(
        config_.acquire_shared ? nanoavb::AemCommandHandler::AcquireMode::shared
                               : nanoavb::AemCommandHandler::AcquireMode::exclusive);
}

void AvbEntityKit::apply_acmp_profile(bool const milan)
{
    // Connection management is one of two implementations, chosen here once
    // for the life of the entity: IEEE 1722.1-2021 clause 8 as written, or
    // Milan v1.3 section 5.5 (bind and probe, no talker-side connections).
    if (!milan) {
        return;
    }
    host_.components().use_milan_acmp();
    // 5.5.4.1: a Milan talker streams "as long as bandwidth is reserved for its stream": no connection
    // for the gate to wait for, and it closes with the Listener's registration (held through a LeaveAll).
    gate_.set_needs_acmp_connection(false);
    gate_.set_listener_grace(std::chrono::seconds{0});
}

void AvbEntityKit::configure_class_a_domain()
{
    // The VID itself is registered at start() (stop() withdraws it).
    host_.components().msrp_handler.set_domain(
        DomainInfo{.sr_class_id = 6, .sr_class_priority = 3, .sr_class_vid = config_.vlan_id});
    host_.components().msrp_handler.set_on_domain_adopted(
        [this](DomainInfo const& domain, TimePoint const now) { follow_domain(domain, now); });
    host_.components().msrp_handler.set_redeclare_registered_listeners(config_.redeclare_registered_listeners);
    host_.components().msrp_handler.set_suppress_leaveall(config_.suppress_leaveall);
}

void AvbEntityKit::wire_controller_hooks()
{
    wire_storage_hooks();
    wire_dynamic_mappings();
    wire_presentation_offsets();
    // A Milan sink's binding changed (saved by the host): counters, stopped states, the kit's save.
    host_.set_on_milan_bindings_changed([this]() { follow_milan_bindings(); });
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
    // A controller's STOP_STREAMING gates the talker slot and START reopens it. It shows in GET_STREAM_INFO
    // as STREAMING_WAIT (IEEE 1722.1-2021 Table 7-145), notified unsolicited to every subscribed controller
    // (Clause 7.5.2) beside the command's own echo.
    storage_handler_.set_on_streaming_changed([this](uint16_t const type, uint16_t const index, bool const streaming) -> uint8_t {
        return on_streaming_changed(type, index, streaming);
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
    // SET_CLOCK_SOURCE (IEEE 1722.1-2021 Clause 7.4.23) on any CLOCK_DOMAIN: a source the entity
    // runs, then the entity's hook; domain 0's active source is what media_rate() reads per tick.
    storage_handler_.set_on_clock_source_changed(
        [this](uint16_t const domain, uint16_t const source) -> uint8_t { return change_clock_source(domain, source); });
}

auto AvbEntityKit::change_clock_source(uint16_t const domain, uint16_t const source) -> uint8_t
{
    // IEEE 1722.1-2021 Table 7-126: "the target of the command is not supported".
    if (domain == 0 && !clock_source_runs(source)) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;
    }
    if (auto const status = ask_clock_source_hook(domain, source); status != atdecc::AEM_STATUS_SUCCESS) {
        return status;
    }
    if (domain == 0 && active_clock_source_.exchange(source, std::memory_order_acq_rel) != source) {
        restart_media_clock(domain);
    }
    if (!config_.kit_data_plane) {
        save_clock_sources(domain, source);  // else in the entity-state, with the other settings
    }
    return atdecc::AEM_STATUS_SUCCESS;
}

auto AvbEntityKit::ask_clock_source_hook(uint16_t const domain, uint16_t const source) -> uint8_t
{
    if (!on_clock_source_changed_ || applying_local_clock_source_) {
        return atdecc::AEM_STATUS_SUCCESS;
    }
    return on_clock_source_changed_(domain, source);
}

void AvbEntityKit::run_clock_source(std::string_view const symbol)
{
    auto const entry = host_.descriptor_for_symbol(symbol_code(symbol));
    if (entry.has_value() && entry->descriptor_type == atdecc::aem::DESCRIPTOR_CLOCK_SOURCE &&
        entry->descriptor_index < MAX_CLOCK_SOURCES) {
        external_clock_sources_run_.set(entry->descriptor_index);
    }
}

auto AvbEntityKit::clock_sources_file(bool const make) -> std::string
{
    if (make) {
        return state_file(std::string{}, entity_state_file::CLOCK_SOURCES, true);
    }
    if (config_.state_dir.empty()) {
        return {};
    }
    auto const dir = entity_state_dir_path(config_.state_dir, host_.components().acmp_listener.entity_id());
    return entity_state_file_path(dir, entity_state_file::CLOCK_SOURCES);
}

namespace {

[[nodiscard]] auto clock_domain_ref(uint16_t const index) noexcept -> nanoavb::DescriptorRef
{
    return {.configuration_index = 0, .descriptor_type = atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN, .descriptor_index = index};
}

}  // namespace

void AvbEntityKit::save_clock_sources(uint16_t const domain, uint16_t const source)
{
    // Milan v1.3 5.3.11.1: "The current clock source shall be saved in a
    // non-volatile memory and restored after a power cycle."
    auto const path = clock_sources_file(true);
    if (path.empty()) {
        return;
    }
    SavedClockSources sources{};
    for (uint16_t index = 0; index < MAX_SAVED_CLOCK_SOURCES; ++index) {
        auto const in_force =
            index == domain ? std::optional{source} : storage_handler_.current_clock_source(clock_domain_ref(index));
        if (in_force.has_value()) {
            sources.push_back(SavedClockSource{.clock_domain = index, .clock_source = *in_force});
        }
    }
    if (auto const saved = avb_entity::save_clock_sources(path, sources); !saved) {
        host_.ctl_log().error("state: the clock sources were not saved (error {})", saved.error().value());
    }
}

void AvbEntityKit::restore_clock_sources()
{
    auto const path = config_.kit_data_plane ? std::string{} : clock_sources_file(false);
    if (path.empty()) {
        return;  // kit_data_plane: the entity-state's sources go through SET_CLOCK_SOURCE at its load
    }
    auto const saved = load_clock_sources(path);
    if (!saved.has_value()) {
        host_.ctl_log().error(
            "state: the saved clock sources cannot be read (error {}); the clock domains keep their authored sources",
            saved.error().value());
        return;
    }
    for (auto const& one : *saved) {
        restore_clock_source(one);
    }
}

void AvbEntityKit::restore_clock_source(SavedClockSource const& saved)
{
    // Asked in this order, so that nothing is taken unless both take it: the
    // entity can run it, the model still lists it.
    bool const taken = (saved.clock_domain != 0 || clock_source_runs(saved.clock_source)) &&
        storage_handler_.set_runtime_clock_source(clock_domain_ref(saved.clock_domain), saved.clock_source);
    if (!taken) {
        host_.ctl_log().status(
            "state: the saved clock source {} of clock domain {} is not one it has now; it keeps its authored source",
            saved.clock_source,
            saved.clock_domain);
        return;
    }
    if (saved.clock_domain == 0) {
        active_clock_source_.store(saved.clock_source, std::memory_order_release);
    }
}

void AvbEntityKit::restart_media_clock(uint16_t const domain)
{
    // IEEE 1722-2016 Clause 4.4.4.3: each stream the domain's media clock times toggles mr.
    for (auto const& spec : talker_specs_) {
        if (spec.clock_domain_index == domain) {
            (void)talker_->toggle_media_clock_restart(spec.index);  // a stream not open has no AVTPDU to tell
        }
    }
}

void AvbEntityKit::restart_retimed_outputs(RetimedFormats const& retimed)
{
    for (auto const& r : retimed) {
        if (r.type == DESCRIPTOR_STREAM_OUTPUT) {
            (void)talker_->toggle_media_clock_restart(r.index);  // its media clock restarts (IEEE 1722-2016 Clause 4.4.4.3)
        }
    }
}

void AvbEntityKit::wire_dynamic_mappings()
{
    // Dynamic input mappings (Milan v1.2 Section 5.3.10): each port starts at its default
    // (the identity the data plane routes); a controller's change goes
    // through the entity's hook.
    install_dynamic_maps();
    reset_input_mappings();
    refresh_output_mappings();  // dynamic output ports carry the kit's identity while at the unit's rate
    host_.components().aem_handler.set_on_audio_mappings_changed(
        [this](uint16_t const port_type, uint16_t const port_index, std::span<nanoavb::DynamicAudioMapping const> const mappings)
            -> uint8_t { return change_input_mappings(port_type, port_index, mappings); });
}

void AvbEntityKit::install_dynamic_maps()
{
    auto& aem = host_.components().aem_handler;
    aem.set_dynamic_audio_maps(&audio_maps_);
    aem.set_output_mappings_follow_streams(config_.kit_data_plane);  // the kit's output identity is its own to drop
    dynamic_input_ports_ = dynamic_ports(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT);
    dynamic_output_ports_ = dynamic_ports(atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT);
}

auto AvbEntityKit::dynamic_ports(uint16_t const port_type) const -> std::vector<uint16_t>
{
    std::vector<uint16_t> ports;
    auto const& aem = host_.components().aem_handler;
    for (uint16_t port = 0; port < UINT16_MAX && host_.get_descriptor(port_type, port).has_value(); ++port) {
        if (aem.has_dynamic_map(port_type, port)) {
            ports.push_back(port);
        }
    }
    return ports;
}

auto AvbEntityKit::change_input_mappings(
    uint16_t const port_type, uint16_t const port_index, std::span<nanoavb::DynamicAudioMapping const> const mappings) -> uint8_t
{
    if (port_type != atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT || !on_input_mappings_changed_) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;
    }
    auto const status = on_input_mappings_changed_(port_index, mappings);
    if (status == atdecc::AEM_STATUS_SUCCESS) {
        host_.ctl_log().status("aem: STREAM_PORT_INPUT {}: {} dynamic mapping(s)", port_index, mappings.size());
    }
    return status;
}

auto AvbEntityKit::input_mappings(uint16_t const port_index) const -> nanoavb::DynamicAudioMaps::Mappings
{
    return held_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port_index);
}

auto AvbEntityKit::output_mappings(uint16_t const port_index) const -> nanoavb::DynamicAudioMaps::Mappings
{
    return held_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT, port_index);
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
    // Section 5.3.3.7) or outputs opted out; none unless the kit's engines route the port.
    bool const at_rate = config_.kit_data_plane && spec != specs.end() &&
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
    auto mappings = default_input_mappings(port_index);
    if (put_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port_index, mappings, false) != atdecc::AEM_STATUS_SUCCESS) {
        mappings.clear();
        (void)put_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port_index, {}, false);
    }
    host_.ctl_log().warning(
        "kit: STREAM_PORT_INPUT {}: the entity cannot route its mappings: {} default mapping(s) instead",
        port_index,
        mappings.size());
    return mappings;
}

auto AvbEntityKit::put_mappings(
    uint16_t const port_type,
    uint16_t const port_index,
    std::span<nanoavb::DynamicAudioMapping const> const mappings,
    bool const apply) -> uint8_t
{
    auto const held = held_mappings(port_type, port_index);
    (void)audio_maps_.replace(port_type, port_index, {});  // the handler seeds an empty port
    auto const status = seed_mappings(port_type, port_index, mappings, apply);
    if (status != atdecc::AEM_STATUS_SUCCESS) {
        (void)audio_maps_.replace(port_type, port_index, held);  // as it was
    }
    return status;
}

auto AvbEntityKit::seed_mappings(
    uint16_t const port_type,
    uint16_t const port_index,
    std::span<nanoavb::DynamicAudioMapping const> const mappings,
    bool const apply) -> uint8_t
{
    auto const status = host_.components().aem_handler.seed_audio_mappings(port_type, port_index, mappings);
    if (status != atdecc::AEM_STATUS_SUCCESS || !apply || !on_input_mappings_changed_) {
        return status;
    }
    return on_input_mappings_changed_(port_index, mappings);
}

auto AvbEntityKit::held_mappings(uint16_t const port_type, uint16_t const port_index) const -> nanoavb::DynamicAudioMaps::Mappings
{
    auto const held = audio_maps_.port(port_type, port_index);
    nanoavb::DynamicAudioMaps::Mappings out{};
    out.assign(held.begin(), held.end());
    return out;
}

void AvbEntityKit::reset_input_mappings()
{
    for (auto const port : dynamic_input_ports_) {
        auto const mappings = default_input_mappings(port);
        if (auto const status = put_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port, mappings, /*apply=*/false);
            status != atdecc::AEM_STATUS_SUCCESS) {
            host_.ctl_log().warning("kit: STREAM_PORT_INPUT {}: its default mappings were refused (status {})", port, status);
            (void)put_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port, {}, false);
        }
    }
}

void AvbEntityKit::refresh_output_mappings()
{
    auto& aem = host_.components().aem_handler;
    for (auto const port : dynamic_output_ports_) {
        if (auto const status =
                aem.follow_local_audio_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT, port, default_output_mappings(port));
            status != atdecc::AEM_STATUS_SUCCESS) {
            host_.ctl_log().warning(
                "kit: STREAM_PORT_OUTPUT {}: its identity mappings were refused (status {}): unmapped", port, status);
            (void)aem.follow_local_audio_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT, port, {});
        }
    }
}

void AvbEntityKit::apply_saved_audio_maps(PersistentState const& saved)
{
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
            auto const status = put_mappings(DESCRIPTOR_STREAM_PORT_INPUT, port, mappings, apply);
            if (status == atdecc::AEM_STATUS_SUCCESS) {
                continue;
            }
            host_.ctl_log().warning(
                "kit: saved audio mappings of STREAM_PORT_INPUT {} refused (status {}): the default", port, status);
        }
        if (auto const status = put_mappings(DESCRIPTOR_STREAM_PORT_INPUT, port, default_input_mappings(port), apply);
            status != atdecc::AEM_STATUS_SUCCESS) {
            // Unmapped (silent), and the data plane told so: never a map
            // the entity reports but does not route.
            host_.ctl_log().warning(
                "kit: STREAM_PORT_INPUT {}: its default mappings were refused (status {}): unmapped", port, status);
            (void)put_mappings(DESCRIPTOR_STREAM_PORT_INPUT, port, {}, apply);
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
        audio_maps_.for_each([this, &named, index](
                                 uint16_t const type, uint16_t const port_index, nanoavb::DynamicAudioMapping const& m) {
            named = named ||
                (type == atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT && m.stream_index == index && !port_converts(type, port_index));
        });
    }
    return named;
}

auto AvbEntityKit::identity_installed(uint16_t const index) const -> bool
{
    bool named = false;
    audio_maps_.for_each(
        [this, &named, index](uint16_t const type, uint16_t const port_index, nanoavb::DynamicAudioMapping const& m) {
            named = named ||
                (type == atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT && m.stream_index == index && !port_converts(type, port_index));
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
    if (components.acmp_is_milan()) {
        return components.milan_acmp_listener->is_bound(idx);
    }
    return components.acmp_listener.is_connected(idx);
}

auto AvbEntityKit::output_streaming(uint16_t const idx, RawTimePoint const now) const noexcept -> bool
{
    bool const transmits = maap_.ready() && !talker_->stream_stopped(idx) && gate_.should_transmit(idx, now);
    return transmits || (talker_declared(idx) && gate_.listener_admitted(idx, now));
}

auto AvbEntityKit::talker_declared(uint16_t const idx) const noexcept -> bool
{
    auto const stream = host_.components().talker_stream(idx);
    if (!stream.has_value()) {
        return false;
    }
    auto const* const declared = host_.components().msrp_handler.get_talker_stream(srp_stream_id(stream->stream_id));
    using State = nanoavb::TalkerReservationState;
    return declared != nullptr && declared->state != State::Idle && declared->state != State::Waiting;
}

auto AvbEntityKit::reshape_stream(bool const output, StreamSpec const& spec) -> Status
{
    if (output) {
        return talker_->reformat_stream(spec);
    }
    return listener_ != nullptr ? listener_->reformat_stream(spec) : Status{};
}

auto AvbEntityKit::engines_follow_format(uint16_t const type) const noexcept -> bool
{
    // Without kit_data_plane, or for an input the entity has no hook for, the listener alone follows.
    return config_.kit_data_plane && (type == DESCRIPTOR_STREAM_OUTPUT || static_cast<bool>(on_stream_format_changed_));
}

auto AvbEntityKit::change_stream_format(uint16_t const type, uint16_t const index, uint64_t const format) -> uint8_t
{
    if (!engines_follow_format(type)) {
        return on_stream_format_changed(type, index, format);
    }
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
    if (output ? output_streaming(spec.index, gate_.told_time()) : input_running(spec.index)) {
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
    audio_maps_.for_each([&](uint16_t const port_type, uint16_t /*port_index*/, nanoavb::DynamicAudioMapping const& m) {
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
        if (talker_->slot_for(updated.index) != nullptr) {
            talker_->notify_frames_changed(updated.index);  // the entity's hook accepted it: the frames are the new format's
        }
    }
    host_.ctl_log().status(
        "aem: {} stream {} format {:016x} -> {:016x}",
        output ? logging::lit("talker") : logging::lit("listener"),
        updated.index,
        before,
        updated.format_word);
    if (output) {
        readvertise_pending_.set(updated.index);  // its TSpec, handed to MSRP at the next pass (this hook is told no time)
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
    // Under the guard where it is held. The port counts memberships: one per input, a changed group swapped.
    StreamPort::Guard const guard{stream_port_};
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

auto AvbEntityKit::on_stream_format_changed(uint16_t const type, uint16_t const index, uint64_t const format) -> uint8_t
{
    // The format in force named again changes nothing and is refused by nothing.
    if (current_stream_format(type, index) == format) {
        return atdecc::AEM_STATUS_SUCCESS;
    }
    // A stream output's serializer is made once, for the format its model authors (Milan v1.3 Section 5.3.3.4).
    return type == DESCRIPTOR_STREAM_INPUT ? change_input_format(index, format) : atdecc::AEM_STATUS_NOT_SUPPORTED;
}

auto AvbEntityKit::change_input_format(uint16_t const index, uint64_t const format) -> uint8_t
{
    // Milan v1.3 Section 5.4.2.7, IEEE 1722.1-2021 Clause 7.4.9.2: not while the input is bound or streaming.
    if (input_running(index)) {
        return atdecc::AEM_STATUS_STREAM_IS_RUNNING;
    }
    if (listener_ == nullptr || !listener_->set_stream_format(index, format)) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;
    }
    host_.ctl_log().status(
        "aem: stream input {} now receives {} channels (format {:016x})", index, decode_stream_format(format).channels, format);
    save_input_formats(index, format);
    return atdecc::AEM_STATUS_SUCCESS;
}

auto AvbEntityKit::current_stream_format(uint16_t const type, uint16_t const index) const -> uint64_t
{
    return storage_handler_.current_stream_format({.configuration_index = 0, .descriptor_type = type, .descriptor_index = index})
        .value_or(ieee::Eui64{})
        .to_uint64();
}

auto AvbEntityKit::stream_formats_file(bool const make) -> std::string
{
    if (make) {
        return state_file(std::string{}, entity_state_file::STREAM_FORMATS, true);
    }
    if (config_.state_dir.empty()) {
        return {};
    }
    auto const dir = entity_state_dir_path(config_.state_dir, host_.components().acmp_listener.entity_id());
    return entity_state_file_path(dir, entity_state_file::STREAM_FORMATS);
}

void AvbEntityKit::save_input_formats(uint16_t const index, uint64_t const format)
{
    // Milan v1.3 Section 5.3.8.1: "The current format shall be saved in a non-volatile memory".
    auto const path = stream_formats_file(true);
    if (path.empty()) {
        return;
    }
    SavedStreamFormats formats{};
    for (auto const& spec : listener_specs_) {
        uint64_t const word = spec.index == index ? format : current_stream_format(DESCRIPTOR_STREAM_INPUT, spec.index);
        (void)formats.try_push_back(
            SavedStreamFormat{.descriptor_type = DESCRIPTOR_STREAM_INPUT, .descriptor_index = spec.index, .format = word});
    }
    if (auto const saved = save_stream_formats(path, formats); !saved) {
        host_.ctl_log().error("state: the stream formats were not saved (error {})", saved.error().value());
    }
}

void AvbEntityKit::restore_input_formats()
{
    auto const path = stream_formats_file(false);
    if (listener_ == nullptr || path.empty()) {
        return;
    }
    auto const saved = load_stream_formats(path);
    if (!saved.has_value()) {
        host_.ctl_log().error(
            "state: the saved stream formats cannot be read (error {}); the inputs keep their authored formats",
            saved.error().value());
        return;
    }
    for (auto const& one : *saved) {
        restore_input_format(one);
    }
}

void AvbEntityKit::restore_input_format(SavedStreamFormat const& saved)
{
    nanoavb::DescriptorRef const ref{
        .configuration_index = 0, .descriptor_type = saved.descriptor_type, .descriptor_index = saved.descriptor_index};
    // In this order, so that nothing changes unless both accept it: the listener can, the model still
    // describes it, the listener does.
    bool const kept = saved.descriptor_type == DESCRIPTOR_STREAM_INPUT &&
        listener_->can_take_stream_format(saved.descriptor_index, saved.format) &&
        storage_handler_.set_runtime_stream_format(ref, ieee::Eui64{}.from_uint64(saved.format)) &&
        listener_->set_stream_format(saved.descriptor_index, saved.format);
    if (!kept) {
        host_.ctl_log().status(
            "state: the saved format {:016x} of stream {} is not one it has now; it keeps its authored format",
            saved.format,
            saved.descriptor_index);
    }
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
            auto const status = config_.kit_data_plane ? add_own_retimed(output, spec, from, rate, out)
                                                       : add_retimed(output, spec.index, from, rate, out);
            if (status != atdecc::AEM_STATUS_SUCCESS) {
                return std::unexpected(status);
            }
        }
    }
    // IEEE 802.1Q-2022 Clause 34.3.1, checked before the kit's engines move (as each SET_STREAM_FORMAT would be).
    bool const fits = !config_.kit_data_plane || talkers_fit(out);
    return fits ? std::expected<RetimedFormats, uint8_t>{out} : std::unexpected(atdecc::AEM_STATUS_NO_RESOURCES);
}

auto AvbEntityKit::add_retimed(
    bool const output, uint16_t const index, uint32_t const from, uint32_t const rate, RetimedFormats& out) const -> uint8_t
{
    auto const word = retimed_from(output, index, from);
    if (!word) {
        return atdecc::AEM_STATUS_SUCCESS;  // a CRF's format stays, as does one at another rate or in another domain
    }
    auto const to = retimed_to(output, index, *word, rate);
    if (!to) {
        return to.error();
    }
    (void)out.try_push_back(
        {.type = output ? DESCRIPTOR_STREAM_OUTPUT : DESCRIPTOR_STREAM_INPUT, .index = index, .from = *word, .to = *to});
    return atdecc::AEM_STATUS_SUCCESS;
}

auto AvbEntityKit::retimed_from(bool const output, uint16_t const index, uint32_t const from) const -> std::optional<uint64_t>
{
    auto const in_force = storage_handler_.current_stream_format(
        {.configuration_index = 0,
         .descriptor_type = output ? DESCRIPTOR_STREAM_OUTPUT : DESCRIPTOR_STREAM_INPUT,
         .descriptor_index = index});
    uint64_t const word = in_force.value_or(ieee::Eui64{}).to_uint64();
    auto const format = decode_stream_format(word);
    bool const moves = is_audio_kind(format.kind) && format.sample_rate_hz == from && in_unit_zero(output, index);
    return moves ? std::optional<uint64_t>{word} : std::nullopt;
}

auto AvbEntityKit::retimed_to(bool const output, uint16_t const index, uint64_t const word, uint32_t const rate) const
    -> std::expected<uint64_t, uint8_t>
{
    auto const to = aaf_format_at_rate(word, rate);
    nanoavb::DescriptorRef const ref{
        .configuration_index = 0,
        .descriptor_type = output ? DESCRIPTOR_STREAM_OUTPUT : DESCRIPTOR_STREAM_INPUT,
        .descriptor_index = index};
    if (!to || !storage_handler_.stream_format_listed(ref, ieee::Eui64{}.from_uint64(*to))) {
        return std::unexpected(atdecc::AEM_STATUS_NOT_SUPPORTED);  // AM824, or a stream that lists no such format
    }
    // Milan v1.3 Section 5.4.2.7: refused as the input's own SET_STREAM_FORMAT would be; an output is the hook's.
    if (!output && input_running(index)) {
        return std::unexpected(atdecc::AEM_STATUS_STREAM_IS_RUNNING);
    }
    return *to;
}

auto AvbEntityKit::add_own_retimed(
    bool const output, StreamSpec const& spec, uint32_t const from, uint32_t const rate, RetimedFormats& out) const -> uint8_t
{
    if (!is_audio_kind(spec.format.kind) || spec.format.sample_rate_hz != from || !in_unit_zero(output, spec.index)) {
        return atdecc::AEM_STATUS_SUCCESS;  // a CRF's format stays, as does one at another rate or of another unit
    }
    // Milan v1.3 Section 5.4.2.7: a stream input's format does not change while it runs.
    if (!output && input_running(spec.index)) {
        return atdecc::AEM_STATUS_STREAM_IS_RUNNING;
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
    if (output ? output_streaming(spec.index, gate_.told_time()) : input_running(spec.index)) {
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
    // Without kit_data_plane the entity's own data plane moved: the kit's engines keep their rate.
    return config_.kit_data_plane ? retime_engines(rate, retimed) : retime_descriptors(rate, retimed);
}

auto AvbEntityKit::retime_engines(uint32_t const rate, RetimedFormats const& retimed) -> uint8_t
{
    uint32_t const before = std::exchange(sample_rate_, rate);
    auto const status = retime_all(before, retimed);
    if (status == atdecc::AEM_STATUS_SUCCESS) {
        restart_retimed_outputs(retimed);
    }
    refresh_output_mappings();
    notify_after_response();
    return status;
}

auto AvbEntityKit::retime_descriptors(uint32_t const rate, RetimedFormats const& retimed) -> uint8_t
{
    // The descriptors, reactor-side state, follow the data plane; the kit's specs and engines are not
    // touched (the media thread reads them unguarded).
    for (auto const& r : retimed) {
        (void)storage_handler_.set_runtime_stream_format(
            {.configuration_index = 0, .descriptor_type = r.type, .descriptor_index = r.index}, ieee::Eui64{}.from_uint64(r.to));
        (void)format_notifications_.try_push_back({r.type, r.index});
    }
    restart_retimed_outputs(retimed);
    notify_after_response();
    host_.ctl_log().status("aem: AUDIO_UNIT 0 at {} Hz, {} streams re-timed", rate, retimed.size());
    return atdecc::AEM_STATUS_SUCCESS;
}

void AvbEntityKit::notify_after_response()
{
    if (!format_notifications_.empty()) {
        host_.components().aem_handler.call_after_response([this] { flush_format_notifications(); });
    }
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
    return host_.components().aem_handler.local_change_refusal();
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
    auto const status = aem.follow_local_descriptor_value(atdecc::AEM_COMMAND_SET_CLOCK_SOURCE, body);  // never refused
    applying_local_clock_source_ = false;
    return status;
}

auto AvbEntityKit::request_audio_mappings(
    uint16_t const port_index,
    std::span<nanoavb::DynamicAudioMapping const> const add,
    std::span<nanoavb::DynamicAudioMapping const> const remove) -> uint8_t
{
    auto const status = host_.components().aem_handler.apply_local_audio_mappings(
        atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port_index, add, remove);
    if (status == atdecc::AEM_STATUS_SUCCESS) {
        persist();  // a local change runs no settings hook
    }
    return status;
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

void AvbEntityKit::follow_domain(DomainInfo const& domain, TimePoint const now)
{  // NOLINT(statusbar-block-statements) MVRP, the streams, the VLAN filter and the controllers follow the Domain
    // Milan v1.3 4.2.7.2.1: the Class A Domain registered from the wire has
    // replaced ours, or a startup or Link Up has brought the configured one
    // back, and the SRP handler has re-declared it. MVRP follows the VLAN,
    // the streams move (move_talker_streams_to), and the registered
    // controllers hear the new mapping (GET_AVB_INFO, Table 5.20).
    uint16_t const old_vlan = current_vlan_id();
    if (domain.sr_class_vid != old_vlan) {
        (void)host_.components().mvrp_handler.withdraw_vlan(old_vlan, now);
        (void)host_.components().mvrp_handler.register_vlan(domain.sr_class_vid, now);
        // Keep the VIDs settled listener streams still use.
        for (auto const& [vid, count] : sink_vlans_) {
            (void)host_.components().mvrp_handler.register_vlan(vid, now);
        }
    }
    move_talker_streams_to(domain, now);
#if defined(__linux__)
    // Past the NIC's receive VLAN filter; the new VID held before the old is dropped, so a
    // link an input also holds stays as it is.
    vlan_filters_.hold(domain.sr_class_vid);
    vlan_filters_.drop(std::exchange(class_a_filtered_, domain.sr_class_vid));
#endif
    host_.refresh_avb_info();  // the msrp_mappings moved: notified once, as the host's last
    host_.ctl_log().status(
        "msrp: the Class A domain in force: priority {} vlan {} ({})",
        domain.sr_class_priority,
        domain.sr_class_vid,
        active_vlan_id_ == 0 && domain.sr_class_priority == 3 ? logging::lit("configured") : logging::lit("adopted from the wire"));
}

void AvbEntityKit::move_talker_streams_to(DomainInfo const& domain, TimePoint const now)
{
    // The stream frames take the new VLAN and priority at once; ACMP's
    // picture of each stream (what a TX_RESPONSE and GET_STREAM_INFO carry)
    // moves with them; and each Talker Advertise is declared again with the
    // new VLAN and priority, the participant leaving its old value first
    // (IEEE 802.1Q 10.7). A host not yet advertising declares them when it
    // does, from the same picture (make_talker_srp_info).
    active_vlan_id_ = domain.sr_class_vid == config_.vlan_id ? 0 : domain.sr_class_vid;
    talker_->set_vlan(domain.sr_class_vid, domain.sr_class_priority);
    for (auto const& spec : talker_specs_) {
        if (auto const stream = host_.components().talker_stream(spec.index); stream.has_value()) {
            (void)host_.components().configure_talker_stream(
                spec.index, stream->stream_id, stream->stream_dest_mac, domain.sr_class_vid);
        }
    }
    if (host_.is_ready()) {
        advertise_talker_streams(now);
    }
}

auto AvbEntityKit::current_vlan_id() const noexcept -> uint16_t
{
    return active_vlan_id_ != 0 ? active_vlan_id_ : config_.vlan_id;
}

auto AvbEntityKit::admitted_talker_streams() const noexcept -> AdmittedStreams
{
    return admitted_talker_streams(gate_.told_time());
}

auto AvbEntityKit::admitted_talker_streams(RawTimePoint const now) const noexcept -> AdmittedStreams
{
    AdmittedStreams admitted{};
    for (auto const& spec : talker_specs_) {
        if (gate_.admitted(spec.index, now)) {
            (void)admitted.try_push_back(spec.index);
        }
    }
    return admitted;
}

void AvbEntityKit::notify_admission(RawTimePoint const now)
{
    arm_admission_deadline(now);
    auto const admitted = admitted_talker_streams(now);
    if (!std::ranges::equal(admitted, admitted_)) {
        admitted_ = admitted;
        if (on_admission_changed_) {
            on_admission_changed_(std::span<uint16_t const>{admitted_.data(), admitted_.size()});
        }
    }
    notify_talker_gates();
}

auto AvbEntityKit::evaluate_talker_gate(uint16_t const idx) const noexcept -> bool
{
    bool const admitted = std::ranges::find(admitted_, idx) != admitted_.end();
    return admitted && maap_.ready() && talker_->slot_for(idx) != nullptr && !talker_->stream_stopped(idx);
}

void AvbEntityKit::notify_talker_gates() noexcept
{
    for (auto const& spec : talker_specs_) {
        bool const open = evaluate_talker_gate(spec.index);
        if (spec.index >= talker_gates_.size() || std::exchange(talker_gates_[spec.index], open) == open) {
            continue;
        }
        if (on_talker_gate_changed_) {
            on_talker_gate_changed_(spec.index, open);
        }
    }
}

void AvbEntityKit::arm_admission_deadline(RawTimePoint const now) noexcept
{
    admission_deadline_.reset();
    for (auto const& spec : talker_specs_) {
        if (auto const ends = gate_.grace_ends(spec.index); ends && now < *ends && gate_.admitted(spec.index, now)) {
            admission_deadline_ = std::min(admission_deadline_.value_or(*ends), *ends);
        }
    }
}

void AvbEntityKit::on_admission_timer(RawTimePoint const now)
{
    if (admission_deadline_ && now >= *admission_deadline_) {
        notify_admission(now);
    }
}

void AvbEntityKit::tell_cycle_time(int64_t const now_ns, RawTimePoint const raw_now) noexcept
{
    cycle_ns_ = now_ns;
    gate_.tell_time(raw_now);
}

auto AvbEntityKit::raw_at(int64_t const now_ns) const noexcept -> RawTimePoint
{
    auto const told = gate_.told_time();
    if (told == RawTimePoint{}) {
        return told;
    }
    using RawClock = RawTimePoint::clock;
    return RawClock::from_ns(RawClock::to_ns(told) + (now_ns - cycle_ns_));
}

auto AvbEntityKit::poll_timeout_ms(int const max_ms, int64_t const now_ns, RawTimePoint const raw_now) const noexcept -> int
{
    int const control_ms = avtp::MaapHandler::poll_timeout_ms(next_control_deadline_ns(), now_ns, max_ms);
    if (!admission_deadline_) {
        return control_ms;
    }
    using RawClock = RawTimePoint::clock;
    return avtp::MaapHandler::poll_timeout_ms(RawClock::to_ns(*admission_deadline_), RawClock::to_ns(raw_now), control_ms);
}

auto AvbEntityKit::poll_timeout_ms(int const max_ms, int64_t const now_ns) const noexcept -> int
{
    return poll_timeout_ms(max_ms, now_ns, raw_at(now_ns));
}

void AvbEntityKit::end_cycle(int64_t const now_ns)
{
    poll_counters(now_ns);
    run_held_work(now_ns);
    on_admission_timer(raw_at(now_ns));
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

auto AvbEntityKit::talker_channels(uint16_t const idx) const noexcept -> uint16_t
{
    auto const* const spec = std::ranges::find(talker_specs_, idx, &StreamSpec::index);
    return spec == talker_specs_.end() ? uint16_t{0} : spec->format.channels;
}

void AvbEntityKit::fold_stream_counters()
{
    fold_output_counters();
    fold_input_counters();
    if (++counters_folds_ < COUNTERS_NOTIFY_FOLDS) {
        return;
    }
    counters_folds_ = 0;
    notify_changed_counters();
}

void AvbEntityKit::fold_output_counters()
{
    for (auto const& spec : talker_specs_) {
        if (spec.index >= output_counters_.size()) {
            continue;
        }
        bool const presence_changed = fold_output_presence(spec);
        if (output_counters_[spec.index].fold(talker_stream_totals(*talker_, spec.index)) || presence_changed) {
            output_counters_changed_[spec.index] = true;
        }
    }
}

void AvbEntityKit::fold_input_counters()
{
    if (listener_ == nullptr) {
        return;
    }
    for (auto const& spec : listener_specs_) {
        if (listener_->fold_input_counters(spec.index) && spec.index < input_counters_changed_.size()) {
            input_counters_changed_[spec.index] = true;
        }
    }
}

auto AvbEntityKit::take_counters_change(std::array<bool, MAX_ENTITY_STREAMS>& changed, uint16_t const index) const noexcept -> bool
{
    return index < changed.size() && std::exchange(changed[index], false) && milan_profile();
}

void AvbEntityKit::notify_changed_counters()
{
    // Milan v1.3 Table 5.20: a notification when a counter changed, one a second per descriptor
    // at most. IEEE 1722.1 asks for none: there the changes are dropped untold.
    auto& aem = host_.components().aem_handler;
    for (auto const& spec : talker_specs_) {
        if (take_counters_change(output_counters_changed_, spec.index)) {
            (void)aem.notify_counters_changed(DESCRIPTOR_STREAM_OUTPUT, spec.index);
        }
    }
    for (auto const& spec : listener_specs_) {
        if (take_counters_change(input_counters_changed_, spec.index)) {
            (void)aem.notify_counters_changed(DESCRIPTOR_STREAM_INPUT, spec.index);
        }
    }
}

auto AvbEntityKit::fold_output_presence(StreamSpec const& spec) noexcept -> bool
{
    // For change detection alone: GET_COUNTERS reads the presence when it answers.
    auto const reported = talker_->signal_presence(spec.index).transform([&spec](uint64_t const channels) {
        return reported_signal_presence(channels, spec.format.channels);
    });
    return std::exchange(output_presence_[spec.index], reported) != reported;
}

void AvbEntityKit::notify_listener_ready(nanoavb::StreamId const& stream_id, bool const ready, RawTimePoint const now)
{
    gate_.note_listener_ready(stream_id, ready, now);
    // GET_STREAM_INFO reports the registered Listener (REGISTERING / REGISTERING_FAILED).
    if (auto const idx = talker_index_of(stream_id); idx && refresh_talker_listener(*idx)) {
        notify_talker_stream_info(*idx);
    }
    notify_admission(now);
}

void AvbEntityKit::publish_talker_connections(uint16_t const idx)
{
    gate_.note_acmp_connections(idx, static_cast<uint32_t>(host_.components().acmp_talker.connection_count(idx)));
    notify_admission(gate_.told_time());
}

auto AvbEntityKit::is_talker_index(uint16_t const idx) const noexcept -> bool
{
    return std::ranges::any_of(talker_specs_, [idx](StreamSpec const& spec) { return spec.index == idx; });
}

auto AvbEntityKit::talker_index_of(nanoavb::StreamId const& stream_id) const noexcept -> std::optional<uint16_t>
{
    uint64_t const sid = stream_id.to_uint64();
    for (auto const& spec : talker_specs_) {
        if (auto const s = host_.components().talker_stream(spec.index); s.has_value() && s->stream_id.to_uint64() == sid) {
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
    auto const s = idx < talker_listener_.size() ? host_.components().talker_stream(idx) : std::nullopt;
    if (!s.has_value()) {
        return std::nullopt;
    }
    return host_.components().msrp_handler.registered_listener(srp_stream_id(s->stream_id));
}

void AvbEntityKit::notify_talker_stream_info(uint16_t const idx)
{
    (void)host_.components().aem_handler.notify_stream_info_changed(DESCRIPTOR_STREAM_OUTPUT, idx);
}

void AvbEntityKit::notify_talker_probe(uint16_t const idx, TimePoint const now)
{
    // Milan v1.3 4.3.3.1: a PROBE_TX_COMMAND for one of the stream outputs. The SRP handler
    // declares the stream's Talker attribute for the next 15 seconds; a handler that does not
    // wait for a listener ignores it.
    auto const stream = is_talker_index(idx) ? host_.components().talker_stream(idx) : std::nullopt;
    if (!stream.has_value()) {
        return;
    }
    host_.ctl_log().status(
        "acmp: talker stream {} ({}) probed (PROBE_TX)", idx, logging::static_str(stream_kind_name(talker_kind_of(idx))));
    refresh_waiting_talker(idx, now);
    host_.components().msrp_handler.note_talker_probe(srp_stream_id(stream->stream_id), now);
    notify_talker_stream_info(idx);
}

void AvbEntityKit::refresh_waiting_talker(uint16_t const idx, TimePoint const now)
{
    // IEEE 802.1Q-2022 Clause 35.2.2.8.6: a Talker about to be declared declares the port's latency as it is now.
    auto const* const spec = std::ranges::find(talker_specs_, idx, &StreamSpec::index);
    if (host_.is_ready() && spec != talker_specs_.end() && !talker_declared(idx)) {
        readvertise_talker(*spec, now);
    }
}

void AvbEntityKit::notify_talker_addresses(bool const valid)
{
    // Milan v1.3 4.3.3.1 condition 1: the SRP handler declares nothing while the addresses are
    // not valid, and withdraws what it had at its next tick. The streams' addresses changed.
    host_.components().msrp_handler.set_talker_addresses_valid(valid);
    for (auto const& spec : talker_specs_) {
        notify_talker_stream_info(spec.index);
    }
}

void AvbEntityKit::readvertise_talker(StreamSpec const& spec, TimePoint const now)
{
    if (auto result = host_.components().msrp_handler.talker_advertise(make_talker_srp_info(spec), now); !result) {
        host_.ctl_log().warning("msrp: talker_advertise (stream {}) failed: errno {}", spec.index, result.error().value());
    }
}

void AvbEntityKit::withdraw_talker_streams(TimePoint const time)
{
    for (auto const& spec : talker_specs_) {
        (void)host_.components().msrp_handler.talker_withdraw(make_talker_srp_info(spec).stream_id, time);
    }
    notify_all_talkers_withdrawn();
    for (auto const& spec : talker_specs_) {
        notify_talker_stream_info(spec.index);
    }
}

void AvbEntityKit::notify_all_talkers_withdrawn()
{
    // No Talker Advertise, no streaming (Milan v1.3 5.3.7.3): close every
    // stream now, without the grace window.
    gate_.notify_talker_withdrawn();
    notify_admission(gate_.told_time());
}

void AvbEntityKit::wire_stream_callbacks()
{  // NOLINT(statusbar-block-statements) the stream callback wiring, one install per hook; pre-dates the gate
    // MSRP reservations for the talker streams. The host declares the SR class
    // domain before calling these, then advertises/withdraws on the MSRP cycle.
    host_.set_advertise_streams([this](TimePoint time) { advertise_talker_streams(time); });
    host_.set_withdraw_streams([this](TimePoint time) { withdraw_talker_streams(time); });
    // The transmit gate tracks the MSRP listener declarations against our talker
    // streams; that alone admits a stream (docs/AVB_ENTITY_KIT.md).
    // Stamped with the cycle's told instant: the MSRP handler's time is the reactor's, not the gate's.
    host_.set_on_listener_ready([this](nanoavb::StreamId const& stream_id, bool const ready, TimePoint /*time*/) {
        notify_listener_ready(stream_id, ready, gate_.told_time());
    });

    wire_acmp_callbacks();
    wire_aecp_queries();
}

void AvbEntityKit::wire_aecp_queries()
{
    // GET_COUNTERS: the streams' (ENTITY, AVB_INTERFACE, CLOCK_DOMAIN from the host); GET_STREAM_INFO:
    // a talker stream's identity (a Milan listener checks it), a listener sink's binding.
    host_.components().aem_handler.set_get_counters(
        [this](uint16_t descriptor_type, uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& counters) -> bool {
            if (descriptor_type == DESCRIPTOR_STREAM_OUTPUT) {
                // Milan v1.3 Table 5.14 by observation interval; IEEE 1722.1's running counts otherwise.
                return milan_profile() && is_talker_index(descriptor_index)
                    ? fill_interval_talker_stream_counters(
                          *talker_,
                          descriptor_index,
                          output_counters_[descriptor_index],
                          talker_channels(descriptor_index),
                          valid,
                          counters)
                    : fill_talker_stream_counters(*talker_, descriptor_index, valid, counters);
            }
            if (descriptor_type == DESCRIPTOR_STREAM_INPUT && listener_ != nullptr) {
                // Milan v1.3 Table 5.13 by observation interval; IEEE 1722.1's running counts otherwise.
                return milan_profile() ? listener_->fill_interval_stream_input_counters(descriptor_index, valid, counters)
                                       : listener_->fill_stream_input_counters(descriptor_index, valid, counters);
            }
            return host_.fill_counters(descriptor_type, descriptor_index, valid, counters);
        });
    host_.components().aem_handler.set_get_stream_info(
        [this](uint16_t descriptor_type, uint16_t descriptor_index, atdecc::aem::AemStreamInfoPayload& out) -> bool {
            bool const ieee_2021 = host_.components().aem_handler.stream_info_length() == atdecc::aem::AEM_STREAM_INFO_2021_LENGTH;
            bool filled = false;
            if (descriptor_type == DESCRIPTOR_STREAM_OUTPUT) {
                if (!is_talker_index(descriptor_index)) {
                    return false;
                }
                TalkerStreamStatus const status{
                    .milan = milan_profile(),
                    .ieee_2021 = ieee_2021,
                    .declaring = talker_declared(descriptor_index),
                    .listener = talker_listener_[descriptor_index],
                    .presentation_offset_ns = presentation_offset_ns(descriptor_index),
                    .accumulated_latency_ns = talker_accumulated_latency_ns(descriptor_index),
                    .stopped = talker_->stream_stopped(descriptor_index)};
                filled = fill_talker_stream_info(host_, descriptor_index, status, out);
            } else if (descriptor_type == DESCRIPTOR_STREAM_INPUT && listener_ != nullptr) {
                filled = host_.components().acmp_is_milan()
                    ? fill_milan_listener_stream_info(host_, descriptor_index, ieee_2021, out)
                    : fill_listener_stream_info(host_, descriptor_index, listener_->stream_stopped(descriptor_index), out);
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

void AvbEntityKit::wire_presentation_offsets()
{
    // The presentation time offset (Milan v1.2 Sections 5.4.2.9-10, v1.3 5.4.2.30-31): refused
    // while the output is streaming and when the entity cannot apply it.
    host_.components().aem_handler.set_presentation_offset_hooks(
        [this](uint16_t const idx) -> std::optional<uint32_t> {
            return is_talker_index(idx) ? std::optional<uint32_t>{presentation_offset_ns(idx)} : std::nullopt;
        },
        [this](uint16_t const idx, uint32_t const offset_ns) -> uint8_t { return change_presentation_offset(idx, offset_ns); });
}

void AvbEntityKit::wire_acmp_callbacks()
{
    if (host_.components().acmp_is_milan()) {
        wire_milan_talker_callbacks();
        wire_milan_listener_callbacks();
        return;
    }
    wire_ieee_acmp_callbacks();
}

void AvbEntityKit::wire_ieee_acmp_callbacks()
{  // NOLINT(statusbar-block-statements) one hook per ACMP event
    host_.components().acmp_talker.set_on_probe(
        [this](uint16_t const stream_index, TimePoint const now) { notify_talker_probe(stream_index, now); });
    // ACMP: log talker connections and publish the fresh connection count for the media-timer gate.
    host_.components().acmp_talker.set_connection_callbacks(
        [this](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            host_.ctl_log().status(
                "acmp: talker stream {} ({}) CONNECTED by listener {:012x} unique_id {}",
                stream_index,
                logging::static_str(stream_kind_name(talker_kind_of(stream_index))),
                listener_entity_id.to_uint64(),
                listener_unique_id);
            publish_talker_connections(stream_index);
        },
        [this](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            host_.ctl_log().status(
                "acmp: talker stream {} ({}) DISCONNECTED by listener {:012x} unique_id {}",
                stream_index,
                logging::static_str(stream_kind_name(talker_kind_of(stream_index))),
                listener_entity_id.to_uint64(),
                listener_unique_id);
            publish_talker_connections(stream_index);
        });
    if (listener_ == nullptr) {
        return;
    }
    // A connected input: the MSRP Listener declaration and the multicast join; a disconnect is the
    // controller's: no interruption.
    host_.components().acmp_listener.set_connection_callbacks(
        [this](uint16_t stream_index, ieee::Eui64 const& stream_id, ieee::Eui48 dest_mac, TimePoint now) {
            listener_->on_listener_connected(stream_index, stream_id, dest_mac, now);
        },
        [this](uint16_t stream_index, TimePoint now) {
            listener_->on_listener_disconnected(stream_index, now, StreamInputLoss::released);
        });
}

auto AvbEntityKit::talker_addresses_valid() const noexcept -> bool
{
    // Milan v1.3 4.3.3.1 (1): MAAP has the address and has reported no conflict.
    return maap_.ready() && host_.components().msrp_handler.talker_addresses_valid();
}

auto AvbEntityKit::talker_dest_mac_valid(uint16_t const stream_index) const -> bool
{
    // As talker_addresses_valid(), for a stream that has an address: a kind with no static one has none.
    auto const stream = host_.components().talker_stream(stream_index);
    return talker_addresses_valid() && stream.has_value() && stream->stream_dest_mac != ieee::Eui48{};
}

auto AvbEntityKit::talker_registering_failed(uint16_t const stream_index) const -> bool
{
    auto const stream = host_.components().talker_stream(stream_index);
    return stream.has_value() && host_.components().msrp_handler.talker_listener_asking_failed(srp_stream_id(stream->stream_id));
}

void AvbEntityKit::wire_milan_talker_callbacks()
{
    // What the Milan talker asks of the rest of the entity (5.5.4): a probe
    // is told to SRP, the address comes from MAAP, REGISTERING_FAILED from SRP.
    auto& talker = *host_.components().milan_acmp_talker;
    talker.set_on_probe([this](uint16_t const stream_index, TimePoint const now) { notify_talker_probe(stream_index, now); });
    talker.set_dest_mac_valid([this](uint16_t const stream_index) { return talker_dest_mac_valid(stream_index); });
    talker.set_registering_failed([this](uint16_t const stream_index) { return talker_registering_failed(stream_index); });
}

auto AvbEntityKit::locked_out(ieee::Eui64 const& controller) const noexcept -> bool
{
    auto const& aem = host_.components().aem_handler;
    return aem.is_locked() && aem.locking_controller() != controller;
}

auto AvbEntityKit::own_gptp_state() const noexcept -> atdecc::MilanGptpState
{
    // What this entity's own ENTITY_AVAILABLE says, kept current by the
    // gPTP Announce handler (nanoavb::report_grandmaster).
    auto const& adpdu = host_.components().adp_advertiser.adpdu();
    return atdecc::MilanGptpState{.grandmaster_id = adpdu.gptp_grandmaster_id, .domain_number = adpdu.gptp_domain_number.get()};
}

auto AvbEntityKit::talker_registration_for(atdecc::MilanSrpParameters const& srp) const -> atdecc::MilanTalkerRegistration
{
    // 5.3.8.8: the attribute counts only when its StreamID, destination
    // address and VLAN are the ones the PROBE_TX_RESPONSE gave. The sink's
    // Listener declaration follows the same match, so the two cannot disagree.
    auto const registered = host_.components().msrp_handler.registered_talker(srp_stream_id(srp.stream_id), probed_talker(srp));
    if (!registered.has_value()) {
        return atdecc::MilanTalkerRegistration::None;
    }
    return registered->failed ? atdecc::MilanTalkerRegistration::Failed : atdecc::MilanTalkerRegistration::Advertise;
}

auto AvbEntityKit::milan_input_loss(uint16_t const stream_index) const -> StreamInputLoss
{
    // Read as the sink stops SRP: why it does (Milan v1.3 Table 5.6 exempts only a controller's unbind).
    auto const* const sink = host_.components().milan_acmp_listener->sink(stream_index);
    bool const lost = sink != nullptr && sink->context().srp_stop == atdecc::MilanSrpStop::TalkerLost;
    return lost ? StreamInputLoss::talker_lost : StreamInputLoss::released;
}

auto AvbEntityKit::probed_talker(atdecc::MilanSrpParameters const& srp) noexcept -> nanoavb::TalkerMatch
{
    return nanoavb::TalkerMatch{.dest_address = srp.stream_dest_mac, .vlan_id = srp.stream_vlan_id};
}

void AvbEntityKit::wire_milan_listener_connections(nanoavb::NanoAvbMilanAcmpListener& listener)
{
    // A sink that settles reserves and listens (5.5.3.6.18 step 4), its VID joined (Milan v1.2
    // Section 4.4.1); one that gives its SRP parameters up stops. Its Listener declaration follows
    // the Talker attribute it probed and no other (5.3.8.5).
    if (listener_ == nullptr) {
        return;
    }
    listener.set_connection_callbacks(
        [this](uint16_t stream_index, ieee::Eui64 const& stream_id, ieee::Eui48 dest_mac, TimePoint now) {
            auto const* sink = host_.components().milan_acmp_listener->sink(stream_index);
            auto const only = sink != nullptr ? std::optional{probed_talker(sink->context().srp)} : std::nullopt;
            listener_->on_listener_connected(stream_index, stream_id, dest_mac, now, only);
            if (sink != nullptr) {
                join_input_vlan(stream_index, sink->context().srp, now);
            }
        },
        [this](uint16_t stream_index, TimePoint now) {
            if (stream_index < input_vid_.size()) {
                leave_sink_vlan(std::exchange(input_vid_[stream_index], uint16_t{0}), now);
            }
            listener_->on_listener_disconnected(stream_index, now, milan_input_loss(stream_index));
        });
}

void AvbEntityKit::wire_milan_listener_callbacks()
{
    // What the Milan listener asks of the rest of the entity (5.5.3, 5.6.4):
    // the lock from AEM, the grandmaster from gPTP, the Talker attribute from
    // SRP, which also says when to look again.
    auto& listener = *host_.components().milan_acmp_listener;
    listener.set_locked_out([this](ieee::Eui64 const& controller) { return locked_out(controller); });
    listener.set_gptp_state([this]() { return own_gptp_state(); });
    listener.set_talker_registration([this](atdecc::MilanSrpParameters const& srp) { return talker_registration_for(srp); });
    host_.components().msrp_handler.set_on_remote_talker([this](nanoavb::StreamId const& stream_id, TimePoint const now) {
        host_.components().milan_acmp_listener->on_srp_changed(now);
        join_settled_input_vlans(stream_id, now);
    });
    wire_milan_listener_connections(listener);
}

void AvbEntityKit::follow_milan_bindings()
{
    // The bound and stopped states, then the kit's own save.
    follow_inputs_bound();
    apply_input_stopped_states();
    persist_pending_ = true;
}

void AvbEntityKit::follow_inputs_bound()
{
    // Milan v1.3 Section 5.3.8.10: bound from unbound zeroes the input's counters; an unbind is
    // no interruption.
    for (auto const& spec : listener_ != nullptr ? std::span{listener_specs_} : std::span<StreamSpec const>{}) {
        follow_input_bound(spec.index);
    }
}

void AvbEntityKit::follow_input_bound(uint16_t const index)
{
    auto const* const listener = host_.components().milan_acmp_listener.get();
    bool const bound = listener != nullptr && listener->is_bound(index);
    if (index >= input_bound_.size() || bound == std::exchange(input_bound_[index], bound)) {
        return;
    }
    if (bound) {
        listener_->reset_input_counters(index);  // Milan v1.3 5.3.8.10: zeroed at a bind, told at the interval's end
    }
}

auto AvbEntityKit::on_streaming_changed(uint16_t const type, uint16_t const index, bool const streaming) -> uint8_t
{
    if (type == DESCRIPTOR_STREAM_INPUT) {
        start_or_stop_input(index, !streaming);
        return atdecc::AEM_STATUS_SUCCESS;
    }
    return type == DESCRIPTOR_STREAM_OUTPUT ? start_or_stop_output(index, !streaming) : atdecc::AEM_STATUS_SUCCESS;
}

auto AvbEntityKit::start_or_stop_output(uint16_t const index, bool const stopped) -> uint8_t
{
    // Milan v1.3 5.4.2.19 and 5.4.2.20: "The PAAD-AE shall not support the
    // START_STREAMING command for a Stream Output (NOT_SUPPORTED shall be
    // returned)", and the same of STOP_STREAMING.
    if (milan_profile()) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;
    }
    // IEEE 1722.1: STOP_STREAMING gates the talker slot; START reopens. It shows in GET_STREAM_INFO
    // as STREAMING_WAIT (IEEE 1722.1-2021 Table 7-145), told to every subscribed controller (7.5.2).
    talker_->set_stream_stopped(index, stopped);
    notify_talker_gates();
    (void)host_.components().aem_handler.notify_stream_info_changed(DESCRIPTOR_STREAM_OUTPUT, index);
    return atdecc::AEM_STATUS_SUCCESS;
}

void AvbEntityKit::start_or_stop_input(uint16_t const index, bool const stopped)
{
    // Milan v1.3 5.3.8.7: the started/stopped state of a stream input is its sink's, a binding
    // parameter saved with the others; a command for an input that is not bound has no effect.
    // What the sink restored comes back through the bindings hook (apply_input_stopped_states).
    if (host_.components().acmp_is_milan()) {
        (void)host_.components().milan_acmp_listener->set_streaming_wait(index, stopped);
        return;
    }
    set_input_stopped(index, stopped);
}

auto AvbEntityKit::input_stopped(uint16_t const index) const noexcept -> bool
{
    if (host_.components().acmp_is_milan()) {
        auto const* const sink = host_.components().milan_acmp_listener->sink(index);
        return sink != nullptr && sink->is_bound() && binding_waits(sink->context().binding);
    }
    return !storage_handler_.is_streaming(DESCRIPTOR_STREAM_INPUT, index);
}

void AvbEntityKit::set_input_stopped(uint16_t const index, bool const stopped)
{
    if (listener_ == nullptr || listener_->stream_stopped(index) == stopped) {
        return;
    }
    listener_->set_stream_stopped(index, stopped);
    host_.ctl_log().status(
        "acmp: listener stream {} {}",
        index,
        stopped ? logging::lit("STOPPED: its frames are discarded") : logging::lit("no longer STOPPED"));
    // Table 5.20: the started/stopped state of a stream input is told.
    (void)host_.components().aem_handler.notify_stream_info_changed(DESCRIPTOR_STREAM_INPUT, index);
}

void AvbEntityKit::apply_input_stopped_states()
{
    if (listener_ == nullptr) {
        return;
    }
    for (auto const& spec : listener_specs_) {
        set_input_stopped(spec.index, input_stopped(spec.index));
    }
}

void AvbEntityKit::restart_milan_sinks(TimePoint const now)
{
    // A start is a power cycle to a Milan sink (5.5.1.4): what it had
    // reserved went with the stop, and a bound one begins again by waiting
    // for its talker's ADP. Nothing to do on a first start.
    if (host_.components().acmp_is_milan()) {
        host_.components().milan_acmp_listener->restart(now);
    }
}

auto AvbEntityKit::change_presentation_offset(uint16_t const idx, uint32_t const offset_ns) -> uint8_t
{
    if (!on_presentation_offset_changed_) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;
    }
    // Streaming as Milan v1.2 Section 5.3.7.3 has it: declaring and a Listener Ready / Ready
    // Failed registered (the grace window included), even with the transmit gate disabled.
    if (config_.kit_data_plane && talker_declared(idx) && gate_.listener_admitted(idx, gate_.told_time())) {
        return atdecc::AEM_STATUS_STREAM_IS_RUNNING;
    }
    uint32_t const before = presentation_offset_ns(idx);
    return offset_ns == before ? atdecc::AEM_STATUS_SUCCESS : commit_presentation_offset(idx, before, offset_ns);
}

auto AvbEntityKit::commit_presentation_offset(uint16_t const idx, uint32_t const before, uint32_t const offset_ns) -> uint8_t
{
    if (auto const status = on_presentation_offset_changed_(idx, offset_ns); status != atdecc::AEM_STATUS_SUCCESS) {
        // The kit keeps no deferred offset to finish later: IN_PROGRESS is not an answer here.
        return status == atdecc::AEM_STATUS_IN_PROGRESS ? atdecc::AEM_STATUS_ENTITY_MISBEHAVING : status;
    }
    presentation_offset_ns_[idx].store(offset_ns, std::memory_order_relaxed);
    host_.ctl_log().status("aem: talker stream {} presentation offset {} -> {} ns", idx, before, offset_ns);
    return atdecc::AEM_STATUS_SUCCESS;
}

void AvbEntityKit::join_input_vlan(uint16_t const index, atdecc::MilanSrpParameters const& srp, TimePoint const now)
{
    // A probe answer naming no VID (0) follows the matched Talker attribute's while one is registered.
    uint16_t const vid = srp.stream_vlan_id != 0 ? srp.stream_vlan_id : matched_talker_vid(srp);
    if (index >= input_vid_.size() || vid == 0 || input_vid_[index] == vid) {
        return;
    }
    join_sink_vlan(vid, now);  // the new VID held before the old one is left
    leave_sink_vlan(std::exchange(input_vid_[index], vid), now);
}

void AvbEntityKit::join_settled_input_vlans(nanoavb::StreamId const& stream_id, TimePoint const now)
{
    // A sink settled on a probe answer naming no VID joins its Talker attribute's VID once that
    // registers, as one naming it does at its settle (Milan v1.2 Section 4.4.1).
    auto const& listener = *host_.components().milan_acmp_listener;
    for (auto const& spec : listener_specs_) {
        auto const* sink = listener.sink(spec.index);
        if (sink != nullptr && sink->is_settled() && srp_stream_id(sink->context().srp.stream_id) == stream_id) {
            join_input_vlan(spec.index, sink->context().srp, now);
        }
    }
}

auto AvbEntityKit::matched_talker_vid(atdecc::MilanSrpParameters const& srp) const -> uint16_t
{
    auto const registered = host_.components().msrp_handler.registered_talker(srp_stream_id(srp.stream_id));
    return registered.has_value() && registered->dest_address == srp.stream_dest_mac ? registered->vlan_id : uint16_t{0};
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

auto AvbEntityKit::setup_maap_addresses(net::MessageReactor& reactor, TimePoint const now) -> Status
{
    sg14::inplace_vector<uint16_t, MAX_ENTITY_STREAMS> indices{};
    for (auto const& spec : talker_specs_) {
        (void)indices.try_push_back(spec.index);
    }
    maap_.set_on_ready_changed([this](bool) { notify_talker_gates(); });
    // On (re)defend: release the ADP start the Link Up held back; the streams
    // declare against the addresses. A conflict withdraws them and a new block
    // waits out 2 x LeaveAll (Milan v1.2 Table 5.3).
    return maap_.setup(
        reactor,
        config_.interface_name,
        stream_base_mac_for(config_.interface_mac, config_.interface_name, config_.entity_id),
        host_.components(),
        *talker_,
        indices,
        host_.ctl_log(),
        [this](TimePoint const time) {
            host_.release_adp(time);
            notify_talker_addresses(true);
            advertise_talker_streams(time);  // at the addresses now theirs
        },
        [this](TimePoint const /*time*/) { notify_talker_addresses(false); },
        now,
        state_file(config_.stream_maap_range_file, entity_state_file::MAAP_RANGE, true));
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
    if (!std::exchange(link_up_, true)) {
        maap_.port_operational(time);    // a repeat Link Up would restart the probe sequence
        host_.hold_adp(!maap_.ready());  // the first ENTITY_AVAILABLE once the block is defended
    }
    host_.on_link_up(time);
    if (!changed) {
        return;
    }
    if (host_.is_ready()) {
        advertise_talker_streams(time);  // the AccumulatedLatency moved
    }
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
{  // NOLINT(statusbar-block-statements) the control plane's start, one step after another
    prepare_control_plane();
    if (auto status = host_.start_control_plane(reactor, config_.interface_name); !status) {
        return status;
    }
    // Milan v1.3 4.2.7.2.1: after startup the Class A Domain is the
    // configured one again, whatever was adopted before the stop; the
    // streams, MVRP and GET_AVB_INFO follow (follow_domain). The Link Up
    // that follows a start declares it.
    (void)host_.components().msrp_handler.return_to_configured_domain(now);
    // The stream addresses are valid until MAAP, where it runs, says they
    // are not: an earlier run's stop left them marked as given up.
    host_.components().msrp_handler.set_talker_addresses_valid(true);
    wire_stream_callbacks();
    // The VID in force, registered on every start: stop() withdraws it with
    // the rest, but the Domain adopted from the wire outlives a stop, and
    // MVRP must say what the MSRP Domain declares.
    (void)host_.components().mvrp_handler.register_vlan(current_vlan_id(), now);
    restart_milan_sinks(now);
    restore_clock_sources();
    follow_grandmaster();
    return start_declarations(reactor, now);
}

void AvbEntityKit::prepare_control_plane()
{
    // Bring up the shared control plane (net handlers + generic SM wiring),
    // then attach the stream-specific callbacks (hooks + ACMP + AEM handlers).
    gate_.set_logger(host_.ctl_log());
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
    size_t const domains = clock_domain_count();
    if (domains != 0 && priorities.size() > domains) {
        host_.ctl_log().warning(
            "kit: {} default media clock reference priorities for {} clock domain(s): the rest ignored",
            priorities.size(),
            domains);
    }
    for (size_t index = 0; !priorities.empty() && index < domains; ++index) {
        (void)host_.components().aem_handler.set_default_mcr_priority(
            static_cast<uint16_t>(index), priorities[std::min(index, priorities.size() - 1)]);
    }
}

auto AvbEntityKit::clock_domain_count() const -> size_t
{
    size_t count = 0;
    while (count < nanoavb::AemCommandHandler::MAX_CLOCK_DOMAINS &&
           host_.get_descriptor(atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN, static_cast<uint16_t>(count)).has_value()) {
        ++count;
    }
    return count;
}

auto AvbEntityKit::start_declarations(net::MessageReactor& reactor, TimePoint const now) -> Status
{
    // In "maap" mode, set up the claim of the talker stream destination
    // addresses via MAAP: the Link Up begins it (B.3.5.9), and the acquired
    // addresses then overwrite the static defaults the data plane reads below.
    // Non-fatal inside: on socket failure the entity keeps the static MACs.
    if (effective_stream_address_mode(config_) == "maap") {
        if (auto status = setup_maap_addresses(reactor, now); !status) {
            return status;
        }
    }
    // Static addresses are valid at once, MAAP's when the block is defended. Until
    // then the configured default must not reach a listener (TALKER_DEST_MAC_FAIL).
    for (auto const& spec : maap_.ready() ? std::span<StreamSpec const>{} : std::span{talker_specs_}) {
        if (auto const s = host_.components().talker_stream(spec.index); s.has_value()) {
            (void)host_.components().configure_talker_stream(spec.index, s->stream_id, ieee::Eui48{}, s->stream_vlan_id);
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
                (*kit)->run_held_work(now_ns);
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
    notify_talker_gates();  // the slots are open: each gate open now is told
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
        if (auto const s = host_.components().talker_stream(spec.index); s.has_value()) {
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
    attach_listener_rx(reactor);
#if defined(__linux__)
    // The VID's tagged stream frames must get past the NIC's VLAN filter.
    // Never fatal: without CAP_NET_ADMIN this only warns.
    // (A control bound before start() -- a test's -- is used as it is.)
    if (!vlan_filters_.bound()) {
        open_listener_vlan_filter();
    }
#endif
    restore_listener_bindings();
    return success();
}

void AvbEntityKit::attach_listener_rx(net::MessageReactor& reactor)
{
    // In RT-timer mode the handler is kept here for the tool's SCHED_FIFO RX timer (drain_stream_rx(),
    // its wake's gPTP time), so the control plane cannot starve RX; rx_to_reactor() is the way back
    // should that timer not start.
    if (stream_rx_error_) {
        return;
    }
    if (auto rx = listener_->attach_rx(stream_port_, rx_static_groups_, reactor, config_.stream_rx_rt_timer)) {
        rt_rx_handler_ = std::move(*rx);
    } else {
        stream_rx_error_ = rx.error();  // tolerated here; a host may refuse (stream_rx_error())
    }
}

void AvbEntityKit::restore_listener_bindings()
{
    restore_bindings();
    if (!config_.kit_data_plane) {
        restore_input_formats();  // each input's format, saved when a controller last set it (Section 5.3.8.1)
    }
}

void AvbEntityKit::restore_bindings()
{
    // A Milan listener's bindings are saved and restored by the host, in the entity's state
    // directory (Milan v1.3 Section 5.3.8.3); a classic one's fast-connect goals in the persistent
    // state when there is one.
    bool const milan = host_.components().acmp_is_milan();
    if (!milan && persistence_active_) {
        apply_saved_listener_bindings();
        return;
    }
    host_.enable_listener_binding_persistence(
        state_file(config_.listener_bindings_path, entity_state_file::LISTENER_BINDINGS, milan));
    // A restored binding is bound, and its started/stopped state restored with it (Milan v1.3
    // Sections 5.3.8.10, 5.3.8.7): its unbind is then no interruption.
    follow_inputs_bound();
    apply_input_stopped_states();
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
    class_a_filtered_ = current_vlan_id();
    vlan_filters_.hold(class_a_filtered_);
    for (auto const vid : input_vid_) {
        vlan_filters_.hold(vid);  // the inputs settled before (0: none)
    }
}
#endif

auto AvbEntityKit::stop(TimePoint const now) -> Status
{  // NOLINT(statusbar-block-statements) the stop sequence, the reverse of start's phases
    if (listener_ != nullptr && host_.is_running()) {
        listener_->release_connections(now);  // a Waiting declaration outlives withdraw_all()
    }
    close_data_plane();
    notify_talker_gates();  // no slot: every gate closes
    if (!host_.is_running()) {
        return failure(std::make_error_code(std::errc::not_connected));
    }
    // Stopping the control plane runs no withdraw hook: withdraw here, so a restart starts undeclared,
    // inactive, unconnected and gated; then the MAAP range (Table B.7 Release!, its port off the reactor),
    // then the control plane. start() builds all three again.
    withdraw_talker_streams(now);
    drop_listener_connections(now);
    maap_.stop(now);
    link_up_ = false;  // the next start()'s Link Up is a first one again
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
    if (host_.components().acmp_is_milan()) {
        return;  // a Milan sink keeps its binding; the next start restarts it (restart_milan_sinks)
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

auto AvbEntityKit::next_control_deadline_ns() const noexcept -> std::optional<int64_t>
{
    auto earliest = maap_.next_deadline_ns();
    auto const& components = host_.components();
    for (auto const next :
         {components.msrp_handler.next_deadline_ns(), components.mvrp_handler.next_deadline_ns(), next_counters_fold_ns_}) {
        if (next && (!earliest || *next < *earliest)) {
            earliest = next;
        }
    }
    return earliest;
}

void AvbEntityKit::apply_mrp_timers(bool const milan)
{
    auto const timers = milan ? nanoavb::MILAN_MRP_TIMERS : srp::mrp::TimerConfig{};
    host_.components().msrp_handler.set_mrp_timers(timers);
    host_.components().mvrp_handler.set_mrp_timers(timers);
}

auto AvbEntityKit::milan_profile() const noexcept -> bool
{
    // The configuration decides; left empty, the model does: a Milan model
    // answers to Milan v1.3, any other to IEEE 1722.1-2021 as written.
    return is_milan_profile(config_);
}

void AvbEntityKit::install_milan_info()
{
    if (!config_.milan) {
        host_.components().aem_handler.set_milan_info(std::nullopt);
        return;
    }
    advertise_milan_capabilities();
    host_.components().aem_handler.set_milan_info(nanoavb::AemCommandHandler::MilanInfo{
        .features_flags = milan_features_flags(),
        .certification_version = milan_version_or(config_.milan_certification_version, 0, "milan.certification_version"),
        .specification_version =
            milan_version_or(config_.milan_specification_version, atdecc::mvu_version(1, 3), "milan.specification_version")});
}

namespace {

/// What a Milan PAAD-AE says of itself in its entity_capabilities (Milan v1.3
/// 5.3.3.1, 5.6.2): it answers the MVU commands, and the ADPDU's
/// interface_index names its AVB_INTERFACE.
constexpr uint32_t MILAN_ENTITY_CAPABILITIES =
    atdecc::entity_capabilities::VENDOR_UNIQUE_SUPPORTED | atdecc::entity_capabilities::AEM_INTERFACE_INDEX_VALID;

}  // namespace

void AvbEntityKit::advertise_milan_capabilities()
{
    // Both are what this entity does because it was built as a Milan one,
    // whatever its model authors, so they are added here: to the ENTITY
    // descriptor it serves, and to the copy of it that ADP advertises from.
    storage_handler_.add_entity_capabilities(MILAN_ENTITY_CAPABILITIES);
    auto& adp = host_.components().adp_advertiser;
    auto entity = adp.entity();
    entity.entity_capabilities.set_flag(MILAN_ENTITY_CAPABILITIES);
    adp.set_entity(entity);
    adp.update_adpdu_from_model();  // before the entity is advertised
}

auto AvbEntityKit::milan_features_flags() const noexcept -> uint32_t
{
    // MVU_BINDING says that BIND_STREAM and UNBIND_STREAM are implemented (Milan v1.3 5.4.4.6,
    // 5.4.4.7): whenever Milan connection management runs and never otherwise. Signal presence
    // when the application reports it (Section 5.3.7.7.1).
    uint32_t const configured = config_.milan_features_flags & ~atdecc::MVU_FEATURE_MVU_BINDING;
    uint32_t const presence = config_.signal_presence ? atdecc::MVU_FEATURE_TALKER_SIGNAL_PRESENCE : 0U;
    return configured | presence | (milan_profile() ? atdecc::MVU_FEATURE_MVU_BINDING : 0U);
}

auto AvbEntityKit::state_file(std::string const& named, std::string_view const name, bool const wanted) -> std::string
{
    if (!named.empty() || !wanted || entity_state_dir().empty()) {
        return named;
    }
    return entity_state_file_path(entity_state_dir(), name);
}

auto AvbEntityKit::entity_state_dir() -> std::string const&
{
    if (entity_state_dir_.has_value()) {
        return *entity_state_dir_;
    }
    entity_state_dir_.emplace();
    if (config_.state_dir.empty()) {
        return *entity_state_dir_;
    }
    auto made = ensure_entity_state_dir(config_.state_dir, host_.components().acmp_listener.entity_id());
    if (made.has_value()) {
        *entity_state_dir_ = std::move(*made);
        host_.ctl_log().status("state: saved in {}", logging::embed<56>(*entity_state_dir_));
    } else {
        host_.ctl_log().status(
            "state: no directory for this entity under {} (error {}); nothing is saved that no key names",
            logging::embed<48>(config_.state_dir),
            made.error().value());
    }
    return *entity_state_dir_;
}

auto AvbEntityKit::milan_version_or(std::string const& text, uint32_t const fallback, std::string_view const key) -> uint32_t
{
    auto const version = atdecc::mvu_version_from_dotted(text);
    if (!version) {
        host_.ctl_log().error(
            "{}: '{}' is not dotted numbers; using {}",
            logging::embed<32>(std::string{key}),
            logging::embed<16>(text),
            logging::embed<16>(atdecc::mvu_version_to_dotted(fallback)));
    }
    return version.value_or(fallback);
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
    readvertise_pending_.reset();      // a re-declaration asked for in this run: the next run declares afresh
    declaration_timer_token_.reset();  // the timer finishes
    if (persist_pending_) {
        persist();  // a listener goal change of this pass
    }
}

void AvbEntityKit::run_held_work(int64_t const now_ns)
{
    // A listener goal change during this pass is saved now, after its ACMP
    // response.
    if (persist_pending_) {
        persist();
    }
    if (!host_.is_running()) {
        return;
    }
    // A TSpec an AEM hook changed (told no time), handed to MSRP at this pass's time.
    auto const pending = std::exchange(readvertise_pending_, {});
    for (auto const& spec : pending.any() && host_.is_ready() ? std::span{talker_specs_} : std::span<StreamSpec const>{}) {
        if (pending.test(spec.index)) {
            readvertise_talker(spec, TimePoint{std::chrono::nanoseconds{now_ns}});
        }
    }
}

auto AvbEntityKit::default_presentation_offset_ns() const noexcept -> uint32_t
{
    return static_cast<uint32_t>(
        std::min<uint64_t>(effective_presentation_offset_ns(config_), atdecc::aem::MILAN_PRESENTATION_OFFSET_MAX_NS));
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
    persistent_state_path_ = persistent_state_file(false);  // the directory is made by the first save
    if (!persistent_state_path_.empty()) {
        restore_persistent_state();
    }
}

void AvbEntityKit::restore_persistent_state()
{
    // A classic listener's bindings file is imported once; a Milan one's is the host's own.
    bool const import_bindings = listener_ != nullptr && !host_.components().acmp_is_milan();
    auto loaded = avb_entity::load_persistent_state(
        persistent_state_path_, served_entity_model_id(), import_bindings ? config_.listener_bindings_path : std::string{});
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
    host_.components().aem_handler.set_on_mvu_settings_changed([this](uint16_t) { persist(); });
}

auto AvbEntityKit::notify_persistent_state_outcome(LoadedPersistentState const& loaded) -> bool
{
    auto log = host_.ctl_log();
    switch (loaded.outcome) {
        case PersistentStateOutcome::fresh:
            log.status("kit: no persistent state yet; starting from the model's defaults");
            persisted_text_ = serialize_persistent_state(collect_persistent_state());  // the defaults need no file
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
        (void)put_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, port, {}, false);
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
    auto& aem = host_.components().aem_handler;
    if (!aem.restore_system_unique_id(saved.system_unique_id, atdecc::aem::AtdeccString{saved.system_unique_name})) {
        host_.ctl_log().warning("persistent state: the saved system unique name is not UTF-8; not restored");
    }
    for (auto const& mcr : saved.media_clock_references) {
        if (mcr.clock_domain >= clock_domain_count() ||
            !aem.restore_media_clock_reference_info(
                mcr.clock_domain, mcr.user_priority, atdecc::aem::AtdeccString{mcr.domain_name})) {
            host_.ctl_log().warning(
                "kit: saved media clock reference of CLOCK_DOMAIN {}: no such clock domain: dropped", mcr.clock_domain);
        }
    }
}

void AvbEntityKit::apply_saved_listener_bindings()
{
    if (auto const armed = restore_saved_bindings(); armed != 0) {
        host_.ctl_log().status("acmp: {} saved listener binding(s) loaded; fast-connect armed", armed);
    }
    // Sticky from here on: every successful connect is remembered, every goal
    // change (connect, retarget, controller DISCONNECT) saved once the
    // reactor pass that made it is done (after the ACMP response).
    host_.components().acmp_listener.enable_sticky_bindings();
    host_.components().acmp_listener.set_on_goals_changed([this] { persist_pending_ = true; });
}

auto AvbEntityKit::restore_saved_bindings() -> size_t
{
    size_t armed = 0;
    for (auto const& binding : saved_bindings_) {
        if (restore_listener_binding(binding)) {
            ++armed;
        }
    }
    saved_bindings_.clear();
    return armed;
}

auto AvbEntityKit::restore_listener_binding(PersistedListenerBinding const& binding) -> bool
{
    if (!saved_binding_applies(binding)) {
        return false;
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
    if (config_.kit_data_plane) {
        collect_stream_formats(state);  // else the inputs' formats are in the stream-formats file
    }
    storage_handler_.for_each_runtime_sampling_rate(
        [&state](uint16_t const unit, uint32_t const rate) { state.sampling_rates.push_back({.audio_unit = unit, .rate = rate}); });
    if (config_.kit_data_plane) {  // else in the clock-sources file (save_clock_sources)
        storage_handler_.for_each_runtime_clock_source([&state](uint16_t const domain, uint16_t const source) {
            state.clock_sources.push_back({.clock_domain = domain, .clock_source = source});
        });
    }
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

void AvbEntityKit::collect_stream_formats(PersistentState& state) const
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
    } else if (host_.components().acmp_is_milan()) {
        return;  // the host's listener-bindings file
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
    auto const& aem = host_.components().aem_handler;
    auto const unique = aem.system_unique_id();
    state.system_unique_id = unique.system_unique_id.get();
    state.system_unique_name = std::string{unique.system_name.as_string_view()};
    for (size_t index = 0; index < clock_domain_count(); ++index) {
        auto const info = aem.media_clock_reference_info(static_cast<uint16_t>(index));
        auto const name = info.media_clock_domain_name.as_string_view();
        if (info.user_mcr_prio.get() != info.default_mcr_prio.get() || name != atdecc::MVU_DEFAULT_MEDIA_CLOCK_DOMAIN_NAME) {
            state.media_clock_references.push_back(
                {.clock_domain = static_cast<uint16_t>(index),
                 .user_priority = info.user_mcr_prio.get(),
                 .domain_name = std::string{name}});
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

auto AvbEntityKit::persistent_state_file(bool const make) -> std::string
{
    if (make) {
        return state_file(std::string{}, entity_state_file::ENTITY_STATE, true);
    }
    if (config_.state_dir.empty()) {
        return {};
    }
    auto const dir = entity_state_dir_path(config_.state_dir, host_.components().acmp_listener.entity_id());
    return entity_state_file_path(dir, entity_state_file::ENTITY_STATE);
}

void AvbEntityKit::save_persisted(std::string text)
{
    if (persistent_state_file(true).empty()) {
        return;  // no directory for this entity: nothing is saved
    }
    if (auto const saved = save_persistent_state(persistent_state_path_, text); !saved) {
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
    fill_talker_srp_identity(spec.index, info);
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
    auto const stream = host_.components().talker_stream(idx);
    if (!stream.has_value() || !talker_declared(idx)) {
        return std::nullopt;
    }
    auto const* const declared = host_.components().msrp_handler.get_talker_stream(srp_stream_id(stream->stream_id));
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

void AvbEntityKit::fill_talker_srp_identity(uint16_t const index, nanoavb::TalkerStreamSrpInfo& info) const
{
    // VLAN and priority are the SR class domain's in force: the configured
    // one, or the one adopted from the wire (adopt_domain); never the
    // header's defaults, which know nothing of how the frames are tagged.
    // ACMP's picture of the stream, where there is one, has the identity.
    auto const& domain = host_.components().msrp_handler.domain();
    info.vlan_id = domain.sr_class_vid;
    info.priority = domain.sr_class_priority;
    if (auto const stream = host_.components().talker_stream(index); stream.has_value()) {
        (void)statusbar::tsn::load_unchecked(stream->stream_id.span(), &info.stream_id);
        info.dest_address = stream->stream_dest_mac;
        info.vlan_id = stream->stream_vlan_id;
    }
}

void AvbEntityKit::advertise_talker_streams(TimePoint const time)
{
    // The control plane advertises: the MSRP handler declares each stream, under the Milan
    // profile once its address is valid and a listener wants it (Milan v1.3 4.3.3.1).
    for (auto const& spec : talker_specs_) {
        readvertise_talker(spec, time);
    }
}

}  // namespace statusbar::avb_entity
