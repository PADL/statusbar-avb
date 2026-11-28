// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AvbEntityKit methods — the Entity Construction Kit facade (refactor phase D;
// the wiring bodies come verbatim from AvbEntityToneGenerator/AvbEntityAudioIO,
// which each carried a copy).

#include "statusbar/avb_entity/avb_entity_kit.hpp"

#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"
#include "statusbar/avb_entity/avb_entity_identity.hpp"
#include "statusbar/avb_entity/avb_entity_state_dir.hpp"
#include "statusbar/avb_entity/avb_entity_stream_info.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <utility>

namespace statusbar::avb_entity {

using statusbar::atdecc::aem::DESCRIPTOR_AUDIO_UNIT;
using statusbar::atdecc::aem::DESCRIPTOR_STREAM_INPUT;
using statusbar::atdecc::aem::DESCRIPTOR_STREAM_OUTPUT;
using statusbar::atdecc::aem::DescriptorRef;
using statusbar::nanoavb::DomainInfo;

namespace {

/// An ACMP stream_id as the StreamID SRP knows the stream by.
[[nodiscard]] auto srp_stream_id(ieee::Eui64 const& acmp_stream_id) noexcept -> nanoavb::StreamId
{
    nanoavb::StreamId stream_id{};
    (void)statusbar::tsn::load_unchecked(acmp_stream_id.span(), &stream_id);
    return stream_id;
}

/// Stream (@p type, @p index) of configuration 0, the one the kit runs.
[[nodiscard]] constexpr auto stream_ref(uint16_t const type, uint16_t const index) noexcept -> DescriptorRef
{
    return DescriptorRef{.configuration_index = 0, .descriptor_type = type, .descriptor_index = index};
}

/// AUDIO_UNIT 0's CLOCK_DOMAIN in @p storage, unless the unit is missing or another unit shares
/// its domain: a stream runs at the rate of the unit in its domain (IEEE 1722.1-2021 Clause 7.2.3).
[[nodiscard]] auto retimed_clock_domain(atdecc::aem::DescriptorStorage const& storage) -> std::optional<uint16_t>
{
    auto const unit_zero = load_descriptor<atdecc::aem::DescriptorAudioUnit>(storage, 0, DESCRIPTOR_AUDIO_UNIT, 0);
    if (!unit_zero) {
        return std::nullopt;
    }
    for (uint16_t k = 1; k < std::numeric_limits<uint16_t>::max(); ++k) {
        auto const unit = load_descriptor<atdecc::aem::DescriptorAudioUnit>(storage, 0, DESCRIPTOR_AUDIO_UNIT, k);
        if (!unit || unit->clock_domain_index == unit_zero->clock_domain_index) {
            return unit ? std::nullopt : std::optional<uint16_t>{unit_zero->clock_domain_index.get()};
        }
    }
    return std::nullopt;
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
    , storage_handler_{handler}
    , retimed_clock_domain_{retimed_clock_domain(handler.storage())}
    , clock_source_kinds_{clock_source_kinds(handler.storage())}
    , host_{handler, default_adp_advertiser_config(), talker_specs_.size(), 4, listener_specs_.size()}
    , gate_{config.gate_talker_on_listener, host_.components()}
    , talker_{std::make_unique<TalkerStreams>(
          TalkerStreamsConfig{.sample_rate = sample_rate, .vlan_id = config.vlan_id, .stream_pcp = config.stream_pcp},
          media_clock,
          last_gptp_ns,
          memory_resource)}
    , admitted_{admitted_talker_streams(RawTimePoint{})}  // a new gate has no stamp: any instant will do
{
    active_clock_source_.store(initial_clock_source, std::memory_order_relaxed);
    crf_clock_source_index_ = crf_clock_source_index;
    make_listener_streams(last_gptp_ns, audio_sink);
    configure_control_plane();
    seed_talker_streams();
    install_storage_hooks();
}

void AvbEntityKit::make_listener_streams(std::atomic<int64_t>& last_gptp_ns, StreamRxAudioSink* const audio_sink)
{
    if (listener_specs_.empty()) {
        return;
    }
    listener_ =
        std::make_unique<ListenerStreams>(config_.lock_tolerance_ns, sample_rate_, host_.components(), last_gptp_ns, audio_sink);
    listener_->set_presentation_offset_ns(effective_presentation_offset_ns(config_));
}

void AvbEntityKit::configure_control_plane()
{
    // ATDECC descriptor wire version. "2016" emits descriptors at their
    // 1722.1-2013/2016 lengths so controllers that reject 2021-length descriptors
    // can enumerate the model; "2021" emits the full 2021 forms. Nothing said:
    // the profile's (effective_atdecc_version).
    host_.components().aem_handler.set_legacy_2016(effective_atdecc_version(config_) != "2021");
    // IDENTIFY honours acquire and lock unless the configuration says otherwise.
    host_.components().aem_handler.set_identify_bypasses_exclusive_access(config_.identify_bypasses_lock);
    apply_profile();
    // The VID itself is registered at start() (stop() withdraws it).
    host_.components().msrp_handler.set_domain(
        DomainInfo{.sr_class_id = 6, .sr_class_priority = 3, .sr_class_vid = config_.vlan_id});
    host_.components().msrp_handler.set_redeclare_registered_listeners(config_.redeclare_registered_listeners);
    host_.components().msrp_handler.set_suppress_leaveall(config_.suppress_leaveall);
}

void AvbEntityKit::apply_profile()
{
    // Where 1722.1 and Milan differ, the profile decides: the AEM handler's
    // answers, connection management, and the SRP rules (apply_srp_profile).
    // A Milan model also has GET_MILAN_INFO to answer, whichever profile it
    // is held to.
    bool const milan = milan_profile();
    host_.components().aem_handler.set_profile(
        milan ? nanoavb::AemCommandHandler::Profile::milan13 : nanoavb::AemCommandHandler::Profile::classic);
    install_milan_info();
    apply_acmp_profile(milan);
    apply_srp_profile(milan);
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
    // Everything after this asks the components for "the ACMP that runs".
    if (!milan) {
        return;
    }
    host_.components().use_milan_acmp();
    // 5.5.4.1: a Milan talker streams "as long as bandwidth is reserved for
    // its stream"; there is no connection for the gate to wait for, and no
    // disconnect to close it, so it closes with the Listener's registration.
    gate_.set_needs_acmp_connection(false);
    gate_.set_listener_grace(std::chrono::seconds{0});
}

void AvbEntityKit::apply_srp_profile(bool const milan)
{
    // On the SRP side the profile decides the MRP timers (Table 4.3), the
    // Class A Domain adopted from the wire (4.2.7.2.1), the instant leave
    // (4.2.7.2.2) and a Talker attribute declared only while a listener
    // wants the stream (4.3.3.1).
    host_.components().msrp_handler.set_adopt_domain(milan);
    host_.components().msrp_handler.set_instant_leave(milan);
    host_.components().msrp_handler.set_talker_waits_for_listener(milan);
    apply_mrp_timers(milan);
    host_.components().msrp_handler.set_on_domain_adopted(
        [this](DomainInfo const& domain, TimePoint const now) { follow_domain(domain, now); });
}

auto AvbEntityKit::static_dest_mac_for(StreamKind const kind) const noexcept -> ieee::Eui48
{
    switch (kind) {
        case StreamKind::am824:
            return config_.am824_talker_dest_mac;
        case StreamKind::aaf:
            return config_.aaf_talker_dest_mac;
        case StreamKind::crf:
            return config_.crf_talker_dest_mac;
        case StreamKind::other:
        default:
            return ieee::Eui48{};
    }
}

void AvbEntityKit::seed_talker_streams()
{
    // Base the talker stream_ids on the NIC MAC (globally unique per box), and
    // seed the static dest MACs by kind (MAAP mode reassigns after acquisition).
    ieee::Eui48 const stream_base_mac = stream_base_mac_for(config_.interface_mac, config_.interface_name, config_.entity_id);
    for (auto const& spec : talker_specs_) {
        (void)host_.components().configure_talker_stream(
            spec.index, stream_id_for(stream_base_mac, spec.index), static_dest_mac_for(spec.format.kind), config_.vlan_id);
    }
}

void AvbEntityKit::install_storage_hooks()
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
    storage_handler_.set_on_streaming_changed(
        [this](uint16_t type, uint16_t index, bool streaming) -> uint8_t { return on_streaming_changed(type, index, streaming); });
    storage_handler_.set_on_stream_format_changed([this](uint16_t type, uint16_t index, uint64_t format) -> uint8_t {
        return on_stream_format_changed(type, index, format);
    });
    // Under Milan the started/stopped state of an input is its sink's
    // (a bind's STREAMING_WAIT, START_STREAMING, STOP_STREAMING), and a bind
    // zeroes its counters: each input follows its sink's binding.
    host_.set_on_milan_bindings_changed([this]() { follow_milan_bindings(); });
    // Kit phase 3c: a controller's SET_CLOCK_SOURCE switches the active
    // source; the media thread reads it per tick (media_rate()).
    storage_handler_.set_on_clock_source_changed(
        [this](uint16_t const domain, uint16_t const source) -> uint8_t { return change_clock_source(domain, source); });
    // SET_SAMPLING_RATE: the rate in force, or a re-time the entity's data plane makes.
    storage_handler_.set_on_sampling_rate_changed(
        [this](uint16_t const audio_unit, uint32_t const rate) -> uint8_t { return change_sampling_rate(audio_unit, rate); });
}

auto AvbEntityKit::change_clock_source(uint16_t const domain, uint16_t const source) -> uint8_t
{
    // IEEE 1722.1-2021 Table 7-126: "the target of the command is not supported".
    if (domain == 0 && !clock_source_runs(source)) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;
    }
    if (domain == 0 && active_clock_source_.exchange(source, std::memory_order_acq_rel) != source) {
        restart_media_clock(domain);
    }
    save_clock_sources(domain, source);
    return atdecc::AEM_STATUS_SUCCESS;
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
    auto const path = clock_sources_file(false);
    if (path.empty()) {
        return;
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
    // returned)", and the same of STOP_STREAMING. A Milan stream output
    // cannot be stopped (5.3.7.3): it streams for as long as its Talker
    // attribute is declared and a Listener registered.
    if (milan_profile()) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;
    }
    // IEEE 1722.1: a controller's STOP_STREAMING gates the talker slot
    // (the media thread treats it as a closed SRP gate); START reopens.
    // The change shows in GET_STREAM_INFO as STREAMING_WAIT (IEEE 1722.1-2021
    // Table 7-145), so every subscribed controller is told by an unsolicited
    // GET_STREAM_INFO response (7.5.2), beside the command's own echo.
    talker_->set_stream_stopped(index, stopped);
    notify_talker_gates();
    (void)host_.components().aem_handler.notify_stream_info_changed(DESCRIPTOR_STREAM_OUTPUT, index);
    return atdecc::AEM_STATUS_SUCCESS;
}

void AvbEntityKit::start_or_stop_input(uint16_t const index, bool const stopped)
{
    // Milan v1.3 5.3.8.7: the started/stopped state of a stream input is
    // its sink's, a binding parameter saved with the others, and a command
    // for an input that is not bound has no effect (5.4.2.19, 5.4.2.20).
    // What the sink took comes back through the bindings hook
    // (apply_input_stopped_states).
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
        return sink != nullptr && sink->is_bound() && sink->context().binding->streaming_wait;
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
    notify_input_info(index);
}

void AvbEntityKit::notify_input_info(uint16_t const index)
{
    if (index < input_info_told_.size() && input_info_told_[index].has_value()) {
        input_info_told_[index] = told_input_info(index);
    }
    (void)host_.components().aem_handler.notify_stream_info_changed(DESCRIPTOR_STREAM_INPUT, index);
}

auto AvbEntityKit::told_input_info(uint16_t const index) -> std::optional<atdecc::aem::AemStreamInfoPayload>
{
    atdecc::aem::AemStreamInfoPayload info{};
    if (!stream_info(DESCRIPTOR_STREAM_INPUT, index, info)) {
        return std::nullopt;
    }
    info.stream_format = {};
    return info;
}

void AvbEntityKit::tell_changed_input_info()
{
    if (listener_ == nullptr || !host_.components().acmp_is_milan()) {
        return;
    }
    for (auto const& spec : listener_specs_) {
        tell_input_info_if_changed(spec.index);
    }
}

void AvbEntityKit::tell_input_info_if_changed(uint16_t const index)
{
    auto found = told_input_info(index);
    if (index >= input_info_told_.size() || !found.has_value()) {
        return;
    }
    auto const told = std::exchange(input_info_told_[index], found);
    if (told.has_value() && *told != *found) {
        (void)host_.components().aem_handler.notify_stream_info_changed(DESCRIPTOR_STREAM_INPUT, index);
    }
}

auto AvbEntityKit::milan_sink_stream_info(uint16_t const index) const -> std::optional<MilanSinkStreamInfo>
{
    auto const* const listener = host_.components().milan_acmp_listener.get();
    auto const* const sink = listener != nullptr ? listener->sink(index) : nullptr;
    if (sink == nullptr) {
        return std::nullopt;
    }
    auto const& srp = sink->context().srp;
    auto const& msrp = host_.components().msrp_handler;
    return MilanSinkStreamInfo{
        .bound = sink->is_bound(),
        .settled = sink->is_settled(),
        .stopped = listener_->stream_stopped(index),
        .srp = srp,
        .talker = sink->is_settled() ? msrp.registered_talker(srp_stream_id(srp.stream_id), probed_talker(srp)) : std::nullopt};
}

auto AvbEntityKit::on_stream_format_changed(uint16_t const type, uint16_t const index, uint64_t const format) -> uint8_t
{
    // The format in force named again changes nothing and is refused by nothing.
    if (current_stream_format(type, index) == format) {
        return atdecc::AEM_STATUS_SUCCESS;
    }
    // A stream output's serializer is made once, for the format its model
    // authors. Milan v1.3 5.3.3.4 has an output with static mappings list
    // only the formats those mappings hold in, which is that one.
    return type == DESCRIPTOR_STREAM_INPUT ? change_input_format(index, format) : atdecc::AEM_STATUS_NOT_SUPPORTED;
}

auto AvbEntityKit::change_input_format(uint16_t const index, uint64_t const format) -> uint8_t
{
    // Milan v1.3 5.4.2.7: "The PAAD-AE shall not accept a SET_STREAM_FORMAT
    // command on a Stream Input if the Stream Input is bound ... In these
    // cases, the STREAM_IS_RUNNING error code shall be returned." IEEE
    // 1722.1-2021 7.4.9.2 says it of a stream that is streaming.
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

auto AvbEntityKit::current_stream_format(uint16_t const type, uint16_t const index) -> uint64_t
{
    return storage_handler_.current_stream_format(stream_ref(type, index)).value_or(ieee::Eui64{}).to_uint64();
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
    // Milan v1.3 5.3.8.1: "The current format shall be saved in a
    // non-volatile memory and restored after a power cycle."
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
    auto const ref = stream_ref(saved.descriptor_type, saved.descriptor_index);
    // Asked in this order, so that nothing is taken unless both take it: the
    // listener can, the model still describes it, the listener does.
    bool const taken = saved.descriptor_type == DESCRIPTOR_STREAM_INPUT &&
        listener_->can_take_stream_format(saved.descriptor_index, saved.format) &&
        storage_handler_.set_runtime_stream_format(ref, ieee::Eui64{}.from_uint64(saved.format)) &&
        listener_->set_stream_format(saved.descriptor_index, saved.format);
    if (!taken) {
        host_.ctl_log().status(
            "state: the saved format {:016x} of stream {} is not one it has now; it keeps its authored format",
            saved.format,
            saved.descriptor_index);
    }
}

void AvbEntityKit::follow_milan_bindings()
{
    apply_input_stopped_states();
    follow_inputs_bound();
}

void AvbEntityKit::follow_inputs_bound()
{
    if (listener_ == nullptr || !host_.components().acmp_is_milan()) {
        return;
    }
    for (auto const& spec : listener_specs_) {
        follow_input_bound(spec.index);
    }
}

void AvbEntityKit::follow_input_bound(uint16_t const index)
{
    // Milan v1.3 5.3.8.10: zeroed each time the input becomes bound, not when it is unbound.
    auto const* const sink = host_.components().milan_acmp_listener->sink(index);
    bool const bound = sink != nullptr && sink->is_bound();
    if (index >= input_bound_.size() || bound == std::exchange(input_bound_[index], bound)) {
        return;
    }
    if (bound) {
        listener_->reset_input_counters(index);
    }
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
{
    // Milan v1.3 4.2.7.2.1: the Class A Domain registered from the wire has
    // replaced ours, or a startup or Link Up has brought the configured one
    // back, and the SRP handler has re-declared it. MVRP follows the VLAN,
    // the streams move (move_talker_streams_to), and the registered
    // controllers hear the new mapping (GET_AVB_INFO, Table 5.20).
    uint16_t const old_vlan = current_vlan_id();
    if (domain.sr_class_vid != old_vlan) {
        (void)host_.components().mvrp_handler.withdraw_vlan(old_vlan, now);
        (void)host_.components().mvrp_handler.register_vlan(domain.sr_class_vid, now);
    }
    move_talker_streams_to(domain, now);
    (void)host_.components().aem_handler.notify_avb_info_changed(0);
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

void AvbEntityKit::wire_stream_callbacks()
{
    wire_srp_callbacks();
    wire_acmp_callbacks();
    wire_aem_stream_callbacks();
}

void AvbEntityKit::wire_srp_callbacks()
{
    // MSRP reservations for the talker streams. The host declares the SR class
    // domain before calling these, then advertises/withdraws on the MSRP cycle.
    host_.set_advertise_streams([this](TimePoint time) { advertise_talker_streams(time); });
    host_.set_withdraw_streams([this](TimePoint time) {
        for (auto const& spec : talker_specs_) {
            (void)host_.components().msrp_handler.talker_withdraw(make_talker_srp_info(spec).stream_id, time);
        }
    });
    // The per-stream transmit gate tracks MSRP Listener Ready, stamped with the cycle's told instant.
    host_.set_on_listener_ready([this](nanoavb::StreamId const& stream_id, bool const ready) {
        notify_listener_ready(stream_id, ready, gate_.told_time());
    });
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

void AvbEntityKit::notify_listener_ready(nanoavb::StreamId const& stream_id, bool const ready, RawTimePoint const now)
{
    gate_.note_listener_ready(stream_id, ready, now);
    notify_admission(now);
}

void AvbEntityKit::publish_talker_connections(uint16_t const idx)
{
    gate_.note_acmp_connections(idx, static_cast<uint32_t>(host_.components().acmp_talker.connection_count(idx)));
    notify_admission(gate_.told_time());
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

auto AvbEntityKit::admitted_talker_streams() const noexcept -> AdmittedStreams
{
    return admitted_talker_streams(gate_.told_time());
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

void AvbEntityKit::note_talker_probe(uint16_t const stream_index, TimePoint const now)
{
    // Milan v1.3 4.3.3.1: a PROBE_TX_COMMAND for one of the stream outputs.
    // The SRP handler declares the stream's Talker attribute for the next
    // 15 seconds, by which time the listener that probed has declared its
    // own; a handler that does not wait for a listener ignores it.
    if (auto const stream = host_.components().talker_stream(stream_index); stream.has_value()) {
        host_.components().msrp_handler.note_talker_probe(srp_stream_id(stream->stream_id), now);
    }
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

auto AvbEntityKit::talker_addresses_valid() const noexcept -> bool
{
    // Milan v1.3 4.3.3.1 (1): MAAP has the address and has reported no conflict.
    return maap_.ready() && host_.components().msrp_handler.talker_addresses_valid();
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
    talker.set_on_probe([this](uint16_t const stream_index, TimePoint const now) { note_talker_probe(stream_index, now); });
    talker.set_dest_mac_valid([this](uint16_t /*stream_index*/) { return talker_addresses_valid(); });
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
    // Listener declaration follows the same match (wire_milan_listener_connections),
    // so the two cannot disagree.
    auto const registered = host_.components().msrp_handler.registered_talker(srp_stream_id(srp.stream_id), probed_talker(srp));
    if (!registered.has_value()) {
        return atdecc::MilanTalkerRegistration::None;
    }
    return registered->failed ? atdecc::MilanTalkerRegistration::Failed : atdecc::MilanTalkerRegistration::Advertise;
}

auto AvbEntityKit::probed_talker(atdecc::MilanSrpParameters const& srp) noexcept -> nanoavb::TalkerMatch
{
    return nanoavb::TalkerMatch{.dest_address = srp.stream_dest_mac, .vlan_id = srp.stream_vlan_id};
}

void AvbEntityKit::wire_milan_listener_connections(nanoavb::NanoAvbMilanAcmpListener& listener)
{
    // A sink that settles reserves and listens (5.5.3.6.18 step 4); one that
    // gives its SRP parameters up stops. Its Listener declaration follows the
    // Talker attribute it probed and no other (5.3.8.5).
    if (listener_ == nullptr) {
        return;
    }
    listener.set_connection_callbacks(
        [this](uint16_t stream_index, ieee::Eui64 const& stream_id, ieee::Eui48 dest_mac, TimePoint now) {
            auto const* sink = host_.components().milan_acmp_listener->sink(stream_index);
            auto const only = sink != nullptr ? std::optional{probed_talker(sink->context().srp)} : std::nullopt;
            listener_->on_listener_connected(stream_index, stream_id, dest_mac, now, only);
        },
        [this](uint16_t stream_index, TimePoint now) {
            listener_->on_listener_disconnected(stream_index, now, milan_input_loss(stream_index));
        });
}

auto AvbEntityKit::milan_input_loss(uint16_t const stream_index) const -> StreamInputLoss
{
    // Read as the sink stops SRP: why it does (Milan v1.3 Table 5.6 exempts only a controller's unbind).
    auto const* const sink = host_.components().milan_acmp_listener->sink(stream_index);
    bool const lost = sink != nullptr && sink->context().srp_stop == atdecc::MilanSrpStop::TalkerLost;
    return lost ? StreamInputLoss::talker_lost : StreamInputLoss::released;
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
    host_.components().msrp_handler.set_on_remote_talker([this](nanoavb::StreamId const& /*stream_id*/, TimePoint const now) {
        host_.components().milan_acmp_listener->on_srp_changed(now);
    });
    wire_milan_listener_connections(listener);
}

template <typename Listener>
void AvbEntityKit::wire_listener_connections(Listener& listener)
{
    // Drive the stream input (MSRP attach + mcast join) when its sink
    // connects, and release it when it disconnects.
    if (listener_ == nullptr) {
        return;
    }
    listener.set_connection_callbacks(
        [this](uint16_t stream_index, ieee::Eui64 const& stream_id, ieee::Eui48 dest_mac, TimePoint now) {
            listener_->on_listener_connected(stream_index, stream_id, dest_mac, now);
        },
        [this](uint16_t stream_index, TimePoint now) {
            listener_->on_listener_disconnected(stream_index, now, StreamInputLoss::released);
        });
}

void AvbEntityKit::wire_ieee_acmp_callbacks()
{
    host_.components().acmp_talker.set_on_probe(
        [this](uint16_t const stream_index, TimePoint const now) { note_talker_probe(stream_index, now); });
    // ACMP: log talker connections + publish the fresh connection count for
    // the media-timer gate.
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
    wire_listener_connections(host_.components().acmp_listener);
}

void AvbEntityKit::wire_aem_stream_callbacks()
{
    // AECP GET_COUNTERS (STREAM_OUTPUT talker rate + STREAM_INPUT listener
    // health) and GET_STREAM_INFO (talker stream identity, queried by a Milan
    // listener to verify the stream before sustaining a connection; listener
    // sink identity so a controller can read what an input is connected to).
    host_.components().aem_handler.set_get_counters(
        [this](uint16_t descriptor_type, uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& counters) -> bool {
            if (descriptor_type == DESCRIPTOR_STREAM_OUTPUT) {
                // Milan v1.3 Table 5.14 by observation interval; IEEE 1722.1's running counts otherwise.
                return milan_profile() && descriptor_index < output_counters_.size()
                    ? fill_interval_talker_stream_counters(
                          *talker_,
                          descriptor_index,
                          output_counters_[descriptor_index],
                          output_channels(descriptor_index),
                          valid,
                          counters)
                    : fill_talker_stream_counters(*talker_, descriptor_index, valid, counters);
            }
            if (descriptor_type == DESCRIPTOR_STREAM_INPUT && listener_ != nullptr) {
                // Milan v1.3 Table 5.13 by observation interval; IEEE 1722.1's running counts otherwise.
                return milan_profile() ? listener_->fill_interval_stream_input_counters(descriptor_index, valid, counters)
                                       : listener_->fill_stream_input_counters(descriptor_index, valid, counters);
            }
            return false;
        });
    host_.components().aem_handler.set_get_stream_info(
        [this](uint16_t descriptor_type, uint16_t descriptor_index, atdecc::aem::AemStreamInfoPayload& out) -> bool {
            return stream_info(descriptor_type, descriptor_index, out);
        });
    host_.components().aem_handler.set_presentation_offset_hooks(
        [this](uint16_t const idx) -> std::optional<uint32_t> {
            return is_talker_index(idx) ? std::optional<uint32_t>{presentation_offset_ns(idx)} : std::nullopt;
        },
        [this](uint16_t const idx, uint32_t const offset_ns) -> uint8_t { return change_presentation_offset(idx, offset_ns); });
}

auto AvbEntityKit::change_presentation_offset(uint16_t const idx, uint32_t const offset_ns) -> uint8_t
{
    if (!on_presentation_offset_changed_) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;  // the configured offset is fixed
    }
    auto const status = on_presentation_offset_changed_(idx, offset_ns);
    if (status == atdecc::AEM_STATUS_SUCCESS) {
        presentation_offsets_[idx] = offset_ns;  // the handler asks only for a talker's index
        host_.ctl_log().status("aem: talker stream {} presentation offset {} ns", idx, offset_ns);
    }
    // The kit keeps no deferred offset to finish later: IN_PROGRESS is not an answer here.
    return status == atdecc::AEM_STATUS_IN_PROGRESS ? atdecc::AEM_STATUS_ENTITY_MISBEHAVING : status;
}

auto AvbEntityKit::is_talker_index(uint16_t const idx) const noexcept -> bool
{
    return idx < MAX_ENTITY_STREAMS && std::ranges::any_of(talker_specs_, [idx](StreamSpec const& s) { return s.index == idx; });
}

auto AvbEntityKit::presentation_offset_ns(uint16_t const idx) const noexcept -> uint32_t
{
    if (!is_talker_index(idx)) {
        return 0;
    }
    auto const configured = std::min<uint64_t>(effective_presentation_offset_ns(config_), std::numeric_limits<uint32_t>::max());
    return presentation_offsets_[idx].value_or(static_cast<uint32_t>(configured));
}

auto AvbEntityKit::stream_info(uint16_t const type, uint16_t const index, atdecc::aem::AemStreamInfoPayload& out) -> bool
{
    bool filled = false;
    if (type == DESCRIPTOR_STREAM_OUTPUT) {
        filled = fill_talker_stream_info(host_, index, presentation_offset_ns(index), talker_->stream_stopped(index), out);
    } else if (type == DESCRIPTOR_STREAM_INPUT && listener_ != nullptr) {
        filled = fill_listener_stream_info(host_, index, listener_->stream_stopped(index), milan_sink_stream_info(index), out);
    }
    // The fillers read the format the model authors; the one in force is the
    // descriptor's, which a SET_STREAM_FORMAT or a restored format changes.
    auto const format = filled ? storage_handler_.current_stream_format(stream_ref(type, index)) : std::nullopt;
    if (format.has_value()) {
        span_copy(make_span(out.stream_format), format->span());
    }
    return filled;
}

auto AvbEntityKit::change_sampling_rate(uint16_t const audio_unit, uint32_t const rate) -> uint8_t
{
    // IEEE 1722.1-2021 Clause 7.4.21: the rate in force is a no-op; another moves the data plane or is refused.
    auto const in_force = storage_handler_.current_sampling_rate(
        {.configuration_index = 0, .descriptor_type = DESCRIPTOR_AUDIO_UNIT, .descriptor_index = audio_unit});
    if (in_force == rate) {
        return atdecc::AEM_STATUS_SUCCESS;
    }
    if (audio_unit != 0 || !in_force || !on_unit_rate_changed_ || !retimed_clock_domain_) {
        return atdecc::AEM_STATUS_NOT_SUPPORTED;
    }
    auto const retimed = retimed_formats(*in_force, rate);
    return retimed ? retime_streams(rate, *retimed) : retimed.error();
}

auto AvbEntityKit::retimed_formats(uint32_t const from, uint32_t const rate) -> std::expected<RetimedFormats, uint8_t>
{
    RetimedFormats out{};
    for (bool const output : {true, false}) {
        for (auto const& spec : output ? talker_specs_ : listener_specs_) {
            if (auto const status = add_retimed(output, spec, from, rate, out); status != atdecc::AEM_STATUS_SUCCESS) {
                return std::unexpected(status);
            }
        }
    }
    return out;
}

auto AvbEntityKit::add_retimed(
    bool const output, StreamSpec const& spec, uint32_t const from, uint32_t const rate, RetimedFormats& out) -> uint8_t
{
    auto const word = retimed_from(output, spec, from);
    if (!word) {
        return atdecc::AEM_STATUS_SUCCESS;  // a CRF's format stays, as does one at another rate or in another domain
    }
    auto const to = retimed_to(output, spec.index, *word, rate);
    if (!to) {
        return to.error();
    }
    (void)out.try_push_back(
        {.type = output ? DESCRIPTOR_STREAM_OUTPUT : DESCRIPTOR_STREAM_INPUT, .index = spec.index, .from = *word, .to = *to});
    return atdecc::AEM_STATUS_SUCCESS;
}

auto AvbEntityKit::retimed_from(bool const output, StreamSpec const& spec, uint32_t const from) -> std::optional<uint64_t>
{
    auto const in_force =
        storage_handler_.current_stream_format(stream_ref(output ? DESCRIPTOR_STREAM_OUTPUT : DESCRIPTOR_STREAM_INPUT, spec.index));
    uint64_t const word = in_force.value_or(ieee::Eui64{}).to_uint64();
    auto const format = decode_stream_format(word);
    bool const audio = format.kind == StreamKind::aaf || format.kind == StreamKind::am824;
    bool const moves = audio && format.sample_rate_hz == from && spec.clock_domain_index == retimed_clock_domain_;
    return moves ? std::optional<uint64_t>{word} : std::nullopt;
}

auto AvbEntityKit::retimed_to(bool const output, uint16_t const index, uint64_t const word, uint32_t const rate) const
    -> std::expected<uint64_t, uint8_t>
{
    auto const to = aaf_format_at_rate(word, rate);
    if (!to ||
        !storage_handler_.stream_format_listed(
            stream_ref(output ? DESCRIPTOR_STREAM_OUTPUT : DESCRIPTOR_STREAM_INPUT, index), ieee::Eui64{}.from_uint64(*to))) {
        return std::unexpected(atdecc::AEM_STATUS_NOT_SUPPORTED);  // AM824, or a stream that lists no such format
    }
    // Milan v1.3 Section 5.4.2.7: refused as the input's own SET_STREAM_FORMAT would be; an output is the hook's.
    if (!output && input_running(index)) {
        return std::unexpected(atdecc::AEM_STATUS_STREAM_IS_RUNNING);
    }
    return *to;
}

auto AvbEntityKit::input_running(uint16_t const index) const noexcept -> bool
{
    auto const& components = host_.components();
    if (components.acmp_is_milan()) {
        return components.milan_acmp_listener->is_bound(index);
    }
    return components.acmp_listener.is_connected(index);
}

auto AvbEntityKit::retime_streams(uint32_t const rate, RetimedFormats const& retimed) -> uint8_t
{
    // The data plane first, all or none; then the descriptors, reactor-side state, follow it. The
    // kit's own specs and engines are not touched: the media thread reads them unguarded.
    if (auto const status = on_unit_rate_changed_(rate, retimed); status != atdecc::AEM_STATUS_SUCCESS) {
        return status;
    }
    for (auto const& r : retimed) {
        (void)storage_handler_.set_runtime_stream_format(stream_ref(r.type, r.index), ieee::Eui64{}.from_uint64(r.to));
        if (r.type == DESCRIPTOR_STREAM_OUTPUT) {
            (void)talker_->toggle_media_clock_restart(r.index);  // its media clock restarts (IEEE 1722-2016 Clause 4.4.4.3)
        }
    }
    notify_after_response(retimed);
    host_.ctl_log().status("aem: AUDIO_UNIT 0 at {} Hz, {} streams re-timed", rate, retimed.size());
    return atdecc::AEM_STATUS_SUCCESS;
}

void AvbEntityKit::notify_after_response(RetimedFormats const& retimed)
{
    retimed_notices_ = retimed;
    host_.components().aem_handler.call_after_response([this] { notify_retimed(); });
}

void AvbEntityKit::notify_retimed()
{
    // Framed as each stream's own SET_STREAM_FORMAT (IEEE 1722.1-2021 Clause 7.5.2), and sent to
    // every registered controller, the requester included (Milan v1.3 Section 5.4.5.3).
    for (auto const& r : std::exchange(retimed_notices_, {})) {
        std::array<uint8_t, atdecc::aem::AemStreamFormatPayload::LENGTH> body{};
        span_store(
            std::span{body},
            atdecc::aem::AemStreamFormatPayload{
                .descriptor_type = r.type,
                .descriptor_index = r.index,
                .stream_format = std::bit_cast<std::array<ieee::octet_t, 8>>(ieee::octlet_t{r.to})});
        host_.components().aem_handler.emit_unsolicited(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, body);
    }
}

auto AvbEntityKit::setup_maap_addresses(net::MessageReactor& reactor, TimePoint const now) -> Status
{
    sg14::inplace_vector<uint16_t, MAX_ENTITY_STREAMS> indices{};
    for (auto const& spec : talker_specs_) {
        (void)indices.try_push_back(spec.index);
    }
    maap_.set_on_ready_changed([this](bool) { notify_talker_gates(); });
    // On (re)defend: release the ADP start the Link Up held back, and
    // re-declare the MSRP Talker Advertise so listeners reserve against the
    // MAAP address, once the SR-class domain is up.
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
            if (host_.is_ready()) {
                advertise_talker_streams(time);
            }
        },
        now,
        state_file(config_.stream_maap_range_file, entity_state_file::MAAP_RANGE, true));
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

auto AvbEntityKit::start(net::MessageReactor& reactor, TimePoint const now) -> Status
{
    if (auto status = bring_up_control_plane(reactor, now); !status) {
        return status;
    }
    open_stream_port();
    if (auto status = claim_stream_addresses(reactor, now); !status) {
        return status;
    }
    if (auto status = open_streams(reactor); !status) {
        return status;
    }
    finish_start();
    return success();
}

auto AvbEntityKit::bring_up_control_plane(net::MessageReactor& reactor, TimePoint const now) -> Status
{
    // Bring up the shared control plane (net handlers + generic SM wiring),
    // then attach the stream-specific callbacks (hooks + ACMP + AEM handlers).
    gate_.set_logger(host_.ctl_log());
    if (listener_ != nullptr) {
        listener_->set_logger(host_.ctl_log());
    }
    if (auto status = host_.start_control_plane(reactor, config_.interface_name); !status) {
        return status;
    }
    restore_control_state(now);
    follow_grandmaster();
    return success();
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
    if (in_force) {
        talker_->notify_grandmaster_changed(gate_.told_time());  // the reactor cycle's instant, as the loop told it
        return;
    }
    talker_->notify_grandmaster_lost();
}

void AvbEntityKit::restore_control_state(TimePoint const now)
{
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

void AvbEntityKit::open_stream_port()
{
    // The stream port, backend by name. Not fatal: an entity whose port
    // would not open keeps its control plane and has no streams, which is
    // what it did before when the socket would not open.
    if (stream_port_.valid()) {  // the host opened it before the start (a test port): kept
        host_.ctl_log().status("stream port: {}, opened by the host", logging::embed<48>(stream_port_.describe()));
        return;
    }
    if (auto status = stream_port_.open(
            {.backend = config_.net_backend,
             .interface_name = config_.interface_name,
             .stream_pcp = config_.stream_pcp,
             .xdp_queue = config_.net_xdp_queue});
        !status) {
        host_.ctl_log().error(
            "stream port: {} did not open: {}",
            logging::embed<8>(config_.net_backend),
            logging::embed<40>(status.error().message()));
    } else {
        host_.ctl_log().status(
            "stream port: {}{}",
            logging::embed<48>(stream_port_.describe()),
            stream_port_.serialized() ? logging::lit(", TX and RX serialised") : logging::lit(""));
    }
}

auto AvbEntityKit::claim_stream_addresses(net::MessageReactor& reactor, TimePoint const now) -> Status
{
    // In "maap" mode, set up the claim of the talker stream destination
    // addresses via MAAP: the Link Up begins it (B.3.5.9), and the acquired
    // addresses then overwrite the static defaults the data plane reads below.
    // Non-fatal inside: on socket failure the entity keeps the static MACs.
    return effective_stream_address_mode(config_) == "maap" ? setup_maap_addresses(reactor, now) : success();
}

auto AvbEntityKit::open_streams(net::MessageReactor& reactor) -> Status
{
    if (auto status = open_talker_streams(); !status) {
        return status;
    }
    return open_listener_streams(reactor);
}

auto AvbEntityKit::open_talker_streams() -> Status
{
    // One TX slot per blob-declared stream, shaped by its format word, with
    // the stream identity (id + dest MAC) the ACMP model carries.
    for (auto const& spec : talker_specs_) {
        auto const stream = host_.components().talker_stream(spec.index).value_or(nanoavb::NanoAvbComponents::TalkerStream{});
        if (auto status = talker_->open_stream(spec, srp_stream_id(stream.stream_id), stream.stream_dest_mac); !status) {
            return status;
        }
    }
    return success();
}

auto AvbEntityKit::open_listener_stream(StreamSpec const& spec) -> StatusValue<ieee::Eui48>
{
    if (auto status = listener_->open_stream(spec); !status) {
        return failure(status.error());
    }
    bind_crf_recovery(spec);
    bool const is_audio = spec.format.kind == StreamKind::am824 || spec.format.kind == StreamKind::aaf;
    auto const* slot = is_audio ? talker_->slot_of(spec.format.kind) : nullptr;
    return success(slot != nullptr ? slot->dest_mac : ieee::Eui48{});
}

void AvbEntityKit::bind_crf_recovery(StreamSpec const& spec)
{
    auto const* slot = listener_->slot_for(spec.index);
    if (slot != nullptr && !slot->on_crf && crf_recovery_input() == spec.index) {
        listener_->set_crf(spec.index, crf_recovery_.make_consumer());
    }
}

auto AvbEntityKit::crf_clock_source_input() const -> std::optional<uint16_t>
{
    bool const of_input = crf_clock_source_index_ && clock_source_kind(*crf_clock_source_index_) == ClockSourceKind::InputStream;
    return of_input ? located_stream_input(storage_handler_.storage(), *crf_clock_source_index_) : std::nullopt;
}

auto AvbEntityKit::crf_recovery_input() const -> std::optional<uint16_t>
{
    if (!crf_clock_source_index_) {
        auto const* const first =
            std::ranges::find(listener_specs_, StreamKind::crf, [](StreamSpec const& spec) { return spec.format.kind; });
        return first != listener_specs_.end() ? std::optional{first->index} : std::nullopt;
    }
    auto const input = crf_clock_source_input();
    bool const is_crf = std::ranges::any_of(
        listener_specs_, [input](StreamSpec const& spec) { return spec.index == input && spec.format.kind == StreamKind::crf; });
    return is_crf ? input : std::nullopt;
}

auto AvbEntityKit::open_listener_streams(net::MessageReactor& reactor) -> Status
{
    // RX slots per blob-declared stream input. One CRF input feeds the media-clock
    // recovery unless the host bound its own consumer there (kit phase 3c); the ACMP
    // listener callbacks (wired at start) drive the MSRP attach + the multicast join per connection.
    if (listener_ == nullptr) {
        return success();
    }
    // Static RX groups: an audio input pre-joins the same-kind talker
    // output's group (the loopback topology listens on its own output
    // group); everything else joins dynamically on ACMP connect (a zero
    // MAC just opens the socket).
    sg14::inplace_vector<ieee::Eui48, MAX_ENTITY_STREAMS> rx_groups{};
    for (auto const& spec : listener_specs_) {
        auto const group = open_listener_stream(spec);
        if (!group.has_value()) {
            return failure(group.error());
        }
        (void)rx_groups.try_push_back(*group);
    }
    attach_listener_rx(rx_groups, reactor);
    return success();
}

void AvbEntityKit::attach_listener_rx(std::span<ieee::Eui48 const> const rx_groups, net::MessageReactor& reactor)
{
    // In RT-timer mode the returned handler is kept alive here; the tool's
    // SCHED_FIFO RX timer drains it via drain_stream_rx() with its wake gPTP
    // time, so RX can't be starved by the control plane. rx_to_reactor()
    // is the way back should that timer not start.
    if (auto rx = listener_->attach_rx(stream_port_, rx_groups, reactor, config_.stream_rx_rt_timer)) {
        rt_rx_handler_ = std::move(*rx);
    }
    // Persisted listener bindings (kit phase 5b): a restarted entity takes
    // its inputs up again without a controller (see AvbEntityHost).
    // Under Milan they are saved without being asked (Milan v1.3 5.3.8.3):
    // with no file named, in the entity's state directory.
    bool const milan = host_.components().acmp_is_milan();
    host_.enable_listener_binding_persistence(
        state_file(config_.listener_bindings_path, entity_state_file::LISTENER_BINDINGS, milan));
    // The started/stopped state is restored with the binding (5.3.8.7),
    // and a restored binding is bound from here on.
    follow_milan_bindings();
    // And each input's format, saved when a controller last set it (5.3.8.1).
    restore_input_formats();
}

void AvbEntityKit::finish_start()
{
    // TX on the stream port + optional pcap capture.
    (void)talker_->open_tx(
        stream_port_,
        TalkerStreams::TxPcapConfig{
            .path = config_.tx_pcap_path, .max_bytes = config_.tx_pcap_max_bytes, .seconds = config_.tx_pcap_seconds});
    // The TX kick off the media thread (the tool's kicker timer, or its RX
    // timer's tick). The tool must then run that timer, or call
    // kick_from_media_tick() / rx_to_reactor() when it cannot.
    bool const kick_elsewhere = config_.stream_tx_kicker || (config_.stream_tx_kick_in_rx_timer && config_.stream_rx_rt_timer);
    if (!talker_->set_external_kick(kick_elsewhere)) {
        host_.ctl_log().warning("stream_tx.kicker: no stream port to kick; kicking in the media tick");
    }
    report_inert_controls();
    notify_talker_gates();  // the slots are open: each gate open now is told
}

void AvbEntityKit::report_inert_controls()
{
    // Menu/selection diagnostics: registrations the model left inert. Status
    // level -- typo-finding, never an error.
    for (auto const& binding : control_bindings_) {
        if (!binding.resolved) {
            host_.ctl_log().status("control: registered symbol 0x{:08x} not a CONTROL in this model (inert)", binding.symbol);
        }
    }
}

void AvbEntityKit::rx_to_reactor(net::MessageReactor& reactor)
{
    if (rt_rx_handler_ != nullptr) {
        reactor.add(std::move(rt_rx_handler_));
    }
    if (config_.stream_tx_kick_in_rx_timer) {
        kick_from_media_tick();
    }
}

auto AvbEntityKit::stop(TimePoint const now) -> Status
{
    if (!host_.is_running()) {
        return failure(std::make_error_code(std::errc::not_connected));
    }
    // The data plane first, once the tool has stopped the timers that drive
    // it, then the MAAP range (Table B.7 Release!: no longer defended, its
    // port off the reactor), then the control plane. start() builds all
    // three again.
    close_streams();
    notify_talker_gates();  // no slot: every gate closes
    maap_.stop(now);
    link_up_ = false;  // the next start()'s Link Up is a first one again
    return host_.stop_control_plane(now);
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
    adp.notify_entity_changed();
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

auto AvbEntityKit::milan_features_flags() const noexcept -> uint32_t
{
    // MVU_BINDING says that BIND_STREAM and UNBIND_STREAM are implemented
    // (Milan v1.3 5.4.4.6, 5.4.4.7). They are whenever Milan connection
    // management runs and never otherwise, so the bit follows the profile
    // and not the configuration.
    uint32_t const configured = config_.milan_features_flags & ~atdecc::MVU_FEATURE_MVU_BINDING;
    return configured | (milan_profile() ? atdecc::MVU_FEATURE_MVU_BINDING : 0U);
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

auto AvbEntityKit::make_talker_srp_info(StreamSpec const& spec) const -> nanoavb::TalkerStreamSrpInfo
{
    nanoavb::TalkerStreamSrpInfo info{};
    fill_talker_srp_identity(spec.index, info);
    info.max_interval_frames = 1;
    // TSpec frame size from the format word (kit phase 1) — no per-entity
    // byte math to keep in step with the model. Milan v1.3 Table 4.4 sizes
    // the frame exactly; classic keeps one sample of headroom.
    info.max_frame_size = milan_profile() ? milan_srp_max_frame_size(spec.format) : srp_max_frame_size(spec.format);
    info.accumulated_latency = 0;
    return info;
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
        info.stream_id = srp_stream_id(stream->stream_id);
        info.dest_address = stream->stream_dest_mac;
        info.vlan_id = stream->stream_vlan_id;
    }
}

void AvbEntityKit::advertise_talker_streams(TimePoint const time)
{
    // Defer until the stream destination MACs are final. In MAAP mode they are
    // acquired asynchronously after gPTP lock (which first triggers this), so
    // advertising a pre-MAAP dest would not match the MAAP address ACMP hands the
    // listener -> AskingFailed. on_acquired re-calls this once the block is defended.
    // Under the Milan profile the SRP handler takes the parameters here and
    // declares each stream only while a listener wants it (4.3.3.1).
    if (!maap_.ready()) {
        return;
    }
    for (auto const& spec : talker_specs_) {
        auto result = host_.components().msrp_handler.talker_advertise(make_talker_srp_info(spec), time);
        if (!result) {
            host_.ctl_log().warning("msrp: talker_advertise (stream {}) failed: errno {}", spec.index, result.error().value());
        }
    }
}

void AvbEntityKit::end_cycle(int64_t const now_ns)
{
    poll_counters(now_ns);
    on_admission_timer(raw_at(now_ns));
    tell_changed_input_info();
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

auto AvbEntityKit::output_channels(uint16_t const index) const noexcept -> uint16_t
{
    auto const* const it = std::ranges::find(talker_specs_, index, &StreamSpec::index);
    return it == talker_specs_.end() ? uint16_t{0} : it->format.channels;
}

}  // namespace statusbar::avb_entity
